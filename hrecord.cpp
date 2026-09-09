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
#include <thread>
#include <vector>
#include <cstdint>
#include <cstring>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <ctime>

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

bool g_running = true;

// Set once from the --realtime CLI flag (see main()). When true, the
// audio-tap ring buffers and the buffer size requested from BSoundPlayer
// are tuned smaller (256 frames / ~0.07s), trading away some of the
// safety margin against buffering-jitter glitches for lower monitoring
// latency -- worth it for a genuinely real-time use case (e.g. playing
// guitar live through rakarrack while recording), not something a casual
// recording needs, and only useful alongside a driver already tuned for
// low latency (see readme.md) -- this doesn't touch the driver's own
// buffer settings. These numbers were originally gated behind a separate
// --experimental flag pending real-world testing; confirmed clean with
// three simultaneous sources on a driver hand-tuned to
// play_buffer_frames 256, so they're --realtime's own defaults now.
// --experimental is still accepted on the command line (a no-op, purely
// so an existing invocation doesn't break) -- see main().
bool g_realtimeAudio = false;

// ============================================================================
// Screen recording quality profiles
//
// Capturing and MJPEG-encoding the whole screen at native resolution, every
// frame, at 30fps with no cap on either was consistently pegging a full CPU
// core -- enough to make the mouse visibly lag, since Haiku's own input/
// compositing work was fighting hrecord for that core. Frame rate and
// capture resolution are what actually drive that cost (both the sws_scale
// colorspace conversion and the MJPEG encode itself are roughly linear in
// pixel count and frame count), so those are the two levers each profile
// tunes; JPEG quality is varied alongside them mostly because it's the
// expected shape of a "quality" profile and it does trade off output size.
// ============================================================================
struct VideoProfile {
    const char* name;
    int fps;
    int maxDimension; // longest edge, in pixels; 0 = record at native resolution
    int swsFlags;      // sws_scale algorithm: cheaper flags cost less CPU per frame
    int jpegQScale;    // FFmpeg constant-quantizer scale, 1 (best) .. 31 (worst)
};

const VideoProfile kVideoProfiles[3] = {
    { "low",    15, 1280, SWS_FAST_BILINEAR, 20 },
    { "medium", 24, 1600, SWS_BILINEAR,      10 },
    { "high",   30, 0,    SWS_BICUBIC,        3 },
};

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
    // GetAudioMixer() (like GetNodeFor() and every other roster call that
    // hands back a media_node) hands out a reference hrecord is now
    // responsible for releasing -- see the note on this same pattern in
    // HijackAppIntoTap. Every exit path below releases it.

    const int32 kMax = 32;
    media_input inputs[kMax];
    int32 count = kMax;
    if (roster->GetConnectedInputsFor(mixerNode, inputs, kMax, &count) != B_OK) {
        std::cerr << "[-] Error: Failed to query the Mixer's connected inputs." << std::endl;
        roster->ReleaseNode(mixerNode);
        return;
    }

    if (count == 0) {
        std::cout << "[!] Nothing is currently playing into the System Mixer. Start playback "
            "somewhere before recording desktop audio." << std::endl;
        roster->ReleaseNode(mixerNode);
        return;
    }

    std::cout << "[+] Apps currently feeding the System Mixer (hrecord taps one of these, or "
        "taps all apps with --allaudio):" << std::endl;
    for (int32 i = 0; i < count; i++) {
        media_node_id sourceNodeId = roster->NodeIDFor(inputs[i].source.port);
        live_node_info info;
        media_node sourceNode;
        const char* name = "(unknown)";
        if (sourceNodeId >= 0 && roster->GetNodeFor(sourceNodeId, &sourceNode) == B_OK) {
            if (roster->GetLiveNodeInfo(sourceNode, &info) == B_OK)
                name = info.name;
            roster->ReleaseNode(sourceNode);
        }
        std::cout << "    - \"" << name << "\"" << std::endl;
    }

    roster->ReleaseNode(mixerNode);
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
        fOverflowBytes = 0;
        fUnderrunBytes = 0;
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
            fOverflowBytes += (size - capacity);
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
            fOverflowBytes += overflow;
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
        if (toCopy < size) {
            memset(dst + toCopy, 0, size - toCopy);
            fUnderrunBytes += (size - toCopy);
        }
    }

    // Diagnostic counters: how many bytes have ever been dropped on
    // overflow (this source producing faster than it's being drained) or
    // zero-filled on underrun (producing slower, or with gaps, than it's
    // being drained). Either one climbing during a session points directly
    // at a buffering/timing mismatch as the cause of any audible
    // pops/dropouts for that source, instead of requiring another guess
    // from the recorded waveform alone.
    size_t OverflowBytes() {
        std::lock_guard<std::mutex> lock(fMutex);
        return fOverflowBytes;
    }
    size_t UnderrunBytes() {
        std::lock_guard<std::mutex> lock(fMutex);
        return fUnderrunBytes;
    }

    // How many bytes are currently ready to Read() without zero-filling.
    // Lets a puller drain exactly what's genuinely available on its own
    // schedule (see MixEncodeWorkerLoop) instead of requesting a fixed
    // chunk size and having any shortfall silently counted as underrun.
    size_t Available() {
        std::lock_guard<std::mutex> lock(fMutex);
        return fAvailable;
    }

private:
    std::mutex fMutex;
    std::vector<uint8_t> fBuffer;
    size_t fWritePos = 0, fReadPos = 0, fAvailable = 0;
    size_t fOverflowBytes = 0;
    size_t fUnderrunBytes = 0;
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

// ============================================================================
// --allaudio: mixing multiple simultaneously-tapped sources
//
// The single-tap approach above hijacks exactly one app's own connection to
// the Mixer. --allaudio repeats that same hijack, unmodified, once per app
// currently playing -- but that leaves N independent streams, each in
// whatever raw format its own app happened to negotiate (rate, channel
// count, sample encoding can all differ between apps). Rather than trying
// to splice into the Mixer's own internal mixing (the approach that
// crashed Haiku's Mixer control thread early on, reproducibly, and was
// abandoned), every tapped source is independently resampled to one fixed
// "bus" format here, then summed together entirely in hrecord's own code.
// ============================================================================

const float kMixBusRate = 48000.0f; // fallback only, if no tapped source ever reports a usable rate
const int kMixBusChannels = 2;

// The bus rate actually in use for a session -- set once, in
// SetupAllAudioTaps, from whichever tapped source is the first to
// successfully connect. That rate is, by definition, one the Mixer/driver
// already accepted without any reconfiguration (the source was already
// playing through it before hrecord touched anything); requesting a fixed
// rate of our own choosing instead risked asking the driver to switch
// rates out from under an already-locked hardware clock domain (e.g. a
// system with its native rate set to 192kHz in Media preferences) -- a
// real, plausible cause of a multi-second startup stall before any audio
// becomes audible. Read only after SetupAllAudioTaps has run.
float g_mixBusRate = kMixBusRate;

