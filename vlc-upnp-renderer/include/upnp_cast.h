/*
 * upnp_cast.h — shared cast session between stream_out and demux_filter
 * (both live in libupnp_cast_plugin)
 */
#ifndef UPNP_CAST_H
#define UPNP_CAST_H

#include "upnp_common.h"
#include "upnp_http_serve.h"

struct input_thread_t;

#define UPNP_CAST_VAR "upnp-cast-sys"

typedef struct upnp_cast_session
{
    upnp_device_t device;
    upnp_http_server_t *httpd;
    char media_url[2048];
    char active_source[4096];
    bool casting;
    bool enabled;
    struct input_thread_t *owner_input;
} upnp_cast_session_t;

int upnp_cast_session_init(upnp_cast_session_t *s, const char *host,
                           uint16_t port, const char *location);
void upnp_cast_session_clear(upnp_cast_session_t *s);

int upnp_cast_start(upnp_cast_session_t *s, const char *source_path);
int upnp_cast_stop(upnp_cast_session_t *s);

#endif /* UPNP_CAST_H */