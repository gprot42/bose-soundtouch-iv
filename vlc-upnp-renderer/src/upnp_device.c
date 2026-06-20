/*
 * upnp_device.c — fetch and parse UPnP device description XML
 */
#include "upnp_device.h"
#include "upnp_common.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

static char *xml_dup_between(const char *xml, const char *tag)
{
    char open[128];
    snprintf(open, sizeof(open), "<%s", tag);

    const char *start = strstr(xml, open);
    if (start == NULL)
        return NULL;

    start = strchr(start, '>');
    if (start == NULL)
        return NULL;
    start++;

    char close[64];
    snprintf(close, sizeof(close), "</%s>", tag);
    const char *end = strstr(start, close);
    if (end == NULL)
        return NULL;

    size_t len = (size_t)(end - start);
    char *out = malloc(len + 1);
    if (out == NULL)
        return NULL;

    memcpy(out, start, len);
    out[len] = '\0';

    /* Trim whitespace */
    while (len > 0 && isspace((unsigned char)out[len - 1]))
        out[--len] = '\0';

    return out;
}

static void strip_xml_namespaces(char *xml)
{
    for (char *p = xml; *p != '\0'; p++)
    {
        if (*p == '<')
        {
            char *colon = strchr(p + 1, ':');
            char *gt = strchr(p + 1, '>');
            if (colon != NULL && gt != NULL && colon < gt)
            {
                size_t tail = strlen(colon + 1) + 1;
                memmove(p + 1, colon + 1, tail);
            }
        }
    }
}

static int parse_host_port(const char *location, char **host, uint16_t *port)
{
    const char *p = strstr(location, "://");
    if (p == NULL)
        return -1;
    p += 3;

    const char *slash = strchr(p, '/');
    size_t hostlen = slash ? (size_t)(slash - p) : strlen(p);

    char hostport[256];
    if (hostlen >= sizeof(hostport))
        return -1;
    memcpy(hostport, p, hostlen);
    hostport[hostlen] = '\0';

    char *colon = strchr(hostport, ':');
    if (colon != NULL)
    {
        *colon = '\0';
        *port = (uint16_t)atoi(colon + 1);
    }
    else
    {
        *port = UPNP_DEFAULT_PORT;
    }

    *host = strdup(hostport);
    return *host != NULL ? 0 : -1;
}

static char *make_control_url(const char *location, const char *control_path)
{
    const char *p = strstr(location, "://");
    if (p == NULL)
        return NULL;
    p += 3;
    const char *slash = strchr(p, '/');
    if (slash == NULL)
        return NULL;

    size_t prefix = (size_t)(slash - location);
    char *url = NULL;

    const char *path = control_path;
    if (path[0] != '/')
    {
        if (asprintf(&url, "%.*s/%s", (int)prefix, location, path) < 0)
            return NULL;
    }
    else
    {
        if (asprintf(&url, "%.*s%s", (int)prefix, location, path) < 0)
            return NULL;
    }

    return url;
}

int upnp_device_parse_xml(const char *xml, size_t xmllen, const char *location,
                          upnp_device_t *dev)
{
    if (xml == NULL || location == NULL || dev == NULL)
        return -1;

    memset(dev, 0, sizeof(*dev));

    char *copy = malloc(xmllen + 1);
    if (copy == NULL)
        return -1;
    memcpy(copy, xml, xmllen);
    copy[xmllen] = '\0';
    strip_xml_namespaces(copy);

    char *friendly = xml_dup_between(copy, "friendlyName");
    char *av_path = NULL;
    char *rc_path = NULL;

    const char *svc = copy;
    while ((svc = strstr(svc, "<service")) != NULL)
    {
        const char *svc_end = strstr(svc, "</service>");
        if (svc_end == NULL)
            break;

        char block[4096];
        size_t blen = (size_t)(svc_end - svc);
        if (blen >= sizeof(block))
            blen = sizeof(block) - 1;
        memcpy(block, svc, blen);
        block[blen] = '\0';

        char *stype = xml_dup_between(block, "serviceType");
        char *sctrl = xml_dup_between(block, "controlURL");
        if (stype != NULL && sctrl != NULL)
        {
            if (strstr(stype, "AVTransport") != NULL)
            {
                free(av_path);
                av_path = sctrl;
                sctrl = NULL;
            }
            else if (strstr(stype, "RenderingControl") != NULL)
            {
                free(rc_path);
                rc_path = sctrl;
                sctrl = NULL;
            }
        }
        free(stype);
        free(sctrl);
        svc = svc_end + 10;
    }

    if (av_path == NULL)
    {
        free(copy);
        free(friendly);
        free(av_path);
        free(rc_path);
        return -1;
    }

    upnp_device_clear(dev);
    dev->location = strdup(location);
    dev->friendly_name = friendly ? friendly : strdup(location);
    dev->av_control = make_control_url(location, av_path);
    if (rc_path != NULL)
        dev->rc_control = make_control_url(location, rc_path);

    if (parse_host_port(location, &dev->host, &dev->port) != 0)
    {
        upnp_device_clear(dev);
        free(copy);
        free(av_path);
        free(rc_path);
        return -1;
    }

    free(copy);
    free(av_path);
    free(rc_path);

    return (dev->location && dev->friendly_name && dev->av_control) ? 0 : -1;
}

