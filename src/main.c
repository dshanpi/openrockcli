#include <rock.h>
#include <rockchip_backend.h>
#ifdef _WIN32
#include <conio.h>
#else
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#endif

static const char * manufacturer[] = {
	"Samsung",
	"Toshiba",
	"Hynix",
	"Infineon",
	"Micron",
	"Renesas",
	"ST",
	"Intel",
	"SanDisk",
};

static void usage(void);
static void command_help(const char * command);
struct tui_state_t;
static void tui_draw(const struct tui_state_t * state);

static int modern_flash(int argc, char * argv[])
{
	struct rockchip_flash_request request;
	const char * firmware = NULL;
	const char * mode = "full_erase";
	const char * partitions = NULL;
	const char * post_action = "reboot";
	const char * bus = NULL;
	const char * port = NULL;
	int verify = 1;
	int verbose = 0;

	for(int i = 2; i < argc; i++)
	{
		if((!strcmp(argv[i], "-a") || !strcmp(argv[i], "--post-action")) && (i + 1 < argc))
			post_action = argv[++i];
		else if(!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose"))
			verbose = 1;
		else if(!strcmp(argv[i], "-V") || !strcmp(argv[i], "--verify"))
		{
			if((i + 1 < argc) && argv[i + 1][0] != '-')
			{
				const char * value = argv[++i];
				verify = strcmp(value, "false") && strcmp(value, "0") && strcmp(value, "no");
			}
			else
				verify = 1;
		}
		else if(!strcmp(argv[i], "--no-verify"))
			verify = 0;
		else if((!strcmp(argv[i], "-m") || !strcmp(argv[i], "--mode")) && (i + 1 < argc))
			mode = argv[++i];
		else if((!strcmp(argv[i], "-p") || !strcmp(argv[i], "--partitions")) && (i + 1 < argc))
			partitions = argv[++i];
		else if((!strcmp(argv[i], "-b") || !strcmp(argv[i], "--bus")) && (i + 1 < argc))
			bus = argv[++i];
		else if((!strcmp(argv[i], "-P") || !strcmp(argv[i], "--port")) && (i + 1 < argc))
			port = argv[++i];
		else if(argv[i][0] != '-' && !firmware)
			firmware = argv[i];
		else if(argv[i][0] != '-')
		{
			printf("Error: unexpected extra firmware argument '%s'\r\n", argv[i]);
			return 0;
		}
		else
		{
			printf("Error: unsupported flash option '%s'\r\n", argv[i]);
			return 0;
		}
	}
	if(!firmware)
	{
		command_help("flash");
		return 0;
	}

	if(strcmp(mode, "partition") && strcmp(mode, "keep_data") &&
		strcmp(mode, "partition_erase") && strcmp(mode, "full_erase"))
	{
		printf("Error: unsupported flash mode '%s'\r\n", mode);
		return 0;
	}
	if(strcmp(post_action, "reboot") && strcmp(post_action, "poweroff") &&
		strcmp(post_action, "shutdown"))
	{
		printf("Error: unsupported post action '%s'\r\n", post_action);
		return 0;
	}
	memset(&request, 0, sizeof(request));
	request.firmware = firmware;
	request.mode = mode;
	request.partitions = partitions;
	request.post_action = post_action;
	request.bus = bus;
	request.port = port;
	request.verify = verify;
	request.verbose = verbose;
	return rockchip_backend_flash(&request);
}

#define TUI_MAX_DEVICES		16
#define TUI_MAX_LOGS		12
#define TUI_MODE_FULL		0
#define TUI_MODE_KEEP		1
#define TUI_MODE_PART_ERASE	2
#define TUI_MODE_PARTITION	3
#define TUI_KEY_UP		1001
#define TUI_KEY_DOWN		1002
#define TUI_KEY_RIGHT		1003
#define TUI_KEY_LEFT		1004

struct tui_state_t {
	struct rockchip_device_info devices[TUI_MAX_DEVICES];
	int device_count;
	int selected_device;
	int focus;
	int mode;
	int verify;
	int post_action;
	int show_help;
	int flashing;
	int progress_percent;
	int firmware_loaded;
	int part_cursor;
	int selected_parts[ROCKCHIP_BACKEND_MAX_FW_ENTRIES];
	char firmware[512];
	char partitions_arg[1024];
	struct rockchip_firmware_info firmware_info;
	char current_stage[96];
	char logs[TUI_MAX_LOGS][160];
	int log_count;
};

static void tui_get_size(int * width, int * height)
{
	struct winsize ws;

	*width = 100;
	*height = 30;
	if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0)
	{
		if(ws.ws_col > 0)
			*width = ws.ws_col;
		if(ws.ws_row > 0)
			*height = ws.ws_row;
	}
}

static void tui_repeat(char ch, int count)
{
	while(count-- > 0)
		putchar(ch);
}

static void tui_log(struct tui_state_t * state, const char * level, const char * message)
{
	char line[160];

	snprintf(line, sizeof(line), "[%s] %s", level, message);
	if(state->log_count < TUI_MAX_LOGS)
		snprintf(state->logs[state->log_count++], sizeof(state->logs[0]), "%s", line);
	else
	{
		for(int i = 1; i < TUI_MAX_LOGS; i++)
			snprintf(state->logs[i - 1], sizeof(state->logs[0]), "%s", state->logs[i]);
		snprintf(state->logs[TUI_MAX_LOGS - 1], sizeof(state->logs[0]), "%s", line);
	}
}

static void tui_backend_event(const struct rockchip_backend_event * event, void * user)
{
	struct tui_state_t * state = user;

	if(!event || !state)
		return;
	if(event->type == ROCKCHIP_BACKEND_EVENT_PROGRESS)
	{
		state->progress_percent = event->percent;
		if(state->progress_percent < 0)
			state->progress_percent = 0;
		if(state->progress_percent > 100)
			state->progress_percent = 100;
		tui_draw(state);
		return;
	}
	if(event->type == ROCKCHIP_BACKEND_EVENT_STAGE)
	{
		if(event->text && !strcmp(state->current_stage, event->text))
		{
			tui_draw(state);
			return;
		}
		snprintf(state->current_stage, sizeof(state->current_stage), "%s", event->text ? event->text : "");
		tui_log(state, "INFO", state->current_stage);
		tui_draw(state);
		return;
	}
	if(event->text)
	{
		tui_log(state, strstr(event->text, "Error") || strstr(event->text, "Fail") ? "ERRO" : "INFO", event->text);
		tui_draw(state);
	}
}

static const char * tui_mode_name(int mode)
{
	switch(mode)
	{
	case TUI_MODE_KEEP:
		return "keep_data";
	case TUI_MODE_PART_ERASE:
		return "partition_erase";
	case TUI_MODE_PARTITION:
		return "partition";
	default:
		return "full_erase";
	}
}

static const char * tui_post_action_name(int post_action)
{
	switch(post_action)
	{
	case 1:
		return "poweroff";
	case 2:
		return "shutdown";
	default:
		return "reboot";
	}
}

static int tui_entry_is_partition(const struct rockchip_firmware_entry * entry)
{
	const char * p = entry ? entry->partition : NULL;

	if(!p || !p[0])
		return 0;
	return !strcmp(p, "boot") || !strcmp(p, "kernel") || !strcmp(p, "system") ||
		!strcmp(p, "recovery") || !strcmp(p, "misc") || !strcmp(p, "uboot") ||
		!strcmp(p, "trust") || !strcmp(p, "resource") || !strcmp(p, "rootfs") ||
		!strcmp(p, "oem") || !strcmp(p, "userdata");
}

static void tui_init_partitions(struct tui_state_t * state)
{
	state->part_cursor = 0;
	memset(state->selected_parts, 0, sizeof(state->selected_parts));
	for(int i = 0; i < state->firmware_info.entry_count; i++)
	{
		if(tui_entry_is_partition(&state->firmware_info.entries[i]))
		{
			state->selected_parts[i] = 1;
			if(!tui_entry_is_partition(&state->firmware_info.entries[state->part_cursor]))
				state->part_cursor = i;
		}
	}
}

static void tui_move_partition(struct tui_state_t * state, int delta)
{
	int count = state->firmware_info.entry_count;
	int pos = state->part_cursor;

	if(!state->firmware_loaded || count <= 0)
		return;
	for(int step = 0; step < count; step++)
	{
		pos += delta;
		if(pos < 0)
			pos = count - 1;
		if(pos >= count)
			pos = 0;
		if(tui_entry_is_partition(&state->firmware_info.entries[pos]))
		{
			state->part_cursor = pos;
			return;
		}
	}
}

static void tui_toggle_partition(struct tui_state_t * state)
{
	if(!state->firmware_loaded)
		return;
	if(state->part_cursor >= 0 && state->part_cursor < state->firmware_info.entry_count &&
		tui_entry_is_partition(&state->firmware_info.entries[state->part_cursor]))
		state->selected_parts[state->part_cursor] = !state->selected_parts[state->part_cursor];
}

