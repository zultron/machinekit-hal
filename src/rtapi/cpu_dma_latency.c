#include "cpu_dma_latency.h"
#include "rtapi.h" // rtapi_print_msg()

#include <stdio.h>  // popen()
#include <string.h>  // strerror()
#include <errno.h>  // errno

#ifndef CPU_DMA_LATENCY_HELPER
#error CPU_DMA_LATENCY_HELPER undefined
#endif

FILE *p = NULL;

int rtapi_set_cpu_dma_latency(void)
{
    // Sanity check
    if (p != NULL) {
        fprintf(
            stderr,
            "rtapi_set_cpu_dma_latency() called while file already open\n");
        return EALREADY;
    }

    // Run set_cpu_dma_latency executable, keeping file open
    fprintf(
        stderr,
        "Running '%s'\n", CPU_DMA_LATENCY_HELPER);
    p = popen(CPU_DMA_LATENCY_HELPER, "w");
    if (!p) {
        fprintf(
            stderr,
            "rtapi_set_cpu_dma_latency() failed to popen(%s) (%d):  %s\n",
            CPU_DMA_LATENCY_HELPER, errno, strerror(errno));
        return errno;
    }

    return 0;
}

int rtapi_unset_cpu_dma_latency(void)
{
    // Sanity check
    if (p == NULL) {
        fprintf(
            stderr,
            "rtapi_set_cpu_dma_latency() called while file already closed\n");
        return EALREADY;
    }

    // Close file
    if (pclose(p) != 0) {
        fprintf(
            stderr,
            "rtapi_set_cpu_dma_latency() failed to close file (%d):  %s\n",
            errno, strerror(errno));
        return errno;
    }
    return 0;
}
