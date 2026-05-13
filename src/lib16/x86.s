;  SPDX-FileCopyrightText: 2022 Dennis Jenkins <dennis.jenkins.75@gmail.com>
;  SPDX-License-Identifier: GPL-2.0-or-later


#if __FIRST_ARG_IN_AX__
  #error __FIRST_ARG_IN_AX__ not supported
#endif

; // uint16_t __get_sp();
.text
.global ___get_sp
___get_sp:
  mov     ax, sp
  ret


; // uint16_t x86_cli();
.text
.global _x86_cli
_x86_cli:
  pushf
  cli
  pop    ax
  ret


; // uint16_t x86_sti(uint16_t saved_flags);
.global _x86_sti
_x86_sti:
  push   bp
  mov    bp, sp
  mov    bx, [bp + 4]                ; bx = saved_flags
  and    bx, #$200                   ; Mask to keep I flag.

  pushf
  pop    ax
  and    ax, #$ef                    ; Mark out I flag.
  or     ax, bx                      ; Copy in other flags.

  push   ax
  popf

  pop    bp
  ret                                ; Note: flags are also in AX.


; // int x86_read_asciiz(
; //  char *dest,
; //  size_t maxlen,
; //  uint16_t segment,
; //  uint16_t offset
; // );
.global _x86_read_asciiz
_x86_read_asciiz:
  push    bp
  mov     bp, sp
  push    ds
  push    es

  mov     di, [bp + 4]               ; dest
  mov     cx, [bp + 6]               ; maxlen
  mov     es, [bp + 8]               ; segment
  mov     si, [bp + 10]              ; offset

  xor     ax, ax                     ; retval, count of chars copied.
copy_loop:
  seg     es
  mov     bl, [si]
  or      bl, bl                     ; Did we read a 0 byte?
  jz      done
  mov     [di], bl
  inc     di
  inc     si
  inc     ax
  dec     cx
  jnz     copy_loop

done:
  xor     bx, bx
  mov     [di], bl                   ; Always terminate string

  pop     es
  pop     ds
  pop     bp
  ret


; // x86_reset_regs(struct CpuRegs *regs);
.global _x86_reset_regs
_x86_reset_regs:
#if __FIRST_ARG_IN_AX__
  mov    bx, ax
#else
  mov    bx, sp
  mov    bx, [bx + 2]
#endif
  mov    [bx + 0], #0          ; regs.ax
  mov    [bx + 2], #0          ; regs.bx
  mov    [bx + 4], #0          ; regs.cx
  mov    [bx + 6], #0          ; regs.dx
  mov    [bx + 8], #0          ; regs.si
  mov    [bx + 10], #0         ; regs.di
  mov    [bx + 12], #0         ; regs.sp
  mov    [bx + 14], #0         ; regs.bp
  mov    [bx + 16], #0         ; regs.flags

  mov    [bx + 18], cs         ; regs.cs
  mov    [bx + 20], ds         ; regs.ds
  mov    [bx + 22], es         ; regs.es
  mov    [bx + 24], ss         ; regs.ss
  ret


; // int x86_call(uint8_t irq, struct CpuRegs *regs);
.global _x86_call
_x86_call:
  push    bp
  mov     bp, sp
  push    ds

  push    bp                  ; Save our stack frame incase it gets clobbered.

  ; Below we will use the `iret` opcode to call the interrupt handler
  ; directly.  That handler will ultimately use `iret` to return to us.
  ; We first need to push that return location onto the stack.
  pushf                       ; iret flags
  mov     ax, [bp - 6]        ; ax = CPU flags to restore upon return.
  push    cs                  ; iret cs
  mov     bx, #ret_addr
  push    bx                  ; iret ip

  ; Our 'iret' (below) will pop 3 words from the stack: IP, CS, Flags.
  ; Push the flags that we want when the ISR runs first.
  and     ah, #$0c            ; Simulate interrupt flags.
  push    ax                  ; interrupt flags

  ; Extract the interrupt vector address so we can call the handler.
  xor     bx, bx
  mov     es, bx
  mov     bl, [bp + 4]        ; irq
  shl     bx, #2
  seg es
          push word [bx + 2]  ; interrupt segment
  seg es
          push word [bx]      ; interrupt offset

  mov     bx, [bp + 6]        ; regs
  mov     ax, [bx + 16]       ; regs.flags
  push    word [bx + 20]      ; regs.ds

  ; Set registers before invoking interrupt
  mov     ax, [bx + 0]
  mov     cx, [bx + 4]
  mov     dx, [bx + 6]
  mov     si, [bx + 8]
  mov     di, [bx + 10]
  mov     es, [bx + 22]
  mov     bx, [bx + 2]
  pop     ds

  iret