static void tui_toggle_all_partitions(struct tui_state_t * state)
{
	int all_selected = 1;

	if(!state->firmware_loaded)
		return;
	for(int i = 0; i < state->firmware_info.entry_count; i++)
	{
		if(tui_entry_is_partition(&state->firmware_info.entries[i]) && !state->selected_parts[i])
			all_selected = 0;
	}
	for(int i = 0; i < state->firmware_info.entry_count; i++)
	{
		if(tui_entry_is_partition(&state->firmware_info.entries[i]))
			state->selected_parts[i] = !all_selected;
	}
}

static const char * tui_selected_partitions_arg(struct tui_state_t * state)
{
	int pos = 0;
	int selected = 0;
	int selectable = 0;

	state->partitions_arg[0] = 0;
	if(state->mode != TUI_MODE_PARTITION || !state->firmware_loaded)
		return NULL;
	for(int i = 0; i < state->firmware_info.entry_count; i++)
	{
		const struct rockchip_firmware_entry * e = &state->firmware_info.entries[i];
		if(!tui_entry_is_partition(e))
			continue;
		selectable++;
		if(!state->selected_parts[i])
			continue;
		selected++;
		pos += snprintf(state->partitions_arg + pos, sizeof(state->partitions_arg) - pos,
			"%s%s", pos ? "," : "", e->partition);
		if(pos >= (int)sizeof(state->partitions_arg))
			break;
	}
	if(!selected)
		return NULL;
	return state->partitions_arg;
}

static void tui_scan_devices(struct tui_state_t * state)
{
	state->selected_device = 0;
	state->device_count = rockchip_backend_scan_devices(state->devices, TUI_MAX_DEVICES);

	if(state->device_count)
	{
		char msg[96];
		snprintf(msg, sizeof(msg), "Found %d Rockchip USB device%s.",
			state->device_count, state->device_count == 1 ? "" : "s");
		tui_log(state, "OKAY", msg);
	}
	else
		tui_log(state, "WARN", "No Rockchip USB devices found.");
}

static void tui_print_title(int width)
{
	const char * title = " OpenRockCLI Terminal v1.2.0";
	const char * help = "[H]elp  [Q]uit ";
	int pad = width - (int)strlen(title) - (int)strlen(help);

	printf("\033[48;5;236m\033[38;5;51m%s\033[0m", title);
	if(pad < 1)
		pad = 1;
	printf("\033[48;5;236m");
	tui_repeat(' ', pad);
	printf("\033[38;5;245m%s\033[0m\r\n", help);
}

static void tui_print_status(int width, const struct tui_state_t * state)
{
	const char * active = state->flashing ?
		" Flashing in progress...  Ctrl+C to abort" :
		" Tab: panel  R: scan  B: firmware  M: mode  V: verify  A: action  Enter: flash";

	printf("\033[48;5;236m\033[38;5;245m");
	printf("%-*.*s", width, width, active);
	printf("\033[0m\r\n");
}

static void tui_box_top(int width)
{
	putchar('+');
	tui_repeat('-', width - 2);
	printf("+\r\n");
}

static void tui_box_row(int width, const char * title, const char * text)
{
	int body = width - 4;

	printf("| ");
	if(title)
		printf("\033[38;5;51m%s\033[0m", title);
	if(text)
		printf("%s", text);
	tui_repeat(' ', body - (title ? (int)strlen(title) : 0) - (text ? (int)strlen(text) : 0));
	printf(" |\r\n");
}

static void tui_draw_help(int width, int height)
{
	int w = width > 70 ? 58 : width - 4;
	int h = 18;
	int x = (width - w) / 2 + 1;
	int y = (height - h) / 2 + 1;
	const char * lines[] = {
		" Q / Esc      Quit application",
		" Tab          Switch focus: Devices / Options",
		" Up/Down      Select device",
		" R            Refresh device scan",
		" B / F        Enter firmware path",
		" Enter        Start flash",
		" M            Cycle flash mode",
		" V            Toggle verify",
		" A            Cycle post action / Select all parts",
		" Space        Toggle partition selection",
		" D            Show upgrade_tool devices",
		" H            Toggle this help",
		" Ctrl+C       Abort / Quit",
		"",
		" Press any key to close",
	};

	printf("\033[%d;%dH+", y, x);
	tui_repeat('-', w - 2);
	putchar('+');
	for(int i = 0; i < h - 2; i++)
	{
		printf("\033[%d;%dH|", y + i + 1, x);
		if(i == 0)
			printf("\033[38;5;51m Help \033[0m");
		else if(i - 1 < (int)ARRAY_SIZE(lines))
			printf(" %-*.*s", w - 4, w - 4, lines[i - 1]);
		tui_repeat(' ', w - 2 - (i == 0 ? 6 : (i - 1 < (int)ARRAY_SIZE(lines) ? (int)strlen(lines[i - 1]) + 1 : 0)));
		putchar('|');
	}
	printf("\033[%d;%dH+", y + h - 1, x);
	tui_repeat('-', w - 2);
	putchar('+');
}

