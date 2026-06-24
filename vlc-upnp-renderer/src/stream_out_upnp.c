/*****************************************************************************
 * stream_out_upnp.c — UPnP cast stream_out + demux_filter for VLC
 *****************************************************************************/
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_sout.h>
#include <vlc_block.h>
#include <vlc_demux.h>
#include <vlc_input.h>
#include <vlc_playlist.h>
#include <vlc_variables.h>
#include <vlc_configuration.h>
#include <vlc_url.h>

#include "../include/upnp_cast.h"
#include "../include/upnp_common.h"
#include "../include/upnp_display.h"
#include "../include/upnp_soap.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CAST_CFG_PREFIX "sout-upnp_cast-"

static upnp_cast_session_t g_session;

/* --- stream_out (drops decoded blocks; remote device plays) --- */

static int SoutOpen(vlc_object_t *);
static void SoutClose(vlc_object_t *);

static sout_stream_id_sys_t *SoutAdd(sout_stream_t *, const es_format_t *);
static void SoutDel(sout_stream_t *, sout_stream_id_sys_t *);
static int SoutSend(sout_stream_t *, sout_stream_id_sys_t *, block_t *);

static const char *const ppsz_sout_options[] = {
    "ip", "port", "location", NULL
};

/* --- demux_filter (starts cast, syncs transport controls) --- */

static int DemuxOpen(vlc_object_t *);
static void DemuxClose(vlc_object_t *);

static int DemuxDemux(demux_t *);
static int DemuxControl(demux_t *, int, va_list);

vlc_module_begin()
    set_shortname("UPnP Cast")
    set_description("UPnP/DLNA renderer cast")
    set_capability("sout stream", 0)
    set_category(CAT_SOUT)
    set_subcategory(SUBCAT_SOUT_STREAM)
    set_callbacks(SoutOpen, SoutClose)
    add_shortcut("upnpcast")
    add_shortcut("upnp_cast")
    add_string(CAST_CFG_PREFIX "ip", NULL, NULL, NULL, true)
        change_private()
    add_integer(CAST_CFG_PREFIX "port", UPNP_DEFAULT_PORT, NULL, NULL, true)
        change_private()
    add_string(CAST_CFG_PREFIX "location", NULL, NULL, NULL, true)
        change_private()

    add_submodule()
        set_description("UPnP cast demux filter")
        set_capability("demux_filter", 0)
        set_category(CAT_INPUT)
        set_subcategory(SUBCAT_INPUT_DEMUX)
        set_callbacks(DemuxOpen, DemuxClose)
        add_shortcut("upnp_demux")
vlc_module_end()

static int SoutOpen(vlc_object_t *obj)
{
    sout_stream_t *stream = (sout_stream_t *)obj;

    config_ChainParse(stream, CAST_CFG_PREFIX, ppsz_sout_options, stream->p_cfg);

    char *ip = var_InheritString(stream, CAST_CFG_PREFIX "ip");
    int port = var_InheritInteger(stream, CAST_CFG_PREFIX "port");
    char *location = var_InheritString(stream, CAST_CFG_PREFIX "location");

    if (ip == NULL)
    {
        free(location);
        return VLC_EGENERIC;
    }

    if (g_session.device.av_control == NULL)
        upnp_cast_session_clear(&g_session);

    char *decoded_location = NULL;
    const char *device_location = location;
    if (location != NULL && strchr(location, '%') != NULL)
    {
        decoded_location = vlc_uri_decode_duplicate(location);
        if (decoded_location != NULL)
            device_location = decoded_location;
    }

    if (upnp_cast_session_init(&g_session, ip, (uint16_t)port, device_location) != 0
     && upnp_cast_session_init(&g_session, ip, (uint16_t)port, NULL) != 0)
    {
        msg_Err(stream, "Cannot initialize UPnP renderer at %s:%d", ip, port);
        free(decoded_location);
        free(ip);
        free(location);
        return VLC_EGENERIC;
    }

    /* demux_filter inherits vars from the input thread, not the sout stream */
    vlc_object_t *input_obj = stream->p_sout != NULL ? stream->p_sout->obj.parent : NULL;
    if (input_obj != NULL)
    {
        var_Create(input_obj, UPNP_CAST_VAR, VLC_VAR_ADDRESS);
        var_SetAddress(input_obj, UPNP_CAST_VAR, &g_session);
    }

    stream->pf_add = SoutAdd;
    stream->pf_del = SoutDel;
    stream->pf_send = SoutSend;
    stream->p_sys = NULL;
    stream->pace_nocontrol = true;

    msg_Info(stream, "UPnP cast session ready for %s",
             g_session.device.friendly_name ? g_session.device.friendly_name : ip);

    free(decoded_location);
    free(ip);
    free(location);
    return VLC_SUCCESS;
}

static void SoutClose(vlc_object_t *obj)
{
    sout_stream_t *stream = (sout_stream_t *)obj;

    if (stream->p_sout != NULL && stream->p_sout->obj.parent != NULL)
    {
        vlc_object_t *input_obj = stream->p_sout->obj.parent;
        var_SetAddress(input_obj, UPNP_CAST_VAR, NULL);
        var_Destroy(input_obj, UPNP_CAST_VAR);
    }

    /*
     * Renderer playback sets sout-keep, so this close callback often does not
     * run when the user presses Stop — DemuxClose must stop the device too.
     * When it does run, ensure remote playback is stopped before teardown.
     */
    if (g_session.casting || g_session.httpd != NULL)
        upnp_cast_stop(&g_session);

    if (g_session.owner_input == NULL && !g_session.casting && g_session.httpd == NULL)
        upnp_cast_session_clear(&g_session);
}

