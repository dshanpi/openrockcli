#include <rockchip_backend.h>
#ifndef _WIN32
#include <dirent.h>
#endif
#include <stdarg.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <sys/wait.h>
#endif

const char * rockchip_backend_upgrade_tool(void)
{
	const char * env = getenv("OPENROCKCLI_UPGRADE_TOOL");
	const char * candidates[] = {
		"tools/linux/Linux_Upgrade_Tool/Linux_Upgrade_Tool/upgrade_tool",
		"../linux/Linux_Upgrade_Tool/Linux_Upgrade_Tool/upgrade_tool",
		"../../tools/linux/Linux_Upgrade_Tool/Linux_Upgrade_Tool/upgrade_tool",
		"upgrade_tool",
	};

	if(env && access(env, X_OK) == 0)
		return env;
	for(int i = 0; i < ARRAY_SIZE(candidates); i++)
	{
		if(access(candidates[i], X_OK) == 0)
			return candidates[i];
	}
	return candidates[ARRAY_SIZE(candidates) - 1];
}

#ifndef _WIN32
static int rockchip_backend_run_tool(char * const args[])
{
	pid_t pid;
	int status;

	fflush(stdout);
	pid = fork();
	if(pid < 0)
	{
		printf("Error: failed to start %s: %s\r\n", args[0], strerror(errno));
		return 0;
	}
	if(pid == 0)
	{
		execv(args[0], args);
		execvp(args[0], args);
		fprintf(stderr, "Error: failed to exec %s: %s\r\n", args[0], strerror(errno));
		_exit(127);
	}
	if(waitpid(pid, &status, 0) < 0)
	{
		printf("Error: failed to wait for %s: %s\r\n", args[0], strerror(errno));
		return 0;
	}
	if(WIFEXITED(status) && WEXITSTATUS(status) == 0)
		return 1;
	if(WIFEXITED(status))
		printf("Error: %s exited with %d\r\n", args[0], WEXITSTATUS(status));
	else
		printf("Error: %s terminated unexpectedly\r\n", args[0]);
	return 0;
}

