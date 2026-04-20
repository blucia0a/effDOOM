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

static void e1x_print(const char *str)
{
    eff_uart_puts(STDIO_UART, str);
}

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

/* --- PackBits-style RLE encoder -----------------------------------------
 * Header byte meaning:
 *   0x00..0x7F  →  (b + 1) literal bytes follow           (1..128)
 *   0x80..0xFF  →  (b - 126) copies of the next byte      (2..129)
 * Worst case: ceil(n/128) * 129 bytes (all-unique input with 128-byte
 * literal batching) — about 1% overhead, never more than ~1.008 × n. */
static int rle_encode(const unsigned char *src, int n,
                      unsigned char *dst, int cap)
{
    int si = 0, di = 0;
    while (si < n) {
        /* Count run of identical bytes (max 128) */
        int run = 1;
        while (si + run < n && run < 128 && src[si + run] == src[si])
            run++;

        if (run >= 2) {
            if (di + 2 > cap) return -1;
            dst[di++] = (unsigned char)(run + 126);   /* 0x80..0xFE */
            dst[di++] = src[si];
            si += run;
        } else {
            /* Accumulate literals, stopping before any run of >=2 */
            int litstart = si;
            int litcount = 0;
            while (si < n && litcount < 128) {
                if (si + 1 < n && src[si] == src[si + 1]) break;
                si++;
                litcount++;
            }
            if (litcount == 0) { si++; litcount = 1; } /* lone byte before long run */
            if (di + 1 + litcount > cap) return -1;
            dst[di++] = (unsigned char)(litcount - 1);  /* 0x00..0x7F */
            for (int i = 0; i < litcount; i++)
                dst[di++] = src[litstart + i];
        }
    }
    return di;
}

/* --- Frame dump: delta-compressed pixel stream over UART ----------------
 *
 * Wire format:
 *   [0xDE 0xAD 0xBE 0xEF]   4-byte sync marker
 *   [W_lo W_hi H_lo H_hi]   frame dimensions, uint16 LE
 *   [flags]                  0x00 = keyframe, 0x01 = XOR delta frame
 *   [len_lo len_hi]          compressed payload length, uint16 LE
 *   [compressed bytes]       PackBits RLE of keyframe pixels or XOR delta
 *
 * Every KEYFRAME_INTERVAL frames a full keyframe is sent; the rest are XOR
 * deltas against the previous frame.  Static regions XOR to 0x00 and
 * compress as long zero-runs (128 zeros → 2 bytes), cutting typical
 * gameplay frames from ~5500 B to ~800–2000 B and enabling FRAME_SKIP=10
 * (~3.5 fps) within the 11520 B/s budget at 115200 baud.
 */
#define DOOM_W            320
#define DOOM_H            200
#define OUT_W             160
#define OUT_H             120
#define FRAME_SKIP         10
#define KEYFRAME_INTERVAL  30

/* worst-case RLE: ceil(OUT_W*OUT_H / 128) * 129 = 150*129 = 19350 bytes */
#define RLE_BUF_MAX  20480

static unsigned char frame_raw[OUT_W * OUT_H];
static unsigned char frame_rle[RLE_BUF_MAX];
static unsigned char prev_frame[OUT_W * OUT_H];
static int           frame_count;

static void dump_frame(void)
{
    const unsigned char *fb = doom_get_framebuffer(1);
    int i = 0;
    int is_keyframe = (frame_count % KEYFRAME_INTERVAL == 0);

    /* Downsample 320×200 → OUT_W×OUT_H (nearest-neighbour) */
    for (int y = 0; y < OUT_H; y++) {
        int fy = (y * DOOM_H) / OUT_H;
        for (int x = 0; x < OUT_W; x++) {
            int fx = (x * DOOM_W) / OUT_W;
            frame_raw[i++] = fb[fy * DOOM_W + fx];
        }
    }

    if (is_keyframe) {
        /* Save raw pixels for next delta */
        for (int j = 0; j < OUT_W * OUT_H; j++)
            prev_frame[j] = frame_raw[j];
    } else {
        /* XOR delta in-place; update prev_frame to current raw */
        for (int j = 0; j < OUT_W * OUT_H; j++) {
            unsigned char cur = frame_raw[j];
            frame_raw[j]  = cur ^ prev_frame[j];
            prev_frame[j] = cur;
        }
    }
    frame_count++;

    int clen = rle_encode(frame_raw, OUT_W * OUT_H, frame_rle, RLE_BUF_MAX);
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
    /* flags: 0x00=keyframe, 0x01=delta */
    eff_uart_putc(STDIO_UART, is_keyframe ? 0x00 : 0x01);
    /* compressed length (uint16 LE) */
    eff_uart_putc(STDIO_UART, clen & 0xFF);
    eff_uart_putc(STDIO_UART, (clen >> 8) & 0xFF);
    /* compressed payload */
    for (int j = 0; j < clen; j++)
        eff_uart_putc(STDIO_UART, frame_rle[j]);
}

int main(void)
{
    doom_set_print(e1x_print);
    doom_set_exit(e1x_exit);
    doom_set_getenv(e1x_getenv);
    doom_set_file_io(e1x_open, e1x_close, e1x_read, e1x_write,
                     e1x_seek, e1x_tell, e1x_eof);
    doom_set_gettime(e1x_gettime);
    /* malloc: use DOOM_IMPLEMENT_MALLOC (C library malloc/free) which draws
     * from the linker-defined heap (_end → __llvm_libc_heap_limit). */

    eff_uart_puts(STDIO_UART, "[doom] _end=");
    uart_hex32((unsigned int)(void*)_end);
    eff_uart_puts(STDIO_UART, " heap_limit=");
    uart_hex32((unsigned int)(void*)__llvm_libc_heap_limit);
    eff_uart_puts(STDIO_UART, " wad_size=");
    uart_hex32((unsigned int)DOOM_WAD_SIZE);
    eff_uart_puts(STDIO_UART, "\r\n");

    eff_uart_puts(STDIO_UART, "[doom] before doom_init\r\n");

    static char *argv[] = { "doom", "-nosound", 0 };
    doom_init(2, argv,
              DOOM_FLAG_HIDE_MOUSE_OPTIONS |
              DOOM_FLAG_HIDE_SOUND_OPTIONS  |
              DOOM_FLAG_HIDE_MUSIC_OPTIONS);

    eff_uart_puts(STDIO_UART, "[doom] doom_init complete\r\n");

    int tick = 0;
    int update_cnt = 0;
    for (;;) {
        /* Print tic number: every 50 tics + every tic near the crash point */
        if ((update_cnt % 50 == 0) || (update_cnt >= 255 && update_cnt <= 285)) {
            eff_uart_puts(STDIO_UART, "[doom] tic ");
            uart_hex32((unsigned int)update_cnt);
            eff_uart_puts(STDIO_UART, "\r\n");
        }
        doom_force_update();
        update_cnt++;
        if (++tick >= FRAME_SKIP) {
            tick = 0;
            dump_frame();
        }
    }
}
