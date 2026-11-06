/* CD music (DESIGN.md decision 4): the game plays its soundtrack from the CD's audio tracks
   through MCI (cdaudio: open, TMSF time format, track lengths and positions, mode, play, stop)
   and sets its volume on the mixer's CD-audio line (0..65535, tdor3.cfg's MusicVolume). Here
   the CD is the user's MUSIC/TrackNN.ogg files (track 1 is the data track; 02..11 the music,
   as the ogg-winmm setup names them), decoded with FFmpeg and played on an SDL audio stream
   of their own, the mixer's volume its gain. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <SDL3/SDL.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include "runtime.h"
#include "options.h"
#include "win.h"

#define MAX_TRACKS 99
#define OUT_RATE 44100
#define CD_DEVICE_ID 1

typedef struct {
    char path[1100];
    double seconds;                      /* its length (0: no file) */
} Track;
static Track tracks[MAX_TRACKS + 1];     /* 1-based: tracks[1] is the data track */
static int ntracks;                      /* the CD's last track */
static int scanned;

static pthread_mutex_t mlock = PTHREAD_MUTEX_INITIALIZER;
static int device_open;
static uint32_t time_format = 0;         /* MCI_FORMAT_MILLISECONDS until set */

/* playback */
static SDL_AudioStream *stream;
static pthread_t dec_thread;
static int dec_running, dec_stop;
static int play_track, play_end_track;   /* playing [play_track .. play_end_track) */
static double play_start_s;              /* where in play_track it started */
static double play_end_s;                /* stop when the last track reaches this (0: its end) */
static double pushed_s;                  /* seconds of play_track given to the stream so far, from play_start_s */
static int playing, paused;
static uint32_t notify_hwnd;             /* MCI_NOTIFY's callback window, 0 none */
static double clock_start;               /* without an audio device: when play started (ms) */
static float music_gain = 0.75f;
static uint32_t mixer_volume = 49152;    /* the CD line's volume, 0..65535 */

/* ------------------------------------------------------------------ the tracks */
static double file_seconds(const char *path)
{
    AVFormatContext *fc = NULL;
    if (avformat_open_input(&fc, path, NULL, NULL) < 0)
        return 0;
    double s = 0;
    if (avformat_find_stream_info(fc, NULL) >= 0 && fc->duration > 0)
        s = fc->duration / (double)AV_TIME_BASE;
    avformat_close_input(&fc);
    return s;
}

static void scan_tracks(void)
{
    if (scanned)
        return;
    scanned = 1;
    av_log_set_level(AV_LOG_ERROR);
    ntracks = 1;
    for (int t = 2; t <= MAX_TRACKS; t++) {
        const char *dirs[] = {"MUSIC", "Music", "music"};
        int found = 0;
        for (int d = 0; d < 3 && !found; d++) {
            snprintf(tracks[t].path, sizeof tracks[t].path, "%s/%s/Track%02d.ogg", game_dir, dirs[d], t);
            FILE *f = fopen(tracks[t].path, "rb");
            if (f) { fclose(f); found = 1; }
        }
        if (!found)
            break;
        tracks[t].seconds = file_seconds(tracks[t].path);
        ntracks = t;
    }
    tracks[1].seconds = 60;              /* the data track: a minute, never played */
    LOG(LOG_ALWAYS, "CD music: %d audio tracks from %s/MUSIC", ntracks - 1, game_dir);
}

/* ------------------------------------------------------------------ time formats */
#define MCI_FORMAT_MILLISECONDS 0
#define MCI_FORMAT_MSF 2
#define MCI_FORMAT_TMSF 10

static uint32_t to_msf(double s)
{
    uint32_t f = (uint32_t)(s * 75.0 + 0.5);
    return (f / 75 / 60) | ((f / 75 % 60) << 8) | ((f % 75) << 16);
}

static double track_start_disc(int t)    /* seconds from the disc's start */
{
    double s = 2.0;                      /* the 2-second lead-in */
    for (int i = 1; i < t && i <= ntracks; i++)
        s += tracks[i].seconds;
    return s;
}

