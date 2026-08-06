"""Build the demo circuit in Blender and export it for the engine.

Run headless:

    blender --background --python tools/blender/build_demo_track.py

Produces ``levels/circuit01.level.json`` (what the game loads) and
``levels/circuit01.blend`` (open this to edit the track by hand).

The track is a list of moves along the kit's tile grid. Every kit road port sits
at the centre of a one-unit cell edge and corners are exact quarter arcs of
radius ``N - 0.5``, so tiles chain together without gaps and the racing line can
be derived from the same walk that places the art.

Straights also carry a rise, which pitches their tiles and lifts the centre
line. Corners stay level: a quarter arc cannot be pitched about a single axis
without twisting, so the layout puts its gradients on the straights and crests
before turning in — which is how real circuits tend to read anyway.

The layout is Spa-inspired rather than a replica: the kit only has 90-degree
corners, so what carries over is the rhythm — a hairpin off the start, a plunge
into a compression, a long climb, a fast straight at the top, chicanes, and big
sweepers on the way back down.
"""

import math
import os
import random
import sys

import bpy
from mathutils import Matrix, Vector

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, "tools", "blender"))

import io_kenney_racing as kr  # noqa: E402

KIT = os.path.join(REPO, "assets", "models")
OUT_JSON = os.path.join(REPO, "levels", "circuit01.level.json")
OUT_BLEND = os.path.join(REPO, "levels", "circuit01.blend")

STRAIGHT = "roadStraight"
START_TILE = "roadStartPositions"
CORNER_BY_CELLS = {1: "roadCornerSmall", 2: "roadCornerLarge", 3: "roadCornerLarger"}

# Which tile of the opening straight carries the starting grid. Far enough in
# that the six staggered grid slots all land on the straight.
START_LINE_TILE = 8

# Moves around the loop:
#   ("s", tiles, rise, label)          straight; tiles may be None to be solved
#   ("t", heading, cells, rise, label) corner into a new heading
#
# Exactly two straights must be left as None, on perpendicular headings; their
# lengths are solved so the loop closes. Any leftover rise is spread across the
# straights afterwards so the elevation closes too.
TRACK = [
    ("s", 13, 0.00, "Start/finish straight"),
    ("t", "E", 2, 0.0, "La Source (entry)"),
    ("s", 1, 0.00, ""),
    ("t", "S", 2, 0.0, "La Source (exit)"),
    ("s", 7, -1.25, "Plunge towards the compression"),
    ("t", "E", 2, 0.0, "Eau Rouge (left)"),
    ("s", 2, -0.25, "Compression"),
    ("t", "S", 2, 0.0, "Eau Rouge (right)"),
    ("s", 10, 1.70, "Raidillon climb"),
    ("t", "W", 2, 0.0, "Onto the top straight"),
    ("s", 14, 0.55, "Kemmel straight"),
    ("t", "S", 2, 0.0, "Les Combes (right)"),
    ("s", 1, 0.00, ""),
    ("t", "W", 2, 0.0, "Les Combes (left)"),
    ("s", 3, -0.45, "Descent to Pouhon"),
    ("t", "N", 3, 0.0, "Pouhon (long sweeper)"),
    ("s", 4, -0.30, "Towards Stavelot"),
    ("t", "E", 2, 0.0, "Stavelot"),
    ("s", None, 0.00, "Blanchimont"),
    ("t", "N", 2, 0.0, "Bus stop (left)"),
    ("s", None, 0.00, "Run to the line"),
]

HEADINGS = {"N": (0.0, 1.0), "E": (1.0, 0.0), "S": (0.0, -1.0), "W": (-1.0, 0.0)}

# Minimum distance from the centre line to anything solid: half the lane (0.345)
# plus the barrier's own reach plus a margin of grass to spin out on.
BARRIER_CLEARANCE = 0.68

# Matches the engine's terrain weighting so scenery sits on the same ground the
# renderer builds. See HeightFromSpline in engine/src/terrain.c.
TERRAIN_FALLOFF = 0.45


# ---------------------------------------------------------------------------
# Engine-space helpers. Positions are (x, z) with height carried separately.
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


