/**
 * manx_fmv.c — wall-clock-paced FMV playback via ffmpeg.
 *
 * See manx_fmv.h for the API. Container frame-rate metadata is never
 * trusted (Xbox XMV reports r_frame_rate as 1000/1), so pacing uses the
 * per-frame timestamps in the stream time base instead.
 */

#include "manx_fmv.h"

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct manx_fmv {
    AVFormatContext   *fmt;
    AVCodecContext    *dec;
    AVCodecContext    *adec;
    struct SwsContext *sws;
    struct SwrContext *swr;
    AVFrame           *pending;   /* decoded but not yet due for display */
    AVFrame           *audio_frame;
    AVPacket          *pkt;
    int                vstream;
    int                astream;
    int                out_w, out_h;
    enum AVPixelFormat out_pix;   /* BGRA or RGBA, fixed at open */
    uint8_t           *pixels;    /* latest displayed frame */
    int                have_pending;
    int                demux_eof; /* no more packets to read */
    int                eof;       /* decoder fully drained */
    double             t0;        /* wall clock at playback start; <0 = unset */
    double             pending_pts;
    int16_t           *audio_ring; /* 48 kHz stereo, SPSC producer/consumer */
    uint64_t           audio_capacity;
    _Atomic uint64_t   audio_read;
    _Atomic uint64_t   audio_write;
    int                audio_drained;
};

#define FMV_AUDIO_RATE 48000
#define FMV_AUDIO_CHANNELS 2
#define FMV_AUDIO_RING_FRAMES (FMV_AUDIO_RATE * 4)

static double fmv_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

