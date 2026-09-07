#include <InterfaceKit.h> // Pulls in BApplication, BScreen, BBitmap
#include <StorageKit.h>
#include <SupportKit.h>   // Pulls in system_time()
#include <MediaRoster.h>
#include <MediaAddOn.h>
#include <MediaDefs.h>
#include <MediaNode.h>
#include <BufferConsumer.h>
#include <BufferProducer.h>
#include <MediaEventLooper.h>
#include <TimeSource.h>
#include <Buffer.h>
#include <iostream>
#include <string>
#include <mutex>
#include <cctype>
#include <cstdint>
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
// muxed from main() while audio packets are muxed from the audio tee node's
// own control thread (see AudioTeeNode::BufferReceived below).
std::mutex g_muxMutex;

void signalHandler(int signum) {
    g_running = false;
}

// Renders an FFmpeg AVERROR code as text, so failures name what actually
// went wrong instead of just "it failed" -- useful since av_log is kept
// quiet (see AV_LOG_QUIET below) and would otherwise swallow the detail.
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

// Diagnostic dump for `hrecord --list-audio-inputs`: shows every dormant
// audio-producing node the media_server knows about. Mostly useful for
// sanity-checking that the System Mixer and a sound card are actually
// present before hrecord tries to splice its audio tee between them.
void ListAudioInputs(BMediaRoster* roster) {
    const int32 kMax = 128;
    dormant_node_info infos[kMax];
    int32 count = kMax;

    media_format outputFormat;
    outputFormat.type = B_MEDIA_RAW_AUDIO;
    outputFormat.u.raw_audio = media_raw_audio_format::wildcard;

    if (roster->GetDormantNodes(infos, &count, nullptr, &outputFormat, nullptr, 0) != B_OK) {
        std::cerr << "[-] Error: Failed to query audio nodes from the media_server." << std::endl;
        return;
    }
    if (count > kMax)
        count = kMax;

    if (count == 0) {
        std::cout << "[!] No audio-producing nodes were reported by the media_server." << std::endl;
        return;
    }

    std::cout << "[+] Audio-producing nodes visible to hrecord:" << std::endl;
    for (int32 i = 0; i < count; i++)
        std::cout << "    - \"" << infos[i].name << "\"" << std::endl;
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
// available. Called from AudioTeeNode::BufferReceived, on the tee's own
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
// Desktop audio capture: the tee node itself
//
// Haiku's System Mixer only ever advertises a single output (see
// AudioMixer::GetNextOutput() in the Haiku sources), and on a normal desktop
// that output is already wired directly to the sound card. To actually
// capture "what you hear", hrecord disconnects that direct wire and splices
// itself in between: Mixer -> AudioTeeNode -> sound card. The node forwards
// every buffer it receives from the Mixer downstream completely unchanged
// (so playback keeps working exactly as before) while also handing a copy
// to hrecord's own Vorbis encoder. On a clean shutdown hrecord tears the tee
// back out and reconnects the Mixer directly to the sound card, leaving the
// system exactly as it found it.
//
// This only touches hrecord's own local audio pipeline -- the very same
// signal already being sent to the speakers -- so there's nothing here that
// intercepts audio you wouldn't otherwise be able to hear yourself.
//
// Caveat: the restore-on-exit step only runs on a normal shutdown (Ctrl+C /
// `hrecord stop`). If hrecord is killed with SIGKILL or crashes while the
// tee is spliced in, the Mixer is left connected to hrecord instead of the
// sound card and system audio will go silent until something reconnects it
// (Haiku's Media preferences "Restart Media Services" does this).
// ============================================================================

class AudioTeeNode : public BBufferConsumer, public BBufferProducer, public BMediaEventLooper {
public:
    AudioTeeNode()
        : BMediaNode("hrecord Audio Tee"),
          BBufferConsumer(B_MEDIA_RAW_AUDIO),
          BBufferProducer(B_MEDIA_RAW_AUDIO),
          BMediaEventLooper()
    {
        AddNodeKind(B_BUFFER_CONSUMER | B_BUFFER_PRODUCER);
    }

    virtual ~AudioTeeNode() {
        BMediaEventLooper::Quit();
    }

    void SetFormat(const media_format& format) { fFormat = format; }
    void SetEncoder(AudioEncoder* encoder) { fEncoder = encoder; }

    // --- BMediaNode ---
    virtual BMediaAddOn* AddOn(int32* internalID) const { return nullptr; }

    virtual void NodeRegistered() {
        Run();
        set_thread_priority(ControlThread(), B_REAL_TIME_PRIORITY);
    }

    virtual status_t HandleMessage(int32 code, const void* data, size_t size) {
        if (BBufferConsumer::HandleMessage(code, data, size) == B_OK) return B_OK;
        if (BBufferProducer::HandleMessage(code, data, size) == B_OK) return B_OK;
        if (BMediaEventLooper::HandleMessage(code, data, size) == B_OK) return B_OK;
        return BMediaNode::HandleMessage(code, data, size);
    }

    virtual void HandleEvent(const media_timed_event* event, bigtime_t lateness,
            bool realTimeEvent = false) {
        // Buffers flow straight through BufferReceived()/SendBuffer() below;
        // this node never schedules timed events of its own.
    }

    // --- BBufferConsumer: input side, connected from the System Mixer ---
    virtual status_t AcceptFormat(const media_destination& dest, media_format* format) {
        if (dest.port != ControlPort() || dest.id != 0)
            return B_MEDIA_BAD_DESTINATION;
        if (format->type != B_MEDIA_RAW_AUDIO && format->type != B_MEDIA_UNKNOWN_TYPE)
            return B_MEDIA_BAD_FORMAT;
        format->type = B_MEDIA_RAW_AUDIO;
        return B_OK;
    }

    virtual status_t GetNextInput(int32* cookie, media_input* out_input) {
        if (*cookie != 0)
            return B_BAD_INDEX;
        out_input->node = Node();
        out_input->destination = media_destination(ControlPort(), 0);
        out_input->source = media_source::null;
        out_input->format = fFormat;
        strcpy(out_input->name, "hrecord Tee In");
        *cookie = 1;
        return B_OK;
    }

    virtual void DisposeInputCookie(int32 cookie) {}

    virtual void BufferReceived(BBuffer* buffer) {
        if (!buffer)
            return;

        if (fEncoder != nullptr && g_running)
            EncodeAudioSamples(fEncoder, buffer->Data(), buffer->SizeUsed(), fFormat.u.raw_audio);

        if (fOutputEnabled && fOutputDestination != media_destination::null) {
            if (SendBuffer(buffer, fOutputSource, fOutputDestination) != B_OK)
                buffer->Recycle();
        } else {
            buffer->Recycle();
        }
    }

    virtual void ProducerDataStatus(const media_destination& forWhom, int32 status,
            bigtime_t atPerformanceTime) {}

    virtual status_t GetLatencyFor(const media_destination& forWhom, bigtime_t* _latency,
            media_node_id* _timesource) {
        *_latency = 2000;
        *_timesource = TimeSource() ? TimeSource()->ID() : 0;
        return B_OK;
    }

    virtual status_t Connected(const media_source& producer, const media_destination& where,
            const media_format& format, media_input* out_input) {
        fInputSource = producer;
        fFormat = format;
        out_input->node = Node();
        out_input->source = producer;
        out_input->destination = where;
        out_input->format = format;
        strcpy(out_input->name, "hrecord Tee In");
        return B_OK;
    }

    virtual void Disconnected(const media_source& producer, const media_destination& where) {
        fInputSource = media_source::null;
    }

    virtual status_t FormatChanged(const media_source& producer, const media_destination& consumer,
            int32 changeTag, const media_format& format) {
        fFormat = format;
        return B_OK;
    }

    // --- BBufferProducer: output side, connected to the sound card ---
    virtual status_t FormatSuggestionRequested(media_type type, int32 quality, media_format* format) {
        if (type != B_MEDIA_RAW_AUDIO && type != B_MEDIA_UNKNOWN_TYPE)
            return B_MEDIA_BAD_FORMAT;
        *format = fFormat;
        return B_OK;
    }

    virtual status_t FormatProposal(const media_source& output, media_format* ioFormat) {
        if (output.port != ControlPort() || output.id != 0)
            return B_MEDIA_BAD_SOURCE;
        if (ioFormat->type != B_MEDIA_RAW_AUDIO && ioFormat->type != B_MEDIA_UNKNOWN_TYPE)
            return B_MEDIA_BAD_FORMAT;
        *ioFormat = fFormat;
        return B_OK;
    }

    virtual status_t FormatChangeRequested(const media_source& source,
            const media_destination& destination, media_format* ioFormat, int32* _deprecated_) {
        // The pass-through format is fixed for the life of the tee -- it was
        // chosen up front to match what the Mixer and sound card already
        // agreed on before hrecord spliced in.
        *ioFormat = fFormat;
        return B_ERROR;
    }

    virtual status_t GetNextOutput(int32* cookie, media_output* out_output) {
        if (*cookie != 0)
            return B_BAD_INDEX;
        out_output->node = Node();
        out_output->source = media_source(ControlPort(), 0);
        out_output->destination = media_destination::null;
        out_output->format = fFormat;
        strcpy(out_output->name, "hrecord Tee Out");
        *cookie = 1;
        return B_OK;
    }

    virtual status_t DisposeOutputCookie(int32 cookie) { return B_OK; }

    virtual status_t SetBufferGroup(const media_source& forSource, BBufferGroup* group) {
        if (forSource.port != ControlPort() || forSource.id != 0)
            return B_MEDIA_BAD_SOURCE;
        if (fInputSource == media_source::null)
            return B_OK; // nothing upstream connected yet to forward this to
        // We forward the Mixer's own buffers unchanged (see BufferReceived)
        // rather than allocating our own, so a buffer group the sound card
        // hands us here is useless to us directly. Per the BBufferProducer
        // contract, pass it upstream to the Mixer instead, so the buffers we
        // actually forward come from the group our real consumer asked for
        // -- a mismatch here would desync buffer/latency bookkeeping across
        // the whole chain instead of just being a missed optimization.
        int32 changeTag = 0;
        return SetOutputBuffersFor(fInputSource, media_destination(ControlPort(), 0), group,
            nullptr, &changeTag, false);
    }

    virtual status_t PrepareToConnect(const media_source& what, const media_destination& where,
            media_format* format, media_source* _source, char* _name) {
        if (what.port != ControlPort() || what.id != 0)
            return B_MEDIA_BAD_SOURCE;
        if (format->type != B_MEDIA_RAW_AUDIO && format->type != B_MEDIA_UNKNOWN_TYPE)
            return B_MEDIA_BAD_FORMAT;
        *format = fFormat;
        *_source = what;
        strcpy(_name, "hrecord Tee Out");
        return B_OK;
    }

    virtual void Connect(status_t error, const media_source& source,
            const media_destination& destination, const media_format& format, char* ioName) {
        strcpy(ioName, "hrecord Tee Out");
        if (error != B_OK)
            return;
        fOutputSource = source;
        fOutputDestination = destination;
        fFormat = format;
    }

    virtual void Disconnect(const media_source& what, const media_destination& where) {
        fOutputDestination = media_destination::null;
    }

    virtual void LateNoticeReceived(const media_source& what, bigtime_t howMuch,
            bigtime_t performanceTime) {}

    virtual void EnableOutput(const media_source& what, bool enabled, int32* _deprecated_) {
        fOutputEnabled = enabled;
    }

    virtual void AdditionalBufferRequested(const media_source& source,
            media_buffer_id previousBuffer, bigtime_t previousTime,
            const media_seek_tag* previousTag) {
        // We never manufacture buffers of our own -- only forward what the
        // Mixer already sent us -- so there's nothing to do here.
    }

    virtual void LatencyChanged(const media_source& source, const media_destination& destination,
            bigtime_t newLatency, uint32 flags) {}

private:
    media_format fFormat;
    media_source fInputSource = media_source::null;
    media_source fOutputSource = media_source::null;
    media_destination fOutputDestination = media_destination::null;
    AudioEncoder* fEncoder = nullptr;
    bool fOutputEnabled = true;
};

// Bundles everything SetupDesktopAudioTee() needs to remember so
// TeardownDesktopAudioTee() can put the system back exactly as it found it.
struct AudioTeeHandles {
    AudioTeeNode* node = nullptr;
    media_node mixerNode;
    media_node audioOutputNode;
    media_output originalOutput; // Mixer's output as connected before hrecord touched it
    media_input originalInput;   // sound card's input as connected before hrecord touched it
    media_output mixerToTeeOutput; // Mixer's output as connected to the tee (for teardown)
    media_input teeInputFromMixer; // tee's input as connected from the Mixer (for teardown)
    media_output teeToHwOutput;    // tee's output as connected to the sound card (for teardown)
    media_input hwInputFromTee;    // sound card's input as connected from the tee (for teardown)
    bool active = false;
};

// Splices an AudioTeeNode in between the System Mixer and the sound card, so
// the tee sees (and can hand a copy of) every buffer already flowing to the
// speakers. On success, handles->active is true and *outFormat carries the
// negotiated raw audio format. On failure, any pre-existing Mixer <-> sound
// card connection is left completely untouched.
bool SetupDesktopAudioTee(BMediaRoster* roster, AudioTeeHandles* handles,
        media_raw_audio_format* outFormat) {
    if (roster->GetAudioMixer(&handles->mixerNode) != B_OK) {
        std::cerr << "[-] Error: Could not reach the System Mixer." << std::endl;
        return false;
    }
    if (roster->GetAudioOutput(&handles->audioOutputNode) != B_OK) {
        std::cerr << "[-] Error: Could not reach the system's audio output node." << std::endl;
        return false;
    }

    int32 outCount = 0, inCount = 0;
    bool haveExisting =
        roster->GetConnectedOutputsFor(handles->mixerNode, &handles->originalOutput, 1, &outCount) == B_OK
        && outCount >= 1
        && roster->GetConnectedInputsFor(handles->audioOutputNode, &handles->originalInput, 1, &inCount) == B_OK
        && inCount >= 1;

    media_format sharedFormat;
    if (haveExisting) {
        sharedFormat = handles->originalOutput.format;
    } else {
        // Nothing is currently flowing between the Mixer and the sound card
        // (e.g. audio idle) -- wire a fresh connection instead of splicing
        // into an existing one.
        media_output freeOutput;
        media_input freeInput;
        int32 c1 = 0, c2 = 0;
        if (roster->GetFreeOutputsFor(handles->mixerNode, &freeOutput, 1, &c1, B_MEDIA_RAW_AUDIO) != B_OK
                || c1 < 1
                || roster->GetFreeInputsFor(handles->audioOutputNode, &freeInput, 1, &c2,
                    B_MEDIA_RAW_AUDIO) != B_OK
                || c2 < 1) {
            std::cerr << "[-] Error: Could not find a free Mixer output / sound card input to "
                "splice the audio tee into." << std::endl;
            return false;
        }
        handles->originalOutput = freeOutput;
        handles->originalInput = freeInput;
        sharedFormat.type = B_MEDIA_RAW_AUDIO;
        sharedFormat.u.raw_audio = media_raw_audio_format::wildcard;
    }

    AudioTeeNode* tee = new AudioTeeNode();
    tee->SetFormat(sharedFormat);
    if (roster->RegisterNode(tee) != B_OK) {
        std::cerr << "[-] Error: Failed to register the audio tee node." << std::endl;
        delete tee;
        return false;
    }

    // Assign (never restart) the same time source the Mixer and sound card
    // are already running on, before any connection negotiation happens --
    // the roster can call our GetLatencyFor() as part of that handshake, and
    // it needs TimeSource() to already be valid rather than null at that
    // point. Restarting a time source other live nodes depend on would
    // corrupt its real-time/performance-time mapping for all of them, so
    // this only ever attaches to it -- it never calls StartTimeSource().
    media_node systemTimeSource;
    if (roster->GetTimeSource(&systemTimeSource) == B_OK)
        roster->SetTimeSourceFor(tee->Node().node, systemTimeSource.node);

    if (haveExisting && roster->Disconnect(handles->originalOutput, handles->originalInput) != B_OK) {
        std::cerr << "[-] Error: Failed to detach the Mixer from the sound card." << std::endl;
        roster->ReleaseNode(tee->Node());
        return false;
    }

    media_destination teeInputDest(tee->ControlPort(), 0);
    media_source teeOutputSrc(tee->ControlPort(), 0);

    media_format fmt1 = sharedFormat;
    media_output newMixerOutput;
    media_input newTeeInput;
    status_t err = roster->Connect(handles->originalOutput.source, teeInputDest, &fmt1,
        &newMixerOutput, &newTeeInput);
    if (err != B_OK) {
        std::cerr << "[-] Error: Failed to connect the Mixer to the audio tee (error " << err
            << ")." << std::endl;
        if (haveExisting) {
            media_format restoreFmt = sharedFormat;
            media_output restoredOutput;
            media_input restoredInput;
            roster->Connect(handles->originalOutput.source, handles->originalInput.destination,
                &restoreFmt, &restoredOutput, &restoredInput);
        }
        roster->ReleaseNode(tee->Node());
        return false;
    }

    media_format fmt2 = newMixerOutput.format;
    media_output newTeeOutput;
    media_input newHwInput;
    err = roster->Connect(teeOutputSrc, handles->originalInput.destination, &fmt2,
        &newTeeOutput, &newHwInput);
    if (err != B_OK) {
        std::cerr << "[-] Error: Failed to connect the audio tee to the sound card (error " << err
            << ")." << std::endl;
        roster->Disconnect(newMixerOutput, newTeeInput);
        if (haveExisting) {
            media_format restoreFmt = sharedFormat;
            media_output restoredOutput;
            media_input restoredInput;
            roster->Connect(handles->originalOutput.source, handles->originalInput.destination,
                &restoreFmt, &restoredOutput, &restoredInput);
        }
        roster->ReleaseNode(tee->Node());
        return false;
    }

    tee->SetFormat(newTeeOutput.format);
    *outFormat = newTeeOutput.format.u.raw_audio;

    // Remember exactly what got connected (rather than re-querying it later)
    // so TeardownDesktopAudioTee() can disconnect precisely these, with no
    // risk of a query racing the node's StopNode() and coming back empty.
    handles->mixerToTeeOutput = newMixerOutput;
    handles->teeInputFromMixer = newTeeInput;
    handles->teeToHwOutput = newTeeOutput;
    handles->hwInputFromTee = newHwInput;

    // Passing 0 ("start now") lets the roster pick the actual performance
    // time itself rather than hrecord guessing one.
    roster->StartNode(tee->Node(), 0);

    handles->node = tee;
    handles->active = true;
    return true;
}

// Tears the tee back out and restores the Mixer's original direct connection
// to the sound card, so system audio is left exactly as hrecord found it.
void TeardownDesktopAudioTee(BMediaRoster* roster, AudioTeeHandles* handles) {
    if (!handles->active || !handles->node)
        return;

    roster->StopNode(handles->node->Node(), 0, true);

    // Disconnect precisely what SetupDesktopAudioTee() connected -- no
    // re-querying, so there's no window where a stale/empty query result
    // skips a Disconnect() and leaves the tee's side of the graph wired.
    roster->Disconnect(handles->teeToHwOutput, handles->hwInputFromTee);
    roster->Disconnect(handles->mixerToTeeOutput, handles->teeInputFromMixer);

    roster->ReleaseNode(handles->node->Node());
    handles->node = nullptr;

    media_format restoreFormat = handles->originalOutput.format;
    media_output restoredOutput;
    media_input restoredInput;
    status_t err = roster->Connect(handles->originalOutput.source, handles->originalInput.destination,
        &restoreFormat, &restoredOutput, &restoredInput);
    if (err != B_OK) {
        std::cerr << "[!] Warning: Could not automatically restore the Mixer's direct connection "
            "to the sound card (error " << err << "). If system audio has gone silent, use Media "
            "preferences to restart the media server." << std::endl;
    }

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
    //                    desktop-audio tee could be set up, a Vorbis audio
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

    // 5a. Splice the desktop-audio tee between the System Mixer and the
    // sound card, then set up the Vorbis encoder using whatever format that
    // negotiated. If either step fails, the tee (if any) is torn back down
    // immediately so system audio is never left mid-rewire.
    BMediaRoster* mediaRoster = BMediaRoster::Roster();
    AudioTeeHandles audioTee;
    bool audioConnected = false;

    if (mediaRoster == nullptr) {
        std::cerr << "[!] Warning: Could not reach the media_server; recording without audio."
            << std::endl;
    } else {
        media_raw_audio_format negotiated;
        if (SetupDesktopAudioTee(mediaRoster, &audioTee, &negotiated)) {
            audioTee.node->SetEncoder(&g_audioEnc);
            if (SetupAudioEncoder(fmtCtx, negotiated, &g_audioEnc)) {
                audioConnected = true;
            } else {
                std::cerr << "[-] Error: Desktop-audio tee connected, but the Vorbis encoder "
                    "failed to start; recording without audio." << std::endl;
                TeardownDesktopAudioTee(mediaRoster, &audioTee);
            }
        } else {
            std::cerr << "[!] Warning: Could not set up desktop-audio capture; recording without "
                "audio." << std::endl;
        }
    }

    if (!audioConnected && audioOnly) {
        std::cerr << "[-] Error: --audioonly requires desktop-audio capture, which could not be "
            "set up on this machine." << std::endl;
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
	    const char* localVersion = "v1.2.2";

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
        // Nothing to poll here - the tee node's own control thread drives
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
        // Stop the tee and restore the Mixer's direct connection to the
        // sound card *before* flushing the encoder, so no BufferReceived()
        // call can race the final flush below.
        TeardownDesktopAudioTee(mediaRoster, &audioTee);

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
