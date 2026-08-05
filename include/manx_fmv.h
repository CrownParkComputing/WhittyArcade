/**
 * manx_fmv.h — wall-clock-paced FMV playback for any board or port.
 *
 * Decodes a video file through ffmpeg (libavformat, libavcodec,
 * libswscale) into fixed-size frames, either BGRA for a D3D8-style
 * texture staging buffer or RGBA for anything that feeds
 * present_rgba_frame directly. Any container ffmpeg can demux plays;
 * the module grew out of the Xbox XMV intro reel but nothing in it is
 * title-specific.
 *
 * Playback is wall-clock paced from the stream's own per-frame
 * timestamps, never the container frame rate — XMV's metadata reports a
 * bogus 1000/1. The clock starts on the first update call, and a host
 * that falls behind drops late frames rather than queuing them. The
 * caller polls manx_fmv_update() once per game frame.
 *
 * This header has no ffmpeg dependency, so it is safe to include from a
 * build without the ffmpeg dev libraries; the manx_fmv library target
 * itself only exists when they were found, and callers gate their call
 * sites on their own HAVE define.
 */

#ifndef MANX_FMV_H
#define MANX_FMV_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct manx_fmv manx_fmv;

/* Byte order of the decoded frame buffer. */
typedef enum manx_fmv_format {
    manx_fmv_format_bgra, /* D3D8 X8R8G8B8 texture staging order */
    manx_fmv_format_rgba, /* present_rgba_frame's byte order */
} manx_fmv_format;

/* Open a video file for playback, scaling output to out_w × out_h in the
 * requested byte order. Returns NULL if the file is missing or not
 * decodable. */
manx_fmv *manx_fmv_open(const char *path, int out_w, int out_h,
                            manx_fmv_format format);

/* Advance playback against the wall clock. Returns:
 *   1  — still playing (frame buffer valid once *out_new_frame was set)
 *   0  — end of stream reached; caller should close and move on.
 * Sets *out_new_frame to 1 when the frame buffer changed this call. */
int manx_fmv_update(manx_fmv *fmv, int *out_new_frame);

/* Latest decoded frame, tightly packed (out_w × out_h × 4). */
const uint8_t *manx_fmv_frame(const manx_fmv *fmv);

/* Pull decoded 48 kHz signed-16-bit stereo audio. Returns the number of
 * sample frames written; callers should fill any remainder with silence.
 * This is lock-free for one decoder/update producer and one audio consumer.
 * Video-only inputs simply return zero. */
int manx_fmv_read_audio(manx_fmv *fmv, int16_t *stereo, int max_frames);

/* True when the opened container has a decodable audio stream. */
int manx_fmv_has_audio(const manx_fmv *fmv);

void manx_fmv_close(manx_fmv *fmv);

#ifdef __cplusplus
}
#endif

#endif /* MANX_FMV_H */
