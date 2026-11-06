/* binkw32.dll stand-in (DESIGN.md decision 2): the 8 functions srEXT_BINK.dll imports, over
   FFmpeg's Bink demuxer and decoders. The handle the program gets is a block of program memory
   with the fields srEXT_BINK reads (DESIGN.md "Movies": width +0, height +4, frames +0x10,
   current frame +0x14 (1-based), frame rate +0x1c / +0x20); the decoder state stays here.
   Sound plays on an SDL audio stream of its own as the frames are decoded. */
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <SDL3/SDL.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include "runtime.h"
#include "options.h"

#define BINK_HANDLE_SIZE 0x200

typedef struct {
    uint32_t handle;                     /* the program's block */
    AVFormatContext *fc;
    int vi, ai;                          /* stream indexes (ai < 0: none) */
    AVCodecContext *vc, *ac;
    AVFrame *frame;                      /* the last decoded picture */
    int decoded;                         /* its number (1-based), 0 none */
    int want_seek;
    struct SwsContext *sws;
    int sws_fmt;
    SwrContext *swr;
    SDL_AudioStream *audio;
    double fps;
} Bink;

static Bink *binks[8];
static int sound_on = 1;

static Bink *bink_of(uint32_t h)
{
    for (int i = 0; i < 8; i++)
        if (binks[i] && binks[i]->handle == h)
            return binks[i];
    return NULL;
}

static void bink_free(Bink *b)
{
    if (b->audio) SDL_DestroyAudioStream(b->audio);
    sws_freeContext(b->sws);
    swr_free(&b->swr);
    av_frame_free(&b->frame);
    avcodec_free_context(&b->vc);
    avcodec_free_context(&b->ac);
    avformat_close_input(&b->fc);
    if (b->handle) gfree(b->handle);
    free(b);
}

static AVCodecContext *open_codec(AVFormatContext *fc, int si)
{
    const AVCodec *c = avcodec_find_decoder(fc->streams[si]->codecpar->codec_id);
    AVCodecContext *cc = c ? avcodec_alloc_context3(c) : NULL;
    if (!cc || avcodec_parameters_to_context(cc, fc->streams[si]->codecpar) < 0 || avcodec_open2(cc, c, NULL) < 0) {
        avcodec_free_context(&cc);
        return NULL;
    }
    return cc;
}

