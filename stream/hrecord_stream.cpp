/*
 * Live streaming for hrecord, see hrecord_stream.h.
 */
#include "hrecord_stream.h"
#include "hrecord_x264.h"

#include <FindDirectory.h>
#include <OS.h>
#include <Path.h>
#include <image.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace {

const int kAudioRate = 48000;
const int kAudioChannels = 2;
const int kMaxQueuedFrames = 4;

// The entry points of the add-on, found by name.
struct Addon {
	image_id	image = -1;
	int			(*apiVersion)(void) = nullptr;
	const char*	(*version)(void) = nullptr;
	void*		(*open)(int, int, int, int, int, const char*, int, const uint8_t**, int*) = nullptr;
	int			(*encode)(void*, const uint8_t* const[3], const int[3], int64_t, const uint8_t**, int*, int64_t*, int64_t*) = nullptr;
	int			(*flush)(void*, const uint8_t**, int*, int64_t*, int64_t*) = nullptr;
	void		(*close)(void*) = nullptr;
};

Addon gAddon;
std::string gAddonPath;

std::string
AvErrorText(int error)
{
	char text[AV_ERROR_MAX_STRING_SIZE] = {0};
	av_strerror(error, text, sizeof(text));
	return text;
}

// The address up to the path ("rtmp://host:port"): what may be shown, since the rest holds the stream key.
std::string
HostOf(const std::string& url)
{
	size_t schemeEnd = url.find("://");
	size_t pathStart = url.find('/', schemeEnd == std::string::npos ? 0 : schemeEnd + 3);
	return pathStart == std::string::npos ? url : url.substr(0, pathStart);
}

bool
FileExists(const std::string& path)
{
	return access(path.c_str(), R_OK) == 0;
}

std::string
FindAddonFile()
{
	std::vector<std::string> folders;
	folders.push_back("/boot/system/add-ons/hrecord");
	BPath path;
	if (find_directory(B_USER_NONPACKAGED_ADDONS_DIRECTORY, &path) == B_OK)
		folders.push_back(std::string(path.Path()) + "/hrecord");
	if (find_directory(B_USER_ADDONS_DIRECTORY, &path) == B_OK)
		folders.push_back(std::string(path.Path()) + "/hrecord");
	for (size_t i = 0; i < folders.size(); i++) {
		std::string file = folders[i] + "/hrecord_x264";
		if (FileExists(file))
			return file;
	}
	return "";
}

bool
LoadAddon(std::string* error)
{
	if (gAddon.image >= 0)
		return true;
	std::string file = FindAddonFile();
	if (file.empty()) {
		if (error)
			*error = "The H.264 add-on (hrecord_x264) is not installed. Download the hrecord_stream package from "
				"https://github.com/ablyssx74/hrecord/releases";
		return false;
	}
	image_id image = load_add_on(file.c_str());
	if (image < 0) {
		if (error)
			*error = "The H.264 add-on could not be loaded (" + file + "): " + strerror(image);
		return false;
	}
	Addon addon;
	addon.image = image;
	bool ok = get_image_symbol(image, "hrx264_api_version", B_SYMBOL_TYPE_TEXT, (void**)&addon.apiVersion) == B_OK
		&& get_image_symbol(image, "hrx264_version", B_SYMBOL_TYPE_TEXT, (void**)&addon.version) == B_OK
		&& get_image_symbol(image, "hrx264_open", B_SYMBOL_TYPE_TEXT, (void**)&addon.open) == B_OK
		&& get_image_symbol(image, "hrx264_encode", B_SYMBOL_TYPE_TEXT, (void**)&addon.encode) == B_OK
		&& get_image_symbol(image, "hrx264_flush", B_SYMBOL_TYPE_TEXT, (void**)&addon.flush) == B_OK
		&& get_image_symbol(image, "hrx264_close", B_SYMBOL_TYPE_TEXT, (void**)&addon.close) == B_OK;
	if (!ok || addon.apiVersion() != HRECORD_X264_API_VERSION) {
		unload_add_on(image);
		if (error)
			*error = "The H.264 add-on (" + file + ") is not the version this hrecord needs.";
		return false;
	}
	gAddon = addon;
	gAddonPath = file;
	return true;
}

// ---- the running stream ----

struct VideoSlot {
	std::vector<uint8_t>	data;
	int64_t					ptsUs = 0;
};

struct Stream {
	AVFormatContext*	format = nullptr;
	AVStream*			videoStream = nullptr;
	AVStream*			audioStream = nullptr;
	void*				x264 = nullptr;
	int					width = 0, height = 0;

