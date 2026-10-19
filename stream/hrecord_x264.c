/*
 * hrecord_x264: the H.264 add-on of hrecord. See hrecord_x264.h. This library is built on x264 and therefore is GPL licensed.
 */
#include "hrecord_x264.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <x264.h>

struct hrx264 {
	x264_t*			encoder;
	x264_param_t	param;
	int				width, height;
	uint8_t*		buffer;			// the last packet
	int				bufferSize;
	uint8_t*		headers;
	int				headersSize;
};

int
hrx264_api_version(void)
{
	return HRECORD_X264_API_VERSION;
}

const char*
hrx264_version(void)
{
	static char text[64];
	snprintf(text, sizeof(text), "x264 core %d", X264_BUILD);
	return text;
}

void*
hrx264_open(int width, int height, int fps, int kbps, int keyint, const char* preset, int fullRange,
	const uint8_t** headers, int* headersSize)
{
	if (width <= 0 || height <= 0 || (width & 1) || (height & 1) || fps <= 0)
		return NULL;

	struct hrx264* self = (struct hrx264*)calloc(1, sizeof(*self));
	if (self == NULL)
		return NULL;

	// zerolatency: no B-frames and no look-ahead, so that frames come out as they go in (a live stream, not a file)
	if (x264_param_default_preset(&self->param, preset != NULL && preset[0] != '\0' ? preset : "veryfast", "zerolatency") < 0) {
		free(self);
		return NULL;
	}
	x264_param_t* p = &self->param;
	p->i_csp = X264_CSP_I420;
	p->i_width = width;
	p->i_height = height;
	p->b_vfr_input = 1;
	p->i_timebase_num = 1;
	p->i_timebase_den = 1000000;
	p->i_fps_num = fps;
	p->i_fps_den = 1;
	p->i_keyint_max = keyint > 0 ? keyint : fps * 2;
	p->i_keyint_min = p->i_keyint_max;
	p->b_repeat_headers = 1;			// SPS/PPS before every key frame, so that a player can join at any one
	p->b_annexb = 1;
	p->b_aud = 0;
	p->rc.i_rc_method = X264_RC_ABR;
	p->rc.i_bitrate = kbps > 0 ? kbps : 2500;
	p->rc.i_vbv_max_bitrate = p->rc.i_bitrate;
	p->rc.i_vbv_buffer_size = p->rc.i_bitrate;		// a one second buffer: the usual setting for live streams
	p->vui.b_fullrange = fullRange ? 1 : 0;
	p->vui.i_colorprim = 6;				// SMPTE 170M (BT.601), which is what the screen is converted with into JPEG-style YUV
	p->vui.i_transfer = 6;
	p->vui.i_colmatrix = 6;
	p->i_log_level = X264_LOG_NONE;
	if (x264_param_apply_profile(p, "high") < 0) {
		free(self);
		return NULL;
	}

	self->encoder = x264_encoder_open(p);
	if (self->encoder == NULL) {
		free(self);
		return NULL;
	}
	self->width = width;
	self->height = height;

	x264_nal_t* nals = NULL;
	int count = 0;
	int size = x264_encoder_headers(self->encoder, &nals, &count);
	if (size > 0 && nals != NULL) {
		self->headers = (uint8_t*)malloc(size);
		if (self->headers != NULL) {
			memcpy(self->headers, nals[0].p_payload, size);
			self->headersSize = size;
		}
	}
	if (headers != NULL)
		*headers = self->headers;
	if (headersSize != NULL)
		*headersSize = self->headersSize;
	return self;
}

static int
TakePacket(struct hrx264* self, x264_nal_t* nals, int count, int frameSize, x264_picture_t* out,
	const uint8_t** data, int* keyframe, int64_t* pts, int64_t* dts)
{
	if (frameSize <= 0 || count <= 0)
		return frameSize < 0 ? frameSize : 0;
	if (frameSize > self->bufferSize) {
		uint8_t* grown = (uint8_t*)realloc(self->buffer, frameSize);
		if (grown == NULL)
			return -1;
		self->buffer = grown;
		self->bufferSize = frameSize;
	}
	memcpy(self->buffer, nals[0].p_payload, frameSize);
	*data = self->buffer;
	if (keyframe != NULL)
		*keyframe = out->b_keyframe ? 1 : 0;
	if (pts != NULL)
		*pts = out->i_pts;
	if (dts != NULL)
		*dts = out->i_dts;
	return frameSize;
}

int
hrx264_encode(void* encoder, const uint8_t* const planes[3], const int strides[3], int64_t ptsUs,
	const uint8_t** out, int* keyframe, int64_t* outPtsUs, int64_t* outDtsUs)
{
	struct hrx264* self = (struct hrx264*)encoder;
	if (self == NULL || self->encoder == NULL)
		return -1;

	x264_picture_t in, picOut;
	x264_picture_init(&in);
	x264_picture_init(&picOut);
	in.img.i_csp = X264_CSP_I420;
	in.img.i_plane = 3;
	for (int i = 0; i < 3; i++) {
		in.img.plane[i] = (uint8_t*)planes[i];
		in.img.i_stride[i] = strides[i];
	}
	in.i_pts = ptsUs;

	x264_nal_t* nals = NULL;
	int count = 0;
	int size = x264_encoder_encode(self->encoder, &nals, &count, &in, &picOut);
	return TakePacket(self, nals, count, size, &picOut, out, keyframe, outPtsUs, outDtsUs);
}

int
hrx264_flush(void* encoder, const uint8_t** out, int* keyframe, int64_t* outPtsUs, int64_t* outDtsUs)
{
	struct hrx264* self = (struct hrx264*)encoder;
	if (self == NULL || self->encoder == NULL)
		return -1;
	if (x264_encoder_delayed_frames(self->encoder) <= 0)
		return 0;

	x264_picture_t picOut;
	x264_picture_init(&picOut);
	x264_nal_t* nals = NULL;
	int count = 0;
	int size = x264_encoder_encode(self->encoder, &nals, &count, NULL, &picOut);
	return TakePacket(self, nals, count, size, &picOut, out, keyframe, outPtsUs, outDtsUs);
}

void
hrx264_close(void* encoder)
{
	struct hrx264* self = (struct hrx264*)encoder;
	if (self == NULL)
		return;
	if (self->encoder != NULL)
		x264_encoder_close(self->encoder);
	free(self->buffer);
	free(self->headers);
	free(self);
}
