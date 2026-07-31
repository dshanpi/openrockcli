#ifndef __RKGPT_H__
#define __RKGPT_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <x.h>

#define RKGPT_SECTOR_SIZE 512U
#define RKGPT_MASTER_SECTORS 34U
#define RKGPT_BACKUP_SECTORS 33U

int rkgpt_build(const void * parameter, size_t parameter_size, uint32_t disk_sectors,
	uint8_t master[RKGPT_MASTER_SECTORS * RKGPT_SECTOR_SIZE],
	uint8_t backup[RKGPT_BACKUP_SECTORS * RKGPT_SECTOR_SIZE],
	char * error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif
