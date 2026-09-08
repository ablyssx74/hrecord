#include <InterfaceKit.h> // Pulls in BApplication, BScreen, BBitmap
#include <StorageKit.h>
#include <SupportKit.h>   // Pulls in system_time()
#include <MediaRoster.h>
#include <MediaAddOn.h>
#include <MediaDefs.h>
#include <MediaNode.h>
#include <BufferConsumer.h>
#include <MediaEventLooper.h>
#include <TimeSource.h>
#include <Buffer.h>
#include <SoundPlayer.h>
#include <algorithm>
#include <iostream>
#include <string>
#include <mutex>
#include <vector>
#include <cstdint>
#include <cstring>
#include <signal.h>
#include <unistd.h>
#include <string.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

// Global configurations
const int TARGET_FPS = 30;
const int FRAME_DELAY = 1000000 / TARGET_FPS; // Microseconds (33.3ms)

bool g_running = true;

// Guards every write to the shared AVFormatContext (avformat_write_header,
// av_interleaved_write_frame, av_write_trailer) since the video frames are
// muxed from main() while audio packets are muxed from the audio tap node's
// own control thread (see AudioTapNode::BufferReceived below).
std::mutex g_muxMutex;

void signalHandler(int signum) {
    g_running = false;
}

// Renders an FFmpeg AVERROR code as text, so failures name what actually
// went wrong instead of just "it failed" -- useful since av_log is kept
// quiet for routine chatter (see AV_LOG_ERROR below) and would otherwise
// swallow the detail.
std::string AvErr(int errnum) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(errnum, buf, sizeof(buf));
    return std::string(buf);
}

// ============================================================================
// Desktop audio capture: Vorbis encoding pipeline
// ============================================================================

struct AudioEncoder {
    AVCodecContext* codecCtx = nullptr;
    AVStream* stream = nullptr;
    SwrContext* swr = nullptr;
    AVAudioFifo* fifo = nullptr;
    AVFormatContext* fmtCtx = nullptr;
    int channels = 0;
    int64_t samplesEncoded = 0;
};

AudioEncoder g_audioEnc;

// Maps a Haiku raw audio sample encoding onto the closest FFmpeg sample format.
AVSampleFormat HaikuAudioFormatToAV(uint32 format) {
    switch (format) {
        case media_raw_audio_format::B_AUDIO_FLOAT:  return AV_SAMPLE_FMT_FLT;
        case media_raw_audio_format::B_AUDIO_INT:    return AV_SAMPLE_FMT_S32;
        case media_raw_audio_format::B_AUDIO_SHORT:  return AV_SAMPLE_FMT_S16;
        case media_raw_audio_format::B_AUDIO_UCHAR:  return AV_SAMPLE_FMT_U8;
        default:                                     return AV_SAMPLE_FMT_S16;
    }
}

// Diagnostic dump for `hrecord --list-audio-inputs`: shows the System Mixer's
// currently connected inputs -- i.e. which apps are actually playing sound
// right now, which is exactly what --audioonly needs at least one of.
void ListAudioInputs(BMediaRoster* roster) {
    media_node mixerNode;
    if (roster->GetAudioMixer(&mixerNode) != B_OK) {
        std::cerr << "[-] Error: Could not reach the System Mixer." << std::endl;
        return;
    }

    const int32 kMax = 32;
    media_input inputs[kMax];
    int32 count = kMax;
    if (roster->GetConnectedInputsFor(mixerNode, inputs, kMax, &count) != B_OK) {
        std::cerr << "[-] Error: Failed to query the Mixer's connected inputs." << std::endl;
        return;
    }

    if (count == 0) {
        std::cout << "[!] Nothing is currently playing into the System Mixer. Start playback "
            "somewhere before recording desktop audio." << std::endl;
        return;
    }

    std::cout << "[+] Apps currently feeding the System Mixer (hrecord taps one of these):"
        << std::endl;
    for (int32 i = 0; i < count; i++) {
        media_node_id sourceNodeId = roster->NodeIDFor(inputs[i].source.port);
        live_node_info info;
        media_node sourceNode;
        const char* name = "(unknown)";
        if (sourceNodeId >= 0 && roster->GetNodeFor(sourceNodeId, &sourceNode) == B_OK
                && roster->GetLiveNodeInfo(sourceNode, &info) == B_OK) {
            name = info.name;
        }
        std::cout << "    - \"" << name << "\"" << std::endl;
    }
}