def arc_points(entry, heading, exit_pos, new_heading, radius, steps=12):
    """Sample the quarter arc a corner piece describes.

    The centre of a turn sits perpendicular to the direction of travel, never
    along it. Getting that wrong still produces an arc through both ports — the
    mirror image of the right one — but its tangents meet the straights at right
    angles, putting a 90-degree kink in the racing line at every corner.
    """
    perp = (-heading[1], heading[0])
    centre = None
    for sign in (1.0, -1.0):
        candidate = (entry[0] + perp[0] * radius * sign, entry[1] + perp[1] * radius * sign)
        if abs(math.dist(candidate, exit_pos) - radius) < 1e-4:
            centre = candidate
            break
    if centre is None:
        raise RuntimeError(f"no arc of radius {radius} joins {entry} to {exit_pos}")

    v0 = (entry[0] - centre[0], entry[1] - centre[1])
    v1 = (exit_pos[0] - centre[0], exit_pos[1] - centre[1])
    cross = v0[0] * v1[1] - v0[1] * v1[0]
    delta = math.pi / 2.0 if cross > 0 else -math.pi / 2.0

    points = []
    for i in range(1, steps + 1):
        phi = delta * (i / steps)
        c, s = math.cos(phi), math.sin(phi)
        points.append((centre[0] + v0[0] * c - v0[1] * s,
                       centre[1] + v0[0] * s + v0[1] * c))
    return points


# ---------------------------------------------------------------------------
# Closing the loop
# ---------------------------------------------------------------------------

def solve_track(moves):
    """Fill in the two unspecified straight lengths so the walk closes.

    Walks the moves with the unknowns at zero, then cancels the leftover
    displacement using the two adjustable straights. They must lie on
    perpendicular headings for the residual to be separable.
    """
    unknowns = [i for i, m in enumerate(moves) if m[0] == "s" and m[1] is None]
    if len(unknowns) != 2:
        raise RuntimeError(f"expected exactly 2 adjustable straights, found {len(unknowns)}")

    pos = (0.0, 0.0)
    heading = HEADINGS["N"]
    turns = 0
    directions = {}

    for index, move in enumerate(moves):
        if move[0] == "s":
            tiles = move[1] or 0
            directions[index] = heading
            pos = (pos[0] + heading[0] * tiles, pos[1] + heading[1] * tiles)
        else:
            _kind, name, cells, _rise, _label = move
            new_heading = HEADINGS[name]
            radius = cells - 0.5
            pos = (pos[0] + (heading[0] + new_heading[0]) * radius,
                   pos[1] + (heading[1] + new_heading[1]) * radius)
            # +1 for a clockwise quarter turn, -1 for anticlockwise.
            cross = heading[0] * new_heading[1] - heading[1] * new_heading[0]
            turns += -1 if cross > 0 else 1
            heading = new_heading

    if heading != HEADINGS["N"]:
        raise RuntimeError(f"track ends heading {heading}, expected north")
    if abs(turns) != 4:
        raise RuntimeError(f"net rotation is {turns * 90} degrees, a simple loop needs +/-360")

    a, b = unknowns
    dir_a, dir_b = directions[a], directions[b]
    if abs(dir_a[0] * dir_b[0] + dir_a[1] * dir_b[1]) > 1e-6:
        raise RuntimeError("the two adjustable straights must be on perpendicular headings")

    # Each adjustable contributes length * heading, so project the residual.
    residual = (-pos[0], -pos[1])
    length_a = residual[0] * dir_a[0] + residual[1] * dir_a[1]
    length_b = residual[0] * dir_b[0] + residual[1] * dir_b[1]

    solved = list(moves)
    for index, length in ((a, length_a), (b, length_b)):
        tiles = int(round(length))
        if abs(length - tiles) > 1e-6:
            raise RuntimeError(f"adjustable straight {index} solved to {length:.3f}, not a whole "
                               f"number of tiles")
        if tiles < 1:
            raise RuntimeError(f"adjustable straight {index} solved to {tiles} tiles; the layout "
                               f"does not leave room to close")
        move = solved[index]
        solved[index] = (move[0], tiles, move[2], move[3])
        print(f"[solve] straight {index} ({move[3] or 'unnamed'}) -> {tiles} tiles")

    return solved


