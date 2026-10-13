#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth_config.h"

static int initialized = 0;
static int secure_mode = 0;

static char worker_secret[256];
static char client_secret[256];
static int have_worker_secret = 0;
static int have_client_secret = 0;

static const char *env_or_null(const char *name)
{
    const char *v = getenv(name);

    if (v == NULL || v[0] == '\0')
    {
        return NULL;
    }

    return v;
}

static int copy_secret(const char *src, char *dst, size_t dst_size)
{
    size_t len = strlen(src);

    if (len >= dst_size)
    {
        return 0;
    }

    memcpy(dst, src, len + 1);
    return 1;
}

int auth_config_init(void)
{
    if (initialized)
    {
        /*
         * If init already ran successfully once, subsequent
         * calls just report that. If it failed the first time,
         * a retry would give the same result (env does not
         * change mid-process), so keep the failure sticky.
         */
        return secure_mode || !have_worker_secret;
    }

    initialized = 1;

    const char *insecure = env_or_null("IDR_INSECURE");

    if (insecure != NULL && strcmp(insecure, "1") == 0)
    {
        secure_mode = 0;
        return 1;
    }

    secure_mode = 1;

    const char *w = env_or_null("IDR_WORKER_SECRET");
    const char *c = env_or_null("IDR_CLIENT_SECRET");

    if (w == NULL || c == NULL)
    {
        return 0;
    }

    if (strlen(w) < AUTH_CONFIG_MIN_SECRET_BYTES ||
        strlen(c) < AUTH_CONFIG_MIN_SECRET_BYTES)
    {
        return 0;
    }

    if (!copy_secret(w, worker_secret, sizeof(worker_secret)) ||
        !copy_secret(c, client_secret, sizeof(client_secret)))
    {
        return 0;
    }

    have_worker_secret = 1;
    have_client_secret = 1;

    return 1;
}

int auth_config_is_secure(void)
{
    /*
     * Auto-init. This lets callers that only need a secret or
     * a mode -- for example the worker's sign_if_secure in
     * worker_registration_client.c -- read the right values
     * without having to remember to call auth_config_init()
     * first.
     */
    if (!initialized)
    {
        auth_config_init();
    }

    return secure_mode;
}

const char *auth_config_worker_secret(void)
{
    if (!initialized)
    {
        auth_config_init();
    }

    return have_worker_secret ? worker_secret : NULL;
}

const char *auth_config_client_secret(void)
{
    if (!initialized)
    {
        auth_config_init();
    }

    return have_client_secret ? client_secret : NULL;
}
