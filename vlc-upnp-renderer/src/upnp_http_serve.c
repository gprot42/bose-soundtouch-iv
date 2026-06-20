/*
 * upnp_http_serve.c — single-file HTTP server for local media
 */
#include "upnp_http_serve.h"
#include "upnp_common.h"

#include <arpa/inet.h>
#include <libgen.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

struct upnp_http_server
{
    char *file_path;
    int listen_fd;
    uint16_t port;
    pthread_t thread;
    volatile int running;
};

static const char *guess_mime(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (ext == NULL)
        return "application/octet-stream";
    if (strcasecmp(ext, ".mp3") == 0) return "audio/mpeg";
    if (strcasecmp(ext, ".flac") == 0) return "audio/flac";
    if (strcasecmp(ext, ".m4a") == 0) return "audio/mp4";
    if (strcasecmp(ext, ".aac") == 0) return "audio/aac";
    if (strcasecmp(ext, ".wav") == 0) return "audio/wav";
    if (strcasecmp(ext, ".ogg") == 0) return "audio/ogg";
    if (strcasecmp(ext, ".opus") == 0) return "audio/opus";
    return "application/octet-stream";
}

static void serve_client(int cfd, const char *file_path)
{
    char req[1024];
    ssize_t n = recv(cfd, req, sizeof(req) - 1, 0);
    if (n <= 0)
    {
        close(cfd);
        return;
    }
    req[n] = '\0';

    struct stat st;
    if (stat(file_path, &st) != 0 || !S_ISREG(st.st_mode))
    {
        const char *err = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
        send(cfd, err, strlen(err), 0);
        close(cfd);
        return;
    }

    char hdr[512];
    int hdrlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %lld\r\n"
        "Accept-Ranges: bytes\r\n"
        "Connection: close\r\n"
        "\r\n",
        guess_mime(file_path), (long long)st.st_size);
    if (hdrlen > 0)
        send(cfd, hdr, (size_t)hdrlen, 0);

    FILE *fh = fopen(file_path, "rb");
    if (fh == NULL)
    {
        close(cfd);
        return;
    }

    char buf[65536];
    size_t rd;
    while ((rd = fread(buf, 1, sizeof(buf), fh)) > 0)
    {
        if (send(cfd, buf, rd, 0) < 0)
            break;
    }

    fclose(fh);
    close(cfd);
}

static void *server_thread(void *arg)
{
    upnp_http_server_t *srv = arg;

    while (srv->running)
    {
        struct sockaddr_in cli;
        socklen_t clen = sizeof(cli);
        int cfd = accept(srv->listen_fd, (struct sockaddr *)&cli, &clen);
        if (cfd < 0)
        {
            if (!srv->running)
                break;
            continue;
        }
        serve_client(cfd, srv->file_path);
    }

    return NULL;
}

upnp_http_server_t *upnp_http_serve_start(const char *file_path,
                                          const char *dest_host,
                                          char *url_out, size_t urllen)
{
    if (file_path == NULL || dest_host == NULL || url_out == NULL)
        return NULL;

    upnp_http_server_t *srv = calloc(1, sizeof(*srv));
    if (srv == NULL)
        return NULL;

    srv->file_path = strdup(file_path);
    if (srv->file_path == NULL)
    {
        free(srv);
        return NULL;
    }

    srv->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv->listen_fd < 0)
        goto error;

    int yes = 1;
    setsockopt(srv->listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(srv->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
        goto error;

    if (listen(srv->listen_fd, 4) != 0)
        goto error;

    socklen_t alen = sizeof(addr);
    if (getsockname(srv->listen_fd, (struct sockaddr *)&addr, &alen) != 0)
        goto error;
    srv->port = ntohs(addr.sin_port);

    char local_ip[64];
    if (upnp_local_ip_toward(dest_host, local_ip, sizeof(local_ip)) != 0)
        strncpy(local_ip, "127.0.0.1", sizeof(local_ip) - 1);

    char *fname = strdup(file_path);
    if (fname == NULL)
        goto error;
    char *base = basename(fname);
    char *enc = upnp_url_encode_path(base);
    free(fname);
    if (enc == NULL)
        goto error;

    snprintf(url_out, urllen, "http://%s:%u/%s", local_ip, srv->port, enc);
    free(enc);

    srv->running = 1;
    if (pthread_create(&srv->thread, NULL, server_thread, srv) != 0)
        goto error;

    return srv;

error:
    if (srv->listen_fd >= 0)
        close(srv->listen_fd);
    free(srv->file_path);
    free(srv);
    return NULL;
}

void upnp_http_serve_stop(upnp_http_server_t *srv)
{
    if (srv == NULL)
        return;

    srv->running = 0;
    if (srv->listen_fd >= 0)
    {
        shutdown(srv->listen_fd, SHUT_RDWR);
        close(srv->listen_fd);
        srv->listen_fd = -1;
    }
    pthread_join(srv->thread, NULL);
    free(srv->file_path);
    free(srv);
}