static sout_stream_id_sys_t *SoutAdd(sout_stream_t *stream, const es_format_t *fmt)
{
    VLC_UNUSED(stream);
    VLC_UNUSED(fmt);
    return malloc(1);
}

static void SoutDel(sout_stream_t *stream, sout_stream_id_sys_t *id)
{
    VLC_UNUSED(stream);
    free(id);
}

static int SoutSend(sout_stream_t *stream, sout_stream_id_sys_t *id, block_t *block)
{
    VLC_UNUSED(stream);
    VLC_UNUSED(id);
    block_ChainRelease(block);
    return VLC_SUCCESS;
}

typedef struct upnp_demux_sys
{
    demux_t *demux;
    upnp_cast_session_t *session;
    input_thread_t *input;
    playlist_t *playlist;
    bool enabled;
    bool folder_mode;
    bool directory_expanded;
    bool track_active;
    bool local_eof;
    bool renderer_confirmed;
    bool seen_playing;
    bool renderer_paused;
    bool stop_sent;
    bool ui_length_set;
    bool volume_sync;
    bool pending_default_volume;
    int last_volume;
    bool last_mute;
    char display_title[256];
    vlc_tick_t last_display_push;
    vlc_tick_t cast_start;
    vlc_tick_t play_confirmed_at;
    vlc_tick_t length;
    vlc_tick_t time;
    vlc_tick_t seek_target;
    vlc_tick_t seek_until;
} upnp_demux_sys_t;

#define UPNP_VOLUME_UNSET (-1)
#define UPNP_DEFAULT_CAST_VOLUME 25
#define UPNP_DEFAULT_CAST_VLC_VOLUME (UPNP_DEFAULT_CAST_VOLUME / 100.f)

#define CAST_PLAY_TIMEOUT (120 * CLOCK_FREQ)
#define CAST_EOF_MARGIN   (1 * CLOCK_FREQ)
#define CAST_MIN_PLAY     (5 * CLOCK_FREQ)
#define CAST_WAIT_DELAY   (CLOCK_FREQ / 10)
#define CAST_SEEK_PENDING   (3 * CLOCK_FREQ)
#define CAST_SEEK_TOLERANCE (2 * CLOCK_FREQ)

enum {
    CAST_START_OK = VLC_SUCCESS,
    CAST_START_WAIT = 2,
    CAST_START_ERR = VLC_EGENERIC,
};

static bool is_active_playback_state(const char *state)
{
    return strcmp(state, "PLAYING") == 0
        || strcmp(state, "PAUSED") == 0
        || strcmp(state, "PAUSED_PLAYBACK") == 0;
}

static bool is_ended_state(const char *state)
{
    return strcmp(state, "STOPPED") == 0
        || strcmp(state, "NO_MEDIA_PRESENT") == 0;
}

static input_thread_t *get_filter_input(demux_t *demux);
static const char *get_source_uri(demux_t *demux);
static void resolve_display_title(demux_t *demux, upnp_demux_sys_t *sys,
                                  const char *source);
static void push_display_title(demux_t *demux, upnp_demux_sys_t *sys);
static void clear_display_title(upnp_demux_sys_t *sys);
static void maybe_refresh_display(demux_t *demux, upnp_demux_sys_t *sys);

static void cache_filter_input(demux_t *demux, upnp_demux_sys_t *sys)
{
    if (sys->input == NULL)
        sys->input = get_filter_input(demux);
}

static void probe_length_from_input(demux_t *demux, upnp_demux_sys_t *sys)
{
    cache_filter_input(demux, sys);
    if (sys->input == NULL)
        return;

    input_item_t *item = input_GetItem(sys->input);
    if (item == NULL)
        return;

    vlc_tick_t dur = input_item_GetDuration(item);
    if (dur > 0)
        sys->length = dur;
}

static void probe_length_from_uri(const char *source, upnp_demux_sys_t *sys)
{
    char path[4096];

    if (source == NULL || sys->length > 0)
        return;

    if (strncmp(source, "file://", 7) == 0)
    {
        const char *p = source + 7;
        if (p[0] == '/' && p[2] == ':')
            p++;
        if (upnp_url_decode(p, path, sizeof(path)) != 0)
            return;
    }
    else if (source[0] == '/')
    {
        if (upnp_url_decode(source, path, sizeof(path)) != 0)
            return;
    }
    else
        return;

    int64_t dur = upnp_probe_media_duration(path);
    if (dur > 0)
        sys->length = dur;
}

static void probe_length_from_source(demux_t *demux, upnp_demux_sys_t *sys)
{
    probe_length_from_uri(get_source_uri(demux), sys);
}

static void push_playback_times(demux_t *demux, upnp_demux_sys_t *sys)
{
    cache_filter_input(demux, sys);

    if (sys->input == NULL || !sys->track_active || sys->length <= 0)
        return;

    double pos = (double)sys->time / (double)sys->length;
    if (pos < 0.0)
        pos = 0.0;
    if (pos > 1.0)
        pos = 1.0;

    vlc_value_t val;

    val.f_float = (float)pos;
    var_Change(sys->input, "position", VLC_VAR_SETVALUE, &val, NULL);

    val.i_int = sys->time;
    var_Change(sys->input, "time", VLC_VAR_SETVALUE, &val, NULL);

    if (!sys->ui_length_set)
    {
        val.i_int = sys->length;
        var_Change(sys->input, "length", VLC_VAR_SETVALUE, &val, NULL);
        var_SetInteger(sys->input, "intf-event", INPUT_EVENT_LENGTH);
        sys->ui_length_set = true;
    }

    var_SetInteger(sys->input, "intf-event", INPUT_EVENT_POSITION);
}