static void tui_draw(const struct tui_state_t * state)
{
	int width;
	int height;
	int main_h;
	int left_w;
	int right_w;
	int progress_h = 7;
	int log_h;
	const char * focus_dev = state->focus == 0 ? "\033[38;5;51m" : "\033[38;5;240m";
	const char * focus_opt = state->focus == 1 ? "\033[38;5;51m" : "\033[38;5;240m";
	char path[80];

	tui_get_size(&width, &height);
	if(width < 60 || height < 15)
	{
		printf("\033[2J\033[H\033[38;5;196mTerminal too small. Minimum: 60x15\033[0m\r\n");
		fflush(stdout);
		return;
	}
	main_h = height - 2;
	left_w = width >= 100 ? width * 40 / 100 : width;
	right_w = width - left_w;
	log_h = main_h - progress_h;
	if(right_w < 34)
	{
		left_w = width;
		right_w = 0;
	}

	printf("\033[2J\033[H");
	tui_print_title(width);

	if(right_w)
	{
		printf("%s+", focus_dev);
		tui_repeat('-', left_w - 2);
		printf("+%s+", focus_opt);
		tui_repeat('-', right_w - 2);
		printf("+\033[0m\r\n");
		printf("| DEVICES          [R]efresh");
		tui_repeat(' ', left_w - 29);
		printf("| FIRMWARE & OPTIONS");
		tui_repeat(' ', right_w - 21);
		printf("|\r\n");
		printf("+");
		tui_repeat('-', left_w - 2);
		printf("+");
		tui_repeat('-', right_w - 2);
		printf("+\r\n");

		for(int row = 0; row < 7; row++)
		{
			char left_text[128] = "";
			char right_text[256] = "";

			if(row < state->device_count)
			{
				const struct rockchip_device_info * dev = &state->devices[row];
				snprintf(left_text, sizeof(left_text), "%c Bus %03u Port %-8.8s %-7.7s %-8.8s",
					row == state->selected_device ? '>' : ' ',
					dev->bus, dev->port, dev->mode, dev->chip);
			}
			else if(row == 0)
				snprintf(left_text, sizeof(left_text), "  No devices found.");
			else if(row == 1)
				snprintf(left_text, sizeof(left_text), "  Connect device & press R");

			if(row == 0)
			{
				snprintf(path, sizeof(path), "%.79s", state->firmware[0] ? state->firmware : "(none)");
				snprintf(right_text, sizeof(right_text), "Firmware: %s", path);
			}
			else if(row == 1)
			{
				if(state->firmware_loaded)
				{
					char size[32];
					double mb = (double)state->firmware_info.file_size / (1024.0 * 1024.0);
					snprintf(size, sizeof(size), "%.2f MB", mb);
					snprintf(right_text, sizeof(right_text), "Size: %s  Entries: %d", size, state->firmware_info.entry_count);
				}
				else
					snprintf(right_text, sizeof(right_text), "Press [B] enter path");
			}
			else if(row == 2 && state->firmware_loaded)
				snprintf(right_text, sizeof(right_text), "Chip: %s  Type: %.24s",
					state->firmware_info.chip_tag[0] ? state->firmware_info.chip_tag : "-",
					state->firmware_info.image_type[0] ? state->firmware_info.image_type : "-");
			else if(row == 3)
				snprintf(right_text, sizeof(right_text), "%c Mode: %s", state->focus == 1 ? '>' : ' ', tui_mode_name(state->mode));
			else if(row == 4)
				snprintf(right_text, sizeof(right_text), "  Verify: %s", state->verify ? "true" : "false");
			else if(row == 5)
				snprintf(right_text, sizeof(right_text), "  Post action: %s", tui_post_action_name(state->post_action));
			else if(row == 6)
			{
				if(state->mode == TUI_MODE_PARTITION && state->firmware_loaded)
				{
					const struct rockchip_firmware_entry * e = &state->firmware_info.entries[state->part_cursor];
					snprintf(right_text, sizeof(right_text), "  Parts: [%c] %-12.12s  Space toggle",
						state->selected_parts[state->part_cursor] ? 'x' : ' ',
						tui_entry_is_partition(e) ? e->partition : "-");
				}
				else
					snprintf(right_text, sizeof(right_text), "  Partitions: %s",
						state->firmware_loaded ? "from package table" : "package table");
			}

			printf("| %-*.*s| %-*.*s|\r\n",
				left_w - 3, left_w - 3, left_text,
				right_w - 3, right_w - 3, right_text);
		}
	}
	else
	{
		tui_box_top(width);
		tui_box_row(width, "DEVICES          [R]efresh", "");
		for(int row = 0; row < 4; row++)
		{
			char text[96] = "";
			if(row < state->device_count)
			{
				const struct rockchip_device_info * dev = &state->devices[row];
				snprintf(text, sizeof(text), " %c Bus %03u Port %-8.8s %-7.7s %-8.8s",
					row == state->selected_device ? '>' : ' ',
					dev->bus, dev->port, dev->mode, dev->chip);
			}
			else if(row == 0)
				snprintf(text, sizeof(text), " No devices found.");
			else if(row == 1)
				snprintf(text, sizeof(text), " Connect device & press R");
			tui_box_row(width, NULL, text);
		}
		tui_box_top(width);
		tui_box_row(width, "FIRMWARE & OPTIONS", "");
		snprintf(path, sizeof(path), " Firmware: %.67s", state->firmware[0] ? state->firmware : "(none)");
		tui_box_row(width, NULL, path);
		if(state->firmware_loaded)
		{
			double mb = (double)state->firmware_info.file_size / (1024.0 * 1024.0);
			snprintf(path, sizeof(path), " Size: %.2f MB    Entries: %d    Chip: %s",
				mb, state->firmware_info.entry_count,
				state->firmware_info.chip_tag[0] ? state->firmware_info.chip_tag : "-");
			tui_box_row(width, NULL, path);
		}
		snprintf(path, sizeof(path), " Mode: %s    Verify: %s", tui_mode_name(state->mode), state->verify ? "true" : "false");
		tui_box_row(width, NULL, path);
		if(state->mode == TUI_MODE_PARTITION && state->firmware_loaded)
		{
			const struct rockchip_firmware_entry * e = &state->firmware_info.entries[state->part_cursor];
			snprintf(path, sizeof(path), " Part: [%c] %.24s",
				state->selected_parts[state->part_cursor] ? 'x' : ' ',
				tui_entry_is_partition(e) ? e->partition : "-");
			tui_box_row(width, NULL, path);
		}
		snprintf(path, sizeof(path), " Post action: %s", tui_post_action_name(state->post_action));
		tui_box_row(width, NULL, path);
	}

	tui_box_top(width);
	tui_box_row(width, "PROGRESS", "");
	if(state->flashing)
	{
		char bar[96];
		int filled = state->progress_percent * 40 / 100;
		int pos = 0;

		pos += snprintf(bar + pos, sizeof(bar) - pos, " [");
		for(int i = 0; i < 40 && pos + 1 < (int)sizeof(bar); i++)
			bar[pos++] = i < filled ? '#' : '-';
		snprintf(bar + pos, sizeof(bar) - pos, "] %3d%%  %.32s",
			state->progress_percent, state->current_stage[0] ? state->current_stage : "Flashing...");
		tui_box_row(width, NULL, bar);
	}
	else if(state->progress_percent >= 100)
		tui_box_row(width, NULL, " [########################################] 100%  Complete");
	else if(state->firmware[0] && state->device_count)
		tui_box_row(width, NULL, " [----------------------------------------] Ready to flash");
	else
		tui_box_row(width, NULL, " [----------------------------------------] Waiting for task...");
	snprintf(path, sizeof(path), " Stage: %.70s",
		state->current_stage[0] ? state->current_stage : "Init > Query > Flash > Reboot");
	tui_box_row(width, NULL, path);

	tui_box_top(width);
	tui_box_row(width, "LOG", "");
	for(int i = 0; i < log_h - 8 && i < TUI_MAX_LOGS; i++)
	{
		int idx = state->log_count - (log_h - 8) + i;
		tui_box_row(width, NULL, idx >= 0 ? state->logs[idx] : "");
	}
	tui_box_top(width);

	if(state->firmware[0] && state->device_count && !state->flashing)
		tui_box_row(width, NULL, " >>> [ FLASH ] <<<  Press Enter");
	else if(state->flashing)
		tui_box_row(width, NULL, "   Flashing...");
	else
		tui_box_row(width, NULL, "   [ FLASH ] (disabled)");
	tui_box_top(width);

	tui_print_status(width, state);
	if(state->show_help)
		tui_draw_help(width, height);
	fflush(stdout);
}

#ifndef _WIN32
static int tui_read_key(void)
{
	unsigned char ch;

	if(read(STDIN_FILENO, &ch, 1) != 1)
		return -1;
	if(ch == 27)
	{
		fd_set set;
		struct timeval tv;
		unsigned char seq[2];

		FD_ZERO(&set);
		FD_SET(STDIN_FILENO, &set);
		tv.tv_sec = 0;
		tv.tv_usec = 50000;
		if(select(STDIN_FILENO + 1, &set, NULL, NULL, &tv) <= 0)
			return 27;
		if(read(STDIN_FILENO, &seq[0], 1) != 1)
			return 27;
		if(seq[0] != '[')
			return 27;
		if(read(STDIN_FILENO, &seq[1], 1) != 1)
			return 27;
		switch(seq[1])
		{
		case 'A':
			return TUI_KEY_UP;
		case 'B':
			return TUI_KEY_DOWN;
		case 'C':
			return TUI_KEY_RIGHT;
		case 'D':
			return TUI_KEY_LEFT;
		default:
			return 27;
		}
	}
	return ch;
}

static int tui_prompt_path(char * firmware, size_t firmware_len)
{
	struct termios term;

	tcgetattr(STDIN_FILENO, &term);
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &term);
	printf("\033[2J\033[H");
	printf("Firmware path: ");
	fflush(stdout);
	if(!fgets(firmware, firmware_len, stdin))
		return 0;
	firmware[strcspn(firmware, "\r\n")] = 0;
	return 1;
}

