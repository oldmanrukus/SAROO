/*
 * ST-V (Sega Titan Video) loader for SAROO.
 *
 * The ST-V is Saturn-derived arcade hardware: same SH-2 pair, SCU, VDP1, VDP2,
 * SCSP and SMPC, but its BIOS sits at SH-2 address 0x00000000 and its game ROMs
 * sit in A-bus CS0 from 0x02000000 up.
 *
 * SAROO is a cartridge-slot device, and the cartridge slot carries A-bus CS0/1/2
 * only. Address 0x00000000 on a retail Saturn is the motherboard's mask boot ROM,
 * so there is no way for SAROO to present the ST-V BIOS where the ST-V BIOS
 * expects to run. This loader therefore does not use the ST-V BIOS at all. It
 * places the ROM set where the game expects it, brings the hardware to a sane
 * state, and jumps into the cart.
 *
 * Two SAROO-specific constraints shape the loading order:
 *
 *   - The MCU can only write the low 16MB of SDRAM. SSCMD_FILERD computes
 *     (baddr & 0x00ffffff) | 0x61000000, and the FPGA only gives the STM32 side
 *     fsmc_addr[23:0] (FPGA/SSMaster.v). So read_file() can only land data in
 *     cart 0x02000000..0x02ffffff. The SH-2 sees all 32MB, so anything destined
 *     for 0x03000000 and up is read into a staging window and relocated here.
 *
 *   - ST-V's IC13 belongs at cart+0, which is exactly where this firmware runs
 *     from (Firm_Saturn/ldscript links at 0x02000000) and where the MCU mailbox
 *     lives (TMPBUFF_ADDR). IC13 is therefore staged too, and copied into place
 *     by a small stub running from LWRAM after everything else is done.
 *
 * See doc/stv.md for the SD card layout and for the parts that still have to be
 * characterised on hardware.
 */

#include "main.h"
#include "smpc.h"
#include "vdp2.h"
#include "stv_loader.h"

/**********************************************************/

/* SCU interrupt control, not covered by smpc.h */
#define SCU_IMS         REG(SCU_BASE+0xa0)
#define SCU_IST         REG(SCU_BASE+0xa4)

/* Cache-through mirror of a cached address. All cart-space ROM writes go
 * through this, so nothing is left sitting in the SH-2 cache when the game
 * starts reading the ROM through the cached window. */
#define CT(a)           ((u32)(a) | 0x20000000)

/* Hand-off stub, in LWRAM, addressed cache-through so that the code we just
 * wrote is what actually executes. game_load.c already uses 0x00280000 as
 * scratch for the same trick, so this region is known to be free here. */
#define STV_STUB_ADDR   0x20280000  /* stub code, STV_STUB_SIZE bytes */
#define STV_STUB_ARGS   0x20280200  /* src, dst, count, entry */
#define STV_STUB_STACK  0x2027ff00  /* stub's stack, grows down, below the code */
#define STV_STUB_SIZE   0x100

/**********************************************************/
/*
 * Built-in ROM maps.
 *
 * The ST-V cartridge layout is IC13 (the program/boot ROM) at cart+0, then the
 * mask ROMs at 4MB strides: IC2 at +0x400000, IC3 at +0x800000, IC4 at
 * +0xc00000, IC5 at +0x1000000, IC6 at +0x1400000, IC7 at +0x1800000.
 *
 * The file names below are the usual MAME names for the set. If your dump uses
 * different names, or the set differs from what is listed here, drop a
 * rommap.txt into the game directory rather than editing this table -- see
 * doc/stv.md. The loader prefers rommap.txt whenever one is present.
 *
 * STV_GAME_SWAP is on because ST-V mask ROMs store their 16-bit words in the
 * opposite byte order to the big-endian SH-2 (this is what MAME's
 * ROM_LOAD16_WORD_SWAP does for this platform). If a set loads but renders as
 * garbage, that flag is the first thing to try turning off.
 */

