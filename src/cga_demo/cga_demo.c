/*
 * Copyright 2026
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// Utility to test CGA graphics mode capture.

#include <stdio.h>
#include <string.h>

#include "common/protocol.h"
#include "lib16/types.h"
#include "lib16/x86.h"

#define CGA_SEGMENT 0xb800
#define CGA_BYTES_PER_LINE 80
#define CGA_HEIGHT 200
#define CGA_WIDTH_BYTES 80
#define MODES 3

static const uint8_t cga_modes[MODES] = {0x04, 0x05, 0x06};
static int mode_index = 0;
static int pattern_seed = 0;

static uint16_t saved_es;

static void set_video_mode(uint8_t mode) {
  struct CpuRegs regs;

  x86_reset_regs(&regs);
  regs.u.b.ah = 0x00;
  regs.u.b.al = mode;
  x86_call(0x10, &regs);
}

static void set_cursor(uint8_t row, uint8_t col) {
  struct CpuRegs regs;

  x86_reset_regs(&regs);
  regs.u.b.ah = 0x02;
  regs.u.b.bh = 0x00;
  regs.u.b.dh = row;
  regs.u.b.dl = col;
  x86_call(0x10, &regs);
}

static void tty_char(char ch) {
  struct CpuRegs regs;

  x86_reset_regs(&regs);
  regs.u.b.ah = 0x0e;
  regs.u.b.al = ch;
  regs.u.b.bh = 0x00;
  regs.u.b.bl = 0x0f;
  x86_call(0x10, &regs);
}

static void tty_str(const char *s) {
  while (*s) {
    tty_char(*s++);
  }
}

static void poke_cga(uint16_t offset, uint8_t value) {
  __set_es(CGA_SEGMENT);
  __poke_es(offset, value);
}

static void draw_graphics_pattern() {
  int y;
  int xb;
  int stripe;
  uint8_t value;
  uint16_t offset;
  uint8_t mode;

  mode = cga_modes[mode_index];

  for (y = 0; y < CGA_HEIGHT; ++y) {
    for (xb = 0; xb < CGA_WIDTH_BYTES; ++xb) {
      stripe = ((xb / 5) + (y / 16) + pattern_seed) & 7;

      if (mode == 0x06) {
        switch (stripe & 3) {
          case 0:
            value = 0x00;
            break;
          case 1:
            value = 0xff;
            break;
          case 2:
            value = 0xaa;
            break;
          default:
            value = 0x55;
            break;
        }
      } else {
        switch (stripe & 7) {
          case 0:
            value = 0x00;
            break;
          case 1:
            value = 0x55;
            break;
          case 2:
            value = 0xaa;
            break;
          case 3:
            value = 0xff;
            break;
          case 4:
            value = 0x1b;
            break;
          case 5:
            value = 0xe4;
            break;
          case 6:
            value = 0x93;
            break;
          default:
            value = 0x6c;
            break;
        }
      }

      offset = (y & 1 ? 0x2000 : 0) + ((y >> 1) * CGA_BYTES_PER_LINE) + xb;
      poke_cga(offset, value);
    }
  }
}

static void draw_labels() {
  set_cursor(0, 0);
  tty_str("cga_demo.com  mode ");
  if (cga_modes[mode_index] == 0x04) {
    tty_str("04h 320x200 4-color");
  } else if (cga_modes[mode_index] == 0x05) {
    tty_str("05h 320x200 4-color");
  } else {
    tty_str("06h 640x200 2-color");
  }
  tty_str("   M mode  SPACE redraw  X/ESC exit");
}

static void draw_screen() {
  set_video_mode(cga_modes[mode_index]);
  draw_graphics_pattern();
  draw_labels();
}

static uint16_t read_key() {
  struct CpuRegs regs;

  x86_reset_regs(&regs);
  regs.u.b.ah = 0x00;
  x86_call(0x16, &regs);
  return regs.u.w.ax;
}

int main(int argc, char *argv[]) {
  int running;
  uint16_t key;

  if (argc > 1 && !strcmp(argv[1], "-v")) {
    printf("cga_demo.com (%s)\n", RMTDOS_VERSION);
    return 0;
  }

  saved_es = __get_es();
  running = 1;
  draw_screen();

  while (running) {
    key = read_key();
    switch (key) {
      case 0x011b: // ESC
      case 0x2d00: // ALT-X
      case 0x2d78: // x
      case 0x2d58: // X
        running = 0;
        break;

      case 0x3920: // SPACE
        ++pattern_seed;
        draw_screen();
        break;

      case 0x3200: // ALT-M
      case 0x326d: // m
      case 0x324d: // M
        mode_index = (mode_index + 1) % MODES;
        ++pattern_seed;
        draw_screen();
        break;
    }
  }

  set_video_mode(0x03);
  __set_es(saved_es);
  return 0;
}