manx_fmv *manx_fmv_open(const char *path, int out_w, int out_h,
                            manx_fmv_format format) {
    manx_fmv *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->out_w = out_w; p->out_h = out_h;
    p->out_pix = format == manx_fmv_format_rgba ? AV_PIX_FMT_RGBA
                                                  : AV_PIX_FMT_BGRA;
    p->t0 = -1.0;
    p->astream = -1;

    if (avformat_open_input(&p->fmt, path, NULL, NULL) < 0) {
        fprintf(stderr, "[FMV] cannot open %s\n", path);
        free(p); return NULL;
    }
    if (avformat_find_stream_info(p->fmt, NULL) < 0) goto fail;

    p->vstream = av_find_best_stream(p->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (p->vstream < 0) goto fail;

    const AVCodecParameters *par = p->fmt->streams[p->vstream]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(par->codec_id);
    if (!codec) goto fail;
    p->dec = avcodec_alloc_context3(codec);
    if (!p->dec || avcodec_parameters_to_context(p->dec, par) < 0) goto fail;
    if (avcodec_open2(p->dec, codec, NULL) < 0) goto fail;

    /* Audio is optional: original XMV files are video-only, while imported
     * MP4 assets contain AAC. A failed audio decoder must not suppress video. */
    p->astream = av_find_best_stream(p->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (p->astream >= 0) {
        const AVCodecParameters *apar = p->fmt->streams[p->astream]->codecpar;
        const AVCodec *acodec = avcodec_find_decoder(apar->codec_id);
        p->adec = acodec ? avcodec_alloc_context3(acodec) : NULL;
        if (!p->adec || avcodec_parameters_to_context(p->adec, apar) < 0 ||
            avcodec_open2(p->adec, acodec, NULL) < 0) {
            if (p->adec) avcodec_free_context(&p->adec);
            p->astream = -1;
        } else {
            AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
            if (swr_alloc_set_opts2(&p->swr, &stereo, AV_SAMPLE_FMT_S16,
                    FMV_AUDIO_RATE, &p->adec->ch_layout, p->adec->sample_fmt,
                    p->adec->sample_rate, 0, NULL) < 0 ||
                !p->swr || swr_init(p->swr) < 0) {
                if (p->swr) swr_free(&p->swr);
                avcodec_free_context(&p->adec);
                p->astream = -1;
            }
        }
    }

    p->pending = av_frame_alloc();
    p->audio_frame = p->astream >= 0 ? av_frame_alloc() : NULL;
    p->pkt     = av_packet_alloc();
    p->pixels  = calloc(1, (size_t)out_w * out_h * 4);
    if (p->astream >= 0) {
        p->audio_capacity = FMV_AUDIO_RING_FRAMES;
        p->audio_ring = calloc((size_t)p->audio_capacity * FMV_AUDIO_CHANNELS,
                               sizeof(*p->audio_ring));
    }
    if (!p->pending || !p->pkt || !p->pixels ||
        (p->astream >= 0 && (!p->audio_frame || !p->audio_ring))) goto fail;

    fprintf(stderr, "[FMV] playing %s (%dx%d %s%s)\n", path,
            par->width, par->height, codec->name,
            p->astream >= 0 ? " + audio" : "");
    return p;

fail:
    manx_fmv_close(p);
    return NULL;
}

static void fmv_queue_audio(manx_fmv *p, const int16_t *samples, int frames) {
    uint64_t write = atomic_load_explicit(&p->audio_write, memory_order_relaxed);
    uint64_t read = atomic_load_explicit(&p->audio_read, memory_order_acquire);
    uint64_t free_frames = p->audio_capacity - (write - read);
    if ((uint64_t)frames > free_frames) frames = (int)free_frames;
    for (int i = 0; i < frames; i++) {
        uint64_t dst = ((write + (uint64_t)i) % p->audio_capacity) * 2;
        p->audio_ring[dst] = samples[i * 2];
        p->audio_ring[dst + 1] = samples[i * 2 + 1];
    }
    atomic_store_explicit(&p->audio_write, write + (uint64_t)frames,
                          memory_order_release);
}

static void fmv_decode_audio_packet(manx_fmv *p, const AVPacket *packet) {
    if (!p->adec || !p->swr || !p->audio_frame) return;
    if (avcodec_send_packet(p->adec, packet) < 0) return;
    while (avcodec_receive_frame(p->adec, p->audio_frame) == 0) {
        int out_frames = (int)av_rescale_rnd(
            swr_get_delay(p->swr, p->adec->sample_rate) + p->audio_frame->nb_samples,
            FMV_AUDIO_RATE, p->adec->sample_rate, AV_ROUND_UP);
        int16_t *converted = av_malloc_array((size_t)out_frames * 2,
                                             sizeof(*converted));
        if (!converted) { av_frame_unref(p->audio_frame); return; }
        uint8_t *output[1] = { (uint8_t *)converted };
        int produced = swr_convert(p->swr, output, out_frames,
            (const uint8_t * const *)p->audio_frame->extended_data,
            p->audio_frame->nb_samples);
        if (produced > 0) fmv_queue_audio(p, converted, produced);
        av_free(converted);
        av_frame_unref(p->audio_frame);
    }
}

/* Decode the next video frame into p->pending. Returns 1 on success,
 * 0 when the stream is exhausted. */
static int fmv_decode_next(manx_fmv *p) {
    for (;;) {
        int r = avcodec_receive_frame(p->dec, p->pending);
        if (r == 0) {
            const AVRational tb = p->fmt->streams[p->vstream]->time_base;
            int64_t ts = p->pending->best_effort_timestamp;
            if (ts == AV_NOPTS_VALUE) ts = 0;
            p->pending_pts = (double)ts * av_q2d(tb);
            p->have_pending = 1;
            return 1;
        }
        if (r == AVERROR_EOF) return 0;
        if (r != AVERROR(EAGAIN)) return 0;

        if (p->demux_eof) {
            if (!p->audio_drained) {
                fmv_decode_audio_packet(p, NULL);
                p->audio_drained = 1;
            }
            avcodec_send_packet(p->dec, NULL);  /* start drain */
            continue;
        }
        r = av_read_frame(p->fmt, p->pkt);
        if (r < 0) { p->demux_eof = 1; continue; }
        if (p->pkt->stream_index == p->vstream)
            avcodec_send_packet(p->dec, p->pkt);
        else if (p->pkt->stream_index == p->astream)
            fmv_decode_audio_packet(p, p->pkt);
        av_packet_unref(p->pkt);
    }
}

static void fmv_blit_pending(manx_fmv *p) {
    p->sws = sws_getCachedContext(p->sws,
        p->pending->width, p->pending->height, p->pending->format,
        p->out_w, p->out_h, p->out_pix,
        SWS_BILINEAR, NULL, NULL, NULL);
    if (!p->sws) return;
    uint8_t *dst[4] = { p->pixels, NULL, NULL, NULL };
    int dst_stride[4] = { p->out_w * 4, 0, 0, 0 };
    sws_scale(p->sws, (const uint8_t * const *)p->pending->data,
              p->pending->linesize, 0, p->pending->height, dst, dst_stride);
    p->have_pending = 0;
}

int manx_fmv_update(manx_fmv *p, int *out_new_frame) {
    if (out_new_frame) *out_new_frame = 0;
    if (!p || p->eof) return 0;

    const double now = fmv_now();
    if (p->t0 < 0) p->t0 = now;
    const double elapsed = now - p->t0;

    /* Display every frame whose timestamp has come due; keep at most one
     * decoded frame waiting so slow hosts drop late frames naturally. */
    for (;;) {
        if (!p->have_pending && !fmv_decode_next(p)) {
            p->eof = 1;
            /* The last blitted frame stays on screen this call; report
             * EOF so the caller advances next call. */
            return 0;
        }
        if (p->pending_pts > elapsed) break;
        fmv_blit_pending(p);
        if (out_new_frame) *out_new_frame = 1;
    }
    return 1;
}

const uint8_t *manx_fmv_frame(const manx_fmv *p) {
    return p ? p->pixels : NULL;
}

int manx_fmv_read_audio(manx_fmv *p, int16_t *stereo, int max_frames) {
    if (!p || !stereo || max_frames <= 0 || !p->audio_ring) return 0;
    uint64_t read = atomic_load_explicit(&p->audio_read, memory_order_relaxed);
    uint64_t write = atomic_load_explicit(&p->audio_write, memory_order_acquire);
    uint64_t available = write - read;
    int frames = available < (uint64_t)max_frames ? (int)available : max_frames;
    for (int i = 0; i < frames; i++) {
        uint64_t src = ((read + (uint64_t)i) % p->audio_capacity) * 2;
        stereo[i * 2] = p->audio_ring[src];
        stereo[i * 2 + 1] = p->audio_ring[src + 1];
    }
    atomic_store_explicit(&p->audio_read, read + (uint64_t)frames,
                          memory_order_release);
    return frames;
}

int manx_fmv_has_audio(const manx_fmv *p) {
    return p && p->astream >= 0;
}

void manx_fmv_close(manx_fmv *p) {
    if (!p) return;
    if (p->sws) sws_freeContext(p->sws);
    if (p->swr) swr_free(&p->swr);
    if (p->pkt) av_packet_free(&p->pkt);
    if (p->pending) av_frame_free(&p->pending);
    if (p->audio_frame) av_frame_free(&p->audio_frame);
    if (p->dec) avcodec_free_context(&p->dec);
    if (p->adec) avcodec_free_context(&p->adec);
    if (p->fmt) avformat_close_input(&p->fmt);
    free(p->audio_ring);
    free(p->pixels);
    free(p);
}