	AVCodecContext*		audioCodec = nullptr;
	SwrContext*			swr = nullptr;
	AVAudioFifo*		fifo = nullptr;
	AVSampleFormat		swrFormat = AV_SAMPLE_FMT_NONE;
	int					swrChannels = 0, swrRate = 0;
	int64_t				audioStartSample = 0;		// the time of the first audio sample, in samples
	int64_t				audioSamplesWritten = 0;
	bool				audioStarted = false;

	std::mutex			muxMutex;					// the muxer takes one writer at a time
	std::mutex			videoMutex;
	std::condition_variable	videoReady;
	std::deque<VideoSlot*>	videoQueue;
	std::vector<VideoSlot*>	freeSlots;
	std::mutex			audioMutex;
	std::thread			videoThread, audioThread;

	std::atomic<bool>	running{false};
	std::atomic<bool>	failed{false};
	std::atomic<bool>	clockSet{false};
	std::atomic<int64_t> clockZeroUs{0};			// system_time() at the recording's time 0
	std::atomic<uint64_t> framesSent{0}, framesDropped{0};
	size_t				frameBytes = 0;
	int64_t				lastVideoPtsUs = -1000000;
};

Stream* gStream = nullptr;

bool
WritePacket(Stream* s, AVPacket* packet)
{
	std::lock_guard<std::mutex> lock(s->muxMutex);
	int result = av_interleaved_write_frame(s->format, packet);
	if (result < 0 && !s->failed.exchange(true)) {
		std::cerr << "[!] The stream stopped (" << AvErrorText(result) << "); the recording to the file goes on."
			<< std::endl;
	}
	return result >= 0;
}

void
WriteVideoPacket(Stream* s, const uint8_t* data, int size, int keyframe, int64_t ptsUs, int64_t dtsUs)
{
	AVPacket* packet = av_packet_alloc();
	if (packet == nullptr || av_new_packet(packet, size) < 0) {
		av_packet_free(&packet);
		return;
	}
	memcpy(packet->data, data, size);
	const AVRational us = {1, 1000000};
	packet->pts = av_rescale_q(ptsUs, us, s->videoStream->time_base);
	packet->dts = av_rescale_q(dtsUs, us, s->videoStream->time_base);
	packet->stream_index = s->videoStream->index;
	if (keyframe)
		packet->flags |= AV_PKT_FLAG_KEY;
	if (WritePacket(s, packet))
		s->framesSent++;
	av_packet_free(&packet);
}

void
VideoLoop(Stream* s)
{
	const int ySize = s->width * s->height;
	const int cSize = (s->width / 2) * (s->height / 2);
	for (;;) {
		VideoSlot* slot = nullptr;
		{
			std::unique_lock<std::mutex> lock(s->videoMutex);
			s->videoReady.wait(lock, [&] { return !s->videoQueue.empty() || !s->running; });
			if (s->videoQueue.empty())
				break;
			slot = s->videoQueue.front();
			s->videoQueue.pop_front();
		}
		// the time stamps must differ by at least a millisecond (FLV counts in milliseconds) even when frames come in a burst
		if (slot->ptsUs <= s->lastVideoPtsUs)
			slot->ptsUs = s->lastVideoPtsUs + 1000;
		s->lastVideoPtsUs = slot->ptsUs;
		if (!s->failed) {
			const uint8_t* planes[3] = { slot->data.data(), slot->data.data() + ySize, slot->data.data() + ySize + cSize };
			const int strides[3] = { s->width, s->width / 2, s->width / 2 };
			const uint8_t* out = nullptr;
			int keyframe = 0;
			int64_t pts = 0, dts = 0;
			int size = gAddon.encode(s->x264, planes, strides, slot->ptsUs, &out, &keyframe, &pts, &dts);
			if (size > 0)
				WriteVideoPacket(s, out, size, keyframe, pts, dts);
		}
		std::lock_guard<std::mutex> lock(s->videoMutex);
		s->freeSlots.push_back(slot);
	}

	// what is still inside the encoder
	for (;;) {
		const uint8_t* out = nullptr;
		int keyframe = 0;
		int64_t pts = 0, dts = 0;
		int size = gAddon.flush(s->x264, &out, &keyframe, &pts, &dts);
		if (size <= 0)
			break;
		if (!s->failed)
			WriteVideoPacket(s, out, size, keyframe, pts, dts);
	}
}