def balance_elevation(moves):
    """Spread any leftover height across the straights so the loop closes."""
    total_rise = sum(m[2] if m[0] == "s" else m[3] for m in moves)
    tiles = sum(m[1] for m in moves if m[0] == "s")
    if abs(total_rise) < 1e-9 or tiles == 0:
        return moves

    per_tile = total_rise / tiles
    print(f"[solve] elevation was {total_rise:+.3f} over the lap; "
          f"trimming {per_tile:+.4f} per tile to close it")
    balanced = []
    for move in moves:
        if move[0] == "s":
            balanced.append((move[0], move[1], move[2] - per_tile * move[1], move[3]))
        else:
            balanced.append(move)
    return balanced


# ---------------------------------------------------------------------------
# Blender scene construction
# ---------------------------------------------------------------------------

_mesh_cache = {}
_centre_cache = {}


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


def _link(name, mesh, location, yaw):
    """Create an object at an engine-space location with a yaw about the up axis."""
    obj = bpy.data.objects.new(name, mesh)
    obj[kr.PROP_PREFAB] = name
    obj.matrix_world = Matrix.Translation(location) @ Matrix.Rotation(yaw, 4, "Z")
    bpy.context.collection.objects.link(obj)
    return obj


def place(name, origin_xz, theta, y=0.0):
    """Place a prefab whose *model origin* sits at engine (x, y, z)."""
    mesh = get_prefab_mesh(name)
    # Engine (x, y, z) -> Blender (x, -z, y); engine yaw == Blender Z rotation.
    return _link(name, mesh, Vector((origin_xz[0], -origin_xz[1], y)), theta)


def place_tile(name, cell_centre, theta, width, depth, y=0.0, pitch=0.0):
    """Place a road tile so its cell lands on `cell_centre`, pitched to a slope.

    Everything happens about the cell centre rather than the model origin, which
    the kit puts at a corner. Rotating or scaling about that corner would slide
    the tile as well as tilt it, and adjacent tiles would stop meeting.

    A tile pitched by theta only reaches cos(theta) as far horizontally, so it is
    also stretched by 1/cos(theta) to keep spanning exactly one grid cell.
    """
    mesh = get_prefab_mesh(name)
    stretch = 1.0 / math.cos(pitch) if abs(pitch) > 1e-9 else 1.0

    # Cell centre in the tile's own space: the kit puts a cell's minimum corner
    # a constant (-0.35, +0.65) from the origin, in Blender XY.
    local_centre = Vector((-0.35 + width / 2.0, 0.65 + depth / 2.0, 0.0))

    obj = bpy.data.objects.new(name, mesh)
    obj[kr.PROP_PREFAB] = name
    obj.matrix_world = (Matrix.Translation(Vector((cell_centre[0], -cell_centre[1], y))) @
                        Matrix.Rotation(theta, 4, "Z") @
                        Matrix.Rotation(pitch, 4, "X") @
                        Matrix.Diagonal((1.0, stretch, 1.0, 1.0)) @
                        Matrix.Translation(-local_centre))
    bpy.context.collection.objects.link(obj)
    return obj


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


def place_centred(name, centre_xz, theta, y=0.0):
    """Place a prefab so its geometry is centred on engine (x, z)."""
    cx, cy = prefab_centre(name)
    c, s = math.cos(theta), math.sin(theta)
    offset_x = cx * c - cy * s
    offset_y = cx * s + cy * c

    mesh = get_prefab_mesh(name)
    location = Vector((centre_xz[0] - offset_x, -centre_xz[1] - offset_y, y))
    return _link(name, mesh, location, theta)


# ---------------------------------------------------------------------------
# The track itself
# ---------------------------------------------------------------------------

