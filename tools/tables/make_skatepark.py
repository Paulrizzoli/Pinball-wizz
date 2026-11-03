"""Builds the Skatepark table: Tables/Skatepark.pwtable and its playfield picture Tables/Skatepark.jpg.

Everything is drawn here from scratch (no art from other games): a concrete park with a tiled bowl around
the bumpers, saw-cut joints, a painted grind ledge, spray-paint tags and the table's name.

    python tools/tables/make_skatepark.py

Needs Pillow (pip install pillow). The lettering uses Inter (SIL Open Font License, tools/hdpack/OFL.txt).
"""

import json
import math
import os
import random

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
TABLES = os.path.join(ROOT, "Tables")
FONT = os.path.join(ROOT, "tools", "hdpack", "Inter-ExtraBoldItalic.otf")

NAME = "Skatepark"
W, L = 16.0, 29.0

# Colours (also used for the parts and lights, so the picture and the table agree).
ORANGE = "#ff5a1f"
MINT = "#2ee6a8"
YELLOW = "#ffd23f"
PINK = "#ff3d7f"
SKY = "#3db8ff"
RED = "#ff3b30"


# -------------------------------------------------------------------------------------------------
# Layout
# -------------------------------------------------------------------------------------------------

def arch(cx, cy, r, x_left, x_right, steps=24):
    """The top of the cabinet: straight sides joined by a half circle."""
    pts = [[x_left, 14.5], [x_left, cy]]
    for i in range(1, steps):
        a = math.pi + math.pi * i / steps
        pts.append([round(cx + r * math.cos(a), 3), round(cy + r * math.sin(a), 3)])
    pts += [[x_right, cy], [x_right, 14.5]]
    return pts


def light(id, x, y, color, radius=0.3, label=None, shape=None, angle=None, label_size=None):
    p = {"type": "light", "id": id, "x": x, "y": y, "radius": radius, "color": color}
    if shape:
        p["shape"] = shape
    if angle is not None:
        p["angle"] = angle
    if label:
        p["label"] = label
    if label_size:
        p["labelSize"] = label_size
    return p


def drop_bank(center, angle, count, spacing):
    """Positions along a bank of drop targets whose faces look towards `angle` degrees."""
    a = math.radians(angle)
    along = (-math.sin(a), math.cos(a))
    out = []
    for i in range(count):
        k = i - (count - 1) / 2
        out.append((round(center[0] + along[0] * spacing * k, 3), round(center[1] + along[1] * spacing * k, 3)))
    return out


DECK_ANGLE = 58
DECK = drop_bank((-3.15, -0.9), DECK_ANGLE, 4, 0.98)
# Lights sit a little in front of each drop target.
DECK_LIGHTS = [(round(x + math.cos(math.radians(DECK_ANGLE)) * 0.95, 3),
                round(y + math.sin(math.radians(DECK_ANGLE)) * 0.95, 3)) for x, y in DECK]

RAMP = [[3.2, 1.0], [3.9, -2.4], [4.75, -5.8], [5.0, -8.1], [4.35, -9.7]]
RAIL_X0, RAIL_X1, RAIL_TOP, RAIL_BOTTOM = -0.55, 0.75, -4.4, -1.5
BUMPERS = [(-1.9, -8.0), (1.7, -8.3), (-0.05, -6.0)]
BOWL_HOLE = (-4.3, -9.6)
TARGETS = [(6.05, 2.3), (6.05, 3.45)]
TARGET_ANGLE = 180


