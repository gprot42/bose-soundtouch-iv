#include "upnp_ssdp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    FILE *f = fopen("fixtures/ssdp_msearch_response.txt", "r");
    if (f == NULL)
        f = fopen("../fixtures/ssdp_msearch_response.txt", "r");
    if (f == NULL)
    {
        fprintf(stderr, "skip: no ssdp fixture\n");
        return 0;
    }

    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    char location[1024];
    if (!upnp_ssdp_is_renderer_response(buf))
    {
        fprintf(stderr, "FAIL: fixture should be a MediaRenderer response\n");
        return 1;
    }

    if (upnp_ssdp_parse_location(buf, location, sizeof(location)) != 0)
    {
        fprintf(stderr, "FAIL: could not parse LOCATION\n");
        return 1;
    }

    if (strncmp(location, "http://", 7) != 0)
    {
        fprintf(stderr, "FAIL: unexpected location: %s\n", location);
        return 1;
    }

    printf("OK: parsed LOCATION: %s\n", location);
    return 0;
}