// Adds a Vorbis audio stream to fmtCtx and wires up the resampler/FIFO used
// to buffer raw audio into fixed-size encoder frames. Vorbis (Ogg's native
// audio codec) is royalty-free and unencumbered, which is why it's used here
// regardless of whether the output container is standalone Ogg (--audioonly)
// or the Matroska file shared with the video.
bool SetupAudioEncoder(AVFormatContext* fmtCtx, const media_raw_audio_format& raw,
        AudioEncoder* enc) {
    // Prefer the real libvorbis encoder when this FFmpeg build has it -- it's
    // far more complete than FFmpeg's own long-experimental native "vorbis"
    // encoder. No extra linking is needed here even when it's available:
    // libavcodec.so already carries its own dependency on libvorbisenc.
    const AVCodec* codec = avcodec_find_encoder_by_name("libvorbis");
    bool usingNativeVorbis = false;
    if (!codec) {
        codec = avcodec_find_encoder(AV_CODEC_ID_VORBIS);
        usingNativeVorbis = true;
    }
    if (!codec) {
        std::cerr << "[-] Error: No Vorbis encoder (libvorbis or built-in) is available in this "
            "FFmpeg build." << std::endl;
        return false;
    }

    AVCodecContext* codecCtx = avcodec_alloc_context3(codec);
    int channels = raw.channel_count > 0 ? (int)raw.channel_count : 2;
    int sampleRate = raw.frame_rate > 0 ? (int)(raw.frame_rate + 0.5f) : 44100;

    av_channel_layout_default(&codecCtx->ch_layout, channels);
    codecCtx->sample_rate = sampleRate;
    codecCtx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    codecCtx->bit_rate = 160000;
    codecCtx->time_base = {1, sampleRate};
    if (usingNativeVorbis) {
        // FFmpeg's built-in Vorbis encoder is still flagged experimental.
        codecCtx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
    }

    // Open (and validate) the encoder before touching fmtCtx at all, so a
    // failure here never leaves a half-configured stream behind in the output.
    int openErr = avcodec_open2(codecCtx, codec, nullptr);
    if (openErr < 0) {
        std::cerr << "[-] Error: Cannot open audio encoder \"" << codec->name << "\" ("
            << AvErr(openErr) << ")." << std::endl;
        avcodec_free_context(&codecCtx);
        return false;
    }

    AVStream* stream = avformat_new_stream(fmtCtx, codec);
    if (!stream) {
        std::cerr << "[-] Error: Failed to create output audio stream." << std::endl;
        avcodec_free_context(&codecCtx);
        return false;
    }

    int paramErr = avcodec_parameters_from_context(stream->codecpar, codecCtx);
    if (paramErr < 0) {
        std::cerr << "[-] Error: Failed to transfer audio codec parameters (" << AvErr(paramErr)
            << ")." << std::endl;
        avcodec_free_context(&codecCtx);
        return false;
    }
    stream->time_base = codecCtx->time_base;

    AVChannelLayout inLayout;
    av_channel_layout_default(&inLayout, channels);

    SwrContext* swr = nullptr;
    int swrErr = swr_alloc_set_opts2(&swr, &codecCtx->ch_layout, AV_SAMPLE_FMT_FLTP,
        sampleRate, &inLayout, HaikuAudioFormatToAV(raw.format), sampleRate, 0, nullptr);
    if (swrErr >= 0 && swr)
        swrErr = swr_init(swr);
    av_channel_layout_uninit(&inLayout);
    if (swrErr < 0 || !swr) {
        std::cerr << "[-] Error: Failed to initialize audio resampler (" << AvErr(swrErr)
            << ")." << std::endl;
        avcodec_free_context(&codecCtx);
        if (swr) swr_free(&swr);
        return false;
    }

    AVAudioFifo* fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, channels, 1);
    if (!fifo) {
        std::cerr << "[-] Error: Failed to allocate audio buffering FIFO." << std::endl;
        avcodec_free_context(&codecCtx);
        swr_free(&swr);
        return false;
    }

    enc->codecCtx = codecCtx;
    enc->stream = stream;
    enc->swr = swr;
    enc->fifo = fifo;
    enc->fmtCtx = fmtCtx;
    enc->channels = channels;
    enc->samplesEncoded = 0;
    return true;
}

// Drains any complete encoder-sized frames currently sitting in the FIFO.
void DrainAudioFifo(AudioEncoder* enc, bool flushShortFrame) {
    int frameSize = enc->codecCtx->frame_size > 0 ? enc->codecCtx->frame_size : 1024;

    while (av_audio_fifo_size(enc->fifo) >= frameSize
            || (flushShortFrame && av_audio_fifo_size(enc->fifo) > 0)) {
        int samples = av_audio_fifo_size(enc->fifo);
        if (samples > frameSize)
            samples = frameSize;

        AVFrame* frame = av_frame_alloc();
        frame->nb_samples = samples;
        frame->format = AV_SAMPLE_FMT_FLTP;
        av_channel_layout_copy(&frame->ch_layout, &enc->codecCtx->ch_layout);
        frame->sample_rate = enc->codecCtx->sample_rate;
        av_frame_get_buffer(frame, 0);

        av_audio_fifo_read(enc->fifo, (void**)frame->data, samples);
        frame->pts = enc->samplesEncoded;
        enc->samplesEncoded += samples;

        {
            std::lock_guard<std::mutex> lock(g_muxMutex);
            if (avcodec_send_frame(enc->codecCtx, frame) == 0) {
                AVPacket* pkt = av_packet_alloc();
                while (avcodec_receive_packet(enc->codecCtx, pkt) == 0) {
                    av_packet_rescale_ts(pkt, enc->codecCtx->time_base, enc->stream->time_base);
                    pkt->stream_index = enc->stream->index;
                    av_interleaved_write_frame(enc->fmtCtx, pkt);
                    av_packet_unref(pkt);
                }
                av_packet_free(&pkt);
            }
        }
        av_frame_free(&frame);

        if (flushShortFrame)
            break; // the leftover partial frame has been written; nothing more to drain
    }
}

