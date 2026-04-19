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

/* --- Frame dump: binary pixel stream over UART ---
 *
 * Each frame:
 *   [0xDE 0xAD 0xBE 0xEF]  4-byte sync marker
 *   [W_lo W_hi H_lo H_hi]  frame dimensions, uint16 little-endian
 *   [W*H bytes]            raw DOOM palette indices 0-255
 *
 * Total: 8 + 80*60 = 4808 bytes per frame.
 * At 115200 baud (11520 bytes/sec) → ~2.4 frames/sec max.
 * FRAME_SKIP=15 ticks at ~35 Hz ≈ one frame per 0.43 s, matching throughput.
 *
 * Use doom_render.py on the host to decode and display with the DOOM palette.
 */
#define DOOM_W     320
#define DOOM_H     200
#define OUT_W       80
#define OUT_H       60
#define FRAME_SKIP  15

static void dump_frame(void)
{
    const unsigned char *fb = doom_get_framebuffer(1);

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
    /* pixel data: raw DOOM palette indices, downsampled from 320x200 */
    for (int y = 0; y < OUT_H; y++) {
        int fy = (y * DOOM_H) / OUT_H;
        for (int x = 0; x < OUT_W; x++) {
            int fx = (x * DOOM_W) / OUT_W;
            eff_uart_putc(STDIO_UART, fb[fy * DOOM_W + fx]);
        }
    }
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
        if (update_cnt < 5)
            eff_uart_puts(STDIO_UART, "[doom] update\r\n");
        doom_force_update();
        if (update_cnt < 5)
            eff_uart_puts(STDIO_UART, "[doom] update done\r\n");
        update_cnt++;
        if (++tick >= FRAME_SKIP) {
            tick = 0;
            dump_frame();
        }
    }
}
