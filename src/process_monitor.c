#include <stdio.h>
#include <stdlib.h>
#include "process_info.h"

int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        printf("Usage: %s <PID>\n", argv[0]);
        return 1;
    }

    int pid = atoi(argv[1]);

    struct ProcessInfo process = {0};

    if (!read_process_info(pid, &process))
    {
        printf("Could not open process information for PID %d.\n", pid);
        return 1;
    }

    printf("\n");
    printf("================================\n");
    printf("       PROCESS INFORMATION\n");
    printf("================================\n");
    printf("PID        : %d\n", process.pid);
    printf("Name       : %s\n", process.name);
    printf("State      : %s\n", process.state);
    printf("Parent PID : %d\n", process.parent_pid);
    printf("Memory     : %ld kB\n", process.memory_kb);
    printf("Threads    : %d\n", process.threads);
    printf("================================\n");

    return 0;
}

