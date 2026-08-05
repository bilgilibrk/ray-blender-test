"""Kenney Racing Kit level editor for Blender.

Turns Blender into the level editor for the raylib top-down racer: import kit
pieces as prefabs, snap them to the kit's one-unit grid, draw the racing line as
a curve, tag spawns and scenery colliders, then export a `.level.json` the
engine loads directly.

Install: Edit > Preferences > Add-ons > Install from Disk, pick this file, and
enable "Kenney Racing Kit Level Editor". The tools appear in the 3D viewport
sidebar (N) under the "Racing Kit" tab.

Coordinate spaces
-----------------
Blender is Z-up; the engine (like glTF) is Y-up. A Blender point ``(x, y, z)``
becomes engine ``(x, z, -y)``, and a Blender basis ``M`` becomes ``C·M·Cᵀ``.
Because imported prefabs have their transforms applied, what you see in the
viewport is exactly what the engine draws.
"""

bl_info = {
    "name": "Kenney Racing Kit Level Editor",
    "author": "generated for the raylib top-down racer",
    "version": (1, 0, 0),
    "blender": (4, 0, 0),
    "location": "View3D > Sidebar > Racing Kit",
    "description": "Author racing levels from the Kenney Racing Kit and export them for the raylib engine",
    "category": "Import-Export",
}

import json
import math
import os

import bpy
from bpy.props import (BoolProperty, EnumProperty, FloatProperty, IntProperty,
                       PointerProperty, StringProperty)
from bpy.types import Object, Operator, Panel, PropertyGroup
from bpy_extras.io_utils import ExportHelper
from mathutils import Matrix, Vector

LEVEL_FORMAT = "kenney-topdown-racer"
LEVEL_VERSION = 1

# Custom properties used to tag objects.
PROP_PREFAB = "kr_prefab"      # str: kit model name, marks an object as a prop
PROP_TYPE = "kr_type"          # str: spawn | checkpoint | racingline | collider
PROP_INDEX = "kr_index"        # int: ordering for spawns and checkpoints
PROP_WIDTH = "kr_width"        # float: gate or track width override
PROP_SOLID = "kr_solid"        # bool: emit a collider for this prop

# The kit's tiles all share this cell origin offset, in Blender XY.
GRID_OFFSET = (-0.35, 0.65)
GRID_SIZE = 1.0

# Prefab name prefixes that get a collider when "auto colliders" is on.
SOLID_PREFIXES = (
    "barrier", "fence", "rail", "tent", "grandStand", "pits", "tree",
    "lightPost", "lightRed", "lightColored", "bannerTower", "billboard",
    "radarEquipment", "pylon", "flag", "overhead", "camera",
)


# ---------------------------------------------------------------------------
# Coordinate conversion
# ---------------------------------------------------------------------------

# Blender (Z-up) -> engine (Y-up): (x, y, z) -> (x, z, -y)
BLENDER_TO_ENGINE = Matrix(((1, 0, 0), (0, 0, 1), (0, -1, 0)))


def to_engine_point(v):
    """Convert a Blender world position to engine space."""
    return [round(v.x, 5), round(v.z, 5), round(-v.y, 5)]


def decompose_engine(matrix_world):
    """Return (position, euler_xyz_degrees, scale) in engine space.

    The euler triple matches raymath's ``MatrixRotateXYZ``, which composes as
    Rx·Ry·Rz acting on column vectors.
    """
    position = to_engine_point(matrix_world.translation)

    basis = matrix_world.to_3x3()
    engine_basis = BLENDER_TO_ENGINE @ basis @ BLENDER_TO_ENGINE.transposed()

    scale = [engine_basis.col[i].length for i in range(3)]
    rot = engine_basis.copy()
    for i in range(3):
        if scale[i] > 1e-9:
            rot.col[i] = engine_basis.col[i] / scale[i]

    # Guard against a mirrored (negative determinant) basis, which euler
    # extraction cannot represent; flip X and record it in the scale.
    if rot.determinant() < 0.0:
        rot.col[0] = -rot.col[0]
        scale[0] = -scale[0]

    if _is_pure_yaw(rot):
        # The overwhelmingly common case for a top-down track. Handled directly
        # because the generic asin branch below picks the wrong solution at
        # exactly 180 degrees, turning a half-turn into [180, 0, 180].
        x, y, z = 0.0, math.atan2(rot[0][2], rot[0][0]), 0.0
    else:
        sy = max(-1.0, min(1.0, rot[0][2]))
        y = math.asin(sy)
        if abs(math.cos(y)) > 1e-6:
            x = math.atan2(-rot[1][2], rot[2][2])
            z = math.atan2(-rot[0][1], rot[0][0])
        else:
            # Gimbal lock: only x+z is observable, so pin z and fold it into x.
            x = math.atan2(rot[1][0], rot[1][1])
            z = 0.0

    degrees = [round(math.degrees(a), 4) for a in (x, y, z)]
    return position, degrees, [round(s, 5) for s in scale]


