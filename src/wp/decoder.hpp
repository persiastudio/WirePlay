// Video decode + colour conversion, FFmpeg-backed. HEVC from the CoreDevice
// backend, H.264 from the QuickTime USB protocol; see wp::VideoCodec.
//
// Latency budget: iOS hands us a low-delay stream (no B-frames, one slice per
// frame at 60 fps), so the decoder should emit a frame for every packet with
// no reordering delay. We say so explicitly via AV_CODEC_FLAG_LOW_DELAY and a
// zeroed frame threading count — the defaults would otherwise buffer frames
// behind a frame-thread pipeline and add tens of milliseconds of glass-to-glass
// latency for nothing.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include "wp/protocol.hpp"  // wp::VideoCodec is a wire constant

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

namespace wp {

struct DecodeStats {
    uint64_t frames_decoded = 0;
    uint64_t decode_errors = 0;
    // Frames the decoder returned with concealed damage (missing or
    // broken references/slices). Distinguishes "the bitstream arrives
    // hurt" from "the presenter hurts it": glitches of this family
    // grow with motion and survive until the next keyframe.
    uint64_t corrupt_frames = 0;
    uint64_t frames_dropped_late = 0;
    int width = 0;
    int height = 0;
    std::string hwaccel;  // "" when running the software decoder
};

// Decodes the Annex-B access units the engine sends. Codec-agnostic on
// purpose: the CoreDevice backend streams HEVC and the QuickTime USB protocol
// streams H.264, and everything downstream of here is identical for both.
class VideoDecoder {
public:
    VideoDecoder() = default;
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    // Tries hardware acceleration first (D3D11VA on Windows, VAAPI on Linux),
    // falling back to multithreaded software decode. Never fails outright.
    bool open(VideoCodec codec, int threads);

    // Feeds one Annex-B access unit. Returns the number of frames produced
    // (0 or 1 in practice); the frame stays valid until the next call.
    // pts90k: the frame's presentation time in 1/90000 s units (the
    // wire format's clock), or -1 when the source provided none.
    int sendAccessUnit(const uint8_t* annexb, size_t size, bool reset,
                       int64_t pts90k = -1);

    // Flushes any frames the decoder is still holding.
    int drain();

    AVFrame* frame() const { return out_; }
    // The hardware surface exactly as the decoder produced it (a D3D11
    // NV12 texture on Windows), valid when the last sendAccessUnit()
    // produced a frame on a hardware context. Zero-copy preview renders
    // from this; the CPU frame above exists for the virtual camera.
    AVFrame* gpuFrame() const { return gpu_; }
    const DecodeStats& stats() const { return stats_; }

    // When false, no CPU copy is made at all (preview-only mode: nothing
    // consumes BGRA pixels). The camera publisher needs the CPU frame, so
    // the app enables this whenever the camera is active.
    void setCpuCopy(bool enabled) { cpu_copy_ = enabled; }
    // Prefer NVDEC (CUDA hwaccel) when opening. Opt-in from the app
    // (--decoder): CUDA surfaces cannot feed the zero-copy D3D11
    // preview, so this trades frame home for the CUDA decode path;
    // frames return through the CPU conversion instead.
    void setPreferCuda(bool enabled) { prefer_cuda_ = enabled; }
    bool usingCuda() const { return cuda_active_; }

    // Optional external D3D11 device (ID3D11Device*/ID3D11DeviceContext*
    // as void*, Windows only). When set before open(), the hardware
    // decoder is created on THIS device so its output textures can be
    // sampled by the app's renderer directly -- two devices cannot share
    // textures, and a decoder-private device would force the CPU round
    // trip this exists to avoid.
    // FFmpeg's D3D11VA lock (wired to hw_mutex_ below). The camera
    // thread takes it around av_hwframe_transfer_data so its readback
    // serialises with decode submits on the shared device context.
    std::recursive_mutex& hwMutex() { return hw_mutex_; }

    void setD3D11Device(void* device, void* context) {
        d3d_device_ = device;
        d3d_context_ = context;
    }

    VideoCodec codec() const { return codec_; }

private:
    bool tryHwAccel(const char* name);
    void resetContext();
    AVCodecID codecId() const;

    VideoCodec codec_ = kCodecHevc;

    AVCodecContext* ctx_ = nullptr;
    AVCodecContext* hw_ctx_ = nullptr;
    AVBufferRef* hw_device_ = nullptr;
    AVPacket* pkt_ = nullptr;
    AVFrame* out_ = nullptr;      // final, software-format frame
    std::recursive_mutex hw_mutex_;
    bool prefer_cuda_ = false;
    bool cuda_active_ = false;
    AVFrame* raw_ = nullptr;      // as the decoder produced it (maybe hw)
    AVFrame* gpu_ = nullptr;      // referenced hw surface, for zero-copy
    bool cpu_copy_ = true;
    void* d3d_device_ = nullptr;   // ID3D11Device* (external, not owned)
    void* d3d_context_ = nullptr;  // ID3D11DeviceContext* (external)
    SwsContext* sws_ = nullptr;
    int sws_src_w_ = 0, sws_src_h_ = 0;
    AVPixelFormat sws_src_fmt_ = AV_PIX_FMT_NONE;
    DecodeStats stats_;
    int threads_ = 0;
};

}  // namespace wp
