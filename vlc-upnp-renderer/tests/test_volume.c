#include "upnp_cast.h"

#include <stdio.h>

int main(void)
{
    if (upnp_cast_volume_percent(0.f) != 0)
    {
        fprintf(stderr, "FAIL: 0%% volume\n");
        return 1;
    }

    if (upnp_cast_volume_percent(1.f) != 100)
    {
        fprintf(stderr, "FAIL: 100%% volume\n");
        return 1;
    }

    if (upnp_cast_volume_percent(0.4f) != 40)
    {
        fprintf(stderr, "FAIL: 40%% volume\n");
        return 1;
    }

    if (upnp_cast_volume_percent(1.25f) != 100)
    {
        fprintf(stderr, "FAIL: amplified volume should cap at 100\n");
        return 1;
    }

    if (upnp_cast_volume_percent(-0.5f) != 0)
    {
        fprintf(stderr, "FAIL: negative volume should clamp to 0\n");
        return 1;
    }

    printf("OK: volume percent mapping\n");
    return 0;
}