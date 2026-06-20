/*
 * upnp_ssdp.c — SSDP M-SEARCH discovery
 */
#include "upnp_ssdp.h"
#include "upnp_common.h"

#include <stdbool.h>

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define SSDP_POLL_MS       200
#define SSDP_IDLE_GRACE_MS 500

int upnp_ssdp_parse_location(const char *response, char *location, size_t loclen)
{
    if (response == NULL || location == NULL || loclen == 0)
        return -1;

    const char *p = response;
    while (*p != '\0')
    {
        if (strncasecmp(p, "LOCATION:", 9) == 0)
        {
            p += 9;
            while (*p == ' ' || *p == '\t')
                p++;

            size_t i = 0;
            while (p[i] != '\0' && p[i] != '\r' && p[i] != '\n' && i + 1 < loclen)
            {
                location[i] = p[i];
                i++;
            }
            location[i] = '\0';
            return i > 0 ? 0 : -1;
        }

        const char *eol = strchr(p, '\n');
        if (eol == NULL)
            break;
        p = eol + 1;
    }

    return -1;
}

static bool header_has_token(const char *response, const char *header,
                             const char *token)
{
    const char *p = response;
    size_t hlen = strlen(header);

    while (*p != '\0')
    {
        if (strncasecmp(p, header, hlen) == 0)
        {
            const char *line = p + hlen;
            while (*line == ' ' || *line == '\t' || *line == ':')
                line++;

            const char *eol = strchr(line, '\n');
            size_t len = eol ? (size_t)(eol - line) : strlen(line);
            if (len >= strlen(token))
            {
                for (size_t i = 0; i + strlen(token) <= len; i++)
                {
                    if (strncasecmp(line + i, token, strlen(token)) == 0)
                        return true;
                }
            }
        }

        const char *eol = strchr(p, '\n');
        if (eol == NULL)
            break;
        p = eol + 1;
    }

    return false;
}

int upnp_ssdp_is_renderer_response(const char *response)
{
    return header_has_token(response, "ST", "MediaRenderer") ||
           header_has_token(response, "USN", "MediaRenderer");
}

static long timeval_diff_ms(const struct timeval *a, const struct timeval *b)
{
    return (a->tv_sec - b->tv_sec) * 1000L +
           (a->tv_usec - b->tv_usec) / 1000L;
}

int upnp_ssdp_discover(upnp_ssdp_cb cb, void *userdata, int timeout_sec)
{
    if (cb == NULL || timeout_sec <= 0)
        return -1;

    char msg[512];
    int len = snprintf(msg, sizeof(msg),
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: %s:%d\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: %d\r\n"
        "ST: %s\r\n"
        "\r\n",
        UPNP_SSDP_ADDR, UPNP_SSDP_PORT, UPNP_SSDP_MX, UPNP_ST_RENDERER);
    if (len <= 0 || (size_t)len >= sizeof(msg))
        return -1;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0)
        return -1;

    int on = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = 0,
    };
    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0)
    {
        close(sock);
        return -2;
    }

    int ttl = 4;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    char local_ip[64];
    if (upnp_local_ip_toward(UPNP_SSDP_ADDR, local_ip, sizeof(local_ip)) == 0)
    {
        struct in_addr iface;
        if (inet_pton(AF_INET, local_ip, &iface) == 1)
            setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, &iface, sizeof(iface));
    }

    struct sockaddr_in mcast = {
        .sin_family = AF_INET,
        .sin_port = htons(UPNP_SSDP_PORT),
    };
    inet_pton(AF_INET, UPNP_SSDP_ADDR, &mcast.sin_addr);

    if (sendto(sock, msg, (size_t)len, 0, (struct sockaddr *)&mcast, sizeof(mcast)) < 0)
    {
        close(sock);
        return -3;
    }

    char seen[64][512];
    int seen_count = 0;
    char buf[4096];
    char location[1024];

    struct timeval start, now, last_rx = { 0, 0 };
    gettimeofday(&start, NULL);

    for (;;)
    {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);

        struct timeval poll_tv = {
            .tv_sec = SSDP_POLL_MS / 1000,
            .tv_usec = (SSDP_POLL_MS % 1000) * 1000L,
        };

        int sel = select(sock + 1, &rfds, NULL, NULL, &poll_tv);
        if (sel > 0 && FD_ISSET(sock, &rfds))
        {
            ssize_t n = recvfrom(sock, buf, sizeof(buf) - 1, 0, NULL, NULL);
            if (n < 0)
            {
                if (errno == EINTR)
                    continue;
                break;
            }
            if (n == 0)
                break;

            buf[n] = '\0';
            if (!upnp_ssdp_is_renderer_response(buf))
                continue; /* skip TVs, servers, etc. that answer every M-SEARCH */
            if (upnp_ssdp_parse_location(buf, location, sizeof(location)) != 0)
                continue;

            bool duplicate = false;
            for (int i = 0; i < seen_count; i++)
            {
                if (strcmp(seen[i], location) == 0)
                {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate)
                continue;

            if (seen_count < 64)
                strncpy(seen[seen_count++], location, sizeof(seen[0]) - 1);

            gettimeofday(&last_rx, NULL);
            cb(location, userdata);
        }
        else if (sel < 0 && errno != EINTR)
        {
            break;
        }

        gettimeofday(&now, NULL);
        long elapsed_ms = timeval_diff_ms(&now, &start);
        if (elapsed_ms >= timeout_sec * 1000L)
            break;

        if (seen_count > 0 && last_rx.tv_sec != 0 &&
            timeval_diff_ms(&now, &last_rx) >= SSDP_IDLE_GRACE_MS)
            break;
    }

    close(sock);
    return 0;
}