// Resamples a chunk of raw PCM into planar float, buffers it, and hands
// complete encoder-sized frames off to the Vorbis encoder as they become
// available. Called from AudioTapNode::BufferReceived, on the tap's own
// control thread.
void EncodeAudioSamples(AudioEncoder* enc, const void* data, size_t size,
        const media_raw_audio_format& format) {
    if (!enc->codecCtx || !g_running)
        return;

    int sampleSize = format.format & media_raw_audio_format::B_AUDIO_SIZE_MASK;
    int bytesPerFrame = sampleSize * (int)format.channel_count;
    if (bytesPerFrame <= 0)
        return;
    int nbSamples = (int)(size / bytesPerFrame);
    if (nbSamples <= 0)
        return;

    uint8_t* converted[8] = { nullptr };
    if (av_samples_alloc(converted, nullptr, enc->channels, nbSamples,
            AV_SAMPLE_FMT_FLTP, 0) < 0) {
        return;
    }

    const uint8_t* inData[1] = { (const uint8_t*)data };
    int producedSamples = swr_convert(enc->swr, converted, nbSamples, inData, nbSamples);
    if (producedSamples > 0)
        av_audio_fifo_write(enc->fifo, (void**)converted, producedSamples);

    av_freep(&converted[0]);

    DrainAudioFifo(enc, false);
}

// ============================================================================
// Desktop audio capture: live playback while recording
//
// Two earlier approaches tried to put hrecord's own Media Kit node back into
// the playback graph as a genuine producer -- first spliced between the
// System Mixer's output and the sound card (crashed Haiku's own Mixer
// control thread, reproducibly, three times), then between one app's output
// and a fresh Mixer input (didn't crash, but the forwarded audio never
// became audible despite delivering without any error at the Media Kit
// level -- consistent with something in the Mixer's own internal per-buffer
// routing silently not recognizing hrecord's connection, which isn't
// something fixable from outside Haiku's own Mixer source).
//
// This version doesn't try to be a producer at all. AudioTapNode only
// *captures* -- every buffer it receives from the hijacked app is handed to
// the Vorbis encoder and copied into a small ring buffer. A BSoundPlayer
// drains that ring buffer to actually produce sound, connecting to the
// System Mixer via Haiku's own well-tested playback path -- the same one
// every ordinary sound-playing app already uses successfully -- rather than
// hrecord's own hand-rolled producer connection.
// ============================================================================

// A small lock-protected circular byte buffer bridging the tap's own
// control thread (writer, paced by the hijacked app's buffer cadence) and
// the BSoundPlayer callback thread (reader, paced by the Mixer's own
// cadence). On overflow, the oldest bytes are dropped rather than blocking
// either side or growing unbounded; on underrun, playback is zero-filled
// (silence) rather than reading stale or garbage data.
class AudioRingBuffer {
public:
    void Init(size_t capacityBytes) {
        std::lock_guard<std::mutex> lock(fMutex);
        fBuffer.assign(capacityBytes, 0);
        fWritePos = fReadPos = fAvailable = 0;
    }

    void Write(const void* data, size_t size) {
        std::lock_guard<std::mutex> lock(fMutex);
        size_t capacity = fBuffer.size();
        if (capacity == 0)
            return;

        const uint8_t* src = (const uint8_t*)data;
        if (size > capacity) {
            // Only the most recent `capacity` bytes can possibly fit.
            src += (size - capacity);
            size = capacity;
        }

        size_t firstChunk = std::min(size, capacity - fWritePos);
        memcpy(&fBuffer[fWritePos], src, firstChunk);
        if (firstChunk < size)
            memcpy(&fBuffer[0], src + firstChunk, size - firstChunk);
        fWritePos = (fWritePos + size) % capacity;

        if (fAvailable + size > capacity) {
            size_t overflow = fAvailable + size - capacity;
            fReadPos = (fReadPos + overflow) % capacity;
            fAvailable = capacity;
        } else {
            fAvailable += size;
        }
    }

    void Read(void* data, size_t size) {
        std::lock_guard<std::mutex> lock(fMutex);
        uint8_t* dst = (uint8_t*)data;
        size_t capacity = fBuffer.size();
        size_t toCopy = std::min(size, fAvailable);

        if (toCopy > 0 && capacity > 0) {
            size_t firstChunk = std::min(toCopy, capacity - fReadPos);
            memcpy(dst, &fBuffer[fReadPos], firstChunk);
            if (firstChunk < toCopy)
                memcpy(dst + firstChunk, &fBuffer[0], toCopy - firstChunk);
            fReadPos = (fReadPos + toCopy) % capacity;
            fAvailable -= toCopy;
        }
        if (toCopy < size)
            memset(dst + toCopy, 0, size - toCopy);
    }

private:
    std::mutex fMutex;
    std::vector<uint8_t> fBuffer;
    size_t fWritePos = 0, fReadPos = 0, fAvailable = 0;
};

AudioRingBuffer g_playbackRing;

// BSoundPlayer::BufferPlayerFunc: called on BSoundPlayer's own thread
// whenever it needs more audio to send to the Mixer.
void PlaybackCallback(void* cookie, void* buffer, size_t size,
        const media_raw_audio_format& format) {
    AudioRingBuffer* ring = (AudioRingBuffer*)cookie;
    if (ring)
        ring->Read(buffer, size);
    else
        memset(buffer, 0, size);
}

