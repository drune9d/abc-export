# Copyright (C) 2026 Aina Olaoluwa
#
# This program is free software: you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the
# Free Software Foundation, either version 3 of the License, or (at your
# option) any later version. It is distributed WITHOUT ANY WARRANTY; see
# <https://www.gnu.org/licenses/> for details.

"""AbcExport -- imports a binary mesh + attribute-sidecar sequence (see
the houdini/ plugin in this project) directly into Blender, rebuilding
the mesh from source geometry every frame instead of going through
Blender's Alembic importer, which does not reliably expose arbitrary
named Alembic point attributes. Handles changing topology (fracture,
fluid, remeshing sims): each frame's vertex/face count can differ from
the last.
"""

import os
import struct

import bpy
from bpy.app.handlers import persistent

bl_info = {
    "name": "AbcExport",
    "author": "Aina Olaoluwa",
    "version": (1, 0, 0),
    "blender": (4, 0, 0),
    "location": "View3D > Sidebar > AbcExport",
    "description": "Import a mesh + point-attribute sequence with correct changing-topology support",
    "category": "Import-Export",
}

ATTR_MAGIC = b"ATR1"
MESH_MAGIC = b"MSH1"

_BLENDER_TYPE_FOR_COMPONENTS = {
    1: "FLOAT",
    2: "FLOAT2",
    3: "FLOAT_VECTOR",
    4: "FLOAT_COLOR",
}
_ACCESSOR_FOR_COMPONENTS = {
    1: "value",
    2: "vector",
    3: "vector",
    4: "color",
}


# --- Core sequence reading (no bpy dependency below this point except the
# final mesh write, so this stays testable/reusable outside the add-on) ---

def _sorted_files(directory):
    return sorted(os.path.join(directory, f) for f in os.listdir(directory))


def _read_mesh_frame(path):
    """Reads a binary MSH1 mesh frame (see houdini/'s PythonModule for the
    exact layout this mirrors). Only positions and face topology are used
    here; the optional UV/normal/color sections that may follow aren't
    read since this add-on doesn't currently import them.
    """
    with open(path, "rb") as f:
        magic = f.read(4)
        if magic != MESH_MAGIC:
            raise ValueError("bad .mesh magic in {}".format(path))

        point_count, vertex_count, face_count, _flags = struct.unpack("<IIII", f.read(16))

        positions = struct.unpack("<{}f".format(point_count * 3), f.read(point_count * 3 * 4))
        verts = [positions[i * 3:i * 3 + 3] for i in range(point_count)]

        face_vertex_counts = struct.unpack("<{}I".format(face_count), f.read(face_count * 4))
        face_vertex_point_indices = struct.unpack("<{}i".format(vertex_count), f.read(vertex_count * 4))

        faces = []
        offset = 0
        for count in face_vertex_counts:
            faces.append(list(face_vertex_point_indices[offset:offset + count]))
            offset += count

    return verts, faces


def _read_attr_frame(path):
    with open(path, "rb") as f:
        magic = f.read(4)
        if magic != ATTR_MAGIC:
            raise ValueError("bad .attr magic in {}".format(path))
        point_count, components = struct.unpack("<II", f.read(8))
        data = f.read(point_count * components * 4)
        values = struct.unpack("<{}f".format(point_count * components), data)
    return point_count, components, values


def list_frames(seq_dir):
    """Sorted mesh frame paths under seq_dir/mesh."""
    return _sorted_files(os.path.join(seq_dir, "mesh"))


def list_attributes(seq_dir):
    """Every attribute subfolder under seq_dir/attrs, sorted."""
    attrs_dir = os.path.join(seq_dir, "attrs")
    if not os.path.isdir(attrs_dir):
        return []
    return sorted(
        name for name in os.listdir(attrs_dir)
        if os.path.isdir(os.path.join(attrs_dir, name))
    )


def load_frame(obj, seq_dir, frame_index, attribute_names):
    """Rebuilds obj's mesh from seq_dir/mesh/<frame_index> and sets each
    named attribute from the matching seq_dir/attrs/<name>/<frame_index>
    sidecar. Matched by sorted position, same convention the exporter uses.
    """
    obj_files = list_frames(seq_dir)
    frame_index = max(0, min(frame_index, len(obj_files) - 1))
    verts, faces = _read_mesh_frame(obj_files[frame_index])

    mesh = obj.data
    mesh.clear_geometry()
    mesh.from_pydata(verts, [], faces)
    mesh.update()

    for name in attribute_names:
        attr_dir = os.path.join(seq_dir, "attrs", name)
        if not os.path.isdir(attr_dir):
            continue
        attr_files = _sorted_files(attr_dir)
        if frame_index >= len(attr_files):
            continue
        point_count, components, values = _read_attr_frame(attr_files[frame_index])

        if point_count != len(mesh.vertices):
            continue

        blender_type = _BLENDER_TYPE_FOR_COMPONENTS.get(components)
        if blender_type is None:
            continue

        existing = mesh.attributes.get(name)
        if existing is not None and existing.data_type != blender_type:
            mesh.attributes.remove(existing)
            existing = None
        attr = existing if existing is not None else mesh.attributes.new(name=name, type=blender_type, domain="POINT")

        accessor = _ACCESSOR_FOR_COMPONENTS[components]
        attr.data.foreach_set(accessor, values)

    return len(obj_files)


# --- Add-on data/UI/operators ---