static int tui(void)
{
	struct termios old_term;
	struct termios raw_term;
	struct tui_state_t state;
	int raw_enabled = 0;
	int rc = 0;

	if(!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
	{
		char line[512];
		int choice = 0;

		printf("openrockcli interactive flasher\r\n");
		printf("1. Scan devices\r\n");
		printf("2. Flash update.img firmware\r\n");
		printf("3. List Rockusb devices with upgrade_tool\r\n");
		printf("4. Quit\r\n");
		printf("> ");
		fflush(stdout);
		if(!fgets(line, sizeof(line), stdin))
			return 0;
		choice = strtol(line, NULL, 0);
		if(choice == 1)
		{
			rockchip_backend_scan(0, 0);
			return 0;
		}
		if(choice == 2)
		{
			struct rockchip_flash_request request;
			printf("Firmware path: ");
			fflush(stdout);
			if(!fgets(line, sizeof(line), stdin))
				return 0;
			line[strcspn(line, "\r\n")] = 0;
			memset(&request, 0, sizeof(request));
			request.firmware = line;
			request.mode = "full_erase";
			request.post_action = "reboot";
			request.verify = 1;
			return rockchip_backend_flash(&request) ? 0 : 1;
		}
		if(choice == 3)
			return rockchip_backend_list_devices() ? 0 : 1;
		return 0;
	}

	if(tcgetattr(STDIN_FILENO, &old_term) == 0)
	{
		raw_term = old_term;
		raw_term.c_lflag &= ~(ICANON | ECHO);
		raw_term.c_cc[VMIN] = 1;
		raw_term.c_cc[VTIME] = 0;
		if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw_term) == 0)
			raw_enabled = 1;
	}
	memset(&state, 0, sizeof(state));
	state.verify = 1;
	tui_log(&state, "INFO", "Welcome to OpenRockCLI Terminal");
	tui_log(&state, "INFO", "Press H for help, Q to quit");
	tui_scan_devices(&state);

	printf("\033[?1049h\033[?25l");
	tui_draw(&state);
	while(1)
	{
		int key = tui_read_key();
		if(key < 0)
			break;
		if(key == 'q' || key == 'Q' || key == 27 || key == 3)
			break;
		if(state.show_help)
		{
			state.show_help = 0;
			tui_draw(&state);
			continue;
		}
		if(key == 'h' || key == 'H')
		{
			state.show_help = 1;
			tui_draw(&state);
		}
		else if(key == 'r' || key == 'R')
		{
			tui_log(&state, "INFO", "Scanning USB devices...");
			tui_scan_devices(&state);
			tui_draw(&state);
		}
		else if(key == 'd' || key == 'D')
		{
			if(raw_enabled)
				tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_term);
			printf("\033[2J\033[H");
			rockchip_backend_list_devices();
			printf("\nPress any key to return...");
			fflush(stdout);
			if(raw_enabled)
				tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw_term);
			tui_read_key();
			tui_log(&state, "INFO", "upgrade_tool device listing finished.");
			tui_draw(&state);
		}
		else if(key == 'f' || key == 'F' || key == 'b' || key == 'B')
		{
			if(raw_enabled)
				tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_term);
			if(tui_prompt_path(state.firmware, sizeof(state.firmware)))
			{
				state.firmware_loaded = 0;
				memset(&state.firmware_info, 0, sizeof(state.firmware_info));
				if(state.firmware[0])
				{
					if(rockchip_backend_read_firmware_info(state.firmware, &state.firmware_info))
					{
						state.firmware_loaded = 1;
						tui_init_partitions(&state);
						tui_log(&state, "OKAY", "Firmware selected.");
					}
					else
						tui_log(&state, "ERRO", "Failed to inspect firmware.");
				}
				else
					tui_log(&state, "WARN", "Firmware selection cleared.");
			}
			if(raw_enabled)
				tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw_term);
			tui_draw(&state);
		}
		else if(key == '\t')
		{
			state.focus = state.focus ? 0 : 1;
			tui_draw(&state);
		}
		else if(key == 'm' || key == 'M')
		{
			state.mode = (state.mode + 1) % 4;
			tui_draw(&state);
		}
		else if(key == 'v' || key == 'V')
		{
			state.verify = !state.verify;
			tui_draw(&state);
		}
		else if(key == 'a' || key == 'A')
		{
			if(state.focus == 1 && state.mode == TUI_MODE_PARTITION)
				tui_toggle_all_partitions(&state);
			else
				state.post_action = (state.post_action + 1) % 3;
			tui_draw(&state);
		}
		else if(key == ' ')
		{
			if(state.focus == 1 && state.mode == TUI_MODE_PARTITION)
				tui_toggle_partition(&state);
			tui_draw(&state);
		}
		else if(key == TUI_KEY_UP || key == 'k' || key == 'K')
		{
			if(state.focus == 1 && state.mode == TUI_MODE_PARTITION)
				tui_move_partition(&state, -1);
			else if(state.selected_device > 0)
				state.selected_device--;
			tui_draw(&state);
		}
		else if(key == TUI_KEY_DOWN || key == 'j' || key == 'J')
		{
			if(state.focus == 1 && state.mode == TUI_MODE_PARTITION)
				tui_move_partition(&state, 1);
			else if(state.selected_device + 1 < state.device_count)
				state.selected_device++;
			tui_draw(&state);
		}
		else if(key == TUI_KEY_LEFT)
		{
			state.mode = (state.mode + 3) % 4;
			tui_draw(&state);
		}
		else if(key == TUI_KEY_RIGHT)
		{
			state.mode = (state.mode + 1) % 4;
			tui_draw(&state);
		}
		else if(key == '\r' || key == '\n')
		{
			if(!state.firmware[0])
			{
				tui_log(&state, "WARN", "Select a firmware path first.");
				tui_draw(&state);
				continue;
			}
			if(!state.device_count)
			{
				tui_log(&state, "WARN", "No device selected. Press R to scan.");
				tui_draw(&state);
				continue;
			}
			state.flashing = 1;
			state.progress_percent = 0;
			state.current_stage[0] = 0;
			tui_log(&state, "INFO", "Starting flash...");
			tui_draw(&state);
			{
				struct rockchip_flash_request request;
				memset(&request, 0, sizeof(request));
				request.firmware = state.firmware;
				request.mode = tui_mode_name(state.mode);
				request.partitions = tui_selected_partitions_arg(&state);
				request.post_action = tui_post_action_name(state.post_action);
				request.verify = state.verify;
				rc = rockchip_backend_flash_with_cb(&request, tui_backend_event, &state) ? 0 : 1;
			}
			state.flashing = 0;
			tui_log(&state, rc ? "ERRO" : "OKAY", rc ? "Flash failed." : "Flash finished.");
			tui_draw(&state);
		}
		else
		{
			tui_draw(&state);
		}
	}
	printf("\033[?25h\033[?1049l");
	if(raw_enabled)
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_term);
	return rc;
}
#else
static int tui(void)
{
	char line[512];
	int choice = 0;

	printf("openrockcli interactive flasher\r\n");
	printf("1. Scan devices\r\n");
	printf("2. Flash update.img firmware (not available in Windows build)\r\n");
	printf("3. List Rockusb devices with upgrade_tool (not available in Windows build)\r\n");
	printf("4. Quit\r\n");
	printf("> ");
	fflush(stdout);
	if(!fgets(line, sizeof(line), stdin))
		return 0;
	choice = strtol(line, NULL, 0);
	if(choice == 1)
	{
		rockchip_backend_scan(0, 0);
		return 0;
	}
	if(choice == 2 || choice == 3)
	{
		printf("Error: this Windows build does not run Rockchip upgrade_tool workflows.\r\n");
		return 1;
	}
	return 0;
}
#endif

static void usage(void)
{
	printf("Firmware flashing CLI tool for Rockchip chips\r\n");
	printf("\r\n");
	printf("Usage: openrockcli [OPTIONS] [COMMAND]\r\n");
	printf("\r\n");
	printf("Commands:\r\n");
	printf("  scan     Scan for connected devices\r\n");
	printf("  flash    Flash firmware to device\r\n");
	printf("  inspect  Inspect firmware contents (image header and package table)\r\n");
	printf("  unpack   Unpack firmware data to disk\r\n");
	printf("  tui      Launch interactive TUI mode\r\n");
	printf("  devices  List Rockusb devices with upgrade_tool\r\n");
	printf("  help     Print this message or the help of the given subcommand(s)\r\n");
	printf("\r\n");
	printf("Options:\r\n");
	printf("  -v, --verbose  Enable verbose output\r\n");
	printf("  -h, --help     Print help\r\n");
	printf("  -V, --version  Print version\r\n");
	printf("\r\n");
	printf("Low-level Rockchip commands are still available; run `openrockcli help low-level`.\r\n");
}

static void low_level_help(void)
{
	printf("Low-level Rockchip commands\r\n\r\n");
	printf("Usage: openrockcli <COMMAND> [ARGS]\r\n\r\n");
	printf("Commands:\r\n");
	printf("  maskrom <ddr> <usbplug> [--rc4-off]    Initial chip using ddr and usbplug in maskrom mode\r\n");
	printf("  download <loader>                      Initial chip using loader in maskrom mode\r\n");
	printf("  upgrade <loader>                       Upgrade loader to flash in loader mode\r\n");
	printf("  ready                                  Show chip ready or not\r\n");
	printf("  version                                Show chip version\r\n");
	printf("  capability                             Show capability information\r\n");
	printf("  reset [maskrom]                        Reset chip to normal or maskrom mode\r\n");
	printf("  dump <address> <length>                Dump memory region in hex format\r\n");
	printf("  read <address> <length> <file>         Read memory to file\r\n");
	printf("  write <address> <file>                 Write file to memory\r\n");
	printf("  exec <address> [dtb]                   Call function address\r\n");
	printf("  otp <length>                           Dump otp memory in hex format\r\n");
	printf("  sn                                     Read serial number\r\n");
	printf("  sn <string>                            Write serial number\r\n");
	printf("  vs dump <index> <length> [type]        Dump vendor storage in hex format\r\n");
	printf("  vs read <index> <length> <file> [type] Read vendor storage\r\n");
	printf("  vs write <index> <file> [type]         Write vendor storage\r\n");
	printf("  storage [index]                        Read or switch storage media\r\n");
	printf("  flash                                  Detect flash and show information\r\n");
	printf("  flash erase <sector> <count>           Erase flash sector\r\n");
	printf("  flash read <sector> <count> <file>     Read flash sector to file\r\n");
	printf("  flash write <sector> <file>            Write file to flash sector\r\n");
	printf("\r\nExtra:\r\n");
	printf("  extra maskrom --rc4 <on|off> [--sram <file> --delay <ms>] [--dram <file> --delay <ms>] [...]\r\n");
	printf("  extra maskrom-dump-arm32 --rc4 <on|off> --uart <register> <address> <length>\r\n");
	printf("  extra maskrom-dump-arm64 --rc4 <on|off> --uart <register> <address> <length>\r\n");
	printf("  extra maskrom-write-arm32 --rc4 <on|off> <address> <file>\r\n");
	printf("  extra maskrom-write-arm64 --rc4 <on|off> <address> <file>\r\n");
	printf("  extra maskrom-exec-arm32 --rc4 <on|off> <address>\r\n");
	printf("  extra maskrom-exec-arm64 --rc4 <on|off> <address>\r\n");
}

