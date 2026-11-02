"""Hand-tuned redraws for a few small sprites that no upscaler can rebuild well.

Everything here is drawn procedurally on the player's PC from their own PINBALL.DAT; nothing from the
original art is stored in this repository.
"""
import numpy as np
from PIL import Image, ImageDraw, ImageFilter

SS = 4  # supersampling on top of the 4x HD scale, for smooth edges


# --- Arrows -------------------------------------------------------------------------------------

# Lit arrow lights whose rounded, blobby heads get redrawn as clean, pointed arrows.
ARROW_LIGHTS = {"lite317", "lite319"}
ARROW_LIT_LEVEL = 100  # brightness that separates the lit arrow from the table behind it


def _arrow_polygon(rgba):
    """Fits a pointed arrow (tip, head, shaft, tail) to the lit pixels of a small arrow sprite.
    Returns (polygon points in 1x pixel coordinates, (ridge point, axis, side)), or None if the
    sprite doesn't look like an arrow."""
    lum = rgba[..., :3].astype(np.float32).mean(-1)
    ys, xs = np.nonzero((lum > ARROW_LIT_LEVEL) & (rgba[..., 3] > 0))
    if len(xs) < 12:
        return None
    pts = np.stack([xs + 0.5, ys + 0.5], 1)
    centre = pts.mean(0)
    _, vecs = np.linalg.eigh(np.cov((pts - centre).T))
    axis = vecs[:, 1]
    side = np.array([-axis[1], axis[0]])
    u, v = (pts - centre) @ axis, (pts - centre) @ side
    # Point the axis from the head (the wide end) to the tail.
    if np.ptp(v[u > np.percentile(u, 70)]) > np.ptp(v[u < np.percentile(u, 30)]):
        axis, side, u, v = -axis, -side, -u, -v

    bins = np.arange(np.floor(u.min()), np.ceil(u.max()) + 1)
    widths = []
    for b in bins:
        sel = (u >= b) & (u < b + 1)
        widths.append((v[sel].min(), v[sel].max()) if sel.any() else (0.0, 0.0))
    spans = np.array([hi - lo for lo, hi in widths])
    head = int(np.argmax(spans))
    head_u = bins[head] + 0.8
    head_lo, head_hi = widths[head]
    head_mid, head_half = (head_lo + head_hi) / 2, (head_hi - head_lo) / 2 + 0.5
    shaft = [w for b, w in zip(bins, widths) if b > head_u]
    if not shaft:
        return None
    shaft_mid = float(np.median([(lo + hi) / 2 for lo, hi in shaft]))
    shaft_half = float(np.median([(hi - lo) / 2 for lo, hi in shaft])) + 0.35
    tip_u, tail_u = u.min() - 0.6, u.max() + 0.4
    mid = (head_mid + shaft_mid) / 2

    local = [
        (tip_u, mid),
        (head_u, head_mid + head_half),
        (head_u - 0.6, shaft_mid + shaft_half),
        (tail_u, shaft_mid + shaft_half),
        (tail_u, shaft_mid - shaft_half),
        (head_u - 0.6, shaft_mid - shaft_half),
        (head_u, head_mid - head_half),
    ]
    poly = np.array([centre + a * axis + b * side for a, b in local])

    # Shrink about the centre until the whole arrow, tip included, fits inside the sprite.
    h, w = rgba.shape[:2]
    lo, hi = np.array([0.4, 0.4]), np.array([w - 0.4, h - 0.4])
    scale = 1.0
    for p in poly:
        for k in range(2):
            d = p[k] - centre[k]
            if p[k] < lo[k]:
                scale = min(scale, (lo[k] - centre[k]) / d)
            elif p[k] > hi[k]:
                scale = min(scale, (hi[k] - centre[k]) / d)
    poly = centre + (poly - centre) * scale
    ridge = (centre + mid * side * scale, axis, side)
    return [tuple(p) for p in poly], ridge