/* a position in the current time format -> track and seconds into it */
static void decode_pos(uint32_t v, int *track, double *sec)
{
    if (time_format == MCI_FORMAT_TMSF) {
        *track = (int)(v & 0xff);
        *sec = ((v >> 8) & 0xff) * 60.0 + ((v >> 16) & 0xff) + ((v >> 24) & 0xff) / 75.0;
        return;
    }
    double s = time_format == MCI_FORMAT_MSF ? (v & 0xff) * 60.0 + ((v >> 8) & 0xff) + ((v >> 16) & 0xff) / 75.0
                                             : v / 1000.0;
    int t = 1;
    while (t < ntracks && s >= track_start_disc(t + 1))
        t++;
    *track = t;
    *sec = s - track_start_disc(t);
    if (*sec < 0) *sec = 0;
}

static uint32_t encode_pos(int track, double sec)
{
    if (time_format == MCI_FORMAT_TMSF) {
        uint32_t f = (uint32_t)(sec * 75.0);
        return (uint32_t)track | ((f / 75 / 60) << 8) | ((f / 75 % 60) << 16) | ((f % 75) << 24);
    }
    double s = track_start_disc(track) + sec;
    return time_format == MCI_FORMAT_MSF ? to_msf(s) : (uint32_t)(s * 1000.0);
}

/* ------------------------------------------------------------------ decoding */
static void stream_open(void)
{
    if (stream || headless)
        return;
    SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, OUT_RATE};
    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!stream) {
        LOG(LOG_ALWAYS, "CD music: no audio output (%s)", SDL_GetError());
        return;
    }
    SDL_SetAudioStreamGain(stream, music_gain);
    SDL_ResumeAudioStreamDevice(stream);
}

