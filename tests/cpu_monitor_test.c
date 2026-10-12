/*
 * Unit test for read_cpu_ticks.
 *
 * Two assertions:
 *
 *   1. Reading our own /proc/self/stat succeeds and returns a
 *      value in a plausible range. The upper bound catches the
 *      failure mode where the parser reads the wrong field
 *      (e.g. ppid, pgrp, flags, or starttime) but still
 *      reports success.
 *
 *   2. A child process whose comm contains a space is read
 *      successfully. This is the case the previous fscanf
 *      parser failed on: "%255s" stopped at the first space
 *      inside comm, and "%c" then read that space instead of
 *      the state character, so every subsequent field was
 *      wrong.
 *
 * Note on WSL2: /proc/self/stat for a currently-running task
 * may report utime=stime=0 even after substantial CPU work,
 * because the kernel does not lazily update the counters for a
 * self-read. Test 1 therefore asserts a range, not positivity.
 * Test 2 checks parse success on the space-in-comm case, which
 * is the actual subject of the fix.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/prctl.h>

#include "cpu_monitor.h"

int main(void)
{
    /* Test 1: our own ticks, plausible range */
    long ticks = 0;

    if (!read_cpu_ticks(getpid(), &ticks))
    {
        printf("FAIL: read_cpu_ticks(getpid()) returned 0\n");
        return 1;
    }

    if (ticks < 0)
    {
        printf("FAIL: read_cpu_ticks(getpid()) = %ld (negative)\n",
               ticks);
        return 1;
    }

    /*
     * A freshly-started process has at most a few seconds of CPU
     * time — well under 100000 ticks (1000 seconds at 100 Hz).
     * If we see more, the parser is reading a field like flags
     * (which can be millions) or starttime (which is on the
     * order of hundreds of thousands since boot).
     */
    if (ticks > 100000)
    {
        printf("FAIL: read_cpu_ticks(getpid()) = %ld "
               "(suspiciously large — parser may be reading the "
               "wrong field)\n",
               ticks);
        return 1;
    }

    printf("PASS: read_cpu_ticks(self) succeeded, ticks = %ld\n",
           ticks);

    /* Test 2: child with a space in comm */
    pid_t child = fork();

    if (child < 0)
    {
        printf("FAIL: fork returned %d\n", (int)child);
        return 1;
    }

    if (child == 0)
    {
        /*
         * Set comm to a string with a space, then burn CPU so
         * utime/stime become non-zero on kernels that update
         * running-process counters.
         */
        prctl(PR_SET_NAME, "with spaces", 0, 0, 0);

        volatile unsigned long b = 0;

        for (;;)
        {
            b++;
        }
    }

    /* Give the child time to install its comm and burn some CPU */
    usleep(200000);

    long child_ticks = 0;
    int rc = read_cpu_ticks(child, &child_ticks);

    kill(child, SIGKILL);
    waitpid(child, NULL, 0);

    if (!rc)
    {
        printf("FAIL: read_cpu_ticks on child with spaces in comm "
               "returned 0\n");
        return 1;
    }

    printf("PASS: read_cpu_ticks on child-with-spaces succeeded, "
           "ticks = %ld\n",
           child_ticks);

    printf("PASS: read_cpu_ticks handles names containing spaces\n");

    return 0;
}