static void command_help(const char * command)
{
	if(!command || !strcmp(command, "help"))
	{
		usage();
		return;
	}
	if(!strcmp(command, "scan"))
	{
		printf("Scan for connected devices\r\n\r\n");
		printf("Usage: openrockcli scan [OPTIONS]\r\n\r\n");
		printf("Options:\r\n");
		printf("  -l, --detailed  Get detailed device information\r\n");
		printf("  -v, --verbose   Enable verbose output\r\n");
		printf("  -h, --help      Print help\r\n");
		return;
	}
	if(!strcmp(command, "flash"))
	{
		printf("Flash firmware to device\r\n\r\n");
		printf("Usage: openrockcli flash [OPTIONS] <FIRMWARE>\r\n\r\n");
		printf("Arguments:\r\n");
		printf("  <FIRMWARE>  Path to firmware file\r\n\r\n");
		printf("Options:\r\n");
		printf("  -b, --bus <BUS>                  USB bus number\r\n");
		printf("  -P, --port <PORT>                USB port number\r\n");
		printf("  -V, --verify <VERIFY>            Enable verification after write [default: true] [possible values: true, false]\r\n");
		printf("      --no-verify                  Disable verification preference\r\n");
		printf("  -m, --mode <MODE>                Flash mode: partition, keep_data, partition_erase, full_erase [default: full_erase]\r\n");
		printf("  -p, --partitions <PARTITIONS>    Partitions to flash in partition mode (boot,uboot,misc,recovery,rootfs,oem,userdata,...)\r\n");
		printf("  -a, --post-action <POST_ACTION>  Post-flash action: reboot, poweroff, shutdown [default: reboot]\r\n");
		printf("  -v, --verbose                    Enable verbose output\r\n");
		printf("  -h, --help                       Print help\r\n");
		return;
	}
	if(!strcmp(command, "tui"))
	{
		printf("Launch interactive TUI mode\r\n\r\n");
		printf("Usage: openrockcli tui [OPTIONS]\r\n\r\n");
		printf("Options:\r\n");
		printf("  -v, --verbose  Enable verbose output\r\n");
		printf("  -h, --help     Print help\r\n");
		return;
	}
	if(!strcmp(command, "devices"))
	{
		printf("List Rockusb devices with upgrade_tool\r\n\r\n");
		printf("Usage: openrockcli devices\r\n");
		return;
	}
	if(!strcmp(command, "inspect") || !strcmp(command, "info"))
	{
		printf("Inspect firmware contents (image header and package table)\r\n\r\n");
		printf("Usage: openrockcli inspect <FIRMWARE>\r\n");
		return;
	}
	if(!strcmp(command, "unpack") || !strcmp(command, "extract"))
	{
		printf("Unpack firmware data to disk\r\n\r\n");
		printf("Usage: openrockcli unpack <FIRMWARE> <DIR>\r\n");
		return;
	}
	if(!strcmp(command, "low-level"))
	{
		low_level_help();
		return;
	}
	printf("error: unrecognized subcommand '%s'\r\n\r\n", command);
	usage();
}

