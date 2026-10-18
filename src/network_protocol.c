
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "network_protocol.h"

/*
 * Send exactly 'length' bytes.
 *
 * TCP may send fewer bytes than requested,
 * so keep calling send() until everything
 * has been transmitted.
 *
 * Retry if the system call is interrupted.
 * MSG_NOSIGNAL prevents SIGPIPE on Linux
 * if the peer has closed its connection.
 */
int send_all(int socket_fd,
             const void *buffer,
             size_t length)
{
    const char *data = buffer;
    size_t total_sent = 0;

    while (total_sent < length)
    {
        ssize_t bytes_sent = send(
            socket_fd,
            data + total_sent,
            length - total_sent,
            MSG_NOSIGNAL
        );

        if (bytes_sent < 0 && errno == EINTR)
        {
            continue;
        }

        if (bytes_sent <= 0)
        {
            return 0;
        }

        total_sent += (size_t)bytes_sent;
    }

    return 1;
}

/*
 * Receive exactly 'length' bytes.
 *
 * TCP may return fewer bytes than requested,
 * so keep calling recv() until the complete
 * data has been received.
 *
 * Retry if the system call is interrupted.
 */
int receive_all(int socket_fd,
                void *buffer,
                size_t length)
{
    char *data = buffer;
    size_t total_received = 0;

    while (total_received < length)
    {
        ssize_t bytes_received = recv(
            socket_fd,
            data + total_received,
            length - total_received,
            0
        );

        if (bytes_received < 0 && errno == EINTR)
        {
            continue;
        }

        if (bytes_received <= 0)
        {
            return 0;
        }

        total_received += (size_t)bytes_received;
    }

    return 1;
}

/*
 * Send one framed message.
 *
 * Wire format:
 *
 * [4-byte network-order length][payload]
 */
int send_message(int socket_fd,
                 const char *message)
{
    if (message == NULL)
    {
        return 0;
    }

    size_t message_length = strlen(message);

    if (message_length > NETWORK_MAX_MESSAGE_SIZE)
    {
        return 0;
    }

    /*
     * Convert the message length to a fixed
     * 32-bit integer.
     */
    uint32_t length = (uint32_t)message_length;

    /*
     * Convert to network byte order.
     */
    uint32_t network_length = htonl(length);

    /*
     * Send the 4-byte length header.
     */
    if (!send_all(
            socket_fd,
            &network_length,
            sizeof(network_length)))
    {
        return 0;
    }

    /*
     * Send the message payload.
     */
    if (!send_all(
            socket_fd,
            message,
            message_length))
    {
        return 0;
    }

    return 1;
}

/*
 * Receive one framed message.
 *
 * Reads:
 *
 * [4-byte network-order length][payload]
 *
 * Adds a terminating '\0' locally.
 */
int receive_message(int socket_fd,
                    char *buffer,
                    size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0)
    {
        return 0;
    }

    /*
     * Receive the 4-byte message length.
     */
    uint32_t network_length;

    if (!receive_all(
            socket_fd,
            &network_length,
            sizeof(network_length)))
    {
        return 0;
    }

    /*
     * Convert the length to host byte order.
     */
    uint32_t message_length = ntohl(network_length);

    /*
     * Reject oversized messages.
     */
    if (message_length > NETWORK_MAX_MESSAGE_SIZE)
    {
        return 0;
    }

    /*
     * Ensure room for the payload and
     * terminating '\0'.
     */
    if ((size_t)message_length >= buffer_size)
    {
        return 0;
    }

    /*
     * Receive the complete payload.
     */
    if (!receive_all(
            socket_fd,
            buffer,
            (size_t)message_length))
    {
        return 0;
    }

    /*
     * Terminate the received data as a
     * C string.
     */
    buffer[message_length] = '\0';

    return 1;
}