media_raw_audio_format MixBusFormat() {
    media_raw_audio_format fmt = media_raw_audio_format::wildcard;
    fmt.frame_rate = g_mixBusRate;
    fmt.channel_count = kMixBusChannels;
    fmt.format = media_raw_audio_format::B_AUDIO_FLOAT;
    fmt.byte_order = B_MEDIA_HOST_ENDIAN;
    // Under --realtime, request 256 frames -- confirmed in testing to
    // work cleanly, matching how far a real user of this project hand-
    // tuned their own driver (play_buffer_frames 256). This is only a
    // hint (the Mixer may renegotiate it away entirely), but matching the
    // same scale as an already-tuned driver gives it the best chance of
    // being honored.
    fmt.buffer_size = g_realtimeAudio
        ? (size_t)(kMixBusChannels * sizeof(float) * 256) : 4096;
    return fmt;
}

// Resamples one buffer of raw PCM, in whatever format its own source tap
// negotiated, into the shared mix bus format (interleaved float, see
// MixBusFormat() above) and appends the result to that tap's own ring
// buffer -- where MixedPlaybackCallback picks it up alongside every other
// tapped source's ring to actually build the mix. Mirrors the allocation
// pattern EncodeAudioSamples uses above (av_samples_alloc/av_freep per
// call) rather than a fixed-size stack buffer, for the same reason: this
// runs on the tap's own real-time control thread, one call per incoming
// buffer, and buffer sizes aren't bounded tightly enough to size a stack
// array with confidence.
void MixAndBuffer(SwrContext* swr, const void* data, size_t size,
        const media_raw_audio_format& format, AudioRingBuffer* ring) {
    int sampleSize = format.format & media_raw_audio_format::B_AUDIO_SIZE_MASK;
    int bytesPerFrame = sampleSize * (int)format.channel_count;
    if (bytesPerFrame <= 0 || !swr || !ring)
        return;
    int nbSamples = (int)(size / bytesPerFrame);
    if (nbSamples <= 0)
        return;

    // Size the output buffer for the *actual* rate ratio, not a flat
    // guess. A source with a native rate well below the 48kHz bus rate
    // (e.g. a synth running its own engine at 8kHz or 11kHz) needs several
    // times as many output samples as input to upsample -- a flat "roughly
    // double" margin (sized for something like 44.1kHz -> 48kHz) silently
    // undersizes that badly. swr_convert doesn't fail loudly when that
    // happens: whatever doesn't fit in the output buffer stays buffered
    // inside swr's own internal state and only comes out on a *later*
    // call. That reads first as growing startup latency (real audio
    // piling up before any of it reaches the ring buffer) and then as
    // chopping once the ring runs dry between those delayed, bursty
    // catch-ups -- exactly what looks like "sample mismatch" but is
    // actually just an undersized buffer here.
    double ratio = (format.frame_rate > 0) ? ((double)g_mixBusRate / format.frame_rate) : 1.0;
    int maxOutSamples = (int)(nbSamples * ratio * 1.2) + 256;

    uint8_t* converted[1] = { nullptr };
    if (av_samples_alloc(converted, nullptr, kMixBusChannels, maxOutSamples,
            AV_SAMPLE_FMT_FLT, 0) < 0)
        return;

    const uint8_t* inData[1] = { (const uint8_t*)data };
    int produced = swr_convert(swr, converted, maxOutSamples, inData, nbSamples);
    if (produced > 0)
        ring->Write(converted[0], (size_t)produced * kMixBusChannels * sizeof(float));
    av_freep(&converted[0]);

    // Belt-and-suspenders: drain anything swr still had buffered from a
    // previous call (in case the ratio above ever underestimates in
    // practice) rather than letting a backlog silently compound across
    // calls. Bounded so a persistently wrong ratio can't spin this forever.
    for (int guard = 0; guard < 4; guard++) {
        int pending = swr_get_out_samples(swr, 0);
        if (pending <= 0)
            break;

        uint8_t* flushed[1] = { nullptr };
        if (av_samples_alloc(flushed, nullptr, kMixBusChannels, pending,
                AV_SAMPLE_FMT_FLT, 0) < 0)
            break;
        int flushedCount = swr_convert(swr, flushed, pending, nullptr, 0);
        if (flushedCount > 0)
            ring->Write(flushed[0], (size_t)flushedCount * kMixBusChannels * sizeof(float));
        av_freep(&flushed[0]);

        if (flushedCount <= 0)
            break;
    }
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

    // --allaudio mode: instead of encoding/playing this source directly
    // (below), resample it to the shared mix bus format and hand it off
    // via its own ring buffer for MixedPlaybackCallback to combine with
    // every other tapped source. Mutually exclusive with SetEncoder/
    // SetPlaybackRing above -- a tap is wired up one way or the other,
    // never both.
    void SetMixOutput(SwrContext* resampler, AudioRingBuffer* ring) {
        fMixResampler = resampler;
        fMixRing = ring;
    }

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
        bool feedsLivePlayback = false;

        if (fMixResampler != nullptr && fMixRing != nullptr) {
            // --allaudio path: this source only ever feeds the shared mix,
            // never the encoder or a playback ring directly.
            MixAndBuffer(fMixResampler, data, size, fInput.format.u.raw_audio, fMixRing);
            feedsLivePlayback = true;
        } else {
            if (fEncoder != nullptr && g_running)
                EncodeAudioSamples(fEncoder, data, size, fInput.format.u.raw_audio);

            if (fRing != nullptr) {
                fRing->Write(data, size);
                feedsLivePlayback = true;
            }
        }

        if (feedsLivePlayback)
            PaceToRealTime(fInput.format.u.raw_audio, size);

        buffer->Recycle();
    }

    // This node is deliberately untimed (SetTimeSource(nullptr),
    // B_RECORDING run mode -- see the constructor) so it just takes
    // whatever the hijacked app hands it, whenever, with no scheduling of
    // its own. That's harmless when nothing downstream cares about
    // wall-clock pacing -- but both single-tap and --allaudio mode always
    // feed a real-time-paced BSoundPlayer for live monitoring (via fRing
    // or fMixRing respectively), tied to the hardware's own clock. If the
    // hijacked app pushes audio faster than real time -- confirmed
    // happening in practice, tens of *millions* of bytes continuously
    // dropped in one --allaudio session, not a one-time burst -- nothing
    // was pushing back on it, and it just piled up in that ring buffer,
    // either overflowing continuously (heard as popping) or, with a big
    // enough ring to absorb it, settling into a fixed, ever-present
    // backlog instead (heard as a long delay before anything is heard,
    // and staying exactly that far behind afterward). Neither is actually
    // fixed by resizing the ring -- that only changes which of the two
    // symptoms shows up.
    //
    // The actual fix is real backpressure: pace this tap's own
    // consumption to real time so the *producer* is throttled instead,
    // the same way it naturally would be were it still connected straight
    // to the Mixer. Delaying Recycle() (back in BufferReceived, after this
    // returns) is what does that -- it's the signal Media Kit uses to let
    // a producer know it can send more.
    //
    // This deliberately doesn't just give the node a real time source
    // instead (the more "normal" way Media Kit nodes stay paced) --
    // SetTimeSource(nullptr) was chosen earlier in this project
    // specifically to avoid a reproducible BTimeSource::RealTimeFor crash
    // in a different (now-abandoned) approach, and revisiting that
    // tradeoff isn't worth the risk here when a simple snooze() achieves
    // the same effect.
    void PaceToRealTime(const media_raw_audio_format& fmt, size_t size) {
        int sampleSize = fmt.format & media_raw_audio_format::B_AUDIO_SIZE_MASK;
        int bytesPerFrame = sampleSize * (int)fmt.channel_count;
        if (bytesPerFrame <= 0 || fmt.frame_rate <= 0)
            return;

        int nbSamples = (int)(size / bytesPerFrame);
        bigtime_t bufferDurationUs = (bigtime_t)(nbSamples * 1000000.0 / fmt.frame_rate);

        bigtime_t now = system_time();
        if (fPacingStartTime == 0)
            fPacingStartTime = now;
        fPacedDurationUs += bufferDurationUs;

        bigtime_t aheadBy = fPacedDurationUs - (now - fPacingStartTime);
        if (aheadBy > 0) {
            // Cap a single snooze so a one-time anomalous burst (e.g.
            // right at startup) corrects gradually over a few calls
            // instead of blocking this thread -- and anything else it
            // needs to handle, a stop request included -- for one long
            // stretch. This is *not* a latency dial: it doesn't scale
            // down with --realtime/--experimental (a real user's testing
            // showed --experimental's previous, tighter cap turning
            // *zero* drops into millions -- a smaller cap means slower
            // correction per call, which a source that's persistently
            // running even slightly ahead of real time, not just
            // bursting once, never fully recovers from before the next
            // buffer arrives; combined with --experimental's smaller
            // ring having far less room to absorb that shortfall, drift
            // compounded across the whole session instead of clearing).
            // A single generous cap lets correction actually keep up
            // during normal operation regardless of mode; ring buffer
            // size (see SetupAllAudioTaps / SetupDesktopAudioTap) is the
            // actual latency dial.
            const bigtime_t kMaxSnooze = 50000; // 50ms
            snooze(std::min(aheadBy, kMaxSnooze));
        }
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
    SwrContext* fMixResampler = nullptr;
    AudioRingBuffer* fMixRing = nullptr;

    // --allaudio pacing state (see BufferReceived) -- unused, and so
    // harmless, in single-tap mode.
    bigtime_t fPacingStartTime = 0;
    bigtime_t fPacedDurationUs = 0;
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

// One hijacked source within an --allaudio session -- the equivalent of
// AudioTapHandles' node/appNode/originalAppOutput trio, plus the per-source
// resampler and ring buffer that feed it into the shared mix (see
// MixAndBuffer/MixedPlaybackCallback above and below).
struct AllAudioTapEntry {
    AudioTapNode* node = nullptr;
    SwrContext* resampler = nullptr;
    AudioRingBuffer* ring = nullptr;
    media_node appNode;
    media_output originalAppOutput;
    std::string appName = "(unknown)"; // for diagnostics only, e.g. overflow/underrun reports
};

// Everything SetupAllAudioTaps() needs to remember so TeardownAllAudioTaps()
// can put every hijacked app back exactly as it found them. One shared
// BSoundPlayer (not one per tap) drives both playback and encoding off the
// combined mix -- see MixedPlaybackCallback.
struct AllAudioHandles {
    std::vector<AllAudioTapEntry> taps;
    std::vector<AudioRingBuffer*> mixSources; // same rings as taps[].ring; the callback's cookie
    BSoundPlayer* player = nullptr;
    bool active = false;
};

// Printed whenever the System Mixer's own bookkeeping doesn't match reality
// -- e.g. it still reports a "connected" input for an app hrecord can no
// longer find or reach. Haiku's Mixer can be left holding one of these
// stale/ghost entries when a producer app disappears without cleanly
// disconnecting first (killed, crashed, or otherwise torn down mid-stream);
// the entry doesn't clear itself until media_server is restarted. This is
// state living inside media_server itself, not anything hrecord's own
// (short-lived, stateless-between-runs) process could have caused or can
// clean up from the outside -- so the fix is the same one Haiku's own Media
// preferences offers for exactly this situation.
void WarnStaleMediaServerState() {
    std::cerr << "[-] Error: The System Mixer is reporting an audio connection hrecord can't "
        "actually find or reach. This usually means a previous app was closed (or crashed) "
        "without cleanly disconnecting from the Mixer, leaving a stale entry behind -- a Haiku "
        "Media Kit quirk, not something hrecord caused." << std::endl;
    std::cerr << "[!] Fix: open Media preferences and click \"Restart Media Services\", then "
        "try again." << std::endl;
}

// Finds the app currently connected at mixerInput and redirects it to a
// freshly registered AudioTapNode, stopping just short of starting either
// node -- the caller wires up encoding/playback/mixing first, then starts
// both, exactly mirroring how SetupDesktopAudioTap always has. On any
// failure the app is put back exactly as found and nullptr is returned;
// on success, the tap is returned connected-but-stopped and *outNegotiated
// carries whatever raw format the connection actually settled on.
//
// Factored out of SetupDesktopAudioTap so SetupAllAudioTaps can repeat the
// same hijack -- the trickiest part of this whole file, all the Media Kit
// connect/disconnect choreography and its per-step rollback -- once per
// currently-playing app, without a second copy of it.
AudioTapNode* HijackAppIntoTap(BMediaRoster* roster, const media_input& mixerInput,
        media_node* outAppNode, media_output* outOriginalAppOutput,
        media_raw_audio_format* outNegotiated) {
    // The Mixer says something is connected at mixerInput.source -- but if
    // that producer has since disappeared without telling the Mixer, this
    // lookup fails even though GetConnectedInputsFor() just reported it as
    // live. See WarnStaleMediaServerState() above for what this means.
    media_node_id appNodeId = roster->NodeIDFor(mixerInput.source.port);
    media_node appNode;
    if (appNodeId < 0 || roster->GetNodeFor(appNodeId, &appNode) != B_OK) {
        WarnStaleMediaServerState();
        return nullptr;
    }

    // GetNodeFor() (like GetAudioMixer() and every other roster call that
    // hands back a media_node) hands out a reference hrecord now owns and
    // must eventually release -- confirmed, in practice, as the cause of
    // apps hijacked by hrecord staying listed as connected in Media
    // preferences' Audio mixer even after being fully closed (never
    // reproduces on a clean Haiku session that never ran hrecord). Every
    // failure path below that acquired appNode releases it before
    // returning nullptr; the success path hands ownership of the
    // reference to the caller via *outAppNode, for it to release once the
    // tap is torn down (UndoHijack / RestoreHijackedApp).
    media_output appOutput;
    int32 outCount = 0;
    if (roster->GetConnectedOutputsFor(appNode, &appOutput, 1, &outCount) != B_OK || outCount < 1
            || appOutput.destination != mixerInput.destination) {
        WarnStaleMediaServerState();
        roster->ReleaseNode(appNode);
        return nullptr;
    }

    AudioTapNode* tap = new AudioTapNode();
    tap->fInput.format = appOutput.format;
    if (roster->RegisterNode(tap) != B_OK) {
        std::cerr << "[-] Error: Failed to register an audio tap node." << std::endl;
        delete tap;
        roster->ReleaseNode(appNode);
        return nullptr;
    }

    // Briefly stop the app before touching its connection, so it can't push
    // a buffer into a destination that's mid-swap.
    roster->StopNode(appNode, 0, true);
    snooze(50000);

    if (roster->Disconnect(appOutput, mixerInput) != B_OK) {
        std::cerr << "[-] Error: Failed to detach a playing app from the Mixer." << std::endl;
        roster->StartNode(appNode, 0);
        roster->ReleaseNode(tap->Node());
        roster->ReleaseNode(appNode);
        return nullptr;
    }
    // Settle delay between Disconnect and Connect -- widened from 20ms to
    // 100ms (see UndoHijack/RestoreHijackedApp, which mirror this) after
    // real-world testing linked the shorter delay to media_server-side
    // connection state that outlived hrecord's own process: a completely
    // separate app (Rakarrack) later hit "SoundPlayNode::FillNextBuffer:
    // RequestBuffer failed" -- a producer failing to push a buffer through
    // a connection the Mixer still considered live -- after an hrecord
    // hijack/restore cycle, clearing only once Media Services were fully
    // restarted. Consistent with the Mixer's own internal connection state
    // not having fully settled before hrecord immediately reused it.
    snooze(100000);

    media_format fmt = appOutput.format;
    media_output newAppOutput;
    media_input newTapInput;
    status_t err = roster->Connect(appOutput.source, tap->fInput.destination, &fmt,
        &newAppOutput, &newTapInput);
    if (err != B_OK) {
        std::cerr << "[-] Error: Failed to connect a playing app to its audio tap (error "
            << err << ")." << std::endl;
        media_format restoreFmt = appOutput.format;
        media_output restoredOutput;
        media_input restoredInput;
        roster->Connect(appOutput.source, mixerInput.destination, &restoreFmt, &restoredOutput,
            &restoredInput);
        roster->StartNode(appNode, 0);
        roster->ReleaseNode(tap->Node());
        roster->ReleaseNode(appNode);
        return nullptr;
    }

    *outAppNode = appNode;
    *outOriginalAppOutput = appOutput;
    *outNegotiated = newTapInput.format.u.raw_audio;
    return tap; // caller wires up encoding/playback/mixing, then starts appNode + tap->Node()
}

// Undoes a successful HijackAppIntoTap() when something *after* it (encoder
// or player setup) fails -- disconnects the tap and reconnects the app
// straight back to the exact Mixer destination it was just freed from,
// since that's still known good this soon after HijackAppIntoTap returned.
// Neither node was ever started, so there's nothing to stop first.
void UndoHijack(BMediaRoster* roster, AudioTapNode* tap, const media_node& appNode,
        const media_output& originalAppOutput) {
    media_input tapInput;
    int32 c2 = 0;
    if (roster->GetConnectedInputsFor(tap->Node(), &tapInput, 1, &c2) == B_OK && c2 >= 1) {
        roster->Disconnect(appNode.node, tapInput.source, tap->Node().node, tapInput.destination);
    }
    // Same settle delay HijackAppIntoTap gives itself between its own
    // Disconnect and Connect (see its comment for why it's 100ms) -- this
    // is the mirror-image operation and was missing it, an asymmetry
    // worth closing regardless of whether it's the whole story behind
    // reconnect failures seen in practice.
    snooze(100000);
    media_format restoreFmt = originalAppOutput.format;
    media_output restoredOutput;
    media_input restoredInput;
    roster->Connect(originalAppOutput.source, originalAppOutput.destination, &restoreFmt,
        &restoredOutput, &restoredInput);
    roster->StartNode(appNode, 0);
    roster->ReleaseNode(tap->Node());
    // Release the reference HijackAppIntoTap's GetNodeFor() acquired and
    // handed us via appNode -- see its own comment on this.
    roster->ReleaseNode(appNode);
}

// Disconnects a running tap and reconnects its app directly back to the
// System Mixer, exactly where it was before hrecord touched it -- used at
// the end of a full recording session (see TeardownDesktopAudioTap /
// TeardownAllAudioTaps below). Unlike UndoHijack, this doesn't assume the
// app's original Mixer destination is still free (a lot may have happened
// since), so it asks the Mixer for any free input instead. Caller must
// already have stopped both the app and the tap.
void RestoreHijackedApp(BMediaRoster* roster, AudioTapNode* tap, const media_node& appNode,
        const media_output& originalAppOutput) {
    media_input tapInput;
    int32 c2 = 0;
    if (roster->GetConnectedInputsFor(tap->Node(), &tapInput, 1, &c2) == B_OK && c2 >= 1) {
        roster->Disconnect(appNode.node, tapInput.source, tap->Node().node, tapInput.destination);
    }
    roster->ReleaseNode(tap->Node());
    // Same settle delay HijackAppIntoTap gives itself between its own
    // Disconnect and Connect (see its comment for why it's 100ms) -- this
    // is the mirror-image operation and was missing it, an asymmetry
    // worth closing regardless of whether it's the whole story behind
    // reconnect failures seen in practice ("BMediaRoster::NodeIDFor:
    // failed", "Bad port ID", and a downstream app hitting
    // "SoundPlayNode::FillNextBuffer: RequestBuffer failed" after this
    // exact reconnect -- all consistent with reconnecting before the
    // Mixer's own side of the old connection has fully settled).
    snooze(100000);

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
            media_format restoreFormat = originalAppOutput.format;
            media_output restoredOutput;
            media_input restoredInput;
            err = roster->Connect(originalAppOutput.source, freeInput.destination,
                &restoreFormat, &restoredOutput, &restoredInput);
        }
        roster->ReleaseNode(mixerNode);
    }
    if (err != B_OK) {
        std::cerr << "[!] Warning: Could not automatically reconnect an app back to the Mixer "
            "(error " << err << "). It may have stopped playing; restart it manually if needed."
            << std::endl;
    }

    roster->StartNode(appNode, 0);
    // Release the reference HijackAppIntoTap's GetNodeFor() acquired and
    // handed us via appNode -- see its own comment on this. Confirmed, in
    // practice, as the reason hijacked apps stayed listed in Media
    // preferences' Audio mixer even after being fully closed.
    roster->ReleaseNode(appNode);
}

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
    bool haveInput = roster->GetConnectedInputsFor(mixerNode, &mixerInput, 1, &inCount) == B_OK
        && inCount >= 1;
    roster->ReleaseNode(mixerNode); // not needed past this point
    if (!haveInput) {
        std::cerr << "[-] Error: Nothing is currently playing into the System Mixer to capture. "
            "Start playback somewhere and try again." << std::endl;
        return false;
    }

    media_node appNode;
    media_output originalAppOutput;
    media_raw_audio_format negotiated;
    AudioTapNode* tap = HijackAppIntoTap(roster, mixerInput, &appNode, &originalAppOutput,
        &negotiated);
    if (!tap)
        return false;

    handles->appNode = appNode;
    handles->originalAppOutput = originalAppOutput;

    // Size the ring buffer to hold roughly half a second of audio -- enough
    // to smooth out the difference in cadence between the app's own buffer
    // delivery and the Mixer's, without adding excessive playback latency.
    int bytesPerFrame = (negotiated.format & media_raw_audio_format::B_AUDIO_SIZE_MASK)
        * (int)(negotiated.channel_count > 0 ? negotiated.channel_count : 2);
    float rate = negotiated.frame_rate > 0 ? negotiated.frame_rate : 44100.0f;
    // See PaceToRealTime's comment: with the tap's own consumption now
    // paced to real time, this ring's steady-state fill tracks genuine
    // jitter rather than a growing backlog, so --realtime can trade some
    // of that jitter headroom for a lower latency ceiling -- confirmed
    // clean in testing at these numbers with three simultaneous sources.
    double ringSeconds = g_realtimeAudio ? 0.07 : 0.5;
    size_t ringCapacity = bytesPerFrame > 0
        ? (size_t)(bytesPerFrame * rate * ringSeconds) : 65536;
    g_playbackRing.Init(ringCapacity);
    tap->SetPlaybackRing(&g_playbackRing);

    if (g_realtimeAudio && bytesPerFrame > 0) {
        // Same rationale as MixBusFormat(): request ~256 frames, matching
        // the scale of an already-tuned driver, rather than leaving
        // whatever buffer_size the app itself happened to negotiate with
        // the Mixer originally.
        negotiated.buffer_size = (size_t)bytesPerFrame * 256;
    }

    BSoundPlayer* player = new BSoundPlayer(&negotiated, "hrecord Playback", PlaybackCallback,
        nullptr, &g_playbackRing);
    if (player->InitCheck() != B_OK) {
        std::cerr << "[-] Error: Could not start local audio playback (error "
            << player->InitCheck() << ")." << std::endl;
        delete player;
        UndoHijack(roster, tap, appNode, originalAppOutput);
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

    RestoreHijackedApp(roster, handles->node, handles->appNode, handles->originalAppOutput);
    handles->node = nullptr;
    handles->active = false;
}