static void rockchip_backend_emit(rockchip_backend_event_cb cb, void * user,
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

static const char * rockchip_backend_stage_name(const char * line)
{
	if(!line)
		return NULL;
	if(strstr(line, "Download Boot") || strstr(line, "Download IDB"))
		return "Writing Boot";
	if(strstr(line, "Download Firmware") || strstr(line, "Download Image"))
		return "Flashing Partitions";
	if(strstr(line, "Erase Flash") || strstr(line, "Erase System") ||
		strstr(line, "Erase Userdata") || strstr(line, "Erase IDB"))
		return "Erasing";
	if(strstr(line, "Check Image") || strstr(line, "Check Chip") ||
		strstr(line, "Test Device") || strstr(line, "Get FlashInfo") ||
		strstr(line, "Get Block State"))
		return "Query Device";
	if(strstr(line, "Wait For Loader") || strstr(line, "Wait For Maskrom") ||
		strstr(line, "Wait For MSC"))
		return "Reconnecting";
	if(strstr(line, "Reset Device") || strstr(line, "Reset Pipe"))
		return "Setting Mode";
	if(strstr(line, "Lowerformat") || strstr(line, "Tag Bad Block") ||
		strstr(line, "Test Block"))
		return "Erasing";
	if(strstr(line, "Prepare IDB"))
		return "Writing Boot";
	return NULL;
}

static int rockchip_backend_line_percent(const char * line, int * percent)
{
	const char * pct;

	if(!line || !percent)
		return 0;
	pct = strchr(line, '(');
	if(pct && sscanf(pct, "(%d%%)", percent) == 1)
		return 1;
	if(sscanf(line, "%*[^0123456789]%d%%", percent) == 1 && strchr(line, '%'))
		return 1;
	return 0;
}

static void rockchip_backend_parse_tool_line(const char * line,
	rockchip_backend_event_cb cb, void * user)
{
	const char * stage;
	int percent;

	if(!line || !line[0])
		return;
	stage = rockchip_backend_stage_name(line);
	if(strstr(line, " Start"))
	{
		rockchip_backend_emit(cb, user, ROCKCHIP_BACKEND_EVENT_STAGE, stage ? stage : line, -1);
		return;
	}
	if(strstr(line, " Success"))
	{
		rockchip_backend_emit(cb, user, ROCKCHIP_BACKEND_EVENT_STAGE, stage ? stage : line, 100);
		return;
	}
	if(strstr(line, " Fail") || strstr(line, "Error:") || strstr(line, "No found"))
	{
		rockchip_backend_emit(cb, user, ROCKCHIP_BACKEND_EVENT_LOG, line, -1);
		return;
	}
	if(rockchip_backend_line_percent(line, &percent))
	{
		if(stage)
			rockchip_backend_emit(cb, user, ROCKCHIP_BACKEND_EVENT_STAGE, stage, percent);
		rockchip_backend_emit(cb, user, ROCKCHIP_BACKEND_EVENT_PROGRESS, stage ? stage : line, percent);
		return;
	}
	rockchip_backend_emit(cb, user, ROCKCHIP_BACKEND_EVENT_LOG, line, -1);
}

static int rockchip_backend_run_tool_capture(char * const args[],
	rockchip_backend_event_cb cb, void * user)
{
	int pipefd[2];
	pid_t pid;
	int status;
	char buf[512];
	char line[1024];
	size_t line_len = 0;
	ssize_t nread;
	int skip_ansi = 0;

	if(pipe(pipefd) != 0)
	{
		char msg[160];
		snprintf(msg, sizeof(msg), "Error: failed to create pipe: %s", strerror(errno));
		rockchip_backend_emit(cb, user, ROCKCHIP_BACKEND_EVENT_LOG, msg, -1);
		return 0;
	}

	fflush(stdout);
	pid = fork();
	if(pid < 0)
	{
		char msg[160];
		close(pipefd[0]);
		close(pipefd[1]);
		snprintf(msg, sizeof(msg), "Error: failed to start %s: %s", args[0], strerror(errno));
		rockchip_backend_emit(cb, user, ROCKCHIP_BACKEND_EVENT_LOG, msg, -1);
		return 0;
	}
	if(pid == 0)
	{
		close(pipefd[0]);
		dup2(pipefd[1], STDOUT_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		close(pipefd[1]);
		execv(args[0], args);
		execvp(args[0], args);
		fprintf(stderr, "Error: failed to exec %s: %s\r\n", args[0], strerror(errno));
		_exit(127);
	}
	close(pipefd[1]);
	while((nread = read(pipefd[0], buf, sizeof(buf))) > 0)
	{
		for(ssize_t i = 0; i < nread; i++)
		{
			char ch = buf[i];
			if(skip_ansi)
			{
				if((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z'))
					skip_ansi = 0;
				continue;
			}
			if(ch == '\033')
			{
				skip_ansi = 1;
				continue;
			}
			if(ch == '\r' || ch == '\n')
			{
				if(line_len)
				{
					line[line_len] = 0;
					rockchip_backend_parse_tool_line(line, cb, user);
					line_len = 0;
				}
				continue;
			}
			if(line_len + 1 < sizeof(line))
				line[line_len++] = ch;
		}
	}
	if(line_len)
	{
		line[line_len] = 0;
		rockchip_backend_parse_tool_line(line, cb, user);
	}
	close(pipefd[0]);

	if(waitpid(pid, &status, 0) < 0)
	{
		char msg[160];
		snprintf(msg, sizeof(msg), "Error: failed to wait for %s: %s", args[0], strerror(errno));
		rockchip_backend_emit(cb, user, ROCKCHIP_BACKEND_EVENT_LOG, msg, -1);
		return 0;
	}
	if(WIFEXITED(status) && WEXITSTATUS(status) == 0)
		return 1;
	if(WIFEXITED(status))
	{
		char msg[160];
		snprintf(msg, sizeof(msg), "Error: %s exited with %d", args[0], WEXITSTATUS(status));
		rockchip_backend_emit(cb, user, ROCKCHIP_BACKEND_EVENT_LOG, msg, -1);
	}
	else
	{
		char msg[160];
		snprintf(msg, sizeof(msg), "Error: %s terminated unexpectedly", args[0]);
		rockchip_backend_emit(cb, user, ROCKCHIP_BACKEND_EVENT_LOG, msg, -1);
	}
	return 0;
}

static int rockchip_backend_run_tool_buffer(char * const args[], char * out, size_t out_size)
{
	int pipefd[2];
	pid_t pid;
	int status;
	size_t used = 0;
	ssize_t nread;

	if(out && out_size)
		out[0] = 0;
	if(pipe(pipefd) != 0)
	{
		printf("Error: failed to create pipe: %s\r\n", strerror(errno));
		return 0;
	}
	fflush(stdout);
	pid = fork();
	if(pid < 0)
	{
		close(pipefd[0]);
		close(pipefd[1]);
		printf("Error: failed to start %s: %s\r\n", args[0], strerror(errno));
		return 0;
	}
	if(pid == 0)
	{
		close(pipefd[0]);
		dup2(pipefd[1], STDOUT_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		close(pipefd[1]);
		execv(args[0], args);
		execvp(args[0], args);
		fprintf(stderr, "Error: failed to exec %s: %s\r\n", args[0], strerror(errno));
		_exit(127);
	}
	close(pipefd[1]);
	while(out && used + 1 < out_size &&
		(nread = read(pipefd[0], out + used, out_size - used - 1)) > 0)
	{
		used += nread;
		out[used] = 0;
	}
	while(read(pipefd[0], &(char){0}, 1) > 0)
		;
	close(pipefd[0]);
	if(waitpid(pid, &status, 0) < 0)
	{
		printf("Error: failed to wait for %s: %s\r\n", args[0], strerror(errno));
		return 0;
	}
	if(WIFEXITED(status) && WEXITSTATUS(status) == 0)
		return 1;
	if(WIFEXITED(status))
		printf("Error: %s exited with %d\r\n", args[0], WEXITSTATUS(status));
	else
		printf("Error: %s terminated unexpectedly\r\n", args[0]);
	return 0;
}

static char * rockchip_backend_size(char * buf, size_t buflen, uint64_t size)
{
	const char * unit[] = {"B", "KB", "MB", "GB", "TB"};
	int idx = 0;
	double value = size;

	while(value >= 1024.0 && idx < (int)ARRAY_SIZE(unit) - 1)
	{
		value /= 1024.0;
		idx++;
	}
	if(idx == 0)
		snprintf(buf, buflen, "%.0f %s", value, unit[idx]);
	else
		snprintf(buf, buflen, "%.2f %s", value, unit[idx]);
	return buf;
}

static void rockchip_backend_report(rockchip_backend_event_cb cb, void * user, const char * fmt, ...);

static int rockchip_backend_firmware_stat(const char * firmware, struct stat * st)
{
	if(!firmware || !firmware[0])
	{
		printf("Error: firmware file not specified\r\n");
		return 0;
	}
	if(stat(firmware, st) != 0 || !S_ISREG(st->st_mode))
	{
		printf("Error: firmware file not found: %s (%s)\r\n", firmware, strerror(errno));
		return 0;
	}
	if(access(firmware, R_OK) != 0)
	{
		printf("Error: unable to read firmware '%s': %s\r\n", firmware, strerror(errno));
		return 0;
	}
	return 1;
}

static int rockchip_backend_firmware_stat_cb(const char * firmware, struct stat * st,
	rockchip_backend_event_cb cb, void * user)
{
	if(!firmware || !firmware[0])
	{
		rockchip_backend_report(cb, user, "Error: firmware file not specified");
		return 0;
	}
	if(stat(firmware, st) != 0 || !S_ISREG(st->st_mode))
	{
		rockchip_backend_report(cb, user, "Error: firmware file not found: %s (%s)", firmware, strerror(errno));
		return 0;
	}
	if(access(firmware, R_OK) != 0)
	{
		rockchip_backend_report(cb, user, "Error: unable to read firmware '%s': %s", firmware, strerror(errno));
		return 0;
	}
	return 1;
}

static int rockchip_backend_valid_flash_mode(const char * mode)
{
	if(!mode)
		return 1;
	return !strcmp(mode, "partition") || !strcmp(mode, "keep_data") ||
		!strcmp(mode, "partition_erase") || !strcmp(mode, "full_erase");
}

static int rockchip_backend_valid_post_action(const char * post_action)
{
	if(!post_action)
		return 1;
	return !strcmp(post_action, "reboot") || !strcmp(post_action, "poweroff") ||
		!strcmp(post_action, "shutdown");
}

static void rockchip_backend_report(rockchip_backend_event_cb cb, void * user, const char * fmt, ...)
{
	char msg[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	if(cb)
		rockchip_backend_emit(cb, user, ROCKCHIP_BACKEND_EVENT_LOG, msg, -1);
	else
		printf("%s\r\n", msg);
}
#endif

int rockchip_backend_scan_devices(struct rockchip_device_info * devices, int max_devices)
{
	libusb_context * context = NULL;
	libusb_device ** list = NULL;
	ssize_t count;
	int found = 0;

	if(libusb_init(&context) != 0)
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
		libusb_device * device = list[i];
		struct rockchip_device_info * out;
		uint8_t ports[8] = { 0 };
		int nports;
		int pos = 0;

		if(libusb_get_device_descriptor(device, &desc) != 0)
			continue;
		if(desc.idVendor != 0x2207)
			continue;

		out = &devices[found++];
		memset(out, 0, sizeof(*out));
		out->bus = libusb_get_bus_number(device);
		out->vid = desc.idVendor;
		out->pid = desc.idProduct;
		snprintf(out->mode, sizeof(out->mode), "%s",
			((desc.bcdUSB & 0x0001) == 0x0000) ? "maskrom" : "loader");
		snprintf(out->chip, sizeof(out->chip), "%s", openrockcli_chip_name(desc.idProduct));

		nports = libusb_get_port_numbers(device, ports, sizeof(ports));
		if(nports > 0)
		{
			for(int p = 0; p < nports && pos < (int)sizeof(out->port) - 1; p++)
				pos += snprintf(out->port + pos, sizeof(out->port) - pos, "%s%u",
					p ? "." : "", ports[p]);
		}
		else
			snprintf(out->port, sizeof(out->port), "-");
	}
	libusb_free_device_list(list, 1);
	libusb_exit(context);
	return found;
}

int rockchip_backend_scan(int detailed, int verbose)
{
	struct rockchip_device_info devices[32];
	int found;

	(void)detailed;
	(void)verbose;
	printf("Scanning USB devices...\r\n\r\n");
	found = rockchip_backend_scan_devices(devices, ARRAY_SIZE(devices));
	if(!found)
	{
		printf("Error: Device not found\r\n");
		return 0;
	}
	printf("Bus  Port  VID:PID    Mode      Chip\r\n");
	printf("---  ----  ---------  --------  --------\r\n");
	for(int i = 0; i < found; i++)
	{
		printf("%03u  %-4.4s  %04x:%04x  %-8s  %s\r\n",
			devices[i].bus, devices[i].port, devices[i].vid, devices[i].pid,
			devices[i].mode, devices[i].chip);
	}
	return found;
}

#ifndef _WIN32
int rockchip_backend_list_devices(void)
{
	const char * tool = rockchip_backend_upgrade_tool();
	char * const args[] = { (char *)tool, "LD", NULL };

	return rockchip_backend_run_tool(args);
}

static void rockchip_backend_trim(char * s)
{
	char * end;

	if(!s)
		return;
	while(isspace((unsigned char)*s))
		memmove(s, s + 1, strlen(s));
	end = s + strlen(s);
	while(end > s && isspace((unsigned char)end[-1]))
		*--end = 0;
}

static int rockchip_backend_hex_u64(const char * s, uint64_t * value)
{
	char * end = NULL;
	unsigned long long v;

	if(!s || !value)
		return 0;
	v = strtoull(s, &end, 0);
	if(end == s)
		return 0;
	*value = v;
	return 1;
}

static void rockchip_backend_parse_entry_field(struct rockchip_firmware_entry * entry, char * field)
{
	char * eq;
	char * key;
	char * value;

	if(!entry || !field)
		return;
	rockchip_backend_trim(field);
	eq = strchr(field, '=');
	if(!eq)
		return;
	*eq = 0;
	key = field;
	value = eq + 1;
	rockchip_backend_trim(key);
	rockchip_backend_trim(value);
	if(!strcmp(key, "EntryNo"))
		entry->entry_no = strtol(value, NULL, 0);
	else if(!strcmp(key, "file"))
		snprintf(entry->file, sizeof(entry->file), "%s", value);
	else if(!strcmp(key, "partition"))
		snprintf(entry->partition, sizeof(entry->partition), "%s", value);
	else if(!strcmp(key, "type"))
		snprintf(entry->type, sizeof(entry->type), "%s", value);
	else if(!strcmp(key, "offset"))
		rockchip_backend_hex_u64(value, &entry->offset);
	else if(!strcmp(key, "size"))
		rockchip_backend_hex_u64(value, &entry->size);
}

static void rockchip_backend_parse_entry_line(struct rockchip_firmware_info * info, char * line)
{
	struct rockchip_firmware_entry * entry;
	char * save = NULL;
	char * field;

	if(!info || info->entry_count >= ROCKCHIP_BACKEND_MAX_FW_ENTRIES)
		return;
	entry = &info->entries[info->entry_count];
	memset(entry, 0, sizeof(*entry));
	entry->entry_no = -1;
	for(field = strtok_r(line, ";", &save); field; field = strtok_r(NULL, ";", &save))
		rockchip_backend_parse_entry_field(entry, field);
	if(entry->entry_no >= 0 || entry->file[0])
		info->entry_count++;
}

static void rockchip_backend_parse_sfi_output(struct rockchip_firmware_info * info, char * output)
{
	char * save = NULL;
	char * line;

	for(line = strtok_r(output, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save))
	{
		rockchip_backend_trim(line);
		if(!strncmp(line, "Type:", 5))
		{
			snprintf(info->image_type, sizeof(info->image_type), "%s", line + 5);
			rockchip_backend_trim(info->image_type);
		}
		else if(!strncmp(line, "Chip Tag:", 9))
		{
			char tmp[256];
			char * version;
			char * build;

			snprintf(tmp, sizeof(tmp), "%s", line + 9);
			version = strstr(tmp, "Version:");
			if(version)
			{
				char version_tmp[128];
				*version = 0;
				version += strlen("Version:");
				snprintf(version_tmp, sizeof(version_tmp), "%s", version);
				build = strstr(version_tmp, "Build Time:");
				if(build)
				{
					*build = 0;
					build += strlen("Build Time:");
					snprintf(info->build_time, sizeof(info->build_time), "%s", build);
					if((build = strstr(info->build_time, "Sign:")))
						*build = 0;
					rockchip_backend_trim(info->build_time);
				}
				snprintf(info->version, sizeof(info->version), "%.31s", version_tmp);
				rockchip_backend_trim(info->version);
			}
			snprintf(info->chip_tag, sizeof(info->chip_tag), "%.15s", tmp);
			rockchip_backend_trim(info->chip_tag);
		}
		else if(!strncmp(line, "EntryNo=", 8))
			rockchip_backend_parse_entry_line(info, line);
	}
}

int rockchip_backend_read_firmware_info(const char * firmware, struct rockchip_firmware_info * info)
{
	const char * tool = rockchip_backend_upgrade_tool();
	char * const args[] = { (char *)tool, "SFI", (char *)firmware, NULL };
	struct stat st;
	char * output;
	int ok;

	if(!info)
		return 0;
	memset(info, 0, sizeof(*info));
	if(!rockchip_backend_firmware_stat(firmware, &st))
		return 0;
	snprintf(info->path, sizeof(info->path), "%s", firmware);
	info->file_size = st.st_size;
	output = malloc(128 * 1024);
	if(!output)
	{
		printf("Error: failed to allocate firmware info buffer\r\n");
		return 0;
	}
	ok = rockchip_backend_run_tool_buffer(args, output, 128 * 1024);
	if(ok)
		rockchip_backend_parse_sfi_output(info, output);
	free(output);
	return ok;
}

int rockchip_backend_inspect_firmware(const char * firmware)
{
	struct rockchip_firmware_info info;
	char size[32];

	if(!rockchip_backend_read_firmware_info(firmware, &info))
		return 0;
	printf("Inspecting firmware...\r\n");
	printf("Firmware: %s\r\n", info.path);
	printf("Size: %s\r\n", rockchip_backend_size(size, sizeof(size), info.file_size));
	if(info.image_type[0])
		printf("Type: %s\r\n", info.image_type);
	if(info.chip_tag[0])
		printf("Chip Tag: %s\r\n", info.chip_tag);
	if(info.version[0])
		printf("Version: %s\r\n", info.version);
	if(info.build_time[0])
		printf("Build Time: %s\r\n", info.build_time);
	printf("Entry Count: %d\r\n", info.entry_count);
	for(int i = 0; i < info.entry_count; i++)
	{
		const struct rockchip_firmware_entry * e = &info.entries[i];
		printf("  [%02d] %-24s partition=%-12s type=%-10s offset=0x%llx size=0x%llx\r\n",
			e->entry_no,
			e->file[0] ? e->file : "-",
			e->partition[0] ? e->partition : "-",
			e->type[0] ? e->type : "-",
			(unsigned long long)e->offset,
			(unsigned long long)e->size);
	}
	return 1;
}

int rockchip_backend_unpack_firmware(const char * firmware, const char * outdir)
{
	const char * tool = rockchip_backend_upgrade_tool();
	char * const args[] = { (char *)tool, "EXF", (char *)firmware, (char *)outdir, NULL };
	struct stat st;
	char size[32];

	if(!rockchip_backend_firmware_stat(firmware, &st))
		return 0;
	if(!outdir || !outdir[0])
	{
		printf("Error: output directory not specified\r\n");
		return 0;
	}
	printf("Unpacking firmware...\r\n");
	printf("Firmware: %s\r\n", firmware);
	printf("Size: %s\r\n", rockchip_backend_size(size, sizeof(size), st.st_size));
	printf("Output: %s\r\n\r\n", outdir);
	return rockchip_backend_run_tool(args);
}

struct rockchip_backend_cli_state {
	char stage[96];
};

static void rockchip_backend_cli_event(const struct rockchip_backend_event * event, void * user)
{
	struct rockchip_backend_cli_state * state = user;

	if(!event)
		return;
	if(event->type == ROCKCHIP_BACKEND_EVENT_PROGRESS)
	{
		int percent = event->percent;
		int filled;

		if(percent < 0)
			percent = 0;
		if(percent > 100)
			percent = 100;
		filled = percent * 40 / 100;
		printf("\rFlashing: [");
		for(int i = 0; i < filled; i++)
			putchar('#');
		for(int i = filled; i < 40; i++)
			putchar('-');
		printf("] %3d%%", percent);
		fflush(stdout);
		if(percent >= 100)
			printf("\r\n");
		return;
	}
	if(event->type == ROCKCHIP_BACKEND_EVENT_STAGE)
	{
		if(state && event->text && !strcmp(state->stage, event->text))
			return;
		if(state && event->text)
			snprintf(state->stage, sizeof(state->stage), "%s", event->text);
		printf("Stage: %s\r\n", event->text);
		return;
	}
	if(event->text)
		printf("%s\r\n", event->text);
}

static const char * rockchip_backend_partition_flag(const char * partition)
{
	if(!partition)
		return NULL;
	if(!strcmp(partition, "parameter"))
		return "-p";
	if(!strcmp(partition, "boot"))
		return "-b";
	if(!strcmp(partition, "kernel"))
		return "-k";
	if(!strcmp(partition, "system"))
		return "-s";
	if(!strcmp(partition, "recovery"))
		return "-r";
	if(!strcmp(partition, "misc"))
		return "-m";
	if(!strcmp(partition, "uboot"))
		return "-u";
	if(!strcmp(partition, "trust"))
		return "-t";
	if(!strcmp(partition, "resource"))
		return "-re";
	return NULL;
}

struct rockchip_backend_partition_layout {
	char name[ROCKCHIP_BACKEND_MAX_NAME];
	uint64_t start;
	uint64_t sectors;
	int grow;
};

static int rockchip_backend_partition_selected(const char * csv, const char * partition)
{
	const char * p = csv;
	size_t len;

	if(!csv || !csv[0])
		return 1;
	if(!partition || !partition[0])
		return 0;
	len = strlen(partition);
	while(*p)
	{
		while(*p == ',' || isspace((unsigned char)*p))
			p++;
		if(!strncmp(p, partition, len) &&
			(p[len] == 0 || p[len] == ',' || isspace((unsigned char)p[len])))
			return 1;
		p = strchr(p, ',');
		if(!p)
			break;
	}
	return 0;
}

static int rockchip_backend_parse_partition_layout(const char * parameter,
	struct rockchip_backend_partition_layout * layouts, int max_layouts)
{
	FILE * fp;
	char data[16384];
	size_t n;
	char * p;
	int count = 0;

	fp = fopen(parameter, "rb");
	if(!fp)
		return 0;
	n = fread(data, 1, sizeof(data) - 1, fp);
	fclose(fp);
	data[n] = 0;
	p = strstr(data, "mtdparts=");
	if(!p)
		return 0;
	p += strlen("mtdparts=");
	if(*p == ':')
		p++;
	while(*p && *p != '\r' && *p != '\n' && !isspace((unsigned char)*p) && count < max_layouts)
	{
		struct rockchip_backend_partition_layout * layout = &layouts[count];
		uint64_t sectors = 0;
		uint64_t start = 0;
		int grow = 0;
		char * end;
		char * name;
		char * name_end;
		size_t name_len;

		while(*p == ',')
			p++;
		if(!*p || *p == '\r' || *p == '\n')
			break;
		if(*p == '-')
		{
			grow = 1;
			p++;
		}
		else
		{
			sectors = strtoull(p, &end, 0);
			if(end == p)
				break;
			p = end;
		}
		if(*p != '@')
			break;
		p++;
		start = strtoull(p, &end, 0);
		if(end == p || *end != '(')
			break;
		p = end + 1;
		name = p;
		name_end = strpbrk(name, ":)");
		if(!name_end)
			break;
		name_len = name_end - name;
		if(name_len >= sizeof(layout->name))
			name_len = sizeof(layout->name) - 1;
		memset(layout, 0, sizeof(*layout));
		memcpy(layout->name, name, name_len);
		layout->start = start;
		layout->sectors = sectors;
		layout->grow = grow;
		count++;
		p = strchr(name_end, ')');
		if(!p)
			break;
		p++;
		if(*p == ',')
			p++;
	}
	return count;
}

static const struct rockchip_backend_partition_layout * rockchip_backend_find_layout(
	const struct rockchip_backend_partition_layout * layouts, int count, const char * partition)
{
	for(int i = 0; i < count; i++)
	{
		if(!strcmp(layouts[i].name, partition))
			return &layouts[i];
	}
	return NULL;
}

static int rockchip_backend_file_sectors(const char * path, uint64_t * sectors)
{
	struct stat st;

	if(stat(path, &st) != 0)
		return 0;
	*sectors = ((uint64_t)st.st_size + 511) / 512;
	return 1;
}

static void rockchip_backend_join_path(char * out, size_t out_size, const char * dir, const char * file)
{
	snprintf(out, out_size, "%s/%s", dir, file);
}

static void rockchip_backend_remove_tree(const char * path)
{
	DIR * dir;
	struct dirent * ent;

	if(!path || !path[0])
		return;
	dir = opendir(path);
	if(!dir)
	{
		unlink(path);
		return;
	}
	while((ent = readdir(dir)))
	{
		char child[1024];
		struct stat st;

		if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
			continue;
		rockchip_backend_join_path(child, sizeof(child), path, ent->d_name);
		if(lstat(child, &st) == 0 && S_ISDIR(st.st_mode))
			rockchip_backend_remove_tree(child);
		else
			unlink(child);
	}
	closedir(dir);
	rmdir(path);
}

static int rockchip_backend_flash_partitions(const struct rockchip_flash_request * request,
	const struct rockchip_firmware_info * info, rockchip_backend_event_cb cb, void * user)
{
	const char * tool = rockchip_backend_upgrade_tool();
	char tmp_template[] = "/tmp/openrockcli-fw-XXXXXX";
	char * tmpdir;
	struct rockchip_backend_partition_layout layouts[ROCKCHIP_BACKEND_MAX_FW_ENTRIES];
	int layout_count = 0;
	int flashed = 0;
	int unsupported = 0;
	int ok = 1;

	tmpdir = mkdtemp(tmp_template);
	if(!tmpdir)
	{
		rockchip_backend_report(cb, user, "Error: failed to create temporary directory: %s", strerror(errno));
		return 0;
	}
	rockchip_backend_report(cb, user, "Extracting firmware entries...");
	{
		char * const exf_args[] = { (char *)tool, "EXF", (char *)request->firmware, tmpdir, NULL };
		if(!rockchip_backend_run_tool_capture(exf_args, cb, user))
		{
			rockchip_backend_remove_tree(tmpdir);
			return 0;
		}
	}
	{
		char parameter[1024];

		rockchip_backend_join_path(parameter, sizeof(parameter), tmpdir, "parameter.txt");
		layout_count = rockchip_backend_parse_partition_layout(parameter, layouts, ARRAY_SIZE(layouts));
		if(layout_count > 0)
			rockchip_backend_report(cb, user, "Loaded partition layout from parameter.txt.");
	}

	for(int i = 0; i < info->entry_count; i++)
	{
		const struct rockchip_firmware_entry * entry = &info->entries[i];
		const char * flag;
		char image[1024];
		char msg[192];
		char * di_args[] = { (char *)tool, "DI", NULL, NULL, NULL };

		if(!entry->partition[0] || !rockchip_backend_partition_selected(request->partitions, entry->partition))
			continue;
		if(!request->partitions && !strcmp(entry->partition, "parameter"))
			continue;
		if(!strcmp(entry->partition, "parameter") && strcmp(entry->file, "parameter.txt"))
			continue;
		flag = rockchip_backend_partition_flag(entry->partition);
		if(!entry->file[0])
		{
			rockchip_backend_report(cb, user, "Error: partition '%s' has no image file in firmware", entry->partition);
			ok = 0;
			continue;
		}
		rockchip_backend_join_path(image, sizeof(image), tmpdir, entry->file);
		if(access(image, R_OK) != 0)
		{
			rockchip_backend_report(cb, user, "Error: extracted image missing for partition '%s': %s", entry->partition, image);
			ok = 0;
			continue;
		}
		snprintf(msg, sizeof(msg), "Flashing partition %s", entry->partition);
		rockchip_backend_emit(cb, user, ROCKCHIP_BACKEND_EVENT_STAGE, "Flashing Partitions", -1);
		rockchip_backend_emit(cb, user, ROCKCHIP_BACKEND_EVENT_LOG, msg, -1);
		if(flag)
		{
			di_args[2] = (char *)flag;
			di_args[3] = image;
			if(!rockchip_backend_run_tool_capture(di_args, cb, user))
			{
				ok = 0;
				break;
			}
		}
		else
		{
			const struct rockchip_backend_partition_layout * layout =
				rockchip_backend_find_layout(layouts, layout_count, entry->partition);
			uint64_t image_sectors;
			char start_arg[32];
			char size_arg[32];
			char * wl_args[] = { (char *)tool, "WL", start_arg, size_arg, image, NULL };

			if(!layout)
			{
				rockchip_backend_report(cb, user, "Error: partition '%s' is not in parameter.txt", entry->partition);
				unsupported++;
				ok = 0;
				continue;
			}
			if(!rockchip_backend_file_sectors(image, &image_sectors) || image_sectors == 0)
			{
				rockchip_backend_report(cb, user, "Error: cannot get image size for partition '%s'", entry->partition);
				ok = 0;
				continue;
			}
			if(!layout->grow && layout->sectors > 0 && image_sectors > layout->sectors)
			{
				rockchip_backend_report(cb, user,
					"Error: image for partition '%s' is larger than parameter.txt partition size",
					entry->partition);
				ok = 0;
				continue;
			}
			snprintf(start_arg, sizeof(start_arg), "0x%llx", (unsigned long long)layout->start);
			snprintf(size_arg, sizeof(size_arg), "0x%llx", (unsigned long long)image_sectors);
			snprintf(msg, sizeof(msg), "Writing partition %s at LBA %s", entry->partition, start_arg);
			rockchip_backend_emit(cb, user, ROCKCHIP_BACKEND_EVENT_LOG, msg, -1);
			if(!rockchip_backend_run_tool_capture(wl_args, cb, user))
			{
				ok = 0;
				break;
			}
		}
		flashed++;
	}

	if(!flashed && !unsupported)
	{
		rockchip_backend_report(cb, user, "Error: no selected partitions can be flashed");
		ok = 0;
	}
	rockchip_backend_remove_tree(tmpdir);
	return ok && flashed > 0;
}

int rockchip_backend_flash_with_cb(const struct rockchip_flash_request * request,
	rockchip_backend_event_cb cb, void * user)
{
	const char * tool = rockchip_backend_upgrade_tool();
	const char * post_action = request && request->post_action ? request->post_action : "reboot";
	const char * firmware = request ? request->firmware : NULL;
	char * const reset_args[] = { (char *)tool, "UF", (char *)firmware, NULL };
	char * const noreset_args[] = { (char *)tool, "UF", (char *)firmware, "-noreset", NULL };
	struct stat st;
	char size[32];
	struct rockchip_device_info devices[32];
	int device_count;
	int ok;

	if(!rockchip_backend_firmware_stat_cb(firmware, &st, cb, user))
		return 0;
	if(!rockchip_backend_valid_flash_mode(request ? request->mode : NULL))
	{
		rockchip_backend_report(cb, user, "Error: unsupported flash mode '%s'", request->mode);
		return 0;
	}
	if(!rockchip_backend_valid_post_action(post_action))
	{
		rockchip_backend_report(cb, user, "Error: unsupported post action '%s'", post_action);
		return 0;
	}
	if(access(tool, X_OK) != 0 && strchr(tool, '/') != NULL)
	{
		rockchip_backend_report(cb, user, "Error: upgrade_tool not executable: %s (%s)", tool, strerror(errno));
		return 0;
	}
	device_count = rockchip_backend_scan_devices(devices, ARRAY_SIZE(devices));
	if(device_count <= 0)
	{
		rockchip_backend_report(cb, user, "Error: Device not found");
		return 0;
	}
	if(device_count > 1 && !(request && (request->bus || request->port)))
	{
		rockchip_backend_report(cb, user,
			"Warning: multiple Rockchip devices detected; upgrade_tool may prompt or choose its default device.");
	}

	if(request && request->verbose)
	{
		rockchip_backend_report(cb, user, "Firmware: %s", request->firmware);
		rockchip_backend_report(cb, user, "Size: %s", rockchip_backend_size(size, sizeof(size), st.st_size));
		rockchip_backend_report(cb, user, "Mode: %s", request->mode ? request->mode : "full_erase");
		rockchip_backend_report(cb, user, "Partitions: %s", request->partitions ? request->partitions : "all");
		rockchip_backend_report(cb, user, "Verify: %s", request->verify ? "true" : "false");
		rockchip_backend_report(cb, user, "Post action: %s", post_action);
		if(request->bus || request->port)
			rockchip_backend_report(cb, user, "Device selector: bus=%s port=%s",
				request->bus ? request->bus : "-", request->port ? request->port : "-");
	}
	if(request && (request->bus || request->port))
		rockchip_backend_report(cb, user,
			"Warning: upgrade_tool does not expose stable bus/port selection here; flashing the selected Rockusb device.");
	if(request && request->partitions && (!request->mode || strcmp(request->mode, "partition")))
		rockchip_backend_report(cb, user,
			"Warning: partition filtering is accepted for OpenixCLI-style compatibility, but Rockchip update.img flashing uses the package table.");
	if(request && !request->verify)
		rockchip_backend_report(cb, user,
			"Warning: verification is controlled by Rockchip upgrade_tool for update.img packages.");
	if(!strcmp(post_action, "poweroff") || !strcmp(post_action, "shutdown"))
		rockchip_backend_report(cb, user,
			"Warning: Rockchip upgrade_tool has no poweroff/shutdown action for update.img; using -noreset after flash.");

	rockchip_backend_report(cb, user, "Starting flash...");
	rockchip_backend_emit(cb, user, ROCKCHIP_BACKEND_EVENT_STAGE, "Query Device", -1);

	if(request && request->mode && !strcmp(request->mode, "partition"))
	{
		struct rockchip_firmware_info info;

		memset(&info, 0, sizeof(info));
		rockchip_backend_report(cb, user, "Partition mode: extracting update.img and flashing selected entries with upgrade_tool DI.");
		if(!rockchip_backend_read_firmware_info(request->firmware, &info))
		{
			rockchip_backend_report(cb, user, "Error: failed to inspect firmware package");
			return 0;
		}
		ok = rockchip_backend_flash_partitions(request, &info, cb, user);
		if(ok)
		{
			rockchip_backend_emit(cb, user, ROCKCHIP_BACKEND_EVENT_STAGE, "Setting Mode", 100);
			if(cb)
			{
				char msg[96];
				snprintf(msg, sizeof(msg), "Flash complete! Device will %s.", post_action);
				rockchip_backend_emit(cb, user, ROCKCHIP_BACKEND_EVENT_LOG, msg, 100);
			}
			else
				printf("Flash complete! Device will %s.\r\n", post_action);
		}
		else
			rockchip_backend_report(cb, user, "Flash failed.");
		return ok;
	}

	rockchip_backend_emit(cb, user, ROCKCHIP_BACKEND_EVENT_STAGE, "Flashing Partitions", -1);
	if(cb)
	{
		char msg[256];
		snprintf(msg, sizeof(msg), "Flashing firmware package with %s", tool);
		rockchip_backend_emit(cb, user, ROCKCHIP_BACKEND_EVENT_LOG, msg, -1);
	}
	else
		printf("Flashing firmware package with %s\r\n", tool);
	if(!strcmp(post_action, "poweroff") || !strcmp(post_action, "shutdown"))
		ok = cb ? rockchip_backend_run_tool_capture(noreset_args, cb, user) : rockchip_backend_run_tool(noreset_args);
	else
		ok = cb ? rockchip_backend_run_tool_capture(reset_args, cb, user) : rockchip_backend_run_tool(reset_args);
	if(ok)
	{
		rockchip_backend_emit(cb, user, ROCKCHIP_BACKEND_EVENT_STAGE, "Setting Mode", 100);
		if(cb)
		{
			char msg[96];
			snprintf(msg, sizeof(msg), "Flash complete! Device will %s.", post_action);
			rockchip_backend_emit(cb, user, ROCKCHIP_BACKEND_EVENT_LOG, msg, 100);
		}
		else
			printf("Flash complete! Device will %s.\r\n", post_action);
	}
	else
		rockchip_backend_report(cb, user, "Flash failed.");
	return ok;
}

int rockchip_backend_flash(const struct rockchip_flash_request * request)
{
	struct rockchip_backend_cli_state state;

	memset(&state, 0, sizeof(state));
	return rockchip_backend_flash_with_cb(request, rockchip_backend_cli_event, &state);
}
#else
static void rockchip_backend_windows_unsupported(const char * command)
{
	printf("Error: '%s' is not available in the Windows build because it requires Rockchip upgrade_tool.\r\n",
		command);
	printf("Use the Linux build or Rockchip's Windows flashing tool for update.img workflows.\r\n");
}

int rockchip_backend_list_devices(void)
{
	rockchip_backend_windows_unsupported("devices");
	return 0;
}

int rockchip_backend_read_firmware_info(const char * firmware, struct rockchip_firmware_info * info)
{
	(void)firmware;
	if(info)
		memset(info, 0, sizeof(*info));
	rockchip_backend_windows_unsupported("inspect");
	return 0;
}

int rockchip_backend_inspect_firmware(const char * firmware)
{
	(void)firmware;
	rockchip_backend_windows_unsupported("inspect");
	return 0;
}

int rockchip_backend_unpack_firmware(const char * firmware, const char * outdir)
{
	(void)firmware;
	(void)outdir;
	rockchip_backend_windows_unsupported("unpack");
	return 0;
}

int rockchip_backend_flash_with_cb(const struct rockchip_flash_request * request,
	rockchip_backend_event_cb cb, void * user)
{
	(void)request;
	(void)cb;
	(void)user;
	rockchip_backend_windows_unsupported("flash");
	return 0;
}

int rockchip_backend_flash(const struct rockchip_flash_request * request)
{
	(void)request;
	rockchip_backend_windows_unsupported("flash");
	return 0;
}
#endif
