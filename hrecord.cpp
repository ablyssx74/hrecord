#include <InterfaceKit.h> // Pulls in BApplication, BScreen, BBitmap
#include <StorageKit.h>
#include <SupportKit.h>   // Pulls in system_time()
#include <iostream>
#include <string>
#include <signal.h>
#include <unistd.h>
#include <string.h>

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
    // ========================================================================
    // FIXED SIGNAL CONTROLLER: Uses absolute kernel execution paths
    // ========================================================================
    if (argc > 1 && argv != nullptr) {
        if (strcmp(argv[1], "stop") == 0) {
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
                if (targetBinary == myPath || (targetBinary.find("hrecord") != std::string::npos && targetBinary != argv[1])) {
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
        } else if (strcmp(argv[1], "start") != 0) {
            std::cout << "Usage: hrecord [start|stop]" << std::endl;
            return 0;
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
    const char* output_filename = "/boot/home/hrecord_capture.mkv";
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
        // FIX: Pointed directly to your local variable fmtCtx
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

    std::cout << "[+] Screen Recording Started!" << std::endl;

    // Track recording epoch base start time in microseconds
    bigtime_t recordingStartTime = system_time();
    AVPacket* pkt = av_packet_alloc();

    {
	    const char* targetUrl = "https://raw.githubusercontent.com/ablyssx74/hrecord/refs/heads/main/VERSION";
	    const char* localVersion = "v1.0.3"; 
	
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
    while (g_running) {
        bigtime_t loopIterationStart = system_time();

        if (screen.GetBitmap(&screenBitmap, false, &screenFrame) == B_OK && screenBitmap != nullptr) {
            void* pixelBuffer = screenBitmap->Bits();

            swsCtx = sws_getCachedContext(swsCtx, width, height, AV_PIX_FMT_BGRA,
                                          width, height, codecCtx->pix_fmt,
                                          SWS_BICUBIC, nullptr, nullptr, nullptr);
            
            uint8_t* srcData[] = { (uint8_t*)pixelBuffer, nullptr, nullptr, nullptr };
            int srcLinesize[] = { (int)screenBitmap->BytesPerRow(), 0, 0, 0 };
            sws_scale(swsCtx, srcData, srcLinesize, 0, height, encodingFrame->data, encodingFrame->linesize);

            // PTS is determined by actual elapsed real-world microseconds
            bigtime_t currentPresentationTime = system_time() - recordingStartTime;
            encodingFrame->pts = currentPresentationTime;

            if (avcodec_send_frame(codecCtx, encodingFrame) == 0) {
                while (avcodec_receive_packet(codecCtx, pkt) == 0) {
                    av_packet_rescale_ts(pkt, codecCtx->time_base, stream->time_base);
                    pkt->stream_index = stream->index;
                    av_interleaved_write_frame(fmtCtx, pkt);
                    av_packet_unref(pkt); 
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

    // 8. Stream Finalization & Clean Up
    std::cout << "\n[+] Clean shutdown initiated. Finalizing video file container..." << std::endl;
    
    avcodec_send_frame(codecCtx, nullptr);
    while (avcodec_receive_packet(codecCtx, pkt) == 0) {
        av_packet_rescale_ts(pkt, codecCtx->time_base, stream->time_base);
        av_interleaved_write_frame(fmtCtx, pkt);
        av_packet_unref(pkt);
    }

    av_write_trailer(fmtCtx);
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
