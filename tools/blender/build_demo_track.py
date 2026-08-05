"""Build the demo circuit in Blender and export it for the engine.

Run headless:

    blender --background --python tools/blender/build_demo_track.py

Produces ``levels/circuit01.level.json`` (what the game loads) and
``levels/circuit01.blend`` (open this to edit the track by hand).

The track is described as a list of moves along the kit's tile grid. Every kit
road port sits at the centre of a one-unit cell edge and corners are exact
quarter arcs of radius ``N - 0.5``, so tiles chain together without gaps and the
racing line can be derived from the same walk that places the art.
"""

import math
import os
import random
import sys

import bpy
from mathutils import Vector

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, "tools", "blender"))

import io_kenney_racing as kr  # noqa: E402

KIT = os.path.join(REPO, "assets", "models")
OUT_JSON = os.path.join(REPO, "levels", "circuit01.level.json")
OUT_BLEND = os.path.join(REPO, "levels", "circuit01.blend")

STRAIGHT = "roadStraight"
CORNER = "roadCornerLarge"
CORNER_CELLS = 2
START_TILE = "roadStartPositions"

# Which tile of the opening straight carries the starting grid. Far enough in
# that the six staggered grid slots all land on the straight.
START_LINE_TILE = 8

# Moves around the loop: ("s", tiles) drives straight, ("t", "N"/"E"/"S"/"W")
# turns to a new heading through a corner piece. The walk is asserted to close.
TRACK = [
    ("s", 20), ("t", "E"),
    ("s", 8),  ("t", "S"),
    ("s", 5),  ("t", "E"),
    ("s", 8),  ("t", "S"),
    ("s", 12), ("t", "W"),
    ("s", 19), ("t", "N"),
]

HEADINGS = {"N": (0.0, 1.0), "E": (1.0, 0.0), "S": (0.0, -1.0), "W": (-1.0, 0.0)}

# Minimum distance from the centre line to anything solid: half the lane (0.345)
# plus the barrier's own reach plus a margin of grass to spin out on.
BARRIER_CLEARANCE = 0.68


# ---------------------------------------------------------------------------
# Engine-space helpers. Positions here are (x, z); the engine's Y is always 0
# for track pieces.
# ---------------------------------------------------------------------------

def rot_engine(v, theta):
    """Rotate (x, z) by `theta` radians about the engine's +Y axis."""
    c, s = math.cos(theta), math.sin(theta)
    return (c * v[0] + s * v[1], -s * v[0] + c * v[1])


def origin_for_block(centre, theta, width, depth):
    """Model origin that lands a `width` x `depth` cell block at `centre`."""
    d = (width / 2.0 - 0.35, -(depth / 2.0 + 0.65))
    r = rot_engine(d, theta)
    return (centre[0] - r[0], centre[1] - r[1])


def corner_ports(n):
    """Ports of an n x n corner at theta=0: (offset from centre, outward normal)."""
    port_z = ((0.5 - n / 2.0, n / 2.0), (0.0, 1.0))
    port_x = ((n / 2.0, 0.5 - n / 2.0), (1.0, 0.0))
    return port_z, port_x


def near(a, b, eps=1e-4):
    return abs(a[0] - b[0]) < eps and abs(a[1] - b[1]) < eps


def solve_corner(pos, heading, new_heading, n):
    """Find the corner placement that enters at `pos` and turns to `new_heading`.

    Returns (theta, block_centre, exit_pos). Brute-forcing the four rotations
    and both traversal directions avoids a pile of sign conventions.
    """
    port_z, port_x = corner_ports(n)
    want_in = (-heading[0], -heading[1])

    for quarter in range(4):
        theta = quarter * math.pi / 2.0
        for entry, exit_ in ((port_z, port_x), (port_x, port_z)):
            n_in = rot_engine(entry[1], theta)
            n_out = rot_engine(exit_[1], theta)
            if near(n_in, want_in) and near(n_out, new_heading):
                rel_in = rot_engine(entry[0], theta)
                rel_out = rot_engine(exit_[0], theta)
                centre = (pos[0] - rel_in[0], pos[1] - rel_in[1])
                exit_pos = (centre[0] + rel_out[0], centre[1] + rel_out[1])
                return theta, centre, exit_pos
    raise RuntimeError(f"no corner rotation turns {heading} into {new_heading}")