API(BINKW32, _BinkOpen_8, 2)
{
    const char *name = gstr(ARG(0));
    char path[2048];
    int slot = -1;
    for (int i = 0; i < 8; i++)
        if (!binks[i]) { slot = i; break; }
    if (slot < 0)
        return 0;
    if (getenv("TDOR3_BINK_FAIL")) {            /* (debugging: a movie that cannot be opened) */
        LOG(LOG_ALWAYS, "BinkOpen(\"%s\"): failing (TDOR3_BINK_FAIL)", name ? name : "");
        return 0;
    }
    const char *base = name ? strrchr(name, '\\') : NULL;
    base = base ? base + 1 : name ? name : "";
    int intro = !strcasecmp(base, "Legals.bik") || !strcasecmp(base, "Intro.bik");
    if (!option_bool("videos", 1) || (intro && !option_bool("intro_videos", 1))) {
        /* videos off: a movie of one black frame (a failed open is an exception the game
           does not catch: srIOManager::Error) */
        Bink *b = calloc(1, sizeof *b);
        b->handle = galloc(BINK_HANDLE_SIZE);
        memset(gptr(b->handle), 0, BINK_HANDLE_SIZE);
        W32(b->handle + 0x00, 640);
        W32(b->handle + 0x04, 480);
        W32(b->handle + 0x10, 1);
        W32(b->handle + 0x14, 1);
        W32(b->handle + 0x1c, 15);
        W32(b->handle + 0x20, 1);
        b->fps = 15;
        binks[slot] = b;
        LOG(LOG_ALWAYS, "BinkOpen(\"%s\"): %s off, one black frame", name ? name : "", intro ? "intro videos" : "videos");
        return b->handle;
    }
    if (!name || !path_to_host(name, path, sizeof path, 0)) {
        LOG(LOG_ALWAYS, "BinkOpen(\"%s\"): not found", name ? name : "");
        return 0;
    }
    Bink *b = calloc(1, sizeof *b);
    av_log_set_level(AV_LOG_ERROR);
    if (avformat_open_input(&b->fc, path, NULL, NULL) < 0 || avformat_find_stream_info(b->fc, NULL) < 0) {
        LOG(LOG_ALWAYS, "BinkOpen(\"%s\"): FFmpeg cannot read it", path);
        bink_free(b);
        return 0;
    }
    b->vi = av_find_best_stream(b->fc, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    b->ai = av_find_best_stream(b->fc, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (b->vi < 0 || !(b->vc = open_codec(b->fc, b->vi))) {
        bink_free(b);
        return 0;
    }
    if (b->ai >= 0 && sound_on && !headless && (b->ac = open_codec(b->fc, b->ai))) {
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        swr_alloc_set_opts2(&b->swr, &stereo, AV_SAMPLE_FMT_FLT, 44100, &b->ac->ch_layout, b->ac->sample_fmt,
                            b->ac->sample_rate, 0, NULL);
        swr_init(b->swr);
        SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, 44100};
        b->audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
        if (b->audio) SDL_ResumeAudioStreamDevice(b->audio);
    }
    AVStream *vs = b->fc->streams[b->vi];
    AVRational fr = vs->avg_frame_rate.num ? vs->avg_frame_rate : vs->r_frame_rate;
    if (!fr.num || !fr.den) fr = (AVRational){15, 1};
    b->fps = av_q2d(fr);
    int64_t frames = vs->nb_frames > 0 ? vs->nb_frames
                                       : (int64_t)(b->fc->duration / (double)AV_TIME_BASE * b->fps + 0.5);
    if (frames < 1) frames = 1;
    b->frame = av_frame_alloc();
    b->handle = galloc(BINK_HANDLE_SIZE);
    memset(gptr(b->handle), 0, BINK_HANDLE_SIZE);
    W32(b->handle + 0x00, (uint32_t)b->vc->width);
    W32(b->handle + 0x04, (uint32_t)b->vc->height);
    W32(b->handle + 0x10, (uint32_t)frames);
    W32(b->handle + 0x14, 1);            /* FrameNum: the first frame next */
    W32(b->handle + 0x1c, (uint32_t)fr.num);
    W32(b->handle + 0x20, (uint32_t)fr.den);
    binks[slot] = b;
    LOG(LOG_ALWAYS, "BinkOpen(\"%s\") -> %08x: %dx%d, %lld frames at %.2f fps%s", path, b->handle,
        b->vc->width, b->vc->height, (long long)frames, b->fps, b->audio ? ", with sound" : "");
    return b->handle;
}

API(BINKW32, _BinkClose_4, 1)
{
    for (int i = 0; i < 8; i++)
        if (binks[i] && binks[i]->handle == ARG(0)) {
            bink_free(binks[i]);
            binks[i] = NULL;
        }
    return 0;
}

/* Decodes up to frame n (1-based), feeding the sound on the way. */
static void decode_to(Bink *b, int n)
{
    if (!b->fc)
        return;
    if (b->want_seek || n <= b->decoded) {
        int64_t ts = (int64_t)((n - 1) / b->fps / av_q2d(b->fc->streams[b->vi]->time_base));
        av_seek_frame(b->fc, b->vi, ts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(b->vc);
        if (b->ac) avcodec_flush_buffers(b->ac);
        if (b->audio) SDL_ClearAudioStream(b->audio);
        b->decoded = 0;
        b->want_seek = 0;
    }
    AVPacket *pkt = av_packet_alloc();
    AVFrame *af = av_frame_alloc();
    while (b->decoded < n) {
        if (av_read_frame(b->fc, pkt) < 0)
            break;
        if (pkt->stream_index == b->ai && b->ac) {
            if (avcodec_send_packet(b->ac, pkt) >= 0)
                while (avcodec_receive_frame(b->ac, af) == 0) {
                    int on = swr_get_out_samples(b->swr, af->nb_samples);
                    float *out = malloc((size_t)on * 2 * sizeof(float));
                    uint8_t *o[1] = {(uint8_t *)out};
                    int got = swr_convert(b->swr, o, on, (const uint8_t **)af->extended_data, af->nb_samples);
                    if (got > 0 && b->audio)
                        SDL_PutAudioStreamData(b->audio, out, got * 2 * (int)sizeof(float));
                    free(out);
                }
        } else if (pkt->stream_index == b->vi) {
            /* (a failed receive empties the frame it is given: decode into another) */
            if (avcodec_send_packet(b->vc, pkt) >= 0)
                while (avcodec_receive_frame(b->vc, af) == 0) {
                    double t = af->pts != AV_NOPTS_VALUE ? af->pts * av_q2d(b->fc->streams[b->vi]->time_base) : 0;
                    int num = (int)(t * b->fps + 0.5) + 1;
                    b->decoded = num > b->decoded ? num : b->decoded + 1;
                    av_frame_unref(b->frame);
                    av_frame_move_ref(b->frame, af);
                }
        }
        av_packet_unref(pkt);
    }
    av_frame_free(&af);
    av_packet_free(&pkt);
}

API(BINKW32, _BinkDoFrame_4, 1)
{
    Bink *b = bink_of(ARG(0));
    if (!b) return 0;
    decode_to(b, (int)R32(b->handle + 0x14));
    return 0;
}

API(BINKW32, _BinkNextFrame_4, 1)
{
    Bink *b = bink_of(ARG(0));
    if (!b) return 0;
    uint32_t n = R32(b->handle + 0x14) + 1;
    if (n > R32(b->handle + 0x10)) n = 1;
    W32(b->handle + 0x14, n);
    return 0;
}

API(BINKW32, _BinkGoto_12, 3)
{
    Bink *b = bink_of(ARG(0));
    if (!b) return 0;
    uint32_t n = ARG(1);
    if (n < 1) n = 1;
    if (n != (uint32_t)b->decoded + 1)
        b->want_seek = n <= (uint32_t)b->decoded;
    W32(b->handle + 0x14, n);
    return 0;
}

/* BinkCopyToBuffer(bink, dest, pitch, height, x, y, flags): flags & 0xf the surface type as
   srEXT_BINK asks for its SurRender surface formats (0xc -> 1, 0xd -> 2, 8 -> 3, 7 -> 4):
   1 24-bit, 2 32-bit (bytes B, G, R, A: SurRender's ARGB8888), 3 15-bit, 4 16-bit. (Type 2 as
   24-bit filled only three quarters of each row of the 640-wide legal screen.) */
API(BINKW32, _BinkCopyToBuffer_28, 7)
{
    Bink *b = bink_of(ARG(0));
    if (b && !b->fc && ARG(1)) {             /* the black frame */
        int bpp = (ARG(6) & 0xf) == 1 ? 3 : (ARG(6) & 0xf) == 2 ? 4 : 2;
        uint32_t h = ARG(3) < 480 ? ARG(3) : 480;
        for (uint32_t y = 0; y < h; y++)
            memset(gptr(ARG(1) + (ARG(5) + y) * ARG(2) + ARG(4) * (uint32_t)bpp), 0, ARG(2) < 640u * (uint32_t)bpp ? ARG(2) : 640u * (uint32_t)bpp);
        return 0;
    }
    if (!b || !b->decoded || !ARG(1) || !b->frame->data[0]) return 0;
    int type = (int)(ARG(6) & 0xf);
    enum AVPixelFormat pf = type == 1 ? AV_PIX_FMT_BGR24 : type == 2 ? AV_PIX_FMT_BGRA
                          : type == 3 ? AV_PIX_FMT_RGB555LE : AV_PIX_FMT_RGB565LE;
    int w = b->vc->width, h = b->vc->height;
    if ((int)ARG(3) < h) h = (int)ARG(3);
    if (!b->sws || b->sws_fmt != pf) {
        sws_freeContext(b->sws);
        b->sws = sws_getContext(b->vc->width, b->vc->height, b->frame->format, b->vc->width, b->vc->height, pf,
                                SWS_BILINEAR, NULL, NULL, NULL);
        b->sws_fmt = pf;
    }
    int bpp = type == 1 ? 3 : type == 2 ? 4 : 2;
    uint8_t *dst[1] = {(uint8_t *)gptr(ARG(1)) + ARG(5) * ARG(2) + ARG(4) * (uint32_t)bpp};
    int pitch[1] = {(int)ARG(2)};
    sws_scale(b->sws, (const uint8_t *const *)b->frame->data, b->frame->linesize, 0, h, dst, pitch);
    (void)w;
    return 0;
}

API(BINKW32, _BinkOpenDirectSound_4, 1) { return 1; }
API(BINKW32, _BinkSetSoundSystem_8, 2) { sound_on = 1; return 1; }

void bink_init(void)
{
}
