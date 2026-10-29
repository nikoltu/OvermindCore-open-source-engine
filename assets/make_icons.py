import os
import struct
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ICO_SIZES = [16, 24, 32, 48, 64, 128, 256]


def scaled(src, size):
    return src.resize((size, size), Image.LANCZOS)


def write_dds(path, img):
    w, h = img.size
    flags = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000
    pf = struct.pack("<II4sIIIII", 32, 0x41, b"\0\0\0\0", 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
    header = struct.pack("<4sIIIIIII", b"DDS ", 124, flags, h, w, w * 4, 0, 0) + b"\0" * 44 + pf
    header += struct.pack("<IIIII", 0x1000, 0, 0, 0, 0)
    r, g, b, a = img.split()
    bgra = Image.merge("RGBA", (b, g, r, a)).tobytes()
    with open(path, "wb") as f:
        f.write(header + bgra)


def main():
    src_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "icon.jpg")
    out = sys.argv[2] if len(sys.argv) > 2 else HERE
    src = Image.open(src_path).convert("RGBA")
    write_dds(os.path.join(out, "emblem.dds"), scaled(src, 384))
    write_dds(os.path.join(out, "emblem_sm.dds"), scaled(src, 160))
    src.save(os.path.join(out, "overmindcore.ico"), format="ICO", sizes=[(s, s) for s in ICO_SIZES])


if __name__ == "__main__":
    main()