def _is_pure_yaw(rot, eps=1e-5):
    """True when the basis is a rotation about the engine's +Y axis alone."""
    return (abs(rot[1][1] - 1.0) < eps and
            abs(rot[0][1]) < eps and abs(rot[2][1]) < eps and
            abs(rot[1][0]) < eps and abs(rot[1][2]) < eps)


def engine_yaw_degrees(obj):
    """Yaw about the engine's +Y axis, in degrees.

    Measured from where the object's forward axis actually points rather than
    from an euler triple: euler decomposition has several valid answers for the
    same orientation, and picking the wrong one would silently spin spawn and
    checkpoint gates. The engine's +Z is Blender's -Y.
    """
    forward = obj.matrix_world.to_3x3() @ Vector((0.0, -1.0, 0.0))
    return math.degrees(math.atan2(forward.x, -forward.y))


# ---------------------------------------------------------------------------
# Kit discovery
# ---------------------------------------------------------------------------

# Set by scripts that import this module directly (headless track building),
# where Blender's add-on preferences do not exist.
KIT_PATH_OVERRIDE = ""


def addon_prefs(context):
    addon = context.preferences.addons.get(__name__)
    return addon.preferences if addon else None


def kit_directory(context):
    if KIT_PATH_OVERRIDE:
        return bpy.path.abspath(KIT_PATH_OVERRIDE)
    prefs = addon_prefs(context)
    if prefs and prefs.kit_path:
        return bpy.path.abspath(prefs.kit_path)
    return ""


def list_kit_models(context):
    directory = kit_directory(context)
    if not directory or not os.path.isdir(directory):
        return []
    names = sorted(f[:-4] for f in os.listdir(directory) if f.endswith(".glb"))
    return names


_enum_cache = {"items": [], "dir": None}


def prefab_enum_items(self, context):
    """Enum callback. Blender requires the returned strings to stay alive, so
    the list is cached rather than rebuilt per redraw."""
    directory = kit_directory(context)
    if _enum_cache["dir"] != directory:
        names = list_kit_models(context)
        _enum_cache["items"] = [(n, n, f"Place {n}") for n in names] or [("", "<no kit found>", "")]
        _enum_cache["dir"] = directory
    return _enum_cache["items"]


# ---------------------------------------------------------------------------
# Scene settings
# ---------------------------------------------------------------------------

class KRLevelSettings(PropertyGroup):
    level_name: StringProperty(name="Level Name", default="Untitled Circuit")
    laps: IntProperty(name="Laps", default=3, min=1, max=99)
    track_width: FloatProperty(name="Track Width", default=0.72, min=0.1, max=50.0,
                               description="Default drivable width along the racing line")
    waypoint_spacing: FloatProperty(name="Waypoint Spacing", default=0.5, min=0.05, max=10.0,
                                    description="Distance between exported racing-line points")
    auto_colliders: BoolProperty(name="Auto Colliders From Scenery", default=True,
                                 description="Emit a box collider for scenery prefabs "
                                             "(barriers, trees, grandstands and friends)")
    sky_color: bpy.props.FloatVectorProperty(name="Sky", subtype="COLOR", size=3,
                                             default=(0.486, 0.690, 0.839), min=0.0, max=1.0)
    # Matches the kit's own "grass" material so the fill plane under the track
    # is invisible against the grass border baked into every road tile.
    ground_color: bpy.props.FloatVectorProperty(name="Ground", subtype="COLOR", size=3,
                                                default=(0.302, 0.561, 0.431), min=0.0, max=1.0)
    prefab: EnumProperty(name="Prefab", items=prefab_enum_items)


class KRAddonPreferences(bpy.types.AddonPreferences):
    bl_idname = __name__

    kit_path: StringProperty(
        name="Kit Models Folder",
        subtype="DIR_PATH",
        default="//assets/models/",
        description="Folder holding the Kenney Racing Kit .glb files",
    )

    def draw(self, context):
        self.layout.prop(self, "kit_path")


