#ifndef _STV_LOADER_H_
#define _STV_LOADER_H_

#include "main.h"

/*****************************************************************************/
/*
 * ST-V (Sega Titan Video) loader.
 *
 * The ST-V is Saturn-derived arcade hardware. Its BIOS lives at SH-2 address
 * 0x00000000 and its game ROMs live in A-bus CS0 at 0x02000000. SAROO sits in
 * the cartridge slot, which is A-bus CS0 only -- there is no way to present
 * memory at 0x00000000 on a retail Saturn, so the ST-V BIOS cannot be executed
 * in place. This loader therefore does not use the ST-V BIOS at all: it places
 * the game ROM set at the cart addresses the game expects, brings the hardware
 * to the state the ST-V BIOS would have left it in, and jumps into the cart.
 *
 * See doc/stv.md for the memory map, the SD card layout and the parts that
 * still need to be characterised on hardware.
 */
/*****************************************************************************/

/* Cart-space (A-bus CS0) layout used by the loader, as byte addresses in the
 * Saturn's cached view of CS0. */
#define STV_CART_BASE   0x02000000  /* ST-V cart offset 0 == IC13 */
#define STV_CART_END    0x04000000  /* end of the 32MB CS0 window */

/* The MCU can only reach the low 16MB of SDRAM, so read_file() can only land
 * data below this address. Anything above it has to be staged and relocated
 * by the SH-2. */
#define STV_MCU_LIMIT   0x03000000

/* IC13 is staged here before being copied down over SAROO's own code. 1MB at
 * 0x02100000 is above SAROO's metadata (IMGINFO/SYSINFO/SAVINFO/TMPBUFF all
 * live below 0x020d0000) and below IC2 at cart+0x400000. */
#define STV_IC13_STAGE  0x02100000
#define STV_IC13_MAX    0x00100000

/* Staging window for ROMs whose final home is above STV_MCU_LIMIT. This is
 * IC2's final address, so those ROMs must be loaded before IC2 is. */
#define STV_HI_STAGE    0x02400000

/* ST-V input state published by the loader for a per-game input hook to read.
 * Cart space above the ROM set: games do not write their own ROM window, so
 * this survives where a work-RAM address would not. */
#define STV_INPUT_PORT  0x03f80000
#define STV_INPUT_SIZE  0x10

/*****************************************************************************/
/* ROM set description */

#define STV_ROM_MAX     8

/* Per-ROM flags */
#define STV_ROM_SWAP    0x0001  /* byte-swap each 16-bit word after loading */

/* Per-game flags */
#define STV_GAME_SWAP   0x0001  /* default STV_ROM_SWAP for every ROM */

typedef struct {
	char *name;     /* file name inside /SAROO/STV/<id>/ */
	u32   offset;   /* destination offset within ST-V cart space */
	u32   size;     /* expected size in bytes, 0 = whole file */
	u32   flags;
}STV_ROM;

typedef struct {
	char *id;       /* directory name under /SAROO/STV */
	char *title;
	u32   entry;    /* entry point, cart-relative */
	u32   flags;
	STV_ROM rom[STV_ROM_MAX];
}STV_GAME;

/*****************************************************************************/

/* Look up a built-in ROM map by directory name. NULL if the game is unknown. */
STV_GAME *stv_find_game(char *id);

/* Fills the IMGINFO list the selection menu reads with the games found under
 * /SAROO/STV, and returns how many there are. Uses only SSCMD_FILERD, so no
 * MCU firmware change is needed. Overwrites the MCU's disc list, so the caller
 * must reissue SSCMD_LISTDISC when it is done. */
int stv_build_list(void);

/* Load and run the game in /SAROO/STV/<id>. Only returns on failure, with a
 * negative STV_E_* code. On success the MCU mailbox and SAROO's own Saturn
 * code have been overwritten, so the only way back to the boot menu is a
 * power cycle. */
int stv_launch(char *id);

/* Error codes */
#define STV_E_UNKNOWN   -1  /* no ROM map for this directory */
#define STV_E_READ      -2  /* a ROM file could not be read */
#define STV_E_SIZE      -3  /* a ROM file was not the expected size */
#define STV_E_LAYOUT    -4  /* ROM set does not fit the cart window */

/*****************************************************************************/

#endif