/* Decodes the tracks being played into the stream, keeping about a second queued. */
static void *decoder(void *arg)
{
    (void)arg;
    int t = play_track;
    double start = play_start_s;
    while (!dec_stop && t < play_end_track) {
        AVFormatContext *fc = NULL;
        if (avformat_open_input(&fc, tracks[t].path, NULL, NULL) < 0 || avformat_find_stream_info(fc, NULL) < 0) {
            LOG(LOG_ALWAYS, "CD music: cannot read %s", tracks[t].path);
            if (fc) avformat_close_input(&fc);
            break;
        }
        int si = av_find_best_stream(fc, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
        const AVCodec *codec = si >= 0 ? avcodec_find_decoder(fc->streams[si]->codecpar->codec_id) : NULL;
        AVCodecContext *cc = codec ? avcodec_alloc_context3(codec) : NULL;
        if (!cc || avcodec_parameters_to_context(cc, fc->streams[si]->codecpar) < 0 || avcodec_open2(cc, codec, NULL) < 0) {
            avcodec_free_context(&cc);
            avformat_close_input(&fc);
            break;
        }
        SwrContext *swr = NULL;
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_FLT, OUT_RATE, &cc->ch_layout, cc->sample_fmt,
                            cc->sample_rate, 0, NULL);
        swr_init(swr);
        if (start > 0.05)
            av_seek_frame(fc, -1, (int64_t)(start * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD);
        double end = (t == play_end_track - 1 && play_end_s > 0) ? play_end_s : 1e9;
        AVPacket *pkt = av_packet_alloc();
        AVFrame *fr = av_frame_alloc();
        float *out = NULL;
        int outcap = 0;
        double at = -1;                  /* seconds decoded so far in this track */
        while (!dec_stop && av_read_frame(fc, pkt) >= 0) {
            if (pkt->stream_index != si) { av_packet_unref(pkt); continue; }
            avcodec_send_packet(cc, pkt);
            av_packet_unref(pkt);
            while (!dec_stop && avcodec_receive_frame(cc, fr) == 0) {
                double fts = fr->pts != AV_NOPTS_VALUE ? fr->pts * av_q2d(fc->streams[si]->time_base) : at;
                if (at < 0) at = fts > 0 ? fts : 0;
                int n = swr_get_out_samples(swr, fr->nb_samples);
                if (n > outcap) { outcap = n; out = realloc(out, (size_t)outcap * 2 * sizeof(float)); }
                uint8_t *o[1] = {(uint8_t *)out};
                int got = swr_convert(swr, o, n, (const uint8_t **)fr->extended_data, fr->nb_samples);
                double dur = got / (double)OUT_RATE;
                int skip = 0;
                if (at < start) {        /* before the seek target */
                    skip = (int)((start - at) * OUT_RATE);
                    if (skip > got) skip = got;
                }
                int keep = got - skip;
                if (at + dur > end)
                    keep = (int)((end - at) * OUT_RATE) - skip;
                if (keep > 0) {
                    while (!dec_stop && stream && SDL_GetAudioStreamQueued(stream) > OUT_RATE * 8) {
                        struct timespec ts = {0, 20000000};
                        nanosleep(&ts, NULL);
                    }
                    if (stream)
                        SDL_PutAudioStreamData(stream, out + 2 * skip, keep * 2 * (int)sizeof(float));
                    pthread_mutex_lock(&mlock);
                    pushed_s += keep / (double)OUT_RATE;
                    pthread_mutex_unlock(&mlock);
                }
                at += dur;
                if (at >= end) { dec_stop = 2; break; }
            }
        }
        free(out);
        av_frame_free(&fr);
        av_packet_free(&pkt);
        swr_free(&swr);
        avcodec_free_context(&cc);
        avformat_close_input(&fc);
        if (dec_stop)
            break;
        t++;
        start = 0;
    }
    pthread_mutex_lock(&mlock);
    dec_running = 0;
    pthread_mutex_unlock(&mlock);
    return NULL;
}

static void stop_playback(void)
{
    if (dec_running) {
        dec_stop = 1;
        pthread_join(dec_thread, NULL);
    }
    dec_running = 0;
    dec_stop = 0;
    if (stream)
        SDL_ClearAudioStream(stream);
    playing = paused = 0;
}

static double played_seconds(void)       /* into the play, from play_start_s */
{
    if (!playing)
        return 0;
    if (!stream)
        return (now_ms() - clock_start) / 1000.0;
    pthread_mutex_lock(&mlock);
    double s = pushed_s - SDL_GetAudioStreamQueued(stream) / (double)(OUT_RATE * 2 * sizeof(float));
    pthread_mutex_unlock(&mlock);
    return s < 0 ? 0 : s;
}

/* where the play is now: track and seconds into it */
static void current_pos(int *track, double *sec)
{
    if (!playing && !paused) {
        *track = play_track ? play_track : 2;
        *sec = play_start_s;
        return;
    }
    double s = play_start_s + played_seconds();
    int t = play_track;
    while (t < ntracks && s >= tracks[t].seconds && t + 1 < play_end_track) {
        s -= tracks[t].seconds;
        t++;
    }
    *track = t;
    *sec = s;
}

/* The play reached its end: MCI_NOTIFY_SUCCESSFUL to the window that asked (checked each frame
   and on every MCI call). */
void music_poll(void)
{
    if (!playing)
        return;
    int done;
    if (stream)
        done = !dec_running && SDL_GetAudioStreamQueued(stream) == 0;
    else {
        double total = 0;
        for (int t = play_track; t < play_end_track; t++)
            total += tracks[t].seconds;
        total -= play_start_s;
        if (play_end_s > 0)
            total -= tracks[play_end_track - 1].seconds - play_end_s;
        done = played_seconds() >= total;
    }
    if (!done)
        return;
    LOG(LOG_SOUND, "CD music: track %d finished", play_track);
    stop_playback();
    if (notify_hwnd) {
        notify_hwnd = 0;
        post_message(0x03b9, 1, CD_DEVICE_ID);   /* MM_MCINOTIFY, MCI_NOTIFY_SUCCESSFUL */
    }
}

static void start_play(int t, double s, int end_t, double end_s)
{
    stop_playback();
    if (t < 2) { t = 2; s = 0; }
    if (t > ntracks)
        return;
    if (end_t > ntracks + 1 || end_t <= t) { end_t = ntracks + 1; end_s = 0; }
    play_track = t;
    play_start_s = s;
    play_end_track = end_t;
    play_end_s = end_s;
    pushed_s = 0;
    playing = 1;
    clock_start = now_ms();
    stream_open();
    if (stream || !headless) {
        dec_stop = 0;
        dec_running = 1;
        pthread_create(&dec_thread, NULL, decoder, NULL);
    }
    LOG(LOG_ALWAYS, "CD music: track %d from %.1f s", t, s);
}

/* ------------------------------------------------------------------ MCI */
#define MCI_OPEN 0x0803
#define MCI_CLOSE 0x0804
#define MCI_PLAY 0x0806
#define MCI_SEEK 0x0807
#define MCI_STOP 0x0808
#define MCI_PAUSE 0x0809
#define MCI_INFO 0x080a
#define MCI_GETDEVCAPS 0x080b
#define MCI_SET 0x080d
#define MCI_STATUS 0x0814
#define MCI_RESUME 0x0855
#define MCI_NOTIFY 0x1
#define MCI_FROM 0x4
#define MCI_TO 0x8
#define MCI_TRACK 0x10
#define MCI_STATUS_ITEM 0x100
#define MCI_SET_TIME_FORMAT 0x400

API(WINMM, mciSendCommandA, 4)
{
    uint32_t dev = ARG(0), msg = ARG(1), flags = ARG(2), p = ARG(3);
    scan_tracks();
    music_poll();
    uint32_t r = 0;
    switch (msg) {
    case MCI_OPEN: {
        /* MCI_OPEN_TYPE (0x2000) with MCI_OPEN_TYPE_ID (0x1000): MCI_DEVTYPE_CD_AUDIO (0x204), or "cdaudio" */
        uint32_t type = p ? R32(p + 8) : 0;
        int cd = (flags & 0x1000) ? (type & 0xffff) == 0x204
                                  : (flags & 0x2000) && type && gstr(type) && !strcasecmp(gstr(type), "cdaudio");
        if (!cd) { r = 263; break; }     /* MCIERR_INVALID_DEVICE_NAME */
        device_open = 1;
        time_format = MCI_FORMAT_MILLISECONDS;
        if (p) W32(p + 4, CD_DEVICE_ID);
        break;
    }
    case MCI_CLOSE:
        stop_playback();
        device_open = 0;
        break;
    case MCI_SET:
        if ((flags & MCI_SET_TIME_FORMAT) && p)
            time_format = R32(p + 4);
        break;                           /* doors (0x100, 0x200), audio channels: done */
    case MCI_PLAY: {
        int t = 2, et = ntracks + 1;
        double s = 0, es = 0;
        if (flags & MCI_FROM) decode_pos(R32(p + 4), &t, &s);
        else current_pos(&t, &s);
        if (flags & MCI_TO) {
            decode_pos(R32(p + 8), &et, &es);
            if (es < 0.05) es = 0;               /* to the start of a track: up to the one before it */
            else et++;
            if (et <= t) { et = t + 1; es = 0; }
        }
        notify_hwnd = (flags & MCI_NOTIFY) && p ? (R32(p) ? R32(p) : 1) : 0;
        start_play(t, s, et, es);
        break;
    }
    case MCI_SEEK: {
        int t = 2;
        double s = 0;
        stop_playback();
        if ((flags & MCI_TO) && p) decode_pos(R32(p + 4), &t, &s);
        play_track = t;
        play_start_s = s;
        break;
    }
    case MCI_STOP:
        stop_playback();
        notify_hwnd = 0;
        break;
    case MCI_PAUSE:
        if (playing) {
            int t; double s;
            current_pos(&t, &s);
            stop_playback();
            play_track = t;
            play_start_s = s;
            paused = 1;
        }
        break;
    case MCI_RESUME:
        if (paused)
            start_play(play_track, play_start_s, play_end_track, play_end_s);
        break;
    case MCI_STATUS: {
        if (!p || !(flags & MCI_STATUS_ITEM)) { r = 0x111; break; }   /* MCIERR_MISSING_PARAMETER */
        uint32_t item = R32(p + 8), track = R32(p + 12), v = 0;
        int t; double s;
        switch (item) {
        case 1:                          /* MCI_STATUS_LENGTH: of a track (MSF), or of the disc */
            if (flags & MCI_TRACK) v = track >= 1 && (int)track <= ntracks ? to_msf(tracks[track].seconds) : 0;
            else v = time_format == MCI_FORMAT_MILLISECONDS ? (uint32_t)(track_start_disc(ntracks + 1) * 1000)
                                                            : to_msf(track_start_disc(ntracks + 1));
            break;
        case 2:                          /* MCI_STATUS_POSITION: of a track's start, or now */
            if (flags & MCI_TRACK) v = encode_pos((int)track, 0);
            else { current_pos(&t, &s); v = encode_pos(t, s); }
            break;
        case 3: v = (uint32_t)ntracks; break;            /* MCI_STATUS_NUMBER_OF_TRACKS */
        case 4: v = playing ? 526 : paused ? 529 : 525; break;   /* MCI_STATUS_MODE: PLAY, PAUSE, STOP */
        case 5: v = 1; break;            /* MCI_STATUS_MEDIA_PRESENT */
        case 6: v = time_format; break;
        case 7: v = 1; break;            /* MCI_STATUS_READY */
        case 8: current_pos(&t, &s); v = (uint32_t)t; break;   /* MCI_STATUS_CURRENT_TRACK */
        case 0x4001: v = (flags & MCI_TRACK) && track == 1 ? 1 : 0; break;   /* MCI_CDA_STATUS_TYPE_TRACK: 0 audio, 1 data */
        default: r = 0x112; break;       /* MCIERR_UNSUPPORTED_FUNCTION */
        }
        W32(p + 4, v);
        break;
    }
    case MCI_GETDEVCAPS:
        if (p) W32(p + 4, 1);
        break;
    default:
        LOG(LOG_ALWAYS, "mciSendCommandA(dev %u, msg %x, flags %x): not handled", dev, msg, flags);
        break;
    }
    LOG(LOG_SOUND, "mciSendCommandA(%u, %x, %x) -> %u", dev, msg, flags, r);
    if (!r && (flags & MCI_NOTIFY) && msg != MCI_PLAY && p && R32(p))
        post_message(0x03b9, 1, CD_DEVICE_ID);
    return r;
}

/* ------------------------------------------------------------------ the mixer: one, with the CD
   line and its volume control (the game reads and sets the music volume there) */
#define LINE_CD 0x00010001u
#define CONTROL_CD_VOLUME 0x00000001u

void music_set_volume(uint32_t v)
{
    mixer_volume = v & 0xffff;
    music_gain = mixer_volume / 65535.0f;
    if (stream)
        SDL_SetAudioStreamGain(stream, music_gain);
}

API(WINMM, mixerGetNumDevs, 0) { return 1; }

API(WINMM, mixerGetLineInfoA, 3)
{
    uint32_t ml = ARG(1), how = ARG(2) & 0xf;
    if (how != 3 && how != 2 && how != 1) return 1024;   /* MIXERR_INVALLINE */
    if (how == 3 && R32(ml + 24) != 0x1005) return 1024; /* by component type: the CD only */
    W32(ml + 4, 0);                      /* dwDestination */
    W32(ml + 8, 0);                      /* dwSource */
    W32(ml + 12, LINE_CD);
    W32(ml + 16, 1);                     /* MIXERLINE_LINEF_ACTIVE */
    W32(ml + 24, 0x1005);                /* MIXERLINE_COMPONENTTYPE_SRC_COMPACTDISC */
    W32(ml + 28, 2);                     /* cChannels */
    W32(ml + 32, 0);
    W32(ml + 36, 1);                     /* cControls */
    gstrncpy(ml + 40, "CD Audio", 16);
    gstrncpy(ml + 56, "CD Audio", 64);
    return 0;
}

API(WINMM, mixerGetLineControlsA, 3)
{
    uint32_t mc = ARG(1);
    uint32_t ctl = R32(mc + 20);         /* pamxctrl */
    if (!ctl) return 11;                 /* MMSYSERR_INVALPARAM */
    memset(gptr(ctl), 0, 0x94);
    W32(ctl, 0x94);
    W32(ctl + 4, CONTROL_CD_VOLUME);
    W32(ctl + 8, 0x50030001);            /* MIXERCONTROL_CONTROLTYPE_VOLUME */
    gstrncpy(ctl + 20, "Volume", 16);
    gstrncpy(ctl + 36, "CD Volume", 64);
    W32(ctl + 100, 0);                   /* Bounds: 0..65535 */
    W32(ctl + 104, 65535);
    W32(ctl + 124, 65535 / 1024);        /* Metrics.cSteps */
    return 0;
}

API(WINMM, mixerGetControlDetailsA, 3)
{
    uint32_t d = ARG(1);
    uint32_t n = R32(d + 8) ? R32(d + 8) : 1, pa = R32(d + 20);
    for (uint32_t i = 0; pa && i < n; i++)
        W32(pa + 4 * i, mixer_volume);
    return 0;
}

API(WINMM, mixerSetControlDetails, 3)
{
    uint32_t d = ARG(1), pa = R32(d + 20);
    if (pa) {
        music_set_volume(R32(pa));
        LOG(LOG_SOUND, "music volume %u", mixer_volume);
    }
    return 0;
}

void music_stop(void)
{
    stop_playback();
}
