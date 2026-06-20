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
#include <vlc_configuration.h>
#include <vlc_url.h>

#include "../include/upnp_cast.h"
#include "../include/upnp_common.h"
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
     * With sout-keep the instance can outlive this stream; only tear down the
     * cast session when nothing is still using it.
     */
    if (g_session.owner_input == NULL)
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
    bool enabled;
    bool folder_mode;
    bool directory_expanded;
    bool track_active;
    bool local_eof;
    bool renderer_confirmed;
    bool seen_playing;
    vlc_tick_t cast_start;
    vlc_tick_t play_confirmed_at;
    vlc_tick_t length;
    vlc_tick_t time;
} upnp_demux_sys_t;

#define CAST_PLAY_TIMEOUT (120 * CLOCK_FREQ)
#define CAST_EOF_MARGIN   (3 * CLOCK_FREQ)
#define CAST_MIN_PLAY     (5 * CLOCK_FREQ)
#define CAST_WAIT_DELAY   (CLOCK_FREQ / 2)

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

static void probe_length_from_next(demux_t *demux, upnp_demux_sys_t *sys)
{
    int64_t len = 0;

    if (demux->p_next != NULL
     && demux_Control(demux->p_next, DEMUX_GET_LENGTH, &len) == VLC_SUCCESS
     && len > 0)
        sys->length = len;
}

static void update_position_from_renderer(upnp_demux_sys_t *sys)
{
    if (sys->session->device.av_control == NULL)
        return;

    char rel[32], dur[32];
    if (upnp_av_get_position_info(sys->session->device.av_control,
                                  rel, sizeof(rel), dur, sizeof(dur)) != 0)
        return;

    int64_t ticks;
    if (upnp_parse_hms_duration(rel, &ticks) == 0)
        sys->time = ticks;
    if (sys->length <= 0 && upnp_parse_hms_duration(dur, &ticks) == 0)
        sys->length = ticks;
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

static bool input_has_ended(input_thread_t *input)
{
    if (input == NULL)
        return true;

    return input_GetState(input) == END_S;
}

static bool cast_input_active(demux_t *demux)
{
    input_thread_t *input = get_filter_input(demux);

    return input != NULL && !demux->b_preparsing && !input_has_ended(input);
}

static void bind_cast_input(demux_t *demux, upnp_demux_sys_t *sys)
{
    input_thread_t *input = get_filter_input(demux);

    if (input != NULL && !demux->b_preparsing)
        sys->session->owner_input = input;
}

static bool cast_playback_ended(upnp_demux_sys_t *sys, const char *state)
{
    if (!sys->renderer_confirmed || !is_ended_state(state))
        return false;

    if (sys->play_confirmed_at > 0
     && mdate() - sys->play_confirmed_at < CAST_MIN_PLAY)
        return false;

    if (sys->length > CAST_EOF_MARGIN
     && sys->time + CAST_EOF_MARGIN < sys->length)
        return false;

    return true;
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

    if (!cast_session_ready(sys->session) || !cast_input_active(demux))
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
        usleep(300000);
    }
    else if (!sys->session->casting && renderer_is_busy(sys->session))
    {
        upnp_av_stop(sys->session->device.av_control);
        usleep(300000);
    }

    if (upnp_cast_start(sys->session, source) != 0)
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
    sys->length = 0;
    probe_length_from_next(demux, sys);

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
    sys->track_active = false;
    sys->seen_playing = false;
    sys->renderer_confirmed = false;
    sys->cast_start = 0;
    sys->play_confirmed_at = 0;
    sys->time = 0;
    sys->length = 0;
}

