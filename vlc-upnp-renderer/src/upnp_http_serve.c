/*
 * upnp_http_serve.c — single-file HTTP server for local media
 */
#include "upnp_http_serve.h"
#include "upnp_common.h"

#include <arpa/inet.h>
#include <signal.h>
#include <stdbool.h>
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

static bool parse_range(const char *req, off_t file_size,
                        off_t *start_out, off_t *end_out)
{
    const char *hdr = strstr(req, "Range:");
    if (hdr == NULL)
        hdr = strstr(req, "range:");
    if (hdr == NULL)
        return false;

    const char *bytes = strstr(hdr, "bytes=");
    if (bytes == NULL)
        return false;

    if (file_size <= 0)
        return false;

    char spec[128];
    const char *spec_in = bytes + 6;
    size_t speclen = strcspn(spec_in, "\r\n");
    if (speclen == 0 || speclen >= sizeof(spec))
        return false;
    memcpy(spec, spec_in, speclen);
    spec[speclen] = '\0';

    char *dash = strchr(spec, '-');
    if (dash == NULL)
        return false;

    off_t start = 0;
    off_t end = file_size - 1;

    if (dash == spec)
    {
        long long suffix = 0;
        if (sscanf(dash + 1, "%lld", &suffix) != 1 || suffix <= 0)
            return false;
        start = file_size > suffix ? file_size - suffix : 0;
    }
    else
    {
        long long start_val = 0;
        long long end_val = 0;
        bool has_end = false;

        *dash = '\0';
        if (sscanf(spec, "%lld", &start_val) != 1 || start_val < 0)
            return false;

        if (dash[1] != '\0' && sscanf(dash + 1, "%lld", &end_val) == 1)
            has_end = true;

        start = (off_t)start_val;
        if (has_end && end_val >= 0)
            end = (off_t)end_val;
    }

    if (start >= file_size)
        return false;
    if (end < start)
        return false;
    if (end >= file_size)
        end = file_size - 1;

    *start_out = start;
    *end_out = end;
    return true;
}

static bool request_is_head(const char *req)
{
    return strncmp(req, "HEAD ", 5) == 0;
}

static void serve_client(int cfd, const char *file_path, const char *req)
{
    struct stat st;
    if (stat(file_path, &st) != 0 || !S_ISREG(st.st_mode))
    {
        const char *err = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
        send(cfd, err, strlen(err), 0);
        close(cfd);
        return;
    }

    const bool is_head = request_is_head(req);
    off_t range_start = 0;
    off_t range_end = st.st_size > 0 ? st.st_size - 1 : 0;
    const bool has_range = parse_range(req, st.st_size, &range_start, &range_end);
    off_t body_len = has_range ? (range_end - range_start + 1) : st.st_size;

    char hdr[512];
    int hdrlen;
    if (has_range)
    {
        hdrlen = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 206 Partial Content\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %lld\r\n"
            "Content-Range: bytes %lld-%lld/%lld\r\n"
            "Accept-Ranges: bytes\r\n"
            "Connection: close\r\n"
            "\r\n",
            guess_mime(file_path), (long long)body_len,
            (long long)range_start, (long long)range_end, (long long)st.st_size);
    }
    else
    {
        hdrlen = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %lld\r\n"
            "Accept-Ranges: bytes\r\n"
            "Connection: close\r\n"
            "\r\n",
            guess_mime(file_path), (long long)body_len);
    }

    if (hdrlen > 0)
        send(cfd, hdr, (size_t)hdrlen, 0);

    if (is_head)
    {
        close(cfd);
        return;
    }

    FILE *fh = fopen(file_path, "rb");
    if (fh == NULL)
    {
        close(cfd);
        return;
    }

    if (has_range && fseeko(fh, range_start, SEEK_SET) != 0)
    {
        fclose(fh);
        close(cfd);
        return;
    }

    char buf[65536];
    off_t remaining = body_len;
    while (remaining > 0)
    {
        size_t chunk = remaining > (off_t)sizeof(buf) ? sizeof(buf) : (size_t)remaining;
        size_t rd = fread(buf, 1, chunk, fh);
        if (rd == 0)
            break;
        if (send(cfd, buf, rd, 0) < 0)
            break;
        remaining -= (off_t)rd;
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

        char req[2048];
        ssize_t n = recv(cfd, req, sizeof(req) - 1, 0);
        if (n <= 0)
        {
            close(cfd);
            continue;
        }
        req[n] = '\0';
        serve_client(cfd, srv->file_path, req);
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

    if (listen(srv->listen_fd, 8) != 0)
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

    static bool sigpipe_ignored;
    if (!sigpipe_ignored)
    {
        signal(SIGPIPE, SIG_IGN);
        sigpipe_ignored = true;
    }

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