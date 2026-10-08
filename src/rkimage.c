#include <rkimage.h>
#include <stdarg.h>
#include <sys/stat.h>

#ifdef _WIN32
#define rk_fseek _fseeki64
#else
#define rk_fseek fseeko
#endif

#define RKFW_TAG 0x57464b52U
#define RKAF_TAG 0x46414b52U
#define RKFW_HEADER_SIZE 102U
#define RKAF_HEADER_SIZE 140U
#define RKAF_ITEM_SIZE 112U

static void rkimage_error(char * error, size_t error_size, const char * fmt, ...)
{
	va_list ap;
	if(!error || !error_size)
		return;
	va_start(ap, fmt);
	vsnprintf(error, error_size, fmt, ap);
	va_end(ap);
}

static void rkimage_string(char * out, size_t out_size, const uint8_t * in, size_t in_size)
{
	size_t n = 0;
	if(!out || !out_size)
		return;
	while(n < in_size && n + 1 < out_size && in[n])
	{
		unsigned char c = in[n];
		out[n] = isprint(c) ? (char)c : '_';
		n++;
	}
	out[n] = 0;
}

static int rkimage_range_valid(uint64_t offset, uint64_t size, uint64_t file_size)
{
	return offset <= file_size && size <= file_size - offset;
}

int rkimage_read(const struct rkimage * image, uint64_t offset, void * data, size_t size)
{
	FILE * fp;
	int ok;
	if(!image || !data || !rkimage_range_valid(offset, size, image->file_size))
		return 0;
	fp = rk_fopen(image->path, "rb");
	if(!fp)
		return 0;
	ok = rk_fseek(fp, (int64_t)offset, SEEK_SET) == 0 && fread(data, 1, size, fp) == size;
	fclose(fp);
	return ok;
}