static void probe_length_from_next(demux_t *demux, upnp_demux_sys_t *sys)
{
    int64_t len = 0;

    if (demux->p_next != NULL
     && demux_Control(demux->p_next, DEMUX_GET_LENGTH, &len) == VLC_SUCCESS
     && len > 0)
        sys->length = len;

    if (sys->length <= 0)
        probe_length_from_input(demux, sys);
    if (sys->length <= 0)
        probe_length_from_source(demux, sys);
}

static void stop_remote_playback(upnp_demux_sys_t *sys)
{
    if (sys == NULL || sys->stop_sent)
        return;

    if (!sys->track_active && !sys->session->casting && sys->session->httpd == NULL)
        return;

    clear_display_title(sys);
    upnp_cast_stop(sys->session);
    sys->stop_sent = true;
}

static void update_position_from_renderer(upnp_demux_sys_t *sys)
{
    if (sys->session->device.av_control == NULL)
        return;

    char rel[32], dur[32];
    if (upnp_av_get_position_info(sys->session->device.av_control,
                                  rel, sizeof(rel), dur, sizeof(dur)) != 0)
        return;

    int64_t ticks = 0;
    bool have_rel = upnp_parse_hms_duration(rel, &ticks) == 0;

    if (sys->seek_until > 0 && mdate() < sys->seek_until)
    {
        if (have_rel)
        {
            int64_t diff = ticks - sys->seek_target;
            if (diff < 0)
                diff = -diff;
            if (diff <= CAST_SEEK_TOLERANCE)
            {
                sys->seek_until = 0;
                sys->time = ticks;
            }
        }
        return;
    }

    sys->seek_until = 0;

    if (have_rel)
        sys->time = ticks;

    if (sys->length <= 0 && dur[0] != '\0' && strcmp(dur, "0:00:00") != 0
     && upnp_parse_hms_duration(dur, &ticks) == 0 && ticks > 0)
        sys->length = ticks;
}

static void ensure_cast_length(demux_t *demux, upnp_demux_sys_t *sys)
{
    if (sys->length > 0)
        return;

    probe_length_from_next(demux, sys);
    if (sys->length > 0)
        return;

    probe_length_from_input(demux, sys);
    if (sys->length > 0)
        return;

    update_position_from_renderer(sys);
    if (sys->length > 0)
        return;

    cache_filter_input(demux, sys);
    if (sys->input != NULL)
    {
        vlc_tick_t len = var_GetInteger(sys->input, "length");
        if (len > 0)
            sys->length = len;
    }
}

static bool cast_seek_pending(upnp_demux_sys_t *sys)
{
    return sys->seek_until > 0 && mdate() < sys->seek_until;
}

static bool cast_session_active(upnp_demux_sys_t *sys)
{
    return sys->track_active
        || (sys->session != NULL && sys->session->casting);
}

static bool vlc_input_playing(upnp_demux_sys_t *sys)
{
    cache_filter_input(sys->demux, sys);

    if (sys->input == NULL)
        return true;

    return input_GetState(sys->input) == PLAYING_S;
}

static int seek_reanchor_and_play(demux_t *demux, upnp_demux_sys_t *sys,
                                  const char *target)
{
    upnp_cast_session_t *session = sys->session;
    const char *av_control = session->device.av_control;

    if (av_control == NULL || session->media_url[0] == '\0')
        return VLC_EGENERIC;

    if (upnp_av_set_uri(av_control, session->media_url, sys->display_title,
                        NULL, NULL, sys->length) != 0)
        return VLC_EGENERIC;

    usleep(UPNP_CAST_SETTLE_US);

    if (upnp_av_seek_rel(av_control, target) != 0)
        return VLC_EGENERIC;

    usleep(UPNP_CAST_SETTLE_US);

    if (upnp_av_play(av_control) != 0)
        return VLC_EGENERIC;

    msg_Dbg(demux, "UPnP re-anchored cast for seek to %s", target);
    return VLC_SUCCESS;
}

static int ensure_renderer_playing(demux_t *demux, upnp_demux_sys_t *sys,
                                   const char *target, bool want_play)
{
    const char *av_control = sys->session->device.av_control;
    char state[64];

    if (!want_play || av_control == NULL)
        return VLC_SUCCESS;

    if (upnp_av_get_transport_state(av_control, state, sizeof(state)) == 0
     && strcmp(state, "PLAYING") == 0)
        return VLC_SUCCESS;

    if (upnp_av_play(av_control) == 0)
        return VLC_SUCCESS;

    msg_Warn(demux, "UPnP Play after seek to %s failed (state=%s)",
              target, state);
    return seek_reanchor_and_play(demux, sys, target);
}

