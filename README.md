# rmtdos-cga-web

Remotely control HP 200LX / CGA DOS systems from Linux, with an experimental
browser-based graphics view.

Tested on FreeDOS-1.3 in QEMU and on a real Pentium-MMX 166 MHz.

This project is a fork of Dennis Jenkins' original `rmtdos`
(https://github.com/dennisjenkins75/rmtdos), adapted for HP 200LX-style CGA
graphics capture and browser-based viewing.

`rmtdos-cga-web` contains a DOS (FreeDOS or MSDOS or equivalent) TSR that allows
a Linux system on the same LAN to remotely control the DOS system.

## Theory of Operation

There are two main programs, one that runs on DOS (`cgaweb.com`), and the other
that runs on Linux (`rmtdos-cga-web-client`).  There is also a VGA text mode demo
(`vga_demo.com`) program for testing VGA text video modes and displaying test
patterns.

`cgaweb.com` ("server"):

1. Probes for a local
   [PC/TCP packet driver](http://crynwr.com/packet_driver.html) (or you can
   specify the exact packet driver IRQ on the command line).
1. Registers to receive raw Ethernet frames with an
   [EtherType](https://en.wikipedia.org/wiki/EtherType) of **80ab**
   (also configurable on the command line).
1. Hooks interrupts **08** (BIOS clock) and **2f** (DOS multiplex).
1. Goes resident (TSR).
1. Responds to Ethernet packets with the correct EtherType and internal
   packet signatures.
1. Streams contents of VGA text framebuffer (**b800:0000**) to any connected
   clients.
1. Can receive control packets that include keystrokes to be shoved into
   the BIOS keyboard buffer.
1. Run with `-h` to see usage and command line options.

`rmtdos-cga-web-client`:

1. The client probes the local LAN by broadcasting a special packet for the same
   EtherType.
1. Presents a cheesy ncurses UI that lists discovered servers, and allows the
   user to select one (just press the key for the server ID, `0`-`9`).
1. Periodically sends a refresher packet to the server the user wants to
   "connect" to.
1. Receives VGA text memory dumps, and renders them via `ncurses`.
1. Experimental: run with `-w` to serve a browser-based CGA graphics view at
   `http://127.0.0.1:8080/`.
1. Run with `-h` to see usage and command line options.
1. Run with `-u` to uninstall/unload the resident program.

## Usage

On the DOS system, install a PC/TCP packet driver, then run `cgaweb.com`.
It should find the packet driver, initialize, "go resident" and return control
to DOS.

![Installing cgaweb.com](/images/install.png)

On Linux, run the client.  You might need to override the default ethernet
device name.  Sadly, you'll need to run it as root since it needs
to create a raw socket.  `sudo ./rmtdos-cga-web-client -i br0`.

For experimental HP 200LX-style CGA graphics viewing, run the client with
`-w` and open `http://127.0.0.1:8080/` in a browser.  When connecting to the
Linux machine over SSH, forward the port from Windows with
`ssh -L 8080:127.0.0.1:8080 user@linux-host`.

![Client menu](/images/menu.png)

The displayed columns are:

1. Ordinal id for the DOS system (0 .. n-1)
1. MAC address
1. VGA mode (text modes are 0,1,2,3).
1. Width and Height of the screen.
1. Time since last packet received from the DOS system.

Then select the client that you want to connect.

![Client view](/images/live.png)
![Another demo](/images/defrag.png)

## Building

1. Install ["dev86"](https://github.com/lkundrak/dev86), which provides a
   16-bit x86 compiler, assembler, and linker.
1. `make`
1. `sudo make setcap` (optional, see https://stackoverflow.com/a/46466642).
1. Copy `out/cgaweb.com` and `out/vga_demo.com` (optional) to a DOS system.

There are some non-traditional makefile targets that I find handy during
development:

1. `make format` - Reformats the C code w/ clang-format.
1. `make setcap` - Runs the `setcap` command on the client, to enable a
   non-root user to run it without requiring sudo.
1. `make typos` - Runs [typos](https://crates.io/crates/typos) on the source
   files, finding spelling mistakes.
1. `make bcclint` - Horrible hack that attempts to compile the DOS binaries
   with actual GCC (it will fail to produce a binary).  This is useful because
   dev86's compiler, `bcc`, is a traditional K&R compiler, which will unhelpfully
   assume function prototypes if you forget to include the proper header.
   Modern `gcc -Wall` can be much more strict and issue all kinds of warnings
   for bad coding practices.

## Future Plans

1. Add mouse support.  DOS has
   [int 33h](https://stanislavs.org/helppc/int_33.html) with lots of support
   for setting the mouse's cursor position.

1. Possibly add remote file transfer capacilities (kind of like TFTP over raw
   Ethernet, maybe with a Linux-side FUSE driver).

## FAQ

Q: Why was this project developed using "dev86", which has poor support for
modern C?

A: I prefer to do development from my Linux system, and treat DOS as an embedded
system that I can push binaries to.  So I needed a C compiler that can run on
Linux and produce DOS real-mode programs.  GCC cannot do this.  I attempted and
failed to get OpenWatcom 1.9 and 2.0 to compile cleanly on Gentoo Linux, so
I used "dev86", which exists for both Linux and DOS.

Q: Why did you use raw Ethernet packets and not TCP/IP or UDP?

A: Memory overhead.  A primary design criteria was to keep the memory usage
of the resident `cgaweb.com` program as small as possible.  Basically,
there were four options for implementing full TCP/IP support, and each
of them would have added serious developmental delays and RAM costs:

1. [mTCP](https://www.brutman.com/mTCP/) - Would not work in TSR mode
   without significant code changes.  mTCP assumes that it can issue DOS
   file IO requests "whenever", and that is not compatible with a TSR that
   does not want to bother switching out the DOS PSPs and such.

1. Wattcp - Unmaintained, didn't even go there.

1. TSR TCP/IP implementation - It wants 73K of RAM at runtime.

1. Roll my own.  Um, no thanks.

A: Compatibility with other mTCP programs.  mTCP tells the packet driver
that it wants to register for ALL EtherTypes, but it only really uses
"ARP" (0x0806) and "IPV4" (0x0800).  If `cgaweb.com` registers for the same,
then the packet driver won't let any mTCP program successfully run.  Since
`cgaweb.com` uses its own EtherType, both stacks can co-exist on the same
packet driver and not require a packet multiplexor.

Q: Can I build it on DOS?

A: `cgaweb.com` and `vga_demo.com` might build there, but I've not bothered
trying.  Currently, all 3 programs are built on Linux.  The DOS programs
are built using ["dev86"](https://github.com/lkundrak/dev86).  FreeDOS ships
with "dev86" (if it is not already installed, you can install it with the
FreeDOS utility `fdimples.exe`).

Q: How do I exit the client?

A: Press `ALT-ESCAPE`.  The client sets ncurses to "raw" mode, so that the typical
termios control characters (ex: `CTRL-C`) do NOT generate signals, but
instead are presented to the client, so that the client can send them to
the DOS end.

Q: How can I view the raw Ethernet traffic (tcpdump filter expression)?

A: `tcpdump 'ether proto 0x80ab'`

A: (wireshark): `"eth.type == 0x80ab"`

Q: My DOS system does not have a NIC, can I use a serial port?

A: In theory, yes; but I've not tested this.  Connect the DOS system to your
Linux system via "null-modem" cable.  On the DOS side, run the "CSLIP" packet
driver.  On the Linux end, configure "cslip" on the serial TTY device.  This
guide contains a step-by-step walkthrough:

- https://mcmackins.org/stories/dos-slip.html

Q: `cgaweb.com` is optimized for minimal memory usage.  How did you audit that?

A: Well, its a goal.  I'm sure that there is always room for improvement.  I've
been auditing the memory usage by examining the linker map via:

- `make && sort -k3 out/cgaweb.map`

And by using `debug.com` to peek at the TSR's private ISR (interrupt service
routine) stack usage (to see if it is safe to shrink the ISR stacks).

1. Build the software, examine the linker map:

   ```make && sort -k3 out/cgaweb.map | grep "stack_"
                   int08   _int08_stack_bottom  3  00003020  R
                   int08      _int08_stack_top  3  00003220  R
                   int2f   _int2f_stack_bottom  3  00003224  R
                   int2f      _int2f_stack_top  3  000032a4  R
                 pktrecv   pktdrv_stack_bottom  3  000032a8  r
                 pktrecv      pktdrv_stack_top  3  000033a8  r
   ```

1. Then load `cgaweb.com` in DOS.  Observe the printed PSP value.

1. Connect with the client from Linux.  This will cause `cgaweb.com` to start
   using its ISR stacks.

1. Run `debug.com`, and examine the RAM where the stacks are (example is
   for `pktdrv_stack_bottom`:

```
C:\TEMP>cgaweb
Packet Driver Initialized.  irq:0x60, NE2100, 82:00:00:00:00:09, et:80ab
Going resident.  PSP:073b, Last Addr: 4c50

C:\TEMP>debug
-d 073b:32a8
073B:32A0                         -00 00 00 00 00 00 00 00         ........
073B:32B0  00 00 00 00 00 00 00 00-00 00 00 00 00 00 00 00 ................
073B:32C0  00 00 00 00 00 00 00 00-00 00 00 00 00 00 00 00 ................
073B:32D0  00 00 00 00 00 00 00 00-00 00 00 00 00 00 00 00 ................
073B:32E0  00 00 00 00 00 00 00 00-00 00 00 00 00 00 00 00 ................
073B:32F0  00 00 00 00 00 00 00 00-00 00 00 00 00 00 00 00 ................
073B:3300  00 00 00 00 00 00 00 00-00 00 00 00 00 00 00 00 ................
073B:3310  00 00 00 00 00 00 00 00-00 00 00 00 00 00 00 00 ................
073B:3320  00 00 00 00 00 00 00 00-                        ........
-d
073B:3320                         -00 00 00 00 00 00 00 00         ........
073B:3330  00 00 00 00 00 00 00 00-00 00 00 00 00 00 00 00 ................
073B:3340  00 00 00 00 00 00 00 00-00 00 00 00 00 00 00 00 ................
073B:3350  00 00 00 00 00 00 00 00-00 00 00 00 00 00 00 00 ................
073B:3360  00 00 00 00 00 00 4E 3C-4E 3C 54 3C 90 3C 78 33 ......N<N<T<.<x3
073B:3370  57 1E 54 3C 54 3C 90 3C-82 33 FD 2A 82 33 3B 07 W.T<T<.<.3.*.3;.
073B:3380  82 33 01 00 40 01 3C 00-00 00 54 3C 90 3C 82 33 .3..@.<...T<.<.3
073B:3390  82 33 00 00 3B 07 3B 07-3B 07 3B 07 3A 0A 00 00 .3..;.;.;.;.:...
073B:33A0  C6 02 E6 00 64 0D 00 00-                        ....d...
```