static STV_GAME stv_games[] = {
	{
		"Diehard", "Die Hard Arcade", 0, STV_GAME_SWAP,
		{
			{ "epr-19119.13", 0x0000000, 0x0100000, 0 },
			{ "mpr-19115.2",  0x0400000, 0x0400000, 0 },
			{ "mpr-19114.3",  0x0800000, 0x0400000, 0 },
			{ "mpr-19113.4",  0x0c00000, 0x0400000, 0 },
			{ "mpr-19112.5",  0x1000000, 0x0400000, 0 },
			{ "mpr-19111.6",  0x1400000, 0x0400000, 0 },
			{ NULL, 0, 0, 0 },
		}
	},
	{
		"Dynamite", "Dynamite Deka", 0, STV_GAME_SWAP,
		{
			{ "epr-19114.13", 0x0000000, 0x0100000, 0 },
			{ "mpr-19115.2",  0x0400000, 0x0400000, 0 },
			{ "mpr-19114.3",  0x0800000, 0x0400000, 0 },
			{ "mpr-19113.4",  0x0c00000, 0x0400000, 0 },
			{ "mpr-19112.5",  0x1000000, 0x0400000, 0 },
			{ "mpr-19111.6",  0x1400000, 0x0400000, 0 },
			{ NULL, 0, 0, 0 },
		}
	},
};

#define STV_GAME_NR   ((int)(sizeof(stv_games)/sizeof(stv_games[0])))


STV_GAME *stv_find_game(char *id)
{
	for(int i=0; i<STV_GAME_NR; i++){
		if(strcmp(stv_games[i].id, id)==0)
			return &stv_games[i];
	}

	return NULL;
}


/**********************************************************/
/*
 * rommap.txt override.
 *
 * One ROM per line, plus optional directives:
 *
 *     # offset     size       file           [swap|noswap]
 *     0x0000000    0x0100000  epr-19119.13
 *     0x0400000    0x0400000  mpr-19115.2
 *     entry        0x0000000
 *     swap         1
 *     title        Die Hard Arcade
 *
 * A size of 0 means "whole file". Blank lines and '#' comments are ignored.
 */

#define ROMMAP_MAX   1024

static STV_GAME  custom_game;
static char      custom_id[64];
static char      custom_title[64];
static char      custom_names[STV_ROM_MAX][40];
static char      rommap_buf[ROMMAP_MAX];


static char *skip_blank(char *p)
{
	while(*p==' ' || *p=='\t')
		p++;
	return p;
}


/* Copies one whitespace-delimited token into dst, returns the next position. */
static char *get_token(char *p, char *dst, int len)
{
	int i = 0;

	p = skip_blank(p);
	while(*p && *p!=' ' && *p!='\t' && *p!='\r'){
		if(i<len-1)
			dst[i++] = *p;
		p++;
	}
	dst[i] = 0;

	return p;
}


