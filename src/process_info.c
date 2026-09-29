#include <stdio.h>
#include "process_info.h"

int read_process_info(int pid, struct ProcessInfo *process)
{
    char path[256];

    snprintf(path, sizeof(path), "/proc/%d/status", pid);

    FILE *file = fopen(path, "r");

    if (file == NULL)
    {
        return 0;
    }

    char line[256];

    while (fgets(line, sizeof(line), file))
    {
        if (sscanf(line, "Name:\t%63[^\n]", process->name) == 1)
        {
            continue;
        }

        if (sscanf(line, "State:\t%63[^\n]", process->state) == 1)
        {
            continue;
        }

        if (sscanf(line, "Pid:\t%d", &process->pid) == 1)
        {
            continue;
        }

        if (sscanf(line, "PPid:\t%d", &process->parent_pid) == 1)
        {
            continue;
        }

        if (sscanf(line, "VmRSS:\t%ld kB", &process->memory_kb) == 1)
        {
            continue;
        }

        if (sscanf(line, "Threads:\t%d", &process->threads) == 1)
        {
            continue;
        }
    }

    fclose(file);

    return 1;
}
