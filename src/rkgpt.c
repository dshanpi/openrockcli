#include <rkgpt.h>
#include <stdarg.h>

#define RKGPT_ENTRY_COUNT 128U
#define RKGPT_ENTRY_SIZE 128U
#define RKGPT_HEADER_SIZE 92U
#define RKGPT_MAX_PARTITIONS 64U

struct rkgpt_partition {
	char name[64];
	uint64_t start;
	uint64_t size;
	int grow;
};

static void rkgpt_error(char * error, size_t size, const char * format, ...)
{
	va_list ap;
	if(!error || !size)
		return;
	va_start(ap, format);
	vsnprintf(error, size, format, ap);
	va_end(ap);
}

uint32_t rkgpt_crc32(const uint8_t * data, size_t size)
{
	uint32_t crc = 0xffffffffU;
	while(size--)
	{
		crc ^= *data++;
		for(int bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (0xedb88320U & (uint32_t)-(int32_t)(crc & 1));
	}
	return crc ^ 0xffffffffU;
}

static int rkgpt_hex(char c)
{
	if(c >= '0' && c <= '9')
		return c - '0';
	if(c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if(c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static int rkgpt_uuid(const char * text, uint8_t uuid[16])
{
	uint8_t raw[16];
	int nibble = 0;
	memset(raw, 0, sizeof(raw));
	for(; *text && *text != '\r' && *text != '\n'; text++)
	{
		int value;
		if(*text == '-')
			continue;
		value = rkgpt_hex(*text);
		if(value < 0 || nibble >= 32)
			return 0;
		if(!(nibble & 1))
			raw[nibble / 2] = (uint8_t)(value << 4);
		else
			raw[nibble / 2] |= (uint8_t)value;
		nibble++;
	}
	if(nibble != 32)
		return 0;
	uuid[0] = raw[3]; uuid[1] = raw[2]; uuid[2] = raw[1]; uuid[3] = raw[0];
	uuid[4] = raw[5]; uuid[5] = raw[4];
	uuid[6] = raw[7]; uuid[7] = raw[6];
	memcpy(uuid + 8, raw + 8, 8);
	return 1;
}

static void rkgpt_deterministic_uuid(const char * name, uint64_t start, uint8_t uuid[16])
{
	uint64_t hash = 1469598103934665603ULL ^ start;
	static const uint8_t base[16] = {
		0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
		0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4
	};
	for(const unsigned char * p = (const unsigned char *)name; *p; p++)
		hash = (hash ^ *p) * 1099511628211ULL;
	memcpy(uuid, base, 16);
	for(int i = 0; i < 8; i++)
		uuid[8 + i] ^= (uint8_t)(hash >> (i * 8));
	uuid[7] = (uuid[7] & 0x0f) | 0x40;
	uuid[8] = (uuid[8] & 0x3f) | 0x80;
}

static int rkgpt_parameter_uuid(const char * parameter, const char * name, uint8_t uuid[16])
{
	char key[96];
	const char * found;
	snprintf(key, sizeof(key), "uuid:%.63s=", name);
	found = strstr(parameter, key);
	return found ? rkgpt_uuid(found + strlen(key), uuid) : 0;
}

static int rkgpt_parse(char * parameter, struct rkgpt_partition * parts, size_t * count,
	uint32_t disk_sectors, char * error, size_t error_size)
{
	char * cursor = strstr(parameter, "mtdparts=");
	char * end;
	size_t used = 0;
	if(!cursor || !(cursor = strchr(cursor, ':')))
	{
		rkgpt_error(error, error_size, "parameter has no mtdparts table");
		return 0;
	}
	cursor++;
	end = strpbrk(cursor, "\r\n");
	if(end)
		*end = 0;
	while(*cursor)
	{
		char * comma = strchr(cursor, ',');
		char * at;
		char * open;
		char * close;
		char * name_option;
		char * number_end;
		struct rkgpt_partition * part;
		if(comma)
			*comma = 0;
		if(used >= RKGPT_MAX_PARTITIONS || !(at = strchr(cursor, '@')) ||
			!(open = strchr(at + 1, '(')) || !(close = strchr(open + 1, ')')))
		{
			rkgpt_error(error, error_size, "invalid mtdparts entry: %s", cursor);
			return 0;
		}
		*at = 0;
		*open = 0;
		*close = 0;
		part = &parts[used];
		memset(part, 0, sizeof(*part));
		part->grow = cursor[0] == '-';
		if(!part->grow)
		{
			part->size = strtoull(cursor, &number_end, 0);
			if(!part->size || *number_end)
			{
				rkgpt_error(error, error_size, "invalid partition size: %s", cursor);
				return 0;
			}
		}
		part->start = strtoull(at + 1, &number_end, 0);
		if(*number_end || part->start < RKGPT_MASTER_SECTORS ||
			part->start > (uint64_t)disk_sectors - RKGPT_BACKUP_SECTORS)
		{
			rkgpt_error(error, error_size, "invalid partition offset: %s", at + 1);
			return 0;
		}
		name_option = strchr(open + 1, ':');
		if(name_option)
			*name_option = 0;
		if(!open[1] || strlen(open + 1) >= sizeof(part->name))
		{
			rkgpt_error(error, error_size, "invalid partition name");
			return 0;
		}
		snprintf(part->name, sizeof(part->name), "%s", open + 1);
		if(part->grow)
			part->size = (uint64_t)disk_sectors - RKGPT_BACKUP_SECTORS - part->start;
		if(!part->size || part->size > (uint64_t)disk_sectors - RKGPT_BACKUP_SECTORS - part->start ||
			(used && part->start < parts[used - 1].start + parts[used - 1].size))
		{
			rkgpt_error(error, error_size, "partition %s exceeds or overlaps the disk", part->name);
			return 0;
		}
		used++;
		if(!comma)
			break;
		cursor = comma + 1;
	}
	*count = used;
	return used > 0;
}

static void rkgpt_header(uint8_t * header, uint64_t self, uint64_t alternate,
	uint64_t entries_lba, uint32_t disk_sectors, const uint8_t disk_guid[16], uint32_t entries_crc)
{
	memset(header, 0, RKGPT_SECTOR_SIZE);
	put_unaligned_le64(header + 0, 0x5452415020494645ULL);
	put_unaligned_le32(header + 8, 0x00010000U);
	put_unaligned_le32(header + 12, RKGPT_HEADER_SIZE);
	put_unaligned_le64(header + 24, self);
	put_unaligned_le64(header + 32, alternate);
	put_unaligned_le64(header + 40, RKGPT_MASTER_SECTORS);
	put_unaligned_le64(header + 48, (uint64_t)disk_sectors - RKGPT_MASTER_SECTORS);
	memcpy(header + 56, disk_guid, 16);
	put_unaligned_le64(header + 72, entries_lba);
	put_unaligned_le32(header + 80, RKGPT_ENTRY_COUNT);
	put_unaligned_le32(header + 84, RKGPT_ENTRY_SIZE);
	put_unaligned_le32(header + 88, entries_crc);
	put_unaligned_le32(header + 16, rkgpt_crc32(header, RKGPT_HEADER_SIZE));
}

int rkgpt_build(const void * data, size_t data_size, uint32_t disk_sectors,
	uint8_t master[RKGPT_MASTER_SECTORS * RKGPT_SECTOR_SIZE],
	uint8_t backup[RKGPT_BACKUP_SECTORS * RKGPT_SECTOR_SIZE],
	char * error, size_t error_size)
{
	static const uint8_t type_guid[16] = {
		0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
		0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4
	};
	struct rkgpt_partition parts[RKGPT_MAX_PARTITIONS];
	uint8_t disk_guid[16];
	const uint8_t * bytes = data;
	char * parameter;
	char * parse_copy;
	size_t text_offset = 0;
	size_t text_size;
	size_t part_count = 0;
	uint32_t entries_crc;
	if(!data || !data_size || disk_sectors <= RKGPT_MASTER_SECTORS + RKGPT_BACKUP_SECTORS)
		return 0;
	while(text_offset + 8 <= data_size && memcmp(bytes + text_offset, "CMDLINE:", 8))
		text_offset++;
	if(text_offset + 8 > data_size)
	{
		rkgpt_error(error, error_size, "parameter has no CMDLINE table");
		return 0;
	}
	text_size = data_size - text_offset;
	parameter = malloc(text_size + 1);
	parse_copy = malloc(text_size + 1);
	if(!parameter || !parse_copy)
	{
		free(parameter);
		free(parse_copy);
		return 0;
	}
	memcpy(parameter, bytes + text_offset, text_size);
	parameter[text_size] = 0;
	memcpy(parse_copy, parameter, text_size + 1);
	if(!rkgpt_parse(parse_copy, parts, &part_count, disk_sectors, error, error_size))
	{
		free(parameter);
		free(parse_copy);
		return 0;
	}
	free(parse_copy);
	memset(master, 0, RKGPT_MASTER_SECTORS * RKGPT_SECTOR_SIZE);
	memset(backup, 0, RKGPT_BACKUP_SECTORS * RKGPT_SECTOR_SIZE);
	master[450] = 0xee;
	put_unaligned_le32(master + 454, 1);
	put_unaligned_le32(master + 458, disk_sectors - 1);
	put_unaligned_le16(master + 510, 0xaa55);
	rkgpt_deterministic_uuid("openrockcli", disk_sectors, disk_guid);
	for(size_t i = 0; i < part_count; i++)
	{
		uint8_t * entry = master + 2 * RKGPT_SECTOR_SIZE + i * RKGPT_ENTRY_SIZE;
		uint8_t unique[16];
		memcpy(entry, type_guid, sizeof(type_guid));
		if(!rkgpt_parameter_uuid(parameter, parts[i].name, unique))
			rkgpt_deterministic_uuid(parts[i].name, parts[i].start, unique);
		memcpy(entry + 16, unique, sizeof(unique));
		put_unaligned_le64(entry + 32, parts[i].start);
		put_unaligned_le64(entry + 40, parts[i].start + parts[i].size - 1);
		for(size_t c = 0; c < strlen(parts[i].name) && c < 36; c++)
			put_unaligned_le16(entry + 56 + c * 2, (uint8_t)parts[i].name[c]);
	}
	entries_crc = rkgpt_crc32(master + 2 * RKGPT_SECTOR_SIZE,
		RKGPT_ENTRY_COUNT * RKGPT_ENTRY_SIZE);
	rkgpt_header(master + RKGPT_SECTOR_SIZE, 1, disk_sectors - 1, 2,
		disk_sectors, disk_guid, entries_crc);
	memcpy(backup, master + 2 * RKGPT_SECTOR_SIZE, RKGPT_ENTRY_COUNT * RKGPT_ENTRY_SIZE);
	rkgpt_header(backup + RKGPT_ENTRY_COUNT * RKGPT_ENTRY_SIZE,
		disk_sectors - 1, 1, disk_sectors - RKGPT_BACKUP_SECTORS,
		disk_sectors, disk_guid, entries_crc);
	free(parameter);
	return 1;
}
