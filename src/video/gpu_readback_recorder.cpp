/**
 * GPU Readback Video Recorder
 *
 * Fallback implementation that uses WebGPU texture readback for video capture.
 * Works on all platforms but has higher CPU/GPU overhead than native capture.
 *
 * Architecture:
 * - Uses AsyncCapture for non-blocking GPU->CPU frame readback
 * - Encodes frames to WebP animation using libwebp
 * - Optionally converts to MP4 using FFmpeg
 *
 * NOTE: This implementation uses Dawn callback APIs and requires WebP mux support.
 */

#include "mystral/video/video_recorder.h"
#include "mystral/video/async_capture.h"
#include <webgpu/webgpu.h>
#include "mystral/webgpu_compat.h"
#include <chrono>
#include <thread>
#include <mutex>
#include <atomic>
#include <queue>
#include <iostream>
#include <fstream>
#include <ctime>
#include <filesystem>

// GPUReadbackRecorder requires Dawn due to Dawn-specific callback APIs
#if defined(MYSTRAL_WEBGPU_DAWN) && defined(MYSTRAL_HAS_WEBP_MUX)
#define MYSTRAL_GPU_READBACK_RECORDER_AVAILABLE 1
#include <webp/encode.h>
#include <webp/mux.h>
#else
#define MYSTRAL_GPU_READBACK_RECORDER_AVAILABLE 0
#endif

#if MYSTRAL_GPU_READBACK_RECORDER_AVAILABLE

// Forward declaration of video capture callback registration from WebGPU bindings
namespace mystral { namespace webgpu {
    extern void setVideoCaptureCallback(void (*callback)(void* texture, uint32_t width, uint32_t height, void* userData), void* userData);
    extern void clearVideoCaptureCallback();
}}

namespace mystral {

namespace {

// Check if FFmpeg is available
bool checkFFmpegAvailable() {
#ifdef _WIN32
    int result = system("where ffmpeg >nul 2>nul");
#else
    int result = system("which ffmpeg >/dev/null 2>&1");
#endif
    return result == 0;
}

// Convert WebP to MP4 using FFmpeg
bool doConvertWebPToMP4(const std::string& webpPath, const std::string& mp4Path, int fps, bool deleteWebp, bool quiet) {
    if (!checkFFmpegAvailable()) {
        if (!quiet) {
            std::cerr << "[Video] FFmpeg not found. WebP file saved: " << webpPath << std::endl;
        }
        return false;
    }

    // Create temp directory for frames
    std::error_code ec;
    std::filesystem::path tempDir = std::filesystem::temp_directory_path(ec) / ("mystral-video-" + std::to_string(std::time(nullptr)));
    if (ec) {
        if (!quiet) std::cerr << "[Video] Failed to get temp directory" << std::endl;
        return false;
    }
    std::filesystem::create_directories(tempDir, ec);
    if (ec) {
        if (!quiet) std::cerr << "[Video] Failed to create temp directory" << std::endl;
        return false;
    }

    // Check if webpmux is available
    bool hasWebpmux = false;
#ifdef _WIN32
    hasWebpmux = system("where webpmux >nul 2>nul") == 0;
#else
    hasWebpmux = system("which webpmux >/dev/null 2>&1") == 0;
#endif

    bool success = false;

    if (hasWebpmux) {
        if (!quiet) std::cout << "[Video] Extracting frames with webpmux..." << std::endl;

        // Extract first frame to test
        std::string extractCmd = "webpmux -get frame 1 \"" + webpPath + "\" -o \"" + tempDir.string() + "/frame_0001.webp\"";
#ifdef _WIN32
        extractCmd += " 2>nul";
#else
        extractCmd += " 2>/dev/null";
#endif

        if (system(extractCmd.c_str()) == 0) {
            // Extract all frames
            int frameNum = 1;
            while (frameNum <= 10000) {
                char framePath[512];
                snprintf(framePath, sizeof(framePath), "%s/frame_%04d.webp", tempDir.string().c_str(), frameNum);

                std::string cmd = "webpmux -get frame " + std::to_string(frameNum) + " \"" + webpPath + "\" -o \"" + framePath + "\"";
#ifdef _WIN32
                cmd += " 2>nul";
#else
                cmd += " 2>/dev/null";
#endif
                if (system(cmd.c_str()) != 0) break;
                frameNum++;
            }

            if (frameNum > 1) {
                if (!quiet) std::cout << "[Video] Extracted " << (frameNum - 1) << " frames, encoding to MP4..." << std::endl;

                std::string ffmpegCmd = "ffmpeg -y -framerate " + std::to_string(fps) +
                    " -i \"" + tempDir.string() + "/frame_%04d.webp\" -c:v libx264 -pix_fmt yuv420p -crf 18 \"" + mp4Path + "\"";
                if (quiet) ffmpegCmd += " -loglevel quiet";

                if (system(ffmpegCmd.c_str()) == 0) {
                    success = true;
                }
            }
        }
    } else if (!quiet) {
        std::cerr << "[Video] MP4 conversion requires 'webpmux' (from libwebp)" << std::endl;
    }

    // Cleanup temp directory
    std::filesystem::remove_all(tempDir, ec);

    if (success && deleteWebp) {
        std::filesystem::remove(webpPath, ec);
    }

    return success;
}

}  // anonymous namespace
namespace video {

/**
 * GPU Readback Video Recorder Implementation
 */
class GPUReadbackRecorder : public VideoRecorder {
public:
    GPUReadbackRecorder(WGPUDevice device, WGPUQueue queue, WGPUInstance instance)
        : device_(device), queue_(queue), instance_(instance) {}