// Holds mixed-but-not-yet-encoded audio between MixedPlaybackCallback
// (producer) and MixEncodeWorkerLoop (consumer) -- see both below.
AudioRingBuffer g_mixEncodeQueue;

// Set at the very start of SetupAllAudioTaps; read back by
// MixedPlaybackCallback's one-time startup diagnostic below, so a report
// of "N seconds before anything is heard" can be pinned down to an actual
// measured elapsed time rather than a stopwatch guess.
bigtime_t g_allAudioSetupStartTime = 0;

// BSoundPlayer::BufferPlayerFunc used only in --allaudio mode. Unlike
// PlaybackCallback (which just drains one ring for one hijacked app), this
// pulls an equal-size chunk from every tapped source's own ring buffer
// (each already resampled to the shared mix bus format by MixAndBuffer,
// called from that source's own AudioTapNode::BufferReceived) and sums
// them into one combined chunk -- this single callback, on one thread, is
// where "all the currently-playing apps" actually becomes "one mixed
// stream."
//
// The sum is scaled by 1/N (a plain average) rather than added outright:
// since every individual source is itself already within [-1, 1], an
// average of N such sources can never clip, at the cost of the mix getting
// quieter as more sources join in. A fixed, guaranteed-safe tradeoff beats
// a louder mix that occasionally distorts.
//
// This callback runs under a hard deadline set by the sound driver's own
// hardware buffer depth -- as little as ~20ms under a low-latency driver
// configuration (the kind serious audio work, e.g. rakarrack, already
// tunes for). Actually encoding here used to be able to miss that
// deadline under exactly that tuning, which is audible as clicking
// independent of anything upstream. So this only mixes and hands the
// result off to g_mixEncodeQueue -- fast, fixed-cost work with no
// comparable timing risk. The actual Vorbis encode happens on its own
// thread instead; see MixEncodeWorkerLoop.
void MixedPlaybackCallback(void* cookie, void* buffer, size_t size,
        const media_raw_audio_format& format) {
    std::vector<AudioRingBuffer*>* sources = (std::vector<AudioRingBuffer*>*)cookie;

    // One-time startup diagnostic: how long after tap setup began did this
    // callback actually start running, and how much real (non-silence)
    // audio had already piled up in each source's own ring by that point?
    // Distinguishes "BSoundPlayer itself took a long time to start calling
    // back at all" from "it started quickly, but a backlog built up before
    // it did and takes a while to drain at real-time speed" -- both would
    // sound identical to a listener (a long wait, then clean audio), but
    // point at completely different places to fix.
    static bool firstCall = true;
    if (firstCall) {
        firstCall = false;
        bigtime_t elapsedUs = system_time() - g_allAudioSetupStartTime;
        std::cout << "[i] Mixed playback: first callback " << (elapsedUs / 1000)
            << "ms after tap setup began." << std::endl;
        if (sources != nullptr) {
            int idx = 0;
            for (AudioRingBuffer* ring : *sources) {
                idx++;
                size_t avail = ring->Available();
                double seconds = g_mixBusRate > 0
                    ? (double)avail / (kMixBusChannels * sizeof(float) * g_mixBusRate) : 0.0;
                std::cout << "    source #" << idx << " backlog already queued: " << avail
                    << " bytes (~" << seconds << "s)" << std::endl;
            }
        }
    }

    float* out = (float*)buffer;
    size_t sampleCount = size / sizeof(float);
    std::fill(out, out + sampleCount, 0.0f);

    if (sources == nullptr || sources->empty())
        return;

    static thread_local std::vector<float> scratch;
    if (scratch.size() < sampleCount)
        scratch.resize(sampleCount);

    float gain = 1.0f / (float)sources->size();
    for (AudioRingBuffer* ring : *sources) {
        ring->Read(scratch.data(), size);
        for (size_t i = 0; i < sampleCount; i++)
            out[i] += scratch[i] * gain;
    }

    if (g_running)
        g_mixEncodeQueue.Write(buffer, size);
}

