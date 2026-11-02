"""Make an HD graphics pack for Pinball Wizz from your own PINBALL.DAT.

The pack is written to an "HD" folder next to the .dat file. The game picks it up on the next start.
Nothing from the original game is included with this tool: it only reads the file you point it at.

Usage:
    make_hd_pack.py [path\\to\\PINBALL.DAT] [--scale 2|3|4] [--out folder]
"""
import argparse
import colorsys
import os
import struct
import sys
import time

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import art_fixes  # noqa: E402

PACK_VERSION = 1
CONTEXT = 8  # pixels of surrounding table art fed to the upscaler, so sprite edges blend in

# Small red logo lettering is too few pixels for any upscaler to rebuild cleanly, so it is redrawn:
# the old letters are painted over with the apron colour and the same words are drawn fresh in a
# vector font, fitted to exactly where the old letters were. Boxes are (left, top, right, bottom) in
# 1x pixels of the named bitmap, and only apply when the bitmap has the expected size (the Space Cadet
# table). Everything is drawn on the player's PC; nothing from the original art is shipped.
LOGO_TEXT = {
    "table": {
        "size": (365, 470),
        "labels": [
            ("CINEMATRONICS", (12, 391, 93, 405)),  # stops before the red trim on the right
            ("MAXIS", (283, 391, 322, 405)),         # stops before the plunger frame
        ],
    },
}
LOGO_FONT = "Inter-ExtraBoldItalic.otf"  # SIL Open Font License, see OFL.txt
LOGO_SATURATION = 0.85  # the original red is dull; push it towards the red trim on the table
LOGO_BRIGHTNESS = 0.75
LOGO_ERASE_FEATHER = 6  # 4x pixels


def resource_path(name):
    base = getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(base, name)


# --- PINBALL.DAT reader -------------------------------------------------------------------------

def read_dat(path):
    data = open(path, "rb").read()
    sig, _app, _desc, _fsize, ngroups, _body, _unk = struct.unpack_from("<21s50s100sIHIH", data, 0)
    if not sig.startswith(b"PARTOUT(4.0)RESOURCE"):
        raise ValueError("This doesn't look like a 3D Pinball .dat file")
    fixed = {0: 2, 2: 2}
    pos = 183
    groups = []
    for _ in range(ngroups):
        count = data[pos]
        pos += 1
        entries = []
        for _ in range(count):
            etype = data[pos]
            pos += 1
            size = fixed.get(etype)
            if size is None:
                size = struct.unpack_from("<I", data, pos)[0]
                pos += 4
            entries.append((etype, data[pos:pos + size]))
            pos += size
        groups.append(entries)
    return groups


def engine_palette(groups):
    """Same palette the game builds in gdrv::display_palette. Alpha 0 means transparent."""
    raw = next(b for g in groups for t, b in g if t == 5)
    pal = np.zeros((256, 4), np.uint8)
    system = [(0, 0, 0, 0), (0x80, 0, 0, 255), (0, 0x80, 0, 255), (0x80, 0x80, 0, 255), (0, 0, 0x80, 255),
              (0x80, 0, 0x80, 255), (0, 0x80, 0x80, 255), (0xC0, 0xC0, 0xC0, 255), (0xC0, 0xDC, 0xC0, 255),
              (0xA6, 0xCA, 0xF0, 255)]
    pal[:10] = system
    for i in range(10, 246):
        b, g, r = raw[i * 4], raw[i * 4 + 1], raw[i * 4 + 2]
        pal[i] = (r, g, b, 255)
    pal[255] = (255, 255, 255, 255)
    return pal


def bitmaps(groups, pal):
    """Yields (group_id, x, y, rgba array) for every 640x480 (resolution 0) 8-bit bitmap."""
    for gid, entries in enumerate(groups):
        for etype, buf in entries:
            if etype != 1:
                continue
            res, w, h, x, y, size, flags = struct.unpack_from("<BhhhhiB", buf, 0)
            if res != 0 or flags & 4 or w <= 0 or h <= 0:  # other resolutions / spliced bitmaps: skipped
                continue
            stride = size // h
            idx = np.frombuffer(buf, np.uint8, size, 14).reshape(h, stride)[::-1, :w]
            yield gid, x, y, pal[idx]


# --- Upscaler -----------------------------------------------------------------------------------

class Upscaler:
    def __init__(self, model_path):
        import onnxruntime as ort
        opts = ort.SessionOptions()
        self.session = ort.InferenceSession(model_path, opts, providers=["CPUExecutionProvider"])

    def __call__(self, rgb):
        x = rgb.astype(np.float32).transpose(2, 0, 1)[None] / 255.0
        y = self.session.run(None, {"input": x})[0][0]
        return (np.clip(y, 0, 1).transpose(1, 2, 0) * 255 + 0.5).astype(np.uint8)


