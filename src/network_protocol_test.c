
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>
#include <sys/socket.h>

#include "network_protocol.h"

static volatile sig_atomic_t signal_received = 0;

static void handle_signal(int signal_number)
{
    (void)signal_number;
    signal_received = 1;
}

struct receive_args
{
    int socket_fd;
    int result;
    char buffer[16];
};

static void *receive_thread(void *arg)
{
    struct receive_args *args = arg;

    args->result = receive_all(
        args->socket_fd,
        args->buffer,
        4
    );

    return NULL;
}

static int test_message_framing(void)
{
    int sockets[2];
    char buffer[128];

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
    {
        perror("socketpair");
        return 0;
    }

    int passed = send_message(sockets[0], "PING") &&
                 receive_message(sockets[1], buffer, sizeof(buffer)) &&
                 strcmp(buffer, "PING") == 0;

    close(sockets[0]);
    close(sockets[1]);

    printf("%s: message framing\n", passed ? "PASS" : "FAIL");
    return passed;
}

static int test_interrupted_receive(void)
{
    int sockets[2];
    pthread_t thread;
    struct receive_args args = {0};
    struct sigaction action = {0};
    struct sigaction old_action;
    struct timespec delay = {0, 100000000L};

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
    {
        perror("socketpair");
        return 0;
    }

    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;

    if (sigaction(SIGUSR1, &action, &old_action) < 0)
    {
        perror("sigaction");
        close(sockets[0]);
        close(sockets[1]);
        return 0;
    }

    args.socket_fd = sockets[1];

    if (pthread_create(&thread, NULL, receive_thread, &args) != 0)
    {
        perror("pthread_create");
        sigaction(SIGUSR1, &old_action, NULL);
        close(sockets[0]);
        close(sockets[1]);
        return 0;
    }

    /*
     * Give the receiving thread time to block in recv().
     * Repeated signals reduce timing sensitivity.
     */
    for (int i = 0; i < 10 && !signal_received; i++)
    {
        nanosleep(&delay, NULL);
        pthread_kill(thread, SIGUSR1);
    }

    const char payload[] = "TEST";
    int sent = send_all(sockets[0], payload, sizeof(payload) - 1);

    pthread_join(thread, NULL);
    sigaction(SIGUSR1, &old_action, NULL);

    int passed = signal_received &&
                 sent &&
                 args.result &&
                 memcmp(args.buffer, payload, 4) == 0;

    close(sockets[0]);
    close(sockets[1]);

    printf("%s: interrupted receive retries\n",
           passed ? "PASS" : "FAIL");

    return passed;
}

static int test_closed_peer(void)
{
    int sockets[2];

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
    {
        perror("socketpair");
        return 0;
    }

    close(sockets[1]);

    const char payload[] = "TEST";
    int result = send_all(sockets[0], payload, sizeof(payload) - 1);

    close(sockets[0]);

    int passed = result == 0;

    printf("%s: closed peer does not terminate process\n",
           passed ? "PASS" : "FAIL");

    return passed;
}

int main(void)
{
    int passed = 0;
    int total = 3;

    passed += test_message_framing();
    passed += test_interrupted_receive();
    passed += test_closed_peer();

    printf("\nProtocol tests: %d/%d passed\n", passed, total);

    return passed == total ? 0 : 1;
}
