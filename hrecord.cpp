#include <InterfaceKit.h> // Pulls in BApplication, BScreen, BBitmap
#include <StorageKit.h>
#include <SupportKit.h>   // Pulls in system_time()
#include <iostream>
#include <signal.h>
#include <unistd.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h> 
#include <libswscale/swscale.h>
}

// Global configurations
const int TARGET_FPS = 30;
const int FRAME_DELAY = 1000000 / TARGET_FPS; // Microseconds (33.3ms)

bool g_running = true;

void signalHandler(int signum) {
    g_running = false;
}

int main(int argc, char* argv[]) {
    // 1. Silence FFmpeg logging noise completely
    av_log_set_level(AV_LOG_QUIET);

    // 2. Register terminal Ctrl+C intercept hook
    signal(SIGINT, signalHandler);

    // 3. Initialize Haiku Application Context
    BApplication haikuApp("application/x-vnd.hrecord");

    // 4. Query Desktop Size
    BScreen screen(B_MAIN_SCREEN_ID);
    if (!screen.IsValid()) {
        std::cerr << "[-] Error: Failed to initialize Haiku native BScreen handler." << std::endl;
        return -1;
    }
    
    BRect screenFrame = screen.Frame();
    int width = screenFrame.IntegerWidth() + 1;
    int height = screenFrame.IntegerHeight() + 1;

    // 5. Build FFmpeg Container and Muxing Pipeline (.mkv)
    const char* output_filename = "hrecord_capture.mkv";
    AVFormatContext* fmtCtx = nullptr;
    if (avformat_alloc_output_context2(&fmtCtx, nullptr, nullptr, output_filename) < 0) {
        std::cerr << "[-] Error: Failed to allocate FFmpeg output context." << std::endl;
        return -1;
    }

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG); 
    if (!codec) {
        std::cerr << "[-] Error: MJPEG Encoder subsystem not found." << std::endl;
        return -1;
    }

    AVStream* stream = avformat_new_stream(fmtCtx, codec);
    if (!stream) {
        std::cerr << "[-] Error: Failed to create output video stream." << std::endl;
        return -1;
    }

    AVCodecContext* codecCtx = avcodec_alloc_context3(codec);
    codecCtx->width = width;
    codecCtx->height = height;
    
    // Set time_base to microseconds for precision real-world timing matching
    codecCtx->time_base = {1, 1000000}; 
    codecCtx->framerate = {TARGET_FPS, 1};
    codecCtx->pix_fmt = AV_PIX_FMT_YUVJ420P; 

    if (avcodec_open2(codecCtx, codec, nullptr) < 0) {
        std::cerr << "[-] Error: Cannot open video encoder." << std::endl;
        return -1;
    }

    if (avcodec_parameters_from_context(stream->codecpar, codecCtx) < 0) {
        std::cerr << "[-] Error: Failed to transfer codec parameters." << std::endl;
        return -1;
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

    // 6. Setup Video Framing Allocations
    AVFrame* encodingFrame = av_frame_alloc();
    encodingFrame->format = codecCtx->pix_fmt;
    encodingFrame->width = width;
    encodingFrame->height = height;
    if (av_image_alloc(encodingFrame->data, encodingFrame->linesize, width, height, codecCtx->pix_fmt, 32) < 0) {
        std::cerr << "[-] Error: Could not allocate raw video image buffers." << std::endl;
        return -1;
    }

    BBitmap* screenBitmap = nullptr; 
    struct SwsContext* swsCtx = nullptr;

    std::cout << "[+] Headless Real-Time Recording Started!" << std::endl;
    std::cout << "[+] Press Ctrl+C in this terminal window to stop recording..." << std::endl;

    // Track recording epoch base start time in microseconds
    bigtime_t recordingStartTime = system_time();
    
   {
	    const char* targetUrl = "https://raw.githubusercontent.com/ablyssx74/hrecord/refs/heads/main/VERSION";
	    const char* localVersion = "v1.0.2"; 
	
	    char updateCmd[1024];
	    snprintf(updateCmd, sizeof(updateCmd),
	        "(REMOTE_V=$(curl -sL \"%s\" | tr -d '\\r\\n'); "
	        "if [ ! -z \"$REMOTE_V\" ] && [ \"$REMOTE_V\" != \"%s\" ]; then "
	        "notify --title \"Update Available\" --group \"hrecord\" "
	        "\"A newer version of hrecord is available! ($REMOTE_V)\"; fi) &",
	        targetUrl, localVersion);	
	    system(updateCmd);
	}

    // --- TWEAK 1: Allocate the AVPacket structure ONCE outside the loop ---
    AVPacket* pkt = av_packet_alloc();

    // 7. Main Core Recording Loop
    while (g_running) {
        bigtime_t loopIterationStart = system_time();

        if (screen.GetBitmap(&screenBitmap, false, &screenFrame) == B_OK && screenBitmap != nullptr) {
            void* pixelBuffer = screenBitmap->Bits();

            swsCtx = sws_getCachedContext(swsCtx, width, height, AV_PIX_FMT_BGRA,
                                          width, height, codecCtx->pix_fmt,
                                          SWS_BICUBIC, nullptr, nullptr, nullptr);
            
            uint8_t* srcData[4] = { (uint8_t*)pixelBuffer, nullptr, nullptr, nullptr };
            int srcLinesize[4] = { (int)screenBitmap->BytesPerRow(), 0, 0, 0 };
            sws_scale(swsCtx, srcData, srcLinesize, 0, height, encodingFrame->data, encodingFrame->linesize);

            // WALL-CLOCK FIX: PTS is determined by actual elapsed real-world microseconds
            bigtime_t currentPresentationTime = system_time() - recordingStartTime;
            encodingFrame->pts = currentPresentationTime;

            if (avcodec_send_frame(codecCtx, encodingFrame) == 0) {
                // --- TWEAK 2: Re-use the existing allocated packet wrapper ---
                while (avcodec_receive_packet(codecCtx, pkt) == 0) {
                    av_packet_rescale_ts(pkt, codecCtx->time_base, stream->time_base);
                    pkt->stream_index = stream->index;
                    av_interleaved_write_frame(fmtCtx, pkt);
                    av_packet_unref(pkt); // Wipes internal data buffer payloads, keeps structural layout allocated
                }
            }
            
            delete screenBitmap;
            screenBitmap = nullptr;
        }

        // Calculate time delta, account for system lag, and sleep accurately
        bigtime_t loopIterationElapsed = system_time() - loopIterationStart;
        if (loopIterationElapsed < FRAME_DELAY) {
            // --- TWEAK 3: Replaced usleep with snooze() for native Haiku scheduling accuracy ---
            snooze(FRAME_DELAY - loopIterationElapsed);
        } else {
            // If the CPU is severely choked up, yield this execution cycle 
            // so the Haiku App Server can draw mouse movements and inputs
            snooze(1000); 
        }
    }

    // 8. Stream Finalization & Clean Up
    std::cout << "\n[+] Clean shutdown initiated. Finalizing video file container..." << std::endl;
    
    // --- TWEAK 4: Flush any trailing video frames sitting inside the encoder pipeline cache ---
    avcodec_send_frame(codecCtx, nullptr);
    while (avcodec_receive_packet(codecCtx, pkt) == 0) {
        av_packet_rescale_ts(pkt, codecCtx->time_base, stream->time_base);
        av_interleaved_write_frame(fmtCtx, pkt);
        av_packet_unref(pkt);
    }

    av_write_trailer(fmtCtx);

    // --- TWEAK 5: Cleanly free our reusable packet allocation wrapper ---
    av_packet_free(&pkt);

    av_freep(&encodingFrame->data);
    av_frame_free(&encodingFrame);
    avcodec_free_context(&codecCtx);
    
    if (!(fmtCtx->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&fmtCtx->pb);
    }
    avformat_free_context(fmtCtx);
    if (swsCtx) sws_freeContext(swsCtx);

    std::cout << "[+] Video written to '" << output_filename << "' successfully!" << std::endl;
    return 0;
}