def build_track(moves):
    """Walk the move list, placing road pieces and collecting the centre line.

    Returns (centre_line, straight_cells) where centre_line holds (x, z, height)
    triples in engine space.
    """
    pos = (0.15, 0.0)          # a lane centre line sits on the half-unit lattice
    height = 0.0
    heading = HEADINGS["N"]
    start_pos, start_heading, start_height = pos, heading, height

    centre_line = [(pos[0], pos[1], height)]
    straight_cells = []        # (cell_centre, theta, heading, height, pitch)
    tiles_placed = []          # (cell_centre, theta, width, depth) for validation

    for move in moves:
        if move[0] == "s":
            _kind, tiles, rise, _label = move
            theta = math.atan2(heading[0], heading[1])
            step = rise / tiles if tiles else 0.0
            # A tile's length runs along its local +Y, but travel goes the other
            # way (engine +Z is Blender -Y), so the pitch that makes the leading
            # edge drop on a descent is the negative of the gradient's angle.
            pitch = -math.atan2(step, 1.0)

            for _ in range(tiles):
                cell_centre = (pos[0] + heading[0] * 0.5, pos[1] + heading[1] * 0.5)
                mid_height = height + step * 0.5
                obj = place_tile(STRAIGHT, cell_centre, theta, 1, 1,
                                 y=mid_height, pitch=pitch)
                straight_cells.append((cell_centre, theta, heading, mid_height, pitch, obj))
                tiles_placed.append((cell_centre, theta, 1, 1))

                pos = (pos[0] + heading[0], pos[1] + heading[1])
                height += step
                centre_line.append((pos[0], pos[1], height))
        else:
            _kind, name, cells, rise, _label = move
            new_heading = HEADINGS[name]
            theta, centre, exit_pos = solve_corner(pos, heading, new_heading, cells)
            place_tile(CORNER_BY_CELLS[cells], centre, theta, cells, cells, y=height)
            tiles_placed.append((centre, theta, cells, cells))

            for point in arc_points(pos, heading, exit_pos, new_heading, cells - 0.5):
                height += rise / 10.0
                centre_line.append((point[0], point[1], height))
            pos, heading = exit_pos, new_heading

    if not near(pos, start_pos) or not near(heading, start_heading):
        raise RuntimeError(f"track does not close: ended at {pos} heading {heading}, "
                           f"expected {start_pos} heading {start_heading}")
    if abs(height - start_height) > 1e-4:
        raise RuntimeError(f"elevation does not close: ended at {height:+.4f}")

    centre_line.pop()   # last point duplicates the first on a closed loop
    verify_line_on_tiles(centre_line, tiles_placed)
    verify_tile_heights(straight_cells, centre_line)
    return centre_line, straight_cells


def verify_tile_heights(straight_cells, centre_line):
    """Assert each straight tile's surface matches the centre line beneath it.

    Pitching a tile the wrong way still leaves it centred on its cell, spanning
    exactly one unit, at the right average height — every cheap check passes —
    while its two ends are swapped, so consecutive tiles step past each other.
    Comparing a tile's leading and trailing edge against the line catches it.
    """
    worst = 0.0
    for cell_centre, theta, heading, mid_height, pitch, obj in straight_cells:
        for end in (-0.5, 0.5):
            spot = (cell_centre[0] + heading[0] * end, cell_centre[1] + heading[1] * end)
            # Height the tile puts that edge at: forward is the tile's local -Y.
            tile_y = mid_height - math.tan(pitch) * end
            want = min(((abs(spot[0] - x) + abs(spot[1] - z), y) for x, z, y in centre_line))[1]
            worst = max(worst, abs(tile_y - want))
    if worst > 0.02:
        raise RuntimeError(f"road tiles disagree with the centre line by up to {worst:.3f} "
                           f"units — check the pitch direction")


def verify_line_on_tiles(centre_line, tiles_placed):
    """Assert every centre-line point lies on a placed road tile.

    The art and the racing line come from the same walk, so they should agree by
    construction — but only if the arc geometry is right. An arc bulging the
    wrong way still joins the same two ports, so it passes every closure check
    while leaving the racing line off the tarmac. This is the check that notices.
    """
    stray = []
    for x, z, _y in centre_line:
        for (cx, cz), theta, width, depth in tiles_placed:
            lx, lz = rot_engine((x - cx, z - cz), -theta)
            if abs(lx) <= width / 2.0 + 1e-6 and abs(lz) <= depth / 2.0 + 1e-6:
                break
        else:
            stray.append((round(x, 2), round(z, 2)))
    if stray:
        raise RuntimeError(f"{len(stray)} centre-line point(s) are not on any road tile, "
                           f"first few: {stray[:5]}")


