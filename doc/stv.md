# ST-V support

SAROO can load ST-V (Sega Titan Video) arcade ROM sets into cartridge space and
hand control to them. This document describes how that works, what is known to
be correct, and what still has to be characterised on hardware.

**Status: the loading path is implemented and verified to build; the hand-off has
not been validated on hardware.** Inputs are not yet wired to a running game.
Read "What is not done" before expecting a game to play.


## Why the ST-V BIOS is not used

ST-V is Saturn-derived: same SH-2 pair, SCU, VDP1, VDP2, SCSP and SMPC. The two
differences that matter are:

| | Saturn | ST-V |
|---|---|---|
| `0x00000000` | motherboard mask boot ROM | ST-V BIOS (512KB) |
| `0x02000000` | A-bus CS0, cartridge slot | game cartridge ROM |

An ST-V BIOS expects to execute at `0x00000000`. SAROO is a cartridge-slot
device and the cartridge connector carries A-bus CS0/CS1/CS2 only — nothing on
the cart slot can drive the address the boot ROM answers. So on a retail Saturn
there is no way to put the ST-V BIOS where it needs to run, and no amount of
loading it elsewhere helps, because it is built for address 0.

The loader therefore skips the ST-V BIOS. It places the ROM set at the cart
addresses the game expects, brings the hardware to a quiet known state, and
jumps into the cart. Whatever BIOS services a given game actually depends on
have to be supplied per game.


## Cart space and the loading order

The FPGA maps the whole Saturn A-bus CS0 window into SAROO's SDRAM
(`FPGA/SSMaster.v`, `ss_ram_addr`), so the SH-2 sees 32MB at
`0x02000000`–`0x03ffffff`. Two constraints shape how ROMs get there.

**The MCU can only write the low 16MB.** `SSCMD_FILERD` targets
`(baddr & 0x00ffffff) | 0x61000000` (`Firm_MCU/Saturn/saturn_main.c`), and the
FPGA only gives the STM32 side `fsmc_addr[23:0]`. So `read_file()` can only land
data below `0x03000000`. Anything above that is read into a staging window and
relocated by the SH-2.

**IC13 wants the address SAROO itself occupies.** `Firm_Saturn/ldscript` links
this firmware at `0x02000000` (128KB, including its stack in `.bss`), and the
MCU mailbox lives at `TMPBUFF_ADDR` = `0x020c7000`. All of that is inside IC13's
first megabyte.

The resulting layout:

```
02000000  IC13 final home            <- also where this firmware runs from
02080000  IMGINFO   (MCU game list)
020a3000  SYSINFO
020a4000  SAVINFO
020c7000  TMPBUFF   (MCU mailbox)
02100000  IC13 staging area          STV_IC13_STAGE, 1MB
02400000  IC2 final home / staging   STV_HI_STAGE, 4MB
...
03000000  STV_MCU_LIMIT              MCU cannot write at or above here
03f80000  ST-V input mailbox         STV_INPUT_PORT
04000000  end of the CS0 window
```

and the loading order, which is what makes it safe:

1. **High ROMs** (final home at or above `0x03000000`), one at a time: read into
   `STV_HI_STAGE`, then copied up by the SH-2. These go first because step 2
   claims the staging window.
2. **Low ROMs**, read straight to their final address.
3. **IC13**, read into `STV_IC13_STAGE`.
4. A ~64 byte stub is copied into LWRAM at `0x20280000`, the stack is moved into
   LWRAM, and the stub copies IC13 down over `0x02000000`, purges the SH-2 cache
   and jumps to the entry point.

The stub has to be self-contained — once it starts copying there is no valid code
left at `0x02000000`, so a call to another function or a read of a global would
land in ROM data. It takes its parameters from a block at `0x20280200` so it
needs no stack frame on entry, and it has been checked in the disassembly to
contain no cart-space references.

All cart-space reads and writes in the loader go through the cache-through mirror
(`0x22000000`/`0x23000000`), because the MCU writes SDRAM directly and never
invalidates the SH-2 cache. The stub purges the cache before jumping, since the
cache can still hold lines of this firmware's own code at the addresses IC13 now
occupies.

Once the stub runs, the MCU mailbox and SAROO's Saturn code are gone. Returning
to the boot menu needs a power cycle.


## SD card layout

One directory per game under `/SAROO/STV`:

```
/SAROO/STV/Diehard/epr-19119.13
/SAROO/STV/Diehard/mpr-19115.2
/SAROO/STV/Diehard/mpr-19114.3
/SAROO/STV/Diehard/mpr-19113.4
/SAROO/STV/Diehard/mpr-19112.5
/SAROO/STV/Diehard/mpr-19111.6
/SAROO/STV/Diehard/rommap.txt      (optional)
```

The directory name is what appears in the menu and what selects the built-in ROM
map. Pick "ST-V Arcade" in the boot menu to list them.


## ROM maps

The ST-V cartridge layout is IC13 (the program ROM) at cart+0, then the mask
ROMs at 4MB strides: IC2 at `+0x400000`, IC3 at `+0x800000`, IC4 at
`+0xc00000`, IC5 at `+0x1000000`, IC6 at `+0x1400000`, IC7 at `+0x1800000`.