def upscale_with_context(up, rgba, x, y, table_rgb):
    """Upscale a sprite 4x. Transparent pixels are filled from the table art underneath, and a margin
    of table art is included, so the AI sees the same surroundings the sprite is drawn on."""
    h, w = rgba.shape[:2]
    th, tw = table_rgb.shape[:2]
    x0, y0 = max(x - CONTEXT, 0), max(y - CONTEXT, 0)
    x1, y1 = min(x + w + CONTEXT, tw), min(y + h + CONTEXT, th)
    on_table = x >= 0 and y >= 0 and x + w <= tw and y + h <= th
    if on_table:
        canvas = table_rgb[y0:y1, x0:x1].copy()
        sub = canvas[y - y0:y - y0 + h, x - x0:x - x0 + w]
        opaque = rgba[..., 3] > 0
        sub[opaque] = rgba[..., :3][opaque]
        big = up(canvas)
        ox, oy = (x - x0) * 4, (y - y0) * 4
        return big[oy:oy + h * 4, ox:ox + w * 4]
    pad = np.pad(rgba[..., :3], ((CONTEXT, CONTEXT), (CONTEXT, CONTEXT), (0, 0)), mode="edge")
    big = up(pad)
    return big[CONTEXT * 4:(CONTEXT + h) * 4, CONTEXT * 4:(CONTEXT + w) * 4]


def logo_colour(pixels):
    """The letters' own hue, made cleaner and brighter."""
    r, g, b = np.percentile(pixels, 90, axis=0) / 255
    h, _, _ = colorsys.rgb_to_hsv(r, g, b)
    return np.array(colorsys.hsv_to_rgb(h, LOGO_SATURATION, LOGO_BRIGHTNESS), np.float32) * 255


def render_text(text, font_path, width, height):
    """Text as an alpha mask stretched to exactly width x height, rendered large then scaled down."""
    font = ImageFont.truetype(font_path, 240)
    left, top, right, bottom = font.getbbox(text)
    canvas = Image.new("L", (right - left + 40, bottom - top + 40), 0)
    ImageDraw.Draw(canvas).text((20 - left, 20 - top), text, font=font, fill=255)
    canvas = canvas.crop(canvas.getbbox()).resize((width, height), Image.LANCZOS)
    return np.asarray(canvas, np.float32)[..., None] / 255


def relabel(rgba, rgb4, spec, font_path):
    """Paint over the old logo letters and draw them again in a clean font."""
    h, w = rgba.shape[:2]
    if (w, h) != spec["size"]:
        return rgb4
    out = rgb4.astype(np.float32)
    for text, (left, top, right, bottom) in spec["labels"]:
        src = rgba[top:bottom, left:right, :3].astype(np.float32)
        red = src[..., 0] - np.maximum(src[..., 1], src[..., 2])
        letters, apron = red > 18, red <= 8
        if not letters.any() or not apron.any():
            continue
        ys, xs = np.nonzero(letters)
        x0, x1, y0, y1 = left + xs.min(), left + xs.max() + 1, top + ys.min(), top + ys.max() + 1

        # 1. Erase: fill the box with the apron colour, softly feathered at the edges.
        mask = np.zeros((h * 4, w * 4), np.float32)
        mask[top * 4:bottom * 4, left * 4:right * 4] = 1
        mask = np.asarray(Image.fromarray((mask * 255).astype(np.uint8))
                          .filter(ImageFilter.GaussianBlur(LOGO_ERASE_FEATHER)), np.float32)[..., None] / 255
        out = out * (1 - mask) + np.median(src[apron], axis=0) * mask

        # 2. Draw the word where the old letters were.
        alpha = render_text(text, font_path, (x1 - x0) * 4, (y1 - y0) * 4)
        region = out[y0 * 4:y1 * 4, x0 * 4:x1 * 4]
        out[y0 * 4:y1 * 4, x0 * 4:x1 * 4] = region * (1 - alpha) + logo_colour(src[letters]) * alpha
    return (out + 0.5).clip(0, 255).astype(np.uint8)


def build_table_canvas(items):
    """640x480 picture of the static table art, used as context for sprite edges."""
    canvas = np.zeros((480, 640, 3), np.uint8)
    for gid, x, y, rgba, name in items:
        if name in ("background", "table"):
            h, w = rgba.shape[:2]
            region = canvas[y:y + h, x:x + w]
            opaque = rgba[:h, :w, 3] > 0
            region[opaque] = rgba[..., :3][opaque]
    return canvas