static int seek_remote_playback(demux_t *demux, upnp_demux_sys_t *sys,
                                int64_t time)
{
    const char *av_control = sys->session->device.av_control;

    if (!cast_session_active(sys) || av_control == NULL)
        return VLC_EGENERIC;

    if (time < 0)
        time = 0;
    if (sys->length > 0 && time > sys->length)
        time = sys->length;

    char target[32];
    if (upnp_format_hms_duration(time, target, sizeof(target)) != 0)
        return VLC_EGENERIC;

    bool want_play = vlc_input_playing(sys);

    sys->seek_target = time;
    sys->seek_until = mdate() + CAST_SEEK_PENDING;
    sys->time = time;

    int ret = VLC_SUCCESS;

    if (upnp_av_seek_rel(av_control, target) != 0)
    {
        msg_Warn(demux, "UPnP Seek to %s failed, re-anchoring", target);
        ret = seek_reanchor_and_play(demux, sys, target);
    }
    else
    {
        usleep(UPNP_CAST_SETTLE_US);
        ret = ensure_renderer_playing(demux, sys, target, want_play);
    }

    if (ret != VLC_SUCCESS)
    {
        sys->seek_until = 0;
        return VLC_EGENERIC;
    }

    sys->renderer_paused = !want_play;
    sys->seen_playing = want_play;
    push_playback_times(demux, sys);
    msg_Info(demux, "UPnP Seek to %s", target);
    return VLC_SUCCESS;
}

static void resolve_display_title(demux_t *demux, upnp_demux_sys_t *sys,
                                  const char *source)
{
    const char *title = NULL;

    cache_filter_input(demux, sys);
    if (sys->input != NULL)
    {
        input_item_t *item = input_GetItem(sys->input);
        if (item != NULL)
        {
            title = input_item_GetTitle(item);
            if (title == NULL || title[0] == '\0')
                title = input_item_GetName(item);
        }
    }

    if (title != NULL && title[0] != '\0')
    {
        strncpy(sys->display_title, title, sizeof(sys->display_title) - 1);
        sys->display_title[sizeof(sys->display_title) - 1] = '\0';
        return;
    }

    upnp_display_title_from_source(source, sys->display_title,
                                   sizeof(sys->display_title));
}

static void push_display_title(demux_t *demux, upnp_demux_sys_t *sys)
{
    const char *host = sys->session->device.host;

    if (host == NULL || sys->display_title[0] == '\0')
        return;

    if (upnp_display_push_title(host, sys->display_title) == 0)
        msg_Info(demux, "Display: %s", sys->display_title);
    else
        msg_Dbg(demux, "Display update skipped for %s", host);

    sys->last_display_push = mdate();
}

static void clear_display_title(upnp_demux_sys_t *sys)
{
    const char *host;

    if (sys == NULL || sys->session == NULL)
        return;

    host = sys->session->device.host;
    if (host == NULL || host[0] == '\0')
        return;

    if (upnp_display_clear(host) == 0)
        msg_Info(sys->demux, "Display cleared");
    else
        msg_Dbg(sys->demux, "Display clear skipped for %s", host);

    sys->last_display_push = 0;
}

static void maybe_refresh_display(demux_t *demux, upnp_demux_sys_t *sys)
{
    if (!sys->track_active || sys->display_title[0] == '\0')
        return;

    if (sys->last_display_push <= 0
     || mdate() - sys->last_display_push >= UPNP_DISPLAY_INTERVAL_US)
        push_display_title(demux, sys);
}

static const char *get_source_uri(demux_t *demux)
{
    demux_t *chain = demux;

    while (chain != NULL)
    {
        if (chain->psz_location != NULL && chain->psz_location[0] != '\0')
            return chain->psz_location;
        if (chain->psz_file != NULL && chain->psz_file[0] != '\0')
            return chain->psz_file;
        if (chain->p_input != NULL)
        {
            input_item_t *item = input_GetItem(chain->p_input);
            if (item != NULL && item->psz_uri != NULL && item->psz_uri[0] != '\0')
                return item->psz_uri;
        }
        chain = chain->p_next;
    }

    return NULL;
}

static bool is_playlist_demux(demux_t *demux)
{
    bool playlist = false;

    if (demux->p_next != NULL
     && demux_Control(demux->p_next, DEMUX_IS_PLAYLIST, &playlist) == VLC_SUCCESS)
        return playlist;

    return false;
}

static int start_cast(demux_t *demux, upnp_demux_sys_t *sys);
static int start_cast_uri(demux_t *demux, upnp_demux_sys_t *sys,
                          const char *source);
static bool cast_session_ready(upnp_cast_session_t *session);

static input_thread_t *get_filter_input(demux_t *demux)
{
    demux_t *chain = demux;

    while (chain != NULL)
    {
        if (chain->p_input != NULL)
            return chain->p_input;
        chain = chain->p_next;
    }

    return NULL;
}

static void bind_cast_var_to_input(demux_t *demux, upnp_cast_session_t *session)
{
    input_thread_t *input = get_filter_input(demux);

    if (input == NULL || session == NULL)
        return;

    var_Create(input, UPNP_CAST_VAR, VLC_VAR_ADDRESS);
    var_SetAddress(input, UPNP_CAST_VAR, session);
}

static playlist_t *get_input_playlist(input_thread_t *input)
{
    if (input == NULL || input->obj.parent == NULL)
        return NULL;

    return (playlist_t *)input->obj.parent;
}

static void read_playlist_volume(playlist_t *playlist, float *vol, bool *mute)
{
    float level = playlist_VolumeGet(playlist);

    if (level < 0.f)
        level = var_GetFloat(playlist, "volume");
    if (level < 0.f)
        level = 1.f;

    *vol = level;
    *mute = var_GetBool(playlist, "mute");
}

