#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <ctype.h>
#include "process_info.h"

int is_pid(const char *name)
{
    for (int i = 0; name[i] != '\0'; i++)
    {
        if (!isdigit(name[i]))
        {
            return 0;
        }
    }

    return 1;
}

int main(void)
{
    DIR *directory;

    directory = opendir("/proc");

    if (directory == NULL)
    {
        printf("Could not open /proc\n");
        return 1;
    }

    struct dirent *entry;

    while ((entry = readdir(directory)) != NULL)
    {
        if (is_pid(entry->d_name))
        {
            int pid = atoi(entry->d_name);

            struct ProcessInfo process = {0};

            if (read_process_info(pid, &process))
            {
                printf("PID: %-6d Name: %-20s Memory: %ld kB Threads: %d\n",
                       process.pid,
                       process.name,
                       process.memory_kb,
                       process.threads);
            }
        }
    }

    closedir(directory);

    return 0;
}