ret_addr:
  pop     bp                   ; Saved earlier, in case ISR clobbers it.
  pushf                        ; Save flags state
  push    cx                   ; Save temp register
  push    bx                   ; Save temp register

  mov     cx, ds               ; Stash returned DS into CX.
  mov     ds, [bp - 2]         ; Restore original (our) DS.

  mov     bx, [bp + 6]         ; regs
  mov     [bx + 20], cx        ; regs.ds
  mov     [bx + 22], es        ; regs.es
  mov     [bx + 0], ax         ; regs.ax
  pop     word [bx + 2]        ; regs.bx
  pop     word [bx + 4]        ; regs.cx
  mov     [bx + 6], dx         ; regs.dx
  mov     [bx + 8], si         ; regs.si
  mov     [bx + 10], di        ; regs.di
  pop     word [bx + 16]       ; regs.flags

  pop     ds
  pop     bp
  ret


; // https://www.xtof.info/Timing-on-PC-familly-under-DOS.html, Section 4.3
; // uint32_t x86_read_bios_tick_clock();
.global _x86_read_bios_tick_clock
_x86_read_bios_tick_clock:
  push    ds
  pushf
  xor     ax, ax
  mov     ds, ax
  cli
  mov     ax, [$046c]
  mov     dx, [$046e]
  popf
  pop     ds
  ret


; // int x86_inject_keystroke(uint8_t bios_scan_code, uint8_t ascii_value, uint8_t flags_17);
.global _x86_inject_keystroke
  _x86_inject_keystroke:
    push    bp
    mov     bp, sp
    push    ds
    push    cx
    push    bx
  
    mov     ax, #$0040
    mov     ds, ax
    mov     bl, [$17]              ; Preserve keyboard flag byte #0.
    mov     bh, [$18]              ; Preserve keyboard flag byte #1.
    mov     cx, [bp + 8]           ; CX = flags_17
    and     cl, #$0f               ; Ensure that we only have the bottom nibble.
    mov     al, [$17]              ; AX = Keyboard flag byte #0
    and     al, #$f0               ; Keep the upper nibble.
    or      al, cl                 ; Compose new keyboard flags byte.
    mov     [$17], al

    mov     ah, [$18]              ; Extended keyboard flag byte.
    and     ah, #$fc               ; Keep non-left-CTRL/ALT state.
    test    cl, #$04
    jz      no_left_ctrl
    or      ah, #$01               ; Left CTRL pressed.
no_left_ctrl:
    test    cl, #$08
    jz      no_left_alt
    or      ah, #$02               ; Left ALT pressed.
no_left_alt:
    mov     [$18], ah

    mov     ax, [bp + 4]           ; AX = bios_scan_code (need in CH)
    mov     cx, [bp + 6]           ; CX = ascii_value (need in CL)
    mov     ch, al

    mov     ax, #$0500             ; http://www.ctyme.com/intr/rb-1761.htm
    int     $16                    ; AL = 0 on success, 1 on fail.
  
    mov     [$17], bl              ; Do not leave CTRL/ALT/SHIFT stuck on.
    mov     [$18], bh
    and     ax, #1
    xor     ax, #1
  
    pop     bx
    pop     cx
    pop     ds
    pop     bp

  ret                            ; AX = 0 on fail, 1 on success.