    // Static callback that forwards to the recorder instance
    static void videoCaptureCallback(void* texture, uint32_t width, uint32_t height, void* userData) {
        auto* recorder = static_cast<GPUReadbackRecorder*>(userData);
        if (recorder) {
            recorder->onVideoCaptureCallback(static_cast<WGPUTexture>(texture), width, height);
        }
    }

    // Pending buffer info for deferred processing
    struct PendingBuffer {
        WGPUBuffer buffer;
        uint32_t width;
        uint32_t height;
        uint32_t bytesPerRow;
        size_t bufferSize;
        int frameNumber;
        std::atomic<bool>* mapComplete;
        WGPUMapAsyncStatus* mapStatus;
    };
    std::vector<PendingBuffer> pendingBuffers_;
    std::mutex pendingMutex_;

    // Process any pending buffers from previous frames
    void processPendingBuffers() {
        std::vector<PendingBuffer> toProcess;
        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            for (auto it = pendingBuffers_.begin(); it != pendingBuffers_.end(); ) {
                if (it->mapComplete->load(std::memory_order_acquire)) {
                    toProcess.push_back(*it);
                    it = pendingBuffers_.erase(it);
                } else {
                    ++it;
                }
            }
        }

        for (auto& pending : toProcess) {
            if (*pending.mapStatus != WGPUMapAsyncStatus_Success) {
                droppedFrames_++;
                wgpuBufferDestroy(pending.buffer);
                wgpuBufferRelease(pending.buffer);
                delete pending.mapComplete;
                delete pending.mapStatus;
                continue;
            }

            const void* mappedData = wgpuBufferGetConstMappedRange(pending.buffer, 0, pending.bufferSize);
            if (!mappedData) {
                droppedFrames_++;
                wgpuBufferUnmap(pending.buffer);
                wgpuBufferDestroy(pending.buffer);
                wgpuBufferRelease(pending.buffer);
                delete pending.mapComplete;
                delete pending.mapStatus;
                continue;
            }

            // Copy to frame queue (BGRA -> RGBA)
            CapturedFrame frame;
            frame.width = pending.width;
            frame.height = pending.height;
            frame.frameNumber = pending.frameNumber;
            frame.timestamp = pending.frameNumber / 60.0;

            size_t pixelDataSize = static_cast<size_t>(pending.width) * static_cast<size_t>(pending.height) * 4;
            frame.pixels.resize(pixelDataSize);

            const uint8_t* src = static_cast<const uint8_t*>(mappedData);
            uint8_t* dst = frame.pixels.data();

            for (uint32_t y = 0; y < pending.height; y++) {
                const uint8_t* srcRow = src + static_cast<size_t>(y) * pending.bytesPerRow;
                uint8_t* dstRow = dst + static_cast<size_t>(y) * pending.width * 4;
                for (uint32_t x = 0; x < pending.width; x++) {
                    size_t srcIdx = static_cast<size_t>(x) * 4;
                    size_t dstIdx = static_cast<size_t>(x) * 4;
                    dstRow[dstIdx + 0] = srcRow[srcIdx + 2];  // R <- B
                    dstRow[dstIdx + 1] = srcRow[srcIdx + 1];  // G
                    dstRow[dstIdx + 2] = srcRow[srcIdx + 0];  // B <- R
                    dstRow[dstIdx + 3] = srcRow[srcIdx + 3];  // A
                }
            }

            wgpuBufferUnmap(pending.buffer);

            {
                std::lock_guard<std::mutex> lock(frameMutex_);
                frameQueue_.push(std::move(frame));
            }

            wgpuBufferDestroy(pending.buffer);
            wgpuBufferRelease(pending.buffer);
            delete pending.mapComplete;
            delete pending.mapStatus;

            stats_.capturedFrames++;
        }
    }

