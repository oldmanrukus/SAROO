// stv_loader.c
// ST‑V arcade loader for SAROO — C implementation (compatible with Saturn Orbit SDK)

#include "stv_loader.h"
#include "ff.h"            // Chan’s FatFs
#include "diskio.h"        // Disk I/O interface
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

// -----------------------------------------------------------------------------
//  Configuration Constants
// -----------------------------------------------------------------------------
#define CART_BASE      ((uint32_t)0x02000000)
#define STV_BIOS_ADDR  ((uint32_t)0x06020000)
#define CREDIT_ADDR    ((uint32_t)0x06000350)

// SMPC registers
#define SMPC_BASE      ((uintptr_t)0x20100000)
#define SMPC_REG(o)    (*(volatile uint8_t*)(SMPC_BASE + (o)))
#define IOSEL1         0x01F
#define IOSEL2         0x01E
#define PDR1           0x075
#define DDR1           0x079
#define PDR2           0x074
#define DDR2           0x078

// Controller button bit‑masks
#define BTN_UP     0x0001
#define BTN_DOWN   0x0002
#define BTN_LEFT   0x0004
#define BTN_RIGHT  0x0008
#define BTN_A      0x0010
#define BTN_B      0x0020
#define BTN_C      0x0040
#define BTN_START  0x0080
#define BTN_X      0x0100
#define BTN_Y      0x0200
#define BTN_Z      0x0400
#define BTN_L      0x0800
#define BTN_R      0x1000

// -----------------------------------------------------------------------------
//  FatFs objects
// -----------------------------------------------------------------------------
static FATFS   fs;
static DIR     dir;
static FILINFO fno;
static FIL     fil;

// -----------------------------------------------------------------------------
//  Low‑level FatFs wrappers
// -----------------------------------------------------------------------------
static bool mountFs(void) {
    return (f_mount(&fs, "", 1) == FR_OK);
}

static bool changeDir(const char* path) {
    return (f_chdir(path) == FR_OK);
}

static bool listNext(char* outName, int* isDir) {
    FRESULT res = f_readdir(&dir, &fno);
    if (res != FR_OK || fno.fname[0] == '\0')
        return false;
    strncpy(outName, fno.fname, 63);
    outName[63] = '\0';
    *isDir = (fno.fattrib & AM_DIR) ? 1 : 0;
    return true;
}

static int loadFile(const char* name, uint32_t addr) {
    if (f_open(&fil, name, FA_READ) != FR_OK)
        return -1;
    UINT br;
    uint8_t* dst = (uint8_t*)addr;
    do {
        if (f_read(&fil, dst, 4096, &br) != FR_OK) {
            f_close(&fil);
            return -1;
        }
        dst += br;
    } while (br == 4096);
    f_close(&fil);
    int size = (int)(dst - (uint8_t*)addr);
    return (size + 1) & ~1;
}

// -----------------------------------------------------------------------------
//  Controller → JAMMA I/O conversion
// -----------------------------------------------------------------------------
static void configureDirectIO(void) {
    SMPC_REG(IOSEL1) = 1;
    SMPC_REG(IOSEL2) = 1;
    SMPC_REG(DDR1 ) = 0x7F;
    SMPC_REG(DDR2 ) = 0x7F;
}

struct Pad { uint16_t held, press; };
extern int pad_read(void);

static struct Pad readPad(int port) {
    static uint16_t prevState[2] = {0, 0};
    static uint32_t lastCombined = 0;
    static bool newRead = false;
    struct Pad p = {0,0};
    uint16_t current = 0;
    uint32_t all;
    if (port == 0) {
        all = (uint32_t)pad_read();
        lastCombined = all;
        newRead = true;
        current = (uint16_t)(all & 0xFFFF);
    } else {
        if (!newRead) {
            all = (uint32_t)pad_read();
            lastCombined = all;
        }
        newRead = false;
        current = (uint16_t)((lastCombined >> 16) & 0xFFFF);
    }
    p.held = current;
    p.press = (uint16_t)(current & ~prevState[port]);
    prevState[port] = current;
    return p;
}

void VBlankHandler(void) {
    static bool coinLatch = false;
    struct Pad p1 = readPad(0), p2 = readPad(1);
    uint8_t out1 = 0x7F, out2 = 0x7F;

    if (p1.held & BTN_UP)    out1 &= ~(1<<0);
    if (p1.held & BTN_DOWN)  out1 &= ~(1<<1);
    if (p1.held & BTN_LEFT)  out1 &= ~(1<<2);
    if (p1.held & BTN_RIGHT) out1 &= ~(1<<3);
    if (p1.held & BTN_A)     out1 &= ~(1<<4);
    if (p1.held & BTN_B)     out1 &= ~(1<<5);
    if (p1.held & BTN_C)     out1 &= ~(1<<6);
    if (p1.press & BTN_X)    coinLatch = true;

    if (p2.held & BTN_UP)    out2 &= ~(1<<0);
    if (p2.held & BTN_DOWN)  out2 &= ~(1<<1);
    if (p2.held & BTN_LEFT)  out2 &= ~(1<<2);
    if (p2.held & BTN_RIGHT) out2 &= ~(1<<3);
    if (p2.held & BTN_A)     out2 &= ~(1<<4);
    if (p2.held & BTN_B)     out2 &= ~(1<<5);
    if (p2.held & BTN_C)     out2 &= ~(1<<6);

    SMPC_REG(PDR1) = out1;
    SMPC_REG(PDR2) = out2;

    if (coinLatch) {
        uint16_t* credits = (uint16_t*)CREDIT_ADDR;
        (*credits)++;
        coinLatch = false;
    }
}

// -----------------------------------------------------------------------------
//  Launch ST‑V BIOS + ROMs
// -----------------------------------------------------------------------------
bool stv_launch(const char* biosPath, const char* gameDir) {
    if (!mountFs()) {
        printf("FS mount failed\n");
        return false;
    }
    if (!changeDir(gameDir)) {
        printf("Directory not found: %s\n", gameDir);
        return false;
    }
    if (loadFile(biosPath, STV_BIOS_ADDR) < 0) {
        printf("BIOS load failed\n");
        return false;
    }
    if (f_opendir(&dir, ".") != FR_OK) {
        printf("Failed to open game dir\n");
        return false;
    }
    uint32_t addr = CART_BASE;
    char nameBuf[64];
    int isDir;
    while (listNext(nameBuf, &isDir)) {
        if (isDir) continue;
        int sz = loadFile(nameBuf, addr);
        if (sz < 0) {
            printf("Failed to load %s\n", nameBuf);
            return false;
        }
        addr += (uint32_t)sz;
    }
    configureDirectIO();

    __asm__ volatile("clrpsw i" ::: "memory");

    ((void(*)(void))STV_BIOS_ADDR)();
    return false;
}

// -----------------------------------------------------------------------------
//  FatFs disk I/O stubs for SD/MMC (implement these yourself)
// -----------------------------------------------------------------------------
DSTATUS MMC_disk_initialize(void) { return 0; }
DSTATUS MMC_disk_status(void)     { return 0; }
DRESULT MMC_disk_read(BYTE *b,LBA_t s,UINT c)    { (void)b; (void)s; (void)c; return RES_ERROR; }
DRESULT MMC_disk_write(const BYTE *b,LBA_t s,UINT c) { (void)b; (void)s; (void)c; return RES_WRPRT; }
DRESULT MMC_disk_ioctl(BYTE cmd, void *buff)     { (void)cmd; (void)buff; return RES_PARERR; }
