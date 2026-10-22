#include "wp/decoder.hpp"

// FFmpeg's headers are not self-guarding against C++ name mangling, so every
// one of them has to come in inside extern "C" — missing this compiles fine
// and then fails at link time with "undefined reference" to a mangled name.
extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}
#ifdef _WIN32
// <d3d11.h> FIRST, on purpose: hwcontext_d3d11va.h includes it too, and if
// that happens inside the extern "C" block below, d3d11.h's C++ operators
// get C linkage and MSVC rejects the whole header (C2733). Once the include
// guard is set here, the FFmpeg header's own include is a no-op.
#include <d3d11.h>
extern "C" {
#include <libavutil/hwcontext_d3d11va.h>
}
#endif

#include <cstdio>
#include <cstring>

namespace wp {

VideoDecoder::~VideoDecoder() {
    if (sws_) sws_freeContext(sws_);
    if (out_) av_frame_free(&out_);
    if (raw_) av_frame_free(&raw_);
    if (pkt_) av_packet_free(&pkt_);
    if (hw_ctx_) avcodec_free_context(&hw_ctx_);
    if (ctx_) avcodec_free_context(&ctx_);
    if (hw_device_) av_buffer_unref(&hw_device_);
}

bool VideoDecoder::tryHwAccel(const char* name) {
    const bool want_cuda = std::strcmp(name, "cuda") == 0;
#ifdef _WIN32
    const AVHWDeviceType want =
        want_cuda ? AV_HWDEVICE_TYPE_CUDA : AV_HWDEVICE_TYPE_D3D11VA;
    const AVPixelFormat want_fmt = want_cuda ? AV_PIX_FMT_CUDA : AV_PIX_FMT_D3D11;
#else
    const AVHWDeviceType want =
        want_cuda ? AV_HWDEVICE_TYPE_CUDA : AV_HWDEVICE_TYPE_VAAPI;
    const AVPixelFormat want_fmt = want_cuda ? AV_PIX_FMT_CUDA : AV_PIX_FMT_VAAPI;
#endif

#ifdef _WIN32
    if (!want_cuda && d3d_device_) {
        // External device (the renderer's): build the hwdevice context on
        // it so decoded textures live on the same GPU device the window
        // samples from. References are AddRef'd: FFmpeg releases them
        // when the context is freed. The lock is real: decode submits
        // (stream thread), preview sampling (preview thread) and the
        // camera readback (camera thread) can all touch this device,
        // and FFmpeg's own use is bracketed by these callbacks.
        hw_device_ = av_hwdevice_ctx_alloc(want);
        if (!hw_device_) return false;
        auto* devctx = reinterpret_cast<AVHWDeviceContext*>(hw_device_->data);
        auto* d3dctx = reinterpret_cast<AVD3D11VADeviceContext*>(devctx->hwctx);
        d3dctx->device = static_cast<ID3D11Device*>(d3d_device_);
        d3dctx->device->AddRef();
        d3dctx->device_context = static_cast<ID3D11DeviceContext*>(d3d_context_);
        if (d3dctx->device_context) d3dctx->device_context->AddRef();
        d3dctx->lock = [](void* p) {
            static_cast<std::recursive_mutex*>(p)->lock();
        };
        d3dctx->unlock = [](void* p) {
            static_cast<std::recursive_mutex*>(p)->unlock();
        };
        d3dctx->lock_ctx = &hw_mutex_;
        if (av_hwdevice_ctx_init(hw_device_) < 0) {
            av_buffer_unref(&hw_device_);
            return false;
        }
    } else
#endif
    // CUDA/NVDEC always gets its own device: its surfaces live in CUDA
    // memory and come back through the CPU path (receiveFrame's
    // transfer), because the D3D11 preview cannot sample them. That is
    // the honest price of NVDEC here -- same decode silicon as
    // D3D11VA, different frame home -- so it is a measured opt-in
    // (--decoder cuda), never a silent default.
    if (av_hwdevice_ctx_create(&hw_device_, want, nullptr, nullptr, 0) < 0) return false;

    const AVCodec* codec = avcodec_find_decoder(codecId());
    if (!codec) { av_buffer_unref(&hw_device_); return false; }

    hw_ctx_ = avcodec_alloc_context3(codec);
    if (!hw_ctx_) { av_buffer_unref(&hw_device_); return false; }
    hw_ctx_->hw_device_ctx = av_buffer_ref(hw_device_);
    // get_format is a C callback: the wanted format travels via opaque.
    hw_ctx_->opaque =
        reinterpret_cast<void*>(static_cast<intptr_t>(want_fmt));
    hw_ctx_->get_format = [](AVCodecContext* c, const AVPixelFormat* fmts) -> AVPixelFormat {
        const auto want =
            AVPixelFormat(static_cast<intptr_t>(reinterpret_cast<intptr_t>(c->opaque)));
        for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
            if (*p == want) return *p;
        }
        return fmts[0];  // software fallback inside the same context
    };
    hw_ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    if (avcodec_open2(hw_ctx_, codec, nullptr) < 0) {
        avcodec_free_context(&hw_ctx_);
        av_buffer_unref(&hw_device_);
        return false;
    }
    ctx_ = hw_ctx_;
    cuda_active_ = want_cuda;
    // cpu_copy_ is the app's call (setCpuCopy before open): with it off,
    // CUDA surfaces stay on the GPU like D3D11 ones, and the preview /
    // camera threads bring them home on their own time instead of the
    // stream thread paying the transfer inside receiveFrame().
    stats_.hwaccel = want_cuda ? "cuda (nvdec)" : name;
    return true;
}