def build_table():
    parts = [
        {"type": "wall", "id": "outer wall", "points": arch(0, -7.0, 7.6, -7.6, 7.6)},
        {"type": "wall", "id": "shooter lane wall", "points": [[6.6, 14.5], [6.6, -6.0]]},
        {"type": "gate", "id": "shooter gate", "points": [[6.6, -6.0], [7.55, -6.9]]},
        {"type": "wall", "id": "left inlane guide", "points": [[-7.6, 7.5], [-3.35, 11.75]]},
        {"type": "wall", "id": "right inlane guide", "points": [[6.6, 7.5], [2.35, 11.75]]},
        # Closes the pocket between the right slingshot and the shooter lane, so a ball can't get stuck there.
        {"type": "wall", "id": "right sling cover", "points": [[5.0, 6.6], [6.6, 5.0]]},
        {"type": "slingshot", "id": "left slingshot", "points": [[-6, 6.6], [-6, 8.9], [-4.4, 10.3]],
         "color": ORANGE},
        {"type": "slingshot", "id": "right slingshot", "points": [[5, 6.6], [3.4, 10.3], [5, 8.9]],
         "color": ORANGE},
        {"type": "flipper", "side": "left", "x": -3.1, "y": 12.0, "length": 2.2},
        {"type": "flipper", "side": "right", "x": 2.1, "y": 12.0, "length": 2.2},
        {"type": "plunger", "x": 7.1, "y": 13.2, "width": 0.7},
        {"type": "drain", "y": 14.25, "x0": -7.6, "x1": 6.6},

        # Left: the halfpipe orbit, with a wheel spinner and a switch near the top.
        {"type": "wall", "id": "halfpipe wall", "points": [[-5.6, 3.2], [-5.6, -5.4], [-5.9, -6.8]],
         "thickness": 0.3},
        {"type": "post", "id": "halfpipe post", "x": -5.6, "y": 3.2, "radius": 0.25},
        {"type": "spinner", "id": "wheel spinner", "x": -6.6, "y": -0.6, "width": 1.6, "angle": 0,
         "color": YELLOW, "score": 150},
        {"type": "lane", "id": "halfpipe switch", "points": [[-7.5, -4.6], [-5.75, -4.6]], "score": 1000,
         "bonus": 1000},
        light("halfpipe light", -6.6, 2.0, MINT, 0.32, "AIR", "arrow", -90),
    ]

    # Top lanes S K 8.
    for x in (-3.0, -1.0, 1.0, 3.0):
        top = -7.0 - math.sqrt(7.6 ** 2 - x ** 2) + 0.12
        parts.append({"type": "wall", "id": f"top guide {x:g}", "points": [[x, round(top, 3)], [x, -11.0]],
                      "thickness": 0.22})
    for i, (x, letter) in enumerate(zip((-2.0, 0.0, 2.0), "SK8"), 1):
        parts.append({"type": "lane", "id": f"lane {letter}", "points": [[x - 0.85, -11.8], [x + 0.85, -11.8]],
                      "score": 500, "bonus": 1000})
        parts.append(light(f"lane light {letter}", x, -10.25, YELLOW, 0.3, letter, label_size=0.34))

    # Traffic-cone bumpers in the bowl.
    for i, (x, y) in enumerate(BUMPERS, 1):
        parts.append({"type": "bumper", "id": f"cone {i}", "x": x, "y": y, "radius": 0.62, "color": ORANGE,
                      "capColor": "#fff3e6", "score": 1000})

    # The bowl drain hole: the DROP IN mission.
    parts.append({"type": "kickout", "id": "bowl hole", "x": BOWL_HOLE[0], "y": BOWL_HOLE[1], "radius": 0.45,
                  "angle": 55, "strength": 1.0, "color": MINT, "score": 2500})
    parts.append(light("drop in light", -3.75, -8.2, MINT, 0.3, "DROP IN", "arrow", -112))

    # DECK drop targets on the left.
    # The bank runs from upper right to lower left, so spell DECK from the lower left.
    for (x, y), (lx, ly), letter in zip(DECK[::-1], DECK_LIGHTS[::-1], "DECK"):
        parts.append({"type": "drop", "id": f"drop {letter}", "x": x, "y": y, "width": 0.88,
                      "angle": DECK_ANGLE, "color": PINK, "score": 750, "bonus": 1000})
        parts.append(light(f"deck light {letter}", lx, ly, PINK, 0.24, letter, label_size=0.28))

    # Grind rail up the middle: a narrow ledge lane that feeds the bumpers.
    parts += [
        {"type": "wall", "id": "rail left", "points": [[RAIL_X0, RAIL_BOTTOM], [RAIL_X0, RAIL_TOP]],
         "thickness": 0.2, "color": "#d9dde2"},
        {"type": "wall", "id": "rail right", "points": [[RAIL_X1, RAIL_BOTTOM], [RAIL_X1, RAIL_TOP]],
         "thickness": 0.2, "color": "#d9dde2"},
        {"type": "post", "id": "rail post left", "x": RAIL_X0, "y": RAIL_BOTTOM, "radius": 0.2},
        {"type": "post", "id": "rail post right", "x": RAIL_X1, "y": RAIL_BOTTOM, "radius": 0.2},
        {"type": "lane", "id": "rail switch", "points": [[RAIL_X0 + 0.12, -3.2], [RAIL_X1 - 0.12, -3.2]],
         "score": 2000, "bonus": 1500},
        light("rail light", (RAIL_X0 + RAIL_X1) / 2, -0.35, RED, 0.32, "GRIND", "arrow", -90),
    ]

    # KICK and FLIP standup targets against the right wall, below the mega ramp.
    for (x, y), word in zip(TARGETS, ("KICK", "FLIP")):
        parts.append({"type": "target", "id": f"{word.lower()} target", "x": x, "y": y, "width": 0.85,
                      "angle": TARGET_ANGLE, "color": SKY, "score": 1500, "bonus": 1000})
    parts.append(light("kick light", 5.2, 2.3, SKY, 0.22, "KICK", shape="arrow", angle=0, label_size=0.22))
    parts.append(light("flip light", 5.2, 3.45, SKY, 0.22, "FLIP", shape="arrow", angle=0, label_size=0.22))

    # The mega ramp on the right.
    parts.append({"type": "ramp", "id": "mega ramp", "points": RAMP, "width": 1.3, "color": ORANGE,
                  "railColor": "#e8ecef", "score": 5000, "bonus": 2000})
    for i in range(4):
        parts.append(light(f"ramp light {i + 1}", round(3.05 + i * 0.12, 2), round(2.9 - i * 0.62, 2), ORANGE,
                           0.24, shape="arrow", angle=-80))

    # Multipliers and shoot again, down by the flippers.
    for x, m in zip((-2.0, -1.0, 0.0, 1.0), ("2X", "3X", "5X", "10X")):
        parts.append(light(f"mult {m}", x, 8.2, YELLOW, 0.3, m))
    parts.append(light("shoot again", -0.5, 10.3, RED, 0.36, "SHOOT AGAIN", label_size=0.26))

    rules = {
        "ballSave": 8,
        "extraBallLight": "shoot again",
        "multiplierLights": ["mult 2X", "mult 3X", "mult 5X", "mult 10X"],
        "groups": [
            {"name": "S-K-8 LANES", "parts": ["lane S", "lane K", "lane 8"],
             "lights": ["lane light S", "lane light K", "lane light 8"], "score": 3000, "bonus": 3000,
             "award": "multiplier"},
            {"name": "DECK", "parts": ["drop D", "drop E", "drop C", "drop K"],
             "lights": ["deck light D", "deck light E", "deck light C", "deck light K"], "score": 10000,
             "bonus": 5000, "award": "startMission", "startsMission": "DROP IN"},
            {"name": "DROP IN", "mission": True, "time": 30, "hint": "SHOOT THE BOWL", "parts": ["bowl hole"],
             "lights": ["drop in light"], "score": 50000, "bonus": 10000, "award": "extraBall",
             "message": "DROPPED IN"},
            {"name": "KICKFLIP", "parts": ["kick target", "flip target"], "lights": ["kick light", "flip light"],
             "score": 5000, "bonus": 3000, "award": "startMission", "startsMission": "GRIND"},
            {"name": "GRIND", "mission": True, "time": 25, "hint": "SHOOT THE RAIL", "parts": ["rail switch"],
             "lights": ["rail light"], "score": 40000, "bonus": 8000, "message": "SICK GRIND"},
            {"name": "MEGA RAMP", "parts": ["mega ramp"], "hits": 4,
             "lights": ["ramp light 1", "ramp light 2", "ramp light 3", "ramp light 4"], "score": 25000,
             "award": "multiball", "message": "SESSION MULTIBALL"},
            {"name": "HALFPIPE", "parts": ["halfpipe switch"], "hits": 3, "lights": ["halfpipe light"],
             "score": 15000, "bonus": 5000, "message": "BIG AIR"},
            {"name": "CONES", "parts": ["cone 1", "cone 2", "cone 3"], "hits": 25, "lights": [],
             "score": 20000, "message": "CONE ZONE"},
        ],
    }

    return {
        "format": "pinball-wizz-table",
        "version": 1,
        "name": NAME,
        "author": "Pinball Wizz",
        "playfield": {"width": W, "length": L, "color": "#8a8f95", "color2": "#3c4046",
                      "image": NAME + ".jpg"},
        "theme": {"name": "Skatepark", "walls": "#eef1f4", "rubber": "#ffffff", "accent": ORANGE,
                  "accent2": MINT, "cabinet": "#23262b", "panel": "#121417", "metal": "#c9ced4",
                  "display": "#ff7a2f"},
        "ballsPerGame": 3,
        "parts": parts,
        "rules": rules,
    }


