#ifndef AUTH_CONFIG_H
#define AUTH_CONFIG_H

/*
 * Read authentication configuration from the environment.
 *
 * Environment variables:
 *
 *   IDR_WORKER_SECRET    Shared secret used by workers and by
 *                        the coordinator to sign and verify
 *                        messages on the worker channel.
 *
 *   IDR_CLIENT_SECRET    Shared secret used by clients and by
 *                        the coordinator to sign and verify
 *                        messages on the client channel.
 *
 *   IDR_INSECURE         If set to "1", authentication is
 *                        disabled. The coordinator additionally
 *                        requires that the bind address be
 *                        loopback in this mode.
 *
 * In secure mode (the default) both secrets are required and
 * must be at least AUTH_CONFIG_MIN_SECRET_BYTES long. The
 * coordinator additionally refuses to start if either secret
 * is missing.
 *
 * auth_config_init() is idempotent: the first call reads the
 * environment, subsequent calls are no-ops.
 *
 * Returns 1 if the configuration is valid (either secure mode
 * with both secrets present, or insecure mode).
 *
 * Returns 0 if the configuration is invalid (secure mode with
 * a missing or too-short secret). The caller should refuse to
 * start.
 */
int auth_config_init(void);

/*
 * Returns 1 if authentication is enabled (the default), 0 if
 * running in insecure mode.
 */
int auth_config_is_secure(void);

/*
 * Returns the worker secret, or NULL if insecure mode is
 * enabled or init has not been called.
 */
const char *auth_config_worker_secret(void);

/*
 * Returns the client secret, or NULL if insecure mode is
 * enabled or init has not been called.
 */
const char *auth_config_client_secret(void);

/*
 * Minimum secret length. 32 bytes is the shortest output of
 * SHA-256 and gives 256 bits of entropy if the secret is
 * random. Shorter secrets are rejected to discourage
 * "password"-style keys.
 */
#define AUTH_CONFIG_MIN_SECRET_BYTES 32

#endif
