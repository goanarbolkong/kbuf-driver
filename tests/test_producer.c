/**
 * test_producer.c — writes messages into /dev/kbuf (blocking mode)
 * Usage: ./test_producer [count] [delay_ms]
 */

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>

#define DEVICE "/dev/kbuf0"

int main(int argc, char *argv[])
{
    int   count    = (argc > 1) ? atoi(argv[1]) : 10;
    int   delay_ms = (argc > 2) ? atoi(argv[2]) : 500;
    char  buf[256];
    int   fd;

    fd = open(DEVICE, O_WRONLY);
    if (fd < 0) { perror("open"); return 1; }

    printf("[Producer] pid=%d, writing %d messages with %d ms delay\n",
           getpid(), count, delay_ms);

    for (int i = 1; i <= count; i++) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        int len = snprintf(buf, sizeof(buf),
                           "MSG#%03d from pid=%d time=%ld.%03ld",
                           i, getpid(), ts.tv_sec, ts.tv_nsec / 1000000);

        ssize_t n = write(fd, buf, len);
        if (n < 0) { perror("write"); break; }

        printf("[Producer] slot written: %s\n", buf);
        usleep((useconds_t)delay_ms * 1000);
    }

    close(fd);
    printf("[Producer] done.\n");
    return 0;
}
