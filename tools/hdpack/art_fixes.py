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