AVCodecID VideoDecoder::codecId() const {
    return codec_ == kCodecH264 ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC;
}

bool VideoDecoder::open(VideoCodec codec, int threads) {
    codec_ = codec;
    threads_ = threads;
    pkt_ = av_packet_alloc();
    out_ = av_frame_alloc();
    raw_ = av_frame_alloc();
    gpu_ = av_frame_alloc();
    if (!pkt_ || !out_ || !raw_ || !gpu_) return false;

    if (prefer_cuda_ && tryHwAccel("cuda")) return true;
    if (tryHwAccel("d3d11va")) return true;
    if (tryHwAccel("vaapi")) return true;

    // Software path. Frame threading adds latency, slice threading does not:
    // prefer slices, and only fall back to frame threads on a decoder that
    // supports neither.
    // Named avcodec, not codec: the parameter is the wire codec enum.
    const AVCodec* avcodec = avcodec_find_decoder(codecId());
    if (!avcodec) return false;
    ctx_ = avcodec_alloc_context3(avcodec);
    if (!ctx_) return false;
    ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx_->thread_count = threads > 0 ? threads : 0;  // 0 = auto
    ctx_->thread_type = FF_THREAD_SLICE;
    if (avcodec_open2(ctx_, avcodec, nullptr) < 0) {
        // Retry with frame threading: some builds reject the slice-only request.
        avcodec_free_context(&ctx_);
        ctx_ = avcodec_alloc_context3(avcodec);
        if (!ctx_) return false;
        ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
        ctx_->thread_count = threads > 0 ? threads : 0;
        ctx_->thread_type = FF_THREAD_FRAME;
        if (avcodec_open2(ctx_, avcodec, nullptr) < 0) {
            avcodec_free_context(&ctx_);
            return false;
        }
        stats_.hwaccel = "software(frame-threads)";
        return true;
    }
    stats_.hwaccel = "software(slice-threads)";
    return true;
}

void VideoDecoder::resetContext() {
    // After a reference-chain break the DPB holds frames nobody will ever ask
    // for; flush it so the next IDR starts clean instead of being dropped as
    // "not decodable until the next key".
    if (ctx_) avcodec_flush_buffers(ctx_);
}

