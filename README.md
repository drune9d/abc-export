<div align="center">

# AbcExport

**Export Alembic from Houdini on any license tier, with arbitrary point
attributes carried along and correct support for changing topology.**

![Platform](https://img.shields.io/badge/platform-macOS-black)
![License](https://img.shields.io/badge/license-GPL--3.0-blue)
![Houdini](https://img.shields.io/badge/houdini-any%20license%20tier-orange)

</div>

---

## Overview

AbcExport gets a standard Alembic cache out of Houdini, on any license
tier, with your custom point attributes (velocity, age, IDs, whatever
your simulation produces) attached, and handles sequences whose topology
changes from frame to frame (fracture, fluids, remeshing) correctly.

It's two pieces that work together:

- **A Houdini plugin**: a single node you drop into your network. Point
  it at an output folder, click Export, get a finished `.abc` with your
  attributes attached as named geometry parameters.
- **A Blender add-on**: reads the same exported data straight onto a live
  object in your scene, rebuilding the mesh every frame so changing
  topology is always correct.

It works the same way on every Houdini license tier, including Apprentice,
since the export step never renders anything, so there's nothing for a
license restriction to interfere with.

## Requirements

- macOS
- Houdini, any edition (built and tested against 21.0)
- Blender 4.0 or newer (built and tested against 5.2)
- [Homebrew](https://brew.sh), to build the Alembic converter used by the
  Houdini plugin

## Installing the Blender add-on

1. Download **`abcexport-blender-addon.zip`** from
   [Releases](../../releases).
2. In Blender: **Edit → Preferences → Add-ons → Install...**, select the
   zip you downloaded.
3. Enable **AbcExport** in the add-on list.
4. In the 3D viewport, press **N** to open the sidebar: a new
   **AbcExport** tab appears there.

## Installing the Houdini plugin

1. Download **`abcexport-houdini-plugin.zip`** from
   [Releases](../../releases) and unzip it.
2. Build the converter it depends on:
   ```bash
   cd abcexport-houdini-plugin/converter
   ./build.sh
   ```
   This installs `alembic`, `hdf5`, `imath`, and `zlib` via Homebrew if you
   don't already have them.
3. In Houdini: **File → Import → Houdini Digital Asset**, choose
   `abc_export.1.0.hda` from the unzipped folder. Install it for the
   current session, or into your asset library directory so it's always
   available.

## Usage guide

1. In Houdini, build whatever point attributes you need onto your geometry
   as usual: wrangles, VOPs, a solver, however they end up there.
2. Drop an **AbcExport** node right after that geometry and wire it in.
3. Set the node's parameters:
   - **Export** tab: output folder, file name, frame range, frame rate.
     Turn on **Export Blender Data** if you plan to bring the result into
     Blender with attributes; alongside the `.abc` you'll get a
     `<name>_data` folder that the Blender add-on reads directly.
   - **Attributes** tab: leave Auto-Detect on to grab every eligible point
     attribute, or list specific names. A second Auto-Detect toggle does the
     same for face-varying (per face-corner) attributes: a second or third
     UV set, or any other custom data authored at vertex scope.
   - **Geometry** tab: whether to include UVs, normals, vertex colors.
4. Click **Export Alembic**. A progress dialog tracks both phases (writing
   frames out of Houdini, then converting them to Alembic) and can be
   cancelled with Escape. The **Status** field also records what happened;
   your output folder ends up with exactly one `.abc` file.
   - If a run fails or gets cancelled partway through, its intermediate
     mesh/attribute files aren't cleaned up automatically (only a
     successful run does that) and are kept so you can inspect what went
     wrong. Click **Delete Intermediate Files** in the **Advanced** tab to
     clear them; the node remembers where they landed, including when
     **Temp Folder** was left blank and Houdini picked a random location
     for them.
5. In Blender, open the **AbcExport** sidebar tab and click **Load
   Sequence**. Point it at the `<name>_data` folder that **Export Blender
   Data** wrote next to your `.abc`. A new object appears with your mesh
   and attributes at the current frame. (The add-on reads this data
   directly rather than the `.abc`; see Troubleshooting for why.)
6. Scrub the timeline. The object updates automatically every frame,
   rebuilding its geometry and attributes from scratch each time, so a
   sequence that fractures from a thousand points to a hundred thousand
   plays back correctly.

## Requirements and supported versions

- Tested against Houdini 21.0.729 (all license tiers, including Apprentice)
  and Blender 5.2.
- String and array point attributes aren't exported: only plain numeric
  ones (float/int, any component count).
- 4-component attributes (a quaternion, say) land in Blender as a
  `FLOAT_COLOR` attribute, since that's the only built-in 4-float type
  Blender's attribute system exposes. The values are correct; the type
  label just isn't literally "color."

## Performance

The Houdini side writes a small custom binary mesh format (`.mesh`,
positions/topology/UV/normal/color as raw floats via `struct.pack`)
instead of a text interchange format. Face topology specifically (which point each face
corner belongs to) isn't available as a single bulk call on Houdini's
geometry API, so two small internal helper wrangles bake it into real
attributes first; reading that via a bulk attribute call is roughly 90x
faster than walking the geometry's primitives one at a time in Python.
End to end, on a 20-frame, ~340k-point fracture sequence, this runs in
about 20 seconds versus roughly 53 for the same sequence through a text
format: about 2.6x faster overall.

## Face-varying attributes and multiple UV sets

Point attributes are shared by every face corner touching a given point.
Some data legitimately varies per face corner instead: a second UV set
for a decal or lightmap, a per-corner ID after a boolean, hard vertex
normals. Houdini represents that as a **vertex-class** attribute rather
than a point-class one.

The **Attributes** tab's second Auto-Detect toggle picks up every eligible
vertex-class attribute on the input geometry (skipping the primary `uv`
and `N`, which already go through the dedicated UV/normal channels) and
writes each one out as its own named Alembic arbGeomParam with
face-varying scope: the same mechanism a second UV set would need. In
Houdini, add the extra channel with a vertex wrangle (`v@uv2 = ...;`);
"Run Over" must be set to **Vertices**, not Points, or Houdini will store
it per-point instead and it'll get picked up as a regular point attribute
here. Wherever the `.abc` gets read next, it shows up as a plain named
face-varying array property under `.arbGeomParams`.

The Blender add-on doesn't read this data yet: it loads from the
intermediate `mesh`/`attrs` sidecars directly rather than the `.abc` (see
Troubleshooting), and doesn't currently look at `attrs_fv`. Face-varying
attributes come through in the Alembic file itself; bring that into
Blender via its built-in Alembic importer if you need them there today.

## What doesn't come through

The pipeline carries a plain polygon mesh plus point and face-varying
attributes. Compared to a native Houdini Alembic ROP export, the
following is **not** preserved:

- **Primitive and detail attributes**: only point-class attributes are
  exported. A material path (`shop_materialpath`), a per-primitive ID, or
  a detail attribute like a scene name are all dropped.
- **Materials**: no material assignment survives.
- **Groups**: point and primitive groups aren't preserved.
- **Object hierarchy**: the plugin exports exactly one node's geometry as
  a single mesh under a single transform. Multiple objects, nested
  transforms, and per-object visibility aren't part of this pipeline.
- **Non-polygonal primitives**: curves, volumes/VDBs, NURBS. A curve,
  for example, comes out as its points with zero faces, not as a curve.

What *is* preserved: point positions, polygon topology (including
per-frame changing topology), one primary UV set, normals, vertex color
(as a proper Alembic `Cs` channel), any other point attribute (including
non-scalar ones) via the sidecar mechanism described above, and any
additional face-varying (per face-corner) attribute (a second or third
UV set, or other custom vertex-scope data) as its own named Alembic
arbGeomParam with the correct face-varying scope.

**Packed primitives are unpacked automatically.** RBD fragments, packed
disk primitives, and instances present themselves as one point and one
degenerate single-vertex face per piece; exporting that raw would
silently produce a centroid cloud instead of the actual geometry. The
node unpacks everything to real polygons internally before export
(converting polygon soups along the way), and per-piece point attributes
such as `v` and `w` are transferred onto the unpacked points. What the
node passes downstream in your network stays packed and untouched.

**Attribute discovery runs on every frame, not just the first.**
Simulation caches routinely grow attributes mid-sequence; debris
emission starting at frame 50 brings `age`, `density`, and friends with
it. Every attribute that exists on any frame in the range is exported;
frames from before it appeared are zero-filled so the channel stays
aligned across the whole sequence.

If your scene relies on any of the above, this isn't a drop-in substitute
for a native Alembic ROP export: it's specifically for getting polygon
meshes with point-attribute data out under a license tier that can't write
Alembic directly.

## Troubleshooting

**Export button reports "converter not found"**: the Houdini plugin
needs the converter built first (see installation steps above), or its
path has moved. Fix the **Converter Path** field under the **Advanced**
tab.

**Nothing happens when I open the Blender sidebar**: confirm
**AbcExport** is checked in Preferences → Add-ons; installing doesn't
automatically enable it.

**Blender's "Load Sequence" can't find anything, or attributes are
missing**: point it at a folder that directly contains `mesh/` and
`attrs/` subfolders. That's the `<name>_data` folder written next to the
`.abc` when **Export Blender Data** is on. If you exported without that
checkbox, the data went to a temp location and was deleted once the
`.abc` was written; turn the checkbox on and export again.

**Why doesn't Blender just read the `.abc` file directly?** Blender's own
Alembic importer doesn't reliably expose arbitrary custom point
attributes; only position and UVs come through consistently, even with
every relevant import option enabled. Reading the same data Houdini wrote
out directly sidesteps that limitation entirely.

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).

This is free software with a copyleft license: you can use, study,
modify, and redistribute it, but anything you build on it and distribute
must be released under the same terms, so it stays free for everyone
downstream.
