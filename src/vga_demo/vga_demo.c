/*
 * Copyright 2022 Dennis Jenkins <dennis.jenkins.75@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// Utility to test VGA text mode capabilities.

#include <bios.h>
#include <stdlib.h>

#include "lib16/vga.h"
#include "lib16/x86.h"

static int running = 1;

#define VIDEO_MODES 3
static int video_mode = 0;

// Current character to show in FB/BG color grid.
// Can be changed w/ arrow keys.
static char active_char = 0xb0;

static char hex_char(int x) { return x > 9 ? (x - 10 + 'a') : (x + '0'); }

// Count of keyboard events to keep in history list.
#define KEY_EVENT_HIST_SIZE 6

static uint16_t key_events[KEY_EVENT_HIST_SIZE];

#define HEADING_NORMAL 0x0e
#define HEADING_HIGHLIGHT 0xe0

// Fill screen w/ a test pattern.
void draw_test_pattern() {
  struct VgaState vga;
  int x, y, xp, yp;
  int x_pitch = 0;  // Spacing between ASCII chars in left grid.
  int y_pitch = 0;  // Spacing between ASCII chars in left grid.
  int x_offset = 0; // Offset to RIGHT grid (colors).
  int y_offset = 3; // Offset to both grids from top of screen.
  char str[2];
  uint8_t attr; // Attribute to draw with.

  vga_read_state(&vga);
  x_pitch = (vga.text_cols >= 80) ? 2 : 1;
  y_pitch = (vga.text_rows > 35) ? 2 : 1;
  x_offset = (vga.text_cols > 40) ? 40 : 20;

  for (y = 0; y < 16; y++) {
    str[0] = hex_char(y);
    str[1] = 0;

    // Across the top (lower nibble)
    attr = (y == (active_char & 0x0f)) ? HEADING_HIGHLIGHT : HEADING_NORMAL;
    vga_write_str(y * x_pitch + 2, y_offset, attr, str);
    vga_write_str(y * x_pitch + 2 + x_offset, y_offset, attr, str);

    // Down left side (upper nibble)
    attr =
        ((y << 4) == (active_char & 0xf0)) ? HEADING_HIGHLIGHT : HEADING_NORMAL;
    vga_write_str(0, y * y_pitch + y_offset + 2, attr, str);
    vga_write_str(x_offset, y * y_pitch + y_offset + 2, attr, str);

    // Character grid
    for (x = 0; x < 16; x++) {
      uint8_t i = x | (y << 4);
      int xx = x * x_pitch + 2;
      int yy = y * y_pitch + y_offset + 2;

      // By character
      str[0] = i;
      attr = (i == active_char) ? 0xf0 : 7;
      vga_write_str(xx, yy, attr, str);

      // By color
      str[0] = active_char;
      for (yp = 0; yp < y_pitch; ++yp) {
        for (xp = 0; xp < x_pitch; ++xp) {
          vga_write_str(xx + xp + x_offset, yy + yp, i, str);
        }
      }
    }
  }
}

// https://stanislavs.org/helppc/bios_data_area.html
// https://stanislavs.org/helppc/kb_flags.html
#define BIOS_LOCATIONS 4
static const uint16_t bios_offsets[] = {
    0x17, // Keyboard flag byte 0
    0x18, // Keyboard flag byte 1
    0x96, // keyboard mode/type
    0x97, // LED Indicator Flags
};

void refresh_screen() {
  char tmp[32];
  int i;
  uint16_t saved_es = __get_es();

  for (i = 0; i < KEY_EVENT_HIST_SIZE; ++i) {
    const uint8_t scan_code = key_events[i] >> 8;
    const uint8_t ascii = key_events[i] & 0xff;

    vga_printf(0, i + 37, 0x1e, "int16: [%d] %02x %02x %c", i, scan_code, ascii,
               ascii ? ascii : ' ');
  }

  __set_es(0x0040);
  for (i = 0; i < BIOS_LOCATIONS; ++i) {
    vga_printf(20, i + 37, 0x1e, "[0040:%04x] %02x", bios_offsets[i],
               __peek_es(bios_offsets[i]));
  }
  __set_es(saved_es);
}

void prep_screen() {
  struct VgaState vga;

  vga_read_state(&vga);
  vga_clear_rows(0, 50);
  vga_disable_blink();
  vga_write_str(0, 0, 15, "<ALT-X> Exit.  <\x18, \x19, \x1a, \x1b> Move.");

  vga_printf(0, 1, 15, "<ALT-V> Video Mode: [%d] %dx%d  ", video_mode,
             vga.text_cols, vga.text_rows);

  draw_test_pattern();
  vga_gotoxy(0, vga.text_rows - 2);
}

static void set_video_mode() {
  switch (video_mode) {
    case 0:
      vga_mode_80x25();
      break;
    case 1:
      vga_mode_80x43();
      break;
    case 2:
      vga_mode_80x50();
      break;
  }
  prep_screen();
}

void process_keyboard() {
  struct CpuRegs regs;
  char tmp[32];
  int i;

  // Read raw scan code + ascii code.
  x86_reset_regs(&regs);
  regs.u.w.ax = 0;
  x86_call(0x16, &regs);

  // User pressed "ALT-X", so exit.
  switch (regs.u.w.ax) {
    case 0x2d00: // ALT-X
      running = 0;
      return;
    case 0x2f00: // ALT-V
      video_mode = (video_mode + 1) % VIDEO_MODES;
      set_video_mode();
      break;
    case 0x4800: // Up arrow
      active_char = (active_char - 16) & 0xff;
      prep_screen();
      break;
    case 0x5000: // Down arrow
      active_char = (active_char + 16) & 0xff;
      prep_screen();
      break;
    case 0x4b00: // Left arrow
      active_char = (active_char & 0xf0) | ((active_char - 1) & 0x0f);
      prep_screen();
      break;
    case 0x4d00: // Right arrow
      active_char = (active_char & 0xf0) | ((active_char + 1) & 0x0f);
      prep_screen();
      break;
    case 0x3920: // Space
      active_char = (active_char + 1) & 0xff;
      prep_screen();
      break;
  }

  // Shift events, push new event to end.
  for (i = 1; i < KEY_EVENT_HIST_SIZE; ++i) {
    key_events[i - 1] = key_events[i];
  }
  key_events[--i] = regs.u.w.ax;
}

int main(int argc, char *argv[]) {
  set_video_mode();
  prep_screen();

  while (running) {
    refresh_screen();

    while (!kbhit()) {
      x86_dos_idle();
    }

    process_keyboard();
  }

  return 0;
}
