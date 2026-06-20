/*
 * upnp_soap.h — UPnP SOAP client
 */
#ifndef UPNP_SOAP_H
#define UPNP_SOAP_H

#include <stddef.h>

int upnp_soap_call(const char *control_url, const char *service_type,
                   const char *action, const char *args_xml,
                   char *response, size_t resplen);

int upnp_av_set_uri(const char *av_control, const char *media_url);
int upnp_av_play(const char *av_control);
int upnp_av_stop(const char *av_control);
int upnp_av_pause(const char *av_control);
int upnp_av_seek_rel(const char *av_control, const char *target_hms);
int upnp_av_get_transport_state(const char *av_control, char *state, size_t statelen);
int upnp_av_get_position_info(const char *av_control, char *rel_time, size_t rel_len,
                              char *track_dur, size_t dur_len);

int upnp_rc_set_volume(const char *rc_control, int volume);

/* Parse a simple <tag>value</tag> element from a SOAP response body. */
int upnp_soap_parse_tag(const char *xml, const char *tag, char *out, size_t outlen);

#endif /* UPNP_SOAP_H */