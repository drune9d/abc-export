// mesh2abc -- converts a binary mesh frame sequence (".mesh", MSH1) into an
// Alembic (.abc) PolyMesh cache, optionally attaching arbitrary named
// point-scope and face-varying-scope attributes read from sidecar
// directories alongside the sequence.
//
// Each frame is written with its own full topology, so sequences whose
// vertex and face counts change over time (fracture, fluid, remeshing sims)
// are preserved rather than frozen to the first frame.
//
// This program is the conversion half of AbcExport's Houdini plugin: the
// plugin writes the frame files with plain Python file I/O (never a ROP/COP
// render), then runs this binary to produce the final .abc. See ../README.md.
//
// Copyright (C) 2026 Aina Olaoluwa
//
// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the
// Free Software Foundation, either version 3 of the License, or (at your
// option) any later version.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
// Public License for more details: <https://www.gnu.org/licenses/>.

#include <Alembic/AbcGeom/All.h>
#include <Alembic/AbcCoreOgawa/All.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace Alembic::AbcGeom;
namespace fs = std::filesystem;

// Sorted list of the ".mesh" frame files directly inside `dir`. The
// extension filter keeps stray files (.DS_Store, notes, partial writes
// under another suffix) from being misread as frames.
static std::vector<std::string> ListMeshFrames(const std::string& dir)
{
	std::vector<fs::path> paths;
	if (!dir.empty() && fs::is_directory(dir)) {
		for (const auto& entry : fs::directory_iterator(dir)) {
			if (entry.is_regular_file() && entry.path().extension() == ".mesh")
				paths.push_back(entry.path());
		}
	}
	std::sort(paths.begin(), paths.end());

	std::vector<std::string> files;
	files.reserve(paths.size());
	for (const auto& p : paths) files.push_back(p.string());
	return files;
}

// --- Named attribute sidecars ------------------------------------------
//
// Layout on disk (written by the Houdini plugin's PythonModule):
//
//   <attrs-dir>/<attribute name>/<anything, sorted>.attr
//
// Each ".attr" file is a tiny custom binary format:
//
//   char[4]   magic "ATR1"
//   uint32 LE element count for this frame (points or face-corners)
//   uint32 LE components per element (1 = scalar, 3 = vector, 4 = quaternion, ...)
//   float32 LE, element_count * components, interleaved
//
// The Nth file (alphabetically sorted) in an attribute's folder pairs with
// the Nth mesh frame (alphabetically sorted), so the exporter doesn't need
// to match frame numbers exactly, only frame *order*.
struct AttributeSpec
{
	std::string name;
	size_t components = 0;
	struct FrameRef
	{
		std::string path;
		uint32_t elementCount = 0;
		uint64_t offset = 0;
		uint64_t byteCount = 0;
		bool packed = false;
	};
	std::vector<FrameRef> frames;   // parallel to the main frame file list
};

static constexpr char kAttrMagic[4] = { 'A', 'T', 'R', '1' };
static constexpr char kAttrPackMagic[4] = { 'A', 'P', 'K', '2' };

static bool ReadU16(std::ifstream& file, uint16_t& out)
{
	file.read(reinterpret_cast<char*>(&out), sizeof(out));
	return (bool)file;
}

static bool ReadU32(std::ifstream& file, uint32_t& out)
{
	file.read(reinterpret_cast<char*>(&out), sizeof(out));
	return (bool)file;
}

static bool ReadU64(std::ifstream& file, uint64_t& out)
{
	file.read(reinterpret_cast<char*>(&out), sizeof(out));
	return (bool)file;
}

