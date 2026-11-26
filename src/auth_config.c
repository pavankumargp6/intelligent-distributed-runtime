#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "auth_config.h"

#define MAX_WORKER_CREDENTIALS 1024
#define MAX_CRED_LINE 512

static int initialized = 0;
static int secure_mode = 0;

static char worker_secret[256];
static char client_secret[256];
static int have_worker_secret = 0;
static int have_client_secret = 0;

/* Per-worker credentials */
static int worker_cred_count = 0;
static int worker_cred_ids[MAX_WORKER_CREDENTIALS];
static char worker_cred_secrets[MAX_WORKER_CREDENTIALS][256];

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

static int load_worker_credentials(void)
{
    const char *path = env_or_null("IDR_WORKER_CREDENTIALS_FILE");

    if (path == NULL)
    {
        return 1; /* Not configured, not an error */
    }

    FILE *f = fopen(path, "r");

    if (f == NULL)
    {
        fprintf(stderr, "Failed to open worker credentials file %s: %s\n",
                path, strerror(errno));
        return 0;
    }

    char line[MAX_CRED_LINE];
    int line_num = 0;

    while (fgets(line, sizeof(line), f) != NULL)
    {
        line_num++;

        /* Trim trailing newline */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n')
        {
            line[len - 1] = '\0';
        }

        /* Skip empty lines and comments */
        if (line[0] == '\0' || line[0] == '#')
        {
            continue;
        }

        /* Parse worker_id:secret */
        char *colon = strchr(line, ':');

        if (colon == NULL)
        {
            fprintf(stderr, "Worker credentials file %s line %d: "
                    "missing ':' separator\n", path, line_num);
            fclose(f);
            return 0;
        }

        *colon = '\0';
        char *id_str = line;
        char *secret = colon + 1;

        if (id_str[0] == '\0' || secret[0] == '\0')
        {
            fprintf(stderr, "Worker credentials file %s line %d: "
                    "empty worker_id or secret\n", path, line_num);
            fclose(f);
            return 0;
        }

        char *endptr = NULL;
        long worker_id = strtol(id_str, &endptr, 10);

        if (endptr == id_str || *endptr != '\0' || worker_id <= 0)
        {
            fprintf(stderr, "Worker credentials file %s line %d: "
                    "invalid worker_id '%s'\n", path, line_num, id_str);
            fclose(f);
            return 0;
        }

        if (strlen(secret) < AUTH_CONFIG_MIN_SECRET_BYTES)
        {
            fprintf(stderr, "Worker credentials file %s line %d: "
                    "secret too short (min %d bytes)\n",
                    path, line_num, AUTH_CONFIG_MIN_SECRET_BYTES);
            fclose(f);
            return 0;
        }

        if (worker_cred_count >= MAX_WORKER_CREDENTIALS)
        {
            fprintf(stderr, "Worker credentials file %s: "
                    "too many entries (max %d)\n",
                    path, MAX_WORKER_CREDENTIALS);
            fclose(f);
            return 0;
        }

        worker_cred_ids[worker_cred_count] = (int)worker_id;
        if (!copy_secret(secret, worker_cred_secrets[worker_cred_count],
                         sizeof(worker_cred_secrets[0])))
        {
            fprintf(stderr, "Worker credentials file %s line %d: "
                    "secret too long\n", path, line_num);
            fclose(f);
            return 0;
        }

        worker_cred_count++;
    }

    fclose(f);
    return 1;
}

int auth_config_init(void)
{
    if (initialized)
    {
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

    if (!load_worker_credentials())
    {
        return 0;
    }

    return 1;
}

int auth_config_is_secure(void)
{
    if (!initialized)
    {
        auth_config_init();
    }

    return secure_mode;
}

const char *auth_config_worker_secret(int worker_id)
{
    if (!initialized)
    {
        auth_config_init();
    }

    if (!secure_mode)
    {
        return NULL;
    }

    if (worker_id > 0)
    {
        for (int i = 0; i < worker_cred_count; i++)
        {
            if (worker_cred_ids[i] == worker_id)
            {
                return worker_cred_secrets[i];
            }
        }
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