# ---------------------------------------------------------------------------
# Prefab placement
# ---------------------------------------------------------------------------

def snap_value(value, offset):
    return round((value - offset) / GRID_SIZE) * GRID_SIZE + offset


def snap_object_to_grid(obj):
    obj.location.x = snap_value(obj.location.x, GRID_OFFSET[0])
    obj.location.y = snap_value(obj.location.y, GRID_OFFSET[1])


def find_prefab_mesh(name):
    """Reuse an already-imported prefab's mesh datablock when possible."""
    for obj in bpy.data.objects:
        if obj.get(PROP_PREFAB) == name and obj.type == "MESH":
            return obj.data
    return None


def import_prefab(context, name):
    """Import a kit .glb, bake its transform, and return a single mesh object.

    Baking matters: with the transform applied, the object's own transform is
    exactly what the engine should apply to the model, so the viewport is a
    faithful preview.
    """
    directory = kit_directory(context)
    path = os.path.join(directory, f"{name}.glb")
    if not os.path.isfile(path):
        raise FileNotFoundError(path)

    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=path)
    imported = [o for o in bpy.data.objects if o not in before]
    meshes = [o for o in imported if o.type == "MESH"]
    if not meshes:
        for o in imported:
            bpy.data.objects.remove(o, do_unlink=True)
        raise RuntimeError(f"{name}.glb contained no mesh")

    # Drop the importer's container empties, keeping the mesh world transforms.
    for o in imported:
        if o.type != "MESH":
            for child in o.children:
                child.matrix_world = child.matrix_world.copy()
                child.parent = None
            bpy.data.objects.remove(o, do_unlink=True)

    bpy.ops.object.select_all(action="DESELECT")
    for o in meshes:
        o.select_set(True)
    context.view_layer.objects.active = meshes[0]
    if len(meshes) > 1:
        bpy.ops.object.join()

    obj = context.view_layer.objects.active
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    obj.name = name
    obj.data.name = name
    obj[PROP_PREFAB] = name
    return obj


class KR_OT_add_prefab(Operator):
    bl_idname = "kr.add_prefab"
    bl_label = "Add Prefab"
    bl_description = "Place the selected kit piece at the 3D cursor"
    bl_options = {"REGISTER", "UNDO"}

    snap: BoolProperty(name="Snap To Grid", default=True)

    def execute(self, context):
        settings = context.scene.kr_level
        name = settings.prefab
        if not name:
            self.report({"ERROR"}, "No kit model selected (check the add-on's Kit Models Folder)")
            return {"CANCELLED"}

        existing = find_prefab_mesh(name)
        if existing is not None:
            obj = bpy.data.objects.new(name, existing)
            obj[PROP_PREFAB] = name
            context.collection.objects.link(obj)
        else:
            try:
                obj = import_prefab(context, name)
            except (FileNotFoundError, RuntimeError) as exc:
                self.report({"ERROR"}, str(exc))
                return {"CANCELLED"}

        obj.location = context.scene.cursor.location.copy()
        if self.snap:
            snap_object_to_grid(obj)

        bpy.ops.object.select_all(action="DESELECT")
        obj.select_set(True)
        context.view_layer.objects.active = obj
        return {"FINISHED"}


class KR_OT_snap_selection(Operator):
    bl_idname = "kr.snap_selection"
    bl_label = "Snap To Kit Grid"
    bl_description = "Snap selected objects to the kit's one-unit tile grid"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        count = 0
        for obj in context.selected_objects:
            snap_object_to_grid(obj)
            count += 1
        self.report({"INFO"}, f"Snapped {count} object(s)")
        return {"FINISHED"}


class KR_OT_rotate_90(Operator):
    bl_idname = "kr.rotate_90"
    bl_label = "Rotate 90°"
    bl_description = "Rotate selected pieces a quarter turn about the up axis"
    bl_options = {"REGISTER", "UNDO"}

    clockwise: BoolProperty(name="Clockwise", default=True)

    def execute(self, context):
        step = -math.pi / 2 if self.clockwise else math.pi / 2
        for obj in context.selected_objects:
            obj.rotation_euler.z += step
        return {"FINISHED"}


# ---------------------------------------------------------------------------
# Tagging
# ---------------------------------------------------------------------------

def _add_empty(context, name, display, size=0.5):
    empty = bpy.data.objects.new(name, None)
    empty.empty_display_type = display
    empty.empty_display_size = size
    empty.location = context.scene.cursor.location.copy()
    context.collection.objects.link(empty)
    return empty


