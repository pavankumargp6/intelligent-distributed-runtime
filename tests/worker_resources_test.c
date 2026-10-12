/*
 * Unit test for collect_worker_resources.
 *
 * Asserts that on a Linux system with /proc and a modern
 * kernel, the four output fields are populated with sane
 * values and the CPU availability bounds hold.
 *
 * This does not test the failure path (missing MemAvailable,
 * empty /proc, etc.) — that would require mocking the
 * filesystem and is deferred to a later milestone.
 */

#include <stdio.h>
#include <string.h>

#include "worker_resources.h"

int main(void)
{
    struct WorkerResources res;

    /*
     * Poison the struct with obvious non-zero values so we
     * can prove collect_worker_resources overwrites them.
     */
    memset(&res, 0xAA, sizeof(res));

    if (!collect_worker_resources(&res))
    {
        printf("FAIL: collect_worker_resources returned 0\n");
        return 1;
    }

    if (res.total_cpu_percent <= 0.0)
    {
        printf("FAIL: total_cpu_percent = %.2f (expected > 0)\n",
               res.total_cpu_percent);
        return 1;
    }

    if (res.available_cpu_percent < 0.0)
    {
        printf("FAIL: available_cpu_percent = %.2f (expected >= 0)\n",
               res.available_cpu_percent);
        return 1;
    }

    if (res.available_cpu_percent > res.total_cpu_percent)
    {
        printf("FAIL: available_cpu_percent (%.2f) > "
               "total_cpu_percent (%.2f)\n",
               res.available_cpu_percent,
               res.total_cpu_percent);
        return 1;
    }

    if (res.total_memory_mb <= 0)
    {
        printf("FAIL: total_memory_mb = %ld (expected > 0)\n",
               res.total_memory_mb);
        return 1;
    }

    if (res.available_memory_mb <= 0)
    {
        printf("FAIL: available_memory_mb = %ld (expected > 0; "
               "if this fails, the A3 fix is not in effect)\n",
               res.available_memory_mb);
        return 1;
    }

    if (res.available_memory_mb > res.total_memory_mb)
    {
        printf("FAIL: available_memory_mb (%ld) > "
               "total_memory_mb (%ld)\n",
               res.available_memory_mb,
               res.total_memory_mb);
        return 1;
    }

    printf("PASS: collect_worker_resources produced sane values\n");
    printf("  total_cpu_percent     = %.2f\n", res.total_cpu_percent);
    printf("  available_cpu_percent = %.2f\n", res.available_cpu_percent);
    printf("  total_memory_mb       = %ld\n", res.total_memory_mb);
    printf("  available_memory_mb   = %ld\n", res.available_memory_mb);

   return 0;
}