static int parse_rommap(char *id, char *buf, int size)
{
	STV_GAME *game = &custom_game;
	char tok[40];
	char *p = buf;
	char *end = buf + size;
	int nrom = 0;
	int swap_all = 1;

	strncpy(custom_id, id, sizeof(custom_id));
	custom_id[sizeof(custom_id)-1] = 0;
	strcpy(custom_title, custom_id);

	memset(game, 0, sizeof(*game));
	game->id = custom_id;
	game->title = custom_title;

	while(p<end){
		char *line = p;

		/* find the end of this line and terminate it */
		while(p<end && *p!='\n')
			p++;
		if(p<end)
			*p++ = 0;

		line = skip_blank(line);
		if(*line==0 || *line=='#' || *line=='\r')
			continue;

		line = get_token(line, tok, sizeof(tok));

		if(strcmp(tok, "entry")==0){
			get_token(line, tok, sizeof(tok));
			game->entry = strtoul(tok, NULL, 16, NULL);
			continue;
		}
		if(strcmp(tok, "swap")==0){
			get_token(line, tok, sizeof(tok));
			swap_all = (strtoul(tok, NULL, 10, NULL))? 1 : 0;
			continue;
		}
		if(strcmp(tok, "title")==0){
			line = skip_blank(line);
			strncpy(custom_title, line, sizeof(custom_title));
			custom_title[sizeof(custom_title)-1] = 0;
			continue;
		}

		if(nrom>=STV_ROM_MAX-1){
			printk("rommap.txt: more than %d ROMs, ignoring the rest\n",
				STV_ROM_MAX-1);
			break;
		}

		/* offset  size  file  [swap|noswap] */
		STV_ROM *rom = &game->rom[nrom];
		rom->offset = strtoul(tok, NULL, 16, NULL);

		line = get_token(line, tok, sizeof(tok));
		if(tok[0]==0){
			printk("rommap.txt: missing size for offset %08x\n", rom->offset);
			return -1;
		}
		rom->size = strtoul(tok, NULL, 16, NULL);

		line = get_token(line, custom_names[nrom], sizeof(custom_names[0]));
		if(custom_names[nrom][0]==0){
			printk("rommap.txt: missing file name for offset %08x\n", rom->offset);
			return -1;
		}
		rom->name = custom_names[nrom];

		get_token(line, tok, sizeof(tok));
		if(strcmp(tok, "noswap")==0)
			rom->flags = 0;
		else if(strcmp(tok, "swap")==0)
			rom->flags = STV_ROM_SWAP;
		else
			rom->flags = (swap_all)? STV_ROM_SWAP : 0;

		nrom += 1;
	}

	game->rom[nrom].name = NULL;

	if(nrom==0){
		printk("rommap.txt: no ROM lines found\n");
		return -1;
	}

	printk("rommap.txt: %d ROMs, entry=%08x\n", nrom, game->entry);
	return 0;
}


/* Returns the ROM map to use: rommap.txt if the game directory has one,
 * otherwise the built-in table. NULL if neither is available. */
static STV_GAME *stv_get_rommap(char *id)
{
	char path[128];
	int retv;

	/* The MCU writes SDRAM directly, so it never invalidates the SH-2 cache.
	 * Read and parse through the cache-through window or the parser can see a
	 * stale copy of this buffer. */
	char *buf = (char*)CT(rommap_buf);

	snprintf(path, sizeof(path), "/SAROO/STV/%s/rommap.txt", id);
	retv = read_file(path, 0, ROMMAP_MAX-1, buf);
	if(retv>0){
		buf[retv] = 0;
		printk("Using %s\n", path);
		if(parse_rommap(id, buf, retv)==0)
			return &custom_game;
		printk("rommap.txt is unusable, falling back to the built-in map.\n");
	}

	return stv_find_game(id);
}


/**********************************************************/
/* ROM loading */


/*
 * Copies size bytes, byte-swapping each 16-bit word if asked. src and dst are
 * cache-through cart addresses. Used both for in-place swapping (src==dst) and
 * for moving staged ROMs up to their final home.
 *
 * Plain SH-2 loads and stores: SCU DMA cannot do A-bus to A-bus, and both ends
 * of these copies are in cart space.
 */
static void stv_copy(u32 dst, u32 src, u32 size, int swap)
{
	volatile u32 *d = (volatile u32*)dst;
	volatile u32 *s = (volatile u32*)src;
	u32 n = size>>2;

	if(swap){
		while(n--){
			u32 v = *s++;
			*d++ = ((v&0x00ff00ff)<<8) | ((v>>8)&0x00ff00ff);
		}
	}else if(dst!=src){
		while(n--)
			*d++ = *s++;
	}
}


/*
 * Reads one ROM file straight into cart space via the MCU. dest must be a
 * cached cart address, and dest+size must stay below STV_MCU_LIMIT.
 *
 * maxsize bounds the transfer for entries with no declared size, so that a
 * larger-than-expected file cannot be written past the end of its slot.
 */
