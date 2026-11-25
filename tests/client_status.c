/*
 * Minimal IDR status client.
 *
 * Sends STATUS, verifies the signed response, and prints it.
 * Exits 0 on STATUS_REPLY.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "network_protocol.h"
#include "auth.h"
#include "auth_config.h"
#include "runtime_endpoint.h"

int main(void)
{
    const char *runtime_address = runtime_connect_host();
    int runtime_port = runtime_connect_port();

    if (runtime_port < 0)
    {
        fprintf(stderr,
                "IDR_COORDINATOR_PORT must be an integer "
                "between 1 and 65535.\n");
        return 1;
    }

    if (!auth_config_init())
    {
        fprintf(stderr,
                "Authentication config invalid. Set "
                "IDR_WORKER_SECRET and IDR_CLIENT_SECRET, or "
                "IDR_INSECURE=1.\n");
        return 1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0)
    {
        perror("socket");
        return 1;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;

    if (inet_pton(AF_INET,
                  runtime_address,
                  &addr.sin_addr) <= 0)
    {
        perror("inet_pton");
        close(fd);
        return 1;
    }

    addr.sin_port = htons((unsigned short)runtime_port);

    if (connect(fd,
                (struct sockaddr *)&addr,
                sizeof(addr)) < 0)
    {
        perror("connect");
        close(fd);
        return 1;
    }

    const char *plain = "STATUS";
    char to_send[AUTH_MAX_SIGNED_SIZE];
    const char *payload = plain;

    if (auth_config_is_secure())
    {
        AuthResult r = auth_sign(auth_config_client_secret(),
                                 AUTH_DIR_CLIENT_TO_COORD,
                                 AUTH_ROLE_CLIENT,
                                 plain,
                                 to_send,
                                 sizeof(to_send));
        if (r != AUTH_OK)
        {
            fprintf(stderr, "auth_sign failed: %s\n",
                    auth_result_name(r));
            close(fd);
            return 1;
        }
        payload = to_send;
    }

    if (!send_message(fd, payload))
    {
        fprintf(stderr, "send failed\n");
        close(fd);
        return 1;
    }

    char response[AUTH_MAX_SIGNED_SIZE];

    if (!receive_message(fd, response, sizeof(response)))
    {
        fprintf(stderr, "receive failed\n");
        close(fd);
        return 1;
    }

    close(fd);

    char verified[AUTH_MAX_SIGNED_SIZE];
    const char *final_response = response;

    if (auth_config_is_secure())
    {
        AuthResult r = auth_verify(auth_config_client_secret(),
                                   AUTH_DIR_COORD_TO_CLIENT,
                                   AUTH_ROLE_COORDINATOR,
                                   response,
                                   verified,
                                   sizeof(verified));
        if (r != AUTH_OK)
        {
            fprintf(stderr, "auth_verify failed: %s\n",
                    auth_result_name(r));
            return 1;
        }
        final_response = verified;
    }

    printf("%s\n", final_response);

    if (strncmp(final_response, "STATUS_REPLY|", 13) == 0)
    {
        return 0;
    }

    return 1;
}
