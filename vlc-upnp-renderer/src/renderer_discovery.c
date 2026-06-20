/*****************************************************************************
 * renderer_discovery.c — UPnP MediaRenderer SSDP discovery for VLC
 *****************************************************************************/
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_renderer_discovery.h>
#include <vlc_probe.h>
#include <vlc_threads.h>

#include "../include/upnp_common.h"
#include "../include/upnp_device.h"
#include "../include/upnp_ssdp.h"
#ifdef __APPLE__
# include "../include/rd_macos_ui.h"
#endif

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define CFG_PREFIX "upnp-renderer-"
#define SCAN_INTERVAL_FOUND   (5 * CLOCK_FREQ)
#define SCAN_INTERVAL_EMPTY   (2 * CLOCK_FREQ)
#define SSDP_TIMEOUT_SEC      (UPNP_SSDP_MX + 1)

static int Open(vlc_object_t *);
static void Close(vlc_object_t *);

VLC_RD_PROBE_HELPER("upnp_renderer", "UPnP/DLNA Renderer Discovery")

vlc_module_begin()
    set_shortname("UPnP Renderer")
    set_description("UPnP/DLNA MediaRenderer discovery")
    set_category(CAT_SOUT)
    set_subcategory(SUBCAT_SOUT_RENDERER)
    set_capability("renderer_discovery", 0)
    set_callbacks(Open, Close)
    add_shortcut("upnp_renderer")
    VLC_RD_PROBE_SUBMODULE
vlc_module_end()

typedef struct rd_item
{
    char *location;
    char *host;
    uint16_t port;
    vlc_renderer_item_t *renderer;
    vlc_tick_t last_seen;
} rd_item_t;

typedef struct vlc_renderer_discovery_sys
{
    vlc_thread_t thread;
    vlc_thread_t fetch_thread;
    vlc_mutex_t lock;
    vlc_cond_t fetch_cond;
    vlc_array_t items;
    vlc_array_t fetch_queue;
    bool stop;
    bool fetch_stop;
} vlc_renderer_discovery_sys_t;