// Peeks just the header of one .attr file to learn its component count.
static bool ReadAttributeHeader(const std::string& path, size_t& outPointCount, size_t& outComponents)
{
	std::ifstream file(path, std::ios::binary);
	if (!file.is_open()) return false;

	char magic[4];
	file.read(magic, 4);
	if (!file || std::memcmp(magic, kAttrMagic, 4) != 0) return false;

	uint32_t pointCount = 0, components = 0;
	file.read(reinterpret_cast<char*>(&pointCount), sizeof(pointCount));
	file.read(reinterpret_cast<char*>(&components), sizeof(components));
	if (!file) return false;

	outPointCount = pointCount;
	outComponents = components;
	return true;
}

// Reads one full .attr file's payload into a flat, interleaved float buffer.
// Returns false (leaving outFlat untouched) on any mismatch, so the caller
// can zero-fill rather than write misaligned data into the Alembic sample.
static bool ReadAttributeFrame(const AttributeSpec::FrameRef& ref, size_t expectedComponents, std::vector<float>& outFlat)
{
	std::ifstream file(ref.path, std::ios::binary);
	if (!file.is_open()) return false;

	if (ref.packed) {
		if (ref.byteCount % sizeof(float) != 0) return false;
		const size_t valueCount = static_cast<size_t>(ref.byteCount / sizeof(float));
		if (expectedComponents == 0 || valueCount != static_cast<size_t>(ref.elementCount) * expectedComponents)
			return false;
		outFlat.assign(valueCount, 0.0f);
		file.seekg(static_cast<std::streamoff>(ref.offset), std::ios::beg);
		file.read(reinterpret_cast<char*>(outFlat.data()), static_cast<std::streamsize>(ref.byteCount));
		return (bool)file;
	}

	char magic[4];
	file.read(magic, 4);
	if (!file || std::memcmp(magic, kAttrMagic, 4) != 0) return false;

	uint32_t pointCount = 0, components = 0;
	file.read(reinterpret_cast<char*>(&pointCount), sizeof(pointCount));
	file.read(reinterpret_cast<char*>(&components), sizeof(components));
	if (!file || components != expectedComponents) return false;

	outFlat.assign(static_cast<size_t>(pointCount) * components, 0.0f);
	file.read(reinterpret_cast<char*>(outFlat.data()), outFlat.size() * sizeof(float));
	if (!file) return false;

	return true;
}

// Lists <attrsDir>/<name>/ subfolders and treats each as one attribute,
// sorted by name for deterministic ordering. Every regular file inside a
// subfolder is treated as one frame's data, sorted alphabetically.
static std::vector<AttributeSpec> DiscoverAttributes(const std::string& attrsDir)
{
	std::vector<AttributeSpec> specs;
	if (attrsDir.empty() || !fs::exists(attrsDir) || !fs::is_directory(attrsDir))
		return specs;

	std::vector<fs::path> subdirs;
	for (const auto& entry : fs::directory_iterator(attrsDir))
	{
		if (entry.is_directory()) subdirs.push_back(entry.path());
	}
	std::sort(subdirs.begin(), subdirs.end());

	for (const auto& dir : subdirs)
	{
		AttributeSpec spec;
		spec.name = dir.filename().string();

		std::vector<fs::path> files;
		for (const auto& entry : fs::directory_iterator(dir))
		{
			if (entry.is_regular_file()) files.push_back(entry.path());
		}
		std::sort(files.begin(), files.end());

		if (files.empty())
		{
			std::cerr << "Warning: attribute folder has no files, skipping: " << dir << std::endl;
			continue;
		}

		size_t pointCount = 0, components = 0;
		if (!ReadAttributeHeader(files.front().string(), pointCount, components) || components == 0)
		{
			std::cerr << "Warning: could not read attribute header, skipping: " << files.front() << std::endl;
			continue;
		}

		spec.components = components;
		for (const auto& f : files) {
			AttributeSpec::FrameRef ref;
			ref.path = f.string();
			spec.frames.push_back(std::move(ref));
		}
		specs.push_back(std::move(spec));
	}

	return specs;
}

