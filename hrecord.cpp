#include <InterfaceKit.h> // Pulls in BApplication, BScreen, BBitmap
#include <StorageKit.h>
#include <SupportKit.h>   // Pulls in system_time()
#include <MediaRoster.h>
#include <MediaRecorder.h>
#include <MediaAddOn.h>
#include <MediaDefs.h>
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
// muxed from main() while audio packets are muxed from the Media Kit's
// recorder-node thread (see AudioRecordHook below).
std::mutex g_muxMutex;

void signalHandler(int signum) {
    g_running = false;
}

// ============================================================================
// Desktop audio (loopback) capture
//
// Haiku's System Mixer only ever advertises a single output (see
// AudioMixer::GetNextOutput() in the Haiku sources), and in a normal desktop
// that output is already wired to the sound card - so a second consumer
// can't simply "tap" it the way a Cortex tee filter would. Instead, hrecord
// looks for a genuine hardware/driver loopback *input* - the same mechanism
// behind "Stereo Mix" / "What U Hear" on other platforms - which some audio
// chips/drivers expose as an ordinary physical capture input alongside the
// microphone. Recording through it just reads a capture device the driver
// legitimately offers (nothing is intercepted off another process or user),
// which is why this stays free of the legal issues a real audio tap would
// raise. If no such input is present on the machine's hardware, hrecord says
// so plainly instead of pretending to capture audio that isn't there.
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

// Looks for a physical audio input whose name suggests it is a desktop-audio
// loopback ("Stereo Mix", "What U Hear", ...) rather than a microphone/line-in.
bool FindDesktopAudioLoopback(BMediaRoster* roster, dormant_node_info* outInfo) {
    const int32 kMaxInputs = 64;
    dormant_node_info infos[kMaxInputs];
    int32 count = kMaxInputs;

    media_format outputFormat;
    outputFormat.type = B_MEDIA_RAW_AUDIO;
    outputFormat.u.raw_audio = media_raw_audio_format::wildcard;

    if (roster->GetDormantNodes(infos, &count, nullptr, &outputFormat, nullptr,
            B_BUFFER_PRODUCER | B_PHYSICAL_INPUT) != B_OK) {
        return false;
    }
    if (count > kMaxInputs)
        count = kMaxInputs;

    static const char* kLoopbackHints[] = {
        "stereo mix", "loopback", "loop back", "what u hear",
        "wave out", "monitor", "mix output", "mixed output", nullptr
    };

    for (int32 i = 0; i < count; i++) {
        std::string name(infos[i].name);
        for (char& c : name)
            c = (char)tolower((unsigned char)c);

        for (int h = 0; kLoopbackHints[h] != nullptr; h++) {
            if (name.find(kLoopbackHints[h]) != std::string::npos) {
                *outInfo = infos[i];
                return true;
            }
        }
    }
    return false;
}