// Encodes the frames in the FIFO; `all` also sends a last, short one.
void
EncodeAudio(Stream* s, bool all)
{
	const int frameSize = s->audioCodec->frame_size > 0 ? s->audioCodec->frame_size : 1024;
	for (;;) {
		AVFrame* frame = nullptr;
		{
			std::lock_guard<std::mutex> lock(s->audioMutex);
			int available = av_audio_fifo_size(s->fifo);
			if (available < frameSize && !(all && available > 0))
				break;
			int samples = available < frameSize ? available : frameSize;
			frame = av_frame_alloc();
			frame->nb_samples = samples;
			frame->format = AV_SAMPLE_FMT_FLTP;
			av_channel_layout_copy(&frame->ch_layout, &s->audioCodec->ch_layout);
			frame->sample_rate = kAudioRate;
			if (av_frame_get_buffer(frame, 0) < 0) {
				av_frame_free(&frame);
				break;
			}
			av_audio_fifo_read(s->fifo, (void**)frame->data, samples);
			frame->pts = s->audioStartSample + s->audioSamplesWritten;
			s->audioSamplesWritten += samples;
		}
		if (!s->failed && avcodec_send_frame(s->audioCodec, frame) == 0) {
			AVPacket* packet = av_packet_alloc();
			while (avcodec_receive_packet(s->audioCodec, packet) == 0) {
				av_packet_rescale_ts(packet, {1, kAudioRate}, s->audioStream->time_base);
				packet->stream_index = s->audioStream->index;
				WritePacket(s, packet);
				av_packet_unref(packet);
			}
			av_packet_free(&packet);
		}
		av_frame_free(&frame);
	}
}

void
AudioLoop(Stream* s)
{
	while (s->running) {
		snooze(10000);
		if (!s->clockSet)
			continue;
		{
			// when no sound arrives (nothing is playing, or a gap), fill with silence so that the stream's audio keeps up with its video
			std::lock_guard<std::mutex> lock(s->audioMutex);
			const int64_t now = ((system_time() - s->clockZeroUs) * kAudioRate) / 1000000;
			if (!s->audioStarted) {
				s->audioStartSample = now;
				s->audioStarted = true;
			}
			const int64_t have = s->audioStartSample + s->audioSamplesWritten + av_audio_fifo_size(s->fifo);
			const int64_t missing = now - have;
			if (missing > kAudioRate / 4) {
				std::vector<float> silence((size_t)missing);
				std::fill(silence.begin(), silence.end(), 0.0f);
				uint8_t* planes[2] = { (uint8_t*)silence.data(), (uint8_t*)silence.data() };
				av_audio_fifo_write(s->fifo, (void**)planes, (int)missing);
			}
		}
		EncodeAudio(s, false);
	}
	EncodeAudio(s, true);
	if (!s->failed) {
		avcodec_send_frame(s->audioCodec, nullptr);
		AVPacket* packet = av_packet_alloc();
		while (avcodec_receive_packet(s->audioCodec, packet) == 0) {
			av_packet_rescale_ts(packet, {1, kAudioRate}, s->audioStream->time_base);
			packet->stream_index = s->audioStream->index;
			WritePacket(s, packet);
			av_packet_unref(packet);
		}
		av_packet_free(&packet);
	}
}

bool
IsRtsp(const std::string& url)
{
	return url.compare(0, 7, "rtsp://") == 0 || url.compare(0, 8, "rtsps://") == 0;
}

void
Destroy(Stream* s)
{
	if (s->x264 != nullptr)
		gAddon.close(s->x264);
	if (s->audioCodec != nullptr)
		avcodec_free_context(&s->audioCodec);
	if (s->swr != nullptr)
		swr_free(&s->swr);
	if (s->fifo != nullptr)
		av_audio_fifo_free(s->fifo);
	if (s->format != nullptr) {
		if (!(s->format->oformat->flags & AVFMT_NOFILE) && s->format->pb != nullptr)
			avio_closep(&s->format->pb);
		avformat_free_context(s->format);
	}
	for (size_t i = 0; i < s->freeSlots.size(); i++)
		delete s->freeSlots[i];
	for (size_t i = 0; i < s->videoQueue.size(); i++)
		delete s->videoQueue[i];
	delete s;
}

}	// namespace


bool
StreamAddonAvailable(std::string* path, std::string* version)
{
	std::string file = FindAddonFile();
	if (file.empty())
		return false;
	if (path != nullptr)
		*path = file;
	if (version != nullptr) {
		std::string ignored;
		*version = LoadAddon(&ignored) ? gAddon.version() : "";
	}
	return true;
}


