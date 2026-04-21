#include "DOOM.h"
#include <eff/mtimer.h>
#include <eff/drivers/uart.h>

/* Linker-provided heap bounds (also declared in i_system.c) */
extern char _end[];
extern char __llvm_libc_heap_limit[];

/* WAD data embedded as rodata via llvm-objcopy from newdoom1_1lev.wad */
extern char _binary_newdoom1_1lev_wad_start[];
extern char _binary_newdoom1_1lev_wad_end[];
#define DOOM_WAD_DATA ((const unsigned char *)_binary_newdoom1_1lev_wad_start)
#define DOOM_WAD_SIZE ((int)(_binary_newdoom1_1lev_wad_end - _binary_newdoom1_1lev_wad_start))

/* --- In-memory WAD file I/O --- */

typedef struct {
    const unsigned char *data;
    int size;
    int pos;
} mem_file_t;

static mem_file_t wad_handle;

static void *e1x_open(const char *filename, const char *mode)
{
    /* Only serve the embedded WAD for "doom.wad" so that D_IdentifyVersion
     * sets gamemode = registered (E1Mx level naming). Rejecting doom2.wad,
     * doomu.wad, etc. prevents commercial mode (MAP01 naming) which would
     * cause W_GetNumForName("map01") to fail on our DOOM1 WAD. */
    const char *p = filename;
    while (*p) p++;
    if ((p - filename) >= 8) {
        const char *t = p - 8;
        if ((t[0]=='d'||t[0]=='D') && (t[1]=='o'||t[1]=='O') &&
            (t[2]=='o'||t[2]=='O') && (t[3]=='m'||t[3]=='M') &&
             t[4]=='.'             &&
            (t[5]=='w'||t[5]=='W') && (t[6]=='a'||t[6]=='A') &&
            (t[7]=='d'||t[7]=='D')) {
            wad_handle.data = DOOM_WAD_DATA;
            wad_handle.size = DOOM_WAD_SIZE;
            wad_handle.pos  = 0;
            return &wad_handle;
        }
    }
    return 0;
}

static void e1x_close(void *handle) {}

static int e1x_read(void *handle, void *buf, int count)
{
    mem_file_t *f = (mem_file_t *)handle;
    int avail = f->size - f->pos;
    if (count > avail) count = avail;
    unsigned char *dst = (unsigned char *)buf;
    const unsigned char *src = f->data + f->pos;
    for (int i = 0; i < count; i++) dst[i] = src[i];
    f->pos += count;
    return count;
}

static int e1x_write(void *handle, const void *buf, int count) { return 0; }

static int e1x_seek(void *handle, int offset, doom_seek_t origin)
{
    mem_file_t *f = (mem_file_t *)handle;
    int pos;
    if      (origin == DOOM_SEEK_SET) pos = offset;
    else if (origin == DOOM_SEEK_CUR) pos = f->pos + offset;
    else                               pos = f->size + offset;
    if (pos < 0)       pos = 0;
    if (pos > f->size) pos = f->size;
    f->pos = pos;
    return 0;
}

static int e1x_tell(void *handle) { return ((mem_file_t *)handle)->pos; }

static int e1x_eof(void *handle)
{
    mem_file_t *f = (mem_file_t *)handle;
    return f->pos >= f->size;
}

/* --- Timing via MTIMER --- */

static void e1x_gettime(int *sec, int *usec)
{
    eff_mtimer_ticks_t us = eff_mtimer_uptime_us();
    *sec  = (int)(us / 1000000ULL);
    *usec = (int)(us % 1000000ULL);
}

/* --- Print via UART --- */

static void e1x_print(const char *str) { (void)str; }

/* --- Hex diagnostics: print a 32-bit value as 0xXXXXXXXX over UART --- */

static void uart_hex32(unsigned int val)
{
    char buf[11];
    int i;
    buf[0] = '0'; buf[1] = 'x';
    for (i = 0; i < 8; i++) {
        int n = (val >> (28 - i * 4)) & 0xF;
        buf[2 + i] = n < 10 ? '0' + n : 'a' + n - 10;
    }
    buf[10] = '\0';
    eff_uart_puts(STDIO_UART, buf);
}

/* --- Null getenv: no environment on E1x --- */

static char *e1x_getenv(const char *var) { return 0; }