def arc_points(entry, heading, exit_pos, new_heading, radius, steps=10):
    """Sample the quarter arc a corner piece describes."""
    centre = (entry[0] + heading[0] * radius, entry[1] + heading[1] * radius)
    v0 = (entry[0] - centre[0], entry[1] - centre[1])
    v1 = (exit_pos[0] - centre[0], exit_pos[1] - centre[1])
    cross = v0[0] * v1[1] - v0[1] * v1[0]
    delta = -math.pi / 2.0 if cross < 0 else math.pi / 2.0

    points = []
    for i in range(1, steps + 1):
        phi = delta * (i / steps)
        c, s = math.cos(phi), math.sin(phi)
        points.append((centre[0] + v0[0] * c - v0[1] * s,
                       centre[1] + v0[0] * s + v0[1] * c))
    return points


# ---------------------------------------------------------------------------
# Blender scene construction
# ---------------------------------------------------------------------------

_mesh_cache = {}


def clear_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)


def get_prefab_mesh(name):
    """Import a prefab once and keep only its mesh datablock.

    The importer's object is discarded immediately; every placement links a
    fresh object to the shared mesh. Leaving the source object behind would
    export as a phantom prop sitting at the origin.
    """
    if name in _mesh_cache:
        return _mesh_cache[name]
    obj = kr.import_prefab(bpy.context, name)
    mesh = obj.data
    bpy.data.objects.remove(obj, do_unlink=True)
    _mesh_cache[name] = mesh
    return mesh


def place(name, origin_xz, theta, y=0.0, collection=None):
    """Place a prefab whose *model origin* sits at engine (x, y, z).

    Road tiles use this because their origins were solved from the kit's cell
    geometry. Scenery should use place_centred instead.
    """
    mesh = get_prefab_mesh(name)
    obj = bpy.data.objects.new(name, mesh)
    obj[kr.PROP_PREFAB] = name
    # Engine (x, y, z) -> Blender (x, -z, y); engine yaw == Blender Z rotation.
    obj.location = Vector((origin_xz[0], -origin_xz[1], y))
    obj.rotation_euler.z = theta
    (collection or bpy.context.collection).objects.link(obj)
    return obj


_centre_cache = {}


def prefab_centre(name):
    """Horizontal centre of a prefab's geometry, in its own local space.

    Kit models do not have their origins at the centre of the mesh — Kenney's
    exporter baked a per-model offset into the glTF node — so placing scenery by
    its raw origin scatters it away from where you asked. The offset cannot be
    fixed by re-centring the mesh in Blender, because the engine loads the
    original .glb; it has to be compensated for at placement time.
    """
    if name in _centre_cache:
        return _centre_cache[name]
    mesh = get_prefab_mesh(name)
    xs = [v.co.x for v in mesh.vertices]
    ys = [v.co.y for v in mesh.vertices]
    centre = (((min(xs) + max(xs)) * 0.5), ((min(ys) + max(ys)) * 0.5)) if xs else (0.0, 0.0)
    _centre_cache[name] = centre
    return centre


def place_centred(name, centre_xz, theta, y=0.0, collection=None):
    """Place a prefab so its geometry is centred on engine (x, z)."""
    cx, cy = prefab_centre(name)
    c, s = math.cos(theta), math.sin(theta)
    # Where the local centre lands once the object is rotated about its origin.
    offset_x = cx * c - cy * s
    offset_y = cx * s + cy * c

    mesh = get_prefab_mesh(name)
    obj = bpy.data.objects.new(name, mesh)
    obj[kr.PROP_PREFAB] = name
    obj.location = Vector((centre_xz[0] - offset_x, -centre_xz[1] - offset_y, y))
    obj.rotation_euler.z = theta
    (collection or bpy.context.collection).objects.link(obj)
    return obj


