/*
 * upnp_common.h — shared UPnP/DLNA renderer types and device registry
 */
#ifndef UPNP_COMMON_H
#define UPNP_COMMON_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define UPNP_SSDP_ADDR          "239.255.255.250"
#define UPNP_SSDP_PORT          1900
#define UPNP_SSDP_MX            1
#define UPNP_DEVICE_CONNECT_TIMEOUT_SEC 3
#define UPNP_DEVICE_IO_TIMEOUT_SEC      3
#define UPNP_ST_RENDERER        "urn:schemas-upnp-org:device:MediaRenderer:1"

#define UPNP_AV_TRANSPORT_SVC   "urn:schemas-upnp-org:service:AVTransport:1"
#define UPNP_RENDERING_CTRL_SVC "urn:schemas-upnp-org:service:RenderingControl:1"

#define UPNP_DEFAULT_PORT       8091

typedef struct upnp_device
{
    char *location;
    char *friendly_name;
    char *av_control;
    char *rc_control;
    char *host;
    uint16_t port;
} upnp_device_t;

void upnp_device_clear(upnp_device_t *dev);
int upnp_device_copy(upnp_device_t *dst, const upnp_device_t *src);

/* Registry keyed by "host:port" — populated by renderer discovery */
int upnp_registry_add(const upnp_device_t *dev);
int upnp_registry_remove(const char *host, uint16_t port);
int upnp_registry_lookup(const char *host, uint16_t port, upnp_device_t *out);
void upnp_registry_clear(void);

/* Pick local IP routed toward dest (UDP connect trick) */
int upnp_local_ip_toward(const char *dest_host, char *buf, size_t buflen);

/* URL-encode path component for HTTP serve */
char *upnp_url_encode_path(const char *path);

/* Percent-decode (%20, etc.) into out (NUL-terminated). Returns 0 on success. */
int upnp_url_decode(const char *in, char *out, size_t outlen);

/* Parse UPnP duration strings (e.g. "0:03:45") into VLC ticks. */
int upnp_parse_hms_duration(const char *hms, int64_t *ticks_out);

#endif /* UPNP_COMMON_H */