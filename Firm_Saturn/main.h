#ifndef MAIN_H
#define MAIN_H

#include <stdint.h>
#include <stdio.h>   /* for printf */
#include <stddef.h>
#include <stdbool.h>

/* Basic types */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;

/* Endian conversion macros */
#define BE32(p)    (((u32)((uint8_t*)(p))[0] << 24) | ((u32)((uint8_t*)(p))[1] << 16) | ((u32)((uint8_t*)(p))[2] << 8) | (u32)((uint8_t*)(p))[3])
#define LE16(p)    (((u32)((uint8_t*)(p))[0]) | ((u32)((uint8_t*)(p))[1] << 8))
#define LE32(p)    (((u32)((uint8_t*)(p))[0]) | ((u32)((uint8_t*)(p))[1] << 8) | ((u32)((uint8_t*)(p))[2] << 16) | ((u32)((uint8_t*)(p))[3] << 24))
#define LE32W(p, val)   do { u32 _v = (u32)(val);                         \
                              ((uint8_t*)(p))[0] = (uint8_t)(_v & 0xFF);      \
                              ((uint8_t*)(p))[1] = (uint8_t)((_v >> 8) & 0xFF);  \
                              ((uint8_t*)(p))[2] = (uint8_t)((_v >> 16) & 0xFF); \
                              ((uint8_t*)(p))[3] = (uint8_t)((_v >> 24) & 0xFF); } while(0)

/* Button bit masks (Saturn controller) */
#define PAD_UP     0x0001
#define PAD_DOWN   0x0002
#define PAD_LEFT   0x0004
#define PAD_RIGHT  0x0008
#define PAD_A      0x0010
#define PAD_B      0x0020
#define PAD_C      0x0040
#define PAD_START  0x0080
#define PAD_X      0x0100
#define PAD_Y      0x0200
#define PAD_Z      0x0400
#define PAD_LT     0x0800  /* Left trigger */
#define PAD_RT     0x1000  /* Right trigger */

/* Helper macro for checking button press */
#define BUTTON_DOWN(ctrl, button)   (((ctrl) & (button)) != 0)

/* Menu return codes */
#define MENU_EXIT    (-1)
#define MENU_RESTART (-2)

/* Text translation macro (no-op for this build) */
#define TT(x)    (x)

/* SAROO communication area and commands */
#define SYSINFO_ADDR   0x00200000                     /* Base address of system info structure */
#define SS_CMD         (*(volatile u32*)(SYSINFO_ADDR + 0x10))
#define SS_ARG         (*(volatile u32*)(SYSINFO_ADDR + 0x14))
#define SS_VER         (*(volatile u32*)(SYSINFO_ADDR + 0x0C))   /* FPGA version in low 8 bits */

/* Command codes for SS_CMD */
#define SSCMD_STARTUP  1
#define SSCMD_UPDATE   2

/* Menu descriptor structure */
typedef struct MENU_DESC {
    int current;
    char title[64];
    char *version;
    int (*handle)(int ctrl);
    /* Additional fields (menu items, count, etc.) could be here */
} MENU_DESC;

/* Global flags for CD operations */
extern int use_sys_bup;
extern int use_sys_load;

#endif /* MAIN_H */