def build_track():
    """Walk the move list, placing road pieces and collecting the centre line."""
    pos = (0.15, 0.0)          # a lane centre line sits on the half-unit lattice
    heading = HEADINGS["N"]
    start_pos, start_heading = pos, heading

    centre_line = [pos]
    straight_cells = []        # (cell_centre, theta) for later start-line swap

    for kind, value in TRACK:
        if kind == "s":
            theta = math.atan2(heading[0], heading[1])
            for _ in range(value):
                cell_centre = (pos[0] + heading[0] * 0.5, pos[1] + heading[1] * 0.5)
                origin = origin_for_block(cell_centre, theta, 1, 1)
                place(STRAIGHT, origin, theta)
                straight_cells.append((cell_centre, theta, heading))
                pos = (pos[0] + heading[0], pos[1] + heading[1])
                centre_line.append(pos)
        else:
            new_heading = HEADINGS[value]
            theta, centre, exit_pos = solve_corner(pos, heading, new_heading, CORNER_CELLS)
            origin = origin_for_block(centre, theta, CORNER_CELLS, CORNER_CELLS)
            place(CORNER, origin, theta)
            centre_line.extend(
                arc_points(pos, heading, exit_pos, new_heading, CORNER_CELLS - 0.5))
            pos, heading = exit_pos, new_heading

    if not near(pos, start_pos) or not near(heading, start_heading):
        raise RuntimeError(f"track does not close: ended at {pos} heading {heading}, "
                           f"expected {start_pos} heading {start_heading}")

    centre_line.pop()   # last point duplicates the first on a closed loop
    return centre_line, straight_cells


def remove_straight_at(cell_centre, theta):
    expected = origin_for_block(cell_centre, theta, 1, 1)
    for obj in list(bpy.context.scene.objects):
        if obj.get(kr.PROP_PREFAB) != STRAIGHT:
            continue
        if abs(obj.location.x - expected[0]) < 1e-3 and abs(obj.location.y + expected[1]) < 1e-3:
            bpy.data.objects.remove(obj, do_unlink=True)
            return True
    return False


def swap_start_line(straight_cells, index=START_LINE_TILE):
    """Replace two straight tiles with the starting-grid markings.

    Sits well into the opening straight so the whole starting grid fits on
    tarmac behind the finish line instead of spilling onto the last corner.
    """
    first, second = straight_cells[index], straight_cells[index + 1]
    for cell_centre, theta, _heading in (first, second):
        if not remove_straight_at(cell_centre, theta):
            raise RuntimeError(f"no straight tile to replace at {cell_centre}")

    # The grid piece is one cell wide and two deep, covering both removed tiles.
    theta = first[1]
    block_centre = ((first[0][0] + second[0][0]) / 2.0,
                    (first[0][1] + second[0][1]) / 2.0)
    place(START_TILE, origin_for_block(block_centre, theta, 1, 2), theta)
    return first[0], first[2]


def distance_to_line(point, centre_line):
    """Shortest distance from `point` to the closed centre line.

    Measured against the segments, not just the vertices: centre-line points are
    a whole unit apart on the straights, so vertex distance alone would
    overstate the clearance of anything sitting beside one.
    """
    best = 1e30
    count = len(centre_line)
    for i in range(count):
        ax, az = centre_line[i]
        bx, bz = centre_line[(i + 1) % count]
        dx, dz = bx - ax, bz - az
        span = dx * dx + dz * dz
        t = 0.0 if span < 1e-12 else (point[0] - ax) * dx + (point[1] - az) * dz
        t = max(0.0, min(1.0, t / span if span > 1e-12 else 0.0))
        ox, oz = point[0] - (ax + dx * t), point[1] - (az + dz * t)
        best = min(best, ox * ox + oz * oz)
    return math.sqrt(best)


