#include <stdio.h>  // getline(), fprintf()
#include <stdlib.h>  // free()
#include <unistd.h>  // write(), geteuid()
#include <stdint.h>  // uint32_t
#include <fcntl.h>  // open()
#include <string.h>  // strerror()
#include <errno.h>  // errno

#define NODE_PATH "/dev/cpu_dma_latency"
#define PFX "cpu_dma_latency_helper:  "

int main(void)
{
    char *buf = NULL;
    size_t n = 0;
    int fd, res;
    uint32_t latency_lim = 0;

    // Sanity checks
    if (geteuid() != 0) {
        fprintf(stderr, PFX "Must run as root user\n");
        return 1;
    }

    // Open cpu_dma_latency file, write (u32)0, and hold fd open
    fd = open(NODE_PATH, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, PFX "Failed to open " NODE_PATH " for write (%d):  %s\n",
                errno, strerror(errno));
        return 1;
    }
    write(fd, &latency_lim, sizeof(latency_lim));
    fprintf(stderr, PFX "Wrote 0x%04x to " NODE_PATH "; holding fd %d open\n",
            latency_lim, fd);

    // Wait for exit signal on stderr
    res = getline(&buf, &n, stdin);
    free(buf);

    // Clean up and exit
    if (close(fd) != 0) {
        fprintf(stderr, PFX "Failed to close fd %d (%d):  %s\n",
                fd, errno, strerror(errno));
        return 1;
    }
    fprintf(stderr, PFX "Closed fd %d upon %s\n",
            fd, res ? "newline" : "closed stdin");
    return 0;
}
