#ifdef __linux__
#define _GNU_SOURCE
#endif
#include "ipc.h"
#include "report.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define RESPONSE_BYTES (64U * 1024U)
#define CLIENT_TIMEOUT (2 * WHY_SECOND)
struct WhyServer {
	int listener, client, lock;
	char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
	char request[32], response[RESPONSE_BYTES];
	size_t received, length, sent;
	uint64_t expires;
	bool replying, bound;
};

static bool private_dir(const char *path, bool create) {
	struct stat st;
	if (create && mkdir(path, 0700) < 0 && errno != EEXIST)
		return false;
	if (lstat(path, &st) < 0)
		return false;
	if (!S_ISDIR(st.st_mode) || st.st_uid != geteuid() ||
		(st.st_mode & 077) != 0) {
		errno = EACCES;
		return false;
	}
	return true;
}
static bool socket_path(char *path, size_t size, bool create) {
	const char *dir = getenv("WHY_RUNTIME_DIR");
	char child[sizeof(((struct sockaddr_un *)0)->sun_path)];
	if (!dir || !*dir) {
		const char *base = getenv("XDG_RUNTIME_DIR");
		if (!base || base[0] != '/') {
			errno = ENOENT;
			return false;
		}
		if (!private_dir(base, false))
			return false;
		int n = snprintf(child, sizeof child, "%s/why-cli", base);
		if (n < 0 || (size_t)n >= sizeof child) {
			errno = ENAMETOOLONG;
			return false;
		}
		dir = child;
	}
	if (dir[0] != '/') {
		errno = EINVAL;
		return false;
	}
	if (!private_dir(dir, create))
		return false;
	int n = snprintf(path, size, "%s/recorder.sock", dir);
	if (n < 0 || (size_t)n >= size) {
		errno = ENAMETOOLONG;
		return false;
	}
	return true;
}
static bool flags(int fd) {
	return fcntl(fd, F_SETFD, FD_CLOEXEC) >= 0 &&
		   fcntl(fd, F_SETFL, O_NONBLOCK) >= 0;
}
static bool same_user(int fd) {
#ifdef __linux__
	struct ucred peer;
	socklen_t size = sizeof peer;
	return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) == 0 &&
		   peer.uid == geteuid();
#elif defined(__APPLE__)
	uid_t uid;
	gid_t gid;
	return getpeereid(fd, &uid, &gid) == 0 && uid == geteuid();
#else
	(void)fd;
	return false;
#endif
}
static struct sockaddr_un address(const char *path) {
	struct sockaddr_un addr = {0};
	addr.sun_family = AF_UNIX;
	memcpy(addr.sun_path, path, strlen(path) + 1);
	return addr;
}
static void client_close(WhyServer *s) {
	if (s->client >= 0)
		close(s->client);
	s->client = -1;
	s->received = s->length = s->sent = 0;
	s->replying = false;
}
void why_server_close(WhyServer *s) {
	if (!s)
		return;
	client_close(s);
	if (s->listener >= 0)
		close(s->listener);
	/* The lock stays held through unlink; the lock file is intentionally kept.
	 */
	if (s->bound)
		unlink(s->path);
	if (s->lock >= 0)
		close(s->lock);
	free(s);
}
WhyServer *why_server_open(void) {
	WhyServer *s = calloc(1, sizeof *s);
	if (!s)
		return NULL;
	s->listener = s->client = s->lock = -1;
	if (!socket_path(s->path, sizeof s->path, true))
		goto fail;
	char lock_path[sizeof s->path + 8];
	snprintf(lock_path, sizeof lock_path, "%s.lock", s->path);
	s->lock = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (s->lock < 0)
		goto fail;
	struct stat st;
	if (fstat(s->lock, &st) < 0)
		goto fail;
	if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 077) ||
		st.st_nlink != 1) {
		errno = EACCES;
		goto fail;
	}
	struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
	if (fcntl(s->lock, F_SETLK, &lock) < 0)
		goto fail;
	if (lstat(s->path, &st) == 0) {
		if (!S_ISSOCK(st.st_mode) || st.st_uid != geteuid()) {
			errno = EACCES;
			goto fail;
		}
		if (unlink(s->path) < 0)
			goto fail;
	} else if (errno != ENOENT)
		goto fail;
	s->listener = socket(AF_UNIX, SOCK_STREAM, 0);
	if (s->listener < 0 || !flags(s->listener))
		goto fail;
	struct sockaddr_un addr = address(s->path);
	if (bind(s->listener, (struct sockaddr *)&addr, sizeof addr) < 0)
		goto fail;
	s->bound = true;
	if (chmod(s->path, 0600) < 0 || listen(s->listener, 4) < 0)
		goto fail;
	return s;
