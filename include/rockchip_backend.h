#ifndef __ROCKCHIP_BACKEND_H__
#define __ROCKCHIP_BACKEND_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <rock.h>

#define ROCKCHIP_BACKEND_MAX_PORT	32
#define ROCKCHIP_BACKEND_MAX_FW_ENTRIES	64
#define ROCKCHIP_BACKEND_MAX_NAME	64

struct rockchip_device_info {
	uint8_t bus;
	char port[ROCKCHIP_BACKEND_MAX_PORT];
	uint16_t vid;
	uint16_t pid;
	char mode[12];
	char chip[16];
};

struct rockchip_firmware_entry {
	int entry_no;
	char file[ROCKCHIP_BACKEND_MAX_NAME];
	char partition[ROCKCHIP_BACKEND_MAX_NAME];
	char type[ROCKCHIP_BACKEND_MAX_NAME];
	uint64_t offset;
	uint64_t size;
};

struct rockchip_firmware_info {
	char path[512];
	char image_type[ROCKCHIP_BACKEND_MAX_NAME];
	char chip_tag[16];
	char version[32];
	char build_time[32];
	uint64_t file_size;
	int entry_count;
	struct rockchip_firmware_entry entries[ROCKCHIP_BACKEND_MAX_FW_ENTRIES];
};

struct rockchip_flash_request {
	const char * firmware;
	const char * mode;
	const char * partitions;
	const char * post_action;
	const char * bus;
	const char * port;
	int verify;
	int verbose;
};

enum rockchip_backend_event_type {
	ROCKCHIP_BACKEND_EVENT_LOG,
	ROCKCHIP_BACKEND_EVENT_STAGE,
	ROCKCHIP_BACKEND_EVENT_PROGRESS,
};

struct rockchip_backend_event {
	enum rockchip_backend_event_type type;
	const char * text;
	int percent;
};

typedef void (*rockchip_backend_event_cb)(const struct rockchip_backend_event * event, void * user);

const char * rockchip_backend_upgrade_tool(void);
int rockchip_backend_scan_devices(struct rockchip_device_info * devices, int max_devices);
int rockchip_backend_scan(int detailed, int verbose);
int rockchip_backend_list_devices(void);
int rockchip_backend_read_firmware_info(const char * firmware, struct rockchip_firmware_info * info);
int rockchip_backend_inspect_firmware(const char * firmware);
int rockchip_backend_unpack_firmware(const char * firmware, const char * outdir);
int rockchip_backend_flash(const struct rockchip_flash_request * request);
int rockchip_backend_flash_with_cb(const struct rockchip_flash_request * request,
	rockchip_backend_event_cb cb, void * user);

#ifdef __cplusplus
}
#endif

#endif /* __ROCKCHIP_BACKEND_H__ */
