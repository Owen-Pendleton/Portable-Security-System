#include "common/suphb.h"

#include "common/ipc.h"
#include "common/log.h"
#include "common/util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int sup_fd = -1;
static int inited;

void suphb_init(void)
{
	const char *s;
	uint32_t fd;

	if (inited)
		return;
	inited = 1;
	s = getenv("PSS_SUP_FD");
	if (s == NULL)
		return;
	if (pss_parse_u32(s, 3, 1023, &fd) != 0 || fcntl((int)fd, F_SETFD, FD_CLOEXEC) != 0) {
		pss_log("PSS_SUP_FD=%s unusable (%s); no supervisor heartbeat", s, strerror(errno));
	} else {
		sup_fd = (int)fd;
	}
	if (unsetenv("PSS_SUP_FD") != 0)
		pss_log("unsetenv PSS_SUP_FD: %s", strerror(errno));
}

void suphb_beat(uint64_t count, uint64_t dropped)
{
	struct pss_msg m;

	if (sup_fd < 0)
		return;
	pss_msg_init(&m, PSS_MSG_HEARTBEAT);
	m.u.hb.count = count;
	m.u.hb.dropped = dropped;
	if (ipc_send(sup_fd, &m) == 0 || errno == EAGAIN)
		return;
	/* The supervisor is gone (PR_SET_PDEATHSIG will stop us shortly). */
	pss_log("supervisor heartbeat: %s; stopping heartbeats", strerror(errno));
	close(sup_fd);
	sup_fd = -1;
}