def redraw_arrow(rgba, background4):
    """Pointed, two-tone bevelled arrow over the table art. background4 is the 4x table art under the
    sprite."""
    fit = _arrow_polygon(rgba)
    if fit is None:
        return None
    poly, (ridge, axis, side) = fit
    h, w = rgba.shape[:2]
    scale = 4 * SS
    mask = Image.new("L", (w * scale, h * scale), 0)
    ImageDraw.Draw(mask).polygon([(x * scale, y * scale) for x, y in poly], fill=255)
    inner = mask.filter(ImageFilter.MinFilter(2 * SS + 1))  # about 1 HD pixel in from the edge
    m = np.asarray(mask.resize((w * 4, h * 4), Image.LANCZOS), np.float32)[..., None] / 255
    body = np.asarray(inner.resize((w * 4, h * 4), Image.LANCZOS), np.float32)[..., None] / 255

    lit = rgba[..., :3][rgba[..., :3].astype(np.float32).mean(-1) > ARROW_LIT_LEVEL].astype(np.float32)
    base = np.percentile(lit, 75, axis=0)
    # Two facets split along the arrow's spine: the one facing up (towards the light) is brighter.
    yy, xx = np.mgrid[0:h * 4, 0:w * 4]
    pos = np.stack([(xx + 0.5) / 4, (yy + 0.5) / 4], -1) - ridge
    up_side = side if side[1] < 0 else -side
    facing_up = np.clip((pos @ up_side) * 4 + 0.5, 0, 1)[..., None]
    light, shade, rim = np.minimum(base * 1.25, 255), base * 0.8, base * 0.45
    colour = shade + (light - shade) * facing_up
    colour = rim + (colour - rim) * body
    out = background4.astype(np.float32) * (1 - m) + colour * m
    return (out + 0.5).clip(0, 255).astype(np.uint8)


# --- Ball ---------------------------------------------------------------------------------------

# Light comes from the upper left, like the table's own shading.
_KEY_LIGHT = np.array([-0.45, -0.6, 0.66]) / np.linalg.norm([-0.45, -0.6, 0.66])
_ROOM_TOP, _ROOM_HORIZON = np.array([95, 100, 125.0]), np.array([205, 210, 225.0])
_TABLE_NEAR, _TABLE_FAR = np.array([55, 50, 115.0]), np.array([14, 12, 30.0])
# Table colours that streak across the lower half of the ball's reflection.
_TABLE_TINTS = [(np.array([40, 140, 200.0]), 0.6), (np.array([150, 70, 190.0]), 2.2), (np.array([200, 120, 40.0]), 4.0)]


def _environment(r):
    """A simple environment for chrome, seen from above the table: the room and its lights reflect
    in the middle and top of the ball, a sharp horizon, and the dark, coloured table around the
    lower rim. r is an array of unit reflection vectors (x right, y down, z towards the viewer)."""
    # Height above the horizon: mostly how much the reflection points back at the viewer,
    # tilted so the room shows more at the top of the ball.
    elev = r[..., 2] * 0.75 - r[..., 1] * 0.55 + 0.1
    room_t = np.clip(elev / 0.9, 0, 1)[..., None] ** 0.5
    room = _ROOM_HORIZON + (_ROOM_TOP - _ROOM_HORIZON) * room_t
    table_t = np.clip(-elev / 0.7, 0, 1)[..., None] ** 0.6
    table = _TABLE_NEAR + (_TABLE_FAR - _TABLE_NEAR) * table_t
    azimuth = np.arctan2(r[..., 1], r[..., 0])
    for tint, phase in _TABLE_TINTS:
        streak = np.clip(np.cos(azimuth * 3 + phase), 0, 1)[..., None] ** 6 * (1 - table_t) * 0.55
        table = table + (tint - table) * streak
    blend = np.clip((elev + 0.03) / 0.06, 0, 1)[..., None]
    return table + (room - table) * blend