    // Called from the video capture callback during present
    void onVideoCaptureCallback(WGPUTexture texture, uint32_t width, uint32_t height) {
        if (!recording_ || !texture || !device_ || !queue_) {
            return;
        }

        // First, process any pending buffers from previous frames
        processPendingBuffers();

        // Store dimensions for encoder initialization
        if (width_ == 0 || height_ == 0) {
            width_ = width;
            height_ = height;
        }

        // Calculate buffer requirements
        uint32_t bytesPerRow = (width * 4 + 255) & ~255;  // 256-byte aligned
        size_t bufferSize = bytesPerRow * height;

        // Create a temporary buffer for this frame
        WGPUBufferDescriptor bufDesc = {};
        bufDesc.size = bufferSize;
        bufDesc.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
        bufDesc.mappedAtCreation = false;
        WGPUBuffer readbackBuffer = wgpuDeviceCreateBuffer(device_, &bufDesc);
        if (!readbackBuffer) {
            droppedFrames_++;
            return;
        }

        // Create command encoder for the copy
        WGPUCommandEncoderDescriptor encDesc = {};
        WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device_, &encDesc);

        // Copy texture to buffer
        WGPUImageCopyTexture_Compat srcCopy = {};
        srcCopy.texture = texture;
        srcCopy.mipLevel = 0;
        srcCopy.origin = {0, 0, 0};
        srcCopy.aspect = WGPUTextureAspect_All;

        WGPUImageCopyBuffer_Compat dstCopy = {};
        dstCopy.buffer = readbackBuffer;
        dstCopy.layout.offset = 0;
        dstCopy.layout.bytesPerRow = bytesPerRow;
        dstCopy.layout.rowsPerImage = height;

        WGPUExtent3D copySize = {width, height, 1};
        wgpuCommandEncoderCopyTextureToBuffer(encoder, &srcCopy, &dstCopy, &copySize);

        // Submit the copy
        WGPUCommandBufferDescriptor cmdDesc = {};
        WGPUCommandBuffer cmdBuffer = wgpuCommandEncoderFinish(encoder, &cmdDesc);
        wgpuQueueSubmit(queue_, 1, &cmdBuffer);
        wgpuCommandBufferRelease(cmdBuffer);
        wgpuCommandEncoderRelease(encoder);

        // Request async map - will be processed in next frame
        auto* mapComplete = new std::atomic<bool>(false);
        auto* mapStatus = new WGPUMapAsyncStatus(WGPUMapAsyncStatus_Success);

        auto mapCallback = [](WGPUMapAsyncStatus status, WGPUStringView message, void* userdata1, void* userdata2) {
            auto* complete = static_cast<std::atomic<bool>*>(userdata1);
            auto* statusPtr = static_cast<WGPUMapAsyncStatus*>(userdata2);
            *statusPtr = status;
            complete->store(true, std::memory_order_release);
        };

