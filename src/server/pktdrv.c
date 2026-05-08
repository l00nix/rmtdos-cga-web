/*
 * Copyright 2022 Dennis Jenkins <dennis.jenkins.75@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// https://en.wikipedia.org/wiki/PC/TCP_Packet_Driver
// http://crynwr.com/packet_driver.html

#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "lib16/x86.h"
#include "server/bufmgr.h"
#include "server/globals.h"
#include "server/pktdrv.h"
#include "server/protocol.h"
#include "server/util.h"

// Implemented in 'server/pktrecv.s'
extern void pktdrv_receive_isr();

// This function is called via interrupt from the packet driver.
// The packet driver calls the asm function `_pktdrv_receive_isr`, which
// switches to a private 256-byte stack and then calls this function.
// Do NOT call any DOS or BIOS functions.  Do not touch the heap.
// Typically called with interrupts disabled, but YMMV.
void pktdrv_receive_func(struct CpuRegs *regs) {
  if (regs->u.w.ax) {
    // Second call for buffer; Packet driver is done writing to it.
    buffer_mark_ready((struct Buffer *)(regs->si));
  } else {
    // First call; attempt to allocate a buffer to return to the driver.
    void *buffer = buffer_acquire(regs->u.w.cx);

    if (buffer) {
      regs->es = __get_ds();
      regs->di = (uint16_t)buffer;
      ++g_pktdrv_stats.packets_recv;
    } else {
      regs->es = 0;
      regs->di = 0;
      ++g_pktdrv_stats.packets_dropped;
    }
  }
}

// Section 6.10, `get_parameters()`
enum PktDrvResultCode pktdrv_get_parameters(PktDrvIrq irq,
                                            struct PktDrvParams *params) {
  struct CpuRegs regs;

  x86_reset_regs(&regs);
  regs.u.w.ax = 0x0a00; // `get_parameters()` call.
  x86_call(irq, &regs);
  if (regs.flags & CPU_FLAG_CARRY) {
    return regs.u.b.dh; // Error code.
  }

  x86_memcpy_bytes(__get_ds(), (uint16_t)params, regs.es, regs.di,
                   sizeof(params));
  return PKTDRV_OK;
}

enum PktDrvResultCode pktdrv_init(PktDrvIrq irq) {
  struct CpuRegs regs;
  uint16_t ethertype = htons(g_ethertype);

  // Init ye-ole-globals.
  g_pktdrv_irq = 0;
  g_pktdrv_handle = 0;
  memset(&g_pktdrv_info, 0, sizeof(g_pktdrv_info));
  memset(&g_pktdrv_stats, 0, sizeof(g_pktdrv_stats));

  if (!irq) {
    irq = pktdrv_probe_all();
  }
  if (!irq) {
    return PKTDRV_ERR_NO_DRIVER;
  }

  // Register to receive "Regular Ethernet" packets.
  // Leave 'char far *type' as NULL, so that we get all packets.
  x86_reset_regs(&regs);
  regs.u.b.al = PKTDRV_CLASS_ETHERNET; // "if_class"
  regs.u.b.ah = PKTDRV_FUNC_ACCESS_TYPE;
  regs.u.w.bx = 0xffff;            // "if_type"
  regs.u.b.dl = 0;                 // "if_number"
  regs.ds = __get_ds();          // "type specification"
  regs.si = &ethertype;          // "type specification"
  regs.u.w.cx = sizeof(ethertype); // typelen
  regs.es = __get_cs();          // "(far *receiver)()"
  regs.di = pktdrv_receive_isr;  // "(far *receiver)()"
  x86_call(irq, &regs);
  if (regs.flags & CPU_FLAG_CARRY) {
    printf("access_type() failed\n");
    return regs.u.b.dh; // Error code.
  }

  // At this point, we are connected.
  // Our ISR could be invoked at any moment.
  g_pktdrv_irq = irq;
  g_pktdrv_handle = regs.u.w.ax;

  // Call `driver_info()` and cache the (static) results.
  x86_reset_regs(&regs);
  regs.u.w.ax = (PKTDRV_FUNC_DRIVER_INFO << 8) | 0xff;
  regs.u.w.bx = g_pktdrv_handle;
  x86_call(irq, &regs);
  if (regs.flags & CPU_FLAG_CARRY) {
    printf("driver_info() failed\n");
    return regs.u.b.dh; // Error code.
  }

  g_pktdrv_info.version = regs.u.w.bx;
  g_pktdrv_info.type = regs.u.w.dx;
  g_pktdrv_info._class = regs.u.b.ch;
  g_pktdrv_info.number = regs.u.b.cl;
  g_pktdrv_info.functionality = regs.u.b.al;
  x86_read_asciiz(g_pktdrv_info.name, sizeof(g_pktdrv_info.name), regs.ds,
                  regs.si);

  // Call `get_address()` to get the MAC address of the NIC.
  x86_reset_regs(&regs);
  regs.u.w.ax = PKTDRV_FUNC_GET_ADDRESS << 8;
  regs.u.w.bx = g_pktdrv_handle;
  regs.u.w.cx = sizeof(g_pktdrv_info.mac_addr);
  regs.es = __get_ds();
  regs.di = g_pktdrv_info.mac_addr;
  x86_call(irq, &regs);
  if (regs.flags & CPU_FLAG_CARRY) {
    printf("get_address() failed\n");
    return regs.u.b.dh; // Error code.
  }

  return PKTDRV_OK;
}

enum PktDrvResultCode pktdrv_done() {
  struct CpuRegs regs;

  if (g_pktdrv_handle) {
    x86_reset_regs(&regs);
    regs.u.w.ax = PKTDRV_FUNC_RELEASE_TYPE << 8;
    regs.u.w.bx = g_pktdrv_handle;

    x86_call(g_pktdrv_irq, &regs);
    if (regs.flags & CPU_FLAG_CARRY) {
      return regs.u.b.dh; // Error code.
    }
  }

  g_pktdrv_irq = 0;
  g_pktdrv_handle = 0;
  return PKTDRV_OK;
}

#define MIN_ETH_FRAME_SIZE 60
// Assumes that 'buffer' points to at least 60 bytes.
// Unused bytes should be set to zero, or packet capture diagnostics will
// look really weird.
enum PktDrvResultCode pktdrv_send(const void *buffer, uint16_t length) {
  struct CpuRegs regs;

  if (g_pktdrv_handle) {
    x86_reset_regs(&regs);
    regs.u.w.ax = PKTDRV_FUNC_SEND_PKT << 8;
    regs.u.w.cx = length > MIN_ETH_FRAME_SIZE ? length : MIN_ETH_FRAME_SIZE;
    regs.ds = __get_ds();
    regs.si = buffer;

    x86_call(g_pktdrv_irq, &regs);
    if (regs.flags & CPU_FLAG_CARRY) {
      return regs.u.b.dh; // Error code.
    }
  }

  return PKTDRV_OK;
}