int rkimage_open(const char * path, struct rkimage * image, char * error, size_t error_size)
{
	uint8_t outer[RKFW_HEADER_SIZE];
	uint8_t inner[RKAF_HEADER_SIZE];
	FILE * fp = NULL;
	uint64_t package_end;
	uint32_t item_count;

	if(!path || !image)
		return 0;
	memset(image, 0, sizeof(*image));
	if(rk_file_size(path, &image->file_size) != 0)
	{
		rkimage_error(error, error_size, "firmware file not found: %s", path);
		return 0;
	}
	if(strlen(path) >= sizeof(image->path)) { rkimage_error(error, error_size, "firmware path too long"); return 0; }
	snprintf(image->path, sizeof(image->path), "%s", path);
	if(image->file_size < sizeof(outer) + sizeof(inner))
	{
		rkimage_error(error, error_size, "firmware is too small");
		return 0;
	}
	fp = rk_fopen(path, "rb");
	if(!fp || fread(outer, 1, sizeof(outer), fp) != sizeof(outer))
	{
		rkimage_error(error, error_size, "cannot read RKFW header");
		goto fail;
	}
	if(get_unaligned_le32(outer) != RKFW_TAG || get_unaligned_le16(outer + 4) != RKFW_HEADER_SIZE)
	{
		rkimage_error(error, error_size, "not a supported Rockchip RKFW image");
		goto fail;
	}
	image->version = get_unaligned_le32(outer + 6);
	image->year = get_unaligned_le16(outer + 14);
	image->month = outer[16];
	image->day = outer[17];
	image->hour = outer[18];
	image->minute = outer[19];
	image->second = outer[20];
	image->chip_tag = get_unaligned_le32(outer + 21);
	image->boot_offset = get_unaligned_le32(outer + 25);
	image->boot_size = get_unaligned_le32(outer + 29);
	image->firmware_offset = get_unaligned_le32(outer + 33);
	image->firmware_size = get_unaligned_le32(outer + 37);
	if(outer[55] == 'H' && outer[56] == 'I')
		image->firmware_size |= (uint64_t)get_unaligned_le32(outer + 57) << 32;
	if(!rkimage_range_valid(image->boot_offset, image->boot_size, image->file_size) ||
		!rkimage_range_valid(image->firmware_offset, image->firmware_size, image->file_size))
	{
		rkimage_error(error, error_size, "RKFW component range exceeds file size");
		goto fail;
	}
	if(rk_fseek(fp, (int64_t)image->firmware_offset, SEEK_SET) != 0 ||
		fread(inner, 1, sizeof(inner), fp) != sizeof(inner))
	{
		rkimage_error(error, error_size, "cannot read RKAF header");
		goto fail;
	}
	if(get_unaligned_le32(inner) != RKAF_TAG)
	{
		rkimage_error(error, error_size, "RKFW does not contain an RKAF package");
		goto fail;
	}
	rkimage_string(image->model, sizeof(image->model), inner + 8, 64);
	item_count = get_unaligned_le32(inner + 136);
	if(item_count == 0 || item_count > RKIMAGE_MAX_ITEMS ||
		(uint64_t)RKAF_HEADER_SIZE + (uint64_t)item_count * RKAF_ITEM_SIZE > image->firmware_size)
	{
		rkimage_error(error, error_size, "invalid RKAF item count: %u", item_count);
		goto fail;
	}
	package_end = image->firmware_offset + image->firmware_size;
	for(uint32_t i = 0; i < item_count; i++)
	{
		uint8_t raw[RKAF_ITEM_SIZE];
		struct rkimage_item * item = &image->items[i];
		uint64_t relative;
		if(fread(raw, 1, sizeof(raw), fp) != sizeof(raw))
		{
			rkimage_error(error, error_size, "cannot read RKAF item %u", i);
			goto fail;
		}
		rkimage_string(item->name, sizeof(item->name), raw, 32);
		rkimage_string(item->file, sizeof(item->file), raw + 32, 64);
		relative = get_unaligned_le32(raw + 96);
		item->data_offset = image->firmware_offset + relative;
		item->flash_offset = get_unaligned_le32(raw + 100);
		/* RKAF stores the payload allocation in 2 KiB blocks; Rockusb uses 512-byte LBAs. */
		uint32_t blocks = get_unaligned_le32(raw + 104);
		if(blocks > UINT32_MAX / 4U) { rkimage_error(error, error_size, "RKAF allocation overflows LBA count"); goto fail; }
		item->flash_sectors = blocks * 4U;
		item->size = get_unaligned_le32(raw + 108);
		if(!item->name[0] || !item->file[0] || relative < RKAF_HEADER_SIZE + (uint64_t)item_count * RKAF_ITEM_SIZE || item->data_offset > package_end ||
			item->size > package_end - item->data_offset)
		{
			rkimage_error(error, error_size, "invalid RKAF item %u", i);
			goto fail;
		}
		for(uint32_t j = 0; j < i; j++)
			if(!strcmp(item->name, image->items[j].name)) { rkimage_error(error, error_size, "duplicate RKAF partition name"); goto fail; }
	}
	image->item_count = (int)item_count;
	fclose(fp);
	return 1;

fail:
	if(fp)
		fclose(fp);
	memset(image, 0, sizeof(*image));
	return 0;
}

const struct rkimage_item * rkimage_find_item(const struct rkimage * image, const char * name)
{
	if(!image || !name)
		return NULL;
	for(int i = 0; i < image->item_count; i++)
		if(!strcmp(image->items[i].name, name) || !strcmp(image->items[i].file, name))
			return &image->items[i];
	return NULL;
}

int rkimage_extract(const struct rkimage * image, const struct rkimage_item * item,
	const char * output)
{
	FILE * in = NULL;
	FILE * out = NULL;
	uint8_t * buffer = NULL;
	uint64_t remaining;
	int ok = 0;
	if(!image || !item || !output)
		return 0;
	in = rk_fopen(image->path, "rb");
	out = rk_create_file(output);
	buffer = malloc(1024 * 1024);
	if(!in || !out || !buffer || rk_fseek(in, (int64_t)item->data_offset, SEEK_SET) != 0)
		goto cleanup;
	remaining = item->size;
	while(remaining)
	{
		size_t chunk = remaining > 1024 * 1024 ? 1024 * 1024 : (size_t)remaining;
		if(fread(buffer, 1, chunk, in) != chunk || fwrite(buffer, 1, chunk, out) != chunk)
			goto cleanup;
		remaining -= chunk;
	}
	ok = 1;

cleanup:
	if(buffer)
		free(buffer);
	if(in)
		fclose(in);
	if(out && fclose(out) != 0)
		ok = 0;
	return ok;
}