def _next_index(kind):
    used = [o.get(PROP_INDEX, 0) for o in bpy.data.objects if o.get(PROP_TYPE) == kind]
    return (max(used) + 1) if used else 0


class KR_OT_add_spawn(Operator):
    bl_idname = "kr.add_spawn"
    bl_label = "Add Spawn"
    bl_description = "Add a grid slot where a car starts. Its +Y axis points down the track"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        empty = _add_empty(context, "Spawn", "SINGLE_ARROW", 0.6)
        empty[PROP_TYPE] = "spawn"
        empty[PROP_INDEX] = _next_index("spawn")
        empty.rotation_euler.x = math.pi / 2   # point the arrow along +Y
        bpy.ops.object.select_all(action="DESELECT")
        empty.select_set(True)
        context.view_layer.objects.active = empty
        return {"FINISHED"}


class KR_OT_add_checkpoint(Operator):
    bl_idname = "kr.add_checkpoint"
    bl_label = "Add Checkpoint"
    bl_description = "Add a gate cars must cross in order. Index 0 is the finish line"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        empty = _add_empty(context, "Checkpoint", "ARROWS", 0.7)
        empty[PROP_TYPE] = "checkpoint"
        empty[PROP_INDEX] = _next_index("checkpoint")
        empty[PROP_WIDTH] = context.scene.kr_level.track_width * 2.0
        bpy.ops.object.select_all(action="DESELECT")
        empty.select_set(True)
        context.view_layer.objects.active = empty
        return {"FINISHED"}


class KR_OT_add_racingline(Operator):
    bl_idname = "kr.add_racingline"
    bl_label = "Add Racing Line"
    bl_description = "Create the closed curve the engine uses for lap progress, AI and off-track tests"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        curve = bpy.data.curves.new("RacingLine", type="CURVE")
        curve.dimensions = "3D"
        spline = curve.splines.new("BEZIER")
        spline.bezier_points.add(3)
        radius = 4.0
        for i, point in enumerate(spline.bezier_points):
            angle = i * math.pi / 2
            point.co = Vector((math.cos(angle) * radius, math.sin(angle) * radius, 0.02))
            point.handle_left_type = "AUTO"
            point.handle_right_type = "AUTO"
        spline.use_cyclic_u = True

        obj = bpy.data.objects.new("RacingLine", curve)
        obj[PROP_TYPE] = "racingline"
        obj[PROP_WIDTH] = context.scene.kr_level.track_width
        context.collection.objects.link(obj)
        bpy.ops.object.select_all(action="DESELECT")
        obj.select_set(True)
        context.view_layer.objects.active = obj
        self.report({"INFO"}, "Edit the curve to trace the centre of your track")
        return {"FINISHED"}


class KR_OT_tag_collider(Operator):
    bl_idname = "kr.tag_collider"
    bl_label = "Tag As Collider"
    bl_description = "Export the selected objects' boxes as solid obstacles"
    bl_options = {"REGISTER", "UNDO"}

    clear: BoolProperty(name="Clear Instead", default=False)

    def execute(self, context):
        for obj in context.selected_objects:
            if self.clear:
                obj.pop(PROP_SOLID, None)
                if obj.get(PROP_TYPE) == "collider":
                    obj.pop(PROP_TYPE, None)
            elif obj.get(PROP_PREFAB):
                obj[PROP_SOLID] = True           # a prop that is also solid
            else:
                obj[PROP_TYPE] = "collider"      # an invisible blocking volume
        return {"FINISHED"}


# ---------------------------------------------------------------------------
# Export
# ---------------------------------------------------------------------------

def collect_racing_line(obj, spacing):
    """Sample a curve object into an ordered list of engine-space points."""
    depsgraph = bpy.context.evaluated_depsgraph_get()
    evaluated = obj.evaluated_get(depsgraph)
    try:
        mesh = evaluated.to_mesh()
    except RuntimeError:
        return []

    try:
        world = obj.matrix_world
        points = [world @ v.co.copy() for v in mesh.vertices]
    finally:
        evaluated.to_mesh_clear()

    if len(points) < 3:
        return []

    # Thin out to the requested spacing, always keeping the loop closed.
    kept = [points[0]]
    for p in points[1:]:
        if (p - kept[-1]).length >= spacing:
            kept.append(p)
    if len(kept) > 2 and (kept[-1] - kept[0]).length < spacing * 0.5:
        kept.pop()
    return kept


