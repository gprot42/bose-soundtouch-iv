/*
 * upnp_soap.c — UPnP SOAP client
 */
#include "upnp_soap.h"
#include "upnp_common.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#define SOAP_CONNECT_TIMEOUT_SEC 3
#define SOAP_IO_TIMEOUT_SEC      5

static int soap_response_ok(const char *response)
{
    if (response == NULL || response[0] == '\0')
        return 0;

    if (strstr(response, "HTTP/1.1 200") == NULL &&
        strstr(response, "HTTP/1.0 200") == NULL)
        return -1;

    if (strstr(response, "<s:Fault") != NULL ||
        strstr(response, "<Fault") != NULL)
        return -1;

    return 0;
}

static int xml_escape_append(const char *in, char *out, size_t outlen)
{
    size_t j = 0;

    if (in == NULL || out == NULL || outlen == 0)
        return -1;

    for (size_t i = 0; in[i] != '\0'; i++)
    {
        const char *rep = NULL;
        char c = in[i];

        if (c == '&')
            rep = "&amp;";
        else if (c == '<')
            rep = "&lt;";
        else if (c == '>')
            rep = "&gt;";
        else if (c == '"')
            rep = "&quot;";
        else if (c == '\'')
            rep = "&apos;";

        if (rep != NULL)
        {
            size_t rlen = strlen(rep);
            if (j + rlen >= outlen)
                return -1;
            memcpy(out + j, rep, rlen);
            j += rlen;
        }
        else
        {
            if (j + 1 >= outlen)
                return -1;
            out[j++] = c;
        }
    }

    if (j >= outlen)
        return -1;
    out[j] = '\0';
    return 0;
}

static int http_post_soap(const char *url, const char *service, const char *action,
                          const char *body_xml, char *response, size_t resplen)
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

    char envelope[4096];
    int envlen = snprintf(envelope, sizeof(envelope),
        "<?xml version=\"1.0\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body>"
        "<u:%s xmlns:u=\"%s\">"
        "%s"
        "</u:%s>"
        "</s:Body>"
        "</s:Envelope>",
        action, service, body_xml ? body_xml : "", action);
    if (envlen <= 0 || (size_t)envlen >= sizeof(envelope))
        return -1;

    char soapaction[256];
    snprintf(soapaction, sizeof(soapaction), "\"%s#%s\"", service, action);

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

    static int sigpipe_ignored;
    if (!sigpipe_ignored)
    {
        signal(SIGPIPE, SIG_IGN);
        sigpipe_ignored = 1;
    }

    struct timeval tv = { .tv_sec = SOAP_IO_TIMEOUT_SEC, .tv_usec = 0 };
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
        struct timeval ctv = { .tv_sec = SOAP_CONNECT_TIMEOUT_SEC, .tv_usec = 0 };
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
        socklen_t so_len = sizeof(so_error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_len) != 0
         || so_error != 0)
        {
            close(fd);
            freeaddrinfo(res);
            return -1;
        }
    }
    freeaddrinfo(res);

    if (flags >= 0)
        fcntl(fd, F_SETFL, flags);

    char req[8192];
    int reqlen = snprintf(req, sizeof(req),
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%s\r\n"
        "Content-Type: text/xml; charset=\"utf-8\"\r\n"
        "SOAPAction: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        path, host, portstr, soapaction, envlen, envelope);
    if (reqlen <= 0 || send(fd, req, (size_t)reqlen, 0) < 0)
    {
        close(fd);
        return -1;
    }

    char buf[4096];
    size_t total = 0;
    for (;;)
    {
        if (total + 1 >= sizeof(buf))
            break;
        ssize_t n = recv(fd, buf + total, sizeof(buf) - 1 - total, 0);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }
        if (n == 0)
            break;
        total += (size_t)n;
    }
    buf[total] = '\0';
    close(fd);

    if (soap_response_ok(buf) != 0)
        return -1;

    if (response != NULL && resplen > 0)
    {
        strncpy(response, buf, resplen - 1);
        response[resplen - 1] = '\0';
    }

    return 0;
}

int upnp_soap_call(const char *control_url, const char *service_type,
                   const char *action, const char *args_xml,
                   char *response, size_t resplen)
{
    return http_post_soap(control_url, service_type, action, args_xml,
                          response, resplen);
}

int upnp_av_set_uri(const char *av_control, const char *media_url)
{
    char escaped[2048];
    char args[4096];

    if (media_url == NULL || xml_escape_append(media_url, escaped, sizeof(escaped)) != 0)
        return -1;

    snprintf(args, sizeof(args),
        "<InstanceID>0</InstanceID>"
        "<CurrentURI>%s</CurrentURI>"
        "<CurrentURIMetaData></CurrentURIMetaData>",
        escaped);
    return upnp_soap_call(av_control, UPNP_AV_TRANSPORT_SVC,
                          "SetAVTransportURI", args, NULL, 0);
}