// Runs on its own thread for the lifetime of an --allaudio recording,
// draining g_mixEncodeQueue and doing the actual (variable-latency) Vorbis
// encode work here instead of on MixedPlaybackCallback's real-time thread
// -- see the comment there for why that matters. Reads exactly what's
// currently available each pass (via AudioRingBuffer::Available()) rather
// than a fixed chunk size, so catching up faster than audio arrives never
// encodes manufactured silence into the file. Keeps draining after
// g_running goes false for one final pass, so nothing written to the
// queue right before shutdown is lost before main()'s own final
// DrainAudioFifo(..., true) flush.
void MixEncodeWorkerLoop(AudioRingBuffer* queue, AudioEncoder* enc,
        media_raw_audio_format format) {
    std::vector<uint8_t> chunk;
    auto drainOnce = [&]() {
        size_t avail = queue->Available();
        if (avail == 0)
            return;
        chunk.resize(avail);
        queue->Read(chunk.data(), chunk.size());
        EncodeAudioSamples(enc, chunk.data(), chunk.size(), format);
    };

    while (g_running) {
        drainOnce();
        snooze(5000); // 5ms: frequent enough to stay well ahead of the
                       // queue filling, with no real-time deadline to meet
    }
    drainOnce(); // final catch-up pass
}

