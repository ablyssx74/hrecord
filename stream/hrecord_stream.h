/*
 * Live streaming for hrecord: the screen and the sound it records go to an RTMP / RTMPS server (YouTube, Twitch, ...) or are pushed
 * to an RTSP server as they are recorded.
 *
 * The video is encoded as H.264 by the hrecord_x264 add-on (see hrecord_x264.h), which hrecord loads when it is installed, and the
 * audio as AAC by ffmpeg. The recording to the file is not touched by this: if the stream breaks, the file goes on.
 */
#ifndef HRECORD_STREAM_H
#define HRECORD_STREAM_H

#include <string>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
}

struct StreamSettings {
	std::string	url;				// rtmp://, rtmps:// (with the stream key in the path) or rtsp://
	int			videoKbps = 2500;
	int			audioKbps = 128;
	std::string	preset = "veryfast";	// x264 preset: faster ones cost less CPU
	int			keyintSeconds = 2;
};

// Is the H.264 add-on installed? Fills in where it was found and its version.
bool StreamAddonAvailable(std::string* path = nullptr, std::string* version = nullptr);

// Opens the connection and starts encoding. width/height/fps are those of the frames that StreamPushVideo gets.
bool StreamStart(const StreamSettings& settings, int width, int height, int fps, bool fullRange, std::string* error);

// A planar YUV 4:2:0 frame and its time in microseconds since the recording started. Copies what it needs and never waits.
void StreamPushVideo(const AVFrame* frame, int64_t ptsUs);

// Raw interleaved or planar audio of any rate, as it comes from the sound source.
void StreamPushAudio(const void* data, size_t bytes, AVSampleFormat format, int channels, int rate);

bool StreamActive();

// Ends the stream (flushes what is left and closes the connection).
void StreamStop();

#endif