static void set_playlist_ui_volume(playlist_t *playlist, float vol)
{
    if (playlist == NULL)
        return;

    /* playlist_VolumeSet updates the audio output that the UI reads;
     * fall back to the playlist variable for hosts without an aout yet. */
    if (playlist_VolumeSet(playlist, vol) != 0)
        var_SetFloat(playlist, "volume", vol);
}

static void apply_default_cast_volume(upnp_demux_sys_t *sys)
{
    if (sys == NULL || !sys->pending_default_volume)
        return;
    if (!sys->enabled || !sys->track_active)
        return;
    if (sys->session == NULL || sys->session->device.rc_control == NULL)
        return;

    if (upnp_rc_set_volume(sys->session->device.rc_control,
                           UPNP_DEFAULT_CAST_VOLUME) != 0)
        return;

    sys->pending_default_volume = false;
    sys->last_volume = UPNP_DEFAULT_CAST_VOLUME;
    msg_Dbg(sys->demux, "UPnP default volume set to %d",
            UPNP_DEFAULT_CAST_VOLUME);
}

static void push_renderer_volume(upnp_demux_sys_t *sys, float vol, bool mute)
{
    if (sys == NULL || !sys->enabled || !sys->track_active)
        return;
    if (sys->session == NULL || sys->session->device.rc_control == NULL)
        return;

    int level = upnp_cast_volume_percent(vol);
    bool mute_changed = sys->last_mute != mute;
    bool volume_changed = sys->last_volume != level;

    if (!mute_changed && !volume_changed)
        return;

    if (upnp_cast_sync_volume(sys->session, vol, mute, mute_changed,
                              !mute && (volume_changed || mute_changed)) != 0)
    {
        msg_Warn(sys->demux, "UPnP volume sync failed (level=%d, mute=%d)",
                 level, mute ? 1 : 0);
        return;
    }

    sys->last_mute = mute;
    sys->last_volume = mute ? 0 : level;
    msg_Dbg(sys->demux, "UPnP volume synced to %d%%%s",
            level, mute ? " (muted)" : "");
}

static int VolumeCallback(vlc_object_t *obj, const char *name,
                          vlc_value_t oldval, vlc_value_t newval, void *data)
{
    upnp_demux_sys_t *sys = data;

    (void)obj;
    (void)name;
    (void)oldval;

    if (newval.f_float < 0.f)
        return VLC_SUCCESS;

    bool mute = sys->playlist != NULL && var_GetBool(sys->playlist, "mute");
    push_renderer_volume(sys, newval.f_float, mute);
    return VLC_SUCCESS;
}

static int MuteCallback(vlc_object_t *obj, const char *name,
                        vlc_value_t oldval, vlc_value_t newval, void *data)
{
    upnp_demux_sys_t *sys = data;
    float vol = 1.f;
    bool ignored_mute = false;

    (void)obj;
    (void)name;
    (void)oldval;

    if (sys->playlist != NULL)
        read_playlist_volume(sys->playlist, &vol, &ignored_mute);
    push_renderer_volume(sys, vol, newval.b_bool);
    return VLC_SUCCESS;
}

static void detach_volume_sync(upnp_demux_sys_t *sys)
{
    if (sys == NULL || !sys->volume_sync || sys->playlist == NULL)
        return;

    var_DelCallback(sys->playlist, "volume", VolumeCallback, sys);
    var_DelCallback(sys->playlist, "mute", MuteCallback, sys);
    sys->volume_sync = false;
    sys->playlist = NULL;
    sys->last_volume = UPNP_VOLUME_UNSET;
}

static void attach_volume_sync(demux_t *demux, upnp_demux_sys_t *sys)
{
    float vol;
    bool mute;

    cache_filter_input(demux, sys);
    if (sys->input == NULL || sys->volume_sync)
        return;

    sys->playlist = get_input_playlist(sys->input);
    if (sys->playlist == NULL)
        return;

    if (sys->session->device.rc_control == NULL)
    {
        msg_Dbg(demux, "UPnP renderer has no RenderingControl endpoint");
        return;
    }

    var_AddCallback(sys->playlist, "volume", VolumeCallback, sys);
    var_AddCallback(sys->playlist, "mute", MuteCallback, sys);
    sys->volume_sync = true;

    read_playlist_volume(sys->playlist, &vol, &mute);
    sys->last_volume = UPNP_DEFAULT_CAST_VOLUME;
    sys->last_mute = mute;
    set_playlist_ui_volume(sys->playlist, UPNP_DEFAULT_CAST_VLC_VOLUME);
}

static bool input_has_ended(input_thread_t *input)
{
    if (input == NULL)
        return true;

    return input_GetState(input) == END_S;
}

static bool cast_input_active(demux_t *demux, upnp_demux_sys_t *sys)
{
    cache_filter_input(demux, sys);

    return sys->input != NULL && !demux->b_preparsing
        && !input_has_ended(sys->input);
}

static void bind_cast_input(demux_t *demux, upnp_demux_sys_t *sys)
{
    cache_filter_input(demux, sys);

    if (sys->input != NULL && !demux->b_preparsing)
        sys->session->owner_input = sys->input;
}