bool
StreamStart(const StreamSettings& settings, int width, int height, int fps, bool fullRange, std::string* error)
{
	std::string problem;
	if (gStream != nullptr) {
		problem = "A stream is running already.";
	} else if (settings.url.empty()) {
		problem = "No stream address is set.";
	} else if (!LoadAddon(&problem)) {
		// problem says why
	} else if ((width & 1) || (height & 1)) {
		problem = "The video size must be even for H.264.";
	}
	if (!problem.empty()) {
		if (error)
			*error = problem;
		return false;
	}

	Stream* s = new Stream();
	s->width = width;
	s->height = height;
	s->frameBytes = (size_t)width * height * 3 / 2;
	auto fail = [&](const std::string& text) {
		if (error)
			*error = text;
		Destroy(s);
		return false;
	};

	const bool rtsp = IsRtsp(settings.url);
	const char* muxer = rtsp ? "rtsp" : "flv";
	if (!rtsp && settings.url.compare(0, 7, "rtmp://") != 0 && settings.url.compare(0, 8, "rtmps://") != 0
			&& settings.url.compare(0, 6, "rtmpt:") != 0)
		return fail("The stream address must start with rtmp://, rtmps:// or rtsp://");
	if (avformat_alloc_output_context2(&s->format, nullptr, muxer, settings.url.c_str()) < 0)
		return fail("Could not set up the stream's container.");

	// video: the H.264 add-on
	const uint8_t* headers = nullptr;
	int headersSize = 0;
	int keyint = fps * (settings.keyintSeconds > 0 ? settings.keyintSeconds : 2);
	s->x264 = gAddon.open(width, height, fps, settings.videoKbps, keyint, settings.preset.c_str(), fullRange ? 1 : 0,
		&headers, &headersSize);
	if (s->x264 == nullptr)
		return fail("The H.264 encoder could not be opened (check the size and the preset).");

	s->videoStream = avformat_new_stream(s->format, nullptr);
	if (s->videoStream == nullptr)
		return fail("Could not add the video to the stream.");
	AVCodecParameters* vp = s->videoStream->codecpar;
	vp->codec_type = AVMEDIA_TYPE_VIDEO;
	vp->codec_id = AV_CODEC_ID_H264;
	vp->width = width;
	vp->height = height;
	vp->format = AV_PIX_FMT_YUV420P;
	vp->bit_rate = (int64_t)settings.videoKbps * 1000;
	if (fullRange) {
		vp->color_range = AVCOL_RANGE_JPEG;
		vp->color_space = AVCOL_SPC_SMPTE170M;
	}
	if (headersSize > 0) {
		vp->extradata = (uint8_t*)av_mallocz(headersSize + AV_INPUT_BUFFER_PADDING_SIZE);
		memcpy(vp->extradata, headers, headersSize);
		vp->extradata_size = headersSize;
	}
	s->videoStream->time_base = {1, 1000000};
	s->videoStream->avg_frame_rate = {fps, 1};

	// audio: AAC from ffmpeg, 48 kHz stereo whatever the sources are
	const AVCodec* aac = avcodec_find_encoder_by_name("aac");
	if (aac == nullptr)
		aac = avcodec_find_encoder(AV_CODEC_ID_AAC);
	if (aac == nullptr)
		return fail("This ffmpeg has no AAC encoder.");
	s->audioCodec = avcodec_alloc_context3(aac);
	av_channel_layout_default(&s->audioCodec->ch_layout, kAudioChannels);
	s->audioCodec->sample_rate = kAudioRate;
	s->audioCodec->sample_fmt = AV_SAMPLE_FMT_FLTP;
	s->audioCodec->bit_rate = (int64_t)settings.audioKbps * 1000;
	s->audioCodec->time_base = {1, kAudioRate};
	if (s->format->oformat->flags & AVFMT_GLOBALHEADER)
		s->audioCodec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	int result = avcodec_open2(s->audioCodec, aac, nullptr);
	if (result < 0)
		return fail("The AAC encoder could not be opened (" + AvErrorText(result) + ").");
	s->audioStream = avformat_new_stream(s->format, nullptr);
	if (s->audioStream == nullptr)
		return fail("Could not add the audio to the stream.");
	avcodec_parameters_from_context(s->audioStream->codecpar, s->audioCodec);
	s->audioStream->time_base = {1, kAudioRate};
	s->fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, kAudioChannels, 4096);
	if (s->fifo == nullptr)
		return fail("Out of memory.");

	// connect
	AVDictionary* options = nullptr;
	if (rtsp) {
		av_dict_set(&options, "rtsp_transport", "tcp", 0);
	} else {
		av_dict_set(&options, "rw_timeout", "10000000", 0);
		if (!(s->format->oformat->flags & AVFMT_NOFILE)) {
			result = avio_open2(&s->format->pb, settings.url.c_str(), AVIO_FLAG_WRITE, nullptr, &options);
			av_dict_free(&options);
			if (result < 0)
				return fail("Could not connect to " + HostOf(settings.url) + " (" + AvErrorText(result) + ").");
		}
	}
	result = avformat_write_header(s->format, rtsp ? &options : nullptr);
	av_dict_free(&options);
	if (result < 0)
		return fail("The stream's server did not accept the stream (" + AvErrorText(result) + ").");

	for (int i = 0; i < kMaxQueuedFrames + 1; i++) {
		VideoSlot* slot = new VideoSlot();
		slot->data.resize(s->frameBytes);
		s->freeSlots.push_back(slot);
	}
	s->running = true;
	gStream = s;
	s->videoThread = std::thread(VideoLoop, s);
	s->audioThread = std::thread(AudioLoop, s);
	return true;
}


