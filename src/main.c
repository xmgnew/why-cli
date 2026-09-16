#include "ipc.h"
#include "report.h"

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopped;
static void stop(int signal_number) {
	(void)signal_number;
	stopped = 1;
}

static bool query_seconds(const char *text, unsigned *seconds) {
	char *end;
	errno = 0;
	unsigned long n = strtoul(text, &end, 10);
	if (text[0] < '1' || text[0] > '9' || errno || n > 300 || strcmp(end, "s"))
		return false;
	*seconds = (unsigned)n;
	return true;
}

int main(int argc, char **argv) {
	/* A disappearing query peer must not terminate the recorder. */
	struct sigaction ignored = {0};
	ignored.sa_handler = SIG_IGN;
	sigemptyset(&ignored.sa_mask);
	sigaction(SIGPIPE, &ignored, NULL);
	if (argc == 2 && strcmp(argv[1], "--help") == 0) {
		puts("Usage: why watch\n"
			 "       why [cpu] [Ns]\n"
			 "       why sample [--count N] [--details] [--history]\n\n"
			 "watch: foreground Linux recorder; Ctrl-C stops and releases "
			 "history.\n"
			 "Queries: last 60 seconds by default; Ns accepts 1s..300s.\n"
			 "Runtime: private $XDG_RUNTIME_DIR/why-cli or $WHY_RUNTIME_DIR.\n"
			 "sample: diagnostic 1-second sampling, default count 2.\n"
			 "--details displays arguments, cwd and executable; --history adds "
			 "lifecycle output.\n"
			 "History targets 300 seconds; 48 MiB history + 16 MiB analysis "
			 "budget.\n"
			 "CPU spikes and rankings are estimates, not proof of causality.");
		return 0;
	}
	bool watch = argc == 2 && !strcmp(argv[1], "watch");
	bool sample = argc >= 2 && !strcmp(argv[1], "sample");
	if (!watch && !sample) {
		unsigned seconds = 60;
		bool valid = argc == 1 || (argc == 2 && !strcmp(argv[1], "cpu"));
		if (argc == 2 && strcmp(argv[1], "cpu"))
			valid = query_seconds(argv[1], &seconds);
		if (argc == 3 && !strcmp(argv[1], "cpu"))
			valid = query_seconds(argv[2], &seconds);
		if (!valid) {
			fprintf(stderr, "Invalid command. Use 'why --help'.\n");
			return 2;
		}
		return why_client_query(seconds);
	}
	size_t count = watch ? SIZE_MAX : 2;
	bool details = false, timeline = false, have_count = false;
	for (int i = 2; i < argc; ++i) {
		if (!strcmp(argv[i], "--details") && !details) {
			details = true;
		} else if (!strcmp(argv[i], "--history") && !timeline) {
			timeline = true;
		} else if (!strcmp(argv[i], "--count") && !have_count && i + 1 < argc) {
			const char *value = argv[++i];
			char *end;
			errno = 0;
			unsigned long n = strtoul(value, &end, 10);
			if (value[0] < '1' || value[0] > '9' || errno || *end ||
				n > 1000000) {
				fprintf(stderr, "--count must be between 1 and 1000000.\n");
				return 2;
			}
			count = (size_t)n;
			have_count = true;
		} else {
			fprintf(stderr, "Unknown, repeated or incomplete option: %s\n",
					argv[i]);
			return 2;
		}
	}
#ifndef __linux__
	fprintf(stderr, "Live collection requires Linux. Core tests can run on "
					"this platform.\n");
	return 1;
#endif
	long hz = sysconf(_SC_CLK_TCK);
	if (hz <= 0) {
		perror("sysconf(_SC_CLK_TCK)");
		return 1;
	}
	struct sigaction action = {0};
	action.sa_handler = stop;
	sigemptyset(&action.sa_mask);
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);
	WhyFrame frames[2] = {0};
	if (!why_frame_init(&frames[0]) || !why_frame_init(&frames[1])) {
		fprintf(stderr, "Cannot allocate bounded sample buffers.\n");
		why_frame_destroy(&frames[0]);
		why_frame_destroy(&frames[1]);
		return 1;
	}
	WhyHistory *history = why_history_create(
		WHY_HISTORY_SECONDS * WHY_SECOND,
		WHY_HISTORY_BYTES - WHY_ATTRIBUTION_BYTES, WHY_MAX_PROCESSES);
	if (!history) {
		perror("Cannot allocate history");
		why_frame_destroy(&frames[0]);
		why_frame_destroy(&frames[1]);
		return 1;
	}
	WhyServer *server = watch ? why_server_open() : NULL;
	if (watch && !server) {
		fprintf(stderr,
				"Cannot start recorder: %s. Check for an existing watch and "
				"use a private runtime directory.\n",
				strerror(errno));
		why_history_destroy(history);
		why_frame_destroy(&frames[0]);
		why_frame_destroy(&frames[1]);
		return 1;
	}
	if (watch) {
		puts("Recording process/CPU activity. Query from another terminal with "
			 "'why' or 'why cpu 60s'. Ctrl-C stops.");
		fflush(stdout);
	}
	uint32_t cursor = 0;
	uint64_t deadline = why_now_ns();
	int status = 0;
	for (size_t i = 0; i < count && !stopped; ++i) {
		WhyFrame *current = &frames[i % 2];
		const WhyFrame *previous = i ? &frames[(i - 1) % 2] : NULL;
		if (!why_collect("/proc", &cursor, previous, current)) {
			perror("Cannot collect /proc");
			status = 1;
			break;
		}
		if (!why_history_append(history, current, hz)) {
			perror("Cannot retain sample");
			status = 1;
			break;
		}
		if (!watch)
			why_report_sample(stdout, previous, current, hz, i + 1, details);
		if (i + 1 == count)
			break;
		deadline += WHY_SECOND;
		uint64_t now = why_now_ns();
		if (now >= deadline)
			deadline += ((now - deadline) / WHY_SECOND + 1) * WHY_SECOND;
		if (watch) {
			if (!why_server_wait(server, history, hz, deadline, &stopped)) {
				perror("Recorder socket");
				status = 1;
				break;
			}
			continue;
		}
		while (!stopped && (now = why_now_ns()) < deadline) {
			uint64_t remaining = deadline - now;
			struct timespec delay = {.tv_sec = (time_t)(remaining / WHY_SECOND),
									 .tv_nsec = (long)(remaining % WHY_SECOND)};
			if (nanosleep(&delay, NULL) != 0 && errno != EINTR) {
				perror("nanosleep");
				status = 1;
				stopped = 1;
			}
		}
	}
	why_server_close(server);
	if (!watch) {
		why_report_history(stdout, history, timeline);
		why_report_spikes(stdout, history, hz);
	} else
		puts("Recorder stopped; in-memory history released.");
	why_history_destroy(history);
	why_frame_destroy(&frames[0]);
	why_frame_destroy(&frames[1]);
	return status;
}