static bool cast_playback_ended(upnp_demux_sys_t *sys, const char *state)
{
    if (!sys->renderer_confirmed || !is_ended_state(state))
        return false;

    if (cast_seek_pending(sys))
        return false;

    if (sys->play_confirmed_at > 0
     && mdate() - sys->play_confirmed_at < CAST_MIN_PLAY)
        return false;

    /* Local file is drained early during cast; rely on remote time/length. */
    if (sys->length > CAST_EOF_MARGIN)
        return sys->time + CAST_EOF_MARGIN >= sys->length;

    if (sys->track_active && sys->seen_playing)
        return false;

    return sys->local_eof;
}

static bool renderer_is_busy(upnp_cast_session_t *session)
{
    if (session->device.av_control == NULL)
        return false;

    char state[64];
    if (upnp_av_get_transport_state(session->device.av_control,
                                    state, sizeof(state)) != 0)
        return false;

    return is_active_playback_state(state);
}

static bool renderer_blocks_new_cast(upnp_cast_session_t *session,
                                     const char *source)
{
    if (!renderer_is_busy(session))
        return false;

    if (session->casting && source != NULL
     && strcmp(session->active_source, source) == 0)
        return false;

    return true;
}

static bool cast_session_ready(upnp_cast_session_t *session)
{
    return session != NULL && session->device.av_control != NULL;
}

static int ensure_cast_enabled(demux_t *demux, upnp_demux_sys_t *sys)
{
    if (sys->enabled)
        return VLC_SUCCESS;

    if (!cast_session_ready(sys->session) || !cast_input_active(demux, sys))
        return VLC_SUCCESS;

    bind_cast_input(demux, sys);
    sys->enabled = true;
    return VLC_SUCCESS;
}

static int start_cast_uri(demux_t *demux, upnp_demux_sys_t *sys,
                          const char *source)
{
    if (source == NULL)
    {
        msg_Err(demux, "No media URI for UPnP cast");
        return CAST_START_ERR;
    }

    if (renderer_blocks_new_cast(sys->session, source))
    {
        upnp_av_stop(sys->session->device.av_control);
        sys->session->casting = false;
        sys->session->active_source[0] = '\0';
        usleep(UPNP_CAST_SETTLE_US);
    }
    else if (renderer_is_busy(sys->session))
    {
        /* Bose may be in QPlay or paused — stop before SetURI. */
        upnp_av_stop(sys->session->device.av_control);
        sys->session->casting = false;
        sys->session->active_source[0] = '\0';
        usleep(UPNP_CAST_SETTLE_US);
    }

    resolve_display_title(demux, sys, source);

    sys->length = 0;
    probe_length_from_next(demux, sys);
    probe_length_from_uri(source, sys);
    probe_length_from_input(demux, sys);

    if (upnp_cast_start(sys->session, source, sys->display_title,
                        sys->length) != 0)
    {
        msg_Err(demux, "Failed to cast '%s' to UPnP renderer", source);
        return CAST_START_ERR;
    }

    sys->seen_playing = false;
    sys->renderer_confirmed = false;
    sys->track_active = true;
    sys->cast_start = mdate();
    sys->play_confirmed_at = 0;
    sys->time = 0;
    sys->seek_target = 0;
    sys->seek_until = 0;
    sys->stop_sent = false;
    sys->ui_length_set = false;
    sys->last_display_push = 0;

    push_display_title(demux, sys);

    sys->pending_default_volume = true;
    apply_default_cast_volume(sys);
    attach_volume_sync(demux, sys);

    msg_Info(demux, "Casting '%s' to %s via %s", source,
             sys->session->device.friendly_name, sys->session->media_url);
    return CAST_START_OK;
}

static int start_cast(demux_t *demux, upnp_demux_sys_t *sys)
{
    return start_cast_uri(demux, sys, get_source_uri(demux));
}

static void reset_track_state(upnp_demux_sys_t *sys)
{
    clear_display_title(sys);
    sys->track_active = false;
    sys->seen_playing = false;
    sys->renderer_confirmed = false;
    sys->renderer_paused = false;
    sys->cast_start = 0;
    sys->play_confirmed_at = 0;
    sys->time = 0;
    sys->length = 0;
    sys->seek_target = 0;
    sys->seek_until = 0;
    sys->ui_length_set = false;
    sys->display_title[0] = '\0';
    sys->last_display_push = 0;
}

static int poll_renderer_playback(demux_t *demux, upnp_demux_sys_t *sys)
{
    char state[64];

    if (!sys->track_active || sys->cast_start <= 0)
        return VLC_DEMUXER_SUCCESS;

    if (upnp_av_get_transport_state(sys->session->device.av_control,
                                    state, sizeof(state)) != 0)
        return VLC_DEMUXER_SUCCESS;

    if (cast_seek_pending(sys) && strcmp(state, "PLAYING") != 0
     && vlc_input_playing(sys))
    {
        char target[32];
        if (upnp_format_hms_duration(sys->seek_target, target, sizeof(target)) == 0)
            (void)ensure_renderer_playing(demux, sys, target, true);
    }

    if (strcmp(state, "PLAYING") == 0)
    {
        if (!sys->renderer_confirmed)
        {
            sys->renderer_confirmed = true;
            sys->play_confirmed_at = mdate();
            sys->time = 0;
            msg_Dbg(demux, "UPnP renderer confirmed playback");
            apply_default_cast_volume(sys);
        }
        sys->seen_playing = true;
        sys->renderer_paused = false;
    }
    else if (strcmp(state, "PAUSED_PLAYBACK") == 0
          || strcmp(state, "PAUSED") == 0)
    {
        sys->renderer_paused = true;
        if (!sys->renderer_confirmed)
            sys->renderer_confirmed = true;
    }

    if (sys->length <= 0)
        probe_length_from_next(demux, sys);

    if (sys->track_active)
    {
        maybe_refresh_display(demux, sys);
        update_position_from_renderer(sys);
        push_playback_times(demux, sys);
    }

    if (cast_playback_ended(sys, state))
    {
        msg_Dbg(demux, "UPnP cast track ended (state=%s, time=%" PRId64
                 ", length=%" PRId64 ")", state,
                 (int64_t)sys->time, (int64_t)sys->length);
        return VLC_DEMUXER_EOF;
    }

    if (!sys->renderer_confirmed
     && mdate() - sys->cast_start > CAST_PLAY_TIMEOUT
     && is_ended_state(state))
    {
        msg_Warn(demux, "UPnP renderer did not start playback (state=%s)",
                 state);
        return VLC_DEMUXER_EOF;
    }

    return VLC_DEMUXER_SUCCESS;
}

