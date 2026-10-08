#ifndef __RKIMAGE_H__
#define __RKIMAGE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <x.h>

#define RKIMAGE_MAX_ITEMS 64
#define RKIMAGE_NAME_SIZE 64

struct rkimage_item {
	char name[RKIMAGE_NAME_SIZE];
	char file[RKIMAGE_NAME_SIZE];
	uint64_t data_offset;
	uint32_t flash_offset;
	uint32_t flash_sectors;
	uint64_t size;
};

struct rkimage {
	char path[512];
	uint64_t file_size;
	uint32_t version;
	uint16_t year;
	uint8_t month;
	uint8_t day;
	uint8_t hour;
	uint8_t minute;
	uint8_t second;
	uint32_t chip_tag;
	uint64_t boot_offset;
	uint64_t boot_size;
	uint64_t firmware_offset;
	uint64_t firmware_size;
	char model[RKIMAGE_NAME_SIZE];
	int item_count;
	struct rkimage_item items[RKIMAGE_MAX_ITEMS];
};

int rkimage_open(const char * path, struct rkimage * image, char * error, size_t error_size);
const struct rkimage_item * rkimage_find_item(const struct rkimage * image, const char * name);
int rkimage_read(const struct rkimage * image, uint64_t offset, void * data, size_t size);
int rkimage_extract(const struct rkimage * image, const struct rkimage_item * item,
	const char * output);

#ifdef __cplusplus
}
#endif

#endif