int main(int argc, char * argv[])
{
	struct openrockcli_ctx_t ctx;

	if(argc < 2)
	{
		return tui();
	}
	while(argc >= 2 && (!strcmp(argv[1], "-v") || !strcmp(argv[1], "--verbose")))
	{
		argc--;
		argv++;
	}
	if(argc < 2)
	{
		return tui();
	}
	if(!strcmp(argv[1], "-V") || !strcmp(argv[1], "--version"))
	{
		printf("openrockcli 1.2.0\r\n");
		return 0;
	}
	if(!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))
	{
		usage();
		return 0;
	}
	if(!strcmp(argv[1], "help"))
	{
		command_help(argc >= 3 ? argv[2] : NULL);
		return 0;
	}
	if(!strcmp(argv[1], "tui"))
	{
		if(argc >= 3 && (!strcmp(argv[2], "-h") || !strcmp(argv[2], "--help")))
		{
			command_help("tui");
			return 0;
		}
		return tui();
	}
	if(!strcmp(argv[1], "scan"))
	{
		int detailed = 0;
		int verbose = 0;
		if(argc >= 3 && (!strcmp(argv[2], "-h") || !strcmp(argv[2], "--help")))
		{
			command_help("scan");
			return 0;
		}
		for(int i = 2; i < argc; i++)
		{
			if(!strcmp(argv[i], "-l") || !strcmp(argv[i], "--detailed"))
			{
				detailed = 1;
				continue;
			}
			if(!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose"))
			{
				verbose = 1;
				continue;
			}
			printf("Error: unsupported scan option '%s'\r\n", argv[i]);
			return 1;
		}
		rockchip_backend_scan(detailed, verbose);
		return 0;
	}
	if(!strcmp(argv[1], "devices"))
	{
		if(argc >= 3 && (!strcmp(argv[2], "-h") || !strcmp(argv[2], "--help")))
		{
			command_help("devices");
			return 0;
		}
		return rockchip_backend_list_devices() ? 0 : 1;
	}
	if(!strcmp(argv[1], "inspect") || !strcmp(argv[1], "info"))
	{
		if(argc >= 3 && (!strcmp(argv[2], "-h") || !strcmp(argv[2], "--help")))
		{
			command_help("inspect");
			return 0;
		}
		if(argc != 3)
		{
			command_help("inspect");
			return 1;
		}
		return rockchip_backend_inspect_firmware(argv[2]) ? 0 : 1;
	}
	if(!strcmp(argv[1], "unpack") || !strcmp(argv[1], "extract"))
	{
		if(argc >= 3 && (!strcmp(argv[2], "-h") || !strcmp(argv[2], "--help")))
		{
			command_help("unpack");
			return 0;
		}
		if(argc != 4)
		{
			command_help("unpack");
			return 1;
		}
		return rockchip_backend_unpack_firmware(argv[2], argv[3]) ? 0 : 1;
	}
	if(!strcmp(argv[1], "flash") && argc >= 3 &&
		(!strcmp(argv[2], "-h") || !strcmp(argv[2], "--help")))
	{
		command_help("flash");
		return 0;
	}
	if(!strcmp(argv[1], "flash") && argc >= 3 &&
		strcmp(argv[2], "erase") && strcmp(argv[2], "read") && strcmp(argv[2], "write"))
	{
		return modern_flash(argc, argv) ? 0 : 1;
	}

	libusb_init(&ctx.context);
	if(!openrockcli_init(&ctx))
	{
		printf("Error: Device not found\r\n");
		if(ctx.hdl)
			libusb_close(ctx.hdl);
		libusb_exit(ctx.context);
		return -1;
	}
	if(!strcmp(argv[1], "maskrom"))
	{
		argc -= 2;
		argv += 2;
		if(argc >= 2)
		{
			if(ctx.maskrom)
			{
				int rc4 = 1;
				if((argc == 3) && !strcmp(argv[2], "--rc4-off"))
					rc4 = 0;
				rock_maskrom_upload_file(&ctx, 0x471, argv[0], rc4);
				usleep(10 * 1000);
				rock_maskrom_upload_file(&ctx, 0x472, argv[1], rc4);
				usleep(10 * 1000);
			}
			else
				printf("Error: The chip '%s' does not in maskrom mode\r\n", ctx.chip->name);
		}
		else
			usage();
	}
	else if(!strcmp(argv[1], "download"))
	{
		argc -= 2;
		argv += 2;
		if(argc == 1)
		{
			if(ctx.maskrom)
			{
				struct rkloader_ctx_t * lctx = rkloader_ctx_alloc(argv[0]);
				if(lctx)
				{
					for(int i = 0; i < lctx->nentry; i++)
					{
						struct rkloader_entry_t * e = lctx->entry[i];
						char str[256];
						if(e->type == RKLOADER_ENTRY_471)
						{
							void * buf = (char *)lctx->buffer + get_unaligned_le32(&e->data_offset);
							uint64_t len = get_unaligned_le32(&e->data_size);
							uint32_t delay = get_unaligned_le32(&e->data_delay);

							printf("Downloading '%s'\r\n", loader_wide2str(str, (uint8_t *)&e->name[0], sizeof(e->name)));
							rock_maskrom_upload_memory(&ctx, 0x471, buf, len, lctx->is_rc4on);
							usleep(delay * 1000);
						}
						else if(e->type == RKLOADER_ENTRY_472)
						{
							void * buf = (char *)lctx->buffer + get_unaligned_le32(&e->data_offset);
							uint64_t len = get_unaligned_le32(&e->data_size);
							uint32_t delay = get_unaligned_le32(&e->data_delay);

							printf("Downloading '%s'\r\n", loader_wide2str(str, (uint8_t *)&e->name[0], sizeof(e->name)));
							rock_maskrom_upload_memory(&ctx, 0x472, buf, len, lctx->is_rc4on);
							usleep(delay * 1000);
						}
					}
					rkloader_ctx_free(lctx);
				}
				else
					printf("Error: Not a valid loader '%s'\r\n", argv[0]);
			}
			else
				printf("Error: The chip '%s' does not in maskrom mode\r\n", ctx.chip->name);
		}
		else
			usage();
	}
	else if(!strcmp(argv[1], "upgrade"))
	{
		argc -= 2;
		argv += 2;
		if(argc == 1)
		{
			struct rkloader_ctx_t * lctx = rkloader_ctx_alloc(argv[0]);
			if(lctx)
			{
				uint32_t sec = 64;
				enum storage_type_t type = rock_storage_read(&ctx);
				switch(type)
				{
				case STORAGE_TYPE_FLASH:
					sec = 64;
					break;
				case STORAGE_TYPE_EMMC:
					sec = 64;
					break;
				case STORAGE_TYPE_SD:
					sec = 64;
					break;
				case STORAGE_TYPE_SD1:
					sec = 64;
					break;
				case STORAGE_TYPE_SPINOR:
					sec = 128;
					break;
				case STORAGE_TYPE_SPINAND:
					sec = 512;
					break;
				case STORAGE_TYPE_RAM:
					sec = 64;
					break;
				case STORAGE_TYPE_USB:
					sec = 64;
					break;
				case STORAGE_TYPE_SATA:
					sec = 64;
					break;
				case STORAGE_TYPE_PCIE:
					sec = 64;
					break;
				default:
					break;
				}
				struct flash_info_t info;
				if(rock_flash_detect(&ctx, &info))
				{
					if(!rock_flash_write_lba_progress(&ctx, sec, lctx->idblen / 512, lctx->idbbuf))
						printf("Failed to write flash\r\n");
				}
				else
					printf("Failed to detect flash\r\n");
				rkloader_ctx_free(lctx);
			}
			else
				printf("Error: Not a valid loader '%s'\r\n", argv[0]);
		}
		else
			usage();
	}
	else if(!strcmp(argv[1], "ready"))
	{
		argc -= 2;
		argv += 2;
		if(argc == 0)
		{
			if(rock_ready(&ctx))
				printf("The chip is ready\r\n");
			else
				printf("Failed to show chip ready status\r\n");
		}
		else
			usage();
	}
	else if(!strcmp(argv[1], "version"))
	{
		argc -= 2;
		argv += 2;
		if(argc == 0)
		{
			uint8_t buf[16];
			if(rock_version(&ctx, buf))
				printf("%s(%c%c%c%c): 0x%02x%02x%02x%02x 0x%02x%02x%02x%02x 0x%02x%02x%02x%02x 0x%02x%02x%02x%02x\r\n", ctx.chip->name,
					buf[ 3], buf[ 2], buf[ 1], buf[ 0],
					buf[ 3], buf[ 2], buf[ 1], buf[ 0],
					buf[ 7], buf[ 6], buf[ 5], buf[ 4],
					buf[11], buf[10], buf[ 9], buf[ 8],
					buf[15], buf[14], buf[13], buf[12]);
			else
				printf("Failed to get chip version\r\n");
		}
		else
			usage();
	}
	else if(!strcmp(argv[1], "capability"))
	{
		argc -= 2;
		argv += 2;
		if(argc == 0)
		{
			uint8_t buf[8];
			if(rock_capability(&ctx, buf))
			{
				printf("Capability: %02x %02x %02x %02x %02x %02x %02x %02x\r\n",
					buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);
				printf("    Direct LBA: %s\r\n", (buf[0] & (1 << 0)) ? "enabled" : "disabled");
				printf("    Vendor Storage: %s\r\n", (buf[0] & (1 << 1)) ? "enabled" : "disabled");
				printf("    First 4M Access: %s\r\n", (buf[0] & (1 << 2)) ? "enabled" : "disabled");
				printf("    Read LBA: %s\r\n", (buf[0] & (1 << 3)) ? "enabled" : "disabled");
				printf("    New Vendor Storage: %s\r\n", (buf[0] & (1 << 4)) ? "enabled" : "disabled");
				printf("    Read Com Log: %s\r\n", (buf[0] & (1 << 5)) ? "enabled" : "disabled");
				printf("    Read IDB Config: %s\r\n", (buf[0] & (1 << 6)) ? "enabled" : "disabled");
				printf("    Read Secure Mode: %s\r\n", (buf[0] & (1 << 7)) ? "enabled" : "disabled");
				printf("    New IDB: %s\r\n", (buf[1] & (1 << 0)) ? "enabled" : "disabled");
				printf("    Switch Storage: %s\r\n", (buf[1] & (1 << 1)) ? "enabled" : "disabled");
				printf("    LBA Parity: %s\r\n", (buf[1] & (1 << 2)) ? "enabled" : "disabled");
				printf("    Read OTP Chip: %s\r\n", (buf[1] & (1 << 3)) ? "enabled" : "disabled");
				printf("    Switch USB3: %s\r\n", (buf[1] & (1 << 4)) ? "enabled" : "disabled");
			}
			else
				printf("Failed to show capability information\r\n");
		}
		else
			usage();
	}
	else if(!strcmp(argv[1], "reset"))
	{
		argc -= 2;
		argv += 2;
		if(argc > 0)
		{
			if(!strcmp(argv[0], "maskrom"))
				rock_reset(&ctx, 1);
			else
				usage();
		}
		else
			rock_reset(&ctx, 0);
	}
	else if(!strcmp(argv[1], "dump"))
	{
		argc -= 2;
		argv += 2;
		if(argc == 2)
		{
			uint32_t addr = strtoul(argv[0], NULL, 0);
			size_t len = strtoul(argv[1], NULL, 0);
			char * buf = malloc(len);
			if(buf)
			{
				rock_read(&ctx, addr, buf, len);
				hexdump(addr, buf, len);
				free(buf);
			}
		}
		else
			usage();
	}
	else if(!strcmp(argv[1], "read"))
	{
		argc -= 2;
		argv += 2;
		if(argc == 3)
		{
			uint32_t addr = strtoul(argv[0], NULL, 0);
			size_t len = strtoul(argv[1], NULL, 0);
			char * buf = malloc(len);
			if(buf)
			{
				rock_read_progress(&ctx, addr, buf, len);
				file_save(argv[2], buf, len);
				free(buf);
			}
		}
		else
			usage();
	}
	else if(!strcmp(argv[1], "write"))
	{
		argc -= 2;
		argv += 2;
		if(argc == 2)
		{
			uint32_t addr = strtoul(argv[0], NULL, 0);
			uint64_t len;
			void * buf = file_load(argv[1], &len);
			if(buf)
			{
				rock_write_progress(&ctx, addr, buf, len);
				free(buf);
			}
		}
		else
			usage();
	}
	else if(!strcmp(argv[1], "exec"))
	{
		argc -= 2;
		argv += 2;
		if(argc >= 1)
		{
			uint32_t addr = strtoul(argv[0], NULL, 0);
			uint32_t dtb = (argc >= 2) ? strtoul(argv[1], NULL, 0) : 0;
			rock_exec(&ctx, addr, dtb);
		}
		else
			usage();
	}
	else if(!strcmp(argv[1], "otp"))
	{
		if(rock_capability_support(&ctx, CAPABILITY_TYPE_READ_OTP_CHIP))
		{
			argc -= 2;
			argv += 2;
			if(argc == 1)
			{
				int len = strtoul(argv[0], NULL, 0);
				if(len > 0)
				{
					uint8_t * otp = malloc(len);
					if(otp)
					{
						if(rock_otp_read(&ctx, otp, len))
							hexdump(0, otp, len);
						free(otp);
					}
				}
			}
			else
				usage();
		}
		else
			printf("The loader don't support dump otp\r\n");
	}
	else if(!strcmp(argv[1], "sn"))
	{
		if(rock_capability_support(&ctx, CAPABILITY_TYPE_VENDOR_STORAGE) || rock_capability_support(&ctx, CAPABILITY_TYPE_NEW_VENDOR_STORAGE))
		{
			argc -= 2;
			argv += 2;
			if(argc == 0)
			{
				char sn[512 - 8 + 1];
				if(rock_sn_read(&ctx, sn))
					printf("SN: %s\r\n", sn);
				else
					printf("No serial number\r\n");
			}
			else
			{
				if(argc == 1)
				{
					if(rock_sn_write(&ctx, argv[0]))
						printf("Write serial number '%s'\r\n", argv[0]);
					else
						printf("Failed to write serial number\r\n");
				}
				else
					usage();
			}
		}
		else
			printf("The loader don't support vendor storage\r\n");
	}
	else if(!strcmp(argv[1], "vs"))
	{
		if(rock_capability_support(&ctx, CAPABILITY_TYPE_VENDOR_STORAGE) || rock_capability_support(&ctx, CAPABILITY_TYPE_NEW_VENDOR_STORAGE))
		{
			argc -= 2;
			argv += 2;
			if(argc > 0)
			{
				if(!strcmp(argv[0], "dump") && (argc >= 3))
				{
					int index = strtoul(argv[1], NULL, 0);
					int len = XMIN((int)strtoul(argv[2], NULL, 0), 512);
					int type = (argc == 4) ? strtoul(argv[3], NULL, 0) : 0;
					if(len > 0)
					{
						uint8_t * buf = malloc(len);
						if(buf)
						{
							if(rock_vs_read(&ctx, type, index, buf, len))
								hexdump(0, buf, len);
							free(buf);
						}
					}
				}
				else if(!strcmp(argv[0], "read") && (argc >= 4))
				{
					int index = strtoul(argv[1], NULL, 0);
					int len = XMIN((int)strtoul(argv[2], NULL, 0), 512);
					int type = (argc == 5) ? strtoul(argv[4], NULL, 0) : 0;
					if(len > 0)
					{
						uint8_t * buf = malloc(len);
						if(buf)
						{
							if(rock_vs_read(&ctx, type, index, buf, len))
								file_save(argv[3], buf, len);
							free(buf);
						}
					}
				}
				else if(!strcmp(argv[0], "write") && (argc >= 3))
				{
					int index = strtoul(argv[1], NULL, 0);
					int type = (argc == 4) ? strtoul(argv[3], NULL, 0) : 0;
					uint64_t len;
					void * buf = file_load(argv[2], &len);
					if(buf && (len > 0))
					{
						if(!rock_vs_write(&ctx, type, index, buf, (len > 512) ? 512 : len))
							printf("Failed to write vendor storage\r\n");
						free(buf);
					}
				}
				else
					usage();
			}
			else
				usage();
		}
		else
			printf("The loader don't support vendor storage\r\n");
	}
	else if(!strcmp(argv[1], "storage"))
	{
		argc -= 2;
		argv += 2;
		if(argc == 0)
		{
			enum storage_type_t type = rock_storage_read(&ctx);
			printf("%s 0.UNKNOWN\r\n", (type == STORAGE_TYPE_UNKNOWN) ? "-->" : "   ");
			printf("%s 1.FLASH\r\n", (type == STORAGE_TYPE_FLASH) ? "-->" : "   ");
			printf("%s 2.EMMC\r\n", (type == STORAGE_TYPE_EMMC) ? "-->" : "   ");
			printf("%s 3.SD\r\n", (type == STORAGE_TYPE_SD) ? "-->" : "   ");
			printf("%s 4.SD1\r\n", (type == STORAGE_TYPE_SD1) ? "-->" : "   ");
			printf("%s 5.SPINOR\r\n", (type == STORAGE_TYPE_SPINOR) ? "-->" : "   ");
			printf("%s 6.SPINAND\r\n", (type == STORAGE_TYPE_SPINAND) ? "-->" : "   ");
			printf("%s 7.RAM\r\n", (type == STORAGE_TYPE_RAM) ? "-->" : "   ");
			printf("%s 8.USB\r\n", (type == STORAGE_TYPE_USB) ? "-->" : "   ");
			printf("%s 9.SATA\r\n", (type == STORAGE_TYPE_SATA) ? "-->" : "   ");
			printf("%s10.PCIE\r\n", (type == STORAGE_TYPE_PCIE) ? "-->" : "   ");
		}
		else
		{
			if(rock_capability_support(&ctx, CAPABILITY_TYPE_SWITCH_STORAGE))
			{
				if(argc == 1)
				{
					enum storage_type_t type = STORAGE_TYPE_UNKNOWN;
					int index = strtol(argv[0], NULL, 0);
					switch(index)
					{
					case 0:
						type = STORAGE_TYPE_UNKNOWN;
						break;
					case 1:
						type = STORAGE_TYPE_FLASH;
						break;
					case 2:
						type = STORAGE_TYPE_EMMC;
						break;
					case 3:
						type = STORAGE_TYPE_SD;
						break;
					case 4:
						type = STORAGE_TYPE_SD1;
						break;
					case 5:
						type = STORAGE_TYPE_SPINOR;
						break;
					case 6:
						type = STORAGE_TYPE_SPINAND;
						break;
					case 7:
						type = STORAGE_TYPE_RAM;
						break;
					case 8:
						type = STORAGE_TYPE_USB;
						break;
					case 9:
						type = STORAGE_TYPE_SATA;
						break;
					case 10:
						type = STORAGE_TYPE_PCIE;
						break;
					default:
						break;
					}
					rock_storage_switch(&ctx, type);
					type = rock_storage_read(&ctx);
					printf("%s 0.UNKNOWN\r\n", (type == STORAGE_TYPE_UNKNOWN) ? "-->" : "   ");
					printf("%s 1.FLASH\r\n", (type == STORAGE_TYPE_FLASH) ? "-->" : "   ");
					printf("%s 2.EMMC\r\n", (type == STORAGE_TYPE_EMMC) ? "-->" : "   ");
					printf("%s 3.SD\r\n", (type == STORAGE_TYPE_SD) ? "-->" : "   ");
					printf("%s 4.SD1\r\n", (type == STORAGE_TYPE_SD1) ? "-->" : "   ");
					printf("%s 5.SPINOR\r\n", (type == STORAGE_TYPE_SPINOR) ? "-->" : "   ");
					printf("%s 6.SPINAND\r\n", (type == STORAGE_TYPE_SPINAND) ? "-->" : "   ");
					printf("%s 7.RAM\r\n", (type == STORAGE_TYPE_RAM) ? "-->" : "   ");
					printf("%s 8.USB\r\n", (type == STORAGE_TYPE_USB) ? "-->" : "   ");
					printf("%s 9.SATA\r\n", (type == STORAGE_TYPE_SATA) ? "-->" : "   ");
					printf("%s10.PCIE\r\n", (type == STORAGE_TYPE_PCIE) ? "-->" : "   ");
				}
				else
					usage();
			}
			else
				printf("The loader don't support switch storage\r\n");
		}
	}
	else if(!strcmp(argv[1], "flash"))
	{
		argc -= 2;
		argv += 2;
		if(argc == 0)
		{
			struct flash_info_t info;
			if(rock_flash_detect(&ctx, &info))
			{
				printf("Flash info:\r\n");
				printf("    Manufacturer: %s (%d)\r\n", (info.manufacturer_id < ARRAY_SIZE(manufacturer))
								? manufacturer[info.manufacturer_id] : "Unknown", info.manufacturer_id);
				printf("    Capacity: %dMB\r\n", info.sector_total >> 11);
				printf("    Sector size: %d\r\n", 512);
				printf("    Sector count: %d\r\n", info.sector_total);
				printf("    Block size: %dKB\r\n", info.block_size >> 1);
				printf("    Page size: %dKB\r\n", info.page_size >> 1);
				printf("    ECC bits: %d\r\n", info.ecc_bits);
				printf("    Access time: %d\r\n", info.access_time);
				printf("    Flash CS: %s%s%s%s\r\n",
								info.chip_select & 1 ? "<0>" : "",
								info.chip_select & 2 ? "<1>" : "",
								info.chip_select & 4 ? "<2>" : "",
								info.chip_select & 8 ? "<3>" : "");
				printf("    Flash ID: %02x %02x %02x %02x %02x\r\n",
								info.id[0], info.id[1],	info.id[2],	info.id[3],	info.id[4]);
			}
			else
				printf("Failed to detect flash\r\n");
		}
		else
		{
			if(!strcmp(argv[0], "erase") && (argc == 3))
			{
				argc -= 1;
				argv += 1;
				struct flash_info_t info;
				uint32_t sec = strtoul(argv[0], NULL, 0);
				uint32_t cnt = strtoul(argv[1], NULL, 0);
				if(rock_flash_detect(&ctx, &info))
				{
					if(sec < info.sector_total)
					{
						if(cnt <= 0)
							cnt = info.sector_total - sec;
						else if(cnt > info.sector_total - sec)
							cnt = info.sector_total - sec;
						if(!rock_flash_erase_lba_progress(&ctx, sec, cnt))
							printf("Failed to erase flash\r\n");
					}
					else
						printf("The start sector is out of range\r\n");
				}
				else
					printf("Failed to detect flash\r\n");
			}
			else if(!strcmp(argv[0], "read") && (argc == 4))
			{
				argc -= 1;
				argv += 1;
				struct flash_info_t info;
				uint32_t sec = strtoul(argv[0], NULL, 0);
				uint32_t cnt = strtoul(argv[1], NULL, 0);
				if(rock_flash_detect(&ctx, &info))
				{
					if(sec < info.sector_total)
					{
						if(cnt <= 0)
							cnt = info.sector_total - sec;
						else if(cnt > info.sector_total - sec)
							cnt = info.sector_total - sec;
						if(!rock_flash_read_lba_to_file_progress(&ctx, sec, cnt, argv[2]))
							printf("Failed to read flash\r\n");
					}
					else
						printf("The start sector is out of range\r\n");
				}
				else
					printf("Failed to detect flash\r\n");
			}
			else if(!strcmp(argv[0], "write") && (argc == 3))
			{
				argc -= 1;
				argv += 1;
				struct flash_info_t info;
				uint32_t sec = strtoul(argv[0], NULL, 0);
				if(rock_flash_detect(&ctx, &info))
				{
					if(sec < info.sector_total)
					{
						if(!rock_flash_write_lba_from_file_progress(&ctx, sec, info.sector_total, argv[1]))
							printf("Failed to write flash\r\n");
					}
					else
						printf("The start sector is out of range\r\n");
				}
				else
					printf("Failed to detect flash\r\n");
			}
			else
				usage();
		}
	}
	else if(!strcmp(argv[1], "extra"))
	{
		argc -= 2;
		argv += 2;
		if(!strcmp(argv[0], "maskrom"))
		{
			argc -= 1;
			argv += 1;
			if(argc >= 2)
			{
				if(ctx.maskrom)
				{
					int rc4 = 0;
					for(int i = 0; i < argc; i++)
					{
						if(!strcmp(argv[i], "--rc4") && (argc > i + 1))
						{
							if(!strcmp(argv[i + 1], "on"))
								rc4 = 1;
							else if(!strcmp(argv[i + 1], "off"))
								rc4 = 0;
							i++;
						}
						else if(!strcmp(argv[i], "--sram") && (argc > i + 1))
						{
							rock_maskrom_upload_file(&ctx, 0x471, argv[i + 1], rc4);
							i++;
						}
						else if(!strcmp(argv[i], "--dram") && (argc > i + 1))
						{
							rock_maskrom_upload_file(&ctx, 0x472, argv[i + 1], rc4);
							i++;
						}
						else if(!strcmp(argv[i], "--delay") && (argc > i + 1))
						{
							uint32_t delay = strtoul(argv[i + 1], NULL, 0) * 1000;
							usleep(delay);
							i++;
						}
						else if(*argv[i] == '-')
						{
							usage();
						}
						else if(*argv[i] != '-' && strcmp(argv[i], "-") != 0)
						{
							usage();
						}
					}
				}
				else
					printf("Error: The chip '%s' does not in maskrom mode\r\n", ctx.chip->name);
			}
			else
				usage();
		}
		else if(!strcmp(argv[0], "maskrom-dump-arm32"))
		{
			argc -= 1;
			argv += 1;
			if(argc >= 2)
			{
				if(ctx.maskrom)
				{
					int rc4 = 0;
					uint32_t uart = 0x0;
					uint32_t addr = 0x0;
					uint32_t len = 0x0;
					for(int i = 0, idx = 0; i < argc; i++)
					{
						if(!strcmp(argv[i], "--rc4") && (argc > i + 1))
						{
							if(!strcmp(argv[i + 1], "on"))
								rc4 = 1;
							else if(!strcmp(argv[i + 1], "off"))
								rc4 = 0;
							i++;
						}
						else if(!strcmp(argv[i], "--uart") && (argc > i + 1))
						{
							uart = strtoul(argv[i + 1], NULL, 0);
							i++;
						}
						else if(*argv[i] == '-')
						{
							usage();
						}
						else if(*argv[i] != '-' && strcmp(argv[i], "-") != 0)
						{
							if(idx == 0)
								addr = strtoul(argv[i], NULL, 0);
							else if(idx == 1)
								len = strtoul(argv[i], NULL, 0);
							idx++;
						}
					}
					rock_maskrom_dump_arm32(&ctx, uart, addr, len, rc4);
				}
				else
					printf("Error: The chip '%s' does not in maskrom mode\r\n", ctx.chip->name);
			}
			else
				usage();
		}
		else if(!strcmp(argv[0], "maskrom-dump-arm64"))
		{
			argc -= 1;
			argv += 1;
			if(argc >= 2)
			{
				if(ctx.maskrom)
				{
					int rc4 = 0;
					uint32_t uart = 0x0;
					uint32_t addr = 0x0;
					uint32_t len = 0x0;
					for(int i = 0, idx = 0; i < argc; i++)
					{
						if(!strcmp(argv[i], "--rc4") && (argc > i + 1))
						{
							if(!strcmp(argv[i + 1], "on"))
								rc4 = 1;
							else if(!strcmp(argv[i + 1], "off"))
								rc4 = 0;
							i++;
						}
						else if(!strcmp(argv[i], "--uart") && (argc > i + 1))
						{
							uart = strtoul(argv[i + 1], NULL, 0);
							i++;
						}
						else if(*argv[i] == '-')
						{
							usage();
						}
						else if(*argv[i] != '-' && strcmp(argv[i], "-") != 0)
						{
							if(idx == 0)
								addr = strtoul(argv[i], NULL, 0);
							else if(idx == 1)
								len = strtoul(argv[i], NULL, 0);
							idx++;
						}
					}
					rock_maskrom_dump_arm64(&ctx, uart, addr, len, rc4);
				}
				else
					printf("Error: The chip '%s' does not in maskrom mode\r\n", ctx.chip->name);
			}
			else
				usage();
		}
		else if(!strcmp(argv[0], "maskrom-write-arm32"))
		{
			argc -= 1;
			argv += 1;
			if(argc >= 2)
			{
				if(ctx.maskrom)
				{
					int rc4 = 0;
					char * filename = NULL;
					uint32_t addr = 0x0;
					for(int i = 0, idx = 0; i < argc; i++)
					{
						if(!strcmp(argv[i], "--rc4") && (argc > i + 1))
						{
							if(!strcmp(argv[i + 1], "on"))
								rc4 = 1;
							else if(!strcmp(argv[i + 1], "off"))
								rc4 = 0;
							i++;
						}
						else if(*argv[i] == '-')
						{
							usage();
						}
						else if(*argv[i] != '-' && strcmp(argv[i], "-") != 0)
						{
							if(idx == 0)
								addr = strtoul(argv[i], NULL, 0);
							else if(idx == 1)
								filename = argv[i];
							idx++;
						}
					}
					uint64_t len;
					void * buf = file_load(filename, &len);
					if(buf)
					{
						rock_maskrom_write_arm32_progress(&ctx, addr, buf, len, rc4);
						free(buf);
					}
				}
				else
					printf("Error: The chip '%s' does not in maskrom mode\r\n", ctx.chip->name);
			}
			else
				usage();
		}
		else if(!strcmp(argv[0], "maskrom-write-arm64"))
		{
			argc -= 1;
			argv += 1;
			if(argc >= 2)
			{
				if(ctx.maskrom)
				{
					int rc4 = 0;
					char * filename = NULL;
					uint32_t addr = 0x0;
					for(int i = 0, idx = 0; i < argc; i++)
					{
						if(!strcmp(argv[i], "--rc4") && (argc > i + 1))
						{
							if(!strcmp(argv[i + 1], "on"))
								rc4 = 1;
							else if(!strcmp(argv[i + 1], "off"))
								rc4 = 0;
							i++;
						}
						else if(*argv[i] == '-')
						{
							usage();
						}
						else if(*argv[i] != '-' && strcmp(argv[i], "-") != 0)
						{
							if(idx == 0)
								addr = strtoul(argv[i], NULL, 0);
							else if(idx == 1)
								filename = argv[i];
							idx++;
						}
					}
					uint64_t len;
					void * buf = file_load(filename, &len);
					if(buf)
					{
						rock_maskrom_write_arm64_progress(&ctx, addr, buf, len, rc4);
						free(buf);
					}
				}
				else
					printf("Error: The chip '%s' does not in maskrom mode\r\n", ctx.chip->name);
			}
			else
				usage();
		}
		else if(!strcmp(argv[0], "maskrom-exec-arm32"))
		{
			argc -= 1;
			argv += 1;
			if(argc >= 2)
			{
				if(ctx.maskrom)
				{
					int rc4 = 0;
					uint32_t addr = 0x0;
					for(int i = 0; i < argc; i++)
					{
						if(!strcmp(argv[i], "--rc4") && (argc > i + 1))
						{
							if(!strcmp(argv[i + 1], "on"))
								rc4 = 1;
							else if(!strcmp(argv[i + 1], "off"))
								rc4 = 0;
							i++;
						}
						else if(*argv[i] == '-')
						{
							usage();
						}
						else if(*argv[i] != '-' && strcmp(argv[i], "-") != 0)
						{
							addr = strtoul(argv[i], NULL, 0);
						}
					}
					rock_maskrom_exec_arm32(&ctx, addr, rc4);
				}
				else
					printf("Error: The chip '%s' does not in maskrom mode\r\n", ctx.chip->name);
			}
			else
				usage();
		}
		else if(!strcmp(argv[0], "maskrom-exec-arm64"))
		{
			argc -= 1;
			argv += 1;
			if(argc >= 2)
			{
				if(ctx.maskrom)
				{
					int rc4 = 0;
					uint32_t addr = 0x0;
					for(int i = 0; i < argc; i++)
					{
						if(!strcmp(argv[i], "--rc4") && (argc > i + 1))
						{
							if(!strcmp(argv[i + 1], "on"))
								rc4 = 1;
							else if(!strcmp(argv[i + 1], "off"))
								rc4 = 0;
							i++;
						}
						else if(*argv[i] == '-')
						{
							usage();
						}
						else if(*argv[i] != '-' && strcmp(argv[i], "-") != 0)
						{
							addr = strtoul(argv[i], NULL, 0);
						}
					}
					rock_maskrom_exec_arm64(&ctx, addr, rc4);
				}
				else
					printf("Error: The chip '%s' does not in maskrom mode\r\n", ctx.chip->name);
			}
			else
				usage();
		}
		else
			usage();
	}
	else
		usage();
	if(ctx.hdl)
		libusb_close(ctx.hdl);
	libusb_exit(ctx.context);

	return 0;
}