def scatter_scenery(centre_line, rng):
    """Dress the circuit: barriers hugging the track, scenery further out."""
    xs = [p[0] for p in centre_line]
    zs = [p[1] for p in centre_line]
    lo = (min(xs) - 8.0, min(zs) - 8.0)
    hi = (max(xs) + 8.0, max(zs) + 8.0)

    # Barriers and pylons just off the racing surface, aligned to the track.
    count = len(centre_line)
    for i in range(0, count, 3):
        p = centre_line[i]
        nxt = centre_line[(i + 1) % count]
        tangent = (nxt[0] - p[0], nxt[1] - p[1])
        length = math.hypot(*tangent) or 1.0
        tangent = (tangent[0] / length, tangent[1] / length)
        left = (-tangent[1], tangent[0])
        theta = math.atan2(tangent[0], tangent[1])

        for side in (-1, 1):
            if rng.random() < 0.45:
                continue
            # Far enough out that there is a strip of grass to run onto before
            # anything solid is hit; the lane is only 0.69 wide.
            offset = 0.78 + rng.uniform(0.0, 0.14)
            spot = (p[0] + left[0] * offset * side, p[1] + left[1] * offset * side)

            # Offsetting perpendicular to the local segment is not enough near a
            # corner, where the arc curves back towards the barrier and can leave
            # it sitting on the racing surface. Check against the whole loop.
            if distance_to_line(spot, centre_line) < BARRIER_CLEARANCE:
                continue

            model = "barrierRed" if (i // 3) % 2 == 0 else "barrierWhite"
            place_centred(model, spot, theta + math.pi / 2)

    # Trees, tents and grandstands scattered clear of the track.
    scenery = [
        ("treeLarge", 26, 2.0), ("treeSmall", 22, 1.8), ("tent", 6, 3.0),
        ("tentLong", 4, 3.2), ("grandStand", 5, 3.4), ("grandStandCovered", 3, 3.6),
        ("lightPostLarge", 10, 1.6), ("pylon", 18, 1.1),
    ]
    for model, amount, clearance in scenery:
        placed = 0
        for _ in range(amount * 60):
            if placed >= amount:
                break
            spot = (rng.uniform(lo[0], hi[0]), rng.uniform(lo[1], hi[1]))
            if distance_to_line(spot, centre_line) < clearance:
                continue
            place_centred(model, spot, rng.uniform(0.0, math.tau))
            placed += 1


def add_start_dressing(start_cell, heading, centre_line):
    """Checkered flags and a banner tower framing the finish line."""
    theta = math.atan2(heading[0], heading[1])
    left = (-heading[1], heading[0])
    for side, model in ((-1, "flagCheckers"), (1, "flagCheckers")):
        spot = (start_cell[0] + left[0] * 0.75 * side, start_cell[1] + left[1] * 0.75 * side)
        place_centred(model, spot, theta)
    place_centred("bannerTowerGreen",
                  (start_cell[0] + left[0] * 1.5, start_cell[1] + left[1] * 1.5), theta)
    place_centred("grandStandCoveredRound",
          (start_cell[0] - left[0] * 2.4, start_cell[1] - left[1] * 2.4), theta + math.pi)


def add_racing_line(centre_line, width):
    curve = bpy.data.curves.new("RacingLine", type="CURVE")
    curve.dimensions = "3D"
    spline = curve.splines.new("POLY")
    spline.points.add(len(centre_line) - 1)
    for point, (x, z) in zip(spline.points, centre_line):
        point.co = (x, -z, 0.02, 1.0)
    spline.use_cyclic_u = True

    obj = bpy.data.objects.new("RacingLine", curve)
    obj[kr.PROP_TYPE] = "racingline"
    obj[kr.PROP_WIDTH] = width
    bpy.context.collection.objects.link(obj)
    return obj


def add_spawns(centre_line, start_index, count=6):
    """Stagger the grid slots back down the straight from the finish line."""
    n = len(centre_line)
    for i in range(count):
        idx = (start_index - 2 - i) % n
        p = centre_line[idx]
        nxt = centre_line[(idx + 1) % n]
        tangent = (nxt[0] - p[0], nxt[1] - p[1])
        length = math.hypot(*tangent) or 1.0
        tangent = (tangent[0] / length, tangent[1] / length)
        left = (-tangent[1], tangent[0])
        side = 0.16 if i % 2 == 0 else -0.16

        empty = bpy.data.objects.new(f"Spawn.{i:02d}", None)
        empty.empty_display_type = "SINGLE_ARROW"
        empty.empty_display_size = 0.4
        spot = (p[0] + left[0] * side, p[1] + left[1] * side)
        empty.location = Vector((spot[0], -spot[1], 0.0))
        empty.rotation_euler.z = math.atan2(tangent[0], tangent[1])
        empty[kr.PROP_TYPE] = "spawn"
        empty[kr.PROP_INDEX] = i
        bpy.context.collection.objects.link(empty)


def add_checkpoints(centre_line, start_index, width, count=12):
    """Space the gates evenly by distance travelled, not by point index.

    Corners are sampled far more densely than straights, so index-based spacing
    would crowd nearly half the gates onto the arcs.
    """
    n = len(centre_line)
    order = [(start_index + k) % n for k in range(n)]
    cumulative, total = [0.0], 0.0
    for k in range(n):
        a, b = centre_line[order[k]], centre_line[order[(k + 1) % n]]
        total += math.dist(a, b)
        cumulative.append(total)

    for i in range(count):
        target = total * i / count
        k = max(j for j in range(n) if cumulative[j] <= target)
        idx = order[k]
        p = centre_line[idx]
        nxt = centre_line[(idx + 1) % n]
        tangent = (nxt[0] - p[0], nxt[1] - p[1])
        length = math.hypot(*tangent) or 1.0
        tangent = (tangent[0] / length, tangent[1] / length)

        empty = bpy.data.objects.new(f"Checkpoint.{i:02d}", None)
        empty.empty_display_type = "ARROWS"
        empty.empty_display_size = 0.5
        empty.location = Vector((p[0], -p[1], 0.0))
        empty.rotation_euler.z = math.atan2(tangent[0], tangent[1])
        empty[kr.PROP_TYPE] = "checkpoint"
        empty[kr.PROP_INDEX] = i
        empty[kr.PROP_WIDTH] = width * 1.6
        bpy.context.collection.objects.link(empty)


def main():
    rng = random.Random(20260805)

    clear_scene()
    kr.register()
    kr.KIT_PATH_OVERRIDE = KIT

    settings = bpy.context.scene.kr_level
    settings.level_name = "Kenney Circuit 01"
    settings.laps = 3
    settings.track_width = 0.69
    settings.waypoint_spacing = 0.45
    settings.auto_colliders = True

    centre_line, straight_cells = build_track()
    print(f"[track] {len(centre_line)} centre-line points, {len(straight_cells)} straight tiles")

    start_cell, heading = swap_start_line(straight_cells)

    # The finish line is where the starting-grid tile sits.
    start_index = min(range(len(centre_line)),
                      key=lambda i: (centre_line[i][0] - start_cell[0]) ** 2 +
                                    (centre_line[i][1] - start_cell[1]) ** 2)

    add_start_dressing(start_cell, heading, centre_line)
    scatter_scenery(centre_line, rng)
    add_racing_line(centre_line, settings.track_width)
    add_spawns(centre_line, start_index)
    add_checkpoints(centre_line, start_index, settings.track_width)

    os.makedirs(os.path.dirname(OUT_JSON), exist_ok=True)
    data = kr.write_level(bpy.context, OUT_JSON)
    print(f"[export] {OUT_JSON}")
    print(f"[export] props={len(data['props'])} colliders={len(data['colliders'])} "
          f"spawns={len(data['spawns'])} waypoints={len(data['waypoints'])} "
          f"checkpoints={len(data['checkpoints'])}")

    bpy.ops.wm.save_as_mainfile(filepath=OUT_BLEND)
    print(f"[export] {OUT_BLEND}")


if __name__ == "__main__":
    main()
