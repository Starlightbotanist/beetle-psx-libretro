#!/usr/bin/env python3
"""Generate original PS-X test content: flipped triangles and a pending quad.

The program uploads a procedural texture, draws triangles/sprites, then sends
only the first three vertices of a textured quad. A RAM marker at 0x800f0000
is 1 during a long CPU delay and 2 after its fourth vertex and a cache-clearing
triangle are sent. A frontend can save at marker 1, finish, and restore that
state to check incomplete-command UV state. No game or BIOS data is included.
"""
import argparse
import struct
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--pending-only', action='store_true', help='Omit completed gallery draws before the pending command')
    parser.add_argument('--one-pixel', action='store_true')
    parser.add_argument('--complete-uv', action='store_true', help='Noncollapsed second UV triangle control')
    args = parser.parse_args()
    words = [0x3c081f80, 0x35081810]  # t0 = GP0

    def write(value, port=0):
        words.extend([0x3c090000 | (value >> 16), 0x35290000 | (value & 65535), 0xad090000 | port])

    def packet(values):
        # Wait for FIFO room before a packet.
        words.extend([0x3c0a0400, 0x8d0b0004, 0, 0x016a5824, 0x1160fffc, 0])
        for value in values:
            write(value)

    def xy(x, y):
        return (x & 65535) | ((y & 65535) << 16)

    def marker(value):
        words.extend([0x3c0c800f, 0x240d0000 | value, 0xad8d0000])

    for value in [0, 0x08000001, 0x06c60260, 0x0703c010, 0x03000000]:
        write(value, 4)
    page = 0x11c
    packet([0xe3000000, 0xe407ffff, 0xe5000000, 0xe6000000, 0xe1000000 | page])
    packet([0x024892d8, 0, xy(1023, 511)])
    texture = [1 | (x & 31) | ((y & 31) << 5) | (((x*11+y*7) & 31) << 10)
        for y in range(32) for x in range(32)]
    packet([0xa0000000, xy(768, 256), xy(32, 32)] +
        [texture[i] | (texture[i+1] << 16) for i in range(0, len(texture), 2)])
    # Four UV orientations, with and without a texture window.
    for window in range(0 if args.pending_only else 2):
        for flip in range(4):
            x, y = 16+flip*72, 16+window*40
            packet([0xe2008421 if window else 0xe2000000])
            u = [31, 0, 31] if flip & 1 else [0, 31, 0]
            v = [31, 31, 0] if flip & 2 else [0, 0, 31]
            packet([0x25808080, xy(x, y), u[0] | (v[0] << 8),
                xy(x+32, y), u[1] | (v[1] << 8) | (page << 16),
                xy(x, y+32), u[2] | (v[2] << 8)])
    packet([0xe2000000])
    # The second row also has genuine rectangle commands as controls.
    if not args.pending_only:
        packet([0x65808080, xy(16, 108), 0, xy(32, 32)])
    height = 1 if args.one_pixel else 32
    positions = [(96, 160), (160, 160), (96, 160+height), (160, 160+height)]
    uv = [(31, 31), (0, 31), (31, 0), (0, 0)]
    if args.one_pixel:
        order = [2, 3, 0, 1]
        positions = [positions[i] for i in order]
        uv = [uv[i] for i in order]
    if not args.complete_uv:
        uv[3] = uv[2]
    packet([0x2d808080, xy(*positions[0]), uv[0][0] | (uv[0][1] << 8),
        xy(*positions[1]), uv[1][0] | (uv[1][1] << 8) | (page << 16),
        xy(*positions[2]), uv[2][0] | (uv[2][1] << 8)])
    marker(1)
    # Six million instructions leave ample time to save the pending command.
    words.extend([0x3c0e001e, 0x35ce8480, 0x25ceffff, 0x15c0fffe, 0])
    # This continues the pending command. DMA-block-ready polling is only
    # valid before a fresh command; polling here would wait on our own data.
    write(xy(*positions[3]))
    write(uv[3][0] | (uv[3][1] << 8))
    packet([0x25808080, xy(500, 200), 0, xy(520, 200), page << 16, xy(500, 210), 0])
    marker(2)
    idle = 0x80010000 + len(words)*4
    words.extend([0x08000000 | ((idle >> 2) & 0x3ffffff), 0])
    text = struct.pack('<'+'I'*len(words), *words)
    text += b'\0' * (-len(text) % 2048)
    header = bytearray(2048)
    header[:8] = b'PS-X EXE'
    for offset, value in [(0x10, 0x80010000), (0x18, 0x80010000),
            (0x1c, len(text)), (0x30, 0x801fff00)]:
        struct.pack_into('<I', header, offset, value)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(header+text)


if __name__ == '__main__':
    main()