`stv_games[]` in `Firm_Saturn/stv_loader.c` carries built-in maps. **The file
names there are the usual MAME names and have not been checked against a real
dump.** If they do not match your set, add a `rommap.txt` to the game directory
rather than rebuilding the firmware — the loader prefers it whenever one exists:

```
# offset     size       file            [swap|noswap]
0x0000000    0x0100000  epr-19119.13
0x0400000    0x0400000  mpr-19115.2
0x0800000    0x0400000  mpr-19114.3
0x0c00000    0x0400000  mpr-19113.4
0x1000000    0x0400000  mpr-19112.5
0x1400000    0x0400000  mpr-19111.6

entry        0x0000000
swap         1
title        Die Hard Arcade
```

* `offset` is relative to cart+0. There must be an entry at offset 0.
* `size` of `0` means "whole file". ROMs above `0x03000000` need an explicit
  size, because they are staged.
* `swap` byte-swaps each 16-bit word. It defaults to on: ST-V mask ROMs store
  their words in the opposite byte order to the big-endian SH-2, which is what
  MAME's `ROM_LOAD16_WORD_SWAP` does for this platform. **If a set loads but
  renders as garbage, turn this off first** — it is the least certain assumption
  in the loader.
* `entry` is the cart-relative entry point. `0` means cart+0.


## Diagnosing a failed boot

The loader logs every step. Enable the serial monitor (`debug_flag & 1`) and
watch it — once the hand-off happens the screen is blanked and serial is the only
channel left. A successful load looks like:

```
======== ST-V loader ========
Game dir: /SAROO/STV/Diehard
Title: Die Hard Arcade
Loading high ROMs (staged via 02400000):
  mpr-19112.5      -> 02400000 4194304 bytes
    relocating 02400000 -> 03000000
  mpr-19111.6      -> 02400000 4194304 bytes
    relocating 02400000 -> 03400000
Loading low ROMs:
  mpr-19115.2      -> 02400000 4194304 bytes
  mpr-19114.3      -> 02800000 4194304 bytes
  mpr-19113.4      -> 02c00000 4194304 bytes
Loading IC13 (staged via 02100000):
  epr-19119.13     -> 02100000 1048576 bytes
IC13 header:
  0000: ...
Init hardware ...
Init inputs ...
IC13 02100000 -> 02000000 (1048576 bytes)
Entry point: 02000000
Handing over to Die Hard Arcade.
```

The **IC13 header dump** is the important part when nothing happens after the
hand-off. The entry point currently defaults to cart+0, which is an assumption,
not a known fact. The dump is there so the real header format can be identified
and a correct `entry` set in `rommap.txt`.

Error returns from `stv_launch()`:

| | |
|---|---|
| `-1` | no ROM map for this directory |
| `-2` | a ROM file could not be read |
| `-3` | a ROM file was not the size the map declared |
| `-4` | the map does not fit the cart window or collides with a staging area |


## What is not done

These are the open items, roughly in the order they will need attention.

**Entry point.** Defaults to cart+0. Unverified. The header dump exists to settle
it.

**Inputs.** ST-V reads its JAMMA controls through the SMPC I/O ports in direct
mode. The loader configures the ports that way and publishes one translated
snapshot at `STV_INPUT_PORT`, so an unpatched game reads a stable idle state
rather than noise — but nothing keeps updating it after the jump. Doing that
needs code still running once the game owns the machine, which means hooking
whatever the game itself polls. That is a per-game patch and it cannot be traced
until the game boots far enough to poll anything. `stv_translate_pad()` is the
translation half, ready for that hook; the bit assignments in it are the
conventional ST-V layout and are also unverified.

**Byte order.** `swap` defaults to on, per MAME's handling of this platform. Not
confirmed against hardware.

**Hardware init.** `stv_init_hw()` does the unambiguous subset: interrupts off,
sound silenced, video blanked, VDP1 given a terminated empty command list, SCU
DMA idle, CS0 switched to plain data with SAROO's CDBLOCK emulation disabled. It
is not a reproduction of the ST-V BIOS boot sequence. Anything a game turns out
to need on top of this belongs there.

**Sound.** The SCSP is silenced and the sound CPU held. ST-V games drive the SCSP
through a sound driver the BIOS would normally have started.

**NVRAM / bookkeeping.** ST-V stores settings and credits in cartridge NVRAM.
Nothing emulates it, so anything a game expects to read back from it is absent.


## Files

| | |
|---|---|
| `Firm_Saturn/stv_loader.c`, `.h` | the loader |
| `Firm_Saturn/main.c` | menu entry, `select_stv()`, `sel_mode == 2` |
| `Firm_Saturn/main.h` | `SSCMD_LISTSTV` |
| `Firm_MCU/Saturn/cdimg.c` | `list_stv()`, enumerates `/SAROO/STV` |
| `Firm_MCU/Saturn/saturn_main.c` | `SSCMD_LISTSTV` dispatch |
| `Firm_MCU/Saturn/cdc.h` | `SSCMD_LISTSTV`, `list_stv()` |