static std::vector<AttributeSpec> DiscoverPackedAttributes(const std::string& packPath)
{
	std::vector<AttributeSpec> specs;
	if (packPath.empty() || !fs::is_regular_file(packPath))
		return specs;

	std::ifstream file(packPath, std::ios::binary);
	if (!file.is_open()) return specs;

	char magic[4];
	file.read(magic, 4);
	if (!file || std::memcmp(magic, kAttrPackMagic, 4) != 0) {
		std::cerr << "Warning: bad .attrpack magic in " << packPath << std::endl;
		return specs;
	}

	uint32_t attrCount = 0, frameCount = 0;
	if (!ReadU32(file, attrCount) || !ReadU32(file, frameCount))
		return specs;

	specs.reserve(attrCount);
	for (uint32_t a = 0; a < attrCount; ++a) {
		uint16_t nameLen = 0;
		uint32_t components = 0;
		if (!ReadU16(file, nameLen) || !ReadU32(file, components) || nameLen == 0 || components == 0)
			return {};

		AttributeSpec spec;
		spec.name.assign(nameLen, '\0');
		file.read(&spec.name[0], nameLen);
		if (!file) return {};
		spec.components = components;
		spec.frames.resize(frameCount);
		specs.push_back(std::move(spec));
	}

	for (uint32_t a = 0; a < attrCount; ++a) {
		for (uint32_t f = 0; f < frameCount; ++f) {
			AttributeSpec::FrameRef ref;
			ref.path = packPath;
			ref.packed = true;
			if (!ReadU32(file, ref.elementCount) || !ReadU64(file, ref.offset) || !ReadU64(file, ref.byteCount))
				return {};
			specs[a].frames[f] = std::move(ref);
		}
	}

	return specs;
}


// Full per-frame mesh, already converted to Alembic-ready arrays. Each frame
// carries its own topology so sequences whose vertex/face counts change over
// time (fracture, fluid, remeshing sims) are represented correctly instead of
// being frozen to the first frame's topology.
struct FrameMesh
{
	std::vector<V3f> positions;
	std::vector<int32_t> faceIndices;   // flattened, file (CCW) order
	std::vector<int32_t> faceCounts;    // vertices per face
	std::vector<V2f> uvs;               // face-varying, only if hasUVs
	bool hasUVs = false;
	std::vector<N3f> normals;           // face-varying, only if hasNormals
	bool hasNormals = false;
	std::vector<C3f> colors;            // per-vertex (same order/index as positions), only if hasColors
	bool hasColors = false;

	// Index-aligned with the AttributeSpec list passed to ReadMeshFrame;
	// each entry is this frame's flat, interleaved values for one point
	// attribute, or empty if the sidecar for this frame couldn't be read.
	std::vector<std::vector<float>> attribs;

	// Same idea as attribs, but for face-varying (per face-corner) named
	// attributes -- e.g. a second or third UV set, or any other custom
	// vertex-class attribute -- index-aligned with fvAttributeSpecs. Each
	// entry has faceIndices.size() * components values, not
	// positions.size() * components.
	std::vector<std::vector<float>> fvAttribs;
};

// Reads one named-attribute sidecar per spec for a given frame into `out`,
// shared by both the point-attribute and face-varying-attribute paths since
// the sidecar file format and matching-by-position convention are identical
// either way -- only the element count they're meant to align with differs.
static void ReadNamedAttributeSidecars(const std::vector<AttributeSpec>& specs, size_t frameIndex,
	std::vector<std::vector<float>>& out)
{
	if (specs.empty()) return;
	out.resize(specs.size());
	for (size_t i = 0; i < specs.size(); ++i) {
		const AttributeSpec& spec = specs[i];
		if (frameIndex >= spec.frames.size()) continue;
		std::vector<float> flat;
		if (ReadAttributeFrame(spec.frames[frameIndex], spec.components, flat)) {
			out[i] = std::move(flat);
		} else {
			std::cerr << "Warning: could not read attribute '" << spec.name
				<< "' frame " << frameIndex << ", zero-filling." << std::endl;
		}
	}
}