def object_box(obj):
    """Local bounding box centre and half extents, with object scale applied."""
    corners = [Vector(c) for c in obj.bound_box]
    lo = Vector((min(c.x for c in corners), min(c.y for c in corners), min(c.z for c in corners)))
    hi = Vector((max(c.x for c in corners), max(c.y for c in corners), max(c.z for c in corners)))
    centre_local = (lo + hi) * 0.5
    size = hi - lo
    scale = obj.matrix_world.to_scale()
    half = Vector((abs(size.x * scale.x) * 0.5,
                   abs(size.y * scale.y) * 0.5,
                   abs(size.z * scale.z) * 0.5))
    return obj.matrix_world @ centre_local, half


def is_solid_prefab(name):
    return any(name.startswith(prefix) for prefix in SOLID_PREFIXES)


def build_level_dict(context, report=None):
    scene = context.scene
    settings = scene.kr_level

    props, colliders, spawns, checkpoints = [], [], [], []
    racing_lines = []

    for obj in scene.objects:
        if not obj.visible_get() and obj.hide_render:
            continue

        kind = obj.get(PROP_TYPE)
        prefab = obj.get(PROP_PREFAB)

        if kind == "racingline":
            racing_lines.append(obj)
            continue
        if kind == "spawn":
            spawns.append(obj)
            continue
        if kind == "checkpoint":
            checkpoints.append(obj)
            continue
        if kind == "collider":
            centre, half = object_box(obj)
            colliders.append({
                "pos": to_engine_point(centre),
                "half": [round(half.x, 4), round(half.y, 4)],
                "height": round(half.z * 2.0, 4),
                "yaw": round(engine_yaw_degrees(obj), 3),
            })
            continue

        if prefab:
            position, rotation, scale = decompose_engine(obj.matrix_world)
            entry = {"model": prefab, "pos": position}
            if any(abs(a) > 1e-4 for a in rotation):
                entry["rot"] = rotation
            if any(abs(s - 1.0) > 1e-4 for s in scale):
                entry["scale"] = scale
            props.append(entry)

            solid = obj.get(PROP_SOLID)
            if solid is None and settings.auto_colliders:
                solid = is_solid_prefab(prefab)
            if solid:
                centre, half = object_box(obj)
                colliders.append({
                    "pos": to_engine_point(centre),
                    "half": [round(half.x, 4), round(half.y, 4)],
                    "height": round(half.z * 2.0, 4),
                    "yaw": round(engine_yaw_degrees(obj), 3),
                })

    spawns.sort(key=lambda o: o.get(PROP_INDEX, 0))
    checkpoints.sort(key=lambda o: o.get(PROP_INDEX, 0))

    waypoints = []
    if racing_lines:
        if len(racing_lines) > 1 and report:
            report({"WARNING"}, f"{len(racing_lines)} racing lines found, using '{racing_lines[0].name}'")
        line = racing_lines[0]
        width = float(line.get(PROP_WIDTH, settings.track_width))
        for point in collect_racing_line(line, settings.waypoint_spacing):
            waypoints.append({"pos": to_engine_point(point), "width": round(width, 4)})
    elif report:
        report({"WARNING"}, "No racing line in the scene: the engine cannot track laps without one")

    def colour(rgb):
        return [int(round(c * 255)) for c in rgb] + [255]

    return {
        "format": LEVEL_FORMAT,
        "version": LEVEL_VERSION,
        "name": settings.level_name,
        "settings": {
            "laps": settings.laps,
            "track_width": round(settings.track_width, 4),
            "sky_color": colour(settings.sky_color),
            "ground_color": colour(settings.ground_color),
            "sun_direction": [-0.45, -1.0, -0.35],
        },
        "props": props,
        "colliders": colliders,
        "spawns": [
            {"pos": to_engine_point(o.matrix_world.translation),
             "yaw": round(engine_yaw_degrees(o), 3)}
            for o in spawns
        ],
        "waypoints": waypoints,
        "checkpoints": [
            {"pos": to_engine_point(o.matrix_world.translation),
             "yaw": round(engine_yaw_degrees(o), 3),
             "width": round(float(o.get(PROP_WIDTH, settings.track_width * 2.0)), 4)}
            for o in checkpoints
        ],
    }


def write_level(context, filepath, report=None):
    data = build_level_dict(context, report)
    os.makedirs(os.path.dirname(os.path.abspath(filepath)) or ".", exist_ok=True)
    with open(filepath, "w", encoding="utf-8") as handle:
        json.dump(data, handle, indent=1)
        handle.write("\n")
    return data


