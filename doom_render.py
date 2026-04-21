#!/usr/bin/env python3
"""
doom_render.py — display DOOM frames streamed from the E1x over UART

Usage:
    python3 doom_render.py <wad> <port>    # live serial
    python3 doom_render.py <wad> -         # stdin / pipe

    <wad>   path to DOOM WAD (for PLAYPAL palette extraction)
    <port>  serial device, e.g. /dev/ttyUSB0 or COM3

Wire format (produced by dump_frame() in main_e1x.c):
    0xDE 0xAD 0xBE 0xEF       4-byte sync marker
    W_lo W_hi H_lo H_hi       frame size, uint16 little-endian
    len_lo len_hi             compressed payload length, uint16 little-endian
    <len bytes>               PackBits RLE of W*H palette indices

RLE encoding:
    header byte b < 0x80  →  b+1 literal bytes follow
    header byte b >= 0x80 →  b-126 copies of the next byte

The sync marker bytes (0xDE 0xAD 0xBE 0xEF) are never generated as RLE
headers (0xDE → 96-copy run; 0xAD → 51-copy run; etc.) unless followed by
the exact continuation pattern, making false syncs statistically negligible.

Requires:
    pip install pygame pyserial
    pip install numpy          # optional, speeds up palette mapping
"""

import sys
import struct

SYNC   = b'\xde\xad\xbe\xef'
SCALE  = 4   # window upscale factor (160*4=640 x 120*4=480)
BAUD   = 115200


# ---------------------------------------------------------------------------
# WAD palette loader
# ---------------------------------------------------------------------------

def load_palette(wad_path):
    """Return a list of 256 (R,G,B) tuples from the first PLAYPAL in the WAD."""
    with open(wad_path, 'rb') as f:
        magic = f.read(4)
        if magic not in (b'IWAD', b'PWAD'):
            raise ValueError(f'{wad_path} is not a WAD file')
        numlumps, dirofs = struct.unpack('<II', f.read(8))
        f.seek(dirofs)
        for _ in range(numlumps):
            filepos, size = struct.unpack('<II', f.read(8))
            name = f.read(8).split(b'\x00')[0].decode('ascii', errors='replace')
            if name == 'PLAYPAL':
                f.seek(filepos)
                raw = f.read(768)   # first of 14 palettes, 256*3 bytes
                return [(raw[i*3], raw[i*3+1], raw[i*3+2]) for i in range(256)]
    print('WARNING: PLAYPAL not found in WAD, using greyscale', file=sys.stderr)
    return [(i, i, i) for i in range(256)]


# ---------------------------------------------------------------------------
# RLE decompressor (matches rle_encode() in main_e1x.c)
# ---------------------------------------------------------------------------

def rle_decode(data, expected_size):
    out = bytearray()
    i = 0
    while i < len(data):
        b = data[i]; i += 1
        if b >= 0x80:
            count = b - 126          # 0x80 → 2 copies, 0xFE → 128 copies
            val   = data[i]; i += 1
            out += bytes([val]) * count
        else:
            count = b + 1            # 0x00 → 1 literal, 0x7F → 128 literals
            out += data[i:i + count]
            i += count
    if len(out) != expected_size:
        raise ValueError(f'RLE decode: got {len(out)} bytes, expected {expected_size}')
    return bytes(out)


# ---------------------------------------------------------------------------
# Stream helpers
# ---------------------------------------------------------------------------

def read_exactly(stream, n):
    buf = b''
    while len(buf) < n:
        chunk = stream.read(n - len(buf))
        if not chunk:
            raise EOFError('stream ended')
        buf += chunk
    return buf


def find_sync(stream):
    """Consume bytes until the 4-byte SYNC marker is aligned."""
    window = bytearray(4)
    while bytes(window) != SYNC:
        b = stream.read(1)
        if not b:
            raise EOFError('stream ended')
        window = window[1:] + bytearray(b)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)

    wad_path = sys.argv[1]
    port     = sys.argv[2]

    palette = load_palette(wad_path)

    # numpy palette array for fast vectorised lookup (optional)
    try:
        import numpy as np
        pal_np = np.array(palette, dtype=np.uint8)   # shape (256, 3)
        HAS_NP = True
    except ImportError:
        HAS_NP = False
        print('numpy not found — falling back to slow per-pixel loop',
              file=sys.stderr)

    import pygame

    if port == '-':
        stream = sys.stdin.buffer
    else:
        import serial
        stream = serial.Serial(port, BAUD, timeout=10)

    pygame.init()
    win        = None
    clock      = pygame.time.Clock()
    frame_num  = 0

    try:
        while True:
            # handle pygame quit at the top of every iteration
            for evt in pygame.event.get():
                if evt.type == pygame.QUIT:
                    return

            # locate next frame
            find_sync(stream)
            hdr          = read_exactly(stream, 6)   # w, h, compressed_len
            w, h, clen   = struct.unpack('<HHH', hdr)
            compressed   = read_exactly(stream, clen)
            raw          = rle_decode(compressed, w * h)
            frame_num   += 1

            # (re)create window if dimensions changed
            win_w, win_h = w * SCALE, h * SCALE
            if win is None or win.get_size() != (win_w, win_h):
                win = pygame.display.set_mode((win_w, win_h))

            pygame.display.set_caption(
                f'DOOM E1x  frame {frame_num}  ({w}x{h})')

            # map palette indices → RGB surface
            if HAS_NP:
                indices = np.frombuffer(raw, dtype=np.uint8).reshape(h, w)
                rgb     = pal_np[indices]                    # (h, w, 3)
                # pygame surfarray expects (w, h, 3) with x-major order
                surf    = pygame.surfarray.make_surface(rgb.swapaxes(0, 1))
            else:
                surf = pygame.Surface((w, h))
                for y in range(h):
                    row = y * w
                    for x in range(w):
                        surf.set_at((x, y), palette[raw[row + x]])

            scaled = pygame.transform.scale(surf, (win_w, win_h))
            win.blit(scaled, (0, 0))
            pygame.display.flip()
            clock.tick(60)

    except EOFError:
        print('Stream ended.', file=sys.stderr)
    except KeyboardInterrupt:
        pass
    finally:
        pygame.quit()
        if port != '-' and hasattr(stream, 'close'):
            stream.close()


if __name__ == '__main__':
    main()
