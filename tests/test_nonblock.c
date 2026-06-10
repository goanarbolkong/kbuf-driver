/**
 * test_nonblock.c — demonstrates O_NONBLOCK behavior on /dev/kbuf
 *
 * Shows:
 *   1. Non-blocking read on empty buffer → EAGAIN immediately
 *   2. Non-blocking write that fills all slots → EAGAIN at slot NUM_BUFFERS
 *   3. Non-blocking read after filling → succeeds for each slot
 */

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#define DEVICE      "/dev/kbuf0"
#define NUM_BUFFERS 8

int main(void)
{
    char    buf[4096];
    ssize_t n;
    int     fd;

    /* --- test 1: non-blocking read on empty buffer --- */
    printf("=== Test 1: non-blocking read on empty buffer ===\n");
    fd = open(DEVICE, O_RDONLY | O_NONBLOCK);
    if (fd < 0) { perror("open"); return 1; }
    n = read(fd, buf, sizeof(buf));
    if (n < 0 && errno == EAGAIN)
        printf("  PASS: got EAGAIN (buffer empty, non-blocking)\n");
    else if (n >= 0)
        printf("  NOTE: read %zd bytes (buffer was not empty)\n", n);
    else
        perror("  FAIL: read");
    close(fd);

    /* --- test 2: non-blocking writes until full --- */
    printf("\n=== Test 2: non-blocking writes until buffer full ===\n");
    fd = open(DEVICE, O_WRONLY | O_NONBLOCK);
    if (fd < 0) { perror("open"); return 1; }
    int written = 0;
    for (int i = 0; i < NUM_BUFFERS + 2; i++) {
        int len = snprintf(buf, sizeof(buf), "nonblock-slot-%d", i);
        n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EAGAIN) {
                printf("  PASS: got EAGAIN at attempt %d (buffer full after %d slots)\n",
                       i, written);
                break;
            }
            perror("  FAIL: write");
            break;
        }
        printf("  Wrote slot %d: %.*s\n", i, (int)n, buf);
        written++;
    }
    close(fd);

    /* --- test 3: non-blocking reads drain what we just wrote --- */
    printf("\n=== Test 3: non-blocking reads drain buffer ===\n");
    fd = open(DEVICE, O_RDONLY | O_NONBLOCK);
    if (fd < 0) { perror("open"); return 1; }
    for (int i = 0; ; i++) {
        n = read(fd, buf, sizeof(buf) - 1);
        if (n < 0) {
            if (errno == EAGAIN) {
                printf("  Buffer drained after %d reads — EAGAIN\n", i);
                break;
            }
            perror("  FAIL: read");
            break;
        }
        buf[n] = '\0';
        printf("  Read slot %d: %s\n", i, buf);
    }
    close(fd);

    return 0;
}