// --- Binary mesh frames (".mesh", MSH1) --------------------------------
//
// Written by the Houdini plugin via plain Python struct.pack, sourced from
// bulk hou.Geometry attribute reads -- fast to write on the Houdini side
// (no text formatting) and fast to read here (no parsing at all).
//
//   char[4]   magic "MSH1"
//   uint32 LE point_count
//   uint32 LE vertex_count   (total face-corners across all faces)
//   uint32 LE face_count
//   uint32 LE flags (bit0 = has UV, bit1 = has normals, bit2 = has color)
//   float32 LE positions[point_count * 3]
//   uint32 LE face_vertex_counts[face_count]
//   int32 LE  face_vertex_point_indices[vertex_count]   (0-based, already)
//   -- if has UV:      float32 LE uvs[vertex_count * 3]      (face-varying; u,v,w -- w unused)
//   -- if has normals: float32 LE normals[vertex_count * 3]  (face-varying)
//   -- if has color:   float32 LE colors[point_count * 3]    (per-point)
static constexpr char kMeshMagic[4] = { 'M', 'S', 'H', '1' };
enum MeshFlag : uint32_t
{
	kMeshFlagUV = 1u << 0,
	kMeshFlagNormal = 1u << 1,
	kMeshFlagColor = 1u << 2,
};

static_assert(sizeof(V3f) == 3 * sizeof(float), "V3f must be 3 tightly packed floats for bulk binary reads");
static_assert(sizeof(N3f) == 3 * sizeof(float), "N3f must be 3 tightly packed floats for bulk binary reads");
static_assert(sizeof(C3f) == 3 * sizeof(float), "C3f must be 3 tightly packed floats for bulk binary reads");

static FrameMesh ReadMeshFrame(const std::string& path,
	const std::vector<AttributeSpec>& attributeSpecs, const std::vector<AttributeSpec>& fvAttributeSpecs,
	size_t frameIndex)
{
	FrameMesh m;
	std::ifstream file(path, std::ios::binary);
	if (!file.is_open()) {
		std::cerr << "Warning: could not open frame: " << path << std::endl;
		return m;
	}

	char magic[4];
	file.read(magic, 4);
	if (!file || std::memcmp(magic, kMeshMagic, 4) != 0) {
		std::cerr << "Warning: bad .mesh magic in " << path << std::endl;
		return m;
	}

	uint32_t pointCount = 0, vertexCount = 0, faceCount = 0, flags = 0;
	file.read(reinterpret_cast<char*>(&pointCount), sizeof(pointCount));
	file.read(reinterpret_cast<char*>(&vertexCount), sizeof(vertexCount));
	file.read(reinterpret_cast<char*>(&faceCount), sizeof(faceCount));
	file.read(reinterpret_cast<char*>(&flags), sizeof(flags));
	if (!file) return m;

	m.positions.resize(pointCount);
	file.read(reinterpret_cast<char*>(m.positions.data()), pointCount * sizeof(V3f));

	std::vector<uint32_t> rawFaceCounts(faceCount);
	file.read(reinterpret_cast<char*>(rawFaceCounts.data()), faceCount * sizeof(uint32_t));
	m.faceCounts.resize(faceCount);
	for (size_t i = 0; i < faceCount; ++i) m.faceCounts[i] = static_cast<int32_t>(rawFaceCounts[i]);

	m.faceIndices.resize(vertexCount);
	file.read(reinterpret_cast<char*>(m.faceIndices.data()), vertexCount * sizeof(int32_t));

	if (flags & kMeshFlagUV) {
		std::vector<float> raw(static_cast<size_t>(vertexCount) * 3);
		file.read(reinterpret_cast<char*>(raw.data()), raw.size() * sizeof(float));
		m.uvs.resize(vertexCount);
		for (size_t i = 0; i < vertexCount; ++i) m.uvs[i] = V2f(raw[i * 3 + 0], raw[i * 3 + 1]);
		m.hasUVs = true;
	}
	if (flags & kMeshFlagNormal) {
		m.normals.resize(vertexCount);
		file.read(reinterpret_cast<char*>(m.normals.data()), vertexCount * sizeof(N3f));
		m.hasNormals = true;
	}
	if (flags & kMeshFlagColor) {
		m.colors.resize(pointCount);
		file.read(reinterpret_cast<char*>(m.colors.data()), pointCount * sizeof(C3f));
		m.hasColors = true;
	}

	if (!file) {
		std::cerr << "Warning: truncated .mesh file, data may be incomplete: " << path << std::endl;
	}

	// Named attribute sidecars, matched by frame position in the sorted lists.
	ReadNamedAttributeSidecars(attributeSpecs, frameIndex, m.attribs);
	ReadNamedAttributeSidecars(fvAttributeSpecs, frameIndex, m.fvAttribs);

	return m;
}

