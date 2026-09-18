#include <rockchip_backend.h>
#include <rkimage.h>
#include <rkgpt.h>
#include <rockchip_usb_error.h>
#include <stdarg.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define rk_fseek _fseeki64
#define rk_mkdir(path) _mkdir(path)
#else
#define rk_fseek fseeko
#define rk_mkdir(path) mkdir(path, 0755)
#endif

static void backend_emit(rockchip_backend_event_cb cb, void * user,
	enum rockchip_backend_event_type type, const char * text, int percent)
{
	struct rockchip_backend_event event;
	if(!cb)
		return;
	memset(&event, 0, sizeof(event));
	event.type = type;
	event.text = text;
	event.percent = percent;
	cb(&event, user);
}

static void backend_report(rockchip_backend_event_cb cb, void * user, const char * fmt, ...)
{
	char message[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(message, sizeof(message), fmt, ap);
	va_end(ap);
	if(cb)
		backend_emit(cb, user, ROCKCHIP_BACKEND_EVENT_LOG, message, -1);
	else
		printf("%s\r\n", message);
}

static void backend_sleep_ms(unsigned int milliseconds)
{
#ifdef _WIN32
	Sleep(milliseconds);
#else
	usleep(milliseconds * 1000U);
#endif
}

static void backend_close_device(struct openrockcli_ctx_t * ctx)
{
	if(!ctx)
		return;
	if(ctx->hdl)
	{
		libusb_release_interface(ctx->hdl, 0);
		libusb_close(ctx->hdl);
		ctx->hdl = NULL;
	}
}

static int backend_port_string(libusb_device * device, char * out, size_t out_size)
{
	uint8_t ports[8] = { 0 };
	int count = libusb_get_port_numbers(device, ports, sizeof(ports));
	int pos = 0;
	if(!out || !out_size)
		return 0;
	out[0] = 0;
	for(int i = 0; i < count && pos < (int)out_size - 1; i++)
		pos += snprintf(out + pos, out_size - pos, "%s%u", i ? "." : "", ports[i]);
	if(count <= 0)
		snprintf(out, out_size, "-");
	return count;
}

static int backend_is_adb(libusb_device * device)
{
	struct libusb_config_descriptor * config = NULL;
	int adb = 0;
	if(libusb_get_active_config_descriptor(device, &config) != 0)
		return 0;
	for(int i = 0; i < config->bNumInterfaces && !adb; i++)
	{
		const struct libusb_interface * iface = &config->interface[i];
		for(int j = 0; j < iface->num_altsetting; j++)
		{
			const struct libusb_interface_descriptor * desc = &iface->altsetting[j];
			if(desc->bInterfaceClass == 0xff && desc->bInterfaceSubClass == 0x42 &&
				desc->bInterfaceProtocol == 0x01)
			{
				adb = 1;
				break;
			}
		}
	}
	libusb_free_config_descriptor(config);
	return adb;
}

int rockchip_backend_scan_devices(struct rockchip_device_info * devices, int max_devices)
{
	libusb_context * context = NULL;
	libusb_device ** list = NULL;
	ssize_t count;
	int found = 0;
	if(!devices || max_devices <= 0 || libusb_init(&context) != 0)
		return 0;
	count = libusb_get_device_list(context, &list);
	if(count < 0)
	{
		libusb_exit(context);
		return 0;
	}
	for(ssize_t i = 0; i < count && found < max_devices; i++)
	{
		struct libusb_device_descriptor desc;
		struct rockchip_device_info * out;
		if(libusb_get_device_descriptor(list[i], &desc) != 0 || desc.idVendor != 0x2207 ||
			backend_is_adb(list[i]))
			continue;
		out = &devices[found++];
		memset(out, 0, sizeof(*out));
		out->bus = libusb_get_bus_number(list[i]);
		out->vid = desc.idVendor;
		out->pid = desc.idProduct;
		backend_port_string(list[i], out->port, sizeof(out->port));
		snprintf(out->mode, sizeof(out->mode), "%s",
			(desc.bcdUSB & 1) ? "loader" : "maskrom");
		if(!strcmp(out->mode, "maskrom") && desc.iProduct)
		{
			libusb_device_handle * handle = NULL;
			unsigned char product[128] = { 0 };
			if(libusb_open(list[i], &handle) == 0)
			{
				if(libusb_get_string_descriptor_ascii(handle, desc.iProduct, product,
					sizeof(product) - 1) > 0 &&
					(strstr((char *)product, "USB-MSC") || strstr((char *)product, "Loader") ||
					 strstr((char *)product, "download")))
					snprintf(out->mode, sizeof(out->mode), "loader");
				libusb_close(handle);
			}
		}
		snprintf(out->chip, sizeof(out->chip), "%s", openrockcli_chip_name(desc.idProduct));
	}
	libusb_free_device_list(list, 1);
	libusb_exit(context);
	return found;
}

int rockchip_backend_scan(int detailed, int verbose)
{
	struct rockchip_device_info devices[32];
	int count;
	(void)detailed;
	(void)verbose;
	printf("Scanning USB devices...\r\n\r\n");
	count = rockchip_backend_scan_devices(devices, ARRAY_SIZE(devices));
	if(!count)
	{
		printf("Error: Device not found\r\n");
		return 0;
	}
	printf("Bus  Port  VID:PID    Mode      Chip\r\n");
	printf("---  ----  ---------  --------  --------\r\n");
	for(int i = 0; i < count; i++)
		printf("%03u  %-4.4s  %04x:%04x  %-8s  %s\r\n", devices[i].bus,
			devices[i].port, devices[i].vid, devices[i].pid, devices[i].mode, devices[i].chip);
	return count;
}

int rockchip_backend_list_devices(void)
{
	return rockchip_backend_scan(1, 0);
}

static void backend_chip_tag(char * out, size_t out_size, uint32_t tag)
{
	char bytes[5];
	memcpy(bytes, &tag, 4);
	bytes[4] = 0;
	if(isprint((unsigned char)bytes[0]) && isprint((unsigned char)bytes[1]) &&
		isprint((unsigned char)bytes[2]) && isprint((unsigned char)bytes[3]))
		snprintf(out, out_size, "%c%c%c%c", bytes[3], bytes[2], bytes[1], bytes[0]);
	else
		snprintf(out, out_size, "%08x", tag);
}

int rockchip_backend_read_firmware_info(const char * firmware, struct rockchip_firmware_info * info)
{
	struct rkimage image;
	char error[192] = { 0 };
	if(!info)
		return 0;
	memset(info, 0, sizeof(*info));
	if(!rkimage_open(firmware, &image, error, sizeof(error)))
	{
		printf("Error: %s\r\n", error[0] ? error : "invalid firmware");
		return 0;
	}
	snprintf(info->path, sizeof(info->path), "%s", firmware);
	snprintf(info->image_type, sizeof(info->image_type), "Update Firmware");
	backend_chip_tag(info->chip_tag, sizeof(info->chip_tag), image.chip_tag);
	snprintf(info->version, sizeof(info->version), "%u.%u.%02u",
		(image.version >> 24) & 0xff, (image.version >> 16) & 0xff, image.version & 0xffff);
	snprintf(info->build_time, sizeof(info->build_time), "%04u-%02u-%02u %02u:%02u:%02u",
		image.year, image.month, image.day, image.hour, image.minute, image.second);
	info->file_size = image.file_size;
	info->entry_count = image.item_count;
	for(int i = 0; i < image.item_count; i++)
	{
		const struct rkimage_item * source = &image.items[i];
		struct rockchip_firmware_entry * entry = &info->entries[i];
		entry->entry_no = i;
		snprintf(entry->file, sizeof(entry->file), "%s", source->file);
		snprintf(entry->partition, sizeof(entry->partition), "%s", source->name);
		snprintf(entry->type, sizeof(entry->type), "%s", source->flash_offset == UINT32_MAX ? "boot" : "image");
		entry->offset = source->data_offset;
		entry->size = source->size;
		entry->flash_offset = source->flash_offset;
		entry->flash_sectors = source->flash_sectors;
	}
	return 1;
}

static void backend_size(char * out, size_t out_size, uint64_t bytes)
{
	const char * units[] = { "B", "KiB", "MiB", "GiB" };
	double value = (double)bytes;
	int unit = 0;
	while(value >= 1024.0 && unit < 3)
	{
		value /= 1024.0;
		unit++;
	}
	snprintf(out, out_size, unit ? "%.1f %s" : "%.0f %s", value, units[unit]);
}

int rockchip_backend_inspect_firmware(const char * firmware)
{
	struct rockchip_firmware_info info;
	char size[32];
	if(!rockchip_backend_read_firmware_info(firmware, &info))
		return 0;
	backend_size(size, sizeof(size), info.file_size);
	printf("Inspecting firmware...\r\nFirmware: %s\r\nSize: %s\r\nType: %s\r\n",
		info.path, size, info.image_type);
	printf("Chip Tag: %s\r\nVersion: %s\r\nBuild Time: %s\r\nEntry Count: %d\r\n",
		info.chip_tag, info.version, info.build_time, info.entry_count);
	for(int i = 0; i < info.entry_count; i++)
	{
		const struct rockchip_firmware_entry * e = &info.entries[i];
		printf("  [%02d] %-24s partition=%-12s type=%-6s offset=0x%llx size=0x%llx",
			e->entry_no, e->file, e->partition, e->type,
			(unsigned long long)e->offset, (unsigned long long)e->size);
		if(e->flash_offset != UINT32_MAX)
			printf(" lba=0x%llx sectors=0x%llx", (unsigned long long)e->flash_offset,
				(unsigned long long)e->flash_sectors);
		printf("\r\n");
	}
	return 1;
}

static int backend_safe_filename(const char * file)
{
	return file && file[0] && !strstr(file, "..") && !strchr(file, '/') && !strchr(file, '\\');
}

int rockchip_backend_unpack_firmware(const char * firmware, const char * outdir)
{
	struct rkimage image;
	char error[192] = { 0 };
	struct stat st;
	if(!outdir || !outdir[0] || !rkimage_open(firmware, &image, error, sizeof(error)))
	{
		printf("Error: %s\r\n", error[0] ? error : "output directory not specified");
		return 0;
	}
	if(stat(outdir, &st) != 0 && rk_mkdir(outdir) != 0)
	{
		printf("Error: cannot create output directory '%s': %s\r\n", outdir, strerror(errno));
		return 0;
	}
	for(int i = 0; i < image.item_count; i++)
	{
		char path[1024];
		if(!backend_safe_filename(image.items[i].file))
		{
			printf("Error: unsafe filename in firmware: %s\r\n", image.items[i].file);
			return 0;
		}
		snprintf(path, sizeof(path), "%s/%s", outdir, image.items[i].file);
		printf("Extracting %s\r\n", image.items[i].file);
		if(!rkimage_extract(&image, &image.items[i], path))
			return 0;
	}
	return 1;
}

static int backend_selected(const char * csv, const char * name)
{
	const char * p = csv;
	size_t length;
	if(!csv || !csv[0])
		return 1;
	length = strlen(name);
	while(*p)
	{
		while(*p == ',' || isspace((unsigned char)*p))
			p++;
		if(!strncmp(p, name, length) &&
			(p[length] == 0 || p[length] == ',' || isspace((unsigned char)p[length])))
			return 1;
		p = strchr(p, ',');
		if(!p)
			break;
	}
	return 0;
}

static int backend_open_device(struct openrockcli_ctx_t * ctx, uint8_t bus, const char * port)
{
	int rc;
	memset(ctx, 0, sizeof(*ctx));
	rc = libusb_init(&ctx->context);
	if(rc != 0)
	{
		ctx->last_usb_error = rc;
		return 0;
	}
	if(!openrockcli_init_selected(ctx, bus, port))
	{
		libusb_exit(ctx->context);
		ctx->context = NULL;
		return 0;
	}
	return 1;
}

static void backend_destroy_device(struct openrockcli_ctx_t * ctx)
{
	backend_close_device(ctx);
	if(ctx && ctx->context)
	{
		libusb_exit(ctx->context);
		ctx->context = NULL;
	}
}

static int backend_download_loader(struct openrockcli_ctx_t * ctx, const struct rkimage * image,
	const struct rkimage_item * loader, uint8_t bus, const char * port,
	rockchip_backend_event_cb cb, void * user)
{
	void * data;
	struct rkloader_ctx_t * lctx;
	int downloaded = 0;
	data = malloc((size_t)loader->size);
	if(!data || !rkimage_read(image, loader->data_offset, data, (size_t)loader->size))
	{
		free(data);
		backend_report(cb, user, "Error: cannot read embedded MiniLoader");
		return 0;
	}
	lctx = rkloader_ctx_alloc_data(data, loader->size);
	free(data);
	if(!lctx)
	{
		backend_report(cb, user, "Error: embedded MiniLoader is invalid");
		return 0;
	}
	backend_emit(cb, user, ROCKCHIP_BACKEND_EVENT_STAGE, "Downloading Loader", -1);
	for(int i = 0; i < lctx->nentry; i++)
	{
		struct rkloader_entry_t * entry = lctx->entry[i];
		if(entry->type == RKLOADER_ENTRY_471 || entry->type == RKLOADER_ENTRY_472)
		{
			char name[64];
			void * payload = (uint8_t *)lctx->buffer + get_unaligned_le32(&entry->data_offset);
			uint32_t size = get_unaligned_le32(&entry->data_size);
			uint32_t delay = get_unaligned_le32(&entry->data_delay);
			backend_report(cb, user, "Downloading %s", loader_wide2str(name, (uint8_t *)entry->name, sizeof(entry->name)));
			rock_maskrom_upload_memory(ctx, entry->type == RKLOADER_ENTRY_471 ? 0x471 : 0x472,
				payload, size, lctx->is_rc4on);
			backend_sleep_ms(delay ? delay : 10);
			downloaded++;
		}
	}
	rkloader_ctx_free(lctx);
	if(!downloaded)
		return 0;
	backend_destroy_device(ctx);
	backend_emit(cb, user, ROCKCHIP_BACKEND_EVENT_STAGE, "Reconnecting", -1);
	for(int attempt = 0; attempt < 60; attempt++)
	{
		backend_sleep_ms(250);
		if(backend_open_device(ctx, bus, port))
		{
			if(!ctx->maskrom || rock_ready(ctx))
			{
				ctx->maskrom = 0;
				return 1;
			}
		}
		backend_destroy_device(ctx);
	}
	backend_report(cb, user, "Error: Loader device did not reconnect");
	return 0;
}

struct backend_progress {
	uint64_t done;
	uint64_t total;
	int last_percent;
};

static void backend_progress_update(struct backend_progress * progress, uint64_t bytes,
	rockchip_backend_event_cb cb, void * user)
{
	int percent;
	progress->done += bytes;
	percent = progress->total ? (int)(progress->done * 100 / progress->total) : 100;
	if(percent != progress->last_percent)
	{
		progress->last_percent = percent;
		backend_emit(cb, user, ROCKCHIP_BACKEND_EVENT_PROGRESS, "Flashing Partitions", percent);
	}
}

static int backend_write_item(struct openrockcli_ctx_t * ctx, const struct rkimage * image,
	const struct rkimage_item * item, uint32_t lba, int * verify,
	struct backend_progress * progress, rockchip_backend_event_cb cb, void * user)
{
	FILE * fp = fopen(image->path, "rb");
	uint8_t * write_buffer = malloc(128 * 512);
	uint8_t * read_buffer = *verify ? malloc(128 * 512) : NULL;
	uint64_t remaining = item->size;
	int ok = 0;
	if(!fp || !write_buffer || (*verify && !read_buffer) ||
		rk_fseek(fp, (int64_t)item->data_offset, SEEK_SET) != 0)
		goto cleanup;
	while(remaining)
	{
		size_t bytes = remaining > 128 * 512 ? 128 * 512 : (size_t)remaining;
		uint32_t sectors = (uint32_t)((bytes + 511) / 512);
		memset(write_buffer, 0, sectors * 512);
		if(fread(write_buffer, 1, bytes, fp) != bytes ||
			!rock_flash_write_lba(ctx, lba, sectors, write_buffer))
			goto cleanup;
		if(*verify)
		{
			int unavailable = lba >= 0x10000;
			memset(read_buffer, 0, sectors * 512);
			if(!rock_flash_read_lba(ctx, lba, sectors, read_buffer))
				goto cleanup;
			if(memcmp(write_buffer, read_buffer, sectors * 512))
			{
				for(size_t index = 0; unavailable && index < sectors * 512; index++)
					if(read_buffer[index] != 0xcc)
						unavailable = 0;
				if(!unavailable)
					goto cleanup;
				backend_report(cb, user,
					"Warning: target Loader blocks reads above 32 MiB; high-address read-back verification is unavailable.");
				*verify = 0;
			}
		}
		lba += sectors;
		remaining -= bytes;
		backend_progress_update(progress, bytes, cb, user);
	}
	ok = 1;

cleanup:
	if(fp)
		fclose(fp);
	free(write_buffer);
	free(read_buffer);
	return ok;
}

static int backend_write_boot(struct openrockcli_ctx_t * ctx, const struct rkimage * image,
	const struct rkimage_item * loader, int verify, struct backend_progress * progress,
	rockchip_backend_event_cb cb, void * user)
{
	void * data = malloc((size_t)loader->size);
	struct rkloader_ctx_t * lctx;
	enum storage_type_t storage;
	uint32_t boot_lba = 64;
	uint32_t boot_stride = 0;
	uint32_t boot_sectors;
	int boot_copies = 1;
	void * boot_data = NULL;
	int ok = 0;
	if(!data || !rkimage_read(image, loader->data_offset, data, (size_t)loader->size))
	{
		free(data);
		return 0;
	}
	lctx = rkloader_ctx_alloc_data(data, loader->size);
	free(data);
	if(!lctx || !lctx->idbbuf || !lctx->idblen)
		goto cleanup;
	storage = rock_storage_read(ctx);
	if(storage == STORAGE_TYPE_SPINOR)
		boot_lba = 128;
	else if(storage == STORAGE_TYPE_SPINAND)
	{
		/* RK3506 SPI NAND uses two complete 256 KiB IDB slots at physical blocks 1 and 3. */
		boot_lba = 256;
		boot_stride = 512;
		boot_copies = 2;
		backend_report(cb, user, "Erasing SPI NAND GPT/IDB reserve (first 1 MiB)");
		if(!rock_flash_erase_lba(ctx, 0, 2048))
			goto cleanup;
	}
	boot_sectors = (uint32_t)((lctx->idblen + 511) / 512);
	boot_data = lctx->idbbuf;
	if(storage == STORAGE_TYPE_SPINAND)
	{
		boot_sectors = 512;
		boot_data = calloc(1, (size_t)boot_sectors * 512);
		if(!boot_data)
			goto cleanup;
		memcpy(boot_data, lctx->idbbuf, (size_t)lctx->idblen);
	}
	for(int copy = 0; copy < boot_copies; copy++)
	{
		uint32_t target = boot_lba + copy * boot_stride;
		backend_report(cb, user, "Writing bootloader IDB copy %d/%d at LBA 0x%x",
			copy + 1, boot_copies, target);
		if(!rock_flash_write_lba(ctx, target, boot_sectors, boot_data))
			goto cleanup;
		if(verify)
		{
			void * check = calloc(1, (size_t)boot_sectors * 512);
			if(!check || !rock_flash_read_lba(ctx, target,
				boot_sectors, check) ||
				memcmp(check, boot_data, (size_t)boot_sectors * 512))
			{
				free(check);
				goto cleanup;
			}
			free(check);
		}
	}
	if(boot_data != lctx->idbbuf)
	{
		free(boot_data);
		boot_data = NULL;
	}
	backend_progress_update(progress, loader->size, cb, user);
	ok = 1;

cleanup:
	if(boot_data && (!lctx || boot_data != lctx->idbbuf))
		free(boot_data);
	if(lctx)
		rkloader_ctx_free(lctx);
	return ok;
}

static int backend_write_gpt(struct openrockcli_ctx_t * ctx, const struct rkimage * image,
	const struct rkimage_item * parameter, uint32_t disk_sectors, int verify,
	struct backend_progress * progress, rockchip_backend_event_cb cb, void * user)
{
	uint8_t * parameter_data = malloc((size_t)parameter->size);
	uint8_t * master = malloc(RKGPT_MASTER_SECTORS * RKGPT_SECTOR_SIZE);
	uint8_t * backup = malloc(RKGPT_BACKUP_SECTORS * RKGPT_SECTOR_SIZE);
	uint8_t * check = verify ? malloc(RKGPT_MASTER_SECTORS * RKGPT_SECTOR_SIZE) : NULL;
	char error[192] = { 0 };
	int ok = 0;
	if(!parameter_data || !master || !backup || (verify && !check) ||
		!rkimage_read(image, parameter->data_offset, parameter_data, (size_t)parameter->size))
		goto cleanup;
	if(!rkgpt_build(parameter_data, (size_t)parameter->size, disk_sectors,
		master, backup, error, sizeof(error)))
	{
		backend_report(cb, user, "Error: cannot generate GPT: %s", error[0] ? error : "invalid parameter");
		goto cleanup;
	}
	backend_report(cb, user, "Writing primary and backup GPT");
	if(!rock_flash_write_lba(ctx, 0, RKGPT_MASTER_SECTORS, master) ||
		!rock_flash_write_lba(ctx, disk_sectors - RKGPT_BACKUP_SECTORS,
			RKGPT_BACKUP_SECTORS, backup))
		goto cleanup;
	if(verify)
	{
		int unavailable = 1;
		if(!rock_flash_read_lba(ctx, 0, RKGPT_MASTER_SECTORS, check) ||
			memcmp(check, master, RKGPT_MASTER_SECTORS * RKGPT_SECTOR_SIZE))
			goto cleanup;
		if(!rock_flash_read_lba(ctx, disk_sectors - RKGPT_BACKUP_SECTORS,
			RKGPT_BACKUP_SECTORS, check))
			goto cleanup;
		if(memcmp(check, backup, RKGPT_BACKUP_SECTORS * RKGPT_SECTOR_SIZE))
		{
			for(size_t i = 0; i < RKGPT_BACKUP_SECTORS * RKGPT_SECTOR_SIZE; i++)
				if(check[i] != 0xcc)
					unavailable = 0;
			if(!unavailable)
				goto cleanup;
			backend_report(cb, user,
				"Warning: target Loader cannot read the backup GPT address; primary GPT verification passed.");
		}
	}
	backend_progress_update(progress, parameter->size, cb, user);
	ok = 1;

cleanup:
	free(parameter_data);
	free(master);
	free(backup);
	free(check);
	return ok;
}

static int backend_mode_valid(const char * mode)
{
	return !mode || !strcmp(mode, "partition") || !strcmp(mode, "keep_data") ||
		!strcmp(mode, "partition_erase") || !strcmp(mode, "full_erase");
}

struct rockchip_backend_cli_state { char stage[96]; };

static void backend_cli_event(const struct rockchip_backend_event * event, void * user)
{
	struct rockchip_backend_cli_state * state = user;
	if(!event)
		return;
	if(event->type == ROCKCHIP_BACKEND_EVENT_PROGRESS)
	{
		int percent = event->percent < 0 ? 0 : (event->percent > 100 ? 100 : event->percent);
		int filled = percent * 40 / 100;
		printf("\rFlashing: [");
		for(int i = 0; i < 40; i++)
			putchar(i < filled ? '#' : '-');
		printf("] %3d%%", percent);
		fflush(stdout);
		if(percent == 100)
			printf("\r\n");
	}
	else if(event->type == ROCKCHIP_BACKEND_EVENT_STAGE)
	{
		if(!event->text || (state && !strcmp(state->stage, event->text)))
			return;
		if(state)
			snprintf(state->stage, sizeof(state->stage), "%s", event->text);
		printf("Stage: %s\r\n", event->text);
	}
	else if(event->text)
		printf("%s\r\n", event->text);
}

int rockchip_backend_flash_with_cb(const struct rockchip_flash_request * request,
	rockchip_backend_event_cb cb, void * user)
{
	struct rkimage image;
	struct openrockcli_ctx_t ctx;
	struct rockchip_device_info devices[32];
	struct flash_info_t flash_info;
	struct backend_progress progress = { 0, 0, -1 };
	const struct rkimage_item * loader;
	const struct rkimage_item * parameter;
	const char * mode;
	const char * post_action;
	char error[192] = { 0 };
	uint8_t bus = 0;
	const char * port = NULL;
	int device_count;
	int verify_active;
	int ok = 0;
	if(!request || !rkimage_open(request->firmware, &image, error, sizeof(error)))
	{
		backend_report(cb, user, "Error: %s", error[0] ? error : "firmware file not specified");
		return 0;
	}
	mode = request->mode ? request->mode : "full_erase";
	post_action = request->post_action ? request->post_action : "reboot";
	if(!backend_mode_valid(mode))
	{
		backend_report(cb, user, "Error: unsupported flash mode '%s'", mode);
		return 0;
	}
	if(strcmp(post_action, "reboot") && strcmp(post_action, "poweroff") && strcmp(post_action, "shutdown"))
	{
		backend_report(cb, user, "Error: unsupported post action '%s'", post_action);
		return 0;
	}
	device_count = rockchip_backend_scan_devices(devices, ARRAY_SIZE(devices));
	if(!device_count)
	{
		backend_report(cb, user, "Error: Device not found");
		return 0;
	}
	if(device_count > 1 && !request->bus && !request->port)
	{
		backend_report(cb, user, "Error: multiple Rockchip devices detected; specify --bus and --port");
		return 0;
	}
	if(request->bus)
		bus = (uint8_t)strtoul(request->bus, NULL, 0);
	if(request->port)
		port = request->port;
	else if(device_count == 1)
	{
		bus = devices[0].bus;
		port = devices[0].port;
	}
	loader = rkimage_find_item(&image, "bootloader");
	parameter = rkimage_find_item(&image, "parameter");
	if(!loader)
	{
		backend_report(cb, user, "Error: update.img has no bootloader entry");
		return 0;
	}
	for(int i = 0; i < image.item_count; i++)
	{
		const struct rkimage_item * item = &image.items[i];
		if(!strcmp(item->name, "package-file"))
			continue;
		if(backend_selected(request->partitions, item->name))
			progress.total += item->size;
	}
	if(request->verbose)
		backend_report(cb, user, "Native OpenRockCLI flash: model=%s mode=%s verify=%s bus=%u port=%s",
			image.model, mode, request->verify ? "true" : "false", bus, port ? port : "-");
	backend_emit(cb, user, ROCKCHIP_BACKEND_EVENT_STAGE, "Query Device", -1);
	if(!backend_open_device(&ctx, bus, port))
	{
		backend_report(cb, user, "Error: %s (%s): %s",
			rockchip_usb_open_error_code(ctx.last_usb_error),
			libusb_error_name(ctx.last_usb_error),
			rockchip_usb_open_recovery(ctx.last_usb_error));
		return 0;
	}
	if(ctx.maskrom && !backend_download_loader(&ctx, &image, loader, bus, port, cb, user))
		goto cleanup;
	/* RK3506 loaders may report SPI NAND as the current medium before its
	 * translation layer is selected.  Explicitly switch it, matching the
	 * vendor Rockusb sequence, before detecting capacity or touching LBAs. */
	if(rock_storage_read(&ctx) == STORAGE_TYPE_SPINAND)
	{
		backend_emit(cb, user, ROCKCHIP_BACKEND_EVENT_STAGE, "Selecting SPI NAND", -1);
		if(!rock_storage_switch(&ctx, STORAGE_TYPE_SPINAND) || !rock_ready(&ctx))
		{
			backend_report(cb, user, "Error: cannot select target SPI NAND");
			goto cleanup;
		}
	}
	if(!rock_flash_detect(&ctx, &flash_info))
	{
		backend_report(cb, user, "Error: cannot detect target flash");
		goto cleanup;
	}
	backend_report(cb, user, "Target flash: %u sectors (%.1f GiB)", flash_info.sector_total,
		(double)flash_info.sector_total / 2097152.0);
	if(!strcmp(mode, "full_erase"))
	{
		backend_emit(cb, user, ROCKCHIP_BACKEND_EVENT_STAGE, "Erasing", -1);
		if(!rock_flash_erase_lba(&ctx, 0, flash_info.sector_total))
		{
			backend_report(cb, user, "Error: full flash erase failed");
			goto cleanup;
		}
	}
	backend_emit(cb, user, ROCKCHIP_BACKEND_EVENT_STAGE, "Flashing Partitions", -1);
	verify_active = request->verify;
	/* IDB must precede GPT on SPI NAND: preparing the IDB reserve erases the
	 * first MiB, which contains the primary GPT. */
	if(backend_selected(request->partitions, loader->name) &&
		!backend_write_boot(&ctx, &image, loader, request->verify, &progress, cb, user))
	{
		backend_report(cb, user, "Error: bootloader write or verification failed");
		goto cleanup;
	}
	if(parameter && backend_selected(request->partitions, parameter->name) &&
		!backend_write_gpt(&ctx, &image, parameter, flash_info.sector_total,
			request->verify, &progress, cb, user))
	{
		backend_report(cb, user, "Error: GPT write or verification failed");
		goto cleanup;
	}
	for(int i = 0; i < image.item_count; i++)
	{
		const struct rkimage_item * item = &image.items[i];
		uint32_t lba = item->flash_offset;
		uint64_t needed = (item->size + 511) / 512;
		if(!backend_selected(request->partitions, item->name) || !strcmp(item->name, "package-file") ||
			!strcmp(item->name, "bootloader") || !strcmp(item->name, "parameter"))
			continue;
		if(lba == UINT32_MAX || needed > item->flash_sectors ||
			(uint64_t)lba + needed > flash_info.sector_total)
		{
			backend_report(cb, user, "Error: invalid target range for partition %s", item->name);
			goto cleanup;
		}
		if(!strcmp(mode, "partition_erase") && !rock_flash_erase_lba(&ctx, lba, item->flash_sectors))
		{
			backend_report(cb, user, "Error: erase failed for partition %s", item->name);
			goto cleanup;
		}
		backend_report(cb, user, "Writing %-12s LBA 0x%x, %llu bytes", item->name, lba,
			(unsigned long long)item->size);
		if(!backend_write_item(&ctx, &image, item, lba, &verify_active, &progress, cb, user))
		{
			backend_report(cb, user, "Error: write or verification failed for partition %s", item->name);
			goto cleanup;
		}
	}
	if(!strcmp(post_action, "reboot"))
	{
		backend_emit(cb, user, ROCKCHIP_BACKEND_EVENT_STAGE, "Setting Mode", 100);
		if(!rock_reset(&ctx, 0))
			backend_report(cb, user, "Warning: reset command was not acknowledged");
	}
	else
		backend_report(cb, user, "Poweroff is not a Rockusb command; leaving the device in Loader mode.");
	backend_report(cb, user, "Flash complete! Device will %s.", post_action);
	ok = 1;

cleanup:
	backend_destroy_device(&ctx);
	if(!ok)
		backend_report(cb, user, "Flash failed.");
	return ok;
}

int rockchip_backend_flash(const struct rockchip_flash_request * request)
{
	struct rockchip_backend_cli_state state;
	memset(&state, 0, sizeof(state));
	return rockchip_backend_flash_with_cb(request, backend_cli_event, &state);
}