class AudioTapNode : public BBufferConsumer, public BMediaEventLooper {
public:
    media_input fInput;

    AudioTapNode()
        : BMediaNode("hrecord Audio Tap"),
          BBufferConsumer(B_MEDIA_RAW_AUDIO),
          BMediaEventLooper()
    {
        AddNodeKind(B_BUFFER_CONSUMER);

        fInput.node = Node();
        fInput.destination = media_destination(ControlPort(), 0);
        fInput.source = media_source::null;
        strcpy(fInput.name, "hrecord Tap In");

        // This node never precisely schedules anything -- it just hands off
        // every buffer it receives, synchronously, to the encoder and the
        // playback ring buffer.
        SetRunMode(BMediaNode::B_RECORDING);
        SetTimeSource(nullptr);
    }

    virtual ~AudioTapNode() {
        BMediaEventLooper::Quit();
    }

    void SetEncoder(AudioEncoder* encoder) { fEncoder = encoder; }
    void SetPlaybackRing(AudioRingBuffer* ring) { fRing = ring; }

    // --- BMediaNode ---
    virtual BMediaAddOn* AddOn(int32* internalID) const { return nullptr; }

    virtual void NodeRegistered() {
        SetRunMode(BMediaNode::B_RECORDING);
        Run();
        set_thread_priority(ControlThread(), B_REAL_TIME_PRIORITY);
    }

    virtual status_t HandleMessage(int32 code, const void* data, size_t size) {
        if (BBufferConsumer::HandleMessage(code, data, size) == B_OK) return B_OK;
        if (BMediaEventLooper::HandleMessage(code, data, size) == B_OK) return B_OK;
        return BMediaNode::HandleMessage(code, data, size);
    }

    virtual void HandleEvent(const media_timed_event* event, bigtime_t lateness,
            bool realTimeEvent = false) {
        if (event->type == BTimedEventQueue::B_HANDLE_BUFFER)
            BufferReceived((BBuffer*)event->pointer);
    }

    // --- BBufferConsumer: connected from the hijacked app ---
    virtual status_t AcceptFormat(const media_destination& dest, media_format* format) {
        if (dest.port != ControlPort() || dest.id != 0)
            return B_MEDIA_BAD_DESTINATION;
        if (format->type != B_MEDIA_RAW_AUDIO && format->type != B_MEDIA_UNKNOWN_TYPE)
            return B_MEDIA_BAD_FORMAT;
        format->type = B_MEDIA_RAW_AUDIO;
        if (format->u.raw_audio.frame_rate == media_raw_audio_format::wildcard.frame_rate)
            format->u.raw_audio.frame_rate = 44100.0f;
        if (format->u.raw_audio.format == media_raw_audio_format::wildcard.format)
            format->u.raw_audio.format = media_raw_audio_format::B_AUDIO_FLOAT;
        if (format->u.raw_audio.channel_count == media_raw_audio_format::wildcard.channel_count)
            format->u.raw_audio.channel_count = 2;
        return B_OK;
    }

    virtual status_t GetNextInput(int32* cookie, media_input* out_input) {
        if (*cookie != 0)
            return B_BAD_INDEX;
        *out_input = fInput;
        *cookie = 1;
        return B_OK;
    }

    virtual void DisposeInputCookie(int32 cookie) {}

    virtual void BufferReceived(BBuffer* buffer) {
        if (!buffer)
            return;

        void* data = buffer->Data();
        size_t size = buffer->SizeUsed();

        if (fEncoder != nullptr && g_running)
            EncodeAudioSamples(fEncoder, data, size, fInput.format.u.raw_audio);

        if (fRing != nullptr)
            fRing->Write(data, size);

        buffer->Recycle();
    }

    virtual void ProducerDataStatus(const media_destination& forWhom, int32 status,
            bigtime_t atPerformanceTime) {}

    virtual status_t GetLatencyFor(const media_destination& forWhom, bigtime_t* _latency,
            media_node_id* _timesource) {
        *_latency = 5000;
        *_timesource = TimeSource() ? TimeSource()->ID() : 0;
        return B_OK;
    }

    virtual status_t Connected(const media_source& producer, const media_destination& where,
            const media_format& format, media_input* out_input) {
        fInput.source = producer;
        fInput.format = format;
        *out_input = fInput;
        return B_OK;
    }

    virtual void Disconnected(const media_source& producer, const media_destination& where) {
        fInput.source = media_source::null;
    }

    virtual status_t FormatChanged(const media_source& producer, const media_destination& consumer,
            int32 changeTag, const media_format& format) {
        fInput.format = format;
        return B_OK;
    }

private:
    AudioEncoder* fEncoder = nullptr;
    AudioRingBuffer* fRing = nullptr;
};

// Everything SetupDesktopAudioTap() needs to remember so
// TeardownDesktopAudioTap() can put the hijacked app back exactly as it
// found it.
struct AudioTapHandles {
    AudioTapNode* node = nullptr;
    BSoundPlayer* player = nullptr;
    media_node appNode;
    media_output originalAppOutput; // the app's output as connected before hrecord touched it
    bool active = false;
};

