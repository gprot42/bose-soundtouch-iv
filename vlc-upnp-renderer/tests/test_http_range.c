#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* Mirror of upnp_http_serve.c parse_range for unit testing. */
static bool parse_range(const char *req, long long file_size,
                        long long *start_out, long long *end_out)
{
    const char *hdr = strstr(req, "Range:");
    if (hdr == NULL)
        hdr = strstr(req, "range:");
    if (hdr == NULL)
        return false;

    const char *bytes = strstr(hdr, "bytes=");
    if (bytes == NULL)
        return false;

    if (file_size <= 0)
        return false;

    char spec[128];
    const char *spec_in = bytes + 6;
    size_t speclen = strcspn(spec_in, "\r\n");
    if (speclen == 0 || speclen >= sizeof(spec))
        return false;
    memcpy(spec, spec_in, speclen);
    spec[speclen] = '\0';

    char *dash = strchr(spec, '-');
    if (dash == NULL)
        return false;

    long long start = 0;
    long long end = file_size - 1;

    if (dash == spec)
    {
        long long suffix = 0;
        if (sscanf(dash + 1, "%lld", &suffix) != 1 || suffix <= 0)
            return false;
        start = file_size > suffix ? file_size - suffix : 0;
    }
    else
    {
        long long start_val = 0;
        long long end_val = 0;
        bool has_end = false;

        *dash = '\0';
        if (sscanf(spec, "%lld", &start_val) != 1 || start_val < 0)
            return false;

        if (dash[1] != '\0' && sscanf(dash + 1, "%lld", &end_val) == 1)
            has_end = true;

        start = start_val;
        if (has_end && end_val >= 0)
            end = end_val;
    }

    if (start >= file_size)
        return false;
    if (end < start)
        return false;
    if (end >= file_size)
        end = file_size - 1;

    *start_out = start;
    *end_out = end;
    return true;
}

int main(void)
{
    long long start, end;
    const long long size = 10000000;

    if (!parse_range("GET /f HTTP/1.1\r\nRange: bytes=12345-\r\n", size, &start, &end)
     || start != 12345 || end != size - 1)
    {
        fprintf(stderr, "FAIL: open-ended range start=%lld end=%lld\n", start, end);
        return 1;
    }

    if (!parse_range("GET /f HTTP/1.1\r\nRange: bytes=1000-5000\r\n", size, &start, &end)
     || start != 1000 || end != 5000)
    {
        fprintf(stderr, "FAIL: closed range\n");
        return 1;
    }

    if (!parse_range("GET /f HTTP/1.1\r\nRange: bytes=-500\r\n", size, &start, &end)
     || start != size - 500 || end != size - 1)
    {
        fprintf(stderr, "FAIL: suffix range start=%lld end=%lld\n", start, end);
        return 1;
    }

    printf("OK: HTTP range parsing\n");
    return 0;
}