static int stv_read_rom(char *id, STV_ROM *rom, u32 dest, u32 maxsize)
{
	char path[128];
	u32 want = (rom->size)? rom->size : maxsize;
	int retv;

	if(want > maxsize){
		printk("ROM %s is %08x bytes but only %08x fits at %08x\n",
			rom->name, want, maxsize, dest);
		return STV_E_LAYOUT;
	}

	snprintf(path, sizeof(path), "/SAROO/STV/%s/%s", id, rom->name);

	printk("  %-16s -> %08x ", rom->name, dest);

	retv = read_file(path, 0, want, (void*)CT(dest));
	if(retv<0){
		printk("FAILED (%d)\n", retv);
		printk("    could not read %s\n", path);
		return STV_E_READ;
	}
	printk("%d bytes\n", retv);

	if(rom->size){
		if(retv!=(int)rom->size){
			printk("    expected %d bytes, got %d\n", rom->size, retv);
			return STV_E_SIZE;
		}
	}else{
		if((u32)retv==maxsize)
			printk("    warning: filled the whole %08x byte slot, the file"
				" may be larger than this\n", maxsize);
		rom->size = retv;
	}

	return 0;
}


static int ranges_overlap(u32 a, u32 asize, u32 b, u32 bsize)
{
	if(asize==0 || bsize==0)
		return 0;
	return (a < b+bsize) && (b < a+asize);
}


/* Sanity-checks the map against the cart window, the two staging areas and the
 * input mailbox, so a bad map is reported instead of quietly corrupting
 * something. Entries with no declared size are re-checked as they load. */
static int stv_check_layout(STV_GAME *game)
{
	u32 cart_size = STV_CART_END - STV_CART_BASE;
	int has_ic13 = 0;

	for(int i=0; game->rom[i].name; i++){
		STV_ROM *r = &game->rom[i];
		u32 dest = STV_CART_BASE + r->offset;

		if(r->offset==0){
			has_ic13 = 1;
			if(r->size > STV_IC13_MAX){
				printk("IC13 is %08x bytes, staging area is only %08x\n",
					r->size, STV_IC13_MAX);
				return STV_E_LAYOUT;
			}
			continue;
		}

		if(r->size==0)
			continue;   /* size comes from the file */

		if(r->offset+r->size > cart_size){
			printk("ROM %s at %08x+%08x runs past the %dMB cart window\n",
				r->name, r->offset, r->size, cart_size>>20);
			return STV_E_LAYOUT;
		}
		if(ranges_overlap(dest, r->size, STV_IC13_STAGE, STV_IC13_MAX)){
			printk("ROM %s overlaps the IC13 staging area at %08x\n",
				r->name, STV_IC13_STAGE);
			return STV_E_LAYOUT;
		}
		if(ranges_overlap(dest, r->size, STV_INPUT_PORT, STV_INPUT_SIZE)){
			printk("ROM %s overlaps the input mailbox at %08x\n",
				r->name, STV_INPUT_PORT);
			return STV_E_LAYOUT;
		}
	}

	if(!has_ic13){
		printk("ROM map has no ROM at cart+0 (IC13)\n");
		return STV_E_LAYOUT;
	}

	return 0;
}


/*
 * Loads the whole set. Order matters:
 *
 *   1. ROMs whose final home is at or above STV_MCU_LIMIT, one at a time,
 *      through the staging window, relocated up by the SH-2 after each read.
 *   2. ROMs below STV_MCU_LIMIT, read straight to their final address. This
 *      has to come second because one of them owns the staging window.
 *   3. IC13, into its own staging area. The stub copies it down at hand-off.
 */
