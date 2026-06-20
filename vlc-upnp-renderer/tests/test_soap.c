#include "upnp_soap.h"

#include <stdio.h>
#include <string.h>

int main(void)
{
    FILE *f = fopen("fixtures/soap_get_transport_info.xml", "r");
    if (f == NULL)
        f = fopen("../fixtures/soap_get_transport_info.xml", "r");
    if (f == NULL)
    {
        fprintf(stderr, "skip: no SOAP fixture\n");
        return 0;
    }

    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    char state[64];
    if (upnp_soap_parse_tag(buf, "CurrentTransportState", state, sizeof(state)) != 0)
    {
        fprintf(stderr, "FAIL: could not parse CurrentTransportState\n");
        return 1;
    }

    if (strcmp(state, "PLAYING") != 0)
    {
        fprintf(stderr, "FAIL: unexpected state: %s\n", state);
        return 1;
    }

    printf("OK: parsed CurrentTransportState: %s\n", state);
    return 0;
}