namespace {
// Reads one frame out of the decoder into `out`, converting hardware surfaces
// and non-BGRA pixel formats on the way. Returns true when a frame was produced.
bool receiveFrame(AVCodecContext* ctx, AVFrame* raw, AVFrame* out, AVFrame* gpu,
                  bool cpu_copy, SwsContext** sws,
                  int* sws_w, int* sws_h, AVPixelFormat* sws_fmt, DecodeStats& stats) {
    while (avcodec_receive_frame(ctx, raw) == 0) {
        if (raw->flags & AV_FRAME_FLAG_CORRUPT) stats.corrupt_frames++;
        AVFrame* src = raw;
        AVFrame* staging = nullptr;

        if (raw->format >= 0 && av_pix_fmt_desc_get(AVPixelFormat(raw->format)) &&
            av_pix_fmt_desc_get(AVPixelFormat(raw->format))->flags & AV_PIX_FMT_FLAG_HWACCEL) {
            // Keep a reference to the hardware surface for zero-copy
            // rendering, whether or not a CPU copy is also needed.
            av_frame_unref(gpu);
            av_frame_ref(gpu, raw);
            if (!cpu_copy) {
                stats.frames_decoded++;
                stats.width = raw->width;
                stats.height = raw->height;
                return true;
            }
            staging = av_frame_alloc();
            if (!staging) return false;
            if (av_hwframe_transfer_data(staging, raw, 0) < 0) {
                av_frame_free(&staging);
                continue;  // unreadable surface; skip rather than tear down
            }
            staging->pts = raw->pts;
            staging->time_base = raw->time_base;
            src = staging;
        }

        if (src->format != AV_PIX_FMT_BGRA || src->width != *sws_w || src->height != *sws_h) {
            if (*sws) { sws_freeContext(*sws); *sws = nullptr; }
            *sws = sws_getContext(src->width, src->height, AVPixelFormat(src->format),
                                  src->width, src->height, AV_PIX_FMT_BGRA,
                                  SWS_BILINEAR, nullptr, nullptr, nullptr);
            *sws_w = src->width;
            *sws_h = src->height;
            *sws_fmt = AVPixelFormat(src->format);
        }
        if (!*sws) {
            if (staging) av_frame_free(&staging);
            continue;
        }

        // Allocate the BGRA plane ourselves with stride == width*4 and no row
        // padding. av_frame_get_buffer would pick its own alignment, and if
        // that differs from the SDL texture's pitch, SDL_UpdateTexture (which
        // copies pitch*height bytes from data[0]) shears every row. Doing the
        // allocation here makes the two agree by construction.
        //
        // av_frame_unref first is required: reusing a buffer sized for the
        // previous geometry -- exactly what a rotation does -- would overflow.
        av_frame_unref(out);
        const int stride = src->width * 4;
        const size_t bytes = size_t(stride) * size_t(src->height);
        uint8_t* pixels = static_cast<uint8_t*>(av_malloc(bytes + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!pixels) {
            if (staging) av_frame_free(&staging);
            continue;
        }
        AVBufferRef* buf = av_buffer_create(pixels, bytes, nullptr, nullptr, 0);
        if (!buf) {
            av_free(pixels);
            if (staging) av_frame_free(&staging);
            continue;
        }
        out->buf[0] = buf;
        out->data[0] = pixels;
        out->linesize[0] = stride;
        out->format = AV_PIX_FMT_BGRA;
        out->width = src->width;
        out->height = src->height;

        uint8_t* dst[4] = {pixels, nullptr, nullptr, nullptr};
        int dst_ls[4] = {stride, 0, 0, 0};
        sws_scale(*sws, src->data, src->linesize, 0, src->height, dst, dst_ls);
        out->pts = src->pts;
        out->time_base = src->time_base;

        stats.frames_decoded++;
        stats.width = src->width;
        stats.height = src->height;
        if (staging) av_frame_free(&staging);
        return true;
    }
    return false;
}
}  // namespace

int VideoDecoder::sendAccessUnit(const uint8_t* annexb, size_t size, bool reset,
                                 int64_t pts90k) {
    if (!ctx_ || !annexb || size == 0) return 0;
    if (reset) resetContext();
    ctx_->pkt_timebase = AVRational{1, 90000};

    pkt_->data = const_cast<uint8_t*>(annexb);
    pkt_->size = int(size);
    // The device's real presentation time, in the wire format's 90 kHz
    // units. Without it the decoder emits timestamp-less frames and
    // the app cannot present on the stream's own cadence.
    pkt_->pts = pts90k >= 0 ? pts90k : AV_NOPTS_VALUE;
    pkt_->dts = pkt_->pts;

    if (avcodec_send_packet(ctx_, pkt_) < 0) {
        stats_.decode_errors++;
        return 0;
    }
    return receiveFrame(ctx_, raw_, out_, gpu_, cpu_copy_, &sws_, &sws_src_w_,
                        &sws_src_h_, &sws_src_fmt_, stats_)
               ? 1
               : 0;
}

int VideoDecoder::drain() {
    if (!ctx_) return 0;
    return receiveFrame(ctx_, raw_, out_, gpu_, cpu_copy_, &sws_, &sws_src_w_,
                        &sws_src_h_, &sws_src_fmt_, stats_)
               ? 1
               : 0;
}

}  // namespace wp