/* --- Exit handler: halt instead of calling OS exit() ---------------------
 * On bare metal, exit() issues an ecall → UNHANDLED TRAP that loops.
 * I_Error calls doom_print (visible on UART) then doom_exit; we halt so
 * the error message is the last thing visible before the hang. */
static void e1x_exit(int code)
{
    eff_uart_puts(STDIO_UART, "\r\n[doom] HALTED\r\n");
    for (;;) {}
}

/* --- Override the weak except_handler from eff_sdk/stdlib/arch/e1x/trap.c
 * to print mcause and mepc over UART before halting, so we know exactly
 * what kind of trap is occurring (misaligned access, store fault, etc.). */
long except_handler(long cause, long epc)
{
    eff_uart_puts(STDIO_UART, "\r\n[doom] TRAP mcause=");
    uart_hex32((unsigned int)cause);
    eff_uart_puts(STDIO_UART, " mepc=");
    uart_hex32((unsigned int)epc);
    eff_uart_puts(STDIO_UART, "\r\n");
    for (;;) {}
    return epc;
}

/* --- 12-bit fixed-width LZW encoder -------------------------------------
 *
 * Dictionary is an open-addressing hash table mapping (prefix<<8|byte) to
 * a 12-bit code.  Codes 0-255 = literals, 256 = CLEAR, 257 = EOI,
 * 258-4095 = dictionary entries.  Codes are packed LSB-first into bytes
 * (same bit order as GIF).  When the dictionary fills (4096 codes) a CLEAR
 * is emitted and the dictionary resets — guaranteeing the output never
 * exceeds the input by more than a small constant per segment.
 *
 * Worst-case output: each 3838-code segment encodes ≥1 byte each, packed
 * at 1.5 bytes/code → ≤28800 bytes for 19200 input bytes.  LZW_BUF_MAX
 * is sized to 32768 to give comfortable headroom.
 */
#define LZW_CLEAR   256u
#define LZW_EOI     257u
#define LZW_FIRST   258u
#define LZW_MAX     4096u
#define LZW_HTAB    5003            /* prime > LZW_MAX */
#define LZW_EMPTY   0xFFFFFFFFu

static unsigned int   lzw_key[LZW_HTAB];
static unsigned short lzw_val[LZW_HTAB];

static void lzw_htclear(void)
{
    for (int i = 0; i < LZW_HTAB; i++) lzw_key[i] = LZW_EMPTY;
}

static int lzw_htfind(unsigned int k)
{
    int h = (int)((k * 0x9e3779b9u) >> 19) % LZW_HTAB;
    while (lzw_key[h] != LZW_EMPTY && lzw_key[h] != k)
        if (++h == LZW_HTAB) h = 0;
    return h;
}

static int lzw_encode(const unsigned char *src, int n,
                      unsigned char *dst, int cap)
{
    unsigned int bit_buf = 0;
    int bit_cnt = 0, di = 0, next_code = LZW_FIRST;
    int code_bits = 9;                    /* variable-width: start at 9 bits */

#define EMIT(c) do {                                         \
    bit_buf |= (unsigned int)(c) << bit_cnt;                 \
    bit_cnt += code_bits;                                    \
    while (bit_cnt >= 8) {                                   \
        if (di >= cap) return -1;                            \
        dst[di++] = (unsigned char)(bit_buf & 0xFF);        \
        bit_buf >>= 8; bit_cnt -= 8;                        \
    }                                                        \
} while (0)

    lzw_htclear();
    EMIT(LZW_CLEAR);

    if (n == 0) { EMIT(LZW_EOI); goto flush; }

    unsigned int prefix = src[0];
    for (int si = 1; si < n; si++) {
        unsigned int c   = src[si];
        unsigned int key = (prefix << 8) | c;
        int h = lzw_htfind(key);
        if (lzw_key[h] == key) {
            prefix = lzw_val[h];
        } else {
            EMIT(prefix);
            if (next_code < LZW_MAX) {
                lzw_key[h] = key;
                lzw_val[h] = (unsigned short)next_code++;
                /* widen code after filling current range */
                if (next_code == (unsigned)(1 << code_bits) && code_bits < 12)
                    code_bits++;
            } else {
                EMIT(LZW_CLEAR);
                lzw_htclear();
                next_code = LZW_FIRST;
                code_bits = 9;
            }
            prefix = c;
        }
    }
    EMIT(prefix);
    EMIT(LZW_EOI);

