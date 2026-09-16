#include "ipc.h"
#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(c)                                                               \
	do {                                                                       \
		if (!(c)) {                                                            \
			fprintf(stderr, "%s:%d: %s (%s)\n", __FILE__, __LINE__, #c,        \
					strerror(errno));                                          \
			exit(1);                                                           \
		}                                                                      \
	} while (0)
static volatile sig_atomic_t stopped;
static void stop(int sig) {
	(void)sig;
	stopped = 1;
}
static char socket_name[108], runtime_dir[1200];
static pid_t parent_pid, active_child;
static void cleanup(void) {
	if (getpid() != parent_pid)
		return;
	if (active_child > 0) {
		kill(active_child, SIGTERM);
		waitpid(active_child, NULL, 0);
	}
	if (*socket_name) {
		unlink(socket_name);
		char lock[1200];
		snprintf(lock, sizeof lock, "%s.lock", socket_name);
		unlink(lock);
	}
	if (*runtime_dir)
		rmdir(runtime_dir);
}
static int connect_client(void) {
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	CHECK(fd >= 0);
	struct sockaddr_un address = {0};
	address.sun_family = AF_UNIX;
	CHECK(strlen(socket_name) < sizeof address.sun_path);
#ifdef __linux__
	snprintf(address.sun_path + 1, sizeof address.sun_path - 1,
			 "why-cli.%lu.%s", (unsigned long)geteuid(),
			 getenv("WHY_SOCKET_NAME"));
	socklen_t length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 +
								   strlen(address.sun_path + 1));
#else
	strcpy(address.sun_path, socket_name);
	socklen_t length = sizeof address;
#endif
	CHECK(connect(fd, (struct sockaddr *)&address, length) == 0);
	struct timeval timeout = {.tv_sec = 5};
	CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout) ==
		  0);
	return fd;
}
static void exchange(const char *request, size_t bytes, const char *expected) {
	int fd = connect_client();
	/* Split a request across writes to exercise stream framing. */
	CHECK(write(fd, request, 1) == 1);
	CHECK(write(fd, request + 1, bytes - 1) == (ssize_t)(bytes - 1));
	char response[4096];
	size_t used = 0;
	for (;;) {
		ssize_t n = read(fd, response + used, sizeof response - 1 - used);
		CHECK(n >= 0);
		if (!n)
			break;
		used += (size_t)n;
		CHECK(used < sizeof response - 1);
	}
	response[used] = 0;
	CHECK(strstr(response, expected));
	close(fd);
}
static pid_t start_server(bool crash) {
	int ready[2];
	CHECK(pipe(ready) == 0);
	pid_t child = fork();
	CHECK(child >= 0);
	if (!child) {
		close(ready[0]);
		struct sigaction action = {0};
		action.sa_handler = stop;
		sigemptyset(&action.sa_mask);
		CHECK(sigaction(SIGTERM, &action, NULL) == 0);
		WhyServer *server = why_server_open();
		CHECK(server);
		WhyHistory *history = why_history_create(300 * WHY_SECOND, 1000000, 4);
		CHECK(history);
		CHECK(write(ready[1], "r", 1) == 1);
		close(ready[1]);
		if (crash)
			_exit(0); /* Deliberately leave a stale socket. */
		CHECK(why_server_wait(server, history, 100,
							  why_now_ns() + 15 * WHY_SECOND, &stopped));
		why_server_close(server);
		why_history_destroy(history);
		exit(0);
	}
	active_child = child;
	close(ready[1]);
	char byte;
	CHECK(read(ready[0], &byte, 1) == 1);
	close(ready[0]);
	return child;
}
static void finish(pid_t child, bool signal_child) {
	if (signal_child)
		CHECK(kill(child, SIGTERM) == 0);
	int status;
	CHECK(waitpid(child, &status, 0) == child);
	active_child = 0;
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
int main(void) {
	parent_pid = getpid();
	CHECK(atexit(cleanup) == 0);
	signal(SIGPIPE, SIG_IGN);
	char template[] = "why-ipc-XXXXXX";
	CHECK(mkdtemp(template));
	char cwd[1024], directory[1200];
	CHECK(getcwd(cwd, sizeof cwd));
	CHECK(snprintf(directory, sizeof directory, "%s/%s", cwd, template) > 0);
	strcpy(runtime_dir, directory);
	CHECK(setenv("WHY_RUNTIME_DIR", directory, 1) == 0);
	CHECK(strlen(directory) + strlen("/recorder.sock") < sizeof socket_name);
	strcpy(socket_name, directory);
	strcat(socket_name, "/recorder.sock");
#ifdef __linux__
	CHECK(setenv("WHY_SOCKET_NAME", template, 1) == 0);
	CHECK(setenv("WHY_SOCKET_NAME", "invalid/name", 1) == 0);
	CHECK(!why_server_open() && errno == EINVAL);
	CHECK(setenv("WHY_SOCKET_NAME", template, 1) == 0);
#else
	CHECK(chmod(directory, 0755) == 0);
	CHECK(!why_server_open() && errno == EACCES);
	CHECK(chmod(directory, 0700) == 0);
	/* Never replace a regular file in place of the socket. */
	FILE *file = fopen(socket_name, "w");
	CHECK(file);
	CHECK(fclose(file) == 0);
	CHECK(!why_server_open() && errno == EACCES);
	CHECK(unlink(socket_name) == 0);
#endif
	pid_t child = start_server(true);
	finish(child, false);
	struct stat st;
#ifdef __linux__
	CHECK(lstat(socket_name, &st) < 0 && errno == ENOENT);
#else
	CHECK(lstat(socket_name, &st) == 0 && S_ISSOCK(st.st_mode));
#endif
	child = start_server(false);
	CHECK(
		!why_server_open()); /* Duplicate process cannot steal the endpoint. */
	const char request[] = "WHY/1 60\n";
	exchange(request, sizeof request - 1, "WHY/1 END\n");
	const char malformed[] = "WHY/1 60\n\0ignored";
	exchange(malformed, sizeof malformed - 1, "WHY/1 ERROR\n");
	exchange("WHY/2 60\n", 9, "WHY/1 ERROR\n");
	int slow = connect_client();
	CHECK(write(slow, "W", 1) == 1);
	char byte;
	CHECK(read(slow, &byte, 1) == 0);
	close(slow); /* Bounded idle timeout. */
	int gone = connect_client();
	close(gone);
	exchange(request, sizeof request - 1, "Recorder is warming up");
	CHECK(why_client_query(60) == 0);
#ifdef __linux__
	CHECK(kill(child, SIGKILL) == 0);
	int killed_status;
	CHECK(waitpid(child, &killed_status, 0) == child);
	active_child = 0;
	CHECK(WIFSIGNALED(killed_status) && WTERMSIG(killed_status) == SIGKILL);
	child =
		start_server(false); /* Kernel releases the endpoint after SIGKILL. */
	exchange(request, sizeof request - 1, "WHY/1 END\n");
#endif
	finish(child, true);
	CHECK(lstat(socket_name, &st) < 0 && errno == ENOENT);
	CHECK(why_client_query(60) == 1);
	char lock[1200];
	snprintf(lock, sizeof lock, "%s.lock", socket_name);
#ifdef __linux__
	CHECK(lstat(lock, &st) < 0 && errno == ENOENT);
	/* rmdir proves no unexpected runtime entries were created. */
#else
	CHECK(unlink(lock) == 0);
#endif
	CHECK(rmdir(directory) == 0);
	puts("IPC privacy, framing, duplicate recorder, stale socket, disconnect "
		 "and timeout tests passed.");
	return 0;
}
