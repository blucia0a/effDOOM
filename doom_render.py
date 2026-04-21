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
    <len bytes>               12-bit fixed-width LZW of W*H palette indices

LZW encoding:
    12-bit codes packed LSB-first into bytes (GIF bit order)
    Code 256 = CLEAR (reset dictionary), 257 = EOI
    Codes 258-4095 = dictionary entries built during encoding
    Dictionary resets automatically when full (4096 codes)

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
# LZW decompressor (matches lzw_encode() in main_e1x.c)
# ---------------------------------------------------------------------------

def lzw_decode(data, expected_size):
    LZW_CLEAR = 256
    LZW_EOI   = 257
    LZW_MAX   = 4096

    # Variable-width bit reader: starts at 9 bits, grows to 12
    bit_buf   = 0
    bit_cnt   = 0
    pos       = 0
    code_bits = 9

    def read_code():
        nonlocal bit_buf, bit_cnt, pos
        while bit_cnt < code_bits and pos < len(data):
            bit_buf |= data[pos] << bit_cnt
            bit_cnt += 8
            pos     += 1
        code     = bit_buf & ((1 << code_bits) - 1)
        bit_buf >>= code_bits
        bit_cnt  -= code_bits
        return code

    def make_table():
        return [bytes([i]) for i in range(256)] + [None, None]

    table = make_table()
    out   = bytearray()
    prev  = None

    while True:
        code = read_code()
        if code == LZW_CLEAR:
            table     = make_table()
            prev      = None
            code_bits = 9
            continue
        if code == LZW_EOI:
            break
        if code < len(table) and table[code] is not None:
            entry = table[code]
        elif code == len(table) and prev is not None:
            entry = prev + prev[:1]   # KwKwK special case
        else:
            raise ValueError(f'LZW bad code {code} (table size {len(table)})')
        out += entry
        if prev is not None and len(table) < LZW_MAX:
            table.append(prev + entry[:1])
            if len(table) == (1 << code_bits) - 1 and code_bits < 12:
                code_bits += 1
        prev = entry

    if len(out) != expected_size:
        raise ValueError(f'LZW decode: got {len(out)} bytes, expected {expected_size}')
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
            raw          = lzw_decode(compressed, w * h)
            # Undo horizontal prediction filter
            if HAS_NP:
                raw = np.cumsum(
                    np.frombuffer(raw, dtype=np.uint8).reshape(h, w),
                    axis=1, dtype=np.uint8).tobytes()
            else:
                arr = bytearray(raw)
                for row in range(h):
                    for x in range(1, w):
                        arr[row*w + x] = (arr[row*w + x] + arr[row*w + x-1]) & 0xFF
                raw = bytes(arr)
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