        WGPUBufferMapCallbackInfo mapInfo = {};
        mapInfo.mode = WGPUCallbackMode_AllowSpontaneous;
        mapInfo.callback = mapCallback;
        mapInfo.userdata1 = mapComplete;
        mapInfo.userdata2 = mapStatus;
        wgpuBufferMapAsync(readbackBuffer, WGPUMapMode_Read, 0, bufferSize, mapInfo);

        // Store pending buffer for deferred processing
        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            pendingBuffers_.push_back({
                readbackBuffer,
                width,
                height,
                bytesPerRow,
                bufferSize,
                frameNumber_,
                mapComplete,
                mapStatus
            });
        }

        frameNumber_++;
    }

    ~GPUReadbackRecorder() override {
        if (isRecording()) {
            stopRecording();
        }
    }

    bool startRecording(void* nativeWindowHandle,
                        const std::string& outputPath,
                        const VideoRecorderConfig& config) override {
#ifndef MYSTRAL_HAS_WEBP_MUX
        std::cerr << "[GPUReadbackRecorder] WebP mux support not available" << std::endl;
        return false;
#else
        if (recording_) {
            std::cerr << "[GPUReadbackRecorder] Already recording" << std::endl;
            return false;
        }

        if (!device_ || !queue_ || !instance_) {
            std::cerr << "[GPUReadbackRecorder] WebGPU handles not provided" << std::endl;
            return false;
        }

        // Store configuration
        config_ = config;
        outputPath_ = outputPath;

        // Determine WebP path (may be converted to MP4 later)
        webpPath_ = outputPath;
        if (config_.convertToMp4) {
            size_t dotPos = webpPath_.rfind('.');
            if (dotPos != std::string::npos) {
                std::string ext = webpPath_.substr(dotPos);
                if (ext == ".mp4" || ext == ".MP4") {
                    webpPath_ = webpPath_.substr(0, dotPos) + ".webp";
                }
            }
        }

        // Initialize WebP encoder
        if (!WebPAnimEncoderOptionsInit(&encOptions_)) {
            std::cerr << "[GPUReadbackRecorder] Failed to initialize WebP encoder options" << std::endl;
            return false;
        }

        encOptions_.anim_params.loop_count = 0;  // Infinite loop
        encOptions_.allow_mixed = 0;
        encOptions_.minimize_size = 0;
        // Force every frame to be a keyframe
        encOptions_.kmin = 1;
        encOptions_.kmax = 1;

        // Note: Encoder will be created when we know the frame dimensions
        encoder_ = nullptr;
        frameNumber_ = 0;
        timestampMs_ = 0;
        frameDurationMs_ = 1000 / config_.fps;
        encodedFrames_ = 0;
        droppedFrames_ = 0;

        startTime_ = std::chrono::high_resolution_clock::now();
        recording_ = true;
        encodingDone_ = false;

        // Start encoder thread
        encoderThread_ = std::thread([this]() {
            runEncoderThread();
        });

        // Register video capture callback to receive frames during present
        webgpu::setVideoCaptureCallback(&GPUReadbackRecorder::videoCaptureCallback, this);

        std::cout << "[GPUReadbackRecorder] Started recording to " << webpPath_ << std::endl;
        return true;
#endif
    }

    bool stopRecording() override {
#ifndef MYSTRAL_HAS_WEBP_MUX
        return false;
#else
        if (!recording_) {
            return false;
        }

        // Unregister video capture callback first
        webgpu::clearVideoCaptureCallback();

        recording_ = false;

        // Wait for any pending GPU work and process remaining buffers
        for (int i = 0; i < 100; i++) {
            wgpuDeviceTick(device_);
            if (instance_) {
                wgpuInstanceProcessEvents(instance_);
            }
            processPendingBuffers();
        }

        // Clean up any remaining pending buffers
        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            for (auto& pending : pendingBuffers_) {
                wgpuBufferDestroy(pending.buffer);
                wgpuBufferRelease(pending.buffer);
                delete pending.mapComplete;
                delete pending.mapStatus;
            }
            pendingBuffers_.clear();
        }

        encodingDone_ = true;

        // Wait for encoder thread to finish
        if (encoderThread_.joinable()) {
            encoderThread_.join();
        }

        // Finalize WebP
        bool success = finalizeWebP();

        // Calculate stats
        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime_);
        stats_.elapsedSeconds = duration.count() / 1000.0;
        stats_.avgFps = stats_.elapsedSeconds > 0 ? stats_.capturedFrames / stats_.elapsedSeconds : 0;

        // Cleanup
        if (encoder_) {
            WebPAnimEncoderDelete(encoder_);
            encoder_ = nullptr;
        }

        // Convert to MP4 if requested
        if (success && config_.convertToMp4) {
            std::string mp4Path = outputPath_;
            size_t dotPos = mp4Path.rfind('.');
            if (dotPos != std::string::npos) {
                mp4Path = mp4Path.substr(0, dotPos) + ".mp4";
            } else {
                mp4Path += ".mp4";
            }

            if (doConvertWebPToMP4(webpPath_, mp4Path, config_.fps, true, false)) {
                std::cout << "[GPUReadbackRecorder] Converted to MP4: " << mp4Path << std::endl;
            }
        }

        std::cout << "[GPUReadbackRecorder] Stopped recording. Captured " << stats_.capturedFrames
                  << " frames, encoded " << stats_.encodedFrames << " frames" << std::endl;

        return success;
