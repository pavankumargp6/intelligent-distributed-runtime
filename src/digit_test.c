#include <stdio.h>
#include <ctype.h>

int main(void)
{
    char name[] = "2944";

    int is_pid = 1;

    for (int i = 0; name[i] != '\0'; i++)
    {
        if (!isdigit(name[i]))
        {
            is_pid = 0;
            break;
        }
    }

    if (is_pid)
    {
        printf("%s is a PID.\n", name);
    }
    else
    {
        printf("%s is not a PID.\n", name);
    }

    return 0;
}
