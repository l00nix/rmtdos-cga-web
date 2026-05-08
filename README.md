# rmtdos-cga-web

Remotely control HP 200LX / CGA DOS systems from Linux, with an experimental
browser-based graphics view.

Tested on a HP 200LX running MS-DOS 5 and a Linux machine with Ubuntu 24.04.4
LTS.

This project is a fork of Dennis Jenkins' original `rmtdos`
(https://github.com/dennisjenkins75/rmtdos), adapted for MS-DOS based CGA
graphics capture and browser-based viewing.

`rmtdos-cga-web` contains a DOS (FreeDOS or MSDOS or equivalent) TSR that allows
a Linux system on the same LAN to remotely control the DOS system.

## Theory of Operation

There are two main programs, one that runs on DOS (`cgaweb.com`), and the other
that runs on Linux (`rmtdos-cga-web-client`).  There is also a VGA text mode demo
(`vga_demo.com`) program for testing VGA text video modes and displaying test
patterns, and a CGA graphics mode demo (`cga_demo.com`) for testing the web
graphics viewer.

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
1. Experimental: run with `--put LOCAL REMOTE` and `-d` to upload one file to
   the DOS machine.
1. Run with `-h` to see usage and command line options.

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

By default the web viewer only listens on `127.0.0.1`, so it is reachable from
the Linux machine itself or through an SSH tunnel.  To expose it directly on the
LAN, pass `-W 0.0.0.0:8080`.  The web viewer has no authentication, so only use
`-W 0.0.0.0:8080` on a trusted network.

![Client menu](/images/menu.png)

The displayed columns are:

1. Ordinal id for the DOS system (0 .. n-1)
1. MAC address
1. VGA mode (text modes are 0,1,2,3).
1. Width and Height of the screen.
1. Time since last packet received from the DOS system.

Then select the client that you want to connect.

Sample screenshot of the ncurses text-based remote DOS shell.

![Text-mode DOS remote session](/images/rmt_dos_screen.png)

Split screen view of the remote DOS shell and the web session CGA output of
`cga_demo.com`.

![CGA demo in the web viewer](/images/live.png)

Split screen view of the remote DOS shell and the web session CGA output of the
startup screen of Space Quest.

![Text remote session and CGA web viewer](/images/defrag.png)

## Building

1. Install ["dev86"](https://github.com/lkundrak/dev86), which provides a
   16-bit x86 compiler, assembler, and linker.
1. `make`
1. `sudo make setcap` (optional, see https://stackoverflow.com/a/46466642).
1. Copy `out/cgaweb.com`, `out/cga_demo.com` (optional), and
   `out/vga_demo.com` (optional) to a DOS system.

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

## Usage Notes

Press `CTRL-]` or `ALT-ESCAPE` to exit `rmtdos-cga-web-client`.  `CTRL-]` is
useful when running the client over SSH from a Windows host, where `ALT-ESCAPE`
is commonly captured by Windows to cycle open windows.

Use `ALT-X` to exit `vga_demo.com`, and `ALT-V` to cycle its video mode.  Those
are DOS-side demo keys sent through the remote keyboard path; they do not exit
the Linux client.

Run `cga_demo.com` on the DOS machine while `cgaweb.com` is resident and the
Linux client is running with `-w` or `-W` to test CGA graphics frames.  The demo
cycles through BIOS CGA graphics modes `04h`, `05h`, and `06h`; press `M` to
switch modes, `SPACE` to redraw the current pattern, and `X` or `ESC` to exit
back to text mode.

The browser viewer renders modes `04h` and `05h` with classic CGA color
palettes, and mode `06h` as black and white.  It does not currently capture
dynamic CGA palette-register changes.

To inspect the raw Ethernet traffic, use `tcpdump 'ether proto 0x80ab'` or the
Wireshark filter `"eth.type == 0x80ab"`.

## File Transfer

Experimental upload support can copy one file from Linux to the DOS current
directory.  Load the current `cgaweb.com` TSR on the DOS machine normally; no
special DOS-side flag is required for receiving files.  The first version is
intentionally simple and requires the target DOS machine's MAC address:

```
sudo ./rmtdos-cga-web-client -i enp2s0 -d 00:07:40:19:1a:4d \
  --put ./cga_demo.com CGA_DEMO.COM
```

Successful upload from Linux to the DOS machine:

![Successful file upload with --put](/images/cgaweb_put.png)

File upload uses the same raw Ethernet transport as the remote screen session.
DOS writes are processed from the DOS idle interrupt, so uploads work best while
the DOS machine is sitting at the command prompt or otherwise calling DOS idle.

The current upload path is stop-and-wait, one file at a time, and the remote
filename must fit in 63 characters.  Download support is not implemented yet.
The `-d` option shown by `cgaweb.com -h` is a debug-overlay flag inherited from
the original TSR code; normal builds do not need it.

## Possible Future Work

Possible next file-transfer steps include download support, directory listings,
and a friendlier target picker that can upload to the selected ncurses session
without typing a MAC address.