// Forward-declared: SetupAllAudioTaps' own failure path below reuses this
// rather than duplicating teardown logic.
void TeardownAllAudioTaps(BMediaRoster* roster, AllAudioHandles* handles);

// Hijacks *every* currently-playing app into its own AudioTapNode (see
// HijackAppIntoTap), each resampled to the shared mix bus format, then
// starts one shared BSoundPlayer/MixedPlaybackCallback to combine and
// drive them. Individual sources that fail to tap are skipped with a
// warning rather than aborting the whole thing; failure is only returned
// if *no* source could be tapped at all.
bool SetupAllAudioTaps(BMediaRoster* roster, AllAudioHandles* handles,
        media_raw_audio_format* outFormat) {
    g_allAudioSetupStartTime = system_time();
    media_node mixerNode;
    if (roster->GetAudioMixer(&mixerNode) != B_OK) {
        std::cerr << "[-] Error: Could not reach the System Mixer." << std::endl;
        return false;
    }

    const int32 kMaxSources = 32;
    media_input mixerInputs[kMaxSources];
    int32 inCount = 0;
    bool haveInputs = roster->GetConnectedInputsFor(mixerNode, mixerInputs, kMaxSources, &inCount)
        == B_OK && inCount >= 1;
    roster->ReleaseNode(mixerNode); // not needed past this point
    if (!haveInputs) {
        std::cerr << "[-] Error: Nothing is currently playing into the System Mixer to capture. "
            "Start playback somewhere and try again." << std::endl;
        return false;
    }

    media_raw_audio_format busFormat = MixBusFormat(); // .frame_rate here is just the fallback
    bool busRateChosen = false;
    AVChannelLayout busLayout;
    av_channel_layout_default(&busLayout, kMixBusChannels);

    for (int32 i = 0; i < inCount; i++) {
        media_node appNode;
        media_output originalAppOutput;
        media_raw_audio_format negotiated;
        AudioTapNode* tap = HijackAppIntoTap(roster, mixerInputs[i], &appNode,
            &originalAppOutput, &negotiated);
        if (!tap)
            continue; // that source's own error was already printed; keep tapping the rest

        if (!busRateChosen) {
            // Lock the bus rate to whatever this first successfully
            // tapped source already negotiated (see g_mixBusRate above)
            // before setting up its resampler below, so even this first
            // source picks up the real rate rather than the fallback.
            busFormat.frame_rate = negotiated.frame_rate > 0 ? negotiated.frame_rate : kMixBusRate;
            g_mixBusRate = busFormat.frame_rate;
            busRateChosen = true;
        }

        AVChannelLayout inLayout;
        av_channel_layout_default(&inLayout, negotiated.channel_count > 0
            ? negotiated.channel_count : 2);

        SwrContext* resampler = nullptr;
        int swrErr = swr_alloc_set_opts2(&resampler, &busLayout, AV_SAMPLE_FMT_FLT,
            (int)busFormat.frame_rate, &inLayout, HaikuAudioFormatToAV(negotiated.format),
            (int)(negotiated.frame_rate > 0 ? negotiated.frame_rate : 44100), 0, nullptr);
        if (swrErr >= 0 && resampler)
            swrErr = swr_init(resampler);
        av_channel_layout_uninit(&inLayout);
        if (swrErr < 0 || !resampler) {
            std::cerr << "[!] Warning: Failed to set up a resampler for one audio source ("
                << AvErr(swrErr) << "); skipping it." << std::endl;
            if (resampler) swr_free(&resampler);
            UndoHijack(roster, tap, appNode, originalAppOutput);
            continue;
        }

        AllAudioTapEntry entry;
        entry.node = tap;
        entry.resampler = resampler;
        entry.ring = new AudioRingBuffer();
        // Deliberately more generous than the single-tap ring's half
        // second (see SetupDesktopAudioTap): a source whose own producer
        // delivers audio in bursts rather than a steady drip -- a network
        // stream doing its own internal buffering/rebuffering being the
        // clearest example -- can overflow (dropping audio -> a pop at the
        // seam) or underrun (zero-filled gaps -> a blip) a too-small ring
        // well before any rate mismatch is even in play. Two seconds costs
        // only ~1.5MB per tapped source and trades a bit more live-
        // monitoring lag for a lot more headroom against exactly that.
        //
        // Now that AudioTapNode paces its own consumption to real time
        // (see BufferReceived), this ring's steady-state fill level tracks
        // genuine jitter, not a growing backlog -- so under --realtime it
        // trades some of that jitter headroom back for a lower worst-case
        // latency ceiling instead, on the theory that a real-time
        // monitoring use case would rather risk an occasional glitch than
        // accept seconds of guaranteed slack it's very unlikely to ever
        // need. Confirmed clean in testing at this number with three
        // simultaneous sources.
        double ringSeconds = g_realtimeAudio ? 0.07 : 2.0;
        size_t ringCapacity =
            (size_t)(kMixBusChannels * sizeof(float) * busFormat.frame_rate * ringSeconds);
        entry.ring->Init(ringCapacity);
        entry.appNode = appNode;
        entry.originalAppOutput = originalAppOutput;
        {
            live_node_info info;
            if (roster->GetLiveNodeInfo(appNode, &info) == B_OK)
                entry.appName = info.name;
        }
        tap->SetMixOutput(resampler, entry.ring);
        handles->taps.push_back(entry);
        std::cout << "[+] Tapped source #" << handles->taps.size() << ": \"" << entry.appName
            << "\"" << std::endl;

        roster->StartNode(tap->Node(), 0);
        roster->StartNode(appNode, 0);
    }
    av_channel_layout_uninit(&busLayout);

    if (handles->taps.empty()) {
        std::cerr << "[-] Error: Could not tap any currently-playing app." << std::endl;
        return false;
    }

    handles->mixSources.clear();
    for (auto& entry : handles->taps)
        handles->mixSources.push_back(entry.ring);

    BSoundPlayer* player = new BSoundPlayer(&busFormat, "hrecord Mixed Playback",
        MixedPlaybackCallback, nullptr, &handles->mixSources);
    if (player->InitCheck() != B_OK) {
        std::cerr << "[-] Error: Could not start local mixed audio playback (error "
            << player->InitCheck() << ")." << std::endl;
        delete player;
        // Not active yet, but the taps above are already live -- tear them
        // all back down through the normal path rather than duplicating it.
        handles->active = true;
        TeardownAllAudioTaps(roster, handles);
        return false;
    }
    player->SetHasData(true);
    player->Start();
    std::cout << "[i] BSoundPlayer::Start() returned " << ((system_time() - g_allAudioSetupStartTime) / 1000)
        << "ms after tap setup began." << std::endl;

    handles->player = player;
    handles->active = true;
    *outFormat = busFormat;
    std::cout << "[+] Tapped " << handles->taps.size() << " currently-playing audio source(s)."
        << std::endl;
    return true;
}