/*
 * Let VLC's directory demux post folder children to the playlist once, then
 * keep the folder input alive until the playlist switches to the first child.
 * Each child file is cast individually via demux_file_cast().
 */
static int demux_directory_expand(demux_t *demux, upnp_demux_sys_t *sys)
{
    if (!sys->directory_expanded && demux->p_next != NULL)
    {
        int ret = demux_Demux(demux->p_next);

        if (ret < 0 && ret != VLC_DEMUXER_EOF)
            return ret;

        sys->directory_expanded = true;
        msg_Dbg(demux, "UPnP cast: expanded directory for playlist");
    }

    msleep(CAST_WAIT_DELAY);
    return VLC_DEMUXER_SUCCESS;
}

static upnp_cast_session_t *get_cast_session(demux_t *demux)
{
    upnp_cast_session_t *session = var_InheritAddress(demux, UPNP_CAST_VAR);
    if (session == NULL)
        session = &g_session;
    return session;
}

static int DemuxOpen(vlc_object_t *obj)
{
    demux_t *demux = (demux_t *)obj;
    upnp_demux_sys_t *sys = calloc(1, sizeof(*sys));
    if (sys == NULL)
        return VLC_ENOMEM;

    sys->demux = demux;
    sys->session = get_cast_session(demux);

    demux->p_sys = (demux_sys_t *)sys;
    demux->pf_demux = DemuxDemux;
    demux->pf_control = DemuxControl;

    sys->last_volume = UPNP_VOLUME_UNSET;
    sys->folder_mode = is_playlist_demux(demux);
    bind_cast_var_to_input(demux, sys->session);
    probe_length_from_next(demux, sys);

    if (!demux->b_preparsing)
        sys->enabled = !sys->folder_mode;

    if (sys->folder_mode)
        msg_Dbg(demux, "UPnP directory expand mode (playlist will play children)");

    return VLC_SUCCESS;
}

static void DemuxClose(vlc_object_t *obj)
{
    demux_t *demux = (demux_t *)obj;
    upnp_demux_sys_t *sys = (upnp_demux_sys_t *)demux->p_sys;

    /* Renderer casts set sout-keep; this is the main stop path for the Bose. */
    stop_remote_playback(sys);
    detach_volume_sync(sys);

    cache_filter_input(demux, sys);
    if (sys->session->owner_input == sys->input)
        sys->session->owner_input = NULL;
    free(sys);
}

static int demux_underlying(demux_t *demux, upnp_demux_sys_t *sys)
{
    if (demux->p_next == NULL || sys->local_eof)
        return VLC_DEMUXER_SUCCESS;

    int ret = demux_Demux(demux->p_next);
    if (ret == VLC_DEMUXER_EOF)
        sys->local_eof = true;
    else if (ret < 0)
        return ret;

    return VLC_DEMUXER_SUCCESS;
}

static int demux_file_cast(demux_t *demux, upnp_demux_sys_t *sys)
{
    if (!sys->enabled)
        return demux->p_next != NULL ? demux_Demux(demux->p_next)
                                     : VLC_DEMUXER_SUCCESS;

    bind_cast_var_to_input(demux, sys->session);

    if (!cast_input_active(demux, sys))
    {
        if (sys->track_active && !cast_seek_pending(sys))
            stop_remote_playback(sys);
        msleep(CAST_WAIT_DELAY);
        return VLC_DEMUXER_SUCCESS;
    }

    if (!cast_session_ready(sys->session))
    {
        msleep(CAST_WAIT_DELAY);
        return VLC_DEMUXER_SUCCESS;
    }

    (void)ensure_cast_enabled(demux, sys);

    if (!sys->track_active)
    {
        if (sys->length <= 0)
            probe_length_from_next(demux, sys);

        int rc = start_cast(demux, sys);
        if (rc == CAST_START_WAIT)
        {
            msleep(CAST_WAIT_DELAY);
            return VLC_DEMUXER_SUCCESS;
        }
        if (rc != CAST_START_OK)
        {
            msleep(CAST_WAIT_DELAY);
            return VLC_DEMUXER_SUCCESS;
        }
    }

    if (sys->length <= 0)
        probe_length_from_next(demux, sys);

    if (sys->track_active)
        (void)demux_underlying(demux, sys);

    if (poll_renderer_playback(demux, sys) == VLC_DEMUXER_EOF)
    {
        reset_track_state(sys);
        sys->local_eof = false;
        return VLC_DEMUXER_EOF;
    }

    /* Local file drained; keep input alive until Bose finishes (Chromecast-style). */
    if (sys->local_eof)
    {
        msleep(CAST_WAIT_DELAY);
        return VLC_DEMUXER_SUCCESS;
    }

    msleep(CAST_WAIT_DELAY);
    return VLC_DEMUXER_SUCCESS;
}

