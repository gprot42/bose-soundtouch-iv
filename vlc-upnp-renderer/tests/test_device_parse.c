#include "upnp_device.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "fixtures/bose_device_description.xml";
    FILE *f = fopen(path, "r");
    if (f == NULL)
    {
        fprintf(stderr, "FAIL: cannot open %s\n", path);
        return 1;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *xml = malloc((size_t)sz + 1);
    if (xml == NULL)
        return 1;
    fread(xml, 1, (size_t)sz, f);
    fclose(f);
    xml[sz] = '\0';

    const char *location =
        "http://192.168.0.119:8091/XD/BO5EBO5E-F00D-F00D-FEED-7C010A90E9CA.xml";

    upnp_device_t dev;
    if (upnp_device_parse_xml(xml, (size_t)sz, location, &dev) != 0)
    {
        fprintf(stderr, "FAIL: parse error\n");
        free(xml);
        return 1;
    }

    if (dev.friendly_name == NULL || strstr(dev.friendly_name, "Bose") == NULL)
    {
        fprintf(stderr, "FAIL: friendly_name: %s\n", dev.friendly_name ? dev.friendly_name : "(null)");
        upnp_device_clear(&dev);
        free(xml);
        return 1;
    }

    if (dev.av_control == NULL || strstr(dev.av_control, "AVTransport") == NULL)
    {
        fprintf(stderr, "FAIL: av_control: %s\n", dev.av_control ? dev.av_control : "(null)");
        upnp_device_clear(&dev);
        free(xml);
        return 1;
    }

    printf("OK: %s\n", dev.friendly_name);
    printf("    av: %s\n", dev.av_control);
    if (dev.rc_control)
        printf("    rc: %s\n", dev.rc_control);

    upnp_device_clear(&dev);
    free(xml);
    return 0;
}