/*
 * upnp_ssdp.h — SSDP M-SEARCH discovery
 */
#ifndef UPNP_SSDP_H
#define UPNP_SSDP_H

#include <stddef.h>

typedef void (*upnp_ssdp_cb)(const char *location, void *userdata);

/* Send M-SEARCH and invoke cb for each unique LOCATION (blocking). */
int upnp_ssdp_discover(upnp_ssdp_cb cb, void *userdata, int timeout_sec);

/* Parse LOCATION from a single SSDP response buffer. Returns 0 on success. */
int upnp_ssdp_parse_location(const char *response, char *location, size_t loclen);

/* True when ST/USN identifies a MediaRenderer device. */
int upnp_ssdp_is_renderer_response(const char *response);

#endif /* UPNP_SSDP_H */