// Adds a Vorbis audio stream to fmtCtx and wires up the resampler/FIFO used
// to buffer the recorder's raw callbacks into fixed-size encoder frames.
// Vorbis (Ogg's native audio codec) is royalty-free and unencumbered, which
// is why it's used here regardless of whether the output container is
// standalone Ogg (--audioonly) or the Matroska file shared with the video.
bool SetupAudioEncoder(AVFormatContext* fmtCtx, const media_raw_audio_format& raw,
        AudioEncoder* enc) {
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_VORBIS);
    if (!codec) {
        std::cerr << "[-] Error: Vorbis encoder subsystem not found." << std::endl;
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
    // FFmpeg's built-in Vorbis encoder is still flagged experimental.
    codecCtx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

    // Open (and validate) the encoder before touching fmtCtx at all, so a
    // failure here never leaves a half-configured stream behind in the output.
    if (avcodec_open2(codecCtx, codec, nullptr) < 0) {
        std::cerr << "[-] Error: Cannot open audio encoder." << std::endl;
        avcodec_free_context(&codecCtx);
        return false;
    }

    AVStream* stream = avformat_new_stream(fmtCtx, codec);
    if (!stream) {
        std::cerr << "[-] Error: Failed to create output audio stream." << std::endl;
        avcodec_free_context(&codecCtx);
        return false;
    }

    if (avcodec_parameters_from_context(stream->codecpar, codecCtx) < 0) {
        std::cerr << "[-] Error: Failed to transfer audio codec parameters." << std::endl;
        avcodec_free_context(&codecCtx);
        return false;
    }
    stream->time_base = codecCtx->time_base;

    AVChannelLayout inLayout;
    av_channel_layout_default(&inLayout, channels);

    SwrContext* swr = nullptr;
    int swrErr = swr_alloc_set_opts2(&swr, &codecCtx->ch_layout, AV_SAMPLE_FMT_FLTP,
        sampleRate, &inLayout, HaikuAudioFormatToAV(raw.format), sampleRate, 0, nullptr);
    av_channel_layout_uninit(&inLayout);
    if (swrErr < 0 || !swr || swr_init(swr) < 0) {
        std::cerr << "[-] Error: Failed to initialize audio resampler." << std::endl;
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

// BMediaRecorder::ProcessFunc hook - invoked on the recorder node's own
// thread every time a fresh chunk of raw audio arrives from the loopback
// input. Resamples into planar float, buffers it, and hands complete
// encoder-sized frames off to the Vorbis encoder as they become available.
void AudioRecordHook(void* cookie, bigtime_t timestamp, void* data, size_t size,
        const media_format& format) {
    AudioEncoder* enc = (AudioEncoder*)cookie;
    if (!enc->codecCtx || !g_running)
        return;

    int sampleSize = format.u.raw_audio.format & media_raw_audio_format::B_AUDIO_SIZE_MASK;
    int bytesPerFrame = sampleSize * (int)format.u.raw_audio.channel_count;
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

void AudioNotifyHook(void* cookie, BMediaRecorder::notification code, ...) {
    // Nothing to react to here today; present so SetHooks() has a target.
}

int main(int argc, char* argv[]) {
    // ========================================================================
    // Argument parsing: "start" (default) / "stop", plus an optional
    // --audioonly flag that restricts recording to desktop audio only.
    // ========================================================================
    bool audioOnly = false;
    bool stopRequested = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "stop") == 0) {
            stopRequested = true;
        } else if (strcmp(argv[i], "start") == 0) {
            // default behavior, nothing to flag
        } else if (strcmp(argv[i], "--audioonly") == 0 || strcmp(argv[i], "--audoonly") == 0) {
            audioOnly = true;
        } else {
            std::cout << "Usage: hrecord [start|stop] [--audioonly]" << std::endl;
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

    // 1. Silence FFmpeg logging noise completely
    av_log_set_level(AV_LOG_QUIET);

    // 2. Register terminal Ctrl+C intercept hook
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    // 3. Initialize Haiku Application Context
    BApplication haikuApp("application/x-vnd.hrecord");

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
    //    default      -> Matroska file carrying MJPEG video plus, when a
    //                    desktop-audio loopback input is available, a Vorbis
    //                    audio track alongside it
    const char* output_filename = audioOnly
        ? "/boot/home/hrecord_capture.ogg"
        : "/boot/home/hrecord_capture.mkv";

    AVFormatContext* fmtCtx = nullptr;
    const char* muxerName = audioOnly ? "ogg" : nullptr;
    if (avformat_alloc_output_context2(&fmtCtx, nullptr, muxerName, output_filename) < 0) {
        std::cerr << "[-] Error: Failed to allocate FFmpeg output context." << std::endl;
        return -1;
    }

    // 5a. Locate a desktop-audio loopback input, if the hardware/driver offers one.
    BMediaRoster* mediaRoster = BMediaRoster::Roster();
    dormant_node_info loopbackInfo;
    bool haveAudioSource = mediaRoster != nullptr
        && FindDesktopAudioLoopback(mediaRoster, &loopbackInfo);

    if (!haveAudioSource) {
        if (audioOnly) {
            std::cerr << "[-] Error: No desktop-audio loopback input (e.g. \"Stereo Mix\"/"
                "\"What U Hear\") was found on this system's audio hardware. "
                "hrecord can only capture desktop audio where the driver exposes one." << std::endl;
            avformat_free_context(fmtCtx);
            return -1;
        } else {
            std::cerr << "[!] Warning: No desktop-audio loopback input found; "
                "recording video only (no audio track)." << std::endl;
        }
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

    // 6a. Audio encoder + BMediaRecorder connection setup
    BMediaRecorder* audioRecorder = nullptr;
    media_node audioSourceNode;
    bool audioConnected = false;

    if (haveAudioSource) {
        media_node instantiated;
        if (mediaRoster->InstantiateDormantNode(loopbackInfo, &instantiated) != B_OK) {
            std::cerr << "[-] Error: Failed to instantiate desktop-audio loopback input \""
                << loopbackInfo.name << "\"." << std::endl;
            haveAudioSource = false;
        } else {
            audioSourceNode = instantiated;

            audioRecorder = new BMediaRecorder("hrecord audio", B_MEDIA_RAW_AUDIO);
            media_format acceptFormat;
            acceptFormat.type = B_MEDIA_RAW_AUDIO;
            acceptFormat.u.raw_audio = media_raw_audio_format::wildcard;
            audioRecorder->SetAcceptedFormat(acceptFormat);

            media_output audioOutput;
            int32 outCount = 0;
            status_t err = mediaRoster->GetFreeOutputsFor(audioSourceNode, &audioOutput, 1,
                &outCount, B_MEDIA_RAW_AUDIO);

            if (err != B_OK || outCount < 1) {
                std::cerr << "[-] Error: Desktop-audio loopback input \"" << loopbackInfo.name
                    << "\" has no free output to record from." << std::endl;
                delete audioRecorder;
                audioRecorder = nullptr;
                mediaRoster->ReleaseNode(audioSourceNode);
                haveAudioSource = false;
            } else {
                media_format connectFormat;
                connectFormat.type = B_MEDIA_RAW_AUDIO;
                connectFormat.u.raw_audio = audioOutput.format.u.raw_audio;

                audioRecorder->SetHooks(AudioRecordHook, AudioNotifyHook, &g_audioEnc);

                if (audioRecorder->Connect(audioSourceNode, &audioOutput, &connectFormat) != B_OK) {
                    std::cerr << "[-] Error: Failed to connect to desktop-audio loopback input \""
                        << loopbackInfo.name << "\"." << std::endl;
                    audioRecorder->SetHooks(nullptr, nullptr, nullptr);
                    delete audioRecorder;
                    audioRecorder = nullptr;
                    mediaRoster->ReleaseNode(audioSourceNode);
                    haveAudioSource = false;
                } else {
                    media_raw_audio_format negotiated = audioRecorder->Format().u.raw_audio;
                    if (!SetupAudioEncoder(fmtCtx, negotiated, &g_audioEnc)) {
                        audioRecorder->Disconnect();
                        audioRecorder->SetHooks(nullptr, nullptr, nullptr);
                        delete audioRecorder;
                        audioRecorder = nullptr;
                        mediaRoster->ReleaseNode(audioSourceNode);
                        haveAudioSource = false;
                    } else {
                        audioConnected = true;
                    }
                }
            }
        }

        if (!haveAudioSource && audioOnly) {
            std::cerr << "[-] Error: Could not set up desktop-audio recording." << std::endl;
            avformat_free_context(fmtCtx);
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

    if (audioConnected)
        audioRecorder->Start();

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
	    const char* localVersion = "v1.1.0";

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
        // Nothing to poll here - the recorder node's own thread drives
        // AudioRecordHook() as buffers arrive. Just idle until asked to stop.
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
        audioRecorder->Stop(true);
        audioRecorder->Disconnect();
        audioRecorder->SetHooks(nullptr, nullptr, nullptr);
        delete audioRecorder;
        mediaRoster->ReleaseNode(audioSourceNode);

        // Flush anything still sitting in the FIFO, then drain the encoder itself.
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
