/*
 * The interface between hrecord and its H.264 add-on (hrecord_x264).
 *
 * hrecord itself is MIT licensed and Haiku's own ffmpeg has no H.264 encoder (x264 is GPL), so the encoder lives in a small separate
 * add-on library, built on x264, that hrecord loads when it is installed. Everything here is plain C so that the two sides need
 * nothing from each other but this header.
 *
 * Frames go in as planar YUV 4:2:0, 8 bit. What comes out is H.264 in the Annex B form (start codes), which is what the FLV and RTSP
 * muxers of ffmpeg take. A returned buffer belongs to the add-on and stays valid until the next call on the same encoder.
 */
#ifndef HRECORD_X264_H
#define HRECORD_X264_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HRECORD_X264_API_VERSION 1

// The add-on's symbols, found by name with get_image_symbol().
int			hrx264_api_version(void);
const char*	hrx264_version(void);		// e.g. "x264 core 164"

// Opens an encoder. width and height must be even. kbps is the average bit rate; keyint the most frames between key frames
// (YouTube wants one about every two seconds); preset is an x264 preset name ("ultrafast" .. "medium"); fullRange says the
// pixels use the full 0..255 range (as JPEG-style YUVJ420P does). The time stamps passed to hrx264_encode() are in microseconds.
// On success *headers / *headersSize hold the SPS/PPS (Annex B). Returns NULL on failure.
void*		hrx264_open(int width, int height, int fps, int kbps, int keyint, const char* preset, int fullRange,
				const uint8_t** headers, int* headersSize);

// Encodes one frame. Returns the number of bytes in *out (0: no packet yet, < 0: an error).
int			hrx264_encode(void* encoder, const uint8_t* const planes[3], const int strides[3], int64_t ptsUs,
				const uint8_t** out, int* keyframe, int64_t* outPtsUs, int64_t* outDtsUs);

// Asks for a frame that is still inside the encoder, at the end of a stream; call until it returns 0.
int			hrx264_flush(void* encoder, const uint8_t** out, int* keyframe, int64_t* outPtsUs, int64_t* outDtsUs);

void		hrx264_close(void* encoder);

#ifdef __cplusplus
}
#endif

#endif
