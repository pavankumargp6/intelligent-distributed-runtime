#include <stdio.h>
#include <stdlib.h>

#include "resource_monitor.h"

int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        printf("Usage: %s <PID>\n", argv[0]);
        return 1;
    }

    int pid = atoi(argv[1]);

    struct ResourceSnapshot snapshot = {0};

    printf("Collecting resource information for PID %d...\n", pid);

    if (!collect_resource_snapshot(pid, &snapshot))
    {
        printf("Could not collect resource information for PID %d.\n", pid);
        return 1;
    }

    printf("\n");
    printf("========================================\n");
    printf("        RESOURCE SNAPSHOT\n");
    printf("========================================\n");

    printf("PID        : %d\n", snapshot.process.pid);
    printf("Name       : %s\n", snapshot.process.name);
    printf("State      : %s\n", snapshot.process.state);
    printf("Parent PID : %d\n", snapshot.process.parent_pid);
    printf("Memory     : %ld kB\n", snapshot.process.memory_kb);
    printf("Threads    : %d\n", snapshot.process.threads);
    printf("CPU Usage  : %.2f%%\n", snapshot.cpu_usage);

    printf("========================================\n");

    return 0;
}
