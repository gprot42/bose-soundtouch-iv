/*
 * upnp_http_serve.h — single-file HTTP server for local media
 */
#ifndef UPNP_HTTP_SERVE_H
#define UPNP_HTTP_SERVE_H

#include <stddef.h>
#include <stdint.h>

typedef struct upnp_http_server upnp_http_server_t;

upnp_http_server_t *upnp_http_serve_start(const char *file_path,
                                          const char *dest_host,
                                          char *url_out, size_t urllen);
void upnp_http_serve_stop(upnp_http_server_t *srv);

#endif /* UPNP_HTTP_SERVE_H */