def group_name(entries):
    for etype, buf in entries:
        if etype == 3:
            return buf.split(b"\0")[0].decode("latin-1")
    return ""


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("dat", nargs="?", help="path to PINBALL.DAT (default: next to this program)")
    parser.add_argument("--scale", type=int, default=4, choices=[2, 3, 4], help="HD scale, default 4")
    parser.add_argument("--out", help="output folder (default: HD folder next to the .dat file)")
    parser.add_argument("--model", default=resource_path("realesrgan_x4_anime6b.onnx"))
    parser.add_argument("--font", default=resource_path(LOGO_FONT), help="font for the redrawn logo text")
    parser.add_argument("--no-pause", action="store_true", help="don't wait for Enter at the end")
    args = parser.parse_args()

    dat = args.dat
    if not dat:
        here = os.path.dirname(os.path.abspath(sys.executable if getattr(sys, "frozen", False) else __file__))
        dat = os.path.join(here, "PINBALL.DAT")
    if not os.path.isfile(dat):
        print(f"Can't find {dat}. Put this program in your game folder, or drag PINBALL.DAT onto it.")
        return finish(args, 1)

    out = args.out or os.path.join(os.path.dirname(os.path.abspath(dat)), "HD")
    os.makedirs(out, exist_ok=True)

    groups = read_dat(dat)
    pal = engine_palette(groups)
    items = [(gid, x, y, rgba, group_name(groups[gid])) for gid, x, y, rgba in bitmaps(groups, pal)]
    table = build_table_canvas(items)
    up = Upscaler(args.model)
    font = args.font if os.path.isfile(args.font) else None
    if not font:
        print("Logo font not found, logo lettering will keep the upscaled original.")

    balls = art_fixes.ball_groups(items)
    plungers = art_fixes.plunger_groups(items)
    screws = art_fixes.find_screws(items)
    table_hd = None  # (x, y, 4x image) of the table art, for sprites redrawn on top of it

    start = time.time()
    for n, (gid, x, y, rgba, name) in enumerate(items, 1):
        h, w = rgba.shape[:2]
        s = args.scale
        if gid in balls:
            # Drawn from scratch as a chrome ball, with its own smooth round edge.
            img = Image.fromarray(art_fixes.render_chrome_ball(rgba, s), "RGBA")
        else:
            rgb4 = upscale_with_context(up, rgba, x, y, table)
            if font and name in LOGO_TEXT:
                rgb4 = relabel(rgba, rgb4, LOGO_TEXT[name], font)
            if name == "table":
                table_hd = (x, y, rgb4)
            if name in art_fixes.ARROW_LIGHTS and table_hd:
                tx, ty, t4 = table_hd
                under = t4[(y - ty) * 4:(y - ty + h) * 4, (x - tx) * 4:(x - tx + w) * 4]
                if under.shape[:2] == (h * 4, w * 4):
                    arrow = art_fixes.redraw_arrow(rgba, under)
                    if arrow is not None:
                        rgb4 = arrow
            if gid in plungers:
                rgb4 = art_fixes.redraw_spring(rgba, rgb4)
            rgb4 = art_fixes.stamp_screws(rgb4, screws, x, y, rgba)
            img = Image.fromarray(rgb4)
            if s != 4:
                img = img.resize((w * s, h * s), Image.LANCZOS)
            # Transparency follows the original pixel grid exactly, so the game's depth logic lines up.
            alpha = Image.fromarray(np.where(rgba[..., 3] > 0, 255, 0).astype(np.uint8)).resize((w * s, h * s), Image.NEAREST)
            img.putalpha(alpha)
        img.save(os.path.join(out, f"{gid:03d}.png"), optimize=False, compress_level=6)
        print(f"\r[{n}/{len(items)}] {name or gid}".ljust(60), end="", flush=True)

    with open(os.path.join(out, "pack.ini"), "w") as f:
        f.write(f"; Pinball Wizz HD pack, generated from {os.path.basename(dat)}\n")
        f.write(f"version={PACK_VERSION}\nscale={args.scale}\n")
    print(f"\nDone: {len(items)} images at {args.scale}x in {time.time() - start:.0f}s -> {out}")
    print("Start the game and it will use the HD graphics.")
    return finish(args, 0)


def finish(args, code):
    if getattr(sys, "frozen", False) and not args.no_pause:
        input("Press Enter to close.")
    return code


if __name__ == "__main__":
    sys.exit(main())
