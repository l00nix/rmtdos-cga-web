/*
 * Copyright 2022 Dennis Jenkins <dennis.jenkins.75@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// Contains wrapper for invoking arbitrary software interrupts from 8086 real
// mode.

#ifndef __RMTDOS_SERVER_X86_H
#define __RMTDOS_SERVER_X86_H

#include "lib16/types.h"
#include <sys/types.h>

#define CPU_FLAG_CARRY (1 << 0)
#define CPU_FLAG_PARITY (1 << 2)
#define CPU_FLAG_AUX_CARRY (1 << 4)
#define CPU_FLAG_ZERO (1 << 6)
#define CPU_FLAG_SIGN (1 << 7)
#define CPU_FLAG_TRAP (1 << 8)
#define CPU_FLAG_INTERRUPT (1 << 9)
#define CPU_FLAG_DIRECTION (1 << 10)
#define CPU_FLAG_OVERFLOW (1 << 11)

// WARNING: DO NOT CHANGE THE LAYOUT OF THIS STRUCTURE.
// Our asm source (*.s) assumes non-symbolic offsets, due to `as86` not being
// able to import C header files.
struct CpuRegs {
  union {
    struct Words {
      uint16_t ax; // [0]
      uint16_t bx; // [2]
      uint16_t cx; // [4]
      uint16_t dx; // [6]
    } w;
    struct Bytes {
      uint8_t al; // [0]
      uint8_t ah; // [1]
      uint8_t bl; // [2]
      uint8_t bh; // [3]
      uint8_t cl; // [4]
      uint8_t ch; // [5]
      uint8_t dl; // [6]
      uint8_t dh; // [7]
    } b;
  } u;

  // These registers are always 16 bits each.
  uint16_t si;    // [8]
  uint16_t di;    // [10]
  uint16_t sp;    // [12]
  uint16_t bp;    // [14]
  uint16_t flags; // [16]
  uint16_t cs;    // [18]
  uint16_t ds;    // [20]
  uint16_t es;    // [22]
  uint16_t ss;    // [24]
};

// Clears all CpuRegs.  Sets the segment registers to current values.
void x86_reset_regs(struct CpuRegs *regs);

// Invokes software interrupt `irq` with registers set to values from `regs`,
// except that the following are NOT set before calling the interrupt:
// `bp`, `sp`, `flags`, `cs`, `ss`.
// Return value is AX register value (as signed int).
// CPU registers in `regs` are updated with the values they have when the
// interrupt returns.
int x86_call(uint8_t irq, struct CpuRegs *regs);

// Copies a NUL-terminated string from `segment:offset` into `dest`, up to
// maxlen chars.  Will terminate the result with NUL, even if it is truncated.
// Returns could of characters copied, including the terminating NUL.
int x86_read_asciiz(char *dest, size_t maxlen, uint16_t segment,
                    uint16_t offset);

// Copies `count` bytes from source to dest.
// Needed b/c the 'bcc' compiler only emits code for the 'Tiny' memory model,
// with all 4 segment registers having identical values.
// Undefined behavior if `source` and `dest` overlap.
void x86_memcpy_bytes(uint16_t dest_segment, uint16_t dest_offset,
                      uint16_t src_segment, uint16_t src_offset,
                      uint16_t byte_count);

// Disables interrupts and returns previous CPU flags.
// Note that interrupts might have already been disabled.
extern uint16_t x86_cli();

// Restores CPU flags previously saved during __cli()
// Returns CPU flags register as set.
extern uint16_t x86_sti(uint16_t saved_flags);

// Invokes int 28h, to allow DOS to perform IDLE processing.
// On a stock FreeDOS system, this will invoke the FDAPM.COM TSR, which will
// surrender a timeslice to any VM that it is running in (cpu "hlt").
extern void x86_dos_idle();

// Returns 4 bytes from "0040:006c", a 32-bit counter maintained by BIOS
// that counts the number of INT 8 occurrences.  Unless some other system
// reprograms the timer chip to run faster (like GWBASIC), then the timer
// frequency is 18.2065 ticks/s, or 54.9254 ms.
extern uint32_t x86_read_bios_tick_clock();

// Injects a keystroke directly into the BIOS keyboard buffer.
// Returns 1 if successful, 0 if buffer is full.
extern int x86_inject_keystroke(uint8_t bios_scan_code, uint8_t ascii_value,
                                uint8_t flags_17);

/* Hacks so that we can PARSE w/ GCC our MS-DOS C code.
   We do this as a stricter type of linter, since BCC is very lax.
*/

#ifdef __GNUC__
extern uint16_t __get_cs();
extern uint16_t __get_ds();
extern uint16_t __get_es();
#endif

extern void *x86_move_stack(void *tos_offset);

#endif // __RMTDOS_SERVER_X86_H
