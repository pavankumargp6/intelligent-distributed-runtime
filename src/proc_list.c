#include <stdio.h>
#include <dirent.h>
#include <ctype.h>

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
            printf("PID: %s\n", entry->d_name);
        }
    }

    closedir(directory);

    return 0;
}