// Stops the shared playback, then disconnects and restores every tapped
// app -- the --allaudio equivalent of TeardownDesktopAudioTap.
void TeardownAllAudioTaps(BMediaRoster* roster, AllAudioHandles* handles) {
    if (!handles->active || handles->taps.empty())
        return;

    for (auto& entry : handles->taps) {
        roster->StopNode(entry.appNode, 0, true);
        if (entry.node)
            roster->StopNode(entry.node->Node(), 0, true);
    }
    snooze(50000);

    if (handles->player) {
        handles->player->Stop();
        delete handles->player;
        handles->player = nullptr;
    }

    for (auto& entry : handles->taps) {
        if (entry.ring) {
            size_t overflow = entry.ring->OverflowBytes();
            size_t underrun = entry.ring->UnderrunBytes();
            if (overflow > 0 || underrun > 0) {
                std::cout << "[i] Audio source \"" << entry.appName << "\": " << overflow
                    << " bytes dropped (arrived faster than the mix could take them), "
                    << underrun << " bytes silence-filled (arrived slower, or with gaps, than "
                    "the mix needed them) -- a likely cause of any popping or dropouts heard "
                    "for this source." << std::endl;
            }
        }

        if (entry.node)
            RestoreHijackedApp(roster, entry.node, entry.appNode, entry.originalAppOutput);
        if (entry.resampler)
            swr_free(&entry.resampler);
        delete entry.ring;
    }

    handles->taps.clear();
    handles->mixSources.clear();
    handles->active = false;
}

