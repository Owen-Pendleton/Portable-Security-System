/* AF_UNIX SOCK_SEQPACKET framing between daemons and the hub. */
#include "t.h"

#include "common/ipc.h"

#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

static void test_roundtrip_and_eof(void)
{
	int sv[2];
	struct pss_msg out, in;

	CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0);
	pss_msg_init(&out, PSS_MSG_PIR_EDGE);
	out.u.pir.rising = 1;
	CHECK(ipc_send(sv[0], &out) == 0);
	CHECK(ipc_recv(sv[1], &in) == 1);
	CHECK(in.type == PSS_MSG_PIR_EDGE && in.u.pir.rising == 1 && in.mono_ns == out.mono_ns);
	close(sv[0]);
	CHECK(ipc_recv(sv[1], &in) == 0);
	close(sv[1]);
}

static void test_rejects_bad_size_and_version(void)
{
	int sv[2];
	struct pss_msg m;
	char junk[3] = { 1, 2, 3 };

	CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0);
	CHECK(send(sv[0], junk, sizeof junk, 0) == (ssize_t)sizeof junk);
	errno = 0;
	CHECK(ipc_recv(sv[1], &m) == -1 && errno == EBADMSG);

	pss_msg_init(&m, PSS_MSG_HEARTBEAT);
	m.ver = PSS_IPC_VER + 1;
	CHECK(send(sv[0], &m, sizeof m, 0) == (ssize_t)sizeof m); /* raw send keeps the bad ver */
	errno = 0;
	CHECK(ipc_recv(sv[1], &m) == -1 && errno == EPROTO);
	close(sv[0]);
	close(sv[1]);
}

static void test_listen_connect(void)
{
	char dir[256], path[300];
	struct pss_msg m;
	int lfd, cfd, afd;

	t_mkdtemp(dir);
	snprintf(path, sizeof path, "%s/hub.sock", dir);
	lfd = ipc_listen(path);
	CHECK(lfd >= 0);
	cfd = ipc_connect(path);
	CHECK(cfd >= 0);
	afd = accept(lfd, NULL, NULL);
	CHECK(afd >= 0);
	pss_msg_init(&m, PSS_MSG_HELLO);
	m.u.hello.role = PSS_ROLE_CAM;
	CHECK(ipc_send(cfd, &m) == 0);
	CHECK(ipc_recv(afd, &m) == 1 && m.u.hello.role == PSS_ROLE_CAM);
	close(afd);
	close(cfd);
	close(lfd);

	/* No listener: connect fails cleanly instead of hanging. */
	CHECK(unlink(path) == 0);
	CHECK(ipc_connect(path) == -1);
	t_rm_rf(dir);
}

int main(void)
{
	T_RUN(test_roundtrip_and_eof);
	T_RUN(test_rejects_bad_size_and_version);
	T_RUN(test_listen_connect);
	T_END();
}