static int poll_renderer_playback(demux_t *demux, upnp_demux_sys_t *sys)
{
    char state[64];

    if (!sys->track_active || sys->cast_start <= 0)
        return VLC_DEMUXER_SUCCESS;

    if (upnp_av_get_transport_state(sys->session->device.av_control,
                                    state, sizeof(state)) != 0)
        return VLC_DEMUXER_SUCCESS;

    if (strcmp(state, "PLAYING") == 0)
    {
        if (!sys->renderer_confirmed)
        {
            sys->renderer_confirmed = true;
            sys->play_confirmed_at = mdate();
            sys->time = 0;
            msg_Dbg(demux, "UPnP renderer confirmed playback");
        }
        sys->seen_playing = true;
    }

    if (sys->renderer_confirmed)
        update_position_from_renderer(sys);

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

    /*
     * Do not stop the renderer on close when it is still playing — a premature
     * local EOF must not cut off Bose or unblock the next queued cast.
     */
    if (sys->enabled && sys->session->owner_input == get_filter_input(demux)
     && !renderer_is_busy(sys->session))
        upnp_cast_stop(sys->session);

    if (sys->session->owner_input == get_filter_input(demux))
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

    if (!cast_input_active(demux))
    {
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

    if (!sys->enabled && query != DEMUX_FILTER_ENABLE
     && query != DEMUX_FILTER_DISABLE)
        return demux_vaControl(next, query, args);

    switch (query)
    {
        case DEMUX_FILTER_ENABLE:
            bind_cast_var_to_input(demux, sys->session);
            sys->enabled = true;
            bind_cast_input(demux, sys);
            return VLC_SUCCESS;

        case DEMUX_FILTER_DISABLE:
            if (sys->enabled && sys->session->owner_input == get_filter_input(demux))
                upnp_cast_stop(sys->session);
            sys->enabled = false;
            if (sys->session->owner_input == get_filter_input(demux))
                sys->session->owner_input = NULL;
            return VLC_SUCCESS;

        case DEMUX_SET_PAUSE_STATE:
        {
            int paused = va_arg(args, int);
            if (paused)
                upnp_av_pause(sys->session->device.av_control);
            else
                upnp_av_play(sys->session->device.av_control);
            return VLC_SUCCESS;
        }

        case DEMUX_CAN_PAUSE:
        case DEMUX_CAN_CONTROL_PACE:
        case DEMUX_CAN_SEEK:
        {
            bool *val = va_arg(args, bool *);
            *val = true;
            return VLC_SUCCESS;
        }

        case DEMUX_GET_LENGTH:
        {
            int64_t *val = va_arg(args, int64_t *);

            if (next != NULL
             && demux_Control(next, DEMUX_GET_LENGTH, val) == VLC_SUCCESS)
            {
                if (*val > 0)
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

            if (sys->enabled)
            {
                if (sys->track_active && sys->renderer_confirmed)
                    update_position_from_renderer(sys);
                *val = (sys->track_active && sys->time > 0) ? sys->time : 0;
                return VLC_SUCCESS;
            }
            if (sys->seen_playing)
                update_position_from_renderer(sys);
            if (sys->time > 0)
            {
                *val = sys->time;
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

            if (sys->enabled)
            {
                if (sys->track_active && sys->renderer_confirmed)
                    update_position_from_renderer(sys);
                if (sys->track_active && sys->length > 0 && sys->time > 0)
                    *val = (double)sys->time / (double)sys->length;
                else
                    *val = 0.0;
                return VLC_SUCCESS;
            }
            break;
        }

        case DEMUX_SET_TIME:
        {
            int64_t time = va_arg(args, int64_t);
            bool precise = va_arg(args, int);
            VLC_UNUSED(precise);

            int sec = (int)(time / CLOCK_FREQ);
            int min = sec / 60;
            sec %= 60;
            int hour = min / 60;
            min %= 60;

            char target[32];
            snprintf(target, sizeof(target), "%02d:%02d:%02d", hour, min, sec);
            upnp_av_seek_rel(sys->session->device.av_control, target);
            sys->time = time;
            return VLC_SUCCESS;
        }

        default:
            break;
    }

    if (next != NULL)
        return demux_vaControl(next, query, args);

    return VLC_EGENERIC;
}