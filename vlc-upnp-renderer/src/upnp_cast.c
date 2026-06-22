/*
 * upnp_cast.c — cast session management
 */
#include "upnp_cast.h"
#include "upnp_device.h"
#include "upnp_soap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int upnp_cast_session_init(upnp_cast_session_t *s, const char *host,
                           uint16_t port, const char *location)
{
    if (s == NULL)
        return -1;

    memset(s, 0, sizeof(*s));

    if (location != NULL && location[0] != '\0')
    {
        if (upnp_device_fetch(location, &s->device) != 0)
            return -1;
    }
    else if (host != NULL)
    {
        if (upnp_registry_lookup(host, port, &s->device) != 0)
            return -1;
    }
    else
    {
        return -1;
    }

    return 0;
}

void upnp_cast_session_clear(upnp_cast_session_t *s)
{
    if (s == NULL)
        return;

    if (s->casting && s->device.av_control != NULL)
        upnp_av_stop(s->device.av_control);

    if (s->httpd != NULL)
        upnp_http_serve_stop(s->httpd);

    s->owner_input = NULL;
    upnp_device_clear(&s->device);
    memset(s, 0, sizeof(*s));
}

static int path_from_file_uri(const char *uri, char *out, size_t outlen)
{
    char raw[4096];

    if (uri == NULL)
        return -1;

    if (strncmp(uri, "file://", 7) == 0)
    {
        const char *p = uri + 7;
        if (p[0] == '/' && p[2] == ':') /* file:///C:/... */
            p++;
        strncpy(raw, p, sizeof(raw) - 1);
        raw[sizeof(raw) - 1] = '\0';
        return upnp_url_decode(raw, out, outlen);
    }

    if (uri[0] == '/')
    {
        strncpy(raw, uri, sizeof(raw) - 1);
        raw[sizeof(raw) - 1] = '\0';
        return upnp_url_decode(raw, out, outlen);
    }

    return -1;
}

int upnp_cast_start(upnp_cast_session_t *s, const char *source_path)
{
    if (s == NULL || source_path == NULL || s->device.av_control == NULL)
        return -1;

    char local_path[4096];
    const char *media_url = source_path;

    if (strncmp(source_path, "http://", 7) == 0 ||
        strncmp(source_path, "https://", 8) == 0)
    {
        strncpy(s->media_url, source_path, sizeof(s->media_url) - 1);
        media_url = s->media_url;
    }
    else if (path_from_file_uri(source_path, local_path, sizeof(local_path)) == 0)
    {
        if (access(local_path, R_OK) != 0)
            return -1;

        if (s->httpd != NULL)
            upnp_http_serve_stop(s->httpd);

        s->httpd = upnp_http_serve_start(local_path, s->device.host,
                                         s->media_url, sizeof(s->media_url));
        if (s->httpd == NULL)
            return -1;
        media_url = s->media_url;
    }
    else
    {
        return -1;
    }

    if (s->casting && s->device.av_control != NULL)
        upnp_av_stop(s->device.av_control);

    if (upnp_av_set_uri(s->device.av_control, media_url) != 0)
        return -1;

    usleep(500000);

    if (upnp_av_play(s->device.av_control) != 0)
        return -1;

    s->casting = true;
    strncpy(s->active_source, source_path, sizeof(s->active_source) - 1);
    s->active_source[sizeof(s->active_source) - 1] = '\0';
    return 0;
}

int upnp_cast_stop(upnp_cast_session_t *s)
{
    if (s == NULL)
        return -1;

    if (s->device.av_control != NULL
     && (s->casting || s->httpd != NULL))
        upnp_av_stop(s->device.av_control);

    s->casting = false;
    s->active_source[0] = '\0';
    return 0;
}

int upnp_cast_volume_percent(float vlc_volume)
{
    if (vlc_volume < 0.f)
        vlc_volume = 0.f;

    int level = (int)(vlc_volume * 100.f + 0.5f);
    if (level > 100)
        level = 100;
    return level;
}

int upnp_cast_sync_volume(upnp_cast_session_t *s, float vlc_volume, bool mute,
                          bool sync_mute, bool sync_volume)
{
    if (s == NULL || s->device.rc_control == NULL)
        return -1;

    if (sync_mute && upnp_rc_set_mute(s->device.rc_control, mute) != 0)
        return -1;

    if (mute || !sync_volume)
        return 0;

    return upnp_rc_set_volume(s->device.rc_control,
                              upnp_cast_volume_percent(vlc_volume));
}