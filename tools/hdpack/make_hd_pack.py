"""Make an HD graphics pack for Pinball Wizz from your own PINBALL.DAT.

The pack is written to an "HD" folder next to the .dat file. The game picks it up on the next start.
Nothing from the original game is included with this tool: it only reads the file you point it at.

Usage:
    make_hd_pack.py [path\\to\\PINBALL.DAT] [--scale 2|3|4] [--out folder]
"""
import argparse
import os
import struct
import sys
import time

import numpy as np
from PIL import Image

PACK_VERSION = 1
CONTEXT = 8  # pixels of surrounding table art fed to the upscaler, so sprite edges blend in


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

    start = time.time()
    for n, (gid, x, y, rgba, name) in enumerate(items, 1):
        h, w = rgba.shape[:2]
        rgb4 = upscale_with_context(up, rgba, x, y, table)
        img = Image.fromarray(rgb4)
        s = args.scale
        if s != 4:
            img = img.resize((w * s, h * s), Image.LANCZOS)
        # Transparency follows the original pixel grid exactly, so the game's depth logic still lines up.
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