def swap_start_line(straight_cells, index=START_LINE_TILE):
    """Replace two straight tiles with the starting-grid markings."""
    first, second = straight_cells[index], straight_cells[index + 1]
    for entry in (first, second):
        bpy.data.objects.remove(entry[5], do_unlink=True)

    theta = first[1]
    block_centre = ((first[0][0] + second[0][0]) / 2.0,
                    (first[0][1] + second[0][1]) / 2.0)
    height = (first[3] + second[3]) / 2.0
    pitch = (first[4] + second[4]) / 2.0
    place_tile(START_TILE, block_centre, theta, 1, 2, y=height, pitch=pitch)
    return first[0], first[2], first[3]


# ---------------------------------------------------------------------------
# Ground height and scenery
# ---------------------------------------------------------------------------

def ground_height(point, centre_line):
    """Inverse-distance blend of the centre line's height.

    Deliberately the same weighting the engine's terrain uses, so a tree placed
    here lands on the ground the renderer actually builds.
    """
    weighted = 0.0
    total = 0.0
    for cx, cz, cy in centre_line:
        dx = point[0] - cx
        dz = point[1] - cz
        d2 = dx * dx + dz * dz
        w = 1.0 / (d2 * d2 + TERRAIN_FALLOFF)
        weighted += cy * w
        total += w
    return weighted / total if total else 0.0


def distance_to_line(point, centre_line):
    """Shortest horizontal distance from `point` to the closed centre line."""
    best = 1e30
    count = len(centre_line)
    for i in range(count):
        ax, az = centre_line[i][0], centre_line[i][1]
        bx, bz = centre_line[(i + 1) % count][0], centre_line[(i + 1) % count][1]
        dx, dz = bx - ax, bz - az
        span = dx * dx + dz * dz
        t = 0.0 if span < 1e-12 else (point[0] - ax) * dx + (point[1] - az) * dz
        t = max(0.0, min(1.0, t / span if span > 1e-12 else 0.0))
        ox, oz = point[0] - (ax + dx * t), point[1] - (az + dz * t)
        best = min(best, ox * ox + oz * oz)
    return math.sqrt(best)


def tangent_at(centre_line, i):
    count = len(centre_line)
    a, b = centre_line[i], centre_line[(i + 1) % count]
    t = (b[0] - a[0], b[1] - a[1])
    length = math.hypot(*t) or 1.0
    return (t[0] / length, t[1] / length)


