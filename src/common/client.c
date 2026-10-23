#include "common/client.h"

#include "common/log.h"
#include "common/time.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

#define BACKOFF_MIN_NS (100 * PSS_NS_PER_MS)
#define BACKOFF_MAX_NS (5 * PSS_NS_PER_S)

int client_init(struct pss_client *c, uint32_t role)
{
	memset(c, 0, sizeof *c);
	c->fd = -1;
	c->role = role;
	c->backoff_ns = BACKOFF_MIN_NS;
	return ipc_sock_path(c->path, sizeof c->path);
}

int client_poll_connect(struct pss_client *c, int64_t now_ns)
{
	struct pss_msg m;
	int fd;

	if (c->fd >= 0 || now_ns < c->next_try_ns)
		return 0;

	fd = ipc_connect(c->path);
	if (fd < 0) {
		if (!c->warned) {
			pss_log("hub %s unreachable (%s); retrying", c->path, strerror(errno));
			c->warned = 1;
		}
		c->next_try_ns = now_ns + c->backoff_ns;
		c->backoff_ns *= 2;
		if (c->backoff_ns > BACKOFF_MAX_NS)
			c->backoff_ns = BACKOFF_MAX_NS;
		return 0;
	}

	c->fd = fd;
	pss_msg_init(&m, PSS_MSG_HELLO);
	m.u.hello.role = c->role;
	m.u.hello.pid = (uint32_t)getpid();
	if (client_send(c, &m) != 0)
		return 0;
	pss_log("connected to hub");
	c->warned = 0;
	c->backoff_ns = BACKOFF_MIN_NS;
	return 1;
}

int client_send(struct pss_client *c, struct pss_msg *m)
{
	if (c->fd < 0) {
		c->dropped++;
		return -1;
	}
	m->seq = c->seq++;
	if (ipc_send(c->fd, m) == 0)
		return 0;
	c->dropped++;
	if (errno != EAGAIN)
		client_disconnect(c, pss_mono_ns(), strerror(errno));
	return -1;
}

void client_disconnect(struct pss_client *c, int64_t now_ns, const char *why)
{
	if (c->fd < 0)
		return;
	pss_log("hub connection lost (%s); dropping messages until reconnect", why);
	if (close(c->fd) != 0)
		pss_log("close hub fd: %s", strerror(errno));
	c->fd = -1;
	c->next_try_ns = now_ns + c->backoff_ns;
}

void client_close(struct pss_client *c)
{
	if (c->fd >= 0 && close(c->fd) != 0)
		pss_log("close hub fd: %s", strerror(errno));
	c->fd = -1;
}
