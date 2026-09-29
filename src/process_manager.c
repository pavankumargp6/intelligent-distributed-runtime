#include <stdio.h>
#include <unistd.h>

int main(void)
{
    printf("Hello from the Intelligent Distributed Runtime!\n");
    printf("My process ID is: %d\n", getpid());

    printf("Process is running for 30 seconds...\n");

    sleep(120);

    printf("Process finished.\n");

    return 0;
}


