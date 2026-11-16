// SPDX-License-Identifier: GPL-2.0-only
/* Read-only timing probe: no affinity, priority, timer-slack or sysctl changes. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/utsname.h>
#include <time.h>

static int compare(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return (x > y) - (x < y);
}

static unsigned int number(const char *text)
{
	char *end;
	unsigned long value;

	errno = 0;
	value = strtoul(text, &end, 10);
	if (errno || *end || end == text || value > 100000) {
		fprintf(stderr, "invalid numeric argument\n");
		exit(2);
	}
	return value;
}

int main(int argc, char **argv)
{
	unsigned int usec = 50, count = 250, i, interrupted = 0, over_1ms = 0;
	struct timespec start, stop, delay, remaining, resolution;
	struct utsname machine;
	uint64_t *samples, sum = 0;
	int rc;

	if (argc > 3) {
		fprintf(stderr, "usage: %s [microseconds [samples]]\n", argv[0]);
		return 2;
	}
	if (argc >= 2)
		usec = number(argv[1]);
	if (argc == 3)
		count = number(argv[2]);
	if (!usec || count < 10 || count > 10000 || (uint64_t)usec * count > 1000000) {
		fprintf(stderr, "require positive delay, 10..10000 samples, total requested sleep <=1s\n");
		return 2;
	}
	samples = calloc(count, sizeof(*samples));
	if (!samples || uname(&machine) || clock_getres(CLOCK_MONOTONIC, &resolution))
		return 1;
	for (i = 0; i < count; i++) {
		delay.tv_sec = 0;
		delay.tv_nsec = (long)usec * 1000;
		if (clock_gettime(CLOCK_MONOTONIC_RAW, &start))
			return 1;
		while ((rc = clock_nanosleep(CLOCK_MONOTONIC, 0, &delay, &remaining)) == EINTR) {
			interrupted++;
			delay = remaining;
		}
		if (rc || clock_gettime(CLOCK_MONOTONIC_RAW, &stop))
			return 1;
		samples[i] = (uint64_t)((int64_t)(stop.tv_sec - start.tv_sec) * 1000000000 +
				       stop.tv_nsec - start.tv_nsec);
		sum += samples[i];
		if (samples[i] >= 1000000)
			over_1ms++;
	}
	qsort(samples, count, sizeof(*samples), compare);
	printf("{\"kernel\":\"%s\",\"requested_us\":%u,\"samples\":%u,"
	       "\"elapsed_clock\":\"CLOCK_MONOTONIC_RAW\",\"sleep_clock\":\"CLOCK_MONOTONIC\","
	       "\"monotonic_resolution_ns\":%" PRIu64 ",\"minimum_us\":%.3f,"
	       "\"median_us\":%.3f,\"p95_us\":%.3f,\"p99_us\":%.3f,\"maximum_us\":%.3f,"
	       "\"mean_us\":%.3f,\"at_least_1ms\":%u,\"eintr_restarts\":%u,\"system_settings_changed\":false}\n",
	       machine.release, usec, count,
	       (uint64_t)resolution.tv_sec * 1000000000 + resolution.tv_nsec,
	       samples[0] / 1000.0, samples[count / 2] / 1000.0,
	       samples[(count - 1) * 95 / 100] / 1000.0,
	       samples[(count - 1) * 99 / 100] / 1000.0,
	       samples[count - 1] / 1000.0, (double)sum / count / 1000.0,
	       over_1ms, interrupted);
	free(samples);
	return 0;
}
