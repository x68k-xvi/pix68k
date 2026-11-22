#!/usr/bin/env python3
"""Generate a small Human68k .X demonstrating GD5434 BitBLT on TS-6BGA.

Usage: python3 tools/ts6bga_blt_demo.py BLTDEMO.X
Run after TS6BGAG.X and a 1024x768 15-bit image (e.g. JPGEDGA -d2 -s0 -k -t TEST.JPG).
The demo fills a red rectangle, copies part of the picture, and overlays a transparent green checker.
It uses only 68000 instructions and GD5434 registers; no compiler, ROM or proprietary assets included.
"""
import struct
import sys
from pathlib import Path


def build():
    code = bytearray()

    def w(*values):
        for value in values:
            code.extend(struct.pack('>H', value))

    def poke(address, value):
        # MOVE.B #value,absolute.L (8 bytes)
        w(0x13fc, value & 255, address >> 16, address & 65535)

    def reg(index, value):
        poke(0xe9e3cf, index)
        poke(0xe9e3ce, value)

    def word(index, value):
        reg(index, value)
        reg(index + 1, value >> 8)

    def address(index, value):
        word(index, value)
        reg(index + 2, value >> 16)

    def blit(src, dst, width, height, mode):
        word(0x20, width - 1)
        word(0x22, height - 1)
        word(0x24, 2048)
        word(0x26, 2048)
        address(0x28, dst)
        address(0x2c, src)
        reg(0x30, mode)
        reg(0x32, 0x0d)  # SRCCOPY
        reg(0x31, 2)
        # MOVE.B ($E9E3CE).L,D0 / BTST #0,D0 / BNE.S to MOVE.B
        w(0x1039, 0x00e9, 0xe3ce, 0x0800, 0, 0x66f4)

    # CLR.L -(SP) / DOS _SUPER / MOVE.L D0,(SP); restore the old mode on exit.
    w(0x42a7, 0xff20, 0x2e80)
    reg(11, 0x20)
    reg(9, 0x60)  # 0x180000, outside visible 1024x768 RGB555 image
    for i in range(8):
        poke(0xee0000 + i, 255)
        # Native pattern row 0=AA,1=55: CPU's adjacent bytes are swapped.
        poke(0xee0008 + i, 0x55 if i % 2 == 0 else 0xaa)
    reg(1, 0)
    reg(0x11, 0x7c)  # RGB555 red, low/high byte
    blit(0x180000, 16 * 2048 + 16 * 2, 300 * 2, 80, 0xd0)
    blit(160 * 2048 + 256 * 2, 400 * 2048 + 32 * 2, 320 * 2, 200, 0x10)
    for index, value in ((1, 0xe0), (0x11, 3), (0x13, 0xe0), (0x15, 3),
                         (0, 0x1f), (0x10, 0xfc), (0x12, 0x1f), (0x14, 0xfc)):
        reg(index, value)
    blit(0x180008, 600 * 2048 + 650 * 2, 320 * 2, 100, 0xd8)
    w(0xff20, 0x588f, 0xff00)  # DOS _SUPER / ADDQ.L #4,SP / DOS _EXIT
    header = bytearray(64)
    header[:2] = b'HU'
    struct.pack_into('>I', header, 0x0c, len(code))
    return header + code


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    Path(sys.argv[1]).write_bytes(build())