int main(int argc, char* argv[]) {
    // ========================================================================
    // Argument parsing: "start" (default) / "stop", plus an optional
    // --audioonly flag that restricts recording to desktop audio only.
    // ========================================================================
    bool audioOnly = false;
    bool allAudio = false;
    bool realtimeAudio = false;
    bool experimentalAudio = false;
    bool stopRequested = false;
    bool listAudioInputs = false;
    int profileIndex = 1; // default: medium

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "stop") == 0) {
            stopRequested = true;
        } else if (strcmp(argv[i], "start") == 0) {
            // default behavior, nothing to flag
        } else if (strcmp(argv[i], "--audioonly") == 0 || strcmp(argv[i], "--audoonly") == 0) {
            audioOnly = true;
        } else if (strcmp(argv[i], "--allaudio") == 0) {
            allAudio = true;
        } else if (strcmp(argv[i], "--realtime") == 0) {
            realtimeAudio = true;
        } else if (strcmp(argv[i], "--experimental") == 0) {
            experimentalAudio = true;
        } else if (strcmp(argv[i], "--list-audio-inputs") == 0) {
            listAudioInputs = true;
        } else if (strcmp(argv[i], "--low") == 0) {
            profileIndex = 0;
        } else if (strcmp(argv[i], "--medium") == 0) {
            profileIndex = 1;
        } else if (strcmp(argv[i], "--high") == 0) {
            profileIndex = 2;
        } else {
            std::cout << "Usage: hrecord [start|stop] [--low|--medium|--high] [--audioonly] "
                "[--allaudio] [--realtime] [--experimental] [--list-audio-inputs]" << std::endl;
            return 0;
        }
    }

    // --experimental's own tuning was folded into --realtime's defaults
    // after confirming it clean in testing (see g_realtimeAudio above);
    // it's still accepted here as a no-op purely so an existing
    // invocation that includes it doesn't break.
    if (experimentalAudio && !realtimeAudio) {
        std::cout << "[i] --experimental now requires nothing extra -- its tuning is part of "
            "--realtime's own defaults. Add --realtime to get it." << std::endl;
    }

    // Read by MixBusFormat()/PaceToRealTime()/the ring-sizing code in
    // SetupDesktopAudioTap and SetupAllAudioTaps -- must be set before any
    // of those run, which the audio-tap setup below (section 5a) does.
    g_realtimeAudio = realtimeAudio;
    if (g_realtimeAudio) {
        std::cout << "[i] --realtime: using tighter audio buffers for lower monitoring "
            "latency. Best paired with a sound driver already tuned for low latency (see "
            "readme.md) -- this doesn't change the driver's own buffer settings." << std::endl;
        if (experimentalAudio) {
            std::cout << "[i] --experimental is redundant now (its tuning is already part of "
                "--realtime) but harmless to keep passing." << std::endl;
        }
    }

    const VideoProfile& profile = kVideoProfiles[profileIndex];

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
    int width = 0, height = 0;   // native screen size -- what's actually captured
    int outWidth = 0, outHeight = 0; // encode size -- what the profile scales down to
    if (!audioOnly) {
        if (!screen.IsValid()) {
            std::cerr << "[-] Error: Failed to initialize Haiku native BScreen handler." << std::endl;
            return -1;
        }
        screenFrame = screen.Frame();
        width = screenFrame.IntegerWidth() + 1;
        height = screenFrame.IntegerHeight() + 1;

        outWidth = width;
        outHeight = height;
        int longEdge = std::max(width, height);
        if (profile.maxDimension > 0 && longEdge > profile.maxDimension) {
            double scale = (double)profile.maxDimension / longEdge;
            outWidth = (int)(width * scale);
            outHeight = (int)(height * scale);
        }
        // YUV420 needs even dimensions for its chroma subsampling.
        outWidth -= outWidth % 2;
        outHeight -= outHeight % 2;
        if (outWidth < 2) outWidth = 2;
        if (outHeight < 2) outHeight = 2;
    }

    // 5. Build FFmpeg Container and Muxing Pipeline
    //    --audioonly  -> standalone Ogg/Vorbis file (no legal baggage: open,
    //                    royalty-free codec and container)
    //    default      -> Matroska file carrying MJPEG video plus, when the
    //                    desktop-audio tap could be set up, a Vorbis audio
    //                    track alongside it
    //    Each run's filename carries its own start-time timestamp
    //    (hrecord_capture_YYYYMMDD_HHMMSS.ext) so starting a new recording
    //    never silently overwrites whatever an earlier run left behind.
    time_t nowTime = time(nullptr);
    struct tm nowTm;
    localtime_r(&nowTime, &nowTm);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", &nowTm);
    std::string output_filename_str = std::string("/boot/home/hrecord_capture_") + timestamp
        + (audioOnly ? ".ogg" : ".mkv");
    const char* output_filename = output_filename_str.c_str();

    AVFormatContext* fmtCtx = nullptr;
    const char* muxerName = audioOnly ? "ogg" : nullptr;
    if (avformat_alloc_output_context2(&fmtCtx, nullptr, muxerName, output_filename) < 0) {
        std::cerr << "[-] Error: Failed to allocate FFmpeg output context." << std::endl;
        return -1;
    }

    // 5a. Hijack one currently-playing app's connection to the System Mixer
    // (or, with --allaudio, every currently-playing app at once -- see
    // SetupAllAudioTaps), then set up the Vorbis encoder using whatever
    // format that negotiated. If any step fails, whatever was tapped is torn
    // back down immediately so no hijacked app is ever left mid-rewire.
    BMediaRoster* mediaRoster = BMediaRoster::Roster();
    AudioTapHandles audioTap;
    AllAudioHandles allAudioTap;
    std::thread mixEncoderThread;
    bool audioConnected = false;

    if (mediaRoster == nullptr) {
        std::cerr << "[!] Warning: Could not reach the media_server; recording without audio."
            << std::endl;
    } else if (allAudio) {
        media_raw_audio_format negotiated;
        if (SetupAllAudioTaps(mediaRoster, &allAudioTap, &negotiated)) {
            if (SetupAudioEncoder(fmtCtx, negotiated, &g_audioEnc)) {
                audioConnected = true;
                // Two seconds, matching the per-tap mix ring sizing
                // rationale (SetupAllAudioTaps) -- slack for the encode
                // worker's own scheduling, not a real-time deadline.
                size_t queueCapacity =
                    (size_t)(kMixBusChannels * sizeof(float) * negotiated.frame_rate * 2.0);
                g_mixEncodeQueue.Init(queueCapacity);
                mixEncoderThread = std::thread(MixEncodeWorkerLoop, &g_mixEncodeQueue,
                    &g_audioEnc, negotiated);
            } else {
                std::cerr << "[-] Error: Audio sources tapped, but the Vorbis encoder failed to "
                    "start; recording without audio." << std::endl;
                TeardownAllAudioTaps(mediaRoster, &allAudioTap);
            }
        } else {
            std::cerr << "[!] Warning: Could not set up desktop-audio capture; recording without "
                "audio." << std::endl;
        }
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
        videoCodecCtx->width = outWidth;
        videoCodecCtx->height = outHeight;

        // Set time_base to microseconds for precision real-world timing matching
        videoCodecCtx->time_base = {1, 1000000};
        videoCodecCtx->framerate = {profile.fps, 1};
        videoCodecCtx->pix_fmt = AV_PIX_FMT_YUVJ420P;

        // Constant-quantizer JPEG quality, tuned per profile (see kVideoProfiles).
        videoCodecCtx->flags |= AV_CODEC_FLAG_QSCALE;
        videoCodecCtx->global_quality = FF_QP2LAMBDA * profile.jpegQScale;

        // MJPEG is intra-only, so slice threading parallelizes cleanly across
        // cores instead of the single-threaded encode this used to be -- a
        // second lever (independent of the profile) against a saturated core
        // making the rest of the system feel sluggish while recording.
        videoCodecCtx->thread_type = FF_THREAD_SLICE;
        videoCodecCtx->thread_count = 0; // let FFmpeg pick based on available cores

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
        encodingFrame->width = outWidth;
        encodingFrame->height = outHeight;
        if (av_image_alloc(encodingFrame->data, encodingFrame->linesize, outWidth, outHeight,
                videoCodecCtx->pix_fmt, 32) < 0) {
            std::cerr << "[-] Error: Could not allocate raw video image buffers." << std::endl;
            return -1;
        }

        // Allocate the capture bitmap once and reuse it every frame via
        // ReadBitmap() below, instead of calling GetBitmap() per frame.
        // GetBitmap() allocates a brand-new BBitmap (and the shared memory
        // area app_server backs it with) on every single call -- that
        // alloc/IPC round trip at full native resolution, every frame
        // regardless of the chosen profile, turned out to be the actual
        // bottleneck causing the sluggishness/mouse lag: the profiles only
        // ever changed downstream scaling/encoding cost, never this. A
        // single long-lived bitmap that app_server just refills in place
        // removes that per-frame allocation entirely.
        screenBitmap = new BBitmap(screenFrame, screen.ColorSpace());
        if (screenBitmap->InitCheck() != B_OK) {
            std::cerr << "[-] Error: Could not allocate the screen capture bitmap." << std::endl;
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
	    const char* localVersion = "v1.9.11";

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
        int frameDelay = 1000000 / profile.fps; // microseconds per frame at this profile's fps
        while (g_running) {
            bigtime_t loopIterationStart = system_time();

            if (screen.ReadBitmap(screenBitmap, false, &screenFrame) == B_OK) {
                void* pixelBuffer = screenBitmap->Bits();

                swsCtx = sws_getCachedContext(swsCtx, width, height, AV_PIX_FMT_BGRA,
                                              outWidth, outHeight, videoCodecCtx->pix_fmt,
                                              profile.swsFlags, nullptr, nullptr, nullptr);

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
            }

            bigtime_t loopIterationElapsed = system_time() - loopIterationStart;
            if (loopIterationElapsed < frameDelay) {
                snooze(frameDelay - loopIterationElapsed);
            } else {
                snooze(1000);
            }
        }
    }

    // 8. Stream Finalization & Clean Up
    std::cout << "\n[+] Clean shutdown initiated. Finalizing output file container..." << std::endl;

    if (audioConnected) {
        // Stop the tap(s)/playback and restore the app(s)' direct
        // connection to the Mixer *before* flushing the encoder, so no
        // BufferReceived()/MixedPlaybackCallback call can race the final
        // flush below.
        if (allAudio) {
            TeardownAllAudioTaps(mediaRoster, &allAudioTap);
            // No more writes into g_mixEncodeQueue past this point (the
            // BSoundPlayer that fed it is already stopped) -- join so the
            // worker's final catch-up pass has definitely happened before
            // DrainAudioFifo's own flush below.
            if (mixEncoderThread.joinable())
                mixEncoderThread.join();
            size_t qOverflow = g_mixEncodeQueue.OverflowBytes();
            size_t qUnderrun = g_mixEncodeQueue.UnderrunBytes();
            if (qOverflow > 0 || qUnderrun > 0) {
                std::cout << "[i] Mix-to-encode queue: " << qOverflow << " bytes dropped, "
                    << qUnderrun << " bytes silence-filled -- the encode worker fell behind or "
                    "ran dry at some point." << std::endl;
            }
        } else {
            TeardownDesktopAudioTap(mediaRoster, &audioTap);
        }

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
        delete screenBitmap;
        screenBitmap = nullptr;
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