def render_chrome_ball(rgba, scale=4):
    """Chrome ball at the HD scale, sized to the original sprite's ball. Pixels outside the ball are fully
    transparent (colour 0), which the game uses as the ball's see-through mask."""
    h, w = rgba.shape[:2]
    inside = rgba[..., 3] > 0
    ys, xs = np.nonzero(inside)
    cx, cy = (xs.min() + xs.max() + 1) / 2, (ys.min() + ys.max() + 1) / 2
    radius = max(xs.max() + 1 - xs.min(), ys.max() + 1 - ys.min()) / 2

    n_px = scale * SS
    yy, xx = np.mgrid[0:h * n_px, 0:w * n_px]
    nx = ((xx + 0.5) / n_px - cx) / radius
    ny = ((yy + 0.5) / n_px - cy) / radius
    d2 = nx * nx + ny * ny
    cover = d2 <= 1.0
    nz = np.sqrt(np.clip(1 - d2, 0, 1))
    normal = np.stack([nx, ny, nz], -1)
    # Reflect the view ray (0, 0, -1) about the normal.
    refl = np.stack([2 * nz * nx, 2 * nz * ny, 2 * nz * nz - 1], -1)
    colour = _environment(refl)
    facing = np.clip(refl @ _KEY_LIGHT, 0, 1)
    # A soft window reflection plus a sharp highlight from the same light.
    colour = colour + 255 * (0.55 * np.clip((facing - 0.82) / 0.1, 0, 1) + 1.4 * facing ** 400)[..., None]
    # Slightly darker rim so the ball reads against bright parts of the table.
    rim = np.clip((1 - nz) * 1.6 - 0.6, 0, 1)[..., None]
    colour = colour * (1 - 0.45 * rim)
    colour = np.clip(colour, 0, 255) * cover[..., None]

    # Average the supersamples, counting only the ones on the ball, so the edge doesn't go dark.
    def down(a):
        return a.reshape(h * scale, SS, w * scale, SS, -1).sum((1, 3))
    total = down(colour)
    count = down(cover[..., None].astype(np.float32))
    rgb = total / np.maximum(count, 1)
    alpha = (count[..., 0] / (SS * SS)) >= 0.5
    rgb[~alpha] = 0
    out = np.zeros((h * scale, w * scale, 4), np.uint8)
    out[..., :3] = (rgb + 0.5).clip(0, 255).astype(np.uint8)
    out[..., 3] = np.where(alpha, 255, 0)
    return out


def ball_groups(items):
    """Group ids of the ball sprites: the group named "ball" and the unnamed groups right after it
    that hold the same ball at growing sizes (one per distance from the camera)."""
    by_id = {gid: (name, rgba) for gid, _x, _y, rgba, name in items}
    found = set()
    for gid, (name, rgba) in by_id.items():
        if name != "ball":
            continue
        size = rgba.shape[0]
        while gid in by_id and rgba.shape[0] == rgba.shape[1] == size:
            found.add(gid)
            gid, size = gid + 1, size + 1
            if gid in by_id:
                name, rgba = by_id[gid]
                if name:
                    break
    return found


# --- Plunger spring -----------------------------------------------------------------------------

SPRING_TURNS = 6
_STEEL_LIGHT, _STEEL_MID, _STEEL_DARK = np.array([235, 238, 245.0]), np.array([150, 155, 165.0]), np.array([55, 58, 66.0])


def plunger_groups(items):
    """The plunger animation frames: the group named "plunger" and the unnamed frames after it."""
    by_id = {gid: name for gid, _x, _y, _rgba, name in items}
    found = set()
    for gid, name in by_id.items():
        if name == "plunger":
            found.add(gid)
            nxt = gid + 1
            while nxt in by_id and not by_id[nxt]:
                found.add(nxt)
                nxt += 1
    return found


def _spring_pixels(rgb):
    lum = rgb.mean(-1)
    sat = rgb.max(-1) - rgb.min(-1)
    return (lum > 140) & (sat < 90)