// --- Conversion --------------------------------------------------------

struct ConvertOptions
{
	std::string inputDir;
	std::string outputFile = "output.abc";
	std::string meshName = "mesh";
	float fps = 24.0f;
	bool writeUVs = true;
	bool writeNormals = false;
	bool writeColors = false;
	std::string attrsDir;
	std::string attrsFvDir;
	std::string attrsPack;
	std::string attrsFvPack;
};

// Reads every frame in opt.inputDir and writes the finished Alembic cache.
// Returns false if there was nothing to convert.
static bool ConvertSequence(const ConvertOptions& opt)
{
	std::vector<std::string> files = ListMeshFrames(opt.inputDir);
	if (files.empty()) {
		std::cerr << "No .mesh frame files found in: " << opt.inputDir << std::endl;
		return false;
	}

	const int totalFrames = static_cast<int>(files.size());
	std::cout << "Found " << totalFrames << " frame file(s) in: " << opt.inputDir << std::endl;

	std::vector<AttributeSpec> attributeSpecs = !opt.attrsPack.empty()
		? DiscoverPackedAttributes(opt.attrsPack)
		: DiscoverAttributes(opt.attrsDir);
	for (const auto& spec : attributeSpecs) {
		std::cout << "Attribute '" << spec.name << "': " << spec.components
			<< " component(s), " << spec.frames.size() << " frame(s) found" << std::endl;
		if (spec.frames.size() != (size_t)totalFrames) {
			std::cerr << "Warning: attribute '" << spec.name << "' has " << spec.frames.size()
				<< " frame(s) but the sequence has " << totalFrames
				<< "; extra/missing frames will be zero-filled." << std::endl;
		}
	}

	// Same sidecar format and discovery as attributeSpecs, but for
	// face-varying (per face-corner) data -- a second/third UV set, or any
	// other custom vertex-class attribute -- written to Alembic with
	// kFacevaryingScope instead of kVertexScope.
	std::vector<AttributeSpec> fvAttributeSpecs = !opt.attrsFvPack.empty()
		? DiscoverPackedAttributes(opt.attrsFvPack)
		: DiscoverAttributes(opt.attrsFvDir);
	for (const auto& spec : fvAttributeSpecs) {
		std::cout << "Face-varying attribute '" << spec.name << "': " << spec.components
			<< " component(s), " << spec.frames.size() << " frame(s) found" << std::endl;
		if (spec.frames.size() != (size_t)totalFrames) {
			std::cerr << "Warning: face-varying attribute '" << spec.name << "' has " << spec.frames.size()
				<< " frame(s) but the sequence has " << totalFrames
				<< "; extra/missing frames will be zero-filled." << std::endl;
		}
	}

	// One transform + one PolyMesh under it, one time sample per frame file.
	OArchive archive(Alembic::AbcCoreOgawa::WriteArchive(), opt.outputFile);
	TimeSamplingPtr timeSampling(new TimeSampling(1.0 / opt.fps, 0.0));
	OXform xform(archive.getTop(), opt.meshName, timeSampling);
	OPolyMesh meshObj(xform, opt.meshName, timeSampling);
	OPolyMeshSchema& mesh = meshObj.getSchema();

	// Read the first frame up front to decide, from frame 0, which optional
	// data this sequence carries. (Doing this on the main thread keeps
	// sources/params set before any sample is written.)
	FrameMesh first = ReadMeshFrame(files[0], attributeSpecs, fvAttributeSpecs, 0);
	const bool writeUVs = opt.writeUVs && first.hasUVs;
	const bool writeNormals = opt.writeNormals && first.hasNormals;
	const bool writeColors = opt.writeColors && first.hasColors;
	if (writeUVs)
		mesh.setUVSourceName("UVMap");

	// Vertex colors have no builtin slot on OPolyMeshSchema::Sample, so they're
	// written as an arbitrary geometry parameter named "Cs" (the convention
	// used by Houdini/Katana/RenderMan for per-vertex color on Alembic meshes).
	OC3fGeomParam colorParam;
	if (writeColors) {
		colorParam = OC3fGeomParam(mesh.getArbGeomParams(), "Cs", false, kVertexScope, 1, timeSampling);
	}

	// One arbitrary geometry parameter per named attribute, generalized over
	// component count via GeomParam "extent" rather than a fixed vector type
	// -- extent 1 is a plain scalar per point, 3 a vector3, 4 a quaternion,
	// and so on, all through the same OFloatGeomParam machinery.
	std::vector<OFloatGeomParam> attribParams;
	attribParams.reserve(attributeSpecs.size());
	for (const auto& spec : attributeSpecs) {
		attribParams.push_back(
			OFloatGeomParam(mesh.getArbGeomParams(), spec.name, false, kVertexScope,
				static_cast<int>(spec.components), timeSampling));
	}

	// Same idea, but face-varying scope -- one geometry parameter per named
	// face-varying attribute (e.g. a second UV set), each with one value per
	// face corner rather than per point.
	std::vector<OFloatGeomParam> fvAttribParams;
	fvAttribParams.reserve(fvAttributeSpecs.size());
	for (const auto& spec : fvAttributeSpecs) {
		fvAttribParams.push_back(
			OFloatGeomParam(mesh.getArbGeomParams(), spec.name, false, kFacevaryingScope,
				static_cast<int>(spec.components), timeSampling));
	}

	const int remaining = totalFrames;
	unsigned int numThreads = std::max(1u, std::thread::hardware_concurrency());
	numThreads = std::min(numThreads, (unsigned int)remaining);

	// Bounded prefetch pipeline: worker threads read whole frames ahead while
	// the main thread writes them to Alembic in order. Full frames are large,
	// so keep the in-flight window modest to bound peak memory.
	const int window = std::max(2, (int)numThreads);

	std::vector<FrameMesh> slots(remaining);
	std::vector<char> ready(remaining, 0);
	std::mutex mtx;
	std::condition_variable cv;
	std::atomic<int> nextRead(1);   // frame 0 already read on the main thread
	int writeIndex = 0;             // guarded by mtx; frames the writer has consumed

	slots[0] = std::move(first);
	ready[0] = 1;

	auto worker = [&]() {
		while (true) {
			int idx = nextRead.fetch_add(1);
			if (idx >= remaining) break;
			{
				std::unique_lock<std::mutex> lk(mtx);
				cv.wait(lk, [&] { return idx < writeIndex + window; });
			}
			FrameMesh data = ReadMeshFrame(files[idx], attributeSpecs, fvAttributeSpecs, (size_t)idx);
			{
				std::lock_guard<std::mutex> lk(mtx);
				slots[idx] = std::move(data);
				ready[idx] = 1;
			}
			cv.notify_all();
		}
	};

	std::vector<std::thread> workers;
	workers.reserve(numThreads);
	for (unsigned int t = 0; t < numThreads; ++t)
		workers.emplace_back(worker);

	// Write frames in order -- Alembic is not thread-safe. Each frame writes its
	// own full topology, so changing vertex/face counts are preserved.
	std::vector<V2f> zeroUVs;
	std::vector<N3f> zeroNormals;
	std::vector<C3f> zeroColors;
	std::vector<float> zeroAttrib;
	for (int i = 0; i < remaining; ++i) {
		FrameMesh fm;
		{
			std::unique_lock<std::mutex> lk(mtx);
			cv.wait(lk, [&] { return ready[i] != 0; });
			fm = std::move(slots[i]);
			writeIndex = i + 1;
		}
		cv.notify_all();  // let blocked readers advance into the freed window

		OPolyMeshSchema::Sample samp(
			V3fArraySample(fm.positions),
			Int32ArraySample(fm.faceIndices),
			Int32ArraySample(fm.faceCounts));

		// Keep each optional param's sample count aligned with the mesh: once a
		// param is written it must be written every frame. A frame missing the
		// data (rare, inconsistent input) gets a zero-filled set of the right
		// length rather than skipping the sample.
		if (writeUVs) {
			if (fm.hasUVs) {
				samp.setUVs(OV2fGeomParam::Sample(V2fArraySample(fm.uvs), kFacevaryingScope));
			} else {
				zeroUVs.assign(fm.faceIndices.size(), V2f(0.0f, 0.0f));
				samp.setUVs(OV2fGeomParam::Sample(V2fArraySample(zeroUVs), kFacevaryingScope));
			}
		}
		if (writeNormals) {
			if (fm.hasNormals) {
				samp.setNormals(ON3fGeomParam::Sample(N3fArraySample(fm.normals), kFacevaryingScope));
			} else {
				zeroNormals.assign(fm.faceIndices.size(), N3f(0.0f, 0.0f, 0.0f));
				samp.setNormals(ON3fGeomParam::Sample(N3fArraySample(zeroNormals), kFacevaryingScope));
			}
		}

		mesh.set(samp);

		if (writeColors) {
			if (fm.hasColors) {
				colorParam.set(OC3fGeomParam::Sample(C3fArraySample(fm.colors), kVertexScope));
			} else {
				zeroColors.assign(fm.positions.size(), C3f(0.0f, 0.0f, 0.0f));
				colorParam.set(OC3fGeomParam::Sample(C3fArraySample(zeroColors), kVertexScope));
			}
		}

		for (size_t a = 0; a < attributeSpecs.size(); ++a) {
			const size_t expectedLen = fm.positions.size() * attributeSpecs[a].components;
			const bool haveData = a < fm.attribs.size() && fm.attribs[a].size() == expectedLen;
			if (haveData) {
				attribParams[a].set(OFloatGeomParam::Sample(
					FloatArraySample(fm.attribs[a]), kVertexScope));
			} else {
				zeroAttrib.assign(expectedLen, 0.0f);
				attribParams[a].set(OFloatGeomParam::Sample(
					FloatArraySample(zeroAttrib), kVertexScope));
			}
		}

		// Face-varying attributes are sized against face corners
		// (faceIndices.size()), not points -- that's the whole difference
		// from the point-attribute loop above.
		for (size_t a = 0; a < fvAttributeSpecs.size(); ++a) {
			const size_t expectedLen = fm.faceIndices.size() * fvAttributeSpecs[a].components;
			const bool haveData = a < fm.fvAttribs.size() && fm.fvAttribs[a].size() == expectedLen;
			if (haveData) {
				fvAttribParams[a].set(OFloatGeomParam::Sample(
					FloatArraySample(fm.fvAttribs[a]), kFacevaryingScope));
			} else {
				zeroAttrib.assign(expectedLen, 0.0f);
				fvAttribParams[a].set(OFloatGeomParam::Sample(
					FloatArraySample(zeroAttrib), kFacevaryingScope));
			}
		}

		std::cout << "PROGRESS " << (i + 1) << " " << totalFrames << std::endl;
	}

	for (auto& w : workers) w.join();
	return true;
}

