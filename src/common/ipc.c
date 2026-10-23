#include "common/ipc.h"

#include "common/log.h"
#include "common/time.h"
#include "common/util.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

const char *pss_role_name(uint32_t role)
{
	switch (role) {
	case PSS_ROLE_PIR: return "pird";
	case PSS_ROLE_CAM: return "camd";
	case PSS_ROLE_MIC: return "micd";
	default: return "unknown";
	}
}

void pss_msg_init(struct pss_msg *m, uint16_t type)
{
	memset(m, 0, sizeof *m);
	m->type = type;
	m->mono_ns = pss_mono_ns();
}

int ipc_sock_path(char *buf, size_t n)
{
	const char *dir = pss_run_dir();
	int r = snprintf(buf, n, "%s/hub.sock", dir);

	if (r < 0 || (size_t)r >= n || (size_t)r >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
		pss_log("ipc: socket path under %s is too long", dir);
		return -1;
	}
	return 0;
}

static int fill_addr(struct sockaddr_un *a, const char *path)
{
	size_t len = strlen(path);

	memset(a, 0, sizeof *a);
	a->sun_family = AF_UNIX;
	if (len >= sizeof a->sun_path) {
		errno = ENAMETOOLONG;
		return -1;
	}
	memcpy(a->sun_path, path, len + 1);
	return 0;
}

int ipc_listen(const char *path)
{
	struct sockaddr_un a;
	int fd;

	if (fill_addr(&a, path) != 0) {
		pss_log("ipc: %s: %s", path, strerror(errno));
		return -1;
	}
	if (unlink(path) != 0 && errno != ENOENT) {
		pss_log("ipc: unlink %s: %s", path, strerror(errno));
		return -1;
	}
	fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0) {
		pss_log("ipc: socket: %s", strerror(errno));
		return -1;
	}
	if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0) {
		pss_log("ipc: bind %s: %s", path, strerror(errno));
		goto fail;
	}
	if (listen(fd, 8) != 0) {
		pss_log("ipc: listen %s: %s", path, strerror(errno));
		goto fail;
	}
	return fd;
fail:
	close(fd);
	return -1;
}

int ipc_connect(const char *path)
{
	struct sockaddr_un a;
	int fd, saved;

	if (fill_addr(&a, path) != 0)
		return -1;
	fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0)
		return -1;
	if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
		saved = errno;
		close(fd);
		errno = saved;
		return -1;
	}
	return fd;
}

int ipc_send(int fd, struct pss_msg *m)
{
	ssize_t n;

	m->ver = PSS_IPC_VER;
	do
		n = send(fd, m, sizeof *m, MSG_NOSIGNAL);
	while (n < 0 && errno == EINTR);
	if (n < 0)
		return -1;
	if ((size_t)n != sizeof *m) {
		errno = EMSGSIZE;
		return -1;
	}
	return 0;
}

int ipc_recv(int fd, struct pss_msg *m)
{
	ssize_t n;

	/* MSG_TRUNC: report the real packet length so oversize packets are caught. */
	do
		n = recv(fd, m, sizeof *m, MSG_TRUNC);
	while (n < 0 && errno == EINTR);
	if (n < 0)
		return -1;
	if (n == 0)
		return 0;
	if ((size_t)n != sizeof *m) {
		errno = EBADMSG;
		return -1;
	}
	if (m->ver != PSS_IPC_VER) {
		errno = EPROTO;
		return -1;
	}
	return 1;
}