def redraw_spring(rgba, rgb4):
    """Replaces the plunger's blotchy upscaled spring with a clean steel coil wound around the rod,
    compressed to the same length as the original frame's spring."""
    rgb = rgba[..., :3].astype(np.float32)
    spring = _spring_pixels(rgb)
    rows = np.nonzero(spring.sum(1) >= 2)[0]
    if len(rows) < 3:
        return rgb4
    top, bottom = rows.min(), rows.max() + 1
    # The plunger leans slightly, so follow the spring's centre line row by row.
    ys, xs = np.nonzero(spring[top:bottom])
    tilt, offset = np.polyfit(ys + top + 0.5, xs + 0.5, 1)
    half_w = max(1.6, (np.percentile(xs, 95) - np.percentile(xs, 5) + 1) / 2 + 0.2)

    h, w = rgba.shape[:2]
    # Below the coil a short stretch of bare rod shows; follow it down so it gets redrawn too.
    lum, sat = rgb.mean(-1), rgb.max(-1) - rgb.min(-1)
    rod_end = bottom
    while rod_end < h:
        cols = np.arange(w) + 0.5
        near = np.abs(cols - (offset + tilt * (rod_end + 0.5))) <= half_w + 0.5
        if not ((lum[rod_end] > 95) & (sat[rod_end] < 90) & near & (rgba[rod_end, :, 3] > 0)).any():
            break
        rod_end += 1

    n = 4 * SS
    yy, xx = np.mgrid[0:h * n, 0:w * n]
    py, px = (yy + 0.5) / n, (xx + 0.5) / n
    cx = offset + tilt * py
    across = (px - cx) / half_w                       # -1..1 across the coil
    along = (py - top) / (bottom - top)               # 0..1 down the coil
    inside_len = (along >= 0) & (along <= 1)

    # Erase the old spring: dark lane colour over the old spring's area.
    dark = rgb[(rgb.mean(-1) < 45) & (rgba[..., 3] > 0)]
    lane = np.median(dark, axis=0) if len(dark) else np.array([20, 20, 24.0])
    pad = 0.7 / (bottom - top)  # also clear stray old spring pixels just above and below
    rod_len = (rod_end - top) / (bottom - top)
    area = (along >= -pad) & (along <= max(1 + pad, rod_len)) & (np.abs(across) <= 1.5)

    # Rod: a shaded steel cylinder through the middle of the coil and down to the plunger tip.
    rod_r = 0.38
    rod = (along >= 0) & (along <= rod_len) & (np.abs(across) <= rod_r)
    rod_shade = np.clip(1 - (across / rod_r + 0.35) ** 2, 0, 1)[..., None]
    rod_col = _STEEL_DARK + (_STEEL_MID - _STEEL_DARK) * rod_shade

    # Coil: a helix seen from the side. At a point across the coil (cos of the wire's angle) the front
    # strand and the back strand pass at different heights within each turn. The back strand is darker
    # and hidden by the rod; the front strand is lit from the upper left.
    a = np.clip(across, -1, 1)
    angle = np.arccos(a) / (2 * np.pi)                # 0..0.5
    phase = along * SPRING_TURNS
    wire = min(0.45, 0.95 / max(bottom - top, 1) * SPRING_TURNS)

    def near(target):
        d = phase - target
        d = d - np.round(d)
        return np.abs(d) < wire / 2
    in_coil = inside_len & (np.abs(across) <= 1)
    front_wire = in_coil & near(angle)
    back_wire = in_coil & near(1 - angle) & ~front_wire
    curve = np.sqrt(np.clip(1 - a ** 2, 0, 1))[..., None]
    lit = np.clip(0.45 + 0.55 * curve - 0.3 * a[..., None], 0, 1)
    front_col = _STEEL_MID + (_STEEL_LIGHT - _STEEL_MID) * lit
    back_col = _STEEL_DARK + (_STEEL_MID - _STEEL_DARK) * 0.35 * curve

    layer = np.zeros((h * n, w * n, 3), np.float32)
    cover = np.zeros((h * n, w * n), np.float32)
    layer[area] = lane
    cover[area] = 1
    layer[back_wire] = back_col[back_wire]
    layer[rod] = rod_col[rod]
    layer[front_wire] = front_col[front_wire]

    def down(a):
        return a.reshape(h * 4, SS, w * 4, SS, -1).mean((1, 3))
    layer4 = down(layer)
    cover4 = down(cover[..., None])
    # Soft edges against the existing upscaled frame.
    out = rgb4.astype(np.float32) * (1 - cover4) + layer4
    return (out + 0.5).clip(0, 255).astype(np.uint8)


# --- Screws -------------------------------------------------------------------------------------

# Where the screw heads sit on the Space Cadet table, in table-art pixels (the table bitmap's own
# coordinates). Each spot is checked against the player's own art and centred on the actual screw
# head before anything is drawn, so a different table or data file simply gets no screws.
TABLE_SCREWS = [
    (89, 20), (166, 21.5), (200, 21.5), (210.8, 55.2), (155, 55), (89.5, 56.5), (130.5, 66),
    (77.8, 68.2), (294.5, 79), (245.8, 89.2), (88.3, 92.2), (164, 124.8), (193.5, 133.1),
    (237.5, 134.5), (105.5, 157.6), (261.5, 168.5), (277.5, 224.5), (104.5, 281.5), (260, 282),
    (284.5, 282.5), (124.2, 339.5), (240.5, 340), (49.5, 375.5), (316.5, 375.5), (262.9, 396.9),
    (101.5, 397.5), (182.5, 409.5),
]
SCREW_RADIUS = 1.75        # head radius in original pixels
SMALL_SCREW_RADIUS = 1.3   # for the tiny rivets on the ends of narrow rails
LARGE_SCREW_RADIUS = 2.1   # for the big screws on the slingshots and beside the flippers
_LIGHT = np.array([-0.45, -0.6, 0.66])
_LIGHT = _LIGHT / np.linalg.norm(_LIGHT)


