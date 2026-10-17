#!/usr/bin/env python3
"""Generate the original 512-primitive enhanced GL/Vulkan comparison fixture.

Only procedural artwork and original PS-X instructions are included. The
fixed seed and RNG consumption preserve the previously measured executable.
"""
import argparse
import random
import struct
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    words = [0x3c081f80, 0x35081810]  # t0 = GP0

    def write(value, port=0):
        words.extend([0x3c090000 | (value >> 16), 0x35290000 | (value & 65535),
            0xad090000 | port])

    def packet(values):
        words.extend([0x3c0a0400, 0x8d0b0004, 0, 0x016a5824, 0x1160fffc, 0])
        for value in values:
            write(value)

    def xy(x, y):
        return (x & 65535) | ((y & 65535) << 16)

    for value in [0, 0x08000001, 0x06c60260, 0x0703c010, 0x03000000]:
        write(value, 4)
    packet([0xe3000000, 0xe407ffff, 0xe5000000, 0xe6000000, 0xe100011c])
    packet([0x024892d8, 0, xy(1023, 511)])
    texture = [1 | (x & 31) | ((y & 31) << 5) | (((x*11+y*7) & 31) << 10) |
        (32768 if (x+y) & 1 else 0) for y in range(256) for x in range(256)]
    packet([0xa0000000, xy(768, 256), xy(256, 256)] +
        [texture[i] | (texture[i+1] << 16) for i in range(0, len(texture), 2)])

    rng = random.Random(10112026)
    for i in range(512):
        x, y = (i % 32)*16, (i // 32)*16
        page = 0x11c | (((i >> 3) & 3) << 5) | ((i & 1) << 9)
        packet([0xe1000000 | page])
        packet([0xe2000000 if i % 5 else 0xe2008421])
        semi_transparent = 0x02000000 if i & 16 else 0
        if i % 4 == 0:
            u = rng.choice([0, 1, 128, 249, 255])
            v = rng.choice([0, 1, 128, 249, 255])
            packet([0x64808080 | semi_transparent, xy(x+1, y+1),
                u | (v << 8), xy(13, 13)])
            continue

        positions = [(x+1, y+1), (x+14, y+1), (x+1, y+14)]
        if i % 3 == 0:
            positions = [(x+1, y+2), (x+14, y+1), (x+3, y+13)]
        low = rng.choice([0, 1, 80, 128, 230])
        high = min(low + rng.choice([1, 7, 13, 25]), 255)
        u = [high, low, high] if i & 1 else [low, high, low]
        v = [high, high, low] if i & 2 else [low, low, high]
        kind = (i // 4) % 3
        gouraud = kind == 2
        command = (0x34000000 if gouraud else 0x25000000 if kind == 1
            else 0x24000000) | semi_transparent
        # The historical fixture advanced the color RNG four times even
        # though only three vertices are drawn; retain its exact input.
        colors = [rng.randrange(1 << 24) for _ in range(4)] if gouraud else [0x808080]*4
        packet_words = []
        for vertex in range(3):
            if vertex == 0 or gouraud:
                packet_words.append((command if vertex == 0 else 0) | colors[vertex])
            packet_words.extend([xy(*positions[vertex]), u[vertex] | (v[vertex] << 8) |
                (page << 16 if vertex == 1 else 0)])
        packet(packet_words)

    idle = 0x80010000 + len(words)*4
    words.extend([0x08000000 | ((idle >> 2) & 0x3ffffff), 0])
    program = struct.pack('<'+'I'*len(words), *words)
    program += b'\0' * (-len(program) % 2048)
    assert len(program) < 0x1e0000
    header = bytearray(2048)
    header[:8] = b'PS-X EXE'
    for offset, value in [(0x10, 0x80010000), (0x18, 0x80010000),
            (0x1c, len(program)), (0x30, 0x801fff00)]:
        struct.pack_into('<I', header, offset, value)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(header+program)


if __name__ == '__main__':
    main()
