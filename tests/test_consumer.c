/**
 * test_consumer.c — reads messages from /dev/kbuf (blocking mode)
 * Usage: ./test_consumer [count] [delay_ms]
 */

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>

#define DEVICE "/dev/kbuf"

int main(int argc, char *argv[])
{
    int  count    = (argc > 1) ? atoi(argv[1]) : 10;
    int  delay_ms = (argc > 2) ? atoi(argv[2]) : 1000;
    char buf[4096];
    int  fd;

    fd = open(DEVICE, O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }

    printf("[Consumer] pid=%d, reading %d messages with %d ms delay\n",
           getpid(), count, delay_ms);

    for (int i = 1; i <= count; i++) {
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n < 0) { perror("read"); break; }
        buf[n] = '\0';
        printf("[Consumer] msg %d/%d: %s\n", i, count, buf);
        usleep((useconds_t)delay_ms * 1000);
    }

    close(fd);
    printf("[Consumer] done.\n");
    return 0;
}