def _screw_head(rgb, px, py):
    """The light grey screw head near (px, py) in a 1x image, or None. Returns its centre, the number
    of light pixels and how far the old screw reaches from the centre (in original pixels)."""
    h, w = rgb.shape[:2]
    x0, y0 = max(int(px) - 5, 0), max(int(py) - 5, 0)
    x1, y1 = min(int(px) + 6, w), min(int(py) + 6, h)
    if x1 <= x0 or y1 <= y0:
        return None
    win = rgb[y0:y1, x0:x1].astype(np.float32)
    lum = win.mean(-1)
    light = (lum > 95) & (win.max(-1) - win.min(-1) < 80)
    yy, xx = np.mgrid[y0:y1, x0:x1] + 0.5
    near = (xx - px) ** 2 + (yy - py) ** 2 <= 2.6 ** 2
    core = light & near & (lum > 105)
    if core.sum() < 2:
        return None
    # Start from the brightest part, then centre on all of the screw's light pixels: the bigger
    # screws are rings that are brighter on top, which would pull a weighted centre upwards.
    weight = (lum - 90).clip(0) * core
    cx, cy = (weight * xx).sum() / weight.sum(), (weight * yy).sum() / weight.sum()
    for _ in range(2):
        part = light & ((xx - cx) ** 2 + (yy - cy) ** 2 <= 2.8 ** 2)
        cx, cy = xx[part].mean(), yy[part].mean()
    reach = np.sqrt((xx[part] - cx) ** 2 + (yy[part] - cy) ** 2).max() + 0.75
    return cx, cy, int(core.sum()), reach


def find_screws(items):
    """Screen positions and sizes of the table's screws: [(x, y, radius, reach)] in original pixels."""
    table = next(((x, y, rgba) for _gid, x, y, rgba, name in items if name == "table"), None)
    if table is None:
        return []
    tx, ty, rgba = table
    found = []
    for px, py in TABLE_SCREWS:
        head = _screw_head(rgba[..., :3], px, py)
        if head:
            cx, cy, size, reach = head
            radius = SMALL_SCREW_RADIUS if size <= 3 else LARGE_SCREW_RADIUS if reach >= 3 else SCREW_RADIUS
            found.append((tx + cx, ty + cy, radius, reach))
    return found


def _render_screw(radius):
    """One screw: a domed steel pan head with a Phillips cross, lit from the upper left, sitting in a
    soft dark shadow. Returns (rgb, alpha) as float arrays at SS x the 4x pixel grid, centred on the
    middle of the patch, and the patch's half size in 4x pixels."""
    r = radius * 4                      # head radius in 4x pixels
    half = int(np.ceil(r * 1.5)) + 1
    n = 2 * half * SS
    yy, xx = np.mgrid[0:n, 0:n]
    u = ((xx + 0.5) / SS - half) / r     # -1..1 over the head
    v = ((yy + 0.5) / SS - half) / r
    d = np.sqrt(u * u + v * v)

    # Dome normal from a flattened sphere.
    z = np.sqrt(np.clip(1 - d * d, 0.02, 1))
    k = 0.55
    nrm = np.stack([u * k / z, v * k / z, np.ones_like(u)], -1)
    nrm /= np.linalg.norm(nrm, axis=-1, keepdims=True)
    diffuse = np.clip(nrm @ _LIGHT, 0, 1)
    half_vec = _LIGHT + np.array([0, 0, 1.0])
    half_vec /= np.linalg.norm(half_vec)
    spec = np.clip(nrm @ half_vec, 0, 1) ** 40
    steel = np.array([150, 154, 162.0])
    col = steel * (0.3 + 0.75 * diffuse)[..., None] + 255 * 0.85 * spec[..., None]
    # A slightly darker bevel round the edge of the head.
    col *= (1 - 0.35 * np.clip((d - 0.82) / 0.18, 0, 1))[..., None]

    # Phillips cross: two slots. Inside, the wall facing the light is lit and the rest is in shadow.
    arm_w, arm_l = 0.15, 0.62
    horiz = (np.abs(v) < arm_w) & (np.abs(u) < arm_l)
    vert = (np.abs(u) < arm_w) & (np.abs(v) < arm_l)
    recess = np.array([38, 40, 46.0])
    wall = np.array([120, 124, 132.0])
    lit_wall = (horiz & (v > arm_w * 0.35)) | (vert & (u > arm_w * 0.35))
    col[horiz | vert] = recess
    col[lit_wall] = wall
    col[horiz & vert] = recess * 0.8

    head = (d <= 1).astype(np.float32)
    # Shadow: a dark ring round the head, pushed a little down and to the right.
    ds = np.sqrt((u - 0.12) ** 2 + (v - 0.15) ** 2)
    shadow = 0.85 * np.clip((1.38 - ds) / 0.38, 0, 1)
    alpha = np.maximum(head, shadow)
    rgb = col * head[..., None]  # shadow is black
    return rgb, alpha, half