// Finds one currently-playing app and redirects its connection to the
// System Mixer through a new AudioTapNode, then starts a BSoundPlayer to
// keep its audio actually audible (see the block comment above). On
// failure, the app's original connection (if any was touched) is left
// exactly as it was found.
bool SetupDesktopAudioTap(BMediaRoster* roster, AudioTapHandles* handles,
        media_raw_audio_format* outFormat) {
    media_node mixerNode;
    if (roster->GetAudioMixer(&mixerNode) != B_OK) {
        std::cerr << "[-] Error: Could not reach the System Mixer." << std::endl;
        return false;
    }

    media_input mixerInput;
    int32 inCount = 0;
    if (roster->GetConnectedInputsFor(mixerNode, &mixerInput, 1, &inCount) != B_OK || inCount < 1) {
        std::cerr << "[-] Error: Nothing is currently playing into the System Mixer to capture. "
            "Start playback somewhere and try again." << std::endl;
        return false;
    }

    media_node_id appNodeId = roster->NodeIDFor(mixerInput.source.port);
    media_node appNode;
    if (appNodeId < 0 || roster->GetNodeFor(appNodeId, &appNode) != B_OK) {
        std::cerr << "[-] Error: Could not resolve the app currently playing audio." << std::endl;
        return false;
    }

    media_output appOutput;
    int32 outCount = 0;
    if (roster->GetConnectedOutputsFor(appNode, &appOutput, 1, &outCount) != B_OK || outCount < 1
            || appOutput.destination != mixerInput.destination) {
        std::cerr << "[-] Error: Could not confirm the playing app's connection to the Mixer."
            << std::endl;
        return false;
    }

    handles->appNode = appNode;
    handles->originalAppOutput = appOutput;

    AudioTapNode* tap = new AudioTapNode();
    tap->fInput.format = appOutput.format;
    if (roster->RegisterNode(tap) != B_OK) {
        std::cerr << "[-] Error: Failed to register the audio tap node." << std::endl;
        delete tap;
        return false;
    }

    // Briefly stop the app before touching its connection, so it can't push
    // a buffer into a destination that's mid-swap.
    roster->StopNode(appNode, 0, true);
    snooze(50000);

    if (roster->Disconnect(appOutput, mixerInput) != B_OK) {
        std::cerr << "[-] Error: Failed to detach the playing app from the Mixer." << std::endl;
        roster->StartNode(appNode, 0);
        roster->ReleaseNode(tap->Node());
        return false;
    }
    snooze(20000);

    media_format fmt = appOutput.format;
    media_output newAppOutput;
    media_input newTapInput;
    status_t err = roster->Connect(appOutput.source, tap->fInput.destination, &fmt,
        &newAppOutput, &newTapInput);
    if (err != B_OK) {
        std::cerr << "[-] Error: Failed to connect the playing app to the audio tap (error "
            << err << ")." << std::endl;
        media_format restoreFmt = appOutput.format;
        media_output restoredOutput;
        media_input restoredInput;
        roster->Connect(appOutput.source, mixerInput.destination, &restoreFmt, &restoredOutput,
            &restoredInput);
        roster->StartNode(appNode, 0);
        roster->ReleaseNode(tap->Node());
        return false;
    }

    media_raw_audio_format negotiated = newTapInput.format.u.raw_audio;

    // Size the ring buffer to hold roughly half a second of audio -- enough
    // to smooth out the difference in cadence between the app's own buffer
    // delivery and the Mixer's, without adding excessive playback latency.
    int bytesPerFrame = (negotiated.format & media_raw_audio_format::B_AUDIO_SIZE_MASK)
        * (int)(negotiated.channel_count > 0 ? negotiated.channel_count : 2);
    float rate = negotiated.frame_rate > 0 ? negotiated.frame_rate : 44100.0f;
    size_t ringCapacity = bytesPerFrame > 0
        ? (size_t)(bytesPerFrame * rate * 0.5) : 65536;
    g_playbackRing.Init(ringCapacity);
    tap->SetPlaybackRing(&g_playbackRing);

    BSoundPlayer* player = new BSoundPlayer(&negotiated, "hrecord Playback", PlaybackCallback,
        nullptr, &g_playbackRing);
    if (player->InitCheck() != B_OK) {
        std::cerr << "[-] Error: Could not start local audio playback (error "
            << player->InitCheck() << ")." << std::endl;
        delete player;
        roster->Disconnect(newAppOutput, newTapInput);
        media_format restoreFmt = appOutput.format;
        media_output restoredOutput;
        media_input restoredInput;
        roster->Connect(appOutput.source, mixerInput.destination, &restoreFmt, &restoredOutput,
            &restoredInput);
        roster->StartNode(appNode, 0);
        roster->ReleaseNode(tap->Node());
        return false;
    }
    player->SetHasData(true);
    player->Start();

    roster->StartNode(tap->Node(), 0);
    roster->StartNode(appNode, 0);

    *outFormat = negotiated;

    handles->node = tap;
    handles->player = player;
    handles->active = true;
    return true;
}

