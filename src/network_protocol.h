#ifndef NETWORK_PROTOCOL_H
#define NETWORK_PROTOCOL_H

#include <stddef.h>

/*
 * Maximum payload that one network message
 * is allowed to contain.
 */
#define NETWORK_MAX_MESSAGE_SIZE 4096

/*
 * Send exactly the requested number of bytes.
 *
 * Returns 1 on success.
 * Returns 0 on failure.
 */
int send_all(int socket_fd,
             const void *buffer,
             size_t length);

/*
 * Receive exactly the requested number of bytes.
 *
 * Returns 1 on success.
 * Returns 0 if the connection closes
 * or an error occurs.
 */
int receive_all(int socket_fd,
                void *buffer,
                size_t length);

/*
 * Send one framed message.
 *
 * Format:
 *
 * [4-byte length][payload]
 *
 * Returns 1 on success.
 * Returns 0 on failure.
 */
int send_message(int socket_fd,
                 const char *message);

/*
 * Receive one framed message.
 *
 * The received payload is written into
 * the provided buffer and null-terminated.
 *
 * Returns 1 on success.
 * Returns 0 on failure.
 */
int receive_message(int socket_fd,
                    char *buffer,
                    size_t buffer_size);

#endif