class AbcExportSettings(bpy.types.PropertyGroup):
    sequence_dir: bpy.props.StringProperty(
        name="Sequence Folder",
        description="Folder containing mesh/ and attrs/, written by the Houdini plugin",
        subtype="DIR_PATH",
    )
    auto_detect: bpy.props.BoolProperty(
        name="Auto-Detect Attributes",
        default=True,
    )
    attribute_names: bpy.props.StringProperty(
        name="Attributes",
        description="Space or comma separated attribute names (used when Auto-Detect is off)",
        default="",
    )
    is_loaded: bpy.props.BoolProperty(default=False, options={"HIDDEN"})
    frame_count: bpy.props.IntProperty(default=0, options={"HIDDEN"})


def _resolve_attribute_names(settings, seq_dir):
    if settings.auto_detect:
        return list_attributes(seq_dir)
    return [s for s in settings.attribute_names.replace(",", " ").split() if s]


class ABC_EXPORT_OT_load_sequence(bpy.types.Operator):
    bl_idname = "abc_export.load_sequence"
    bl_label = "Load Sequence"
    bl_description = "Create a new object from an exported mesh + attribute sequence"
    bl_options = {"REGISTER", "UNDO"}

    directory: bpy.props.StringProperty(subtype="DIR_PATH")
    filter_folder: bpy.props.BoolProperty(default=True, options={"HIDDEN"})

    def invoke(self, context, event):
        context.window_manager.fileselect_add(self)
        return {"RUNNING_MODAL"}

    def execute(self, context):
        seq_dir = self.directory.rstrip("/\\")
        if not seq_dir or not os.path.isdir(os.path.join(seq_dir, "mesh")):
            self.report({"ERROR"}, "No mesh/ folder found in: {}".format(seq_dir))
            return {"CANCELLED"}

        name = os.path.basename(seq_dir.rstrip("/\\")) or "Sequence"
        mesh = bpy.data.meshes.new(name)
        obj = bpy.data.objects.new(name, mesh)
        context.collection.objects.link(obj)

        obj.abc_export.sequence_dir = seq_dir
        attribute_names = _resolve_attribute_names(obj.abc_export, seq_dir)
        frame_count = load_frame(obj, seq_dir, context.scene.frame_current, attribute_names)
        obj.abc_export.is_loaded = True
        obj.abc_export.frame_count = frame_count

        for other in context.selected_objects:
            other.select_set(False)
        obj.select_set(True)
        context.view_layer.objects.active = obj

        self.report({"INFO"}, "Loaded {} ({} frames)".format(name, frame_count))
        return {"FINISHED"}


class ABC_EXPORT_OT_refresh_frame(bpy.types.Operator):
    bl_idname = "abc_export.refresh_frame"
    bl_label = "Refresh Current Frame"
    bl_description = "Reload the active object's mesh and attributes at the current frame"
    bl_options = {"REGISTER", "UNDO"}

    @classmethod
    def poll(cls, context):
        obj = context.active_object
        return obj is not None and obj.type == "MESH" and obj.abc_export.is_loaded

    def execute(self, context):
        obj = context.active_object
        settings = obj.abc_export
        attribute_names = _resolve_attribute_names(settings, settings.sequence_dir)
        settings.frame_count = load_frame(obj, settings.sequence_dir, context.scene.frame_current, attribute_names)
        return {"FINISHED"}


class VIEW3D_PT_abc_export(bpy.types.Panel):
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "AbcExport"
    bl_label = "AbcExport"

    def draw(self, context):
        layout = self.layout
        layout.operator("abc_export.load_sequence", icon="IMPORT")

        obj = context.active_object
        if obj is None or not obj.abc_export.is_loaded:
            layout.label(text="No sequence loaded on the active object.")
            return

        settings = obj.abc_export
        box = layout.box()
        box.label(text=obj.name, icon="OBJECT_DATA")
        box.label(text=settings.sequence_dir)
        box.label(text="{} frame(s)".format(settings.frame_count))
        box.prop(settings, "auto_detect")
        if not settings.auto_detect:
            box.prop(settings, "attribute_names")
        box.operator("abc_export.refresh_frame", icon="FILE_REFRESH")


@persistent
def _on_frame_change(scene, depsgraph=None):
    frame = scene.frame_current
    for obj in bpy.data.objects:
        settings = getattr(obj, "abc_export", None)
        if settings is None or not settings.is_loaded or not settings.sequence_dir:
            continue
        attribute_names = _resolve_attribute_names(settings, settings.sequence_dir)
        try:
            settings.frame_count = load_frame(obj, settings.sequence_dir, frame, attribute_names)
        except (OSError, ValueError):
            continue


_classes = (
    AbcExportSettings,
    ABC_EXPORT_OT_load_sequence,
    ABC_EXPORT_OT_refresh_frame,
    VIEW3D_PT_abc_export,
)


def register():
    for cls in _classes:
        bpy.utils.register_class(cls)
    bpy.types.Object.abc_export = bpy.props.PointerProperty(type=AbcExportSettings)
    if _on_frame_change not in bpy.app.handlers.frame_change_post:
        bpy.app.handlers.frame_change_post.append(_on_frame_change)


def unregister():
    if _on_frame_change in bpy.app.handlers.frame_change_post:
        bpy.app.handlers.frame_change_post.remove(_on_frame_change)
    del bpy.types.Object.abc_export
    for cls in reversed(_classes):
        bpy.utils.unregister_class(cls)


if __name__ == "__main__":
    register()
