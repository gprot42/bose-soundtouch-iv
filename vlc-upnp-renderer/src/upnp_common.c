/*
 * upnp_common.c — shared helpers and device registry
 */
#include "upnp_common.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct registry_entry
{
    char *key;
    upnp_device_t dev;
    struct registry_entry *next;
} registry_entry_t;

static registry_entry_t *g_registry;
static pthread_mutex_t g_registry_lock = PTHREAD_MUTEX_INITIALIZER;

void upnp_device_clear(upnp_device_t *dev)
{
    if (dev == NULL)
        return;
    free(dev->location);
    free(dev->friendly_name);
    free(dev->av_control);
    free(dev->rc_control);
    free(dev->host);
    memset(dev, 0, sizeof(*dev));
}

int upnp_device_copy(upnp_device_t *dst, const upnp_device_t *src)
{
    upnp_device_clear(dst);
    if (src->location && !(dst->location = strdup(src->location)))
        return -1;
    if (src->friendly_name && !(dst->friendly_name = strdup(src->friendly_name)))
        goto error;
    if (src->av_control && !(dst->av_control = strdup(src->av_control)))
        goto error;
    if (src->rc_control && !(dst->rc_control = strdup(src->rc_control)))
        goto error;
    if (src->host && !(dst->host = strdup(src->host)))
        goto error;
    dst->port = src->port;
    return 0;

error:
    upnp_device_clear(dst);
    return -1;
}

static char *make_registry_key(const char *host, uint16_t port)
{
    char *key = NULL;
    if (asprintf(&key, "%s:%u", host, port) < 0)
        return NULL;
    return key;
}

int upnp_registry_add(const upnp_device_t *dev)
{
    if (dev == NULL || dev->host == NULL)
        return -1;

    char *key = make_registry_key(dev->host, dev->port);
    if (key == NULL)
        return -1;

    pthread_mutex_lock(&g_registry_lock);

    for (registry_entry_t *e = g_registry; e != NULL; e = e->next)
    {
        if (strcmp(e->key, key) == 0)
        {
            upnp_device_clear(&e->dev);
            if (upnp_device_copy(&e->dev, dev) != 0)
            {
                pthread_mutex_unlock(&g_registry_lock);
                free(key);
                return -1;
            }
            pthread_mutex_unlock(&g_registry_lock);
            free(key);
            return 0;
        }
    }

    registry_entry_t *entry = calloc(1, sizeof(*entry));
    if (entry == NULL)
    {
        pthread_mutex_unlock(&g_registry_lock);
        free(key);
        return -1;
    }

    entry->key = key;
    if (upnp_device_copy(&entry->dev, dev) != 0)
    {
        free(entry->key);
        free(entry);
        pthread_mutex_unlock(&g_registry_lock);
        return -1;
    }

    entry->next = g_registry;
    g_registry = entry;
    pthread_mutex_unlock(&g_registry_lock);
    return 0;
}

int upnp_registry_remove(const char *host, uint16_t port)
{
    char *key = make_registry_key(host, port);
    if (key == NULL)
        return -1;

    pthread_mutex_lock(&g_registry_lock);

    registry_entry_t **pp = &g_registry;
    while (*pp != NULL)
    {
        if (strcmp((*pp)->key, key) == 0)
        {
            registry_entry_t *rm = *pp;
            *pp = rm->next;
            upnp_device_clear(&rm->dev);
            free(rm->key);
            free(rm);
            pthread_mutex_unlock(&g_registry_lock);
            free(key);
            return 0;
        }
        pp = &(*pp)->next;
    }

    pthread_mutex_unlock(&g_registry_lock);
    free(key);
    return -1;
}

int upnp_registry_lookup(const char *host, uint16_t port, upnp_device_t *out)
{
    char *key = make_registry_key(host, port);
    if (key == NULL)
        return -1;

    pthread_mutex_lock(&g_registry_lock);

    for (registry_entry_t *e = g_registry; e != NULL; e = e->next)
    {
        if (strcmp(e->key, key) == 0)
        {
            int ret = upnp_device_copy(out, &e->dev);
            pthread_mutex_unlock(&g_registry_lock);
            free(key);
            return ret;
        }
    }

    pthread_mutex_unlock(&g_registry_lock);
    free(key);
    return -1;
}

void upnp_registry_clear(void)
{
    pthread_mutex_lock(&g_registry_lock);
    while (g_registry != NULL)
    {
        registry_entry_t *e = g_registry;
        g_registry = e->next;
        upnp_device_clear(&e->dev);
        free(e->key);
        free(e);
    }
    pthread_mutex_unlock(&g_registry_lock);
}

int upnp_local_ip_toward(const char *dest_host, char *buf, size_t buflen)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_in dst = { .sin_family = AF_INET };
    if (inet_pton(AF_INET, dest_host, &dst.sin_addr) != 1)
    {
        struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM };
        struct addrinfo *res = NULL;
        if (getaddrinfo(dest_host, NULL, &hints, &res) != 0 || res == NULL)
        {
            close(fd);
            return -1;
        }
        struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
        dst.sin_addr = sa->sin_addr;
        freeaddrinfo(res);
    }
    dst.sin_port = htons(1);

    if (connect(fd, (struct sockaddr *)&dst, sizeof(dst)) != 0)
    {
        close(fd);
        return -1;
    }

    struct sockaddr_in local;
    socklen_t len = sizeof(local);
    if (getsockname(fd, (struct sockaddr *)&local, &len) != 0)
    {
        close(fd);
        return -1;
    }

    const char *ip = inet_ntoa(local.sin_addr);
    if (ip == NULL)
    {
        close(fd);
        return -1;
    }

    strncpy(buf, ip, buflen - 1);
    buf[buflen - 1] = '\0';
    close(fd);
    return 0;
}

int upnp_parse_hms_duration(const char *hms, int64_t *ticks_out)
{
    if (hms == NULL || ticks_out == NULL || hms[0] == '\0')
        return -1;

    int hour = 0, min = 0, sec = 0;
    int n = sscanf(hms, "%d:%d:%d", &hour, &min, &sec);
    if (n == 2)
    {
        sec = min;
        min = hour;
        hour = 0;
    }
    else if (n != 3)
        return -1;

    *ticks_out = ((int64_t)hour * 3600 + min * 60 + sec) * 1000000LL;
    return 0;
}

int upnp_url_decode(const char *in, char *out, size_t outlen)
{
    if (in == NULL || out == NULL || outlen == 0)
        return -1;

    size_t j = 0;
    for (size_t i = 0; in[i] != '\0'; i++)
    {
        unsigned char c = (unsigned char)in[i];
        if (c == '%' && isxdigit((unsigned char)in[i + 1])
                   && isxdigit((unsigned char)in[i + 2]))
        {
            char hex[3] = { in[i + 1], in[i + 2], '\0' };
            c = (unsigned char)strtoul(hex, NULL, 16);
            i += 2;
        }

        if (j + 1 >= outlen)
            return -1;
        out[j++] = (char)c;
    }

    out[j] = '\0';
    return 0;
}

char *upnp_url_encode_path(const char *path)
{
    size_t inlen = strlen(path);
    char *out = malloc(inlen * 3 + 1);
    if (out == NULL)
        return NULL;

    static const char *safe = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.~/";
    char *p = out;

    for (size_t i = 0; i < inlen; i++)
    {
        unsigned char c = (unsigned char)path[i];
        if (strchr(safe, c) != NULL)
            *p++ = c;
        else
            p += sprintf(p, "%%%02X", c);
    }
    *p = '\0';
    return out;
}