fail:;
	int saved = errno;
	why_server_close(s);
	errno = saved;
	return NULL;
}
static void reply(WhyServer *s, const WhyHistory *history, long hz) {
	unsigned long seconds = 0;
	char *end = NULL;
	char canonical[32];
	s->request[s->received] = '\0';
	errno = 0;
	if (s->received > 6 && !memcmp(s->request, "WHY/1 ", 6))
		seconds = strtoul(s->request + 6, &end, 10);
	if (!errno && end && seconds >= 1 && seconds <= 300 &&
		!memchr(s->request, '\0', s->received)) {
		snprintf(canonical, sizeof canonical, "WHY/1 %lu\n", seconds);
		if (!strcmp(s->request, canonical)) {
			FILE *out = fmemopen(s->response, sizeof s->response, "w");
			if (out) {
				fputs("WHY/1 OK\n", out);
				why_report_query(out, history, hz, (unsigned)seconds,
								 why_now_ns());
				fputs("WHY/1 END\n", out);
				bool ok = fflush(out) == 0 && !ferror(out);
				long length = ftell(out);
				if (fclose(out) != 0)
					ok = false;
				if (ok && length > 0 && (size_t)length < sizeof s->response) {
					s->length = (size_t)length;
					s->replying = true;
					return;
				}
			}
			strcpy(s->response, "WHY/1 ERROR\nQuery report exceeded its bound "
								"or could not be generated.\n");
			goto done;
		}
	}
	strcpy(s->response, "WHY/1 ERROR\nUnsupported request; expected WHY/1 and "
						"1..300 seconds.\n");
done:
	s->length = strlen(s->response);
	s->replying = true;
}
bool why_server_wait(WhyServer *s, const WhyHistory *history, long hz,
					 uint64_t deadline, const volatile sig_atomic_t *stopped) {
	while (!*stopped) {
		uint64_t now = why_now_ns();
		if (now >= deadline)
			return true;
		if (s->client >= 0 && now >= s->expires)
			client_close(s);
		uint64_t wake =
			s->client >= 0 && s->expires < deadline ? s->expires : deadline;
		int timeout = (int)((wake - now + 999999) / 1000000);
		struct pollfd p = {.fd = s->client >= 0 ? s->client : s->listener,
						   .events = s->client >= 0 && s->replying ? POLLOUT
																   : POLLIN};
		int ready = poll(&p, 1, timeout);
		if (ready < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		if (!ready)
			continue;
		if (s->client < 0) {
			int fd = accept(s->listener, NULL, NULL);
			if (fd < 0) {
				if (errno == EAGAIN || errno == EINTR)
					continue;
				return false;
			}
			if (!flags(fd) || !same_user(fd)) {
				close(fd);
				continue;
			}
			s->client = fd;
			s->expires = why_now_ns() + CLIENT_TIMEOUT;
		} else if (p.revents & (POLLERR | POLLNVAL))
			client_close(s);
		else if (s->replying) {
			ssize_t n =
				send(s->client, s->response + s->sent, s->length - s->sent, 0);
			if (n > 0)
				s->sent += (size_t)n;
			else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
				client_close(s);
				continue;
			}
			if (s->sent == s->length)
				client_close(s);
		} else {
			ssize_t n = recv(s->client, s->request + s->received,
							 sizeof s->request - 1 - s->received, 0);
			if (n > 0) {
				s->received += (size_t)n;
				if (memchr(s->request, '\n', s->received))
					reply(s, history, hz);
				else if (s->received == sizeof s->request - 1)
					client_close(s);
			} else if (n == 0 || (errno != EAGAIN && errno != EINTR))
				client_close(s);
		}
	}
	return true;
}

int why_client_query(unsigned seconds) {
	char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
	int fd = -1;
	if (!socket_path(path, sizeof path, false))
		goto fail;
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0 || !flags(fd))
		goto fail;
	struct sockaddr_un addr = address(path);
	if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
		if (errno != EINPROGRESS)
			goto fail;
		struct pollfd p = {.fd = fd, .events = POLLOUT};
		if (poll(&p, 1, 5000) <= 0) {
			errno = ETIMEDOUT;
			goto fail;
		}
		int error = 0;
		socklen_t size = sizeof error;
		if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0)
			goto fail;
		if (error) {
			errno = error;
			goto fail;
		}
	}
	if (!same_user(fd)) {
		errno = EACCES;
		goto fail;
	}
	char request[32];
	int length = snprintf(request, sizeof request, "WHY/1 %u\n", seconds);
	size_t sent = 0, used = 0;
	char response[RESPONSE_BYTES];
	uint64_t deadline = why_now_ns() + 5 * WHY_SECOND;
	for (;;) {
		uint64_t now = why_now_ns();
		if (now >= deadline) {
			errno = ETIMEDOUT;
			goto fail;
		}
		struct pollfd p = {.fd = fd,
						   .events = sent < (size_t)length ? POLLOUT : POLLIN};
		int timeout = (int)((deadline - now + 999999) / 1000000);
		int ready = poll(&p, 1, timeout);
		if (ready < 0 && errno == EINTR)
			continue;
		if (ready <= 0) {
			if (!ready)
				errno = ETIMEDOUT;
			goto fail;
		}
		if (sent < (size_t)length) {
			ssize_t n = send(fd, request + sent, (size_t)length - sent, 0);
			if (n > 0)
				sent += (size_t)n;
			else if (n < 0 && errno != EAGAIN && errno != EINTR)
				goto fail;
		} else {
			ssize_t n =
				recv(fd, response + used, sizeof response - 1 - used, 0);
			if (n > 0) {
				used += (size_t)n;
				if (used == sizeof response - 1) {
					errno = EMSGSIZE;
					goto fail;
				}
			} else if (!n)
				break;
			else if (errno != EAGAIN && errno != EINTR)
				goto fail;
		}
	}
	close(fd);
	response[used] = '\0';
	if (used < 19 || memcmp(response, "WHY/1 OK\n", 9) ||
		memcmp(response + used - 10, "WHY/1 END\n", 10)) {
		fprintf(stderr,
				"Recorder returned an incomplete or unsupported response.\n");
		return 1;
	}
	if (fwrite(response + 9, 1, used - 19, stdout) != used - 19 ||
		fflush(stdout) != 0)
		return 1;
	return 0;
fail:
	fprintf(stderr,
			"Cannot query recorder: %s. Start 'why watch' with the same "
			"private runtime directory.\n",
			strerror(errno));
	if (fd >= 0)
		close(fd);
	return 1;
}