static int stv_load_romset(char *id, STV_GAME *game)
{
	int retv;

	printk("Loading high ROMs (staged via %08x):\n", STV_HI_STAGE);
	for(int i=0; game->rom[i].name; i++){
		STV_ROM *r = &game->rom[i];
		u32 dest = STV_CART_BASE + r->offset;

		if(r->offset==0 || dest<STV_MCU_LIMIT)
			continue;

		if(r->size==0){
			printk("ROM %s is above %08x and needs an explicit size to be"
				" staged\n", r->name, STV_MCU_LIMIT);
			return STV_E_LAYOUT;
		}

		retv = stv_read_rom(id, r, STV_HI_STAGE, STV_MCU_LIMIT-STV_HI_STAGE);
		if(retv<0)
			return retv;

		printk("    relocating %08x -> %08x\n", STV_HI_STAGE, dest);
		stv_copy(CT(dest), CT(STV_HI_STAGE), r->size, r->flags & STV_ROM_SWAP);
	}

	printk("Loading low ROMs:\n");
	for(int i=0; game->rom[i].name; i++){
		STV_ROM *r = &game->rom[i];
		u32 dest = STV_CART_BASE + r->offset;

		if(r->offset==0 || dest>=STV_MCU_LIMIT)
			continue;

		retv = stv_read_rom(id, r, dest, STV_MCU_LIMIT-dest);
		if(retv<0)
			return retv;

		if(r->flags & STV_ROM_SWAP)
			stv_copy(CT(dest), CT(dest), r->size, 1);
	}

	printk("Loading IC13 (staged via %08x):\n", STV_IC13_STAGE);
	for(int i=0; game->rom[i].name; i++){
		STV_ROM *r = &game->rom[i];

		if(r->offset!=0)
			continue;

		retv = stv_read_rom(id, r, STV_IC13_STAGE, STV_IC13_MAX);
		if(retv<0)
			return retv;

		if(r->flags & STV_ROM_SWAP)
			stv_copy(CT(STV_IC13_STAGE), CT(STV_IC13_STAGE), r->size, 1);
	}

	return 0;
}


/* IC13's first bytes decide where we jump. Dump them so an unrecognised header
 * can be identified from the serial log. */
static void stv_dump_header(void)
{
	u8 *h = (u8*)CT(STV_IC13_STAGE);

	printk("IC13 header:\n");
	for(int i=0; i<0x40; i+=16){
		printk("  %04x: ", i);
		for(int j=0; j<16; j++)
			printk("%02x ", h[i+j]);
		printk(" ");
		for(int j=0; j<16; j++){
			int c = h[i+j];
			printk("%c", (c>=0x20 && c<0x7f)? c : '.');
		}
		printk("\n");
	}
}


/**********************************************************/
/* Hardware setup */

/* Local copy: main.h's set_imask() is defined inline in ubr_debug.c, so do not
 * depend on an external definition being emitted for it. */
static void stv_set_imask(u32 imask)
{
	u32 sr;

	__asm__ volatile("stc sr,%0" : "=r"(sr));
	sr &= 0xffffff0f;
	sr |= (imask<<4);
	__asm__ volatile("ldc %0,sr" :: "r"(sr));
}


/*
 * Brings the machine to a quiet, known state before handing over. This is not a
 * reproduction of the ST-V BIOS boot sequence -- it is the subset that is
 * unambiguous: interrupts off, sound silent, video blanked, VDP1 given a valid
 * empty command list, DMA idle. Anything a specific game turns out to need on
 * top of this belongs here, discovered on hardware.
 */
static void stv_init_hw(void)
{
	printk("Init hardware ...\n");

	/* interrupts off, at both the SH-2 and the SCU */
	stv_set_imask(15);
	SCU_IMS = 0xbfff;
	SCU_IST = 0;

	/* stop the SH-2 free-running timer SAROO uses for msleep() */
	TCR   = 0;
	FTCSR = 0;

	/* idle the SCU DMA channels */
	SD0EN = 0;
	SD1EN = 0;
	SD2EN = 0;

	/* silence the SCSP: master volume to zero, then hold the sound CPU */
	REG16(0x25b00400) = 0x0000;
	smpc_cmd(SNDOFF);

	/* blank the display; the game sets VDP2 up for itself */
	TVMD = 0x0000;
	BGON = 0x0000;

	/* Some games do not initialise VDP1 before their first draw. Give it a
	 * terminated, empty command list -- the same treatment game_load.c applies
	 * for Saturn discs. */
	memset((u8*)0x25c00200, 0x00, 0x80000-0x200);
	for(int i=0; i<0x80000; i+=0x20){
		REG16(0x25c00000+i) = 0x8000;
	}
	REG16(0x25c7fffe) = 0xffff;

	/* Hand CS0 over as plain data and drop SAROO's CDBLOCK emulation: with
	 * SAROO_EN clear the MCU stops answering CD commands (saturn_cdc.c). The
	 * SDRAM stays mapped at CS0 either way (FPGA: ss_ram_cs = ~SS_CS0). */
	SS_CTRL = CS0_DATA;
}