static int http_get(const char *url, char **body, size_t *bodylen)
{
    const char *p = strstr(url, "://");
    if (p == NULL)
        return -1;
    p += 3;

    const char *path = strchr(p, '/');
    if (path == NULL)
        return -1;

    char hostport[256];
    size_t hlen = (size_t)(path - p);
    if (hlen >= sizeof(hostport))
        return -1;
    memcpy(hostport, p, hlen);
    hostport[hlen] = '\0';

    char *host = hostport;
    char portstr[16] = "80";
    char *colon = strchr(hostport, ':');
    if (colon != NULL)
    {
        *colon = '\0';
        snprintf(portstr, sizeof(portstr), "%s", colon + 1);
    }

    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || res == NULL)
        return -1;

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0)
    {
        freeaddrinfo(res);
        return -1;
    }

    struct timeval tv = { .tv_sec = UPNP_DEVICE_IO_TIMEOUT_SEC, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int cr = connect(fd, res->ai_addr, res->ai_addrlen);
    if (cr != 0 && errno != EINPROGRESS)
    {
        close(fd);
        freeaddrinfo(res);
        return -1;
    }
    if (cr != 0)
    {
        struct timeval ctv = {
            .tv_sec = UPNP_DEVICE_CONNECT_TIMEOUT_SEC,
            .tv_usec = 0,
        };
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        if (select(fd + 1, NULL, &wfds, NULL, &ctv) <= 0)
        {
            close(fd);
            freeaddrinfo(res);
            return -1;
        }

        int so_error = 0;
        socklen_t solen = sizeof(so_error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &solen) != 0 ||
            so_error != 0)
        {
            close(fd);
            freeaddrinfo(res);
            return -1;
        }
    }

    if (flags >= 0)
        fcntl(fd, F_SETFL, flags);
    freeaddrinfo(res);

    char req[2048];
    int reqlen = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%s\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host, portstr);
    if (reqlen <= 0 || send(fd, req, (size_t)reqlen, 0) < 0)
    {
        close(fd);
        return -1;
    }

    size_t cap = 8192;
    size_t len = 0;
    char *buf = malloc(cap);
    if (buf == NULL)
    {
        close(fd);
        return -1;
    }

    for (;;)
    {
        if (len + 4096 > cap)
        {
            cap *= 2;
            char *nbuf = realloc(buf, cap);
            if (nbuf == NULL)
            {
                free(buf);
                close(fd);
                return -1;
            }
            buf = nbuf;
        }

        ssize_t n = recv(fd, buf + len, cap - len, 0);
        if (n <= 0)
            break;
        len += (size_t)n;
    }
    close(fd);

    char *body_start = strstr(buf, "\r\n\r\n");
    if (body_start == NULL)
    {
        free(buf);
        return -1;
    }
    body_start += 4;
    size_t blen = len - (size_t)(body_start - buf);
    char *out = malloc(blen + 1);
    if (out == NULL)
    {
        free(buf);
        return -1;
    }
    memcpy(out, body_start, blen);
    out[blen] = '\0';
    free(buf);

    *body = out;
    *bodylen = blen;
    return 0;
}

int upnp_device_fetch(const char *location, upnp_device_t *dev)
{
    char *body = NULL;
    size_t bodylen = 0;

    if (http_get(location, &body, &bodylen) != 0)
        return -1;

    int ret = upnp_device_parse_xml(body, bodylen, location, dev);
    free(body);
    return ret;
}