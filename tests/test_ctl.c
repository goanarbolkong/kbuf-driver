/*
 * test_ctl.c — /dev/kbuf-ctl dynamic create/destroy and kref lifetime.
 *
 * Verifies that a device created at runtime works, that destroying it while a
 * process still has it open does NOT free the ring out from under that process
 * (the data written before destroy is still readable afterwards — the kref
 * contract), and that the node is gone for new opens once destroyed.
 */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>

#include "kbuf.h"

#define CTL "/dev/kbuf-ctl"

static int failures;

static void check(const char *what, int cond)
{
	printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
	if (!cond)
		failures++;
}

int main(void)
{
	char path[32], buf[32];
	int ctl, id, fd;
	ssize_t n;

	ctl = open(CTL, O_RDWR);
	if (ctl < 0) {
		perror("open " CTL);
		return 1;
	}

	printf("=== Test 1: create a device ===\n");
	id = -1;
	check("KBUF_CTL_CREATE ok", ioctl(ctl, KBUF_CTL_CREATE, &id) == 0);
	check("returned a valid id", id >= 0);
	snprintf(path, sizeof(path), "/dev/kbufd%d", id);

	fd = open(path, O_RDWR | O_NONBLOCK);
	check("new device node opens", fd >= 0);
	check("device works (write 5 bytes)", fd >= 0 && write(fd, "hello", 5) == 5);

	printf("=== Test 2: destroy while still open (kref) ===\n");
	check("KBUF_CTL_DESTROY ok", ioctl(ctl, KBUF_CTL_DESTROY, &id) == 0);

	/* New opens must fail: the node is gone. */
	check("node removed for new opens", open(path, O_RDONLY) < 0);

	/* But our existing fd must still work — the ring is kept alive by the
	 * reference our open holds, so the queued message is still readable. */
	n = read(fd, buf, sizeof(buf));
	check("queued data still readable via open fd", n == 5);
	check("data intact", n == 5 && memcmp(buf, "hello", 5) == 0);
	close(fd);			/* last reference drops; ring freed now */

	printf("=== Test 3: destroying an unknown id fails ===\n");
	id = 9999;
	errno = 0;
	check("destroy bad id returns ENOENT",
	      ioctl(ctl, KBUF_CTL_DESTROY, &id) == -1 && errno == ENOENT);

	printf("=== Test 4: create/destroy round trip reuses cleanly ===\n");
	check("create again", ioctl(ctl, KBUF_CTL_CREATE, &id) == 0 && id >= 0);
	check("destroy again", ioctl(ctl, KBUF_CTL_DESTROY, &id) == 0);

	close(ctl);

	printf("\n%s (%d failure%s)\n", failures ? "RESULT: FAIL" : "RESULT: PASS",
	       failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