// Stops playback, disconnects the tap, and reconnects the hijacked app
// directly back to the System Mixer, exactly where it was before hrecord
// touched it.
void TeardownDesktopAudioTap(BMediaRoster* roster, AudioTapHandles* handles) {
    if (!handles->active || !handles->node)
        return;

    roster->StopNode(handles->appNode, 0, true);
    roster->StopNode(handles->node->Node(), 0, true);
    snooze(50000);

    if (handles->player) {
        handles->player->Stop();
        delete handles->player;
        handles->player = nullptr;
    }

    // Disconnect the hijacked app from the tap.
    media_input tapInput;
    int32 c2 = 0;
    if (roster->GetConnectedInputsFor(handles->node->Node(), &tapInput, 1, &c2) == B_OK && c2 >= 1) {
        roster->Disconnect(handles->appNode.node, tapInput.source, handles->node->Node().node,
            tapInput.destination);
    }

    roster->ReleaseNode(handles->node->Node());
    handles->node = nullptr;

    // Reconnect the app straight to the Mixer, letting it hand back a fresh
    // input -- we didn't retain the app's original destination.id, and the
    // Mixer treats any of its free inputs identically.
    status_t err = B_ERROR;
    media_node mixerNode;
    if (roster->GetAudioMixer(&mixerNode) == B_OK) {
        media_input freeInput;
        int32 freeCount = 0;
        if (roster->GetFreeInputsFor(mixerNode, &freeInput, 1, &freeCount, B_MEDIA_RAW_AUDIO) == B_OK
                && freeCount >= 1) {
            media_format restoreFormat = handles->originalAppOutput.format;
            media_output restoredOutput;
            media_input restoredInput;
            err = roster->Connect(handles->originalAppOutput.source, freeInput.destination,
                &restoreFormat, &restoredOutput, &restoredInput);
        }
    }
    if (err != B_OK) {
        std::cerr << "[!] Warning: Could not automatically reconnect the app back to the Mixer "
            "(error " << err << "). It may have stopped playing; restart it manually if needed."
            << std::endl;
    }

    roster->StartNode(handles->appNode, 0);
    handles->active = false;
}