def scatter_scenery(centre_line, rng):
    """Dress the circuit: barriers hugging the track, scenery further out."""
    xs = [p[0] for p in centre_line]
    zs = [p[1] for p in centre_line]
    lo = (min(xs) - 8.0, min(zs) - 8.0)
    hi = (max(xs) + 8.0, max(zs) + 8.0)

    count = len(centre_line)
    for i in range(0, count, 3):
        p = centre_line[i]
        tangent = tangent_at(centre_line, i)
        left = (-tangent[1], tangent[0])
        theta = math.atan2(tangent[0], tangent[1])

        for side in (-1, 1):
            if rng.random() < 0.45:
                continue
            offset = 0.78 + rng.uniform(0.0, 0.14)
            spot = (p[0] + left[0] * offset * side, p[1] + left[1] * offset * side)
            if distance_to_line(spot, centre_line) < BARRIER_CLEARANCE:
                continue
            model = "barrierRed" if (i // 3) % 2 == 0 else "barrierWhite"
            place_centred(model, spot, theta + math.pi / 2, y=p[2])

    scenery = [
        ("treeLarge", 30, 2.0), ("treeSmall", 26, 1.8), ("tent", 6, 3.0),
        ("tentLong", 4, 3.2), ("grandStand", 6, 3.4), ("grandStandCovered", 4, 3.6),
        ("pylon", 20, 1.1),
    ]
    for model, amount, clearance in scenery:
        placed = 0
        for _ in range(amount * 60):
            if placed >= amount:
                break
            spot = (rng.uniform(lo[0], hi[0]), rng.uniform(lo[1], hi[1]))
            if distance_to_line(spot, centre_line) < clearance:
                continue
            place_centred(model, spot, rng.uniform(0.0, math.tau),
                          y=ground_height(spot, centre_line))
            placed += 1


def place_lamp(centre_xz, height, energy, reach, colour=(1.0, 0.86, 0.62),
               kind="POINT", direction=None, cone=(70.0, 0.35), name="Lamp"):
    """Create a Blender lamp at an engine-space position.

    Blender lamps export straight through the add-on, so the demo track is lit
    with exactly the same data path an artist would use by hand.
    """
    data = bpy.data.lights.new(name=name, type=kind)
    data.energy = energy
    data.color = colour
    data.use_custom_distance = True
    data.cutoff_distance = reach
    if kind == "SPOT":
        data.spot_size = math.radians(cone[0])
        data.spot_blend = cone[1]

    obj = bpy.data.objects.new(name, data)
    obj.location = Vector((centre_xz[0], -centre_xz[1], height))
    if kind == "SPOT" and direction is not None:
        beam = Vector((direction[0], -direction[2], direction[1])).normalized()
        obj.rotation_euler = beam.to_track_quat("-Z", "Y").to_euler()
    bpy.context.collection.objects.link(obj)
    return obj


def place_light_posts(centre_line, rng, spacing=13, offset=1.05):
    """Line the circuit with lamp posts, each carrying a real point light."""
    count = len(centre_line)
    placed = 0
    for i in range(0, count, spacing):
        p = centre_line[i]
        tangent = tangent_at(centre_line, i)
        left = (-tangent[1], tangent[0])
        side = 1 if (placed % 2 == 0) else -1

        spot = (p[0] + left[0] * offset * side, p[1] + left[1] * offset * side)
        if distance_to_line(spot, centre_line) < BARRIER_CLEARANCE + 0.2:
            continue

        base = ground_height(spot, centre_line)
        theta = math.atan2(-left[0] * side, -left[1] * side)
        place_centred("lightPostLarge", spot, theta, y=base)

        head = (spot[0] - left[0] * 0.18 * side, spot[1] - left[1] * 0.18 * side)
        place_lamp(head, base + 0.72, energy=210.0, reach=3.0, name=f"LampPost.{placed:02d}")
        placed += 1
    return placed


def place_beside_track(model, anchor, left, offset, theta, centre_line, clearance):
    """Place scenery to one side of the track, flipping sides if that side is busy.

    Circuits double back on themselves — on this one the plunge runs two units
    from the pit straight — so an offset that looks clear of your own bit of
    track can land squarely on another. Try the far side, then give up.
    """
    for sign in (1.0, -1.0):
        spot = (anchor[0] + left[0] * offset * sign, anchor[1] + left[1] * offset * sign)
        if distance_to_line(spot, centre_line) >= clearance:
            place_centred(model, spot, theta if sign > 0 else theta + math.pi,
                          y=ground_height(spot, centre_line))
            return spot
    print(f"[warn] no room beside the track for {model}, skipped")
    return None


def add_start_dressing(start_cell, heading, height, centre_line):
    """Checkered flags and a banner tower framing the finish line."""
    theta = math.atan2(heading[0], heading[1])
    left = (-heading[1], heading[0])
    for side in (-1, 1):
        spot = (start_cell[0] + left[0] * 0.75 * side, start_cell[1] + left[1] * 0.75 * side)
        if distance_to_line(spot, centre_line) >= BARRIER_CLEARANCE:
            place_centred("flagCheckers", spot, theta, y=height)

    place_beside_track("bannerTowerGreen", start_cell, left, 1.5, theta, centre_line, 1.1)
    place_beside_track("grandStandCoveredRound", start_cell,
                       (-left[0], -left[1]), 2.4, theta + math.pi, centre_line, 1.9)

    for side in (-1, 1):
        spot = (start_cell[0] + left[0] * 1.7 * side, start_cell[1] + left[1] * 1.7 * side)
        aim = (-left[0] * side, -1.5, -left[1] * side)
        place_lamp(spot, height + 2.3, energy=340.0, reach=5.2, colour=(1.0, 0.95, 0.85),
                   kind="SPOT", direction=aim, cone=(86.0, 0.45),
                   name=f"StartFlood.{'L' if side < 0 else 'R'}")


def add_racing_line(centre_line, width):
    curve = bpy.data.curves.new("RacingLine", type="CURVE")
    curve.dimensions = "3D"
    spline = curve.splines.new("POLY")
    spline.points.add(len(centre_line) - 1)
    for point, (x, z, y) in zip(spline.points, centre_line):
        point.co = (x, -z, y + 0.02, 1.0)
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
        tangent = tangent_at(centre_line, idx)
        left = (-tangent[1], tangent[0])
        side = 0.16 if i % 2 == 0 else -0.16

        empty = bpy.data.objects.new(f"Spawn.{i:02d}", None)
        empty.empty_display_type = "SINGLE_ARROW"
        empty.empty_display_size = 0.4
        spot = (p[0] + left[0] * side, p[1] + left[1] * side)
        empty.location = Vector((spot[0], -spot[1], p[2]))
        empty.rotation_euler.z = math.atan2(tangent[0], tangent[1])
        empty[kr.PROP_TYPE] = "spawn"
        empty[kr.PROP_INDEX] = i
        bpy.context.collection.objects.link(empty)


def add_checkpoints(centre_line, start_index, width, count=12):
    """Space the gates evenly by distance travelled, not by point index."""
    n = len(centre_line)
    order = [(start_index + k) % n for k in range(n)]
    cumulative, total = [0.0], 0.0
    for k in range(n):
        a, b = centre_line[order[k]], centre_line[order[(k + 1) % n]]
        total += math.dist((a[0], a[1]), (b[0], b[1]))
        cumulative.append(total)

    for i in range(count):
        target = total * i / count
        k = max(j for j in range(n) if cumulative[j] <= target)
        idx = order[k]
        p = centre_line[idx]
        tangent = tangent_at(centre_line, idx)

        empty = bpy.data.objects.new(f"Checkpoint.{i:02d}", None)
        empty.empty_display_type = "ARROWS"
        empty.empty_display_size = 0.5
        empty.location = Vector((p[0], -p[1], p[2]))
        empty.rotation_euler.z = math.atan2(tangent[0], tangent[1])
        empty[kr.PROP_TYPE] = "checkpoint"
        empty[kr.PROP_INDEX] = i
        empty[kr.PROP_WIDTH] = width * 1.6
        bpy.context.collection.objects.link(empty)


def main():
    rng = random.Random(20260806)

    clear_scene()
    kr.register()
    kr.KIT_PATH_OVERRIDE = KIT

    settings = bpy.context.scene.kr_level
    settings.level_name = "Ardennes Circuit"
    settings.laps = 3
    settings.track_width = 0.69
    settings.waypoint_spacing = 0.45
    settings.auto_colliders = True
    settings.sun_intensity = 0.62
    settings.ambient_color = (0.345, 0.353, 0.392)

    moves = balance_elevation(solve_track(TRACK))
    centre_line, straight_cells = build_track(moves)

    heights = [p[2] for p in centre_line]
    print(f"[track] {len(centre_line)} centre-line points, {len(straight_cells)} straight tiles")
    print(f"[track] elevation {min(heights):+.2f} .. {max(heights):+.2f} "
          f"(range {max(heights) - min(heights):.2f} units)")

    start_cell, heading, start_height = swap_start_line(straight_cells)
    start_index = min(range(len(centre_line)),
                      key=lambda i: (centre_line[i][0] - start_cell[0]) ** 2 +
                                    (centre_line[i][1] - start_cell[1]) ** 2)

    add_start_dressing(start_cell, heading, start_height, centre_line)
    lamps = place_light_posts(centre_line, rng)
    print(f"[track] {lamps} lamp posts")
    scatter_scenery(centre_line, rng)
    add_racing_line(centre_line, settings.track_width)
    add_spawns(centre_line, start_index)
    add_checkpoints(centre_line, start_index, settings.track_width)

    os.makedirs(os.path.dirname(OUT_JSON), exist_ok=True)
    data = kr.write_level(bpy.context, OUT_JSON)
    print(f"[export] {OUT_JSON}")
    print(f"[export] props={len(data['props'])} colliders={len(data['colliders'])} "
          f"spawns={len(data['spawns'])} waypoints={len(data['waypoints'])} "
          f"checkpoints={len(data['checkpoints'])} lights={len(data['lights'])}")

    bpy.ops.wm.save_as_mainfile(filepath=OUT_BLEND)
    print(f"[export] {OUT_BLEND}")


if __name__ == "__main__":
    main()