# -------------------------------------------------------------------------------------------------
# Playfield picture
# -------------------------------------------------------------------------------------------------

PX = 64  # pixels per table unit (the most the game uses)
IW, IH = int(W * PX), int(L * PX)
SS = 2   # drawn at twice the size, then scaled down for smooth edges


def U(x):
    return (x + W / 2) * PX * SS


def V(y):
    return (y + L / 2) * PX * SS


def rgb(h, a=255):
    h = h.lstrip("#")
    return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16), a)


def font(size):
    return ImageFont.truetype(FONT, int(size))


def layer():
    return Image.new("RGBA", (IW * SS, IH * SS), (0, 0, 0, 0))


def over(base, top):
    base.alpha_composite(top)


def concrete_base(rnd):
    w, h = IW * SS, IH * SS
    # Gradient: lighter at the far end, darker by the flippers.
    grad = Image.new("RGBA", (1, h))
    for y in range(h):
        t = y / (h - 1)
        top, bot = (124, 128, 133), (66, 70, 76)
        grad.putpixel((0, y), tuple(int(top[i] + (bot[i] - top[i]) * t) for i in range(3)) + (255,))
    img = grad.resize((w, h))

    # Blotchy stains: low-frequency noise, blurred.
    small = Image.effect_noise((w // 48, h // 48), 14).resize((w, h), Image.BICUBIC)
    small = small.filter(ImageFilter.GaussianBlur(18))
    stain = Image.merge("RGBA", [small, small, small, Image.new("L", (w, h), 255)])
    img = ImageChops.overlay(img, stain)

    # Fine grain.
    grain = Image.effect_noise((w // 2, h // 2), 16).resize((w, h), Image.NEAREST)
    grain = Image.merge("RGBA", [grain, grain, grain, Image.new("L", (w, h), 255)])
    img = ImageChops.blend(img, ImageChops.overlay(img, grain), 0.55)

    # Aggregate specks.
    d = ImageDraw.Draw(img)
    for _ in range(int(W * L * 55)):
        x, y = rnd.uniform(0, w), rnd.uniform(0, h)
        r = rnd.choice((1, 1, 1.5, 2, 2.5))
        c = rnd.choice(((150, 152, 156), (52, 54, 58), (170, 166, 158), (96, 98, 102)))
        d.ellipse((x - r, y - r, x + r, y + r), fill=c + (255,))
    return img


def saw_cuts(img):
    """Expansion joints cut into the slab, with a light lip on one side."""
    d = ImageDraw.Draw(img)
    for y in (-2.6, 4.2, 10.4):
        d.line((U(-W / 2), V(y), U(W / 2), V(y)), fill=(34, 36, 40, 255), width=5)
        d.line((U(-W / 2), V(y) + 4, U(W / 2), V(y) + 4), fill=(150, 154, 158, 120), width=2)
    for x in (-2.6, 2.6):
        d.line((U(x), V(-2.6), U(x), V(4.2)), fill=(34, 36, 40, 255), width=5)
        d.line((U(x) + 4, V(-2.6), U(x) + 4, V(4.2)), fill=(150, 154, 158, 120), width=2)


def bowl(img, rnd):
    """The bowl around the bumpers: a darker basin with a pool-tile band and coping."""
    cx, cy = 0.0, -7.0
    # Basin: an oval reaching from the top lanes down past the lowest bumper.
    basin = layer()
    d = ImageDraw.Draw(basin)
    box = (U(-5.1), V(-11.3), U(5.1), V(-3.5))
    d.ellipse(box, fill=(150, 168, 174, 255))
    # Transition shading: lighter towards the lip, as a curved wall would catch the light.
    for i in range(30):
        t = i / 29
        inset = t * 2.3 * PX * SS
        shade = (int(150 - 72 * t), int(168 - 70 * t), int(174 - 66 * t), 255)
        d.ellipse((box[0] + inset, box[1] + inset * 0.78, box[2] - inset, box[3] - inset * 0.78), fill=shade)
    # Concrete grain on the basin too.
    grain = Image.effect_noise(basin.size, 18)
    grain = Image.merge("RGBA", [grain, grain, grain, basin.getchannel("A")])
    basin = ImageChops.blend(basin, ImageChops.overlay(basin, grain), 0.5)
    mask = basin.getchannel("A")
    img.paste(basin, (0, 0), mask)

    # Pool tiles: a ring of small blue squares just inside the lip.
    d = ImageDraw.Draw(img)
    rx, ry = (box[2] - box[0]) / 2, (box[3] - box[1]) / 2
    mx, my = (box[0] + box[2]) / 2, (box[1] + box[3]) / 2
    n = 110
    for k in range(2):
        for i in range(n):
            a = 2 * math.pi * (i + 0.5 * k) / n
            r0 = 1 - 0.045 - k * 0.04
            x, y = mx + math.cos(a) * rx * r0, my + math.sin(a) * ry * r0
            s = 9
            col = (38, 120, 190, 255) if (i + k) % 2 == 0 else (230, 236, 240, 255)
            d.rectangle((x - s, y - s, x + s, y + s), fill=col, outline=(20, 40, 60, 255), width=2)

    # Coping: a steel pipe around the lip.
    d.ellipse(box, outline=(30, 32, 36, 255), width=26)
    d.ellipse((box[0] + 4, box[1] + 4, box[2] - 4, box[3] - 4), outline=(214, 220, 226, 255), width=14)
    d.ellipse((box[0] + 6, box[1] + 6, box[2] - 6, box[3] - 6), outline=(250, 252, 255, 255), width=4)

    # A drain grate at the bottom of the bowl.
    gx, gy = U(cx), V(-7.2)
    d.ellipse((gx - 30, gy - 30, gx + 30, gy + 30), fill=(26, 28, 30, 255), outline=(150, 156, 160, 255), width=5)
    for k in range(-2, 3):
        d.line((gx + k * 10, gy - 24, gx + k * 10, gy + 24), fill=(120, 126, 130, 255), width=3)


def halfpipe(img):
    """The left orbit lane, painted as the flat bottom of a halfpipe with deck lines."""
    d = ImageDraw.Draw(img, "RGBA")
    x0, x1 = U(-7.6), U(-5.6)
    for i in range(24):
        t = i / 23
        x = x0 + (x1 - x0) * t
        # Light on the curved walls, darker on the flat bottom.
        k = abs(t - 0.5) * 2
        c = int(78 + 70 * k * k)
        d.line((x, V(-5.8), x, V(3.0)), fill=(c, c + 4, c + 8, 200), width=int((x1 - x0) / 24) + 2)
    # Painted edge stripes.
    d.line((x0 + 18, V(-5.4), x0 + 18, V(3.2)), fill=rgb(MINT, 200), width=6)
    d.line((x1 - 26, V(-5.4), x1 - 26, V(3.2)), fill=rgb(MINT, 200), width=6)
    f = font(0.42 * PX * SS)
    txt = layer()
    td = ImageDraw.Draw(txt)
    td.text((0, 0), "HALFPIPE", font=f, fill=rgb(MINT, 230))
    bbox = txt.getbbox()
    txt = txt.crop(bbox).rotate(90, expand=True)
    img.alpha_composite(txt, (int(U(-6.6) - txt.width / 2), int(V(-2.7) - txt.height / 2)))


def grind_ledge(img):
    """A painted kerb under the grind rail: red and white blocks with a waxed top."""
    d = ImageDraw.Draw(img, "RGBA")
    x0, x1 = U(RAIL_X0 - 0.15), U(RAIL_X1 + 0.15)
    d.rectangle((x0, V(RAIL_TOP - 0.3), x1, V(RAIL_BOTTOM + 0.1)), fill=(30, 30, 34, 255))
    step = 0.5
    y = RAIL_TOP - 0.25
    i = 0
    while y < RAIL_BOTTOM:
        col = rgb(RED) if i % 2 == 0 else (240, 240, 236, 255)
        d.rectangle((x0 + 6, V(y), x1 - 6, V(min(y + step, RAIL_BOTTOM + 0.05)) - 3), fill=col)
        y += step
        i += 1
    # Wax scuffs along the centre.
    for k in range(18):
        yy = V(RAIL_TOP + k * 0.16)
        d.line((U(0.1) - 18, yy, U(0.1) + 18, yy + 10), fill=(255, 255, 255, 50), width=3)


def stencil_arrow(d, x, y, angle, size, color):
    a = math.radians(angle)
    ca, sa = math.cos(a), math.sin(a)

    def p(px, py):
        return (U(x) + (px * ca - py * sa) * size, V(y) + (px * sa + py * ca) * size)

    shape = [(0.9, 0), (0.1, -0.6), (0.1, -0.25), (-0.8, -0.25), (-0.8, 0.25), (0.1, 0.25), (0.1, 0.6)]
    d.polygon([p(*q) for q in shape], fill=color)


def skateboard(img, cx, cy, length, angle):
    """A skateboard seen from below: deck with kicked ends, trucks and wheels."""
    s = PX * SS
    board = Image.new("RGBA", (int(length * s * 1.2), int(length * s * 0.5)), (0, 0, 0, 0))
    d = ImageDraw.Draw(board)
    bw, bh = board.size
    deck_h = bh * 0.52
    top = (bh - deck_h) / 2
    # Shadow.
    d.rounded_rectangle((bw * 0.08 + 10, top + 14, bw * 0.92 + 10, top + deck_h + 14), radius=deck_h / 2,
                        fill=(0, 0, 0, 90))
    # Deck with a bold graphic.
    d.rounded_rectangle((bw * 0.08, top, bw * 0.92, top + deck_h), radius=deck_h / 2, fill=rgb(ORANGE),
                        outline=(25, 20, 18, 255), width=8)
    d.polygon([(bw * 0.3, top + 8), (bw * 0.55, top + 8), (bw * 0.45, top + deck_h - 8),
               (bw * 0.2, top + deck_h - 8)], fill=rgb(YELLOW))
    d.polygon([(bw * 0.58, top + 8), (bw * 0.7, top + 8), (bw * 0.6, top + deck_h - 8),
               (bw * 0.48, top + deck_h - 8)], fill=rgb(PINK))
    # Trucks and wheels.
    for tx in (0.24, 0.76):
        x = bw * tx
        d.rectangle((x - 16, top - 26, x + 16, top + deck_h + 26), fill=(190, 196, 202, 255),
                    outline=(40, 42, 46, 255), width=5)
        for wy in (top - 40, top + deck_h + 40):
            d.rounded_rectangle((x - 30, wy - 26, x + 30, wy + 26), radius=18, fill=(245, 240, 225, 255),
                                outline=(40, 42, 46, 255), width=6)
            d.ellipse((x - 9, wy - 9, x + 9, wy + 9), fill=(120, 124, 128, 255))
    board = board.rotate(angle, expand=True, resample=Image.BICUBIC)
    img.alpha_composite(board, (int(U(cx) - board.width / 2), int(V(cy) - board.height / 2)))


def spray_tag(img, text, x, y, size, fill, outline, angle=0, drips=True, rnd=None):
    """Bold slanted lettering with a thick outline, a drop shadow and a few paint drips."""
    f = font(size * PX * SS)
    pad = int(size * PX * SS)
    tmp = Image.new("RGBA", (int(len(text) * size * PX * SS * 1.0 + pad * 2), int(size * PX * SS * 2.2)),
                    (0, 0, 0, 0))
    d = ImageDraw.Draw(tmp)
    stroke = max(4, int(size * PX * SS * 0.09))
    d.text((pad + 12, pad * 0.35 + 14), text, font=f, fill=(0, 0, 0, 140), stroke_width=stroke,
           stroke_fill=(0, 0, 0, 140))
    d.text((pad, pad * 0.35), text, font=f, fill=fill, stroke_width=stroke, stroke_fill=outline)
    bbox = tmp.getbbox()
    if drips and rnd is not None:
        d = ImageDraw.Draw(tmp)
        for _ in range(len(text) * 2):
            dx = rnd.uniform(bbox[0] + pad * 0.3, bbox[2] - pad * 0.3)
            # Find the bottom of the paint in this column.
            col_bottom = None
            for yy in range(bbox[3] - 1, bbox[1], -1):
                if tmp.getpixel((int(dx), yy))[3] > 200 and tmp.getpixel((int(dx), yy))[:3] == fill[:3]:
                    col_bottom = yy
                    break
            if col_bottom is None:
                continue
            ln = rnd.uniform(0.15, 0.5) * size * PX * SS
            wdt = rnd.uniform(5, 9)
            d.rounded_rectangle((dx - wdt / 2, col_bottom - 4, dx + wdt / 2, col_bottom + ln), radius=wdt / 2,
                                fill=fill)
            d.ellipse((dx - wdt * 0.8, col_bottom + ln - wdt * 0.6, dx + wdt * 0.8, col_bottom + ln + wdt),
                      fill=fill)
        bbox = tmp.getbbox()
    tmp = tmp.crop(bbox)
    if angle:
        tmp = tmp.rotate(angle, expand=True, resample=Image.BICUBIC)
    img.alpha_composite(tmp, (int(U(x) - tmp.width / 2), int(V(y) - tmp.height / 2)))


def splatter(img, rnd, x, y, color, spread):
    d = ImageDraw.Draw(img, "RGBA")
    for _ in range(60):
        r = abs(rnd.gauss(0, spread)) * PX * SS
        a = rnd.uniform(0, 2 * math.pi)
        s = rnd.uniform(2, 9) * (1 - min(r / (spread * PX * SS * 3), 0.8))
        px, py = U(x) + math.cos(a) * r, V(y) + math.sin(a) * r
        d.ellipse((px - s, py - s, px + s, py + s), fill=color[:3] + (rnd.randint(150, 230),))


def stars(img, rnd, x, y, r, color):
    d = ImageDraw.Draw(img)
    pts = []
    for i in range(10):
        a = -math.pi / 2 + i * math.pi / 5
        rr = r if i % 2 == 0 else r * 0.45
        pts.append((U(x) + math.cos(a) * rr * PX * SS, V(y) + math.sin(a) * rr * PX * SS))
    d.polygon(pts, fill=color, outline=(20, 20, 24, 255))


def hazard_stripes(img):
    """Yellow and black stripes along the drain."""
    band = layer()
    d = ImageDraw.Draw(band)
    y0, y1 = V(13.35), V(14.5)
    for i in range(-4, 40):
        x = U(-8) + i * 0.8 * PX * SS
        d.polygon([(x, y1), (x + 0.4 * PX * SS, y1), (x + 0.4 * PX * SS + (y1 - y0), y0), (x + (y1 - y0), y0)],
                  fill=(255, 205, 40, 255))
    d.rectangle((0, y0, IW * SS, y1), fill=None)
    base = Image.new("RGBA", band.size, (0, 0, 0, 0))
    ImageDraw.Draw(base).rectangle((0, y0, IW * SS, y1), fill=(24, 24, 26, 255))
    base.alpha_composite(band)
    mask = Image.new("L", band.size, 0)
    ImageDraw.Draw(mask).rectangle((0, y0, IW * SS, y1), fill=200)
    img.paste(base, (0, 0), mask)


def flipper_shadow(img):
    """Darken the lower playfield towards the drain."""
    shade = layer()
    d = ImageDraw.Draw(shade)
    for i in range(40):
        t = i / 39
        y = V(10.0 + t * 4.5)
        d.rectangle((0, y, IW * SS, y + 0.12 * PX * SS), fill=(0, 0, 0, int(110 * t)))
    img.alpha_composite(shade)


def paint_picture(path):
    rnd = random.Random(20261103)
    img = concrete_base(rnd)
    saw_cuts(img)
    bowl(img, rnd)
    halfpipe(img)
    grind_ledge(img)

    d = ImageDraw.Draw(img, "RGBA")
    # Painted arrows at the main shots.
    stencil_arrow(d, 0.1, 1.1, -90, 0.6 * PX * SS, rgb(RED, 200))              # grind rail
    stencil_arrow(d, -6.6, 4.2, -90, 0.55 * PX * SS, rgb(MINT, 200))           # halfpipe
    # Lane marks in the top lanes.
    for x in (-2.0, 0.0, 2.0):
        d.rounded_rectangle((U(x) - 7, V(-12.9), U(x) + 7, V(-11.2)), radius=7, fill=(255, 255, 255, 70))

    # Tags and graffiti around the park.
    splatter(img, rnd, -4.2, 6.2, rgb(PINK), 0.5)
    splatter(img, rnd, 3.7, 5.9, rgb(MINT), 0.4)
    splatter(img, rnd, -5.0, -12.0, rgb(YELLOW), 0.3)
    stars(img, rnd, -4.6, 2.6, 0.32, rgb(YELLOW))
    stars(img, rnd, 4.9, 3.4, 0.24, rgb(PINK))
    stars(img, rnd, 3.6, -11.6, 0.2, rgb(MINT))

    # The skateboard and the name, across the lower playfield.
    skateboard(img, -0.6, 6.25, 4.4, 8)
    spray_tag(img, "SKATEPARK", -0.8, 4.35, 1.3, rgb(ORANGE), (24, 22, 26, 255), angle=6, rnd=rnd)
    spray_tag(img, "SKATE AT YOUR OWN RISK", -0.6, 7.45, 0.28, (236, 238, 240, 230), (24, 22, 26, 255),
              angle=6, drips=False)

    hazard_stripes(img)
    flipper_shadow(img)

    out = img.resize((IW, IH), Image.LANCZOS).convert("RGB")
    out.save(path, quality=90, optimize=True)


def main():
    table = build_table()
    with open(os.path.join(TABLES, NAME + ".pwtable"), "w", newline="\n") as f:
        json.dump(table, f, indent=2)
        f.write("\n")
    paint_picture(os.path.join(TABLES, NAME + ".jpg"))
    print("Wrote", NAME + ".pwtable", "and", NAME + ".jpg")


if __name__ == "__main__":
    main()