int main(int argc, char* argv[]) {
    // ========================================================================
    // Argument parsing: "start" (default) / "stop", plus an optional
    // --audioonly flag that restricts recording to desktop audio only.
    // ========================================================================
    bool audioOnly = false;
    bool stopRequested = false;
    bool listAudioInputs = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "stop") == 0) {
            stopRequested = true;
        } else if (strcmp(argv[i], "start") == 0) {
            // default behavior, nothing to flag
        } else if (strcmp(argv[i], "--audioonly") == 0 || strcmp(argv[i], "--audoonly") == 0) {
            audioOnly = true;
        } else if (strcmp(argv[i], "--list-audio-inputs") == 0) {
            listAudioInputs = true;
        } else {
            std::cout << "Usage: hrecord [start|stop] [--audioonly] [--list-audio-inputs]" << std::endl;
            return 0;
        }
    }

    // ========================================================================
    // FIXED SIGNAL CONTROLLER: Uses absolute kernel execution paths
    // ========================================================================
    if (stopRequested) {
        int32 cookie = 0;
        team_info info;
        pid_t myPid = getpid();
        bool found = false;

        // Determine our own absolute binary path layout dynamically from the OS kernel
        std::string myPath = "";
        team_info myInfo;
        if (get_team_info(myPid, &myInfo) == B_OK) {
            myPath = myInfo.args;
            // Trim trailing parameters if present to isolate the raw path token
            size_t spacePos = myPath.find(' ');
            if (spacePos != std::string::npos) {
                myPath = myPath.substr(0, spacePos);
            }
        }

        while (get_next_team_info(&cookie, &info) == B_OK) {
            if (info.team == myPid) continue;

            std::string targetArgs = info.args;
            size_t spacePos = targetArgs.find(' ');
            std::string targetBinary = (spacePos != std::string::npos) ? targetArgs.substr(0, spacePos) : targetArgs;

            // Absolute verification: Only signal if the binary images match exactly
            // and the target process does NOT contain our transient shutdown command flag.
            if (targetBinary == myPath || (targetBinary.find("hrecord") != std::string::npos && targetBinary != "stop")) {
                if (targetArgs.find("stop") == std::string::npos) {
                    kill(info.team, SIGINT); // Fire clean Ctrl+C directly into the recorder
                    found = true;
                }
            }
        }
        if (found) {
            std::cout << "[+] Sent stop signal to recording instance." << std::endl;
            return 0;
        } else {
            std::cerr << "[-] Error: No active hrecord recording session found." << std::endl;
            return -1;
        }
    }
    // ========================================================================

    // 1. Keep FFmpeg's per-frame chatter quiet, but let real errors through --
    // codecs like libvorbis log a specific reason via av_log before handing
    // back a bare AVERROR, and AV_LOG_QUIET was swallowing that detail.
    av_log_set_level(AV_LOG_ERROR);

    // 2. Register terminal Ctrl+C intercept hook
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    // 3. Initialize Haiku Application Context
    BApplication haikuApp("application/x-vnd.hrecord");

    if (listAudioInputs) {
        BMediaRoster* roster = BMediaRoster::Roster();
        if (!roster) {
            std::cerr << "[-] Error: Could not reach the media_server." << std::endl;
            return -1;
        }
        ListAudioInputs(roster);
        return 0;
    }

    // 4. Query Desktop Size (skipped entirely in --audioonly mode)
    BScreen screen(B_MAIN_SCREEN_ID);
    BRect screenFrame;
    int width = 0, height = 0;
    if (!audioOnly) {
        if (!screen.IsValid()) {
            std::cerr << "[-] Error: Failed to initialize Haiku native BScreen handler." << std::endl;
            return -1;
        }
        screenFrame = screen.Frame();
        width = screenFrame.IntegerWidth() + 1;
        height = screenFrame.IntegerHeight() + 1;
    }

    // 5. Build FFmpeg Container and Muxing Pipeline
    //    --audioonly  -> standalone Ogg/Vorbis file (no legal baggage: open,
    //                    royalty-free codec and container)
    //    default      -> Matroska file carrying MJPEG video plus, when the
    //                    desktop-audio tap could be set up, a Vorbis audio
    //                    track alongside it
    const char* output_filename = audioOnly
        ? "/boot/home/hrecord_capture.ogg"
        : "/boot/home/hrecord_capture.mkv";

    AVFormatContext* fmtCtx = nullptr;
    const char* muxerName = audioOnly ? "ogg" : nullptr;
    if (avformat_alloc_output_context2(&fmtCtx, nullptr, muxerName, output_filename) < 0) {
        std::cerr << "[-] Error: Failed to allocate FFmpeg output context." << std::endl;
        return -1;
    }

    // 5a. Hijack one currently-playing app's connection to the System Mixer,
    // then set up the Vorbis encoder using whatever format that negotiated.
    // If either step fails, the tap (if any) is torn back down immediately
    // so the hijacked app is never left mid-rewire.
    BMediaRoster* mediaRoster = BMediaRoster::Roster();
    AudioTapHandles audioTap;
    bool audioConnected = false;

    if (mediaRoster == nullptr) {
        std::cerr << "[!] Warning: Could not reach the media_server; recording without audio."
            << std::endl;
    } else {
        media_raw_audio_format negotiated;
        if (SetupDesktopAudioTap(mediaRoster, &audioTap, &negotiated)) {
            audioTap.node->SetEncoder(&g_audioEnc);
            if (SetupAudioEncoder(fmtCtx, negotiated, &g_audioEnc)) {
                audioConnected = true;
            } else {
                std::cerr << "[-] Error: Desktop-audio tap connected, but the Vorbis encoder "
                    "failed to start; recording without audio." << std::endl;
                TeardownDesktopAudioTap(mediaRoster, &audioTap);
            }
        } else {
            std::cerr << "[!] Warning: Could not set up desktop-audio capture; recording without "
                "audio." << std::endl;
        }
    }

    if (!audioConnected && audioOnly) {
        std::cerr << "[-] Error: --audioonly requires desktop-audio capture, which could not be "
            "set up right now." << std::endl;
        avformat_free_context(fmtCtx);
        return -1;
    }

    // 6. Video encoder setup (skipped in --audioonly mode)
    AVStream* videoStream = nullptr;
    AVCodecContext* videoCodecCtx = nullptr;
    AVFrame* encodingFrame = nullptr;
    BBitmap* screenBitmap = nullptr;
    struct SwsContext* swsCtx = nullptr;

    if (!audioOnly) {
        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
        if (!codec) {
            std::cerr << "[-] Error: MJPEG Encoder subsystem not found." << std::endl;
            return -1;
        }

        videoStream = avformat_new_stream(fmtCtx, codec);
        if (!videoStream) {
            std::cerr << "[-] Error: Failed to create output video stream." << std::endl;
            return -1;
        }

        videoCodecCtx = avcodec_alloc_context3(codec);
        videoCodecCtx->width = width;
        videoCodecCtx->height = height;

        // Set time_base to microseconds for precision real-world timing matching
        videoCodecCtx->time_base = {1, 1000000};
        videoCodecCtx->framerate = {TARGET_FPS, 1};
        videoCodecCtx->pix_fmt = AV_PIX_FMT_YUVJ420P;

        if (avcodec_open2(videoCodecCtx, codec, nullptr) < 0) {
            std::cerr << "[-] Error: Cannot open video encoder." << std::endl;
            return -1;
        }

        if (avcodec_parameters_from_context(videoStream->codecpar, videoCodecCtx) < 0) {
            std::cerr << "[-] Error: Failed to transfer codec parameters." << std::endl;
            return -1;
        }
    }

    if (!(fmtCtx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&fmtCtx->pb, output_filename, AVIO_FLAG_WRITE) < 0) {
            std::cerr << "[-] Error: Failed to open output capture file." << std::endl;
            return -1;
        }
    }

    if (avformat_write_header(fmtCtx, nullptr) < 0) {
        std::cerr << "[-] Error: Failed writing file container headers." << std::endl;
        return -1;
    }

    // 6b. Setup Video Framing Allocations (skipped in --audioonly mode)
    if (!audioOnly) {
        encodingFrame = av_frame_alloc();
        encodingFrame->format = videoCodecCtx->pix_fmt;
        encodingFrame->width = width;
        encodingFrame->height = height;
        if (av_image_alloc(encodingFrame->data, encodingFrame->linesize, width, height,
                videoCodecCtx->pix_fmt, 32) < 0) {
            std::cerr << "[-] Error: Could not allocate raw video image buffers." << std::endl;
            return -1;
        }
    }

    if (audioOnly) {
        std::cout << "[+] Desktop Audio Recording Started!" << std::endl;
    } else if (audioConnected) {
        std::cout << "[+] Screen + Desktop Audio Recording Started!" << std::endl;
    } else {
        std::cout << "[+] Screen Recording Started!" << std::endl;
    }

    // Track recording epoch base start time in microseconds
    bigtime_t recordingStartTime = system_time();
    AVPacket* pkt = av_packet_alloc();

    {
	    const char* targetUrl = "https://raw.githubusercontent.com/ablyssx74/hrecord/refs/heads/main/VERSION";
	    const char* localVersion = "v1.4.0";

	    char updateCmd[1024];
	    snprintf(updateCmd, sizeof(updateCmd),
	        "(REMOTE_V=$(curl -sL \"%s\" | tr -d '\\r\\n'); "
	        "if [ ! -z \"$REMOTE_V\" ] && [ \"$REMOTE_V\" != \"%s\" ]; then "
	        "notify --title \"Update Available\" --group \"hrecord\" "
	        "\"A newer version of hrecord is available! ($REMOTE_V)\"; fi) &",
	        targetUrl, localVersion);
	    system(updateCmd);
	}


    // 7. Main Core Recording Loop
    if (audioOnly) {
        // Nothing to poll here - the tap node's own control thread drives
        // BufferReceived() as buffers arrive. Just idle until asked to stop.
        while (g_running) {
            snooze(200000);
        }
    } else {
        while (g_running) {
            bigtime_t loopIterationStart = system_time();

            if (screen.GetBitmap(&screenBitmap, false, &screenFrame) == B_OK && screenBitmap != nullptr) {
                void* pixelBuffer = screenBitmap->Bits();

                swsCtx = sws_getCachedContext(swsCtx, width, height, AV_PIX_FMT_BGRA,
                                              width, height, videoCodecCtx->pix_fmt,
                                              SWS_BICUBIC, nullptr, nullptr, nullptr);

                uint8_t* srcData[] = { (uint8_t*)pixelBuffer, nullptr, nullptr, nullptr };
                int srcLinesize[] = { (int)screenBitmap->BytesPerRow(), 0, 0, 0 };
                sws_scale(swsCtx, srcData, srcLinesize, 0, height, encodingFrame->data, encodingFrame->linesize);

                // PTS is determined by actual elapsed real-world microseconds
                bigtime_t currentPresentationTime = system_time() - recordingStartTime;
                encodingFrame->pts = currentPresentationTime;

                {
                    std::lock_guard<std::mutex> lock(g_muxMutex);
                    if (avcodec_send_frame(videoCodecCtx, encodingFrame) == 0) {
                        while (avcodec_receive_packet(videoCodecCtx, pkt) == 0) {
                            av_packet_rescale_ts(pkt, videoCodecCtx->time_base, videoStream->time_base);
                            pkt->stream_index = videoStream->index;
                            av_interleaved_write_frame(fmtCtx, pkt);
                            av_packet_unref(pkt);
                        }
                    }
                }

                delete screenBitmap;
                screenBitmap = nullptr;
            }

            bigtime_t loopIterationElapsed = system_time() - loopIterationStart;
            if (loopIterationElapsed < FRAME_DELAY) {
                snooze(FRAME_DELAY - loopIterationElapsed);
            } else {
                snooze(1000);
            }
        }
    }

    // 8. Stream Finalization & Clean Up
    std::cout << "\n[+] Clean shutdown initiated. Finalizing output file container..." << std::endl;

    if (audioConnected) {
        // Stop the tap/playback and restore the app's direct connection to
        // the Mixer *before* flushing the encoder, so no BufferReceived()
        // call can race the final flush below.
        TeardownDesktopAudioTap(mediaRoster, &audioTap);

        DrainAudioFifo(&g_audioEnc, true);
        {
            std::lock_guard<std::mutex> lock(g_muxMutex);
            avcodec_send_frame(g_audioEnc.codecCtx, nullptr);
            while (avcodec_receive_packet(g_audioEnc.codecCtx, pkt) == 0) {
                av_packet_rescale_ts(pkt, g_audioEnc.codecCtx->time_base, g_audioEnc.stream->time_base);
                pkt->stream_index = g_audioEnc.stream->index;
                av_interleaved_write_frame(fmtCtx, pkt);
                av_packet_unref(pkt);
            }
        }
        avcodec_free_context(&g_audioEnc.codecCtx);
        swr_free(&g_audioEnc.swr);
        av_audio_fifo_free(g_audioEnc.fifo);
    }

    if (!audioOnly) {
        avcodec_send_frame(videoCodecCtx, nullptr);
        while (avcodec_receive_packet(videoCodecCtx, pkt) == 0) {
            av_packet_rescale_ts(pkt, videoCodecCtx->time_base, videoStream->time_base);
            av_interleaved_write_frame(fmtCtx, pkt);
            av_packet_unref(pkt);
        }

        av_freep(&encodingFrame->data);
        av_frame_free(&encodingFrame);
        avcodec_free_context(&videoCodecCtx);
        if (swsCtx) sws_freeContext(swsCtx);
    }

    av_write_trailer(fmtCtx);
    av_packet_free(&pkt);

    if (!(fmtCtx->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&fmtCtx->pb);
    }
    avformat_free_context(fmtCtx);

    std::cout << "[+] Output written to '" << output_filename << "' successfully!" << std::endl;
    return 0;
}