class KR_OT_export_level(Operator, ExportHelper):
    bl_idname = "kr.export_level"
    bl_label = "Export Level"
    bl_description = "Write the scene as a level JSON for the raylib engine"

    filename_ext = ".json"
    filter_glob: StringProperty(default="*.json", options={"HIDDEN"})

    def execute(self, context):
        data = write_level(context, self.filepath, self.report)
        self.report({"INFO"},
                    f"{len(data['props'])} props, {len(data['waypoints'])} waypoints, "
                    f"{len(data['colliders'])} colliders -> {os.path.basename(self.filepath)}")
        return {"FINISHED"}


class KR_OT_validate(Operator):
    bl_idname = "kr.validate"
    bl_label = "Validate Level"
    bl_description = "Check the scene has everything the engine needs"

    def execute(self, context):
        data = build_level_dict(context)
        problems = []
        if not data["waypoints"]:
            problems.append("no racing line")
        elif len(data["waypoints"]) < 3:
            problems.append("racing line has fewer than 3 points")
        if not data["spawns"]:
            problems.append("no spawn points")
        if not data["props"]:
            problems.append("no track pieces")

        if problems:
            self.report({"WARNING"}, "; ".join(problems))
        else:
            self.report({"INFO"},
                        f"OK — {len(data['props'])} props, {len(data['spawns'])} spawns, "
                        f"{len(data['waypoints'])} waypoints, {len(data['colliders'])} colliders")
        return {"FINISHED"}


# ---------------------------------------------------------------------------
# UI
# ---------------------------------------------------------------------------

class KR_PT_panel(Panel):
    bl_label = "Racing Kit"
    bl_idname = "KR_PT_panel"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Racing Kit"

    def draw(self, context):
        layout = self.layout
        settings = context.scene.kr_level

        box = layout.box()
        box.label(text="Place", icon="MESH_GRID")
        box.prop(settings, "prefab", text="")
        box.operator(KR_OT_add_prefab.bl_idname, icon="ADD")
        row = box.row(align=True)
        row.operator(KR_OT_rotate_90.bl_idname, text="Rotate ⟳").clockwise = True
        row.operator(KR_OT_rotate_90.bl_idname, text="Rotate ⟲").clockwise = False
        box.operator(KR_OT_snap_selection.bl_idname, icon="SNAP_GRID")

        box = layout.box()
        box.label(text="Track Logic", icon="CURVE_PATH")
        box.operator(KR_OT_add_racingline.bl_idname, icon="CURVE_BEZCIRCLE")
        row = box.row(align=True)
        row.operator(KR_OT_add_spawn.bl_idname, icon="TRACKING")
        row.operator(KR_OT_add_checkpoint.bl_idname, icon="EMPTY_ARROWS")
        row = box.row(align=True)
        row.operator(KR_OT_tag_collider.bl_idname, text="Tag Solid").clear = False
        row.operator(KR_OT_tag_collider.bl_idname, text="Clear Solid").clear = True

        box = layout.box()
        box.label(text="Level Settings", icon="SETTINGS")
        box.prop(settings, "level_name")
        box.prop(settings, "laps")
        box.prop(settings, "track_width")
        box.prop(settings, "waypoint_spacing")
        box.prop(settings, "auto_colliders")
        row = box.row(align=True)
        row.prop(settings, "sky_color")
        row.prop(settings, "ground_color")

        box = layout.box()
        box.operator(KR_OT_validate.bl_idname, icon="CHECKMARK")
        box.operator(KR_OT_export_level.bl_idname, icon="EXPORT")


CLASSES = (
    KRAddonPreferences,
    KRLevelSettings,
    KR_OT_add_prefab,
    KR_OT_snap_selection,
    KR_OT_rotate_90,
    KR_OT_add_spawn,
    KR_OT_add_checkpoint,
    KR_OT_add_racingline,
    KR_OT_tag_collider,
    KR_OT_export_level,
    KR_OT_validate,
    KR_PT_panel,
)


def menu_export(self, context):
    self.layout.operator(KR_OT_export_level.bl_idname, text="Racing Level (.json)")


def register():
    for cls in CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.Scene.kr_level = PointerProperty(type=KRLevelSettings)
    bpy.types.TOPBAR_MT_file_export.append(menu_export)


def unregister():
    bpy.types.TOPBAR_MT_file_export.remove(menu_export)
    del bpy.types.Scene.kr_level
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)


if __name__ == "__main__":
    register()
