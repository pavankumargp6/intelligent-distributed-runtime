#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ProcessInfo
{
    int pid;
    int parent_pid;
    char name[64];
    char state[64];
    long memory_kb;
    int threads;
};

int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        printf("Usage: %s <PID>\n", argv[0]);
        return 1;
    }

    int pid = atoi(argv[1]);

    char path[256];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);

    FILE *file = fopen(path, "r");

    if (file == NULL)
    {
        printf("Could not open process information for PID %d.\n", pid);
        return 1;
    }

    struct ProcessInfo process = {0};

    char line[256];

    while (fgets(line, sizeof(line), file))
    {
        if (sscanf(line, "Name:\t%63[^\n]", process.name) == 1)
        {
            continue;
        }

        if (sscanf(line, "State:\t%63[^\n]", process.state) == 1)
        {
            continue;
        }

        if (sscanf(line, "Pid:\t%d", &process.pid) == 1)
        {
            continue;
        }

        if (sscanf(line, "PPid:\t%d", &process.parent_pid) == 1)
        {
            continue;
        }

        if (sscanf(line, "VmRSS:\t%ld kB", &process.memory_kb) == 1)
        {
            continue;
        }

        if (sscanf(line, "Threads:\t%d", &process.threads) == 1)
        {
            continue;
        }
    }

    fclose(file);

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