#endif
    }

    bool isRecording() const override {
        return recording_;
    }

    VideoRecorderStats getStats() const override {
        return stats_;
    }

    const char* getTypeName() const override {
        return "GPUReadbackRecorder";
    }

    void processFrame() override {
        // No-op: capture happens in the callback during present
    }

    /**
     * Submit a texture for capture (called from render loop)
     * Note: With callback-based capture, this is a no-op since capture happens
     * automatically during queue.submit via the video capture callback.
     */
    bool captureFrame(void* texture, uint32_t width, uint32_t height) override {
        (void)texture;
        (void)width;
        (void)height;
        // No-op: capture happens in the callback during present
        return true;
    }

private:
#ifdef MYSTRAL_HAS_WEBP_MUX
    void runEncoderThread() {
        while (!encodingDone_ || getQueuedFrameCount() > 0) {
            video::CapturedFrame frame;
            if (tryGetFrame(frame)) {
                if (!encodeFrame(frame)) {
                    std::cerr << "[GPUReadbackRecorder] Failed to encode frame " << frame.frameNumber << std::endl;
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }

    bool tryGetFrame(video::CapturedFrame& outFrame) {
        std::lock_guard<std::mutex> lock(frameMutex_);
        if (frameQueue_.empty()) {
            return false;
        }
        outFrame = std::move(frameQueue_.front());
        frameQueue_.pop();
        return true;
    }

    size_t getQueuedFrameCount() const {
        std::lock_guard<std::mutex> lock(frameMutex_);
        return frameQueue_.size();
    }

    bool encodeFrame(const CapturedFrame& frame) {
        // Create encoder on first frame
        if (!encoder_) {
            encoder_ = WebPAnimEncoderNew(frame.width, frame.height, &encOptions_);
            if (!encoder_) {
                std::cerr << "[GPUReadbackRecorder] Failed to create WebP encoder" << std::endl;
                return false;
            }
            width_ = frame.width;
            height_ = frame.height;
        }

        // Set up WebP picture
        WebPPicture pic;
        if (!WebPPictureInit(&pic)) {
            return false;
        }

        pic.width = frame.width;
        pic.height = frame.height;
        pic.use_argb = 1;

        if (!WebPPictureAlloc(&pic)) {
            return false;
        }

        // Import RGBA data
        if (!WebPPictureImportRGBA(&pic, frame.pixels.data(), frame.width * 4)) {
            WebPPictureFree(&pic);
            return false;
        }

        // Set up encoding config
        WebPConfig config;
        if (!WebPConfigInit(&config)) {
            WebPPictureFree(&pic);
            return false;
        }

        config.quality = static_cast<float>(config_.quality);
        config.method = 4;  // Balance speed/quality

        // Add frame to animation
        if (!WebPAnimEncoderAdd(encoder_, &pic, timestampMs_, &config)) {
            std::cerr << "[GPUReadbackRecorder] Failed to add frame " << frame.frameNumber << ": "
                      << WebPAnimEncoderGetError(encoder_) << std::endl;
            WebPPictureFree(&pic);
            return false;
        }

        WebPPictureFree(&pic);

        timestampMs_ += frameDurationMs_;
        encodedFrames_++;
        stats_.encodedFrames = encodedFrames_;

        return true;
    }

    bool finalizeWebP() {
        if (!encoder_) {
            std::cerr << "[GPUReadbackRecorder] No encoder to finalize" << std::endl;
            return false;
        }

        // Add final null frame
        if (!WebPAnimEncoderAdd(encoder_, nullptr, timestampMs_, nullptr)) {
            std::cerr << "[GPUReadbackRecorder] Failed to finalize animation" << std::endl;
            return false;
        }

        // Assemble animation
        WebPData webpData;
        WebPDataInit(&webpData);

        if (!WebPAnimEncoderAssemble(encoder_, &webpData)) {
            std::cerr << "[GPUReadbackRecorder] Failed to assemble animation: "
                      << WebPAnimEncoderGetError(encoder_) << std::endl;
            return false;
        }

        // Write to file
        std::ofstream file(webpPath_, std::ios::binary);
        if (!file.is_open()) {
            std::cerr << "[GPUReadbackRecorder] Failed to open output file: " << webpPath_ << std::endl;
            WebPDataClear(&webpData);
            return false;
        }

        file.write(reinterpret_cast<const char*>(webpData.bytes), webpData.size);
        file.close();

        WebPDataClear(&webpData);
        return true;
    }
#endif

    // WebGPU handles
    WGPUDevice device_ = nullptr;
    WGPUQueue queue_ = nullptr;
    WGPUInstance instance_ = nullptr;

    // Configuration
    VideoRecorderConfig config_;
    std::string outputPath_;
    std::string webpPath_;

    // State
    std::atomic<bool> recording_{false};
    std::atomic<bool> encodingDone_{false};
    std::chrono::high_resolution_clock::time_point startTime_;

    // Frame queue for encoder thread
    std::queue<video::CapturedFrame> frameQueue_;
    mutable std::mutex frameMutex_;

    // Encoder
#ifdef MYSTRAL_HAS_WEBP_MUX
    WebPAnimEncoder* encoder_ = nullptr;
    WebPAnimEncoderOptions encOptions_;
#endif
    std::thread encoderThread_;

    // Frame tracking
    int frameNumber_ = 0;
    int timestampMs_ = 0;
    int frameDurationMs_ = 16;
    std::atomic<int> encodedFrames_{0};
    std::atomic<int> droppedFrames_{0};
    uint32_t width_ = 0;
    uint32_t height_ = 0;

    // Statistics
    VideoRecorderStats stats_;
};

// Factory function to create GPU readback recorder (used by video_recorder.cpp)
std::unique_ptr<VideoRecorder> createGPUReadbackRecorder(
    WGPUDevice device, WGPUQueue queue, WGPUInstance instance) {
    return std::make_unique<GPUReadbackRecorder>(device, queue, instance);
}

}  // namespace video
}  // namespace mystral

#else  // !MYSTRAL_GPU_READBACK_RECORDER_AVAILABLE

// Stub implementation used when WebP mux support is unavailable.
namespace mystral {
namespace video {

std::unique_ptr<VideoRecorder> createGPUReadbackRecorder(
    WGPUDevice device, WGPUQueue queue, WGPUInstance instance) {
    (void)device;
    (void)queue;
    (void)instance;
    // Return nullptr - caller will fall back to native capture or disable recording
    return nullptr;
}

}  // namespace video
}  // namespace mystral

#endif  // MYSTRAL_GPU_READBACK_RECORDER_AVAILABLE
