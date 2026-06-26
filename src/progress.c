#include <progress.h>
#ifdef _WIN32
#include <windows.h>
#endif

static double gettime(void)
{
#ifdef _WIN32
	return GetTickCount() / 1000.0;
#else
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec + (double)tv.tv_usec / 1000000.0;
#endif
}

static const char * format_eta(double remaining)
{
	static char result[6] = "";
	int seconds = remaining + 0.5;
	if(seconds >= 0 && seconds < 6000)
	{
		snprintf(result, sizeof(result), "%02d:%02d", seconds / 60, seconds % 60);
		return result;
	}
	return "--:--";
}

static const char * format_elapsed(double elapsed)
{
	static char result[16] = "";
	int seconds = elapsed + 0.5;

	if(seconds < 0)
		seconds = 0;
	if(seconds < 3600)
		snprintf(result, sizeof(result), "%02d:%02d", seconds / 60, seconds % 60);
	else
		snprintf(result, sizeof(result), "%d:%02d:%02d", seconds / 3600, (seconds / 60) % 60, seconds % 60);
	return result;
}

static char * ssize(char * buf, double size)
{
	const char * unit[] = {"B", "KB", "MB", "GB", "TB", "PB", "EB", "ZB", "YB"};
	int count = 0;

	while((size > 1024) && (count < 8))
	{
		size /= 1024;
		count++;
	}
	if(count == 0)
		sprintf(buf, "%.0f %s", size, unit[count]);
	else
		sprintf(buf, "%.2f %s", size, unit[count]);
	return buf;
}

void progress_start(struct progress_t * p, uint64_t total)
{
	if(p && (total > 0))
	{
		p->total = total;
		p->done = 0;
		p->start = gettime();
	}
}

void progress_update(struct progress_t * p, uint64_t bytes)
{
	char buf1[32], buf2[32];

	if(p)
	{
		p->done += bytes;
		double ratio = p->total > 0 ? (double)p->done / (double)p->total : 0.0;
		double elapsed = gettime() - p->start;
		double speed = elapsed > 0 ? (double)p->done / elapsed : 0;
		double eta = speed > 0 ? (p->total - p->done) / speed : 0;
		int i, pos;

		if(ratio > 1.0)
			ratio = 1.0;
		pos = 40 * ratio;
		printf("\rFlashing: [");
		for(i = 0; i < pos; i++)
			putchar('#');
		for(i = pos; i < 40; i++)
			putchar('-');
		if(p->done < p->total)
			printf("] %3.0f%%  %s/s  ETA %s        ", ratio * 100, ssize(buf1, speed), format_eta(eta));
		else
			printf("] 100%%  %s written  %s/s  elapsed %s        ",
				ssize(buf1, p->done), ssize(buf2, speed), format_elapsed(elapsed));
		fflush(stdout);
	}
}

void progress_stop(struct progress_t * p)
{
	if(p)
		printf("\r\n");
}