def _erase_old_screw(out, reach, lx, ly, radius):
    """Paints over the old upscaled screw (often a lopsided white blob or ring, larger than the new
    head) with the dark colour around it. Only light, cool coloured pixels are touched, so rails and
    inserts next to the screw keep their colour."""
    reach = np.clip(reach + 0.5, radius + 0.3, 4) * 4
    cx4, cy4 = lx * 4, ly * 4
    X0, Y0 = max(int(cx4 - reach - 8), 0), max(int(cy4 - reach - 8), 0)
    X1, Y1 = min(int(cx4 + reach + 9), out.shape[1]), min(int(cy4 + reach + 9), out.shape[0])
    patch = out[Y0:Y1, X0:X1]
    yy, xx = np.mgrid[Y0:Y1, X0:X1] + 0.5
    dist = np.sqrt((xx - cx4) ** 2 + (yy - cy4) ** 2)
    lum = patch.mean(-1)
    sat = patch.max(-1) - patch.min(-1)
    ring = (dist >= reach) & (dist < reach + 8) & (lum < 70)
    dark = np.median(patch[ring], axis=0) if ring.sum() >= 8 else np.array([14, 14, 16.0])
    cool = (sat < 90) | (patch[..., 2] >= patch[..., 0])   # white and blue-white, not peach or red
    old = (dist < reach) & cool & (lum > dark.mean() + 25)
    # Feather the edge so no hard outline is left.
    edge = np.clip((reach - dist) / 2, 0, 1)[..., None] * old[..., None]
    patch[:] = patch * (1 - edge) + dark * edge


def stamp_screws(rgb4, screws, x, y, rgba):
    """Draws the screws that fall on a bitmap at (x, y) over its 4x image. Only screws whose spot
    shows the same small grey screw head in this bitmap's own 1x pixels are drawn, so a sprite that
    covers a screw with something else (a lit insert, a pop-up post) is left alone."""
    h, w = rgba.shape[:2]
    out = None
    for sx, sy, radius, table_reach in screws:
        lx, ly = sx - x, sy - y
        if not (0 <= lx < w and 0 <= ly < h) or rgba[int(ly), int(lx), 3] == 0:
            continue
        head = _screw_head(rgba[..., :3], lx, ly)
        if not head or head[3] > table_reach + 0.5:
            continue
        if out is None:
            out = rgb4.astype(np.float32)
        _erase_old_screw(out, head[3], lx, ly, radius)
        rgb, alpha, half = _render_screw(radius)
        # Place the patch so its centre lands exactly on the screw centre (in 4x pixels).
        cx4, cy4 = lx * 4, ly * 4
        ox, oy = int(round(cx4)) - half, int(round(cy4)) - half
        # Sub-pixel shift is absorbed by resampling at SS resolution.
        shift_x = int(round((cx4 - round(cx4)) * SS))
        shift_y = int(round((cy4 - round(cy4)) * SS))
        rgb = np.roll(rgb, (shift_y, shift_x), (0, 1))
        alpha = np.roll(alpha, (shift_y, shift_x), (0, 1))
        size = 2 * half
        rgb = rgb.reshape(size, SS, size, SS, 3).mean((1, 3))
        alpha = alpha.reshape(size, SS, size, SS).mean((1, 3))
        x0, y0 = max(ox, 0), max(oy, 0)
        x1, y1 = min(ox + size, w * 4), min(oy + size, h * 4)
        if x1 <= x0 or y1 <= y0:
            continue
        p_rgb = rgb[y0 - oy:y1 - oy, x0 - ox:x1 - ox]
        p_a = alpha[y0 - oy:y1 - oy, x0 - ox:x1 - ox, None]
        out[y0:y1, x0:x1] = out[y0:y1, x0:x1] * (1 - p_a) + p_rgb
    if out is None:
        return rgb4
    return (out + 0.5).clip(0, 255).astype(np.uint8)