// --- CLI ---------------------------------------------------------------

static void PrintUsage(const char* name)
{
	std::cout << "\nUsage: " << name << " -i <mesh_dir> -o <output.abc> [options]\n\n"
		"Converts a directory of binary .mesh frame files into one Alembic cache.\n\n"
		"Options:\n"
		"  -i, --in <dir>        directory of .mesh frame files (required)\n"
		"  -o, --out <file>      output .abc path (default: output.abc)\n"
		"  -f, --fps <fps>       frame rate (default: 24)\n"
		"  -n, --name <name>     mesh/transform name inside the archive\n"
		"  --uvs <0|1>           write UVs when present (default 1)\n"
		"  --normals <0|1>       write normals when present (default 0)\n"
		"  --colors <0|1>        write vertex colors when present (default 0)\n"
		"  --attrs-dir <dir>     named point-attribute sidecar directory\n"
		"  --attrs-fv-dir <dir>  named face-varying attribute sidecar directory\n"
		"  --attrs-pack <file>   packed point-attribute sidecar file\n"
		"  --attrs-fv-pack <file> packed face-varying attribute sidecar file\n"
		"  -h, --help            this text\n"
		<< std::endl;
}

int main(int argc, char* argv[])
{
	ConvertOptions opt;

	auto nextValue = [&](int& i) -> const char* {
		if (i + 1 >= argc) {
			std::cerr << "Missing value for option: " << argv[i] << std::endl;
			PrintUsage(argv[0]);
			std::exit(1);
		}
		return argv[++i];
	};

	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		if (arg == "-h" || arg == "--help") { PrintUsage(argv[0]); return 0; }
		else if (arg == "-i" || arg == "--in") opt.inputDir = nextValue(i);
		else if (arg == "-o" || arg == "--out") opt.outputFile = nextValue(i);
		else if (arg == "-f" || arg == "--fps") opt.fps = std::stof(nextValue(i));
		else if (arg == "-n" || arg == "--name") opt.meshName = nextValue(i);
		else if (arg == "--uvs") opt.writeUVs = std::stoi(nextValue(i)) != 0;
		else if (arg == "--normals") opt.writeNormals = std::stoi(nextValue(i)) != 0;
		else if (arg == "--colors") opt.writeColors = std::stoi(nextValue(i)) != 0;
		else if (arg == "--attrs-dir") opt.attrsDir = nextValue(i);
		else if (arg == "--attrs-fv-dir") opt.attrsFvDir = nextValue(i);
		else if (arg == "--attrs-pack") opt.attrsPack = nextValue(i);
		else if (arg == "--attrs-fv-pack") opt.attrsFvPack = nextValue(i);
		else {
			std::cerr << "Unknown option: " << arg << std::endl;
			PrintUsage(argv[0]);
			return 1;
		}
	}

	if (opt.inputDir.empty()) {
		std::cerr << "No input directory given. Use -i <mesh_dir>." << std::endl;
		PrintUsage(argv[0]);
		return 1;
	}

	std::cout << "input dir: " << opt.inputDir << "\n"
		<< "output abc: " << opt.outputFile << "\n"
		<< "fps: " << opt.fps << "\n"
		<< "mesh name: " << opt.meshName << std::endl;
	if (!opt.attrsDir.empty())
		std::cout << "attrs dir: " << opt.attrsDir << std::endl;
	if (!opt.attrsFvDir.empty())
		std::cout << "attrs-fv dir: " << opt.attrsFvDir << std::endl;
	if (!opt.attrsPack.empty())
		std::cout << "attrs pack: " << opt.attrsPack << std::endl;
	if (!opt.attrsFvPack.empty())
		std::cout << "attrs-fv pack: " << opt.attrsFvPack << std::endl;

	return ConvertSequence(opt) ? 0 : 1;
}