flush:
    if (bit_cnt > 0) {
        if (di >= cap) return -1;
        dst[di++] = (unsigned char)(bit_buf & 0xFF);
    }
#undef EMIT
    return di;
}

/* --- Frame dump: LZW-compressed pixel stream over UART ------------------
 *
 * Wire format:
 *   [0xDE 0xAD 0xBE 0xEF]   4-byte sync marker
 *   [W_lo W_hi H_lo H_hi]   frame dimensions, uint16 LE
 *   [len_lo len_hi]          compressed payload length, uint16 LE
 *   [compressed bytes]       12-bit fixed-width LZW of W×H palette indices
 */
#define DOOM_W      320
#define DOOM_H      200
#define OUT_W       120
#define OUT_H       90
#define FRAME_SKIP    2

#define LZW_BUF_MAX  32768

static unsigned char frame_raw[OUT_W * OUT_H];
static unsigned char frame_lzw[LZW_BUF_MAX];

static void dump_frame(void)
{
    const unsigned char *fb = doom_get_framebuffer(1);
    int i = 0;

    /* Downsample 320×200 → OUT_W×OUT_H (nearest-neighbour) */
    for (int y = 0; y < OUT_H; y++) {
        int fy = (y * DOOM_H) / OUT_H;
        for (int x = 0; x < OUT_W; x++) {
            int fx = (x * DOOM_W) / OUT_W;
            frame_raw[i++] = fb[fy * DOOM_W + fx];
        }
    }

    /* Horizontal prediction filter (PNG "Sub"): replace each pixel with
     * pixel - left_neighbor.  Solid regions become zero-runs; LZW then
     * builds long zero-sequence codes rapidly.  Process right-to-left so
     * the in-place subtraction reads original values. */
    for (int y = 0; y < OUT_H; y++)
        for (int x = OUT_W - 1; x >= 1; x--)
            frame_raw[y * OUT_W + x] -= frame_raw[y * OUT_W + x - 1];

    int clen = lzw_encode(frame_raw, OUT_W * OUT_H, frame_lzw, LZW_BUF_MAX);
    if (clen < 0) return;

    /* sync marker */
    eff_uart_putc(STDIO_UART, 0xDE);
    eff_uart_putc(STDIO_UART, 0xAD);
    eff_uart_putc(STDIO_UART, 0xBE);
    eff_uart_putc(STDIO_UART, 0xEF);
    /* dimensions (uint16 LE) */
    eff_uart_putc(STDIO_UART, OUT_W & 0xFF);
    eff_uart_putc(STDIO_UART, (OUT_W >> 8) & 0xFF);
    eff_uart_putc(STDIO_UART, OUT_H & 0xFF);
    eff_uart_putc(STDIO_UART, (OUT_H >> 8) & 0xFF);
    /* compressed length (uint16 LE) */
    eff_uart_putc(STDIO_UART, clen & 0xFF);
    eff_uart_putc(STDIO_UART, (clen >> 8) & 0xFF);
    /* compressed payload */
    for (int j = 0; j < clen; j++)
        eff_uart_putc(STDIO_UART, frame_lzw[j]);
}

int main(void)
{
    eff_uart_cfg_t ucfg = EFF_UART_DEFAULTS;
    ucfg.baud = 460800;
    eff_uart_init(STDIO_UART, ucfg);

    doom_set_print(e1x_print);
    doom_set_exit(e1x_exit);
    doom_set_getenv(e1x_getenv);
    doom_set_file_io(e1x_open, e1x_close, e1x_read, e1x_write,
                     e1x_seek, e1x_tell, e1x_eof);
    doom_set_gettime(e1x_gettime);
    /* malloc: use DOOM_IMPLEMENT_MALLOC (C library malloc/free) which draws
     * from the linker-defined heap (_end → __llvm_libc_heap_limit). */

    static char *argv[] = { "doom", "-nosound", 0 };
    doom_init(2, argv,
              DOOM_FLAG_HIDE_MOUSE_OPTIONS |
              DOOM_FLAG_HIDE_SOUND_OPTIONS  |
              DOOM_FLAG_HIDE_MUSIC_OPTIONS);

    int tick = 0;
    for (;;) {
        doom_force_update();
        if (++tick >= FRAME_SKIP) {
            tick = 0;
            dump_frame();
        }
    }
}