/**********************************************************/
/* Inputs
 *
 * ST-V reads its JAMMA controls through the SMPC I/O ports in direct mode,
 * which is also the only way for us to read a Saturn pad once the SMPC is in
 * that mode (SMPC command mode and direct mode are mutually exclusive, so
 * pad_read() from main.c cannot be used past this point).
 *
 * What is missing is the other half: presenting translated values to the game.
 * That needs code still running after the jump, which means hooking whatever
 * the game itself polls -- a per-game patch, and the game has to boot before it
 * can be traced. Until then this sets the ports up the ST-V way and publishes
 * one snapshot at STV_INPUT_PORT, so an unpatched game sees a stable idle state
 * rather than noise. See doc/stv.md.
 */

/* ST-V player input bits, active low. */
#define STV_IN_UP       (1<<0)
#define STV_IN_DOWN     (1<<1)
#define STV_IN_LEFT     (1<<2)
#define STV_IN_RIGHT    (1<<3)
#define STV_IN_BTN1     (1<<4)
#define STV_IN_BTN2     (1<<5)
#define STV_IN_BTN3     (1<<6)


/* Reads a digital Saturn pad on port 1 with the SMPC in direct mode. Same
 * multiplexing sequence as the PADMODE_DIRECT path in main.c:pad_read(). */
static int stv_pad_read_direct(void)
{
	u32 bits = 0;

	PDR1 = 0x60; bits |= (PDR1 & 0x8) << 0;
	PDR1 = 0x40; bits |= (PDR1 & 0xf) << 8;
	PDR1 = 0x20; bits |= (PDR1 & 0xf) << 12;
	PDR1 = 0x00; bits |= (PDR1 & 0xf) << 4;
	bits ^= 0xfff8;

	return bits;
}


/* Saturn pad bits -> one ST-V player's active-low input byte. */
static u8 stv_translate_pad(int pad)
{
	u8 out = 0x7f;

	if(pad & PAD_UP)    out &= ~STV_IN_UP;
	if(pad & PAD_DOWN)  out &= ~STV_IN_DOWN;
	if(pad & PAD_LEFT)  out &= ~STV_IN_LEFT;
	if(pad & PAD_RIGHT) out &= ~STV_IN_RIGHT;
	if(pad & PAD_A)     out &= ~STV_IN_BTN1;
	if(pad & PAD_B)     out &= ~STV_IN_BTN2;
	if(pad & PAD_C)     out &= ~STV_IN_BTN3;

	return out;
}


static void stv_init_input(void)
{
	volatile u8 *port = (volatile u8*)CT(STV_INPUT_PORT);
	int pad;

	printk("Init inputs ...\n");

	/* direct mode on both ports; PDR1 bits 5-6 drive the pad's multiplexer */
	EXLE = 0;
	DDR1 = 0x60;
	DDR2 = 0x60;
	IOSEL = IOSEL1 | IOSEL2;

	pad = stv_pad_read_direct();
	port[0] = stv_translate_pad(pad);
	port[1] = 0x7f;

	printk("  pad=%04x  p1=%02x\n", pad, port[0]);
}


/**********************************************************/
/* Hand-off */

/*
 * Copies staged IC13 down over cart+0 and jumps to the game.
 *
 * This runs from LWRAM because it destroys the cart space this firmware
 * executes from. It must stay a leaf function that touches nothing but its
 * locals and integer constants: once the copy starts there is no valid code
 * left at 0x02000000, so a call to another function or a read of a global would
 * land in ROM data. Parameters come from a block at STV_STUB_ARGS rather than
 * from arguments, so no stack frame is needed on entry. The pointers are
 * volatile to stop the compiler turning the loop into a memcpy() call.
 */