static char *url_encode_config(const char *in)
{
    size_t len = strlen(in);
    char *out = malloc(len * 3 + 1);
    if (out == NULL)
        return NULL;

    char *p = out;
    for (size_t i = 0; i < len; i++)
    {
        unsigned char c = (unsigned char)in[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            *p++ = c;
        else
            p += sprintf(p, "%%%02X", c);
    }
    *p = '\0';
    return out;
}

static rd_item_t *find_item(vlc_renderer_discovery_sys_t *sys, const char *location)
{
    for (size_t i = 0; i < vlc_array_count(&sys->items); i++)
    {
        rd_item_t *it = vlc_array_item_at_index(&sys->items, i);
        if (strcmp(it->location, location) == 0)
            return it;
    }
    return NULL;
}

typedef void (*rd_ui_fn)(void *userdata);

static void rd_run_on_ui(rd_ui_fn fn, void *userdata)
{
#ifdef __APPLE__
    rd_macos_run_on_ui(fn, userdata);
#else
    fn(userdata);
#endif
}

static void rd_run_on_ui_sync(rd_ui_fn fn, void *userdata)
{
#ifdef __APPLE__
    rd_macos_run_on_ui_sync(fn, userdata);
#else
    fn(userdata);
#endif
}

static void add_renderer(vlc_renderer_discovery_t *rd, const upnp_device_t *dev)
{
    vlc_renderer_discovery_sys_t *sys = rd->p_sys;

    char uri[128];
    /* VLC URL parser rejects underscores in schemes — use "upnpcast", not "upnp_cast". */
    snprintf(uri, sizeof(uri), "upnpcast://%s:%u", dev->host, dev->port);

    char *enc = url_encode_config(dev->location);
    if (enc == NULL)
        return;

    char extra[2048];
    snprintf(extra, sizeof(extra), "location=%s", enc);
    free(enc);

    vlc_renderer_item_t *item = vlc_renderer_item_new(
        "upnp", dev->friendly_name, uri, extra, "upnp_demux", NULL,
        VLC_RENDERER_CAN_AUDIO);

    if (item == NULL)
    {
        msg_Warn(rd, "Failed to create renderer item for %s (uri=%s)",
                 dev->friendly_name, uri);
        return;
    }

    rd_item_t *entry = malloc(sizeof(*entry));
    if (entry == NULL)
    {
        vlc_renderer_item_release(item);
        return;
    }

    entry->location = strdup(dev->location);
    entry->host = strdup(dev->host);
    entry->port = dev->port;
    entry->renderer = item;
    entry->last_seen = mdate();
    if (entry->location == NULL || entry->host == NULL)
    {
        free(entry);
        vlc_renderer_item_release(item);
        return;
    }

    vlc_mutex_lock(&sys->lock);
    if (find_item(sys, dev->location) != NULL)
    {
        vlc_mutex_unlock(&sys->lock);
        free(entry->location);
        free(entry->host);
        free(entry);
        vlc_renderer_item_release(item);
        return;
    }

    upnp_registry_add(dev);
    vlc_array_append_or_abort(&sys->items, entry);
    vlc_mutex_unlock(&sys->lock);

    /* Never call vlc_rd_add_item while holding sys->lock — it updates NSMenu. */
    vlc_rd_add_item(rd, item);
    msg_Info(rd, "Found UPnP renderer: %s (%s)", dev->friendly_name, dev->location);
}

typedef struct remove_ui_ctx
{
    vlc_renderer_discovery_t *rd;
    vlc_renderer_item_t *renderer;
    char *location;
    char *host;
    uint16_t port;
} remove_ui_ctx_t;

static void remove_ui_ctx_free(remove_ui_ctx_t *ctx)
{
    if (ctx == NULL)
        return;
    free(ctx->location);
    free(ctx->host);
    free(ctx);
}

static void remove_renderer_ui(void *userdata)
{
    remove_ui_ctx_t *ctx = userdata;
    vlc_renderer_discovery_t *rd = ctx->rd;

    vlc_rd_remove_item(rd, ctx->renderer);
    vlc_renderer_item_release(ctx->renderer);
    upnp_registry_remove(ctx->host, ctx->port);
    remove_ui_ctx_free(ctx);
}

static void schedule_remove_renderer(vlc_renderer_discovery_t *rd, rd_item_t *it)
{
    remove_ui_ctx_t *ctx = malloc(sizeof(*ctx));
    if (ctx == NULL)
        return;

    ctx->rd = rd;
    ctx->renderer = it->renderer;
    ctx->location = it->location;
    ctx->host = it->host;
    ctx->port = it->port;
    it->location = NULL;
    it->host = NULL;
    free(it);

    rd_run_on_ui(remove_renderer_ui, ctx);
}

static void remove_stale(vlc_renderer_discovery_t *rd)
{
    vlc_renderer_discovery_sys_t *sys = rd->p_sys;
    vlc_tick_t now = mdate();
    vlc_array_t stale;

    vlc_array_init(&stale);

    vlc_mutex_lock(&sys->lock);
    for (size_t i = 0; i < vlc_array_count(&sys->items); )
    {
        rd_item_t *it = vlc_array_item_at_index(&sys->items, i);
        if (now - it->last_seen > 30 * CLOCK_FREQ)
        {
            vlc_array_append_or_abort(&stale, it);
            vlc_array_remove(&sys->items, i);
        }
        else
        {
            i++;
        }
    }
    vlc_mutex_unlock(&sys->lock);

    for (size_t i = 0; i < vlc_array_count(&stale); i++)
        schedule_remove_renderer(rd, vlc_array_item_at_index(&stale, i));
    vlc_array_clear(&stale);
}

typedef struct discover_ctx
{
    vlc_renderer_discovery_t *rd;
    vlc_renderer_discovery_sys_t *sys;
} discover_ctx_t;

static bool queue_has_location(vlc_array_t *queue, const char *location)
{
    for (size_t i = 0; i < vlc_array_count(queue); i++)
    {
        const char *queued = vlc_array_item_at_index(queue, i);
        if (strcmp(queued, location) == 0)
            return true;
    }
    return false;
}

static void enqueue_fetch(vlc_renderer_discovery_sys_t *sys, const char *location)
{
    char *copy = strdup(location);
    if (copy == NULL)
        return;

    vlc_array_append_or_abort(&sys->fetch_queue, copy);
    vlc_cond_signal(&sys->fetch_cond);
}

typedef struct publish_ui_ctx
{
    vlc_renderer_discovery_t *rd;
    upnp_device_t dev;
} publish_ui_ctx_t;

static void publish_ui_ctx_free(publish_ui_ctx_t *ctx)
{
    if (ctx == NULL)
        return;
    upnp_device_clear(&ctx->dev);
    free(ctx);
}

static void publish_renderer_ui(void *userdata)
{
    publish_ui_ctx_t *ctx = userdata;
    vlc_renderer_discovery_t *rd = ctx->rd;
    vlc_renderer_discovery_sys_t *sys = rd->p_sys;
    bool publish = false;

    vlc_mutex_lock(&sys->lock);
    rd_item_t *existing = find_item(sys, ctx->dev.location);
    if (existing != NULL)
        existing->last_seen = mdate();
    else
        publish = true;
    vlc_mutex_unlock(&sys->lock);

    if (publish)
        add_renderer(rd, &ctx->dev);

    publish_ui_ctx_free(ctx);
}

static void schedule_publish_renderer(vlc_renderer_discovery_t *rd,
                                      const upnp_device_t *dev)
{
    publish_ui_ctx_t *ctx = malloc(sizeof(*ctx));
    if (ctx == NULL)
        return;

    ctx->rd = rd;
    if (upnp_device_copy(&ctx->dev, dev) != 0)
    {
        free(ctx);
        return;
    }

    rd_run_on_ui(publish_renderer_ui, ctx);
}

static void process_fetch(vlc_renderer_discovery_t *rd,
                          vlc_renderer_discovery_sys_t *sys, const char *location)
{
    upnp_device_t dev;
    if (upnp_device_fetch(location, &dev) != 0)
    {
        msg_Warn(rd, "UPnP device description fetch failed: %s", location);
        return;
    }

    bool publish = false;

    vlc_mutex_lock(&sys->lock);
    rd_item_t *existing = find_item(sys, location);
    if (existing != NULL)
        existing->last_seen = mdate();
    else
        publish = true;
    vlc_mutex_unlock(&sys->lock);

    if (publish)
        schedule_publish_renderer(rd, &dev);

    upnp_device_clear(&dev);
}

static void *FetchWorker(void *data)
{
    vlc_renderer_discovery_t *rd = data;
    vlc_renderer_discovery_sys_t *sys = rd->p_sys;

    vlc_mutex_lock(&sys->lock);
    while (!sys->fetch_stop)
    {
        while (vlc_array_count(&sys->fetch_queue) == 0 && !sys->fetch_stop)
            vlc_cond_wait(&sys->fetch_cond, &sys->lock);

        if (sys->fetch_stop)
            break;

        char *location = vlc_array_item_at_index(&sys->fetch_queue, 0);
        vlc_array_remove(&sys->fetch_queue, 0);
        vlc_mutex_unlock(&sys->lock);

        process_fetch(rd, sys, location);
        free(location);

        vlc_mutex_lock(&sys->lock);
    }
    vlc_mutex_unlock(&sys->lock);
    return NULL;
}

static void on_location(const char *location, void *userdata)
{
    discover_ctx_t *ctx = userdata;
    vlc_renderer_discovery_sys_t *sys = ctx->sys;

    vlc_mutex_lock(&sys->lock);

    if (find_item(sys, location) != NULL)
    {
        vlc_mutex_unlock(&sys->lock);
        return;
    }

    if (queue_has_location(&sys->fetch_queue, location))
    {
        vlc_mutex_unlock(&sys->lock);
        return;
    }

    enqueue_fetch(sys, location);
    vlc_mutex_unlock(&sys->lock);
}

static void *Run(void *data)
{
    vlc_renderer_discovery_t *rd = data;
    vlc_renderer_discovery_sys_t *sys = rd->p_sys;
    discover_ctx_t ctx = { .rd = rd, .sys = sys };

    while (!sys->stop)
    {
        int rc = upnp_ssdp_discover(on_location, &ctx, SSDP_TIMEOUT_SEC);
        if (rc != 0)
            msg_Warn(rd, "SSDP M-SEARCH failed (rc=%d, errno=%d)", rc, errno);

        remove_stale(rd);

        vlc_tick_t interval;
        vlc_mutex_lock(&sys->lock);
        interval = vlc_array_count(&sys->items) > 0
            ? SCAN_INTERVAL_FOUND
            : SCAN_INTERVAL_EMPTY;
        vlc_mutex_unlock(&sys->lock);
        msleep(interval);
    }

    return NULL;
}

static int Open(vlc_object_t *obj)
{
    vlc_renderer_discovery_t *rd = (vlc_renderer_discovery_t *)obj;

    vlc_renderer_discovery_sys_t *sys = calloc(1, sizeof(*sys));
    if (sys == NULL)
        return VLC_ENOMEM;

    vlc_array_init(&sys->items);
    vlc_array_init(&sys->fetch_queue);
    vlc_mutex_init(&sys->lock);
    vlc_cond_init(&sys->fetch_cond);
    rd->p_sys = sys;

    if (vlc_clone(&sys->fetch_thread, FetchWorker, rd, VLC_THREAD_PRIORITY_LOW))
    {
        vlc_cond_destroy(&sys->fetch_cond);
        vlc_mutex_destroy(&sys->lock);
        free(sys);
        return VLC_ENOMEM;
    }

    if (vlc_clone(&sys->thread, Run, rd, VLC_THREAD_PRIORITY_LOW))
    {
        sys->fetch_stop = true;
        vlc_cond_signal(&sys->fetch_cond);
        vlc_join(sys->fetch_thread, NULL);
        vlc_cond_destroy(&sys->fetch_cond);
        vlc_mutex_destroy(&sys->lock);
        free(sys);
        return VLC_ENOMEM;
    }

    msg_Dbg(rd, "UPnP renderer discovery started");
    return VLC_SUCCESS;
}

static void Close(vlc_object_t *obj)
{
    vlc_renderer_discovery_t *rd = (vlc_renderer_discovery_t *)obj;
    vlc_renderer_discovery_sys_t *sys = rd->p_sys;

    sys->stop = true;
    vlc_join(sys->thread, NULL);

    vlc_mutex_lock(&sys->lock);
    sys->fetch_stop = true;
    vlc_cond_signal(&sys->fetch_cond);
    vlc_mutex_unlock(&sys->lock);
    vlc_join(sys->fetch_thread, NULL);

    vlc_array_t remaining;
    vlc_array_init(&remaining);

    vlc_mutex_lock(&sys->lock);
    for (size_t i = 0; i < vlc_array_count(&sys->fetch_queue); i++)
        free(vlc_array_item_at_index(&sys->fetch_queue, i));
    vlc_array_clear(&sys->fetch_queue);
    for (size_t i = 0; i < vlc_array_count(&sys->items); i++)
        vlc_array_append_or_abort(&remaining,
            vlc_array_item_at_index(&sys->items, i));
    vlc_array_clear(&sys->items);
    vlc_mutex_unlock(&sys->lock);

    for (size_t i = 0; i < vlc_array_count(&remaining); i++)
    {
        remove_ui_ctx_t *ctx = malloc(sizeof(*ctx));
        rd_item_t *it = vlc_array_item_at_index(&remaining, i);
        if (ctx == NULL)
            continue;

        ctx->rd = rd;
        ctx->renderer = it->renderer;
        ctx->location = it->location;
        ctx->host = it->host;
        ctx->port = it->port;
        it->location = NULL;
        it->host = NULL;
        free(it);
        rd_run_on_ui_sync(remove_renderer_ui, ctx);
    }
    vlc_array_clear(&remaining);

    vlc_cond_destroy(&sys->fetch_cond);
    vlc_mutex_destroy(&sys->lock);
    free(sys);
    upnp_registry_clear();
}