int upnp_av_play(const char *av_control)
{
    return upnp_soap_call(av_control, UPNP_AV_TRANSPORT_SVC, "Play",
                          "<InstanceID>0</InstanceID><Speed>1</Speed>",
                          NULL, 0);
}

int upnp_av_stop(const char *av_control)
{
    return upnp_soap_call(av_control, UPNP_AV_TRANSPORT_SVC, "Stop",
                          "<InstanceID>0</InstanceID><Speed>1</Speed>",
                          NULL, 0);
}

int upnp_av_pause(const char *av_control)
{
    return upnp_soap_call(av_control, UPNP_AV_TRANSPORT_SVC, "Pause",
                          "<InstanceID>0</InstanceID>",
                          NULL, 0);
}

int upnp_av_seek_rel(const char *av_control, const char *target_hms)
{
    char args[512];
    snprintf(args, sizeof(args),
        "<InstanceID>0</InstanceID>"
        "<Unit>REL_TIME</Unit>"
        "<Target>%s</Target>",
        target_hms);
    return upnp_soap_call(av_control, UPNP_AV_TRANSPORT_SVC, "Seek", args,
                          NULL, 0);
}

static void soap_strip_namespaces(char *xml)
{
    for (char *p = xml; *p != '\0'; p++)
    {
        if (*p != '<' || p[1] == '/' || p[1] == '?')
            continue;

        char *colon = strchr(p + 1, ':');
        char *gt = strchr(p + 1, '>');
        if (colon != NULL && gt != NULL && colon < gt)
        {
            size_t tail = strlen(colon + 1) + 1;
            memmove(p + 1, colon + 1, tail);
        }
    }
}

static const char *soap_body_start(const char *response)
{
    if (response == NULL)
        return NULL;

    const char *body = strstr(response, "\r\n\r\n");
    return body != NULL ? body + 4 : response;
}

int upnp_soap_parse_tag(const char *xml, const char *tag, char *out, size_t outlen)
{
    if (xml == NULL || tag == NULL || out == NULL || outlen == 0)
        return -1;

    const char *body = soap_body_start(xml);
    char scratch[4096];
    const char *parse_xml = body;

    if (body != NULL)
    {
        strncpy(scratch, body, sizeof(scratch) - 1);
        scratch[sizeof(scratch) - 1] = '\0';
        soap_strip_namespaces(scratch);
        parse_xml = scratch;
    }

    char open[64], close[64];
    snprintf(open, sizeof(open), "<%s>", tag);
    snprintf(close, sizeof(close), "</%s>", tag);

    const char *start = strstr(parse_xml, open);
    if (start == NULL)
        return -1;
    start += strlen(open);
    const char *end = strstr(start, close);
    if (end == NULL)
        return -1;

    size_t len = (size_t)(end - start);
    if (len + 1 > outlen)
        return -1;

    memcpy(out, start, len);
    out[len] = '\0';
    return 0;
}

int upnp_av_get_transport_state(const char *av_control, char *state, size_t statelen)
{
    char response[4096];
    if (upnp_soap_call(av_control, UPNP_AV_TRANSPORT_SVC, "GetTransportInfo",
                       "<InstanceID>0</InstanceID>", response, sizeof(response)) != 0)
        return -1;

    return upnp_soap_parse_tag(response, "CurrentTransportState", state, statelen);
}

int upnp_av_get_position_info(const char *av_control, char *rel_time, size_t rel_len,
                              char *track_dur, size_t dur_len)
{
    char response[4096];
    if (upnp_soap_call(av_control, UPNP_AV_TRANSPORT_SVC, "GetPositionInfo",
                       "<InstanceID>0</InstanceID>", response, sizeof(response)) != 0)
        return -1;

    int ok = 0;
    if (rel_time != NULL && rel_len > 0
     && upnp_soap_parse_tag(response, "RelTime", rel_time, rel_len) == 0)
        ok = 1;
    if (track_dur != NULL && dur_len > 0
     && upnp_soap_parse_tag(response, "TrackDuration", track_dur, dur_len) == 0)
        ok = 1;
    return ok ? 0 : -1;
}

int upnp_rc_set_volume(const char *rc_control, int volume)
{
    if (rc_control == NULL)
        return -1;

    if (volume < 0)
        volume = 0;
    if (volume > 100)
        volume = 100;

    char args[256];
    snprintf(args, sizeof(args),
        "<InstanceID>0</InstanceID>"
        "<Channel>Master</Channel>"
        "<DesiredVolume>%d</DesiredVolume>",
        volume);
    return upnp_soap_call(rc_control, UPNP_RENDERING_CTRL_SVC, "SetVolume",
                          args, NULL, 0);
}