static int DemuxDemux(demux_t *demux)
{
    upnp_demux_sys_t *sys = (upnp_demux_sys_t *)demux->p_sys;

    if (sys->folder_mode)
        return demux_directory_expand(demux, sys);

    return demux_file_cast(demux, sys);
}

static int DemuxControl(demux_t *demux, int query, va_list args)
{
    upnp_demux_sys_t *sys = (upnp_demux_sys_t *)demux->p_sys;
    demux_t *next = demux->p_next;

    if (!sys->enabled && !cast_session_active(sys)
     && query != DEMUX_FILTER_ENABLE && query != DEMUX_FILTER_DISABLE)
        return demux_vaControl(next, query, args);

    switch (query)
    {
        case DEMUX_FILTER_ENABLE:
            bind_cast_var_to_input(demux, sys->session);
            if (!sys->folder_mode)
            {
                sys->enabled = true;
                bind_cast_input(demux, sys);
            }
            return VLC_SUCCESS;

        case DEMUX_FILTER_DISABLE:
            stop_remote_playback(sys);
            detach_volume_sync(sys);
            sys->enabled = false;
            cache_filter_input(demux, sys);
            if (sys->session->owner_input == sys->input)
                sys->session->owner_input = NULL;
            return VLC_SUCCESS;

        case DEMUX_SET_PAUSE_STATE:
        {
            int paused = va_arg(args, int);

            if (cast_seek_pending(sys))
                return VLC_SUCCESS;

            cache_filter_input(demux, sys);

            if (paused)
            {
                if (upnp_av_pause(sys->session->device.av_control) == 0)
                    sys->renderer_paused = true;
            }
            else
            {
                if (upnp_av_play(sys->session->device.av_control) == 0)
                    sys->renderer_paused = false;
            }

            if (sys->input != NULL)
                input_Control(sys->input, INPUT_SET_STATE,
                              paused ? PAUSE_S : PLAYING_S);
            return VLC_SUCCESS;
        }

        case DEMUX_CAN_CONTROL_PACE:
        {
            bool *val = va_arg(args, bool *);
            *val = false;
            return VLC_SUCCESS;
        }

        case DEMUX_CAN_PAUSE:
        case DEMUX_CAN_SEEK:
        {
            bool *val = va_arg(args, bool *);
            *val = true;
            return VLC_SUCCESS;
        }

        case DEMUX_GET_LENGTH:
        {
            int64_t *val = va_arg(args, int64_t *);

            if (sys->length <= 0)
                probe_length_from_next(demux, sys);

            if (next != NULL
             && demux_Control(next, DEMUX_GET_LENGTH, val) == VLC_SUCCESS
             && *val > 0)
            {
                sys->length = *val;
                return VLC_SUCCESS;
            }
            if (sys->length > 0)
            {
                *val = sys->length;
                return VLC_SUCCESS;
            }
            *val = 0;
            return VLC_EGENERIC;
        }

        case DEMUX_GET_TIME:
        {
            int64_t *val = va_arg(args, int64_t *);

            if (sys->length <= 0)
                probe_length_from_next(demux, sys);

            if (sys->track_active)
                update_position_from_renderer(sys);

            if (sys->track_active)
            {
                *val = sys->time;
                push_playback_times(demux, sys);
                return VLC_SUCCESS;
            }
            if (next != NULL && demux_Control(next, DEMUX_GET_TIME, val) == VLC_SUCCESS)
                return VLC_SUCCESS;
            *val = 0;
            return VLC_SUCCESS;
        }

        case DEMUX_GET_POSITION:
        {
            double *val = va_arg(args, double *);

            if (sys->length <= 0)
                probe_length_from_next(demux, sys);

            if (sys->track_active)
                update_position_from_renderer(sys);

            if (sys->length > 0 && sys->track_active)
            {
                *val = (double)sys->time / (double)sys->length;
                if (*val < 0.0)
                    *val = 0.0;
                if (*val > 1.0)
                    *val = 1.0;
                push_playback_times(demux, sys);
                return VLC_SUCCESS;
            }
            *val = 0.0;
            return VLC_SUCCESS;
        }

        case DEMUX_SET_TIME:
        {
            int64_t time = va_arg(args, int64_t);
            bool precise = va_arg(args, int);
            VLC_UNUSED(precise);

            if (!cast_session_active(sys))
                break;

            ensure_cast_length(demux, sys);

            return seek_remote_playback(demux, sys, time);
        }

        case DEMUX_SET_POSITION:
        {
            double pos = va_arg(args, double);
            bool precise = va_arg(args, int);
            VLC_UNUSED(precise);

            if (!cast_session_active(sys))
                break;

            ensure_cast_length(demux, sys);
            if (sys->length <= 0)
            {
                msg_Warn(demux, "UPnP seek ignored: unknown track length");
                return VLC_EGENERIC;
            }

            pos = VLC_CLIP(pos, 0.0, 1.0);
            int64_t time = (int64_t)(pos * (double)sys->length);

            return seek_remote_playback(demux, sys, time);
        }

        default:
            break;
    }

    if (next != NULL)
        return demux_vaControl(next, query, args);

    return VLC_EGENERIC;
}