void
StreamPushVideo(const AVFrame* frame, int64_t ptsUs)
{
	Stream* s = gStream;
	if (s == nullptr || !s->running || s->failed)
		return;
	if (!s->clockSet) {
		s->clockZeroUs = system_time() - ptsUs;
		s->clockSet = true;
	}
	VideoSlot* slot = nullptr;
	{
		std::lock_guard<std::mutex> lock(s->videoMutex);
		if (s->freeSlots.empty()) {
			s->framesDropped++;		// the network or the encoder is behind: never hold the capture up for it
			return;
		}
		slot = s->freeSlots.back();
		s->freeSlots.pop_back();
	}
	av_image_copy_to_buffer(slot->data.data(), (int)slot->data.size(), frame->data, frame->linesize,
		AV_PIX_FMT_YUV420P, s->width, s->height, 1);
	slot->ptsUs = ptsUs;
	{
		std::lock_guard<std::mutex> lock(s->videoMutex);
		s->videoQueue.push_back(slot);
	}
	s->videoReady.notify_one();
}


void
StreamPushAudio(const void* data, size_t bytes, AVSampleFormat format, int channels, int rate)
{
	Stream* s = gStream;
	if (s == nullptr || !s->running || s->failed || !s->clockSet || channels <= 0 || rate <= 0)
		return;
	const int bytesPerFrame = av_get_bytes_per_sample(format) * channels;
	if (bytesPerFrame <= 0)
		return;
	const int inSamples = (int)(bytes / bytesPerFrame);
	if (inSamples <= 0)
		return;

	std::lock_guard<std::mutex> lock(s->audioMutex);
	if (s->swr == nullptr || s->swrFormat != format || s->swrChannels != channels || s->swrRate != rate) {
		swr_free(&s->swr);
		AVChannelLayout in, out;
		av_channel_layout_default(&in, channels);
		av_channel_layout_default(&out, kAudioChannels);
		int result = swr_alloc_set_opts2(&s->swr, &out, AV_SAMPLE_FMT_FLTP, kAudioRate, &in, format, rate, 0, nullptr);
		av_channel_layout_uninit(&in);
		av_channel_layout_uninit(&out);
		if (result < 0 || swr_init(s->swr) < 0) {
			swr_free(&s->swr);
			return;
		}
		s->swrFormat = format;
		s->swrChannels = channels;
		s->swrRate = rate;
	}
	const int maxOut = (int)((int64_t)inSamples * kAudioRate / rate) + 64;
	std::vector<float> left(maxOut), right(maxOut);
	uint8_t* planes[2] = { (uint8_t*)left.data(), (uint8_t*)right.data() };
	const uint8_t* input[1] = { (const uint8_t*)data };
	int got = swr_convert(s->swr, planes, maxOut, input, inSamples);
	if (got > 0)
		av_audio_fifo_write(s->fifo, (void**)planes, got);
}


bool
StreamActive()
{
	return gStream != nullptr && gStream->running && !gStream->failed;
}


void
StreamStop()
{
	Stream* s = gStream;
	if (s == nullptr)
		return;
	s->running = false;
	s->videoReady.notify_all();
	if (s->videoThread.joinable())
		s->videoThread.join();
	if (s->audioThread.joinable())
		s->audioThread.join();
	if (!s->failed) {
		std::lock_guard<std::mutex> lock(s->muxMutex);
		av_write_trailer(s->format);
	}
	std::cout << "[i] Stream ended: " << s->framesSent.load() << " video frames sent, " << s->framesDropped.load()
		<< " dropped because the network or encoder could not keep up." << std::endl;
	gStream = nullptr;
	Destroy(s);
}
