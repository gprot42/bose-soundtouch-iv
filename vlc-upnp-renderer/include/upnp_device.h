/*
 * upnp_device.h — fetch and parse UPnP device description XML
 */
#ifndef UPNP_DEVICE_H
#define UPNP_DEVICE_H

#include "upnp_common.h"

/* Fetch device description from location URL. Returns 0 on success. */
int upnp_device_fetch(const char *location, upnp_device_t *dev);

/* Parse device description XML buffer (for tests). Returns 0 on success. */
int upnp_device_parse_xml(const char *xml, size_t xmllen, const char *location,
                          upnp_device_t *dev);

#endif /* UPNP_DEVICE_H */