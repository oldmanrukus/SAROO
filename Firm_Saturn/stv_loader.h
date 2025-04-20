#ifndef STV_LOADER_H
#define STV_LOADER_H

#include <stdint.h>
#include <stdbool.h>
#include "ff.h"
#include "diskio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Launch the ST-V BIOS and game from SD card */
bool stv_launch(const char* biosPath, const char* gameDir);

/* VBlank interrupt handler for JAMMA outputs */
void VBlankHandler(void);

/* Disk I/O functions for SD card (provided in stv_loader.c) */
DSTATUS MMC_disk_initialize(void);
DSTATUS MMC_disk_status(void);
DRESULT MMC_disk_read(BYTE *buff, LBA_t sector, UINT count);
DRESULT MMC_disk_write(const BYTE *buff, LBA_t sector, UINT count);
DRESULT MMC_disk_ioctl(BYTE cmd, void *buff);

#ifdef __cplusplus
}
#endif

#endif /* STV_LOADER_H */