static void stv_boot_stub(void)
{
	volatile u32 *args = (volatile u32*)STV_STUB_ARGS;
	volatile u32 *s = (volatile u32*)args[0];
	volatile u32 *d = (volatile u32*)args[1];
	u32 n = args[2]>>2;
	u32 entry = args[3];

	while(n--)
		*d++ = *s++;

	/* The copy went through the cache-through window, so the SH-2 cache can
	 * still hold lines of SAROO's own code at the addresses IC13 now occupies.
	 * Purge, then re-enable -- same sequence game_load.c uses. Without this the
	 * game's first instructions can come out of a stale cache line. */
	*(volatile u8*)0xfffffe92 = 0x10;   /* CCR.CP: purge */
	*(volatile u8*)0xfffffe92 = 0x01;   /* CCR.CE: cache on */

	((void(*)(void))entry)();
}


static int stv_handoff(STV_GAME *game, u32 ic13_size, u32 entry)
{
	volatile u32 *args = (volatile u32*)STV_STUB_ARGS;

	printk("\nIC13 %08x -> %08x (%d bytes)\n",
		STV_IC13_STAGE, STV_CART_BASE, ic13_size);
	printk("Entry point: %08x\n", entry);
	printk("Handing over to %s. SAROO's menu is gone until a power cycle.\n\n",
		game->title);

	args[0] = CT(STV_IC13_STAGE);
	args[1] = CT(STV_CART_BASE);
	args[2] = ic13_size;
	args[3] = entry;

	memcpy((void*)STV_STUB_ADDR, (void*)stv_boot_stub, STV_STUB_SIZE);

	/* Move the stack into LWRAM and jump. SAROO's own stack lives in .bss at
	 * 0x0200xxxx, inside the region the stub is about to overwrite, so it
	 * cannot be used past this point. Nothing after the jmp ever runs. */
	__asm__ volatile(
		"mov  %0, r15\n\t"
		"jmp  @%1\n\t"
		"nop\n\t"
		:
		: "r"(STV_STUB_STACK), "r"(STV_STUB_ADDR)
		: "memory"
	);

	return 0;
}


/**********************************************************/


int stv_launch(char *id)
{
	static char game_id[64];
	STV_GAME *game;
	u32 ic13_size = 0;
	u32 entry;
	int retv;

	if(debug_flag&1)
		sci_init();

	/* The caller's id points into the IMGINFO list in cart space. Keep our
	 * own copy: from here on cart space is being overwritten. */
	strncpy(game_id, id, sizeof(game_id));
	game_id[sizeof(game_id)-1] = 0;

	printk("\n======== ST-V loader ========\n");
	printk("Game dir: /SAROO/STV/%s\n", game_id);

	game = stv_get_rommap(game_id);
	if(game==NULL){
		printk("No ROM map for '%s'. Add a rommap.txt to the game directory"
			" or a built-in entry to stv_games[].\n", game_id);
		return STV_E_UNKNOWN;
	}
	printk("Title: %s\n", game->title);

	if(game->flags & STV_GAME_SWAP){
		for(int i=0; game->rom[i].name; i++)
			game->rom[i].flags |= STV_ROM_SWAP;
	}

	retv = stv_check_layout(game);
	if(retv<0)
		return retv;

	retv = stv_load_romset(game_id, game);
	if(retv<0)
		return retv;

	for(int i=0; game->rom[i].name; i++){
		if(game->rom[i].offset==0)
			ic13_size = game->rom[i].size;
	}
	if(ic13_size==0){
		printk("IC13 loaded with zero size\n");
		return STV_E_LAYOUT;
	}

	stv_dump_header();

	/* game->entry is cart-relative. 0 means "start of IC13", which is the
	 * working assumption until a header field is identified for this set --
	 * the dump above is there to settle that. */
	entry = STV_CART_BASE + game->entry;
	if(game->entry==0)
		printk("No entry point in the map, assuming cart+0.\n");

	stv_init_hw();
	stv_init_input();

	return stv_handoff(game, ic13_size, entry);
}
