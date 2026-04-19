RIPTOOLS  ?= /home/blucia/effcc/riptools/build
RIPPER    ?= $(RIPTOOLS)/bin/effcc
OBJCOPY   ?= $(RIPTOOLS)/bin/llvm-objcopy
PUREDOOMDIR ?= PureDOOM

SDK_ROOT   = /home/blucia/cvsandbox/apps/eff_sdk
SDK_BUILD  = /home/blucia/cvsandbox/apps/build/eff_sdk
SDK_INC    = $(SDK_ROOT)/include

LIBEFF        = $(SDK_BUILD)/stdlib/libeff.a
LIBEFF_INIT   = $(SDK_BUILD)/stdlib/libeff_init.a
LIBDRV_UART   = $(SDK_BUILD)/drivers/uart/scalar/libeff_eff_drv_uart.a
LIBDRV_PINMUX = $(SDK_BUILD)/drivers/pinmux/scalar/libeff_eff_drv_pinmux.a
LIBDRV_GPIO   = $(SDK_BUILD)/drivers/gpio/scalar/libeff_eff_drv_gpio.a
LIBDRV_I2C    = $(SDK_BUILD)/drivers/i2c/scalar/libeff_eff_drv_i2c.a
LIBDRV_SPI    = $(SDK_BUILD)/drivers/spi/scalar/libeff_eff_drv_spi.a

SDK_LIBS = $(LIBEFF_INIT) $(LIBEFF) $(LIBDRV_UART) $(LIBDRV_PINMUX) \
           $(LIBDRV_GPIO) $(LIBDRV_I2C) $(LIBDRV_SPI)

STDIO_OPTS = -DSTDIO_UART=UART_3 -DSTDIO_PINMUX=PINMUX_3

# DOOM_IMPLEMENT_MALLOC/EXIT: use stdlib malloc/free and exit() from libeff.a.
# File I/O, gettime, print, and getenv are provided via callbacks in main_e1x.c.
BASERIPPERFLAGS = \
    -DDOOM_IMPLEMENT_MALLOC \
    -DDOOM_IMPLEMENT_EXIT \
    -DEFF_ARCH_E1X $(STDIO_OPTS) \
    --target=e1x -O3 -c \
    -I$(SDK_INC) -IPureDOOM/src/DOOM -flto

YESRIPPERFLAGS = $(BASERIPPERFLAGS) --promote-func-name-filter=.*
NORIPPERFLAGS  = $(BASERIPPERFLAGS) --promote-func-name-filter=

E1X_LDFLAGS = \
    -O3 -flto --target=e1x \
    -DEFF_ARCH_E1X $(STDIO_OPTS) \
    -Wl,--whole-archive $(SDK_LIBS) -Wl,--no-whole-archive \
    -Wl,--allow-multiple-definition

BLD_DIR  ?= bld
WAD_FILE  = newdoom1_1lev.wad
SRC_DIR   = $(PUREDOOMDIR)/src/DOOM

YESSRCS = d_items.c d_main.c doomdef.c doomstat.c dstrings.c f_finale.c \
          f_wipe.c hu_lib.c hu_stuff.c i_net.c i_sound.c i_system.c \
          i_video.c info.c m_argv.c m_bbox.c m_fixed.c m_random.c m_swap.c \
          p_ceilng.c p_doors.c p_floor.c p_lights.c p_map.c p_maputl.c \
          p_plats.c p_pspr.c p_saveg.c p_sight.c p_telept.c p_tick.c \
          p_user.c r_bsp.c r_data.c r_draw.c r_main.c r_plane.c r_sky.c \
          sounds.c st_lib.c st_stuff.c tables.c v_video.c wi_stuff.c \
          z_zone.c DOOM.c am_map.c d_net.c g_game.c m_cheat.c m_menu.c \
          m_misc.c p_enemy.c p_inter.c p_mobj.c p_switch.c r_segs.c \
          r_things.c s_sound.c main_e1x.c p_spec.c w_wad.c p_setup.c

YESOBJS = $(addprefix $(BLD_DIR)/,$(filter %.o,$(YESSRCS:.c=.o)))
NOOBJS  =

OBJS = $(YESOBJS) $(NOOBJS) $(BLD_DIR)/doom_wad.o

$(YESOBJS): RIPPERFLAGS := $(YESRIPPERFLAGS)
$(NOOBJS):  RIPPERFLAGS := $(NORIPPERFLAGS)

all: create_bld $(BLD_DIR)/doom.hex

create_bld:
	mkdir -p $(BLD_DIR)

$(BLD_DIR)/doom.hex: $(BLD_DIR)/doom
	objcopy -Overilog $< $@

$(BLD_DIR)/doom: $(OBJS)
	$(RIPPER) -o $@ $(OBJS) $(E1X_LDFLAGS)

# Embed WAD as read-only data in the ELF binary.
# Symbols: _binary_<name>_start/_end (with dots replaced by underscores).
$(BLD_DIR)/doom_wad.o: $(WAD_FILE)
	$(OBJCOPY) \
	    --input-target binary \
	    --output-target elf32-littleriscv \
	    --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	    $< $@

$(BLD_DIR)/%.o: $(SRC_DIR)/%.c
	-$(RIPPER) $(RIPPERFLAGS) -o $@ $<

$(BLD_DIR)/main_e1x.o: main_e1x.c
	-$(RIPPER) $(RIPPERFLAGS) -o $@ $<

run:
	LD_LIBRARY_PATH=$(RIPTOOLS)/lib $(BLD_DIR)/doom

clean:
	-rm -f $(OBJS) $(BLD_DIR)/doom $(BLD_DIR)/doom.hex
	-rm -rf $(BLD_DIR)

setup:
	-git clone git@github.com:Daivuk/PureDOOM.git
	-wget https://archive.org/download/2020_03_22_DOOM/DOOM%20WADs/Doom%20%28v1.9%29.zip
	-unzip 'Doom (v1.9).zip'
	-rm 'Doom (v1.9).zip'
	-mv DOOM.WAD doom.wad

cleansetup:
	-rm -rf PureDOOM
	-rm 'Doom (v1.9).zip'
	-rm doom.wad
