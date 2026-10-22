// WirePlay — iPhone screen + audio on your PC over USB.
//
// Media runs on one thread: drain the engine pipe, decode, present. Only audio
// runs elsewhere, because SDL's audio callback has to, and because audio is the
// clock everything else is slaved to.
//
// Latency design: USB gives us a jitter-free transport, so there is nothing to
// smooth over. We hold no frame backlog and present the newest frame on the
// next display refresh; buffering here would only add glass-to-glass delay.

#include <SDL.h>
#if defined(WIREPLAY_HAVE_VULKAN)
#include <vulkan/vulkan.h>
#include <SDL_vulkan.h>
#endif

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <cstdio>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
// Needed by attachToParentConsole() below. Each header is here for a specific
// symbol rather than by habit:
//   io.h      _get_osfhandle
//   windows.h HANDLE, GetStdHandle, SetStdHandle, STD_ERROR_HANDLE
//   wincon.h  AttachConsole, ATTACH_PARENT_PROCESS
// wincon.h is named explicitly because this target builds with
// WIN32_LEAN_AND_MEAN, and whether that still pulls in the console header is
// not something to discover through a failed build on the user's machine. The
// include is a no-op if windows.h already brought it in.
#include <io.h>
#include <winsock2.h>  // before windows.h: usbmux preflight (TCP 27015)
#include <windows.h>
#include <wincon.h>
// Zero-copy preview: the decoder (D3D11VA) and this renderer share one
// ID3D11Device, so decoded NV12 textures are sampled in place instead of
// making a GPU->CPU->GPU round trip per frame.
#include <d3d11.h>
#include <d3d11_4.h>  // ID3D11Multithread lives here in current SDKs
#include <d3d12.h>    // D3D12 presentation engine (decoder stays D3D11VA)
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>  // IDXGISwapChain3 (D3D12 present)
#include <SDL_syswm.h>
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
}

#include "wp/decoder.hpp"
#include "wp/process.hpp"
#include "wp/backend.hpp"
#include "wp/framebus.hpp"

#if defined(_WIN32) || defined(__linux__)
#define WIREPLAY_HAVE_GL 1
#include <SDL_opengl.h>
#endif
#include "wp/protocol.hpp"

namespace {

struct Options {
    std::string engine_cmd;   // command line that starts the engine
    std::string udid;
    std::string offline;      // replay a recorded frame dump instead of a device
    std::string dump_video;   // tee the Annex-B bitstream to a file
    bool use_stdin = false;   // frames come from stdin instead of a child engine
    bool no_audio = false;
    bool fullscreen = false;
    bool always_on_top = false;
    bool headless = false;
    bool no_camera = false;  // do not publish to the OBS virtual camera
    bool Vulkan = false;     // --renderer vulkan: Vulkan preview (D3D11 default)
    bool opengl = false;     // --renderer opengl: OpenGL preview ("de zueira")
    // Windows renderer choice: D3D12 is the default presentation engine
    // (decoding stays D3D11VA -- FFmpeg has no D3D12 decode -- and the
    // NV12 texture crosses over through a shared handle + shared fence,
    // zero CPU copy). --renderer d3d11 pins the proven D3D11 presenter;
    // any D3D12 failure falls back to it automatically.
    bool force_d3d11 = false;
    // How the stream loop waits for the next engine frame while idle.
    // true (--pipe-wait event, the default): sleep in SDL's event wait,
    // waking instantly for window events and within 5 ms for pipe data;
    // near-zero idle CPU. false (--pipe-wait poll): the older aggressive
    // 1 ms PeekNamedPipe spin, for anyone chasing the last millisecond
    // of input latency. Destined to become a UI setting.
    bool pipe_wait_event = true;
    // Presentation timing. true (--present pts, default): each real
    // frame is presented at its device stream timestamp (held briefly
    // in the preview thread; never duplicated). false (--present
    // arrival): presented the moment it is decoded, the behaviour of
    // the session where 60 fps first ran smooth. Kept side by side so
    // the choice is measurable in the field, and later in the UI.
    bool present_pts = true;
    // PTS presentation cushion (ms): the stream clock is anchored this
    // far in the future so encoder bursts that arrive a little late
    // still meet their slot instead of bunching on screen. 17 ms is
    // one 60 fps frame; 0 restores the previous zero-cushion anchor.
    double cushion_ms = 17.0;
    // Extra audio delay (ms) on top of the prime cushion. The video is
    // presented on the PTS scheduler's timeline while the audio used to
    // play the moment it arrived, so the sound ran ahead of the picture
    // (field: "audio passing in front"). Holding the SDL queue deeper
    // by this amount lines the two up; tunable because the right value
    // is set by ear against lip sync.
    double audio_delay_ms = 50.0;
    // Output effects (--msaa/--aniso): opt-in texture/edge filtering at
    // the output stage. 0 = off (the historical path, byte for byte).
    // --msaa N takes 2/4/8/16 (clamped to what the GPU supports);
    // --aniso N takes 1..16 and only matters with the mip chain it
    // enables. Both apply to the preview and, by default, to the
    // virtual camera's D3D11 pass; --no-camera-effects opts the camera
    // out (it publishes at native resolution, where OBS does its own
    // filtering downstream, so skipping the pass loses nothing).
    int msaa = 0;
    int aniso = 0;
    bool no_camera_effects = false;
    // Preview scaling filter (--scaling): lanczos is the default (the
    // future UI's quality mode), aniso the middle mode (anisotropic 16x
    // by default), linear the performance mode. This selects how the
    // preview resamples the native frame down to the window; the camera
    // publishes at native resolution and is not resampled by it.
    enum class Scaling { Linear, Aniso, Lanczos };
    Scaling scaling = Scaling::Lanczos;
    // Hardware decoder selection: 0 = auto (D3D11VA zero-copy;
    // detects and reports an NVIDIA adapter), 1 = d3d11va forced,
    // 2 = cuda (NVDEC; frames return via CPU -- opt-in measurement).
    int decoder_pref = 0;
    // Which screen-mirroring backend to use. kAuto picks from the iOS version:
    // CoreDevice on 17+, the QuickTime protocol below that.
    wp::BackendPreference backend = wp::BackendPreference::kAuto;
    int frames_limit = 0;
    int threads = 0;
    int max_queue_ms = 80;
    bool verbose = false;
};

void usage(const char* argv0) {
    std::fprintf(stderr,
                 "WirePlay - iPhone screen + audio over USB\n"
                 "\n"
                 "usage: %s [options]\n"
                 "  --engine CMD       command line that starts the GPL engine\n"
                 "  --udid UDID        target device (default: first on USB)\n"
                 "  --offline FILE     replay a frame dump instead of a device\n"
                 "  --stdin            read frames from stdin (engine piped in)\n"
                 "  --dump-video FILE  also write the raw Annex-B bitstream here\n"
                 "  --frames N         stop after N presented frames\n"
                 "  --no-audio         do not play device audio\n"
                 "  --fullscreen       start fullscreen (F11 toggles)\n"
                 "  --always-on-top    keep the window above others\n"
                 "  --headless         decode without opening a window\n"
                 "  --no-camera        do not publish to the WirePlay virtual camera\n"
                 "  --backend NAME     auto | coredevice | quicktime\n"
                 "  --renderer NAME    d3d12 (default, falls back to d3d11) | d3d11 |\n"
                 "                     vulkan | opengl preview renderer\n"
                 "  --pipe-wait MODE   event (default, sleeps) | poll (1 ms spin, lowest latency)\n"
                 "  --present MODE     pts (default, stream timestamps) | arrival (as decoded)\n"
                 "  --decoder MODE     auto (default) | d3d11va | cuda (NVDEC, via CPU)\n"
                 "  --cushion MS       minimum PTS cushion in ms (default 17); adapts up to 150\n"
                 "  --audio-delay MS   extra audio delay in ms (default 50) to sync sound to picture\n"
                 "  --msaa N           MSAA 2/4/8/16 at the output (default 0=off; clamped to GPU max)\n"
                 "  --aniso N          anisotropic filter 1..16 with mipmaps (default 0=off)\n"
                 "  --no-camera-effects  keep --msaa/--aniso out of the virtual camera path\n"
                 "  --scaling MODE     preview resize filter: lanczos (default, quality) |\n"
                 "                     aniso (anisotropic 16x) | linear (performance)\n"
                 "                     auto: CoreDevice on iOS 17+, QuickTime below that\n"
                 "  --threads N        software decoder threads (0 = auto)\n"
                 "  -v, --verbose      verbose logging\n"
                 "  -h, --help         this text\n"
                 "\n"
                 "keys: F11 fullscreen, K force keyframe, R restart stream, Esc quit\n",
                 argv0);
}

// ---------------------------------------------------------------------------
// Audio: AAC-ELD -> s16 interleaved -> SDL queue.
//
// The device sends 480-sample access units at 48 kHz stereo, 10 ms each. The
// queue *is* the latency, so anything past the ceiling is stale audio left over
// from a stall: it gets dropped instead of played, which is what keeps sound
// glued to picture after a hiccup.
// ---------------------------------------------------------------------------
class AudioOut {
public:
    ~AudioOut() { close(); }

    bool open() {
        SDL_AudioSpec want {};
        want.freq = 48000;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 1024;  // ~21 ms period: coarse enough that a
                              // bursty producer does not starve the DAC
                              // between AUs, fine enough to stay responsive
        want.callback = nullptr;
        dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have_, 0);
        if (dev_ == 0) {
            std::fprintf(stderr, "[wireplay] audio device failed: %s\n", SDL_GetError());
            return false;
        }
        // NOT unpaused here. Playback starts on prime (see maybeStart):
        // unpausing onto an empty queue plays silence gaps between the
        // first bursty AUs, which the ear hears as crackle.
        started_ = false;
        return true;
    }

    // QuickTime hands us samples that are already decoded: 48 kHz s16 stereo,
    // exactly what the SDL device was opened for. No codec, no resampler --
    // just the same stale-audio queue discipline as the AAC path.
    bool configurePcm(int sample_rate, int channels) {
        teardownDecoder();
        pcm_mode_ = true;
        rate_ = sample_rate;
        channels_ = channels;
        ready_ = sample_rate > 0 && channels > 0;
        return ready_;
    }

    bool configure(const uint8_t* asc, size_t asc_len, int sample_rate) {
        teardownDecoder();
        pcm_mode_ = false;
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
        if (!codec) return false;
        ctx_ = avcodec_alloc_context3(codec);
        if (!ctx_) return false;
        if (asc && asc_len > 0) {
            ctx_->extradata = static_cast<uint8_t*>(av_mallocz(asc_len + AV_INPUT_BUFFER_PADDING_SIZE));
            if (!ctx_->extradata) return false;
            std::memcpy(ctx_->extradata, asc, asc_len);
            ctx_->extradata_size = int(asc_len);
        }
        // AAC-ELD frames are 480 samples, not the 1024 the generic AAC path
        // assumes. Leaving the default in place makes each AU read as a longer
        // block and comes out as clipped garbage.
        ctx_->frame_size = 480;
        if (avcodec_open2(ctx_, codec, nullptr) < 0) {
            avcodec_free_context(&ctx_);
            return false;
        }
        frame_ = av_frame_alloc();
        pkt_ = av_packet_alloc();

        AVChannelLayout in = AV_CHANNEL_LAYOUT_STEREO;
        AVChannelLayout out = AV_CHANNEL_LAYOUT_STEREO;
        const int rc = swr_alloc_set_opts2(&swr_, &out, AV_SAMPLE_FMT_S16, sample_rate, &in,
                                           AV_SAMPLE_FMT_FLTP, sample_rate, 0, nullptr);
        av_channel_layout_uninit(&in);
        av_channel_layout_uninit(&out);
        if (rc < 0 || swr_init(swr_) < 0) return false;

        rate_ = sample_rate;
        ready_ = true;
        return true;
    }

    void pushAU(const uint8_t* au, size_t size) {
        if (!ready_ || dev_ == 0) return;
        if (pcm_mode_) {
            const int queued_ms = int(double(SDL_GetQueuedAudioSize(dev_)) / (rate_ * 4.0) * 1000.0);
            if (queued_ms > max_queue_ms_) {
                SDL_ClearQueuedAudio(dev_);
                ++dropped_;
            }
            SDL_QueueAudio(dev_, au, uint32_t(size));
            clock_samples_ += size / 4;
            maybeStart();
            return;
        }
        if (!ctx_) return;
        const int queued_ms = int(double(SDL_GetQueuedAudioSize(dev_)) / (rate_ * 4.0) * 1000.0);
        if (queued_ms > max_queue_ms_) {
            SDL_ClearQueuedAudio(dev_);
            ++dropped_;
        }
        pkt_->data = const_cast<uint8_t*>(au);
        pkt_->size = int(size);
        if (avcodec_send_packet(ctx_, pkt_) < 0) { ++errors_; return; }
        while (avcodec_receive_frame(ctx_, frame_) == 0) {
            uint8_t* pcm = nullptr;
            const int n = swr_convert(swr_, &pcm, frame_->nb_samples,
                                      const_cast<const uint8_t**>(frame_->extended_data),
                                      frame_->nb_samples);
            if (n > 0 && pcm) {
                SDL_QueueAudio(dev_, pcm, n * 4);  // stereo s16
                clock_samples_ += uint64_t(n);
                maybeStart();
            }
            av_frame_unref(frame_);
        }
    }

    void close() {
        ready_ = false;
        started_ = false;
        teardownDecoder();
        if (dev_) { SDL_CloseAudioDevice(dev_); dev_ = 0; }
    }

    // Milliseconds of audio played since open(). This is the master clock: the
    // DAC cannot be paused or hurried, so it is the only timeline that is
    // guaranteed monotonic and honest.
    double clockMs() const {
        if (dev_ == 0) return 0.0;
        const int64_t played = int64_t(clock_samples_) - int64_t(SDL_GetQueuedAudioSize(dev_) / 4);
        return played <= 0 ? 0.0 : double(played) * 1000.0 / double(rate_);
    }

    void setMaxQueue(int ms) { max_queue_ms_ = ms; }

    // The prime cushion IS the audio delay line: playback starts only
    // once (43 + delay) ms are queued, and a producer/consumer pair
    // both running at real-time keeps the queue hovering there. The
    // clear cap rises with the delay so burst arrivals behind a big
    // IDR are absorbed instead of being mistaken for runaway latency.
    void setDelayMs(int ms) {
        delay_ms_ = ms < 0 ? 0 : ms;
        // The clear cap must stay above the prime this delay installs
        // (43 + delay ms): a cap below the prime clears the queue
        // before playback can ever start -- field-proven, cap 80 ms
        // against a 93 ms prime kept the DAC silent forever.
        const int floor_ms = 43 + delay_ms_ + 120;
        if (max_queue_ms_ < floor_ms) max_queue_ms_ = floor_ms;
    }
    int delayMs() const { return delay_ms_; }

    // Prime before playing: hold the DAC paused until ~43 ms of audio
    // is queued, then start. The stream arrives in pipe bursts; that
    // cushion absorbs the burst gaps instead of airing them as clicks.
    // Steady state hovers near the cushion, well under the clear cap.
    void maybeStart() {
        if (started_ || dev_ == 0) return;
        const uint32_t prime = primeBytes();
        const uint32_t queued = SDL_GetQueuedAudioSize(dev_);
        if (queued >= prime) {
            std::fprintf(stderr, "[wireplay] audio playback starting (queued %u >= prime %u)\n",
                         queued, prime);
            SDL_PauseAudioDevice(dev_, 0);
            started_ = true;
        }
    }
    uint64_t dropped() const { return dropped_; }
    bool started() const { return started_; }
    int queuedMs() const {
        if (dev_ == 0 || rate_ <= 0) return 0;
        return int(double(SDL_GetQueuedAudioSize(dev_)) / (rate_ * 4.0) * 1000.0);
    }
    uint32_t prime() const { return primeBytes(); }
    uint64_t errors() const { return errors_; }
    bool active() const { return ready_ && dev_ != 0; }

private:
    void teardownDecoder() {
        if (swr_) swr_free(&swr_);
        if (frame_) av_frame_free(&frame_);
        if (pkt_) av_packet_free(&pkt_);
        if (ctx_) avcodec_free_context(&ctx_);
    }

    SDL_AudioDeviceID dev_ = 0;
    bool started_ = false;  // DAC unpaused (after priming)
    SDL_AudioSpec have_ {};
    AVCodecContext* ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* pkt_ = nullptr;
    SwrContext* swr_ = nullptr;
    uint64_t clock_samples_ = 0;
    uint64_t dropped_ = 0;
    uint64_t errors_ = 0;
    int rate_ = 48000;
    int max_queue_ms_ = 160;
    int delay_ms_ = 0;
    // ~43 ms of audio (2048 stereo frames at 48 kHz) plus the sync delay.
    uint32_t primeBytes() const {
        const int rate = rate_ > 0 ? rate_ : 48000;
        return uint32_t((43 + delay_ms_) * rate * 4 / 1000);
    }
    bool ready_ = false;
    bool pcm_mode_ = false;
    int channels_ = 2;
};

#if defined(WIREPLAY_HAVE_VULKAN)
namespace {
// SPIR-V for the Vulkan Lanczos-3 preview pass (GLSL compiled ahead of
// time; the app ships no shader compiler). Same separable kernel as the
// D3D11/GL paths: push constants src.xy = source size, src.zw = 1/size,
// ker.x = downscale factor, ker.y = axis (0=H, 1=V).
const uint32_t kVkLzVert[] = {
    0x07230203u, 0x00010000u, 0x0008000bu, 0x0000002fu, 0x00000000u, 0x00020011u, 0x00000001u, 0x0006000bu,
    0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u,
    0x0008000fu, 0x00000000u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x0000000bu, 0x00000017u, 0x00000021u,
    0x00030003u, 0x00000002u, 0x000001c2u, 0x00040005u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x00030005u,
    0x00000008u, 0x00000078u, 0x00060005u, 0x0000000bu, 0x565f6c67u, 0x65747265u, 0x646e4978u, 0x00007865u,
    0x00030005u, 0x00000010u, 0x00000079u, 0x00030005u, 0x00000017u, 0x00765576u, 0x00060005u, 0x0000001fu,
    0x505f6c67u, 0x65567265u, 0x78657472u, 0x00000000u, 0x00060006u, 0x0000001fu, 0x00000000u, 0x505f6c67u,
    0x7469736fu, 0x006e6f69u, 0x00070006u, 0x0000001fu, 0x00000001u, 0x505f6c67u, 0x746e696fu, 0x657a6953u,
    0x00000000u, 0x00070006u, 0x0000001fu, 0x00000002u, 0x435f6c67u, 0x4470696cu, 0x61747369u, 0x0065636eu,
    0x00070006u, 0x0000001fu, 0x00000003u, 0x435f6c67u, 0x446c6c75u, 0x61747369u, 0x0065636eu, 0x00030005u,
    0x00000021u, 0x00000000u, 0x00040047u, 0x0000000bu, 0x0000000bu, 0x0000002au, 0x00040047u, 0x00000017u,
    0x0000001eu, 0x00000000u, 0x00030047u, 0x0000001fu, 0x00000002u, 0x00050048u, 0x0000001fu, 0x00000000u,
    0x0000000bu, 0x00000000u, 0x00050048u, 0x0000001fu, 0x00000001u, 0x0000000bu, 0x00000001u, 0x00050048u,
    0x0000001fu, 0x00000002u, 0x0000000bu, 0x00000003u, 0x00050048u, 0x0000001fu, 0x00000003u, 0x0000000bu,
    0x00000004u, 0x00020013u, 0x00000002u, 0x00030021u, 0x00000003u, 0x00000002u, 0x00030016u, 0x00000006u,
    0x00000020u, 0x00040020u, 0x00000007u, 0x00000007u, 0x00000006u, 0x00040015u, 0x00000009u, 0x00000020u,
    0x00000001u, 0x00040020u, 0x0000000au, 0x00000001u, 0x00000009u, 0x0004003bu, 0x0000000au, 0x0000000bu,
    0x00000001u, 0x0004002bu, 0x00000009u, 0x0000000du, 0x00000001u, 0x00040017u, 0x00000015u, 0x00000006u,
    0x00000002u, 0x00040020u, 0x00000016u, 0x00000003u, 0x00000015u, 0x0004003bu, 0x00000016u, 0x00000017u,
    0x00000003u, 0x00040017u, 0x0000001bu, 0x00000006u, 0x00000004u, 0x00040015u, 0x0000001cu, 0x00000020u,
    0x00000000u, 0x0004002bu, 0x0000001cu, 0x0000001du, 0x00000001u, 0x0004001cu, 0x0000001eu, 0x00000006u,
    0x0000001du, 0x0006001eu, 0x0000001fu, 0x0000001bu, 0x00000006u, 0x0000001eu, 0x0000001eu, 0x00040020u,
    0x00000020u, 0x00000003u, 0x0000001fu, 0x0004003bu, 0x00000020u, 0x00000021u, 0x00000003u, 0x0004002bu,
    0x00000009u, 0x00000022u, 0x00000000u, 0x0004002bu, 0x00000006u, 0x00000024u, 0x40000000u, 0x0004002bu,
    0x00000006u, 0x00000026u, 0x3f800000u, 0x0004002bu, 0x00000006u, 0x0000002bu, 0x00000000u, 0x00040020u,
    0x0000002du, 0x00000003u, 0x0000001bu, 0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u,
    0x000200f8u, 0x00000005u, 0x0004003bu, 0x00000007u, 0x00000008u, 0x00000007u, 0x0004003bu, 0x00000007u,
    0x00000010u, 0x00000007u, 0x0004003du, 0x00000009u, 0x0000000cu, 0x0000000bu, 0x000500c7u, 0x00000009u,
    0x0000000eu, 0x0000000cu, 0x0000000du, 0x0004006fu, 0x00000006u, 0x0000000fu, 0x0000000eu, 0x0003003eu,
    0x00000008u, 0x0000000fu, 0x0004003du, 0x00000009u, 0x00000011u, 0x0000000bu, 0x000500c3u, 0x00000009u,
    0x00000012u, 0x00000011u, 0x0000000du, 0x000500c7u, 0x00000009u, 0x00000013u, 0x00000012u, 0x0000000du,
    0x0004006fu, 0x00000006u, 0x00000014u, 0x00000013u, 0x0003003eu, 0x00000010u, 0x00000014u, 0x0004003du,
    0x00000006u, 0x00000018u, 0x00000008u, 0x0004003du, 0x00000006u, 0x00000019u, 0x00000010u, 0x00050050u,
    0x00000015u, 0x0000001au, 0x00000018u, 0x00000019u, 0x0003003eu, 0x00000017u, 0x0000001au, 0x0004003du,
    0x00000006u, 0x00000023u, 0x00000008u, 0x00050085u, 0x00000006u, 0x00000025u, 0x00000023u, 0x00000024u,
    0x00050083u, 0x00000006u, 0x00000027u, 0x00000025u, 0x00000026u, 0x0004003du, 0x00000006u, 0x00000028u,
    0x00000010u, 0x00050085u, 0x00000006u, 0x00000029u, 0x00000028u, 0x00000024u, 0x00050083u, 0x00000006u,
    0x0000002au, 0x00000029u, 0x00000026u, 0x00070050u, 0x0000001bu, 0x0000002cu, 0x00000027u, 0x0000002au,
    0x0000002bu, 0x00000026u, 0x00050041u, 0x0000002du, 0x0000002eu, 0x00000021u, 0x00000022u, 0x0003003eu,
    0x0000002eu, 0x0000002cu, 0x000100fdu, 0x00010038u,
};
const uint32_t kVkLzFrag[] = {
    0x07230203u, 0x00010000u, 0x0008000bu, 0x000000feu, 0x00000000u, 0x00020011u, 0x00000001u, 0x0006000bu,
    0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u,
    0x0007000fu, 0x00000004u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x00000032u, 0x000000dcu, 0x00030010u,
    0x00000004u, 0x00000007u, 0x00030003u, 0x00000002u, 0x000001c2u, 0x00040005u, 0x00000004u, 0x6e69616du,
    0x00000000u, 0x00050005u, 0x0000000au, 0x636e616cu, 0x28736f7au, 0x003b3166u, 0x00030005u, 0x00000009u,
    0x00000078u, 0x00030005u, 0x0000001du, 0x00007870u, 0x00040005u, 0x00000030u, 0x65786574u, 0x0000006cu,
    0x00030005u, 0x00000032u, 0x00765576u, 0x00030005u, 0x00000035u, 0x00004350u, 0x00040006u, 0x00000035u,
    0x00000000u, 0x00637273u, 0x00040006u, 0x00000035u, 0x00000001u, 0x0072656bu, 0x00030005u, 0x00000037u,
    0x00006370u, 0x00040005u, 0x00000043u, 0x69726f68u, 0x0000007au, 0x00040005u, 0x0000004bu, 0x726f6f63u,
    0x00000064u, 0x00030005u, 0x00000057u, 0x00000066u, 0x00040005u, 0x0000005bu, 0x70707573u, 0x0074726fu,
    0x00030005u, 0x0000005eu, 0x00003074u, 0x00030005u, 0x00000063u, 0x00003174u, 0x00040005u, 0x00000069u,
    0x696d696cu, 0x00000074u, 0x00040005u, 0x00000076u, 0x6568746fu, 0x00000072u, 0x00030005u, 0x00000092u,
    0x00636361u, 0x00030005u, 0x00000094u, 0x00007377u, 0x00030005u, 0x00000095u, 0x00000069u, 0x00030005u,
    0x0000009eu, 0x00000074u, 0x00030005u, 0x000000a9u, 0x00000077u, 0x00040005u, 0x000000afu, 0x61726170u,
    0x0000006du, 0x00030005u, 0x000000b6u, 0x00006974u, 0x00030005u, 0x000000bdu, 0x00000070u, 0x00030005u,
    0x000000cdu, 0x00786574u, 0x00050005u, 0x000000dcu, 0x4374756fu, 0x726f6c6fu, 0x00000000u, 0x00040047u,
    0x00000032u, 0x0000001eu, 0x00000000u, 0x00030047u, 0x00000035u, 0x00000002u, 0x00050048u, 0x00000035u,
    0x00000000u, 0x00000023u, 0x00000000u, 0x00050048u, 0x00000035u, 0x00000001u, 0x00000023u, 0x00000010u,
    0x00040047u, 0x000000cdu, 0x00000021u, 0x00000000u, 0x00040047u, 0x000000cdu, 0x00000022u, 0x00000000u,
    0x00040047u, 0x000000dcu, 0x0000001eu, 0x00000000u, 0x00020013u, 0x00000002u, 0x00030021u, 0x00000003u,
    0x00000002u, 0x00030016u, 0x00000006u, 0x00000020u, 0x00040020u, 0x00000007u, 0x00000007u, 0x00000006u,
    0x00040021u, 0x00000008u, 0x00000006u, 0x00000007u, 0x0004002bu, 0x00000006u, 0x0000000fu, 0x3727c5acu,
    0x00020014u, 0x00000010u, 0x0004002bu, 0x00000006u, 0x00000014u, 0x3f800000u, 0x0004002bu, 0x00000006u,
    0x00000017u, 0x40400000u, 0x0004002bu, 0x00000006u, 0x0000001bu, 0x00000000u, 0x0004002bu, 0x00000006u,
    0x0000001eu, 0x40490fdbu, 0x00040017u, 0x0000002eu, 0x00000006u, 0x00000002u, 0x00040020u, 0x0000002fu,
    0x00000007u, 0x0000002eu, 0x00040020u, 0x00000031u, 0x00000001u, 0x0000002eu, 0x0004003bu, 0x00000031u,
    0x00000032u, 0x00000001u, 0x00040017u, 0x00000034u, 0x00000006u, 0x00000004u, 0x0004001eu, 0x00000035u,
    0x00000034u, 0x00000034u, 0x00040020u, 0x00000036u, 0x00000009u, 0x00000035u, 0x0004003bu, 0x00000036u,
    0x00000037u, 0x00000009u, 0x00040015u, 0x00000038u, 0x00000020u, 0x00000001u, 0x0004002bu, 0x00000038u,
    0x00000039u, 0x00000000u, 0x00040020u, 0x0000003au, 0x00000009u, 0x00000034u, 0x0004002bu, 0x00000006u,
    0x0000003fu, 0x3f000000u, 0x00040020u, 0x00000042u, 0x00000007u, 0x00000010u, 0x0004002bu, 0x00000038u,
    0x00000044u, 0x00000001u, 0x00040015u, 0x00000045u, 0x00000020u, 0x00000000u, 0x0004002bu, 0x00000045u,
    0x00000046u, 0x00000001u, 0x00040020u, 0x00000047u, 0x00000009u, 0x00000006u, 0x0004002bu, 0x00000045u,
    0x00000050u, 0x00000000u, 0x00040020u, 0x00000068u, 0x00000007u, 0x00000038u, 0x00040020u, 0x00000091u,
    0x00000007u, 0x00000034u, 0x0007002cu, 0x00000034u, 0x00000093u, 0x0000001bu, 0x0000001bu, 0x0000001bu,
    0x0000001bu, 0x0004002bu, 0x00000038u, 0x0000009cu, 0x00000028u, 0x00040017u, 0x000000bbu, 0x00000038u,
    0x00000002u, 0x00040020u, 0x000000bcu, 0x00000007u, 0x000000bbu, 0x00090019u, 0x000000cau, 0x00000006u,
    0x00000001u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00000000u, 0x0003001bu, 0x000000cbu,
    0x000000cau, 0x00040020u, 0x000000ccu, 0x00000000u, 0x000000cbu, 0x0004003bu, 0x000000ccu, 0x000000cdu,
    0x00000000u, 0x00040020u, 0x000000dbu, 0x00000003u, 0x00000034u, 0x0004003bu, 0x000000dbu, 0x000000dcu,
    0x00000003u, 0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u, 0x000200f8u, 0x00000005u,
    0x0004003bu, 0x0000002fu, 0x00000030u, 0x00000007u, 0x0004003bu, 0x00000042u, 0x00000043u, 0x00000007u,
    0x0004003bu, 0x00000007u, 0x0000004bu, 0x00000007u, 0x0004003bu, 0x00000007u, 0x0000004du, 0x00000007u,
    0x0004003bu, 0x00000007u, 0x00000057u, 0x00000007u, 0x0004003bu, 0x00000007u, 0x0000005bu, 0x00000007u,
    0x0004003bu, 0x00000007u, 0x0000005eu, 0x00000007u, 0x0004003bu, 0x00000007u, 0x00000063u, 0x00000007u,
    0x0004003bu, 0x00000068u, 0x00000069u, 0x00000007u, 0x0004003bu, 0x00000007u, 0x0000006bu, 0x00000007u,
    0x0004003bu, 0x00000068u, 0x00000076u, 0x00000007u, 0x0004003bu, 0x00000007u, 0x00000078u, 0x00000007u,
    0x0004003bu, 0x00000007u, 0x00000085u, 0x00000007u, 0x0004003bu, 0x00000091u, 0x00000092u, 0x00000007u,
    0x0004003bu, 0x00000007u, 0x00000094u, 0x00000007u, 0x0004003bu, 0x00000068u, 0x00000095u, 0x00000007u,
    0x0004003bu, 0x00000007u, 0x0000009eu, 0x00000007u, 0x0004003bu, 0x00000007u, 0x000000a9u, 0x00000007u,
    0x0004003bu, 0x00000007u, 0x000000afu, 0x00000007u, 0x0004003bu, 0x00000068u, 0x000000b6u, 0x00000007u,
    0x0004003bu, 0x000000bcu, 0x000000bdu, 0x00000007u, 0x0004003bu, 0x000000bcu, 0x000000bfu, 0x00000007u,
    0x0004003bu, 0x00000091u, 0x000000dfu, 0x00000007u, 0x0004003du, 0x0000002eu, 0x00000033u, 0x00000032u,
    0x00050041u, 0x0000003au, 0x0000003bu, 0x00000037u, 0x00000039u, 0x0004003du, 0x00000034u, 0x0000003cu,
    0x0000003bu, 0x0007004fu, 0x0000002eu, 0x0000003du, 0x0000003cu, 0x0000003cu, 0x00000000u, 0x00000001u,
    0x00050085u, 0x0000002eu, 0x0000003eu, 0x00000033u, 0x0000003du, 0x00050050u, 0x0000002eu, 0x00000040u,
    0x0000003fu, 0x0000003fu, 0x00050083u, 0x0000002eu, 0x00000041u, 0x0000003eu, 0x00000040u, 0x0003003eu,
    0x00000030u, 0x00000041u, 0x00060041u, 0x00000047u, 0x00000048u, 0x00000037u, 0x00000044u, 0x00000046u,
    0x0004003du, 0x00000006u, 0x00000049u, 0x00000048u, 0x000500b8u, 0x00000010u, 0x0000004au, 0x00000049u,
    0x0000003fu, 0x0003003eu, 0x00000043u, 0x0000004au, 0x0004003du, 0x00000010u, 0x0000004cu, 0x00000043u,
    0x000300f7u, 0x0000004fu, 0x00000000u, 0x000400fau, 0x0000004cu, 0x0000004eu, 0x00000053u, 0x000200f8u,
    0x0000004eu, 0x00050041u, 0x00000007u, 0x00000051u, 0x00000030u, 0x00000050u, 0x0004003du, 0x00000006u,
    0x00000052u, 0x00000051u, 0x0003003eu, 0x0000004du, 0x00000052u, 0x000200f9u, 0x0000004fu, 0x000200f8u,
    0x00000053u, 0x00050041u, 0x00000007u, 0x00000054u, 0x00000030u, 0x00000046u, 0x0004003du, 0x00000006u,
    0x00000055u, 0x00000054u, 0x0003003eu, 0x0000004du, 0x00000055u, 0x000200f9u, 0x0000004fu, 0x000200f8u,
    0x0000004fu, 0x0004003du, 0x00000006u, 0x00000056u, 0x0000004du, 0x0003003eu, 0x0000004bu, 0x00000056u,
    0x00060041u, 0x00000047u, 0x00000058u, 0x00000037u, 0x00000044u, 0x00000050u, 0x0004003du, 0x00000006u,
    0x00000059u, 0x00000058u, 0x0007000cu, 0x00000006u, 0x0000005au, 0x00000001u, 0x00000028u, 0x00000059u,
    0x00000014u, 0x0003003eu, 0x00000057u, 0x0000005au, 0x0004003du, 0x00000006u, 0x0000005cu, 0x00000057u,
    0x00050085u, 0x00000006u, 0x0000005du, 0x00000017u, 0x0000005cu, 0x0003003eu, 0x0000005bu, 0x0000005du,
    0x0004003du, 0x00000006u, 0x0000005fu, 0x0000004bu, 0x0004003du, 0x00000006u, 0x00000060u, 0x0000005bu,
    0x00050083u, 0x00000006u, 0x00000061u, 0x0000005fu, 0x00000060u, 0x0006000cu, 0x00000006u, 0x00000062u,
    0x00000001u, 0x00000009u, 0x00000061u, 0x0003003eu, 0x0000005eu, 0x00000062u, 0x0004003du, 0x00000006u,
    0x00000064u, 0x0000004bu, 0x0004003du, 0x00000006u, 0x00000065u, 0x0000005bu, 0x00050081u, 0x00000006u,
    0x00000066u, 0x00000064u, 0x00000065u, 0x0006000cu, 0x00000006u, 0x00000067u, 0x00000001u, 0x00000008u,
    0x00000066u, 0x0003003eu, 0x00000063u, 0x00000067u, 0x0004003du, 0x00000010u, 0x0000006au, 0x00000043u,
    0x000300f7u, 0x0000006du, 0x00000000u, 0x000400fau, 0x0000006au, 0x0000006cu, 0x00000070u, 0x000200f8u,
    0x0000006cu, 0x00060041u, 0x00000047u, 0x0000006eu, 0x00000037u, 0x00000039u, 0x00000050u, 0x0004003du,
    0x00000006u, 0x0000006fu, 0x0000006eu, 0x0003003eu, 0x0000006bu, 0x0000006fu, 0x000200f9u, 0x0000006du,
    0x000200f8u, 0x00000070u, 0x00060041u, 0x00000047u, 0x00000071u, 0x00000037u, 0x00000039u, 0x00000046u,
    0x0004003du, 0x00000006u, 0x00000072u, 0x00000071u, 0x0003003eu, 0x0000006bu, 0x00000072u, 0x000200f9u,
    0x0000006du, 0x000200f8u, 0x0000006du, 0x0004003du, 0x00000006u, 0x00000073u, 0x0000006bu, 0x0004006eu,
    0x00000038u, 0x00000074u, 0x00000073u, 0x00050082u, 0x00000038u, 0x00000075u, 0x00000074u, 0x00000044u,
    0x0003003eu, 0x00000069u, 0x00000075u, 0x0004003du, 0x00000010u, 0x00000077u, 0x00000043u, 0x000300f7u,
    0x0000007au, 0x00000000u, 0x000400fau, 0x00000077u, 0x00000079u, 0x0000007du, 0x000200f8u, 0x00000079u,
    0x00050041u, 0x00000007u, 0x0000007bu, 0x00000030u, 0x00000046u, 0x0004003du, 0x00000006u, 0x0000007cu,
    0x0000007bu, 0x0003003eu, 0x00000078u, 0x0000007cu, 0x000200f9u, 0x0000007au, 0x000200f8u, 0x0000007du,
    0x00050041u, 0x00000007u, 0x0000007eu, 0x00000030u, 0x00000050u, 0x0004003du, 0x00000006u, 0x0000007fu,
    0x0000007eu, 0x0003003eu, 0x00000078u, 0x0000007fu, 0x000200f9u, 0x0000007au, 0x000200f8u, 0x0000007au,
    0x0004003du, 0x00000006u, 0x00000080u, 0x00000078u, 0x00050081u, 0x00000006u, 0x00000081u, 0x00000080u,
    0x0000003fu, 0x0004006eu, 0x00000038u, 0x00000082u, 0x00000081u, 0x0003003eu, 0x00000076u, 0x00000082u,
    0x0004003du, 0x00000038u, 0x00000083u, 0x00000076u, 0x0004003du, 0x00000010u, 0x00000084u, 0x00000043u,
    0x000300f7u, 0x00000087u, 0x00000000u, 0x000400fau, 0x00000084u, 0x00000086u, 0x0000008au, 0x000200f8u,
    0x00000086u, 0x00060041u, 0x00000047u, 0x00000088u, 0x00000037u, 0x00000039u, 0x00000046u, 0x0004003du,
    0x00000006u, 0x00000089u, 0x00000088u, 0x0003003eu, 0x00000085u, 0x00000089u, 0x000200f9u, 0x00000087u,
    0x000200f8u, 0x0000008au, 0x00060041u, 0x00000047u, 0x0000008bu, 0x00000037u, 0x00000039u, 0x00000050u,
    0x0004003du, 0x00000006u, 0x0000008cu, 0x0000008bu, 0x0003003eu, 0x00000085u, 0x0000008cu, 0x000200f9u,
    0x00000087u, 0x000200f8u, 0x00000087u, 0x0004003du, 0x00000006u, 0x0000008du, 0x00000085u, 0x0004006eu,
    0x00000038u, 0x0000008eu, 0x0000008du, 0x00050082u, 0x00000038u, 0x0000008fu, 0x0000008eu, 0x00000044u,
    0x0008000cu, 0x00000038u, 0x00000090u, 0x00000001u, 0x0000002du, 0x00000083u, 0x00000039u, 0x0000008fu,
    0x0003003eu, 0x00000076u, 0x00000090u, 0x0003003eu, 0x00000092u, 0x00000093u, 0x0003003eu, 0x00000094u,
    0x0000001bu, 0x0003003eu, 0x00000095u, 0x00000039u, 0x000200f9u, 0x00000096u, 0x000200f8u, 0x00000096u,
    0x000400f6u, 0x00000098u, 0x00000099u, 0x00000000u, 0x000200f9u, 0x0000009au, 0x000200f8u, 0x0000009au,
    0x0004003du, 0x00000038u, 0x0000009bu, 0x00000095u, 0x000500b1u, 0x00000010u, 0x0000009du, 0x0000009bu,
    0x0000009cu, 0x000400fau, 0x0000009du, 0x00000097u, 0x00000098u, 0x000200f8u, 0x00000097u, 0x0004003du,
    0x00000006u, 0x0000009fu, 0x0000005eu, 0x0004003du, 0x00000038u, 0x000000a0u, 0x00000095u, 0x0004006fu,
    0x00000006u, 0x000000a1u, 0x000000a0u, 0x00050081u, 0x00000006u, 0x000000a2u, 0x0000009fu, 0x000000a1u,
    0x0003003eu, 0x0000009eu, 0x000000a2u, 0x0004003du, 0x00000006u, 0x000000a3u, 0x0000009eu, 0x0004003du,
    0x00000006u, 0x000000a4u, 0x00000063u, 0x000500bau, 0x00000010u, 0x000000a5u, 0x000000a3u, 0x000000a4u,
    0x000300f7u, 0x000000a7u, 0x00000000u, 0x000400fau, 0x000000a5u, 0x000000a6u, 0x000000a7u, 0x000200f8u,
    0x000000a6u, 0x000200f9u, 0x00000098u, 0x000200f8u, 0x000000a7u, 0x0004003du, 0x00000006u, 0x000000aau,
    0x0000004bu, 0x0004003du, 0x00000006u, 0x000000abu, 0x0000009eu, 0x00050083u, 0x00000006u, 0x000000acu,
    0x000000aau, 0x000000abu, 0x0004003du, 0x00000006u, 0x000000adu, 0x00000057u, 0x00050088u, 0x00000006u,
    0x000000aeu, 0x000000acu, 0x000000adu, 0x0003003eu, 0x000000afu, 0x000000aeu, 0x00050039u, 0x00000006u,
    0x000000b0u, 0x0000000au, 0x000000afu, 0x0003003eu, 0x000000a9u, 0x000000b0u, 0x0004003du, 0x00000006u,
    0x000000b1u, 0x000000a9u, 0x000500b4u, 0x00000010u, 0x000000b2u, 0x000000b1u, 0x0000001bu, 0x000300f7u,
    0x000000b4u, 0x00000000u, 0x000400fau, 0x000000b2u, 0x000000b3u, 0x000000b4u, 0x000200f8u, 0x000000b3u,
    0x000200f9u, 0x00000099u, 0x000200f8u, 0x000000b4u, 0x0004003du, 0x00000006u, 0x000000b7u, 0x0000009eu,
    0x0004006eu, 0x00000038u, 0x000000b8u, 0x000000b7u, 0x0004003du, 0x00000038u, 0x000000b9u, 0x00000069u,
    0x0008000cu, 0x00000038u, 0x000000bau, 0x00000001u, 0x0000002du, 0x000000b8u, 0x00000039u, 0x000000b9u,
    0x0003003eu, 0x000000b6u, 0x000000bau, 0x0004003du, 0x00000010u, 0x000000beu, 0x00000043u, 0x000300f7u,
    0x000000c1u, 0x00000000u, 0x000400fau, 0x000000beu, 0x000000c0u, 0x000000c5u, 0x000200f8u, 0x000000c0u,
    0x0004003du, 0x00000038u, 0x000000c2u, 0x000000b6u, 0x0004003du, 0x00000038u, 0x000000c3u, 0x00000076u,
    0x00050050u, 0x000000bbu, 0x000000c4u, 0x000000c2u, 0x000000c3u, 0x0003003eu, 0x000000bfu, 0x000000c4u,
    0x000200f9u, 0x000000c1u, 0x000200f8u, 0x000000c5u, 0x0004003du, 0x00000038u, 0x000000c6u, 0x00000076u,
    0x0004003du, 0x00000038u, 0x000000c7u, 0x000000b6u, 0x00050050u, 0x000000bbu, 0x000000c8u, 0x000000c6u,
    0x000000c7u, 0x0003003eu, 0x000000bfu, 0x000000c8u, 0x000200f9u, 0x000000c1u, 0x000200f8u, 0x000000c1u,
    0x0004003du, 0x000000bbu, 0x000000c9u, 0x000000bfu, 0x0003003eu, 0x000000bdu, 0x000000c9u, 0x0004003du,
    0x000000cbu, 0x000000ceu, 0x000000cdu, 0x0004003du, 0x000000bbu, 0x000000cfu, 0x000000bdu, 0x00040064u,
    0x000000cau, 0x000000d0u, 0x000000ceu, 0x0007005fu, 0x00000034u, 0x000000d1u, 0x000000d0u, 0x000000cfu,
    0x00000002u, 0x00000039u, 0x0004003du, 0x00000006u, 0x000000d2u, 0x000000a9u, 0x0005008eu, 0x00000034u,
    0x000000d3u, 0x000000d1u, 0x000000d2u, 0x0004003du, 0x00000034u, 0x000000d4u, 0x00000092u, 0x00050081u,
    0x00000034u, 0x000000d5u, 0x000000d4u, 0x000000d3u, 0x0003003eu, 0x00000092u, 0x000000d5u, 0x0004003du,
    0x00000006u, 0x000000d6u, 0x000000a9u, 0x0004003du, 0x00000006u, 0x000000d7u, 0x00000094u, 0x00050081u,
    0x00000006u, 0x000000d8u, 0x000000d7u, 0x000000d6u, 0x0003003eu, 0x00000094u, 0x000000d8u, 0x000200f9u,
    0x00000099u, 0x000200f8u, 0x00000099u, 0x0004003du, 0x00000038u, 0x000000d9u, 0x00000095u, 0x00050080u,
    0x00000038u, 0x000000dau, 0x000000d9u, 0x00000044u, 0x0003003eu, 0x00000095u, 0x000000dau, 0x000200f9u,
    0x00000096u, 0x000200f8u, 0x00000098u, 0x0004003du, 0x00000006u, 0x000000ddu, 0x00000094u, 0x000500bau,
    0x00000010u, 0x000000deu, 0x000000ddu, 0x0000001bu, 0x000300f7u, 0x000000e1u, 0x00000000u, 0x000400fau,
    0x000000deu, 0x000000e0u, 0x000000e6u, 0x000200f8u, 0x000000e0u, 0x0004003du, 0x00000034u, 0x000000e2u,
    0x00000092u, 0x0004003du, 0x00000006u, 0x000000e3u, 0x00000094u, 0x00070050u, 0x00000034u, 0x000000e4u,
    0x000000e3u, 0x000000e3u, 0x000000e3u, 0x000000e3u, 0x00050088u, 0x00000034u, 0x000000e5u, 0x000000e2u,
    0x000000e4u, 0x0003003eu, 0x000000dfu, 0x000000e5u, 0x000200f9u, 0x000000e1u, 0x000200f8u, 0x000000e6u,
    0x0004003du, 0x000000cbu, 0x000000e7u, 0x000000cdu, 0x00050041u, 0x00000007u, 0x000000e8u, 0x00000030u,
    0x00000050u, 0x0004003du, 0x00000006u, 0x000000e9u, 0x000000e8u, 0x00050081u, 0x00000006u, 0x000000eau,
    0x000000e9u, 0x0000003fu, 0x0004006eu, 0x00000038u, 0x000000ebu, 0x000000eau, 0x00060041u, 0x00000047u,
    0x000000ecu, 0x00000037u, 0x00000039u, 0x00000050u, 0x0004003du, 0x00000006u, 0x000000edu, 0x000000ecu,
    0x0004006eu, 0x00000038u, 0x000000eeu, 0x000000edu, 0x00050082u, 0x00000038u, 0x000000efu, 0x000000eeu,
    0x00000044u, 0x0008000cu, 0x00000038u, 0x000000f0u, 0x00000001u, 0x0000002du, 0x000000ebu, 0x00000039u,
    0x000000efu, 0x00050041u, 0x00000007u, 0x000000f1u, 0x00000030u, 0x00000046u, 0x0004003du, 0x00000006u,
    0x000000f2u, 0x000000f1u, 0x00050081u, 0x00000006u, 0x000000f3u, 0x000000f2u, 0x0000003fu, 0x0004006eu,
    0x00000038u, 0x000000f4u, 0x000000f3u, 0x00060041u, 0x00000047u, 0x000000f5u, 0x00000037u, 0x00000039u,
    0x00000046u, 0x0004003du, 0x00000006u, 0x000000f6u, 0x000000f5u, 0x0004006eu, 0x00000038u, 0x000000f7u,
    0x000000f6u, 0x00050082u, 0x00000038u, 0x000000f8u, 0x000000f7u, 0x00000044u, 0x0008000cu, 0x00000038u,
    0x000000f9u, 0x00000001u, 0x0000002du, 0x000000f4u, 0x00000039u, 0x000000f8u, 0x00050050u, 0x000000bbu,
    0x000000fau, 0x000000f0u, 0x000000f9u, 0x00040064u, 0x000000cau, 0x000000fbu, 0x000000e7u, 0x0007005fu,
    0x00000034u, 0x000000fcu, 0x000000fbu, 0x000000fau, 0x00000002u, 0x00000039u, 0x0003003eu, 0x000000dfu,
    0x000000fcu, 0x000200f9u, 0x000000e1u, 0x000200f8u, 0x000000e1u, 0x0004003du, 0x00000034u, 0x000000fdu,
    0x000000dfu, 0x0003003eu, 0x000000dcu, 0x000000fdu, 0x000100fdu, 0x00010038u, 0x00050036u, 0x00000006u,
    0x0000000au, 0x00000000u, 0x00000008u, 0x00030037u, 0x00000007u, 0x00000009u, 0x000200f8u, 0x0000000bu,
    0x0004003bu, 0x00000007u, 0x0000001du, 0x00000007u, 0x0004003du, 0x00000006u, 0x0000000cu, 0x00000009u,
    0x0006000cu, 0x00000006u, 0x0000000du, 0x00000001u, 0x00000004u, 0x0000000cu, 0x0003003eu, 0x00000009u,
    0x0000000du, 0x0004003du, 0x00000006u, 0x0000000eu, 0x00000009u, 0x000500b8u, 0x00000010u, 0x00000011u,
    0x0000000eu, 0x0000000fu, 0x000300f7u, 0x00000013u, 0x00000000u, 0x000400fau, 0x00000011u, 0x00000012u,
    0x00000013u, 0x000200f8u, 0x00000012u, 0x000200feu, 0x00000014u, 0x000200f8u, 0x00000013u, 0x0004003du,
    0x00000006u, 0x00000016u, 0x00000009u, 0x000500beu, 0x00000010u, 0x00000018u, 0x00000016u, 0x00000017u,
    0x000300f7u, 0x0000001au, 0x00000000u, 0x000400fau, 0x00000018u, 0x00000019u, 0x0000001au, 0x000200f8u,
    0x00000019u, 0x000200feu, 0x0000001bu, 0x000200f8u, 0x0000001au, 0x0004003du, 0x00000006u, 0x0000001fu,
    0x00000009u, 0x00050085u, 0x00000006u, 0x00000020u, 0x0000001eu, 0x0000001fu, 0x0003003eu, 0x0000001du,
    0x00000020u, 0x0004003du, 0x00000006u, 0x00000021u, 0x0000001du, 0x0006000cu, 0x00000006u, 0x00000022u,
    0x00000001u, 0x0000000du, 0x00000021u, 0x0004003du, 0x00000006u, 0x00000023u, 0x0000001du, 0x00050088u,
    0x00000006u, 0x00000024u, 0x00000022u, 0x00000023u, 0x0004003du, 0x00000006u, 0x00000025u, 0x0000001du,
    0x00050088u, 0x00000006u, 0x00000026u, 0x00000025u, 0x00000017u, 0x0006000cu, 0x00000006u, 0x00000027u,
    0x00000001u, 0x0000000du, 0x00000026u, 0x0004003du, 0x00000006u, 0x00000028u, 0x0000001du, 0x00050088u,
    0x00000006u, 0x00000029u, 0x00000028u, 0x00000017u, 0x00050088u, 0x00000006u, 0x0000002au, 0x00000027u,
    0x00000029u, 0x00050085u, 0x00000006u, 0x0000002bu, 0x00000024u, 0x0000002au, 0x000200feu, 0x0000002bu,
    0x00010038u,
};
}  // namespace
#endif

// ---------------------------------------------------------------------------
// Presentation surface
// ---------------------------------------------------------------------------
class View {
public:
    bool open(const Options& opt) {
        present_pts_ = opt.present_pts;
        cushion_floor_ms_ = opt.cushion_ms;
        msaa_ = opt.msaa;
        aniso_ = opt.aniso;
        scaling_ = opt.scaling;
        // The aniso scaling mode means anisotropic 16x unless --aniso
        // picked another level; the other modes sample with a plain
        // (or Lanczos) kernel and get no mip chain at all.
        paniso_ = scaling_ == Options::Scaling::Aniso
                      ? (opt.aniso > 0 ? opt.aniso : 16)
                      : 0;
        std::fprintf(stderr, "[wireplay] preview scaling: %s\n",
                     scaling_ == Options::Scaling::Lanczos ? "lanczos (Lanczos-3)"
                     : scaling_ == Options::Scaling::Aniso ? "aniso"
                                                            : "linear");
        if (SDL_Init(SDL_INIT_VIDEO | (opt.no_audio ? 0 : SDL_INIT_AUDIO)) != 0) {
            std::fprintf(stderr, "[wireplay] SDL_Init failed: %s\n", SDL_GetError());
            return false;
        }
        sdl_owned_ = true;
        // RESIZABLE from birth: without it the window has no maximise box
        // on Windows, and enabling it later (setGeometry did) does not
        // retrofit one. Maximised-window is not fullscreen: the taskbar
        // and window frame stay, which is what a streamer wants next to
        // OBS. F11 remains the true-fullscreen toggle.
        Uint32 flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE;
        if (opt.Vulkan) flags |= SDL_WINDOW_VULKAN;
        if (opt.opengl) flags |= SDL_WINDOW_OPENGL;
        if (opt.fullscreen) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
        // Born in the top-left corner of the primary display, never
        // centred: the window grows to fit the phone's aspect from
        // wherever its top-left corner sits, so a centred birth pushes
        // the grown window off the right/bottom edges and the user has
        // to drag it back before they can even see the whole picture.
        SDL_Rect display {};
        if (SDL_GetDisplayUsableBounds(0, &display) != 0) { display.x = 0; display.y = 0; }
        window_ = SDL_CreateWindow("WirePlay", display.x, display.y, 430, 932, flags);
        if (!window_ && opt.Vulkan) {
            // An SDL2 built without Vulkan support refuses the window
            // outright; retry plainly so the app still starts on the
            // fallback renderer instead of dying before it can explain.
            std::fprintf(stderr, "[wireplay] Vulkan window failed (%s); retrying without Vulkan\n",
                         SDL_GetError());
            flags &= ~SDL_WINDOW_VULKAN;
            window_ = SDL_CreateWindow("WirePlay", display.x, display.y, 430, 932, flags);
        }
        if (window_) {
            // SDL window position refers to the outer frame, and the OS
            // frame extends beyond it (Windows 11 adds invisible resize
            // borders on top of the visible ones). Placing the outer
            // corner exactly at the display corner pushes the visible
            // border and title bar off-screen -- born looking borderless
            // and impossible to grab. Inset by the real border sizes.
            int top = 0, left = 0, bottom = 0, right = 0;
            if (SDL_GetWindowBordersSize(window_, &top, &left, &bottom, &right) == 0)
                SDL_SetWindowPosition(window_, display.x + left, display.y + top);
        }
        if (!window_) {
            std::fprintf(stderr, "[wireplay] SDL_CreateWindow failed: %s\n", SDL_GetError());
            return false;
        }
#if defined(WIREPLAY_HAVE_VULKAN)
        if (opt.Vulkan) {
            if (initVulkan()) {
                if (opt.always_on_top) SDL_SetWindowAlwaysOnTop(window_, SDL_TRUE);
                if (!opt.headless) SDL_ShowWindow(window_);
                return true;
            }
            std::fprintf(stderr, "[wireplay] Vulkan preview unavailable, falling back\n");
        }
#elif defined(WIREPLAY_NO_VULKAN_NOTE)
        (void)opt;
#endif
#ifdef WIREPLAY_HAVE_GL
        if (opt.opengl) {
            if (initGL()) {
                if (opt.always_on_top) SDL_SetWindowAlwaysOnTop(window_, SDL_TRUE);
                if (!opt.headless) SDL_ShowWindow(window_);
                return true;
            }
            std::fprintf(stderr, "[wireplay] OpenGL preview unavailable, falling back\n");
        }
#endif
#ifdef _WIN32
        if (!opt.force_d3d11) {
            if (initD3D12()) {
                if (opt.always_on_top) SDL_SetWindowAlwaysOnTop(window_, SDL_TRUE);
                if (!opt.headless) SDL_ShowWindow(window_);
                return true;
            }
            std::fprintf(stderr, "[wireplay] D3D12 preview unavailable, falling back to D3D11\n");
        }
        if (initD3D()) {
            if (opt.always_on_top) SDL_SetWindowAlwaysOnTop(window_, SDL_TRUE);
            if (!opt.headless) SDL_ShowWindow(window_);
            return true;
        }
        std::fprintf(stderr, "[wireplay] D3D11 preview unavailable, using SDL renderer\n");
#endif
        renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED);
        if (!renderer_) {
            // No GPU driver: software rendering still holds 60 fps at phone
            // resolution, and keeps the app usable over remote desktop.
            renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
        }
        if (!renderer_) {
            std::fprintf(stderr, "[wireplay] SDL_CreateRenderer failed: %s\n", SDL_GetError());
            return false;
        }
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        // SDL fallback renderer: "2" is SDL's best (anisotropic where
        // the driver offers it); MSAA does not exist on this path.
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, opt.aniso > 1 ? "2" : "1");
        if (opt.always_on_top) SDL_SetWindowAlwaysOnTop(window_, SDL_TRUE);
        if (!opt.headless) SDL_ShowWindow(window_);
        return true;
    }

    // Resizes the window to the stream's aspect ratio and recreates the target
    // texture. Called on the first frame and whenever geometry changes, which
    // on iOS means every rotation.
    void setGeometry(int w, int h) {
        if (w <= 0 || h <= 0 || (w == width_ && h == height_)) return;
        // Headless mode has no renderer; still record the geometry so the run
        // reports what it would have presented, but do not touch SDL.
        if (!renderer_ && !d3d_ready_ && !d3d12_ready_ && !vulkanReady() && !glReady()) {
            width_ = w;
            height_ = h;
            return;
        }
        width_ = w;
        height_ = h;
        if (renderer_) {
            if (texture_) { SDL_DestroyTexture(texture_); texture_ = nullptr; }
            texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_BGRA32,
                                         SDL_TEXTUREACCESS_STREAMING, w, h);
            if (!texture_) {
                std::fprintf(stderr, "[wireplay] SDL_CreateTexture(%dx%d) failed: %s\n", w, h,
                             SDL_GetError());
                return;
            }
        }
        // Window placement rule (product spec, not a guess): in windowed
        // mode the window is sized to half the phone's resolution and
        // centred on the display, and its total size -- frame included --
        // may not exceed 90% of the usable display in either dimension.
        // Maximised and fullscreen windows are left exactly as the user
        // put them; only the texture is recreated for the new geometry.
        const Uint32 wflags = SDL_GetWindowFlags(window_);
        if (!(wflags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN |
                        SDL_WINDOW_FULLSCREEN_DESKTOP))) {
            SDL_Rect db {};
            if (SDL_GetDisplayUsableBounds(SDL_GetWindowDisplayIndex(window_), &db) != 0 ||
                db.w <= 0 || db.h <= 0) {
                db.x = 0; db.y = 0; db.w = 1920; db.h = 1080;
            }
            int top = 0, left = 0, bottom = 0, right = 0;
            SDL_GetWindowBordersSize(window_, &top, &left, &bottom, &right);
            const double avail_w = 0.9 * db.w - (left + right);
            const double avail_h = 0.9 * db.h - (top + bottom);
            const double scale = std::min({0.5, avail_w / w, avail_h / h});
            const int client_w = std::max(160, int(w * scale));
            const int client_h = std::max(120, int(h * scale));
            SDL_SetWindowSize(window_, client_w, client_h);
            // Centre the *outer* rectangle (client + frame) on the usable
            // display, so the visible window is what is actually centred.
            const int outer_w = client_w + left + right;
            const int outer_h = client_h + top + bottom;
            SDL_SetWindowPosition(window_, db.x + (db.w - outer_w) / 2,
                                  db.y + (db.h - outer_h) / 2);
        }
        // std::string, not char[160] + snprintf: nothing to size, nothing
        // to truncate at 160, and nothing for a compiler to deprecate.
        const std::string title =
            "WirePlay - " + std::to_string(w) + "x" + std::to_string(h);
        SDL_SetWindowTitle(window_, title.c_str());
    }

    void present(const AVFrame* frame) {
#if defined(WIREPLAY_HAVE_VULKAN)
        if (vk_ready_) { presentVulkan(frame); ++presented_; return; }
#endif
#ifdef _WIN32
        if (d3d12_ready_) { presentBlank12(); return; }
        if (d3d_ready_) { presentBlank(); return; }
#endif
        if (!renderer_) return;
        // Black, explicitly, every frame: drawOverlay() leaves the draw colour
        // at its light text colour, and without this the clear -- which is what
        // the letterbox bars are -- came out white, flashing on every status
        // message and glaring around the picture in fullscreen.
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        SDL_RenderClear(renderer_);
        if (texture_ && frame && frame->width == width_ && frame->height == height_) {
            SDL_UpdateTexture(texture_, nullptr, frame->data[0], frame->linesize[0]);
            SDL_Rect dst = fitRect(width_, height_);
            SDL_RenderCopy(renderer_, texture_, nullptr, &dst);
        } else {
            drawOverlay();
        }
        SDL_RenderPresent(renderer_);
        ++presented_;
    }

#if defined(WIREPLAY_HAVE_VULKAN)
    // ------------------------------------------------------------------
    // Vulkan preview (--renderer vulkan). CPU BGRA frames are uploaded
    // through a staging buffer into a Vulkan image, then blitted (with
    // scaling, so letterboxing is just a sub-rectangle of the swapchain
    // image) and presented with vsync OFF: IMMEDIATE if the surface
    // offers it, MAILBOX next, FIFO only as a last resort. It exists as
    // an alternative presentation engine; the D3D11 path stays the
    // default and the rollback.
    // ------------------------------------------------------------------
    bool vkFail(const char* what, VkResult r) {
        std::fprintf(stderr, "[wireplay] Vulkan: %s failed (VkResult %d)\n", what, int(r));
        return false;
    }

    bool initVulkan() {
        if (SDL_Vulkan_LoadLibrary(nullptr) != 0) {
            std::fprintf(stderr, "[wireplay] Vulkan: SDL_Vulkan_LoadLibrary: %s\n", SDL_GetError());
            return false;
        }
        unsigned ext_count = 0;
        if (!SDL_Vulkan_GetInstanceExtensions(window_, &ext_count, nullptr)) return false;
        std::vector<const char*> exts(ext_count);
        if (!SDL_Vulkan_GetInstanceExtensions(window_, &ext_count, exts.data())) return false;

        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.apiVersion = VK_API_VERSION_1_0;
        VkInstanceCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ici.pApplicationInfo = &app;
        ici.enabledExtensionCount = uint32_t(exts.size());
        ici.ppEnabledExtensionNames = exts.data();
        VkResult r = vkCreateInstance(&ici, nullptr, &vk_inst_);
        if (r != VK_SUCCESS) return vkFail("vkCreateInstance", r);
        if (!SDL_Vulkan_CreateSurface(window_, vk_inst_, &vk_surf_)) {
            std::fprintf(stderr, "[wireplay] Vulkan: SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
            return false;
        }

        uint32_t ndev = 0;
        vkEnumeratePhysicalDevices(vk_inst_, &ndev, nullptr);
        std::vector<VkPhysicalDevice> devs(ndev);
        vkEnumeratePhysicalDevices(vk_inst_, &ndev, devs.data());
        for (VkPhysicalDevice cand : devs) {
            uint32_t nq = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(cand, &nq, nullptr);
            std::vector<VkQueueFamilyProperties> qfs(nq);
            vkGetPhysicalDeviceQueueFamilyProperties(cand, &nq, qfs.data());
            for (uint32_t i = 0; i < nq; ++i) {
                VkBool32 can_present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(cand, i, vk_surf_, &can_present);
                if ((qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && can_present) {
                    vk_phys_ = cand;
                    vk_qfamily_ = i;
                    break;
                }
            }
            if (vk_phys_) break;
        }
        if (!vk_phys_) { std::fprintf(stderr, "[wireplay] Vulkan: no graphics+present GPU\n"); return false; }

        const float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{};
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = vk_qfamily_;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        const char* dext = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        VkDeviceCreateInfo dci{};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = 1;
        dci.ppEnabledExtensionNames = &dext;
        r = vkCreateDevice(vk_phys_, &dci, nullptr, &vk_dev_);
        if (r != VK_SUCCESS) return vkFail("vkCreateDevice", r);
        vkGetDeviceQueue(vk_dev_, vk_qfamily_, 0, &vk_queue_);

        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = vk_qfamily_;
        if (vkCreateCommandPool(vk_dev_, &pci, nullptr, &vk_pool_) != VK_SUCCESS) return false;
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = vk_pool_;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(vk_dev_, &cai, &vk_cmd_) != VK_SUCCESS) return false;
        VkSemaphoreCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (vkCreateSemaphore(vk_dev_, &sci, nullptr, &vk_acquire_) != VK_SUCCESS ||
            vkCreateSemaphore(vk_dev_, &sci, nullptr, &vk_done_) != VK_SUCCESS ||
            vkCreateFence(vk_dev_, &fci, nullptr, &vk_fence_) != VK_SUCCESS) return false;

        // The swapchain is created lazily on the first present(): the
        // window is still hidden here, and a surface created against a
        // hidden window reports a stale extent (field result:
        // VK_ERROR_SURFACE_LOST on every frame).
        vk_ready_ = true;
        stop_preview_ = false;
        preview_thread_ = std::thread([this] { previewLoop(); });
        std::fprintf(stderr, "[wireplay] preview renderer: Vulkan (vsync off)\n");
        if (msaa_ > 1)
            std::fprintf(stderr,
                         "[wireplay] Vulkan: --msaa not applied on this path "
                         "(the --scaling filter still applies)\n");
        return true;
    }

    bool vkEnsureSwapchain() {
        VkSurfaceCapabilitiesKHR caps{};
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk_phys_, vk_surf_, &caps);
        // When currentExtent is fixed (not the 0xFFFFFFFF sentinel) the
        // spec requires  that exact extent; asking for anything else
        // loses the surface. This was the field failure: swapchain sized
        // from SDL's drawable size while the surface was pinned to the
        // hidden window's birth size.
        int dw = 0, dh = 0;
        if (caps.currentExtent.width != 0xFFFFFFFFu) {
            dw = int(caps.currentExtent.width);
            dh = int(caps.currentExtent.height);
        } else {
            SDL_Vulkan_GetDrawableSize(window_, &dw, &dh);
            dw = std::clamp(dw, int(caps.minImageExtent.width), int(caps.maxImageExtent.width));
            dh = std::clamp(dh, int(caps.minImageExtent.height), int(caps.maxImageExtent.height));
        }
        if (dw <= 0 || dh <= 0) return false;  // minimised: skip quietly
        if (vk_swap_ && dw == vk_sw_w_ && dh == vk_sw_h_) return true;
        vkDeviceWaitIdle(vk_dev_);

        uint32_t nfmt = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(vk_phys_, vk_surf_, &nfmt, nullptr);
        std::vector<VkSurfaceFormatKHR> fmts(nfmt);
        vkGetPhysicalDeviceSurfaceFormatsKHR(vk_phys_, vk_surf_, &nfmt, fmts.data());
        VkSurfaceFormatKHR chosen{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
        if (!(nfmt == 1 && fmts[0].format == VK_FORMAT_UNDEFINED)) {
            bool found = false;
            for (const auto& f : fmts)
                if (f.format == VK_FORMAT_B8G8R8A8_UNORM) { chosen = f; found = true; break; }
            if (!found) { std::fprintf(stderr, "[wireplay] Vulkan: surface has no BGRA format\n"); return false; }
        }

        // Vsync off: IMMEDIATE presents as fast as we submit; MAILBOX is
        // the tear-free approximation; FIFO (true vsync) is the fallback
        // we hope never to take.
        uint32_t nmodes = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(vk_phys_, vk_surf_, &nmodes, nullptr);
        std::vector<VkPresentModeKHR> modes(nmodes);
        vkGetPhysicalDeviceSurfacePresentModesKHR(vk_phys_, vk_surf_, &nmodes, modes.data());
        VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
        vk_mode_name_ = "FIFO";
        for (VkPresentModeKHR m : modes) if (m == VK_PRESENT_MODE_MAILBOX_KHR) { mode = m; vk_mode_name_ = "MAILBOX"; }
        for (VkPresentModeKHR m : modes) if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) { mode = m; vk_mode_name_ = "IMMEDIATE"; }

        VkSwapchainCreateInfoKHR sci{};
        sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        sci.surface = vk_surf_;
        sci.minImageCount = caps.maxImageCount > 0
                                ? std::min(caps.minImageCount + 1, caps.maxImageCount)
                                : caps.minImageCount + 1;
        sci.imageFormat = chosen.format;
        sci.imageColorSpace = chosen.colorSpace;
        sci.imageExtent = {uint32_t(dw), uint32_t(dh)};
        sci.imageArrayLayers = 1;
        sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                         (vk_lz_ready_ ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0);
        sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        sci.preTransform = caps.currentTransform;
        sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        sci.presentMode = mode;
        sci.clipped = VK_TRUE;
        sci.oldSwapchain = vk_swap_;
        VkSwapchainKHR fresh = VK_NULL_HANDLE;
        VkResult r = vkCreateSwapchainKHR(vk_dev_, &sci, nullptr, &fresh);
        if (r != VK_SUCCESS) {
            if (!vk_swap_err_logged_) {
                vk_swap_err_logged_ = true;
                vkFail("vkCreateSwapchainKHR", r);
            }
            return false;
        }
        vk_swap_err_logged_ = false;
        std::fprintf(stderr, "[wireplay] Vulkan swapchain %dx%d (present mode %s)\n", dw, dh,
                     vk_mode_name_);
        vkDestroySwapTargets();
        if (vk_swap_) vkDestroySwapchainKHR(vk_dev_, vk_swap_, nullptr);
        vk_swap_ = fresh;
        uint32_t nimg = 0;
        vkGetSwapchainImagesKHR(vk_dev_, vk_swap_, &nimg, nullptr);
        vk_images_.assign(nimg, VK_NULL_HANDLE);
        vkGetSwapchainImagesKHR(vk_dev_, vk_swap_, &nimg, vk_images_.data());
        vk_sw_w_ = dw;
        vk_sw_h_ = dh;
        if (vk_lz_ready_) vkBuildSwapTargets();
        return true;
    }

    void vkDestroySwapTargets() {
        for (VkFramebuffer fb : vk_swap_fbs_)
            if (fb) vkDestroyFramebuffer(vk_dev_, fb, nullptr);
        for (VkImageView v : vk_swap_views_)
            if (v) vkDestroyImageView(vk_dev_, v, nullptr);
        vk_swap_fbs_.clear();
        vk_swap_views_.clear();
    }

    void vkBuildSwapTargets() {
        vkDestroySwapTargets();
        vk_swap_views_.assign(vk_images_.size(), VK_NULL_HANDLE);
        vk_swap_fbs_.assign(vk_images_.size(), VK_NULL_HANDLE);
        for (size_t i = 0; i < vk_images_.size(); ++i) {
            VkImageViewCreateInfo vci{};
            vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vci.image = vk_images_[i];
            vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vci.format = VK_FORMAT_B8G8R8A8_UNORM;
            vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            if (vkCreateImageView(vk_dev_, &vci, nullptr, &vk_swap_views_[i]) != VK_SUCCESS)
                return;
            VkFramebufferCreateInfo fci{};
            fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fci.renderPass = vk_lz_rp_out_;
            fci.attachmentCount = 1;
            fci.pAttachments = &vk_swap_views_[i];
            fci.width = uint32_t(vk_sw_w_);
            fci.height = uint32_t(vk_sw_h_);
            fci.layers = 1;
            if (vkCreateFramebuffer(vk_dev_, &fci, nullptr, &vk_swap_fbs_[i]) != VK_SUCCESS)
                return;
        }
    }

    void vkWriteSet(VkDescriptorSet set, VkImageView view) {
        VkDescriptorImageInfo info{};
        info.sampler = vk_lz_sampler_;
        info.imageView = view;
        info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet wr{};
        wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr.dstSet = set;
        wr.dstBinding = 0;
        wr.descriptorCount = 1;
        wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr.pImageInfo = &info;
        vkUpdateDescriptorSets(vk_dev_, 1, &wr, 0, nullptr);
    }

    bool vkEnsureMid(int dw, int fh) {
        if (vk_mid_img_ && dw == vk_mid_w_ && fh == vk_mid_h_) return true;
        vkDeviceWaitIdle(vk_dev_);
        if (vk_mid_fb_) { vkDestroyFramebuffer(vk_dev_, vk_mid_fb_, nullptr); vk_mid_fb_ = VK_NULL_HANDLE; }
        if (vk_mid_view_) { vkDestroyImageView(vk_dev_, vk_mid_view_, nullptr); vk_mid_view_ = VK_NULL_HANDLE; }
        if (vk_mid_img_) { vkDestroyImage(vk_dev_, vk_mid_img_, nullptr); vk_mid_img_ = VK_NULL_HANDLE; }
        if (vk_mid_mem_) { vkFreeMemory(vk_dev_, vk_mid_mem_, nullptr); vk_mid_mem_ = VK_NULL_HANDLE; }
        VkImageCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_B8G8R8A8_UNORM;
        ici.extent = {uint32_t(dw), uint32_t(fh), 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        if (vkCreateImage(vk_dev_, &ici, nullptr, &vk_mid_img_) != VK_SUCCESS) return false;
        VkMemoryRequirements mr{};
        vkGetImageMemoryRequirements(vk_dev_, vk_mid_img_, &mr);
        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = vkMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(vk_dev_, &mai, nullptr, &vk_mid_mem_) != VK_SUCCESS) return false;
        vkBindImageMemory(vk_dev_, vk_mid_img_, vk_mid_mem_, 0);
        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = vk_mid_img_;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_B8G8R8A8_UNORM;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(vk_dev_, &vci, nullptr, &vk_mid_view_) != VK_SUCCESS) return false;
        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = vk_lz_rp_mid_;
        fci.attachmentCount = 1;
        fci.pAttachments = &vk_mid_view_;
        fci.width = uint32_t(dw);
        fci.height = uint32_t(fh);
        fci.layers = 1;
        if (vkCreateFramebuffer(vk_dev_, &fci, nullptr, &vk_mid_fb_) != VK_SUCCESS) return false;
        vkWriteSet(vk_lz_sets_[1], vk_mid_view_);
        vk_mid_w_ = dw;
        vk_mid_h_ = fh;
        return true;
    }

    // Build the Lanczos pipeline once (device scope; sizes are dynamic
    // viewport state). Failure leaves vk_lz_failed_ set and the caller
    // keeps the blit path.
    bool vkSetupLanczos() {
        if (vk_lz_ready_) return true;
        if (vk_lz_failed_) return false;
        auto fail = [this](const char* what) {
            std::fprintf(stderr, "[wireplay] Vulkan Lanczos: %s failed; blit fallback\n", what);
            vk_lz_failed_ = true;
            return false;
        };
        VkShaderModuleCreateInfo smci{};
        smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smci.codeSize = sizeof(kVkLzVert);
        smci.pCode = kVkLzVert;
        if (vkCreateShaderModule(vk_dev_, &smci, nullptr, &vk_lz_vs_) != VK_SUCCESS)
            return fail("vkCreateShaderModule(vert)");
        smci.codeSize = sizeof(kVkLzFrag);
        smci.pCode = kVkLzFrag;
        if (vkCreateShaderModule(vk_dev_, &smci, nullptr, &vk_lz_fs_) != VK_SUCCESS)
            return fail("vkCreateShaderModule(frag)");

        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = VK_FILTER_NEAREST;
        sci.minFilter = VK_FILTER_NEAREST;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = sci.addressModeV = sci.addressModeW =
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(vk_dev_, &sci, nullptr, &vk_lz_sampler_) != VK_SUCCESS)
            return fail("vkCreateSampler");

        VkDescriptorSetLayoutBinding bind{};
        bind.binding = 0;
        bind.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bind.descriptorCount = 1;
        bind.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo dslci{};
        dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslci.bindingCount = 1;
        dslci.pBindings = &bind;
        if (vkCreateDescriptorSetLayout(vk_dev_, &dslci, nullptr, &vk_lz_dsl_) != VK_SUCCESS)
            return fail("vkCreateDescriptorSetLayout");
        VkDescriptorPoolSize psz{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
        VkDescriptorPoolCreateInfo dpci{};
        dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpci.maxSets = 2;
        dpci.poolSizeCount = 1;
        dpci.pPoolSizes = &psz;
        if (vkCreateDescriptorPool(vk_dev_, &dpci, nullptr, &vk_lz_pool_) != VK_SUCCESS)
            return fail("vkCreateDescriptorPool");
        VkDescriptorSetLayout layouts[2] = {vk_lz_dsl_, vk_lz_dsl_};
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = vk_lz_pool_;
        dsai.descriptorSetCount = 2;
        dsai.pSetLayouts = layouts;
        if (vkAllocateDescriptorSets(vk_dev_, &dsai, vk_lz_sets_) != VK_SUCCESS)
            return fail("vkAllocateDescriptorSets");

        auto makePass = [this](VkImageLayout final_layout, VkAttachmentLoadOp load,
                               VkRenderPass* out) {
            VkAttachmentDescription ad{};
            ad.format = VK_FORMAT_B8G8R8A8_UNORM;
            ad.samples = VK_SAMPLE_COUNT_1_BIT;
            ad.loadOp = load;
            ad.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            ad.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            ad.finalLayout = final_layout;
            VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
            VkSubpassDescription sub{};
            sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            sub.colorAttachmentCount = 1;
            sub.pColorAttachments = &ref;
            VkSubpassDependency deps[2]{};
            deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
            deps[0].dstSubpass = 0;
            deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            deps[1].srcSubpass = 0;
            deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
            deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            VkRenderPassCreateInfo rpci{};
            rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
            rpci.attachmentCount = 1;
            rpci.pAttachments = &ad;
            rpci.subpassCount = 1;
            rpci.pSubpasses = &sub;
            rpci.dependencyCount = 2;
            rpci.pDependencies = deps;
            return vkCreateRenderPass(vk_dev_, &rpci, nullptr, out) == VK_SUCCESS;
        };
        if (!makePass(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_ATTACHMENT_LOAD_OP_DONT_CARE, &vk_lz_rp_mid_))
            return fail("vkCreateRenderPass(mid)");
        if (!makePass(VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ATTACHMENT_LOAD_OP_CLEAR,
                      &vk_lz_rp_out_))
            return fail("vkCreateRenderPass(out)");

        VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &vk_lz_dsl_;
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &pcr;
        if (vkCreatePipelineLayout(vk_dev_, &plci, nullptr, &vk_lz_layout_) != VK_SUCCESS)
            return fail("vkCreatePipelineLayout");

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vk_lz_vs_;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = vk_lz_fs_;
        stages[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vin{};
        vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo ias{};
        ias.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ias.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        VkPipelineViewportStateCreateInfo vps{};
        vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vps.viewportCount = 1;
        vps.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo ras{};
        ras.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        ras.polygonMode = VK_POLYGON_MODE_FILL;
        ras.cullMode = VK_CULL_MODE_NONE;
        ras.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        ras.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo mss{};
        mss.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        mss.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cbs{};
        cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cbs.attachmentCount = 1;
        cbs.pAttachments = &cba;
        VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dys{};
        dys.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dys.dynamicStateCount = 2;
        dys.pDynamicStates = dyn;
        VkGraphicsPipelineCreateInfo gpci{};
        gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gpci.stageCount = 2;
        gpci.pStages = stages;
        gpci.pVertexInputState = &vin;
        gpci.pInputAssemblyState = &ias;
        gpci.pViewportState = &vps;
        gpci.pRasterizationState = &ras;
        gpci.pMultisampleState = &mss;
        gpci.pColorBlendState = &cbs;
        gpci.pDynamicState = &dys;
        gpci.layout = vk_lz_layout_;
        gpci.subpass = 0;
        // One pipeline per render pass: loadOp/finalLayout differ between
        // them, and render-pass compatibility does not cover that on
        // every driver.
        gpci.renderPass = vk_lz_rp_mid_;
        if (vkCreateGraphicsPipelines(vk_dev_, VK_NULL_HANDLE, 1, &gpci, nullptr,
                                      &vk_lz_pipe_[0]) != VK_SUCCESS)
            return fail("vkCreateGraphicsPipelines(mid)");
        gpci.renderPass = vk_lz_rp_out_;
        if (vkCreateGraphicsPipelines(vk_dev_, VK_NULL_HANDLE, 1, &gpci, nullptr,
                                      &vk_lz_pipe_[1]) != VK_SUCCESS)
            return fail("vkCreateGraphicsPipelines(out)");
        vk_lz_ready_ = true;
        std::fprintf(stderr, "[wireplay] Vulkan: Lanczos-3 pipeline active\n");
        return true;
    }

    uint32_t vkMemoryType(uint32_t type_bits, VkMemoryPropertyFlags props) const {
        VkPhysicalDeviceMemoryProperties mp{};
        vkGetPhysicalDeviceMemoryProperties(vk_phys_, &mp);
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
            if ((type_bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props)
                return i;
        return 0;
    }

    bool vkEnsureFrame(int w, int h) {
        if (vk_fimg_ && w == vk_fw_ && h == vk_fh_) return true;
        vkDeviceWaitIdle(vk_dev_);
        if (vk_fimg_view_) { vkDestroyImageView(vk_dev_, vk_fimg_view_, nullptr); vk_fimg_view_ = VK_NULL_HANDLE; }
        if (vk_fimg_) { vkDestroyImage(vk_dev_, vk_fimg_, nullptr); vk_fimg_ = VK_NULL_HANDLE; }
        if (vk_fmem_) { vkFreeMemory(vk_dev_, vk_fmem_, nullptr); vk_fmem_ = VK_NULL_HANDLE; }
        if (vk_stage_) { vkDestroyBuffer(vk_dev_, vk_stage_, nullptr); vk_stage_ = VK_NULL_HANDLE; }
        if (vk_stage_mem_) {
            if (vk_stage_ptr_) vkUnmapMemory(vk_dev_, vk_stage_mem_);
            vkFreeMemory(vk_dev_, vk_stage_mem_, nullptr);
            vk_stage_mem_ = VK_NULL_HANDLE;
            vk_stage_ptr_ = nullptr;
        }
        VkImageCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_B8G8R8A8_UNORM;
        ici.extent = {uint32_t(w), uint32_t(h), 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                    (vk_lz_ready_ ? VK_IMAGE_USAGE_SAMPLED_BIT : 0);
        if (vkCreateImage(vk_dev_, &ici, nullptr, &vk_fimg_) != VK_SUCCESS) return false;
        VkMemoryRequirements mr{};
        vkGetImageMemoryRequirements(vk_dev_, vk_fimg_, &mr);
        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = vkMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(vk_dev_, &mai, nullptr, &vk_fmem_) != VK_SUCCESS) return false;
        vkBindImageMemory(vk_dev_, vk_fimg_, vk_fmem_, 0);

        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = VkDeviceSize(w) * uint32_t(h) * 4;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(vk_dev_, &bci, nullptr, &vk_stage_) != VK_SUCCESS) return false;
        vkGetBufferMemoryRequirements(vk_dev_, vk_stage_, &mr);
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = vkMemoryType(
            mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(vk_dev_, &mai, nullptr, &vk_stage_mem_) != VK_SUCCESS) return false;
        vkBindBufferMemory(vk_dev_, vk_stage_, vk_stage_mem_, 0);
        if (vkMapMemory(vk_dev_, vk_stage_mem_, 0, VK_WHOLE_SIZE, 0, &vk_stage_ptr_) != VK_SUCCESS)
            return false;
        if (vk_lz_ready_) {
            VkImageViewCreateInfo vci{};
            vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vci.image = vk_fimg_;
            vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vci.format = VK_FORMAT_B8G8R8A8_UNORM;
            vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            if (vkCreateImageView(vk_dev_, &vci, nullptr, &vk_fimg_view_) != VK_SUCCESS)
                return false;
            vkWriteSet(vk_lz_sets_[0], vk_fimg_view_);
        }
        vk_fw_ = w;
        vk_fh_ = h;
        vk_flayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
        return true;
    }

    void presentVulkan(const AVFrame* frame) {
        if (scaling_ == Options::Scaling::Lanczos) vkSetupLanczos();
        if (!vkEnsureSwapchain()) return;
        const bool have = frame && frame->data[0] && frame->width > 0 && frame->height > 0;
        if (have && !vkEnsureFrame(frame->width, frame->height)) return;
        if (have) {
            auto* dst = static_cast<uint8_t*>(vk_stage_ptr_);
            const int row = frame->width * 4;
            for (int y = 0; y < frame->height; ++y)
                std::memcpy(dst + size_t(y) * row, frame->data[0] + size_t(y) * frame->linesize[0],
                            size_t(std::min(row, frame->linesize[0])));
        }
        uint32_t idx = 0;
        VkResult r = vkAcquireNextImageKHR(vk_dev_, vk_swap_, UINT64_MAX, vk_acquire_,
                                           VK_NULL_HANDLE, &idx);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_SURFACE_LOST_KHR) {
            vk_swap_ = VK_NULL_HANDLE;
            return;
        }
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return;
        vkWaitForFences(vk_dev_, 1, &vk_fence_, VK_TRUE, UINT64_MAX);
        vkResetFences(vk_dev_, 1, &vk_fence_);
        vkResetCommandBuffer(vk_cmd_, 0);
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(vk_cmd_, &bi);
        VkImage swap_img = vk_images_[idx];
        auto barrier = [&](VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src_acc,
                           VkAccessFlags dst_acc, VkPipelineStageFlags src_stage,
                           VkPipelineStageFlags dst_stage) {
            VkImageMemoryBarrier b{};
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.oldLayout = from;
            b.newLayout = to;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = img;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            b.srcAccessMask = src_acc;
            b.dstAccessMask = dst_acc;
            vkCmdPipelineBarrier(vk_cmd_, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
        };
        if (have && scaling_ == Options::Scaling::Lanczos && vk_lz_ready_) {
            const double s = std::min(double(vk_sw_w_) / vk_fw_, double(vk_sw_h_) / vk_fh_);
            const int dw = std::max(1, int(vk_fw_ * s)), dh = std::max(1, int(vk_fh_ * s));
            const int ox = (vk_sw_w_ - dw) / 2, oy = (vk_sw_h_ - dh) / 2;
            if (idx < vk_swap_fbs_.size() && vk_swap_fbs_[idx] && vkEnsureMid(dw, vk_fh_)) {
                barrier(vk_fimg_, vk_flayout_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                VkBufferImageCopy region{};
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.imageExtent = {uint32_t(vk_fw_), uint32_t(vk_fh_), 1};
                vkCmdCopyBufferToImage(vk_cmd_, vk_stage_, vk_fimg_,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                barrier(vk_fimg_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                vk_flayout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                float push[8] = {float(vk_fw_), float(vk_fh_), 1.0f / float(vk_fw_),
                                 1.0f / float(vk_fh_),
                                 float(vk_fw_) / float(dw), 0.0f, 0.0f, 0.0f};
                VkClearValue clear{};
                VkRenderPassBeginInfo rbi{};
                rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
                rbi.renderPass = vk_lz_rp_mid_;
                rbi.framebuffer = vk_mid_fb_;
                rbi.renderArea = {{0, 0}, {uint32_t(dw), uint32_t(vk_fh_)}};
                vkCmdBeginRenderPass(vk_cmd_, &rbi, VK_SUBPASS_CONTENTS_INLINE);
                vkCmdBindPipeline(vk_cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_lz_pipe_[0]);
                VkViewport vp{0.0f, 0.0f, float(dw), float(vk_fh_), 0.0f, 1.0f};
                VkRect2D sc{{0, 0}, {uint32_t(dw), uint32_t(vk_fh_)}};
                vkCmdSetViewport(vk_cmd_, 0, 1, &vp);
                vkCmdSetScissor(vk_cmd_, 0, 1, &sc);
                vkCmdBindDescriptorSets(vk_cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        vk_lz_layout_, 0, 1, &vk_lz_sets_[0], 0, nullptr);
                vkCmdPushConstants(vk_cmd_, vk_lz_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                   32, push);
                vkCmdDraw(vk_cmd_, 4, 1, 0, 0);
                vkCmdEndRenderPass(vk_cmd_);
                push[0] = float(dw);
                push[1] = float(vk_fh_);
                push[2] = 1.0f / float(dw);
                push[3] = 1.0f / float(vk_fh_);
                push[4] = float(vk_fh_) / float(dh);
                push[5] = 1.0f;
                rbi.renderPass = vk_lz_rp_out_;
                rbi.framebuffer = vk_swap_fbs_[idx];
                rbi.renderArea = {{0, 0}, {uint32_t(vk_sw_w_), uint32_t(vk_sw_h_)}};
                rbi.clearValueCount = 1;
                rbi.pClearValues = &clear;
                vkCmdBeginRenderPass(vk_cmd_, &rbi, VK_SUBPASS_CONTENTS_INLINE);
                vkCmdBindPipeline(vk_cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_lz_pipe_[1]);
                vp = VkViewport{float(ox), float(oy), float(dw), float(dh), 0.0f, 1.0f};
                sc = VkRect2D{{ox, oy}, {uint32_t(dw), uint32_t(dh)}};
                vkCmdSetViewport(vk_cmd_, 0, 1, &vp);
                vkCmdSetScissor(vk_cmd_, 0, 1, &sc);
                vkCmdBindDescriptorSets(vk_cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        vk_lz_layout_, 0, 1, &vk_lz_sets_[1], 0, nullptr);
                vkCmdPushConstants(vk_cmd_, vk_lz_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                   32, push);
                vkCmdDraw(vk_cmd_, 4, 1, 0, 0);
                vkCmdEndRenderPass(vk_cmd_);
                vkEndCommandBuffer(vk_cmd_);
                VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
                VkSubmitInfo si{};
                si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                si.waitSemaphoreCount = 1;
                si.pWaitSemaphores = &vk_acquire_;
                si.pWaitDstStageMask = &wait_stage;
                si.commandBufferCount = 1;
                si.pCommandBuffers = &vk_cmd_;
                si.signalSemaphoreCount = 1;
                si.pSignalSemaphores = &vk_done_;
                vkQueueSubmit(vk_queue_, 1, &si, vk_fence_);
                VkPresentInfoKHR pi{};
                pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
                pi.waitSemaphoreCount = 1;
                pi.pWaitSemaphores = &vk_done_;
                pi.swapchainCount = 1;
                pi.pSwapchains = &vk_swap_;
                pi.pImageIndices = &idx;
                r = vkQueuePresentKHR(vk_queue_, &pi);
                if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_SURFACE_LOST_KHR)
                    vk_swap_ = VK_NULL_HANDLE;
                return;
            }
        }
        barrier(swap_img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
        if (have) {
            barrier(vk_fimg_, vk_flayout_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {uint32_t(vk_fw_), uint32_t(vk_fh_), 1};
            vkCmdCopyBufferToImage(vk_cmd_, vk_stage_, vk_fimg_,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            barrier(vk_fimg_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);
            vk_flayout_ = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            const double s = std::min(double(vk_sw_w_) / vk_fw_, double(vk_sw_h_) / vk_fh_);
            const int dw = std::max(1, int(vk_fw_ * s)), dh = std::max(1, int(vk_fh_ * s));
            const int ox = (vk_sw_w_ - dw) / 2, oy = (vk_sw_h_ - dh) / 2;
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.srcOffsets[1] = {vk_fw_, vk_fh_, 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.dstOffsets[0] = {ox, oy, 0};
            blit.dstOffsets[1] = {ox + dw, oy + dh, 1};
            // The swapchain image started as pure black only if we clear
            // it; blit leaves the letterbox area untouched, so clear
            // first (below in the same layout) -- done via the clear
            // call before blit.
            VkClearColorValue black{};
            VkImageSubresourceRange full{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(vk_cmd_, swap_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black,
                                 1, &full);
            vkCmdBlitImage(vk_cmd_, vk_fimg_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swap_img,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        } else {
            VkClearColorValue black{};
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(vk_cmd_, swap_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black,
                                 1, &range);
        }
        barrier(swap_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        vkEndCommandBuffer(vk_cmd_);
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &vk_acquire_;
        si.pWaitDstStageMask = &wait_stage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &vk_cmd_;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &vk_done_;
        vkQueueSubmit(vk_queue_, 1, &si, vk_fence_);
        VkPresentInfoKHR pi{};
        pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &vk_done_;
        pi.swapchainCount = 1;
        pi.pSwapchains = &vk_swap_;
        pi.pImageIndices = &idx;
        r = vkQueuePresentKHR(vk_queue_, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_SURFACE_LOST_KHR)
            vk_swap_ = VK_NULL_HANDLE;
    }

    void shutdownVulkan() {
        if (!vk_dev_) return;
        vkDeviceWaitIdle(vk_dev_);
        vkDestroySwapTargets();
        if (vk_mid_fb_) vkDestroyFramebuffer(vk_dev_, vk_mid_fb_, nullptr);
        if (vk_mid_view_) vkDestroyImageView(vk_dev_, vk_mid_view_, nullptr);
        if (vk_mid_img_) vkDestroyImage(vk_dev_, vk_mid_img_, nullptr);
        if (vk_mid_mem_) vkFreeMemory(vk_dev_, vk_mid_mem_, nullptr);
        if (vk_fimg_view_) vkDestroyImageView(vk_dev_, vk_fimg_view_, nullptr);
        for (auto& pipe : vk_lz_pipe_)
            if (pipe) { vkDestroyPipeline(vk_dev_, pipe, nullptr); pipe = VK_NULL_HANDLE; }
        if (vk_lz_layout_) vkDestroyPipelineLayout(vk_dev_, vk_lz_layout_, nullptr);
        if (vk_lz_rp_mid_) vkDestroyRenderPass(vk_dev_, vk_lz_rp_mid_, nullptr);
        if (vk_lz_rp_out_) vkDestroyRenderPass(vk_dev_, vk_lz_rp_out_, nullptr);
        if (vk_lz_pool_) vkDestroyDescriptorPool(vk_dev_, vk_lz_pool_, nullptr);
        if (vk_lz_dsl_) vkDestroyDescriptorSetLayout(vk_dev_, vk_lz_dsl_, nullptr);
        if (vk_lz_sampler_) vkDestroySampler(vk_dev_, vk_lz_sampler_, nullptr);
        if (vk_lz_vs_) vkDestroyShaderModule(vk_dev_, vk_lz_vs_, nullptr);
        if (vk_lz_fs_) vkDestroyShaderModule(vk_dev_, vk_lz_fs_, nullptr);
        vk_lz_ready_ = false;
        if (vk_stage_ptr_ && vk_stage_mem_) vkUnmapMemory(vk_dev_, vk_stage_mem_);
        vk_stage_ptr_ = nullptr;
        if (vk_stage_) vkDestroyBuffer(vk_dev_, vk_stage_, nullptr);
        if (vk_stage_mem_) vkFreeMemory(vk_dev_, vk_stage_mem_, nullptr);
        if (vk_fimg_) vkDestroyImage(vk_dev_, vk_fimg_, nullptr);
        if (vk_fmem_) vkFreeMemory(vk_dev_, vk_fmem_, nullptr);
        if (vk_swap_) vkDestroySwapchainKHR(vk_dev_, vk_swap_, nullptr);
        if (vk_acquire_) vkDestroySemaphore(vk_dev_, vk_acquire_, nullptr);
        if (vk_done_) vkDestroySemaphore(vk_dev_, vk_done_, nullptr);
        if (vk_fence_) vkDestroyFence(vk_dev_, vk_fence_, nullptr);
        if (vk_pool_) vkDestroyCommandPool(vk_dev_, vk_pool_, nullptr);
        vkDestroyDevice(vk_dev_, nullptr);
        vk_dev_ = VK_NULL_HANDLE;
        if (vk_surf_) vkDestroySurfaceKHR(vk_inst_, vk_surf_, nullptr);
        if (vk_inst_) vkDestroyInstance(vk_inst_, nullptr);
        vk_ready_ = false;
    }
#endif  // WIREPLAY_HAVE_VULKAN

#ifdef WIREPLAY_HAVE_GL
    // ------------------------------------------------------------------
    // OpenGL preview (--renderer opengl). Added "de zueira" to complete
    // the renderer comparison (D3D11 vs Vulkan vs GL): CPU BGRA upload
    // into a texture, fixed-function letterboxed quad, swap interval 0
    // (vsync off, like everywhere else in the preview). GL 1.1 only --
    // no extension loader, nothing Windows opengl32.dll does not export.
    // ------------------------------------------------------------------
    bool initGL() {
        if (msaa_ > 1) {  // default-framebuffer MSAA (context creation time)
            SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
            SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, msaa_);
        }
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
        gl_ctx_ = SDL_GL_CreateContext(window_);
        if (!gl_ctx_) {
            std::fprintf(stderr, "[wireplay] SDL_GL_CreateContext: %s\n", SDL_GetError());
            return false;
        }
        if (SDL_GL_MakeCurrent(window_, gl_ctx_) != 0) return false;
        SDL_GL_SetSwapInterval(0);  // vsync off: the stream is the clock
        GLuint tex = 0;
        glGenTextures(1, &tex);
        gl_tex_ = tex;
        glBindTexture(GL_TEXTURE_2D, gl_tex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                        paniso_ > 1 ? 0x2703 /*GL_LINEAR_MIPMAP_LINEAR*/ : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F /*CLAMP_TO_EDGE*/);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
        gl_gen_mipmap_ = reinterpret_cast<void (*)(unsigned int)>(
            SDL_GL_GetProcAddress("glGenerateMipmap"));
        glDisable(GL_DEPTH_TEST);
        glClearColor(0.f, 0.f, 0.f, 1.f);
        // The preview thread presents; release the context here so that
        // thread may claim it (a GL context is current on one thread).
        SDL_GL_MakeCurrent(window_, nullptr);
        gl_ready_ = true;
        // The preview thread starts here, as in initD3D/initVulkan. The
        // first GL build forgot it, and the field log told the tale:
        // frames submitted happily, "0 presented", a white line and
        // black -- nobody was consuming.
        stop_preview_ = false;
        preview_thread_ = std::thread([this] { previewLoop(); });
        std::fprintf(stderr, "[wireplay] preview renderer: OpenGL (vsync off)\n");
        return true;
    }

    bool glReady() const { return gl_ready_; }

    // Runtime-load the GL2 entry points the Lanczos path needs and build
    // its program. Runs on the preview thread (context current there).
    bool glSetupLanczos() {
        if (gl_lz_ok_) return true;
        if (gl_lz_failed_) return false;
        auto load = [](const char* n) { return SDL_GL_GetProcAddress(n); };
        glp_create_shader_ = reinterpret_cast<unsigned int (*)(unsigned int)>(load("glCreateShader"));
        glp_shader_source_ = reinterpret_cast<void (*)(unsigned int, int, const char* const*, const int*)>(load("glShaderSource"));
        glp_compile_shader_ = reinterpret_cast<void (*)(unsigned int)>(load("glCompileShader"));
        glp_get_shaderiv_ = reinterpret_cast<void (*)(unsigned int, unsigned int, int*)>(load("glGetShaderiv"));
        glp_create_program_ = reinterpret_cast<unsigned int (*)()>(load("glCreateProgram"));
        glp_attach_shader_ = reinterpret_cast<void (*)(unsigned int, unsigned int)>(load("glAttachShader"));
        glp_link_program_ = reinterpret_cast<void (*)(unsigned int)>(load("glLinkProgram"));
        glp_get_programiv_ = reinterpret_cast<void (*)(unsigned int, unsigned int, int*)>(load("glGetProgramiv"));
        glp_use_program_ = reinterpret_cast<void (*)(unsigned int)>(load("glUseProgram"));
        glp_get_uniform_ = reinterpret_cast<int (*)(unsigned int, const char*)>(load("glGetUniformLocation"));
        glp_uniform1i_ = reinterpret_cast<void (*)(int, int)>(load("glUniform1i"));
        glp_uniform4f_ = reinterpret_cast<void (*)(int, float, float, float, float)>(load("glUniform4f"));
        glp_gen_framebuffers_ = reinterpret_cast<void (*)(int, unsigned int*)>(load("glGenFramebuffers"));
        glp_bind_framebuffer_ = reinterpret_cast<void (*)(unsigned int, unsigned int)>(load("glBindFramebuffer"));
        glp_framebuffer_texture2d_ = reinterpret_cast<void (*)(unsigned int, unsigned int, unsigned int, unsigned int, int)>(load("glFramebufferTexture2D"));
        if (!glp_create_shader_ || !glp_create_program_ || !glp_gen_framebuffers_ ||
            !glp_bind_framebuffer_ || !glp_framebuffer_texture2d_ || !glp_use_program_) {
            gl_lz_failed_ = true;
            std::fprintf(stderr, "[wireplay] OpenGL: no shader/FBO entry points; Lanczos off\n");
            return false;
        }
        static const char* kVs =
            "varying vec2 vUv;\n"
            "void main(){ vUv = gl_MultiTexCoord0.xy; "
            "gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex; }\n";
        static const char* kFs =
            "uniform sampler2D tex; uniform vec4 src; uniform vec4 ker;\n"
            "varying vec2 vUv;\n"
            "float lanczos(float x){ x = abs(x); if (x < 0.00001) return 1.0; "
            "if (x >= 3.0) return 0.0; float px = 3.14159265 * x; "
            "return (sin(px) / px) * (sin(px / 3.0) / (px / 3.0)); }\n"
            "void main(){\n"
            "  vec2 tc = vUv * src.xy - 0.5;\n"
            "  bool horiz = ker.y < 0.5;\n"
            "  float coord = horiz ? tc.x : tc.y;\n"
            "  float f = max(ker.x, 1.0); float sup = 3.0 * f;\n"
            "  float t0 = ceil(coord - sup), t1 = floor(coord + sup);\n"
            "  float lim = (horiz ? src.x : src.y) - 1.0;\n"
            "  float other = clamp((horiz ? tc.y : tc.x) + 0.5, 0.0, horiz ? src.y - 1.0 : src.x - 1.0);\n"
            "  vec4 acc = vec4(0.0); float ws = 0.0;\n"
            "  for (int k = 0; k < 40; ++k) {\n"
            "    float t = t0 + float(k); if (t > t1) break;\n"
            "    float w = lanczos((coord - t) / f); if (w == 0.0) continue;\n"
            "    float ti = clamp(t, 0.0, lim);\n"
            "    vec2 uv = horiz ? vec2((ti + 0.5) * src.z, (other + 0.5) * src.w)\n"
            "                    : vec2((other + 0.5) * src.z, (ti + 0.5) * src.w);\n"
            "    acc += texture2D(tex, uv) * w; ws += w;\n"
            "  }\n"
            "  gl_FragColor = ws > 0.0 ? acc / ws : texture2D(tex, vUv);\n"
            "}\n";
        auto build = [&](unsigned int type, const char* src) -> unsigned int {
            unsigned int sh = glp_create_shader_(type);
            glp_shader_source_(sh, 1, &src, nullptr);
            glp_compile_shader_(sh);
            int ok = 0;
            glp_get_shaderiv_(sh, 0x8B81 /*GL_COMPILE_STATUS*/, &ok);
            return ok ? sh : 0;
        };
        const unsigned int vs = build(0x8B31 /*GL_VERTEX_SHADER*/, kVs);
        const unsigned int fs = build(0x8B30 /*GL_FRAGMENT_SHADER*/, kFs);
        if (!vs || !fs) {
            gl_lz_failed_ = true;
            std::fprintf(stderr, "[wireplay] OpenGL: Lanczos shader compile failed\n");
            return false;
        }
        gl_lz_prog_ = glp_create_program_();
        glp_attach_shader_(gl_lz_prog_, vs);
        glp_attach_shader_(gl_lz_prog_, fs);
        glp_link_program_(gl_lz_prog_);
        int ok = 0;
        glp_get_programiv_(gl_lz_prog_, 0x8B82 /*GL_LINK_STATUS*/, &ok);
        if (!ok) {
            gl_lz_failed_ = true;
            std::fprintf(stderr, "[wireplay] OpenGL: Lanczos program link failed\n");
            return false;
        }
        gl_u_src_ = glp_get_uniform_(gl_lz_prog_, "src");
        gl_u_ker_ = glp_get_uniform_(gl_lz_prog_, "ker");
        const int u_tex = glp_get_uniform_(gl_lz_prog_, "tex");
        glp_use_program_(gl_lz_prog_);
        if (u_tex >= 0) glp_uniform1i_(u_tex, 0);
        glp_use_program_(0);
        glp_gen_framebuffers_(1, &gl_fbo_);
        glGenTextures(1, &gl_rz_tex_);
        glBindTexture(GL_TEXTURE_2D, gl_rz_tex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
        // The uploaded frame is fetched texel-exactly by the kernel.
        glBindTexture(GL_TEXTURE_2D, gl_tex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl_lz_ok_ = true;
        std::fprintf(stderr, "[wireplay] OpenGL: Lanczos-3 shader path active\n");
        return true;
    }

    // One textured quad with the preview's long-standing texcoord
    // assignment (v=1 on the bottom edge), shared by both Lanczos passes.
    void glDrawQuad(float x0, float y0, float x1, float y1) {
        glBegin(GL_TRIANGLE_FAN);
        glTexCoord2f(0.f, 1.f); glVertex2f(x0, y0);
        glTexCoord2f(1.f, 1.f); glVertex2f(x1, y0);
        glTexCoord2f(1.f, 0.f); glVertex2f(x1, y1);
        glTexCoord2f(0.f, 0.f); glVertex2f(x0, y1);
        glEnd();
    }

    void presentGL(AVFrame* fr) {
        if (!gl_ready_ || !window_) return;
        SDL_GL_MakeCurrent(window_, gl_ctx_);
        int win_w = 0, win_h = 0;
        SDL_GetWindowSize(window_, &win_w, &win_h);
        glViewport(0, 0, win_w, win_h);
        glClear(GL_COLOR_BUFFER_BIT);
        if (fr && fr->data[0] && fr->width > 0 && fr->height > 0) {
            glBindTexture(GL_TEXTURE_2D, gl_tex_);
            if (fr->width != gl_tex_w_ || fr->height != gl_tex_h_) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fr->width, fr->height, 0,
                             0x80E1 /*GL_BGRA*/, GL_UNSIGNED_BYTE, nullptr);
                gl_tex_w_ = fr->width;
                gl_tex_h_ = fr->height;
            }
            glPixelStorei(0x0CF2 /*GL_UNPACK_ROW_LENGTH*/,
                          fr->linesize[0] > 0 ? fr->linesize[0] / 4 : 0);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, fr->width, fr->height,
                            0x80E1 /*GL_BGRA*/, GL_UNSIGNED_BYTE, fr->data[0]);
            glPixelStorei(0x0CF2 /*GL_UNPACK_ROW_LENGTH*/, 0);
            if (paniso_ > 1) {
                glTexParameterf(GL_TEXTURE_2D, 0x84FE /*GL_TEXTURE_MAX_ANISOTROPY_EXT*/,
                                float(std::min(paniso_, 16)));
                if (gl_gen_mipmap_) gl_gen_mipmap_(GL_TEXTURE_2D);
            }
            if (scaling_ == Options::Scaling::Lanczos && glSetupLanczos()) {
                const SDL_Rect r = fitRect(fr->width, fr->height);
                if (r.w > 0 && r.h > 0) {
                    if (gl_rz_w_ != r.w || gl_rz_h_ != fr->height) {
                        glBindTexture(GL_TEXTURE_2D, gl_rz_tex_);
                        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, r.w, fr->height, 0,
                                     0x80E1 /*GL_BGRA*/, GL_UNSIGNED_BYTE, nullptr);
                        gl_rz_w_ = r.w;
                        gl_rz_h_ = fr->height;
                    }
                    glp_bind_framebuffer_(0x8D40 /*GL_FRAMEBUFFER*/, gl_fbo_);
                    glp_framebuffer_texture2d_(0x8D40, 0x8CE0 /*GL_COLOR_ATTACHMENT0*/,
                                               GL_TEXTURE_2D, gl_rz_tex_, 0);
                    glViewport(0, 0, r.w, fr->height);
                    glp_use_program_(gl_lz_prog_);
                    glBindTexture(GL_TEXTURE_2D, gl_tex_);
                    glp_uniform4f_(gl_u_src_, float(fr->width), float(fr->height),
                                   1.0f / float(fr->width), 1.0f / float(fr->height));
                    glp_uniform4f_(gl_u_ker_, float(fr->width) / float(r.w), 0.f, 0.f, 0.f);
                    glDrawQuad(-1.f, -1.f, 1.f, 1.f);
                    glp_bind_framebuffer_(0x8D40 /*GL_FRAMEBUFFER*/, 0);
                    glViewport(0, 0, win_w, win_h);
                    glClear(GL_COLOR_BUFFER_BIT);
                    glBindTexture(GL_TEXTURE_2D, gl_rz_tex_);
                    glp_uniform4f_(gl_u_src_, float(r.w), float(fr->height),
                                   1.0f / float(r.w), 1.0f / float(fr->height));
                    glp_uniform4f_(gl_u_ker_, float(fr->height) / float(r.h), 1.f, 0.f, 0.f);
                    const float x0 = 2.f * float(r.x) / float(win_w) - 1.f;
                    const float x1 = 2.f * float(r.x + r.w) / float(win_w) - 1.f;
                    const float y1 = 1.f - 2.f * float(r.y) / float(win_h);
                    const float y0 = 1.f - 2.f * float(r.y + r.h) / float(win_h);
                    glDrawQuad(x0, y0, x1, y1);
                    glp_use_program_(0);
                    SDL_GL_SwapWindow(window_);
                    return;
                }
            }
            if (!gl_logged_) {
                gl_logged_ = true;
                std::fprintf(stderr,
                             "[wireplay] GL first frame: %dx%d linesize=%d tex=%u glError=0x%x win=%dx%d\n",
                             fr->width, fr->height, fr->linesize[0], gl_tex_,
                             unsigned(glGetError()), win_w, win_h);
            }
            const SDL_Rect r = fitRect(fr->width, fr->height);
            const float x0 = 2.f * float(r.x) / float(win_w) - 1.f;
            const float x1 = 2.f * float(r.x + r.w) / float(win_w) - 1.f;
            const float y1 = 1.f - 2.f * float(r.y) / float(win_h);
            const float y0 = 1.f - 2.f * float(r.y + r.h) / float(win_h);
            glEnable(GL_TEXTURE_2D);
            glBegin(GL_TRIANGLE_FAN);
            glTexCoord2f(0.f, 1.f); glVertex2f(x0, y0);
            glTexCoord2f(1.f, 1.f); glVertex2f(x1, y0);
            glTexCoord2f(1.f, 0.f); glVertex2f(x1, y1);
            glTexCoord2f(0.f, 0.f); glVertex2f(x0, y1);
            glEnd();
            glDisable(GL_TEXTURE_2D);
        }
        SDL_GL_SwapWindow(window_);
    }
#else
    bool glReady() const { return false; }
#endif

    bool vulkanReady() const {
#if defined(WIREPLAY_HAVE_VULKAN)
        return vk_ready_;
#else
        return false;
#endif
    }

    void message(const std::string& m) {
        if (m == overlay_msg_) return;
        overlay_msg_ = m;
        // Once video is flowing the status text must not touch the screen:
        // presenting an overlay-only frame replaces the picture for a vsync,
        // which the user sees as a white blink on every engine log line. The
        // message still goes to stderr; the overlay is for the waiting screen.
        if (texture_ || ((d3d_ready_ || d3d12_ready_) && width_ > 0)) return;
#ifdef _WIN32
        if (d3d12_ready_) { presentBlank12(); return; }
        if (d3d_ready_) { presentBlank(); return; }
#endif
        if (renderer_ && window_) {
            SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
            SDL_RenderClear(renderer_);
            drawOverlay();
            SDL_RenderPresent(renderer_);
        }
    }

    // Events must be pumped or the window stops repainting and the OS marks
    // the process hung. Returns false when the user asked to quit.
    bool pumpEvents() {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) return false;
            if (ev.type == SDL_KEYDOWN) {
                switch (ev.key.keysym.sym) {
                case SDLK_ESCAPE: return false;
                case SDLK_F11: {
                    const Uint32 f = SDL_GetWindowFlags(window_);
                    SDL_SetWindowFullscreen(window_, (f & SDL_WINDOW_FULLSCREEN_DESKTOP)
                                                         ? 0
                                                         : SDL_WINDOW_FULLSCREEN_DESKTOP);
                    break;
                }
                case SDLK_k: key_request_ = true; break;
                case SDLK_r: restart_request_ = true; break;
                case SDLK_F8:
                    // Preview pause: the decoder and the virtual camera
                    // keep receiving full-resolution frames; only the
                    // on-screen presentation stops (which also hands the
                    // vsync wait back to the stream loop). For streaming
                    // to OBS the window is pure overhead, so this is the
                    // performance mode, not a sleep mode.
                    preview_paused_ = !preview_paused_;
                    if (preview_paused_) showPreviewPaused();
                    break;
                default: break;
                }
            }
            if (ev.type == SDL_WINDOWEVENT &&
                ev.window.event == SDL_WINDOWEVENT_RESTORED) {
                // Un-maximising restores the pre-maximise geometry, which
                // may belong to the other orientation (maximised in
                // portrait, rotated to landscape inside a game, then
                // un-maximised into a portrait-shaped window). The main
                // loop reapplies the placement rule for the current
                // stream geometry when it sees this flag.
                restore_request_ = true;
            }
        }
        return true;
    }

    bool previewPaused() const { return preview_paused_; }
    bool consumeRestoreRequest() {
        const bool r = restore_request_;
        restore_request_ = false;
        return r;
    }

    // Re-run the placement rule for the geometry already on screen.
    void refreshGeometry() {
        if (width_ <= 0 || height_ <= 0) return;
        const int w = width_, h = height_;
        width_ = 0;
        height_ = 0;
        setGeometry(w, h);
    }

    void showPreviewPaused() {
        if (!renderer_ && !d3d_ready_ && !d3d12_ready_) return;
        overlay_msg_ = "Preview pausado - F8 para continuar";
#ifdef _WIN32
        if (d3d_ready_ || d3d12_ready_) { blank_dirty_ = true; return; }
#endif
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        SDL_RenderClear(renderer_);
        drawOverlay();
        SDL_RenderPresent(renderer_);
    }

    // ---- zero-copy D3D11 presentation (Windows) -------------------------
    bool d3dReady() const { return d3d_ready_; }
    bool d3d12Ready() const { return d3d12_ready_; }
    void* d3dDevice() const {
#ifdef _WIN32
        return d3d_device_;
#else
        return nullptr;
#endif
    }
    void* d3dContext() const {
#ifdef _WIN32
        return d3d_ctx_;
#else
        return nullptr;
#endif
    }

    // Camera effects pass (camera thread): run one BGRA frame through
    // the same output stage the preview samples -- mip chain +
    // anisotropic sampler when --aniso is on, MSAA render target +
    // resolve when --msaa is on -- at NATIVE resolution, and read the
    // result back into `out`. Returns false (caller publishes the
    // untouched frame) when D3D11 is not the active path. At 1:1 the
    // visual delta is deliberately small -- the camera's real filtering
    // happens in OBS's downscale -- which is exactly why
    // --no-camera-effects exists to skip this cost.
    bool fxBgra(const AVFrame* in, AVFrame* out) {
#ifdef _WIN32
        if (!d3d_ready_ || !in || !out || !in->data[0] ||
            in->format != AV_PIX_FMT_BGRA || in->width <= 0 || in->height <= 0)
            return false;
        std::lock_guard<std::recursive_mutex> fxlock(fx_mutex_);
        const int w = in->width, h = in->height;
        if (!fx_in_ || fx_w_ != w || fx_h_ != h) {
            auto rel = [](auto*& r) { if (r) { r->Release(); r = nullptr; } };
            rel(fx_in_srv_); rel(fx_in_); rel(fx_out_rtv_); rel(fx_out_);
            rel(fx_msaa_rtv_); rel(fx_msaa_); rel(fx_read_);
            fx_w_ = fx_h_ = 0;
            D3D11_TEXTURE2D_DESC td {};
            td.Width = UINT(w); td.Height = UINT(h);
            td.MipLevels = aniso_ > 1 ? 0 : 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE |
                           (aniso_ > 1 ? UINT(D3D11_BIND_RENDER_TARGET) : 0u);
            if (FAILED(d3d_device_->CreateTexture2D(&td, nullptr, &fx_in_))) return false;
            if (FAILED(d3d_device_->CreateShaderResourceView(fx_in_, nullptr, &fx_in_srv_))) return false;
            td.MipLevels = 1;
            td.BindFlags = D3D11_BIND_RENDER_TARGET;
            if (FAILED(d3d_device_->CreateTexture2D(&td, nullptr, &fx_out_))) return false;
            if (FAILED(d3d_device_->CreateRenderTargetView(fx_out_, nullptr, &fx_out_rtv_))) return false;
            if (msaa_ > 1) {
                int chosen = 0;
                for (int cand : {16, 8, 4, 2}) {
                    UINT ql = 0;
                    if (cand <= msaa_ &&
                        SUCCEEDED(d3d_device_->CheckMultisampleQualityLevels(
                            DXGI_FORMAT_B8G8R8A8_UNORM, UINT(cand), &ql)) && ql > 0) { chosen = cand; break; }
                }
                if (chosen > 1) {
                    td.SampleDesc.Count = UINT(chosen);
                    if (SUCCEEDED(d3d_device_->CreateTexture2D(&td, nullptr, &fx_msaa_)))
                        d3d_device_->CreateRenderTargetView(fx_msaa_, nullptr, &fx_msaa_rtv_);
                }
            }
            td.SampleDesc.Count = 1;
            td.BindFlags = 0;
            td.Usage = D3D11_USAGE_STAGING;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(d3d_device_->CreateTexture2D(&td, nullptr, &fx_read_))) return false;
            fx_w_ = w; fx_h_ = h;
        }
        if (!fx_in_ || !fx_in_srv_ || !fx_out_ || !fx_out_rtv_ || !fx_read_) return false;
        d3d_ctx_->UpdateSubresource(fx_in_, 0, nullptr, in->data[0], UINT(in->linesize[0]), 0);
        if (aniso_ > 1) d3d_ctx_->GenerateMips(fx_in_srv_);
        ID3D11RenderTargetView* target = fx_msaa_rtv_ ? fx_msaa_rtv_ : fx_out_rtv_;
        d3d_ctx_->OMSetRenderTargets(1, &target, nullptr);
        D3D11_VIEWPORT vp {};
        vp.Width = float(w); vp.Height = float(h); vp.MaxDepth = 1.0f;
        d3d_ctx_->RSSetViewports(1, &vp);
        d3d_ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        d3d_ctx_->VSSetShader(vs_, nullptr, 0);
        d3d_ctx_->PSSetShader(ps_bgra_, nullptr, 0);
        d3d_ctx_->PSSetSamplers(0, 1, &samp_);
        ID3D11ShaderResourceView* srv = fx_in_srv_;
        d3d_ctx_->PSSetShaderResources(2, 1, &srv);
        setParams(0, 0, 0, 0, -1.0f, 1.0f, 2.0f, 2.0f);  // fullscreen rect
        d3d_ctx_->Draw(4, 0);
        ID3D11ShaderResourceView* none[1] = {nullptr};
        d3d_ctx_->PSSetShaderResources(2, 1, none);
        if (fx_msaa_rtv_)
            d3d_ctx_->ResolveSubresource(fx_out_, 0, fx_msaa_, 0, DXGI_FORMAT_B8G8R8A8_UNORM);
        d3d_ctx_->CopyResource(fx_read_, fx_out_);
        D3D11_MAPPED_SUBRESOURCE map {};
        if (FAILED(d3d_ctx_->Map(fx_read_, 0, D3D11_MAP_READ, 0, &map))) return false;
        if (out->width != w || out->height != h || out->format != AV_PIX_FMT_BGRA) {
            av_frame_unref(out);
            out->format = AV_PIX_FMT_BGRA; out->width = w; out->height = h;
            if (av_frame_get_buffer(out, 32) < 0) { d3d_ctx_->Unmap(fx_read_, 0); return false; }
        }
        const int row = w * 4;
        for (int y = 0; y < h; ++y)
            std::memcpy(out->data[0] + size_t(y) * out->linesize[0],
                        static_cast<const uint8_t*>(map.pData) + size_t(y) * map.RowPitch,
                        size_t(row));
        d3d_ctx_->Unmap(fx_read_, 0);
        return true;
#else
        (void)in; (void)out;
        return false;
#endif
    }

    // Hand a decoded frame to the preview thread. The frame is
    // referenced (not copied), so the decoder may immediately reuse its
    // own surface. This is a small FIFO, NOT a latest-wins slot: the
    // previous single slot silently overwrote frames that arrived
    // while the preview thread was holding one for its PTS slot --
    // with the encoder's bursty delivery, one frame per burst vanished
    // (field: ~2.6% of decoded frames never presented, a hole in the
    // cadence every ~0.6 s, perceived as the standing "pulse"). Six
    // deep (~100 ms at 60 fps) absorbs bursts behind the cushion;
    // beyond that the OLDEST is dropped (bounded staleness) and the
    // loss is counted, never silent.
    void stashPending(const AVFrame* fr) {
        if (!fr || !fr->data[0]) return;
        AVFrame* slot = av_frame_alloc();
        if (!slot) return;
        if (av_frame_ref(slot, fr) != 0) { av_frame_free(&slot); return; }
        // The arrival stamp is what the scheduler's offset is measured
        // from -- never the pop time. Measuring at pop once created a
        // feedback loop: a bigger target made frames wait longer in
        // this queue, the longer wait read as a bigger "arrival
        // offset", and the target chased its own tail to 100+ ms
        // while the queue overflowed (field: 840 frames dropped in
        // one landscape session). Arrival is exogenous; pop is not.
        const auto arrived = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_queue_.push_back(StashItem{slot, arrived});
        while (pending_queue_.size() > 18) {
            AVFrame* old = pending_queue_.front().frame;
            pending_queue_.pop_front();
            av_frame_free(&old);
            ++stash_drops_;
        }
    }
    uint64_t stashDrops() const { return stash_drops_.load(); }

    void submitGpuFrame(const AVFrame* gpu) {
#if defined(_WIN32) || defined(WIREPLAY_HAVE_VULKAN) || defined(WIREPLAY_HAVE_GL)
        if (!d3dReady() && !d3d12Ready() && !vulkanReady() && !glReady()) return;
        stashPending(gpu);
#else
        (void)gpu;
#endif
    }

    // The decoder's D3D11VA lock: GPU->CPU transfers on the preview and
    // camera threads serialise on it (FFmpeg's lock callbacks point here
    // when the decoder shares the renderer's D3D11 device).
    void setHwMutex(std::recursive_mutex* m) { hw_mutex_ = m; }

#if defined(_WIN32) || defined(WIREPLAY_HAVE_VULKAN) || defined(WIREPLAY_HAVE_GL)
    // Bring one hardware frame home to BGRA on the PREVIEW thread:
    // av_hwframe_transfer_data + sws_scale for a 2688x1242 frame cost
    // ~14 ms, and when that ran on the stream thread (the old forced
    // cpu-copy in GL/Vulkan/CUDA modes) the pipe backed up behind it --
    // bursts, jitter, standing delay, audio crackle. Here it can only
    // ever cost the preview its own cadence. Returns a View-owned frame
    // valid until the next call, or nullptr on failure.
    AVFrame* hwToBgra(AVFrame* f) {
        if (!f || !f->hw_frames_ctx) return nullptr;
        std::unique_lock<std::recursive_mutex> lock;
        if (hw_mutex_) lock = std::unique_lock<std::recursive_mutex>(*hw_mutex_);
        if (!pv_stage_) pv_stage_ = av_frame_alloc();
        if (!pv_bgra_) pv_bgra_ = av_frame_alloc();
        if (!pv_stage_ || !pv_bgra_) return nullptr;
        av_frame_unref(pv_stage_);
        if (av_hwframe_transfer_data(pv_stage_, f, 0) < 0) return nullptr;
        AVFrame* src = pv_stage_;
        if (src->format == AV_PIX_FMT_BGRA) return src;
        if (!pv_sws_ || pv_sws_w_ != src->width || pv_sws_h_ != src->height ||
            pv_sws_fmt_ != AVPixelFormat(src->format)) {
            if (pv_sws_) sws_freeContext(pv_sws_);
            pv_sws_ = sws_getContext(src->width, src->height,
                                     AVPixelFormat(src->format),
                                     src->width, src->height, AV_PIX_FMT_BGRA,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
            pv_sws_w_ = src->width;
            pv_sws_h_ = src->height;
            pv_sws_fmt_ = AVPixelFormat(src->format);
        }
        if (!pv_sws_) return nullptr;
        av_frame_unref(pv_bgra_);
        pv_bgra_->format = AV_PIX_FMT_BGRA;
        pv_bgra_->width = src->width;
        pv_bgra_->height = src->height;
        if (av_frame_get_buffer(pv_bgra_, 32) < 0) return nullptr;
        sws_scale(pv_sws_, src->data, src->linesize, 0, src->height,
                  pv_bgra_->data, pv_bgra_->linesize);
        return pv_bgra_;
    }
#endif

    void submitCpuFrame(const AVFrame* fr) {
#ifdef _WIN32
        if (d3d_ready_ || d3d12_ready_) { stashPending(fr); return; }
#endif
#if defined(WIREPLAY_HAVE_VULKAN) || defined(WIREPLAY_HAVE_GL)
        if (!vulkanReady() && !glReady()) return;
        stashPending(fr);
#else
        (void)fr;
#endif
    }


#if defined(_WIN32) || defined(WIREPLAY_HAVE_VULKAN) || defined(WIREPLAY_HAVE_GL)
    // Arrival-driven presentation, deliberately: every decoded frame is
    // presented exactly once, as soon as it lands -- and nothing else is
    // ever presented. An earlier experiment re-presented the held frame
    // on a fixed display cadence ("frame pacing" by duplication); the
    // user rejected it in the field, correctly: duplicated frames are
    // fake frames, and a 45 Hz source duplicated onto a high-refresh
    // cadence judders worse than honest 45. No artificial fill, ever.
    void previewLoop() {
        bool was_paused = false;
        while (!stop_preview_) {
            // A minimized window has no compositor slot to present
            // into: Present() can sit blocked for the whole minimized
            // interval (worse with maximum frame latency 1), freezing
            // this thread while decode flows on -- the field saw
            // `presented` frozen for 8 s under a Streamlabs window.
            // Just wait it out; the camera path is main-thread and
            // unaffected, and the newest frame wins on restore.
            if (window_ && (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            if (preview_paused_) {
                if (!was_paused || blank_dirty_) {
#ifdef _WIN32
                    if (d3d12_ready_) presentBlank12();
                    else if (d3d_ready_) presentBlank();
#endif
#if defined(WIREPLAY_HAVE_VULKAN)
                    if (vk_ready_) presentVulkan(nullptr);
#endif
#ifdef WIREPLAY_HAVE_GL
                    if (gl_ready_) presentGL(nullptr);
#endif
                    blank_dirty_ = false;
                }
                was_paused = true;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            was_paused = false;
            AVFrame* local = nullptr;
            std::chrono::steady_clock::time_point arrived {};
            {
                std::lock_guard<std::mutex> lock(pending_mutex_);
                if (!pending_queue_.empty()) {
                    local = pending_queue_.front().frame;
                    arrived = pending_queue_.front().arrived;
                    pending_queue_.pop_front();
                }
            }
            if (!local) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            // (--present arrival skips this block entirely.)
            // Present at the frame's own stream time (its PTS), not at
            // its arrival time. The phone's encoder is variable-rate: it
            // pauses, then releases a burst -- and on arrival-driven
            // presentation the burst hits the screen as a burst (the
            // user's exact field model of the late-session jitter).
            // Anchoring the first PTS to the wall clock and presenting
            // each real frame when its stream time comes spreads those
            // bursts back over their true cadence. Late frames present
            // immediately; a frame newer than the one being held
            // replaces it, so the added delay is bounded by one frame
            // interval; nothing is ever duplicated (permanent rule:
            // no artificial frames). Mirrors UxPlay's default
            // timestamp-based `-vsync` behaviour.
            bool have_deadline = false;
            std::chrono::steady_clock::time_point deadline {};
            if (present_pts_) {
                using namespace std::chrono;
                auto dueFor = [&](const AVFrame* f, steady_clock::time_point now,
                                  std::chrono::steady_clock::time_point arrived_tp,
                                  bool force = false) {
                    const int64_t ts = f->best_effort_timestamp;
                    if (ts == AV_NOPTS_VALUE) return now;
                    // The stream timebase is 90-kilohertz BY CONSTRUCTION
                    // (the wire carries 90-kilohertz PTS and the decoder's
                    // pkt_timebase is {1,90000}). AVFrame::time_base
                    // must NOT be consulted: FFmpeg leaves it at 0/1
                    // on decoded frames (probe-verified against the
                    // system FFmpeg: best_effort advances 0,1500,3000
                    // while time_base stays 0/1), and the old guard on
                    // it early-returned `now` for every frame -- every
                    // PTS-scheduling feature (cushion, adaptive
                    // target) was dead code and presentation was
                    // silently arrival-driven the whole time.
                    const double secs = double(ts) / 90000.0;
                    // Deadline = this frame's stream instant + a
                    // target offset that tracks the ARRIVAL offset
                    // (pop time minus stream instant). Forensics:
                    // PTS perfectly even (pts_holes=0) yet ~3% of
                    // frames pop >150 ms after their stream instant
                    // relative to the first frame -- the pipeline's
                    // delivery delay itself wanders (the encoder
                    // holds, then dumps: the user's bucket). A fixed
                    // cushion anchored on the first frame's luck can
                    // never cover a wandering delay; the target
                    // follows the measured worst offset (instant up,
                    // slow decay to the --cushion floor), so each
                    // frame is presented on cadence with only the
                    // margin the current stream behaviour requires.
                    if (force || !pts_anchor_set_) {
                        pts_anchor_set_ = true;
                        pts_anchor_secs_ = secs;
                        pts_wall_anchor_ = now;
                        if (force) smoothed_set_ = false;
                    }
                    double rel = secs - pts_anchor_secs_;
                    if (rel < -0.25 || rel > 3600.0) {  // stream clock jumped: re-anchor
                        pts_anchor_secs_ = secs;
                        pts_wall_anchor_ = now;
                        rel = 0.0;
                    }
                    const auto stream_instant =
                        pts_wall_anchor_ +
                        duration_cast<steady_clock::duration>(duration<double>(rel));
                    if (!force) {
                        // Offset of ARRIVAL vs stream instant (both
                        // wall clock via the anchor): exogenous, so
                        // the adaptation cannot feed on its own queue.
                        // The deadline tracks a SMOOTHED offset, not
                        // the worst one: the device clock and the
                        // wall clock drift against each other (field:
                        // ~0.3 ms/s), so a fixed anchor falls behind
                        // progressively over a session -- spikes that
                        // grow the longer you play. An EMA follows
                        // slow drift continuously (slow alpha), jumps
                        // faster toward sudden lateness (fast alpha),
                        // and the only latency added on top is the
                        // --cushion floor.
                        const double offset_ms =
                            duration<double, std::milli>(arrived_tp - stream_instant)
                                .count();
                        if (!smoothed_set_) {
                            smoothed_offset_ms_ = offset_ms;
                            smoothed_set_ = true;
                        } else {
                            const double alpha =
                                offset_ms > smoothed_offset_ms_ ? 0.2 : 0.02;
                            smoothed_offset_ms_ +=
                                (offset_ms - smoothed_offset_ms_) * alpha;
                        }
                        std::lock_guard<std::mutex> lock(pres_mutex_);
                        off_sum_ms_ += offset_ms;
                        if (offset_ms > off_max_ms_) off_max_ms_ = offset_ms;
                        ++off_n_;
                    }
                    return stream_instant + duration_cast<steady_clock::duration>(
                                                duration<double, std::milli>(
                                                    smoothed_offset_ms_ +
                                                    cushion_floor_ms_));
                };
                deadline = dueFor(local, steady_clock::now(), arrived);
                have_deadline = true;
                if (deadline < steady_clock::now() - milliseconds(120)) {
                    // Hopelessly behind (upstream stall, rotation):
                    // re-anchor on this frame rather than dumping the
                    // whole backlog back-to-back, which reads as one
                    // big pulse after every stall.
                    deadline = dueFor(local, steady_clock::now(), arrived, true);
                }
                while (!stop_preview_ && !preview_paused_ &&
                       steady_clock::now() < deadline) {
                    const auto wait = deadline - steady_clock::now();
                    if (wait > milliseconds(500)) {
                        // Untrustworthy clock (PTS jump): re-anchor on
                        // this frame. This used to `break` at >50 ms,
                        // which silently capped every cushion at 50 ms
                        // and presented immediately -- the reason
                        // --cushion 34 and 256 felt identical to 0.
                        deadline = dueFor(local, steady_clock::now(), arrived, true);
                    }
                    // Deliberately NOT replaced by a newer arrival while
                    // waiting. The first version swapped the held frame
                    // for the newcomer, which read as "freshest wins"
                    // but dropped ~8% of frames in the field (5208
                    // decoded, 4796 presented): whenever the thread's
                    // slip crossed a frame boundary -- about twice a
                    // second -- one frame silently never reached the
                    // screen, a periodic hitch in an otherwise smooth
                    // cadence. The held frame is at most one interval
                    // from its slot; present it on time and let the
                    // newcomer wait its own turn in the stash.
                    std::this_thread::sleep_for(milliseconds(1));
                }
                if (preview_paused_) {  // paused mid-wait: hand the frame back
                    std::lock_guard<std::mutex> lock(pending_mutex_);
                    pending_queue_.push_front(StashItem{local, arrived});
                    continue;
                }
            }
            // Hardware frames that cannot be sampled where they live
            // (CUDA surfaces anywhere; D3D11 surfaces behind the
            // GL/Vulkan previews) become BGRA here, on the preview
            // thread -- never on the stream thread.
            AVFrame* show = local;
            if (local->hw_frames_ctx) {
#ifdef _WIN32
                const bool gpu_route = local->format == AV_PIX_FMT_D3D11 &&
                                       (d3d_ready_ || d3d12_ready_);
#else
                const bool gpu_route = false;
#endif
                if (!gpu_route) {
                    if (AVFrame* bgra = hwToBgra(local)) show = bgra;
                }
            }
#ifdef WIREPLAY_HAVE_GL
            if (gl_ready_) {
                presentGL(show);
                ++presented_;
            } else
#endif
#if defined(WIREPLAY_HAVE_VULKAN)
            if (vk_ready_) {
                presentVulkan(show);
                ++presented_;
            } else
#endif
#ifdef _WIN32
            if (d3d12_ready_) {
                if (show->format == AV_PIX_FMT_D3D11) {
                    presentGpu12(show->data[0],
                                 int(reinterpret_cast<intptr_t>(show->data[1])),
                                 show->width, show->height);
                } else {
                    presentCpu12(show);
                }
            } else {
                if (show->format == AV_PIX_FMT_D3D11) {
                    presentGpu(show->data[0],
                               int(reinterpret_cast<intptr_t>(show->data[1])),
                               show->width, show->height);
                } else {
                    // CPU frame (converted above, software decode, or
                    // NVDEC/CUDA brought home by hwToBgra): upload +
                    // sample. Without this branch a D3D11 window showed
                    // a black screen and presented 0 frames in CUDA mode.
                    presentCpu(show);
                }
            }
#else
            {
            }
#endif
            {
                double pts_secs = -1.0;
                const int64_t ts = local->best_effort_timestamp;
                if (ts != AV_NOPTS_VALUE) pts_secs = double(ts) / 90000.0;
                notePresented(std::chrono::steady_clock::now(), have_deadline, deadline,
                              pts_secs);
            }
            av_frame_free(&local);
        }
        std::lock_guard<std::mutex> lock(pending_mutex_);
        while (!pending_queue_.empty()) {
            AVFrame* f = pending_queue_.front().frame;
            pending_queue_.pop_front();
            av_frame_free(&f);
        }
    }
#endif

    // Present one decoded hardware frame: `texture` is the ID3D11Texture2D*
    // of the decoder's NV12 surface array, `slice` its array index. No CPU
    // copy of the pixels happens anywhere in this path.
    bool presentGpu(void* texture, int slice, int w, int h) {
#ifdef _WIN32
        if (!d3d_ready_ || !texture || w <= 0 || h <= 0) return false;
        std::lock_guard<std::recursive_mutex> fxlock(fx_mutex_);
        if (!ensureBackBuffer()) return false;
        auto* tex = static_cast<ID3D11Texture2D*>(texture);
        D3D11_TEXTURE2D_DESC td {};
        tex->GetDesc(&td);
        if (td.Format != DXGI_FORMAT_NV12) return false;  // P010 etc.: CPU path

        // Decoder surfaces are created with D3D11_BIND_DECODER and cannot
        // be sampled. Copy the slice -- GPU to GPU, never through the CPU --
        // into our own shader-resource texture, then sample that.
        if (!stage_ || stage_w_ != w || stage_h_ != h || stage_fmt_ != td.Format) {
            for (auto*& srv : stage_srv_) { if (srv) { srv->Release(); srv = nullptr; } }
            if (stage_) { stage_->Release(); stage_ = nullptr; }
            D3D11_TEXTURE2D_DESC sd2 {};
            sd2.Width = UINT(w);
            sd2.Height = UINT(h);
            sd2.ArraySize = 1;
            sd2.Format = td.Format;
            sd2.SampleDesc.Count = 1;
            sd2.Usage = D3D11_USAGE_DEFAULT;
            // --aniso needs a mip chain to filter across; building one on
            // an NV12 texture needs RENDER_TARGET too, which some drivers
            // refuse for NV12 -- fall back to the single-level texture
            // (historical behaviour) instead of failing the preview.
            bool want_mips = paniso_ > 1;
            sd2.MipLevels = want_mips ? 0 : 1;
            sd2.BindFlags = D3D11_BIND_SHADER_RESOURCE |
                            (want_mips ? UINT(D3D11_BIND_RENDER_TARGET) : 0u);
            if (FAILED(d3d_device_->CreateTexture2D(&sd2, nullptr, &stage_))) {
                if (!want_mips) return false;
                sd2.MipLevels = 1;
                sd2.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                want_mips = false;
                if (FAILED(d3d_device_->CreateTexture2D(&sd2, nullptr, &stage_)))
                    return false;
                std::fprintf(stderr,
                             "[wireplay] D3D11: NV12 mip chain refused; aniso preview off\n");
            }
            stage_mips_ = want_mips;
            stage_w_ = w;
            stage_h_ = h;
            stage_fmt_ = td.Format;
            // The staging texture is fixed for a given stream geometry,
            // so its two plane SRVs are created once here and reused --
            // they used to be created and destroyed on every frame.
            D3D11_SHADER_RESOURCE_VIEW_DESC sv {};
            sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sv.Texture2D.MipLevels = stage_mips_ ? UINT(-1) : 1;
            sv.Format = DXGI_FORMAT_R8_UNORM;
            if (FAILED(d3d_device_->CreateShaderResourceView(stage_, &sv, &stage_srv_[0])))
                return false;
            sv.Format = DXGI_FORMAT_R8G8_UNORM;
            if (FAILED(d3d_device_->CreateShaderResourceView(stage_, &sv, &stage_srv_[1])))
                return false;
        }
        d3d_ctx_->CopySubresourceRegion(stage_, 0, 0, 0, 0, tex, UINT(slice), nullptr);
        if (stage_mips_) {
            d3d_ctx_->GenerateMips(stage_srv_[0]);
            d3d_ctx_->GenerateMips(stage_srv_[1]);
        }
        ID3D11ShaderResourceView* const srvs[2] = {stage_srv_[0], stage_srv_[1]};
        if (!srvs[0] || !srvs[1]) return false;

        if (scaling_ == Options::Scaling::Lanczos) {
            const double lsc = std::min(double(bb_w_) / w, double(bb_h_) / h);
            const int ldw = std::max(1, int(w * lsc)), ldh = std::max(1, int(h * lsc));
            const float lrx = 2.0f * ((bb_w_ - ldw) / 2) / bb_w_ - 1.0f;
            const float lry = 1.0f - 2.0f * ((bb_h_ - ldh) / 2) / bb_h_;
            const float lrw = 2.0f * ldw / bb_w_, lrh = 2.0f * ldh / bb_h_;
            if (drawLanczosNv12(ldw, ldh, lrx, lry, lrw, lrh, w, h)) {
                presentSwap();
                ++presented_;
                return true;
            }
        }

        const float black[4] = {0, 0, 0, 1};
        ID3D11RenderTargetView* target = drawTarget();
        d3d_ctx_->OMSetRenderTargets(1, &target, nullptr);
        d3d_ctx_->ClearRenderTargetView(target, black);
        D3D11_VIEWPORT vp {};
        vp.Width = float(bb_w_);
        vp.Height = float(bb_h_);
        vp.MaxDepth = 1.0f;
        d3d_ctx_->RSSetViewports(1, &vp);
        // Letterbox destination as an NDC rectangle for the shader.
        const double scale = std::min(double(bb_w_) / w, double(bb_h_) / h);
        const int dw = std::max(1, int(w * scale)), dh = std::max(1, int(h * scale));
        const float rx = 2.0f * ((bb_w_ - dw) / 2) / bb_w_ - 1.0f;
        const float ry = 1.0f - 2.0f * ((bb_h_ - dh) / 2) / bb_h_;
        const float rw = 2.0f * dw / bb_w_, rh = 2.0f * dh / bb_h_;
        d3d_ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        d3d_ctx_->VSSetShader(vs_, nullptr, 0);
        d3d_ctx_->PSSetShader(ps_nv12_, nullptr, 0);
        d3d_ctx_->PSSetSamplers(0, 1, &samp_);
        d3d_ctx_->PSSetShaderResources(0, 2, srvs);
        setParams(float(slice), 0, 0, 0, rx, ry, rw, rh);
        d3d_ctx_->Draw(4, 0);
        ID3D11ShaderResourceView* none[2] = {nullptr, nullptr};
        d3d_ctx_->PSSetShaderResources(0, 2, none);
        presentSwap();  // resolves MSAA if active, then Present(0,0):
                        // no vsync: the stream is the clock, and a
                               // vsync wait only adds latency (and hung
                               // the preview thread outright in the field)
        ++presented_;
        return true;
#else
        (void)texture; (void)slice; (void)w; (void)h;
        return false;
#endif
    }

    // Present one CPU BGRA frame on the D3D11 window: upload into a
    // shader-resource texture, sample it 1:1. This is the display path
    // for --decoder cuda (NVDEC frames return via CPU) and for the
    // software fallback decoder.
    bool presentCpu(const AVFrame* f) {
#ifdef _WIN32
        if (!d3d_ready_ || !f || !f->data[0] || f->format != AV_PIX_FMT_BGRA ||
            f->width <= 0 || f->height <= 0)
            return false;
        std::lock_guard<std::recursive_mutex> fxlock(fx_mutex_);
        if (!ensureBackBuffer()) return false;
        const int w = f->width, h = f->height;
        if (!cpu_stage_ || cpu_stage_w_ != w || cpu_stage_h_ != h) {
            if (cpu_srv_) { cpu_srv_->Release(); cpu_srv_ = nullptr; }
            if (cpu_stage_) { cpu_stage_->Release(); cpu_stage_ = nullptr; }
            D3D11_TEXTURE2D_DESC td {};
            td.Width = UINT(w);
            td.Height = UINT(h);
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            bool want_mips = paniso_ > 1;
            td.MipLevels = want_mips ? 0 : 1;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE |
                           (want_mips ? UINT(D3D11_BIND_RENDER_TARGET) : 0u);
            if (FAILED(d3d_device_->CreateTexture2D(&td, nullptr, &cpu_stage_))) {
                if (!want_mips) return false;
                td.MipLevels = 1;
                td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                want_mips = false;
                if (FAILED(d3d_device_->CreateTexture2D(&td, nullptr, &cpu_stage_)))
                    return false;
            }
            cpu_mips_ = want_mips;
            if (FAILED(d3d_device_->CreateShaderResourceView(cpu_stage_, nullptr,
                                                             &cpu_srv_)))
                return false;
            cpu_stage_w_ = w;
            cpu_stage_h_ = h;
        }
        d3d_ctx_->UpdateSubresource(cpu_stage_, 0, nullptr, f->data[0],
                                    UINT(f->linesize[0]), 0);
        if (cpu_mips_ && cpu_srv_) d3d_ctx_->GenerateMips(cpu_srv_);
        if (scaling_ == Options::Scaling::Lanczos && cpu_srv_) {
            const double lsc = std::min(double(bb_w_) / w, double(bb_h_) / h);
            const int ldw = std::max(1, int(w * lsc)), ldh = std::max(1, int(h * lsc));
            const float lrx = 2.0f * ((bb_w_ - ldw) / 2) / bb_w_ - 1.0f;
            const float lry = 1.0f - 2.0f * ((bb_h_ - ldh) / 2) / bb_h_;
            const float lrw = 2.0f * ldw / bb_w_, lrh = 2.0f * ldh / bb_h_;
            if (drawLanczosBgra(cpu_srv_, ldw, ldh, lrx, lry, lrw, lrh, w, h)) {
                presentSwap();
                ++presented_;
                return true;
            }
        }
        const float black[4] = {0, 0, 0, 1};
        ID3D11RenderTargetView* target = drawTarget();
        d3d_ctx_->OMSetRenderTargets(1, &target, nullptr);
        d3d_ctx_->ClearRenderTargetView(target, black);
        D3D11_VIEWPORT vp {};
        vp.Width = float(bb_w_);
        vp.Height = float(bb_h_);
        vp.MaxDepth = 1.0f;
        d3d_ctx_->RSSetViewports(1, &vp);
        const double scale = std::min(double(bb_w_) / w, double(bb_h_) / h);
        const int dw = std::max(1, int(w * scale)), dh = std::max(1, int(h * scale));
        const float rx = 2.0f * ((bb_w_ - dw) / 2) / bb_w_ - 1.0f;
        const float ry = 1.0f - 2.0f * ((bb_h_ - dh) / 2) / bb_h_;
        const float rw = 2.0f * dw / bb_w_, rh = 2.0f * dh / bb_h_;
        d3d_ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        d3d_ctx_->VSSetShader(vs_, nullptr, 0);
        d3d_ctx_->PSSetShader(ps_bgra_, nullptr, 0);
        d3d_ctx_->PSSetSamplers(0, 1, &samp_);
        ID3D11ShaderResourceView* srv = cpu_srv_;
        d3d_ctx_->PSSetShaderResources(2, 1, &srv);
        setParams(0, 0, 0, 0, rx, ry, rw, rh);
        d3d_ctx_->Draw(4, 0);
        ID3D11ShaderResourceView* none[1] = {nullptr};
        d3d_ctx_->PSSetShaderResources(2, 1, none);
        presentSwap();
        ++presented_;
        return true;
#else
        (void)f;
        return false;
#endif
    }

    SDL_Rect fitRect(int w, int h) const {
        int win_w = 0, win_h = 0;
        if (window_) SDL_GetWindowSize(window_, &win_w, &win_h);
        if (w <= 0 || h <= 0 || win_w <= 0 || win_h <= 0) return SDL_Rect{0, 0, win_w, win_h};
        const double s = std::min(double(win_w) / w, double(win_h) / h);
        const int dw = std::max(1, int(w * s)), dh = std::max(1, int(h * s));
        return SDL_Rect{(win_w - dw) / 2, (win_h - dh) / 2, dw, dh};
    }

    bool consumeKeyRequest() { const bool v = key_request_; key_request_ = false; return v; }
    bool consumeRestartRequest() { const bool v = restart_request_; restart_request_ = false; return v; }
    uint64_t presented() const { return presented_.load(); }
    void notePresented(std::chrono::steady_clock::time_point now, bool have_deadline,
                       std::chrono::steady_clock::time_point deadline, double pts_secs) {
        std::lock_guard<std::mutex> lock(pres_mutex_);
        if (pres_have_last_) {
            const double gap =
                std::chrono::duration<double, std::milli>(now - pres_last_).count();
            if (gap > pres_max_gap_ms_) pres_max_gap_ms_ = gap;
            if (gap > 25.0) ++pres_gt25_;
            if (gap > 50.0) ++pres_gt50_;
        }
        pres_last_ = now;
        pres_have_last_ = true;
        if (have_deadline && now > deadline + std::chrono::milliseconds(5)) ++pres_late_;
        if (pts_secs >= 0.0) {
            if (pts_last_secs_ >= 0.0 && pts_secs - pts_last_secs_ > 0.025) ++pts_holes_;
            pts_last_secs_ = pts_secs;
        }
    }
    std::string takePresStats() {
        std::lock_guard<std::mutex> lock(pres_mutex_);
        char buf[192];
        std::snprintf(buf, sizeof(buf),
                      "pres_gap>25ms=%llu >50ms=%llu max=%.0fms late=%llu pts_holes=%llu "
                      "target=%.0fms off_avg=%.0fms off_max=%.0fms",
                      (unsigned long long)pres_gt25_, (unsigned long long)pres_gt50_,
                      pres_max_gap_ms_, (unsigned long long)pres_late_,
                      (unsigned long long)pts_holes_,
                      smoothed_offset_ms_ + cushion_floor_ms_,
                      off_n_ ? off_sum_ms_ / double(off_n_) : 0.0, off_max_ms_);
        pres_gt25_ = pres_gt50_ = pres_late_ = pts_holes_ = 0;
        pres_max_gap_ms_ = 0.0;
        off_sum_ms_ = off_max_ms_ = 0.0;
        off_n_ = 0;
        return buf;
    }
    int width() const { return width_; }
    int height() const { return height_; }
    bool ok() const {
        return renderer_ != nullptr || d3d_ready_ || d3d12_ready_ || vulkanReady() || glReady();
    }

    void close() {
#if defined(_WIN32) || defined(WIREPLAY_HAVE_VULKAN)
        stop_preview_ = true;
        if (preview_thread_.joinable()) preview_thread_.join();
#endif
#ifdef _WIN32
        releaseD3D12();
        releaseD3D();
#endif
#if defined(WIREPLAY_HAVE_VULKAN)
        shutdownVulkan();
#endif
#if defined(_WIN32) || defined(WIREPLAY_HAVE_VULKAN) || defined(WIREPLAY_HAVE_GL)
        if (pv_sws_) { sws_freeContext(pv_sws_); pv_sws_ = nullptr; }
        if (pv_stage_) av_frame_free(&pv_stage_);
        if (pv_bgra_) av_frame_free(&pv_bgra_);
#endif
        if (texture_) SDL_DestroyTexture(texture_);
        if (renderer_) SDL_DestroyRenderer(renderer_);
        if (window_) SDL_DestroyWindow(window_);
        texture_ = nullptr;
        renderer_ = nullptr;
        window_ = nullptr;
        if (sdl_owned_) { SDL_Quit(); sdl_owned_ = false; }
    }

private:
    // A 5x7 bitmap font instead of SDL_ttf: one dependency fewer for a single
    // status string, and the binary stays self-contained.
    void drawOverlay() const;
#ifdef _WIN32
    bool initD3D();
    bool initD3D12();
    bool ensureBackBuffer12();
    ID3D12PipelineState* psoFor12(int kind, int samples);
    void setParams12(float slice, float r, float g, float b, float rx, float ry,
                     float rw, float rh, float tw, float th, float kf, float axis);
    void barrier12(ID3D12Resource* res, D3D12_RESOURCE_STATES before,
                   D3D12_RESOURCE_STATES after);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvAt12(int slot) const;
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpuAt12(int slot) const;
    void drawPass12(int kind, D3D12_CPU_DESCRIPTOR_HANDLE rtv, int vp_w, int vp_h,
                    float rx, float ry, float rw, float rh, float tw, float th,
                    float kf, float axis, int samples);
    bool beginFrame12();
    void finishPresent12(bool from_msaa);
    bool ensureStage12(int w, int h);
    bool ensureCpu12(int w, int h);
    bool ensureRz12(int w, int h);
    void ensureMsaa12();
    void waitIdle12();
    void presentBlank12();
    bool presentGpu12(void* texture, int slice, int w, int h);
    bool presentCpu12(const AVFrame* f);
    void releaseD3D12();
    bool ensureBackBuffer();
    bool ensureRz(int w, int h);
    bool drawLanczosNv12(int dw, int dh, float rx, float ry, float rw, float rh,
                         int sw, int sh);
    bool drawLanczosBgra(ID3D11ShaderResourceView* src, int dw, int dh, float rx,
                         float ry, float rw, float rh, int sw, int sh);
    ID3D11RenderTargetView* drawTarget();
    void presentSwap();
    void ensureMsaa();
    void setParams(float slice, float r, float g, float b, float rx = -1.0f,
                   float ry = 1.0f, float rw = 2.0f, float rh = 2.0f,
                   float tw = 0.0f, float th = 0.0f, float kf = 1.0f,
                   float axis = 0.0f);
    void presentBlank();
    void drawOverlayD3D();
    void releaseD3D();
#endif

    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
#if defined(WIREPLAY_HAVE_VULKAN)
    VkInstance vk_inst_ = VK_NULL_HANDLE;
    VkPhysicalDevice vk_phys_ = VK_NULL_HANDLE;
    VkDevice vk_dev_ = VK_NULL_HANDLE;
    VkQueue vk_queue_ = VK_NULL_HANDLE;
    VkSurfaceKHR vk_surf_ = VK_NULL_HANDLE;
    VkSwapchainKHR vk_swap_ = VK_NULL_HANDLE;
    std::vector<VkImage> vk_images_;
    VkCommandPool vk_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer vk_cmd_ = VK_NULL_HANDLE;
    VkSemaphore vk_acquire_ = VK_NULL_HANDLE;
    VkSemaphore vk_done_ = VK_NULL_HANDLE;
    VkFence vk_fence_ = VK_NULL_HANDLE;
    VkImage vk_fimg_ = VK_NULL_HANDLE;
    VkDeviceMemory vk_fmem_ = VK_NULL_HANDLE;
    VkBuffer vk_stage_ = VK_NULL_HANDLE;
    VkDeviceMemory vk_stage_mem_ = VK_NULL_HANDLE;
    void* vk_stage_ptr_ = nullptr;
    VkImageLayout vk_flayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t vk_qfamily_ = 0;
    int vk_sw_w_ = 0, vk_sw_h_ = 0, vk_fw_ = 0, vk_fh_ = 0;
    const char* vk_mode_name_ = "?";
    bool vk_ready_ = false;
    bool vk_swap_err_logged_ = false;
    // Vulkan Lanczos-3 pipeline (--scaling lanczos): graphics pipeline
    // with the embedded SPIR-V kernel; the blit below stays the fallback.
    VkShaderModule vk_lz_vs_ = VK_NULL_HANDLE, vk_lz_fs_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout vk_lz_dsl_ = VK_NULL_HANDLE;
    VkDescriptorPool vk_lz_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet vk_lz_sets_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkPipelineLayout vk_lz_layout_ = VK_NULL_HANDLE;
    VkPipeline vk_lz_pipe_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};  // [mid, out]
    VkRenderPass vk_lz_rp_mid_ = VK_NULL_HANDLE, vk_lz_rp_out_ = VK_NULL_HANDLE;
    VkSampler vk_lz_sampler_ = VK_NULL_HANDLE;
    VkImage vk_mid_img_ = VK_NULL_HANDLE;
    VkDeviceMemory vk_mid_mem_ = VK_NULL_HANDLE;
    VkImageView vk_mid_view_ = VK_NULL_HANDLE;
    VkFramebuffer vk_mid_fb_ = VK_NULL_HANDLE;
    int vk_mid_w_ = 0, vk_mid_h_ = 0;
    VkImageView vk_fimg_view_ = VK_NULL_HANDLE;
    std::vector<VkImageView> vk_swap_views_;
    std::vector<VkFramebuffer> vk_swap_fbs_;
    bool vk_lz_ready_ = false, vk_lz_failed_ = false;
#endif
    SDL_Texture* texture_ = nullptr;
    // Preview-thread GPU->BGRA conversion state (hwToBgra): the transfer
    // target and the sws output, both reused across frames.
    AVFrame* pv_stage_ = nullptr;
    AVFrame* pv_bgra_ = nullptr;
    SwsContext* pv_sws_ = nullptr;
    int pv_sws_w_ = 0, pv_sws_h_ = 0;
    AVPixelFormat pv_sws_fmt_ = AV_PIX_FMT_NONE;
    std::recursive_mutex* hw_mutex_ = nullptr;
#ifdef _WIN32
    ID3D11Device* d3d_device_ = nullptr;
    ID3D11DeviceContext* d3d_ctx_ = nullptr;
    IDXGISwapChain1* swap_ = nullptr;
    ID3D11RenderTargetView* rtv_ = nullptr;
    ID3D11VertexShader* vs_ = nullptr;
    ID3D11VertexShader* vs_solid_ = nullptr;
    ID3D11PixelShader* ps_nv12_ = nullptr;
    ID3D11PixelShader* ps_solid_ = nullptr;
    ID3D11Buffer* params_ = nullptr;
    ID3D11SamplerState* samp_ = nullptr;
    ID3D11Texture2D* stage_ = nullptr;
    ID3D11ShaderResourceView* stage_srv_[2] = {nullptr, nullptr};
    ID3D11PixelShader* ps_bgra_ = nullptr;
    ID3D11Texture2D* cpu_stage_ = nullptr;
    ID3D11ShaderResourceView* cpu_srv_ = nullptr;
    bool cpu_mips_ = false;
    bool stage_mips_ = false;
    // MSAA resolve target for the preview (swapchain is FLIP_DISCARD,
    // which must stay single-sample, so AA renders offscreen and resolves).
    ID3D11Texture2D* msaa_tex_ = nullptr;
    ID3D11RenderTargetView* msaa_rtv_ = nullptr;
    int msaa_w_ = 0, msaa_h_ = 0, msaa_active_ = 0;
    ID3D11PixelShader* ps_lz_nv12h_ = nullptr;
    ID3D11PixelShader* ps_lz_bgrah_ = nullptr;
    ID3D11PixelShader* ps_lz_bgrav_ = nullptr;
    ID3D11SamplerState* samp_point_ = nullptr;
    ID3D11Texture2D* rz_tex_ = nullptr;  // Lanczos intermediate (dstW x srcH)
    ID3D11ShaderResourceView* rz_srv_ = nullptr;
    ID3D11RenderTargetView* rz_rtv_ = nullptr;
    int rz_w_ = 0, rz_h_ = 0;
    // Camera effects pass textures (camera thread, fx_mutex_-serialised).
    ID3D11Texture2D* fx_in_ = nullptr;
    ID3D11ShaderResourceView* fx_in_srv_ = nullptr;
    ID3D11Texture2D* fx_out_ = nullptr;
    ID3D11RenderTargetView* fx_out_rtv_ = nullptr;
    ID3D11Texture2D* fx_msaa_ = nullptr;
    ID3D11RenderTargetView* fx_msaa_rtv_ = nullptr;
    ID3D11Texture2D* fx_read_ = nullptr;
    int fx_w_ = 0, fx_h_ = 0;
    int cpu_stage_w_ = 0, cpu_stage_h_ = 0;
    int stage_w_ = 0, stage_h_ = 0;
    DXGI_FORMAT stage_fmt_ = DXGI_FORMAT_UNKNOWN;
    int bb_w_ = 0, bb_h_ = 0;
    bool d3d_ready_ = false;
    // ---- D3D12 presentation engine -------------------------------------
    // The decoder still decodes into D3D11 textures; the NV12 slice is
    // copied (GPU->GPU, preview thread) into a D3D11 texture created
    // D3D11_RESOURCE_MISC_SHARED, opened here as an ID3D12Resource, and
    // sampled. A fence shared between the two devices orders the queues:
    // D3D11 signals after the copy, D3D12 waits, renders, signals back,
    // and the next copy waits for that. No CPU copy, no CPU stall.
    ID3D12Device* d12_ = nullptr;
    ID3D12CommandQueue* d12_queue_ = nullptr;
    IDXGISwapChain3* swap12_ = nullptr;
    ID3D12DescriptorHeap* rtv_heap12_ = nullptr;  // 0,1 backbuffers 2 msaa 3 rz
    ID3D12DescriptorHeap* srv_heap12_ = nullptr;  // 0 Y 1 UV 2 BGRA 3 rz
    ID3D12DescriptorHeap* samp_heap12_ = nullptr; // 0 main 1 point
    UINT rtv_inc12_ = 0, srv_inc12_ = 0, samp_inc12_ = 0;
    ID3D12RootSignature* root12_ = nullptr;
    ID3DBlob* vs_blob12_ = nullptr;
    ID3DBlob* ps_blob12_[5] = {};  // nv12, bgra, lzNV12H, lzBGRAH, lzBGRAV
    ID3D12PipelineState* pso12_[5] = {};
    ID3D12PipelineState* pso12_msaa_[5] = {};
    int pso12_msaa_samples_ = 0;
    ID3D12CommandAllocator* alloc12_[2] = {};
    ID3D12GraphicsCommandList* list12_ = nullptr;
    ID3D12Fence* fence12_ = nullptr;
    HANDLE fence_event12_ = nullptr;
    uint64_t frame_fence_val12_[2] = {};
    ID3D12Resource* back12_[2] = {};
    int bb12_w_ = 0, bb12_h_ = 0;
    ID3D12Resource* msaa12_ = nullptr;
    int msaa12_w_ = 0, msaa12_h_ = 0, msaa12_samples_ = 0;
    ID3D11Texture2D* stage12_11_ = nullptr;
    ID3D12Resource* stage12_ = nullptr;
    int stage12_w_ = 0, stage12_h_ = 0;
    ID3D11Device5* dev5_11_ = nullptr;
    ID3D11DeviceContext4* ctx4_11_ = nullptr;
    ID3D12Fence* fence_i12_ = nullptr;
    ID3D11Fence* fence_i11_ = nullptr;
    uint64_t interop_val_ = 0;
    uint64_t fence_seq12_ = 0;
    int cur12_ = 0;
    ID3D12Resource* cpu12_ = nullptr, *cpu12_up_ = nullptr;
    uint8_t* cpu12_ptr_ = nullptr;
    int cpu12_w_ = 0, cpu12_h_ = 0;
    UINT cpu12_pitch_ = 0;
    int cpu12_state_ = 0;  // D3D12_RESOURCE_STATES as int (rz/cpu tracking)
    ID3D12Resource* rz12_ = nullptr;
    int rz12_w_ = 0, rz12_h_ = 0, rz12_state_ = 0;
    ID3D12Resource* cb12_ = nullptr;
    uint8_t* cb12_ptr_ = nullptr;
    int cb12_slot_ = 0;
    bool d3d12_ready_ = false;
#else
    static constexpr bool d3d_ready_ = false;
    static constexpr bool d3d12_ready_ = false;
#endif
    // Preview-thread state (D3D11 on Windows, Vulkan when selected).
    std::thread preview_thread_;
    std::atomic<bool> stop_preview_ {false};
    std::atomic<bool> blank_dirty_ {false};
    std::mutex pending_mutex_;
    struct StashItem {
        AVFrame* frame;
        std::chrono::steady_clock::time_point arrived;
    };
    std::deque<StashItem> pending_queue_;
    std::atomic<uint64_t> stash_drops_ {0};
    // Presentation-cadence forensics, reset on each read: separates
    // "the stream's own PTS has holes" from "we presented it late".
    std::mutex pres_mutex_;
    uint64_t pres_gt25_ = 0, pres_gt50_ = 0, pres_late_ = 0, pts_holes_ = 0;
    double pres_max_gap_ms_ = 0.0;
    std::chrono::steady_clock::time_point pres_last_ {};
    bool pres_have_last_ = false;
    double pts_last_secs_ = -1.0;
    int width_ = 0, height_ = 0;
    // PTS presentation anchor (preview-thread owned).
    bool pts_anchor_set_ = false;
    double pts_anchor_secs_ = 0.0;
    std::chrono::steady_clock::time_point pts_wall_anchor_ {};
    bool present_pts_ = true;  // --present mode, set in open()
    int msaa_ = 0;   // --msaa (requested; GPU may clamp)
    int aniso_ = 0;  // --aniso 1..16 (0 = off)
    Options::Scaling scaling_ = Options::Scaling::Lanczos;  // --scaling
    int paniso_ = 0;  // effective preview anisotropy (aniso mode: 16 default)
    // Serialises whole D3D11 render sequences (preview presents vs the
    // camera effects pass): the immediate context's bindings are state,
    // and per-call multithread protection does not make a sequence atomic.
    std::recursive_mutex fx_mutex_;
    double cushion_floor_ms_ = 17.0;             // --cushion: margin on the EMA
    double smoothed_offset_ms_ = 0.0;            // EMA of arrival offsets
    bool smoothed_set_ = false;
    double off_sum_ms_ = 0.0, off_max_ms_ = 0.0; // arrival-offset forensics
    uint64_t off_n_ = 0;
    void (*gl_gen_mipmap_)(unsigned int) = nullptr;
    // OpenGL Lanczos (runtime-loaded GL2 entry points; the fixed-function
    // path below stays the fallback when they are missing).
    unsigned int (*glp_create_shader_)(unsigned int) = nullptr;
    void (*glp_shader_source_)(unsigned int, int, const char* const*, const int*) = nullptr;
    void (*glp_compile_shader_)(unsigned int) = nullptr;
    void (*glp_get_shaderiv_)(unsigned int, unsigned int, int*) = nullptr;
    unsigned int (*glp_create_program_)() = nullptr;
    void (*glp_attach_shader_)(unsigned int, unsigned int) = nullptr;
    void (*glp_link_program_)(unsigned int) = nullptr;
    void (*glp_get_programiv_)(unsigned int, unsigned int, int*) = nullptr;
    void (*glp_use_program_)(unsigned int) = nullptr;
    int (*glp_get_uniform_)(unsigned int, const char*) = nullptr;
    void (*glp_uniform1i_)(int, int) = nullptr;
    void (*glp_uniform4f_)(int, float, float, float, float) = nullptr;
    void (*glp_gen_framebuffers_)(int, unsigned int*) = nullptr;
    void (*glp_bind_framebuffer_)(unsigned int, unsigned int) = nullptr;
    void (*glp_framebuffer_texture2d_)(unsigned int, unsigned int, unsigned int,
                                       unsigned int, int) = nullptr;
    unsigned int gl_lz_prog_ = 0, gl_rz_tex_ = 0, gl_fbo_ = 0;
    int gl_rz_w_ = 0, gl_rz_h_ = 0, gl_u_src_ = -1, gl_u_ker_ = -1;
    bool gl_lz_ok_ = false, gl_lz_failed_ = false;
    SDL_GLContext gl_ctx_ = nullptr;
    unsigned int gl_tex_ = 0;
    int gl_tex_w_ = 0, gl_tex_h_ = 0;
    bool gl_logged_ = false;
    bool gl_ready_ = false;
    std::atomic<uint64_t> presented_ {0};
    std::string overlay_msg_;
    bool key_request_ = false;
    bool restart_request_ = false;
    std::atomic<bool> preview_paused_ {false};
    bool restore_request_ = false;
    bool sdl_owned_ = false;
};

const uint8_t kFont[96][5] = {
    {0,0,0,0,0},{0,0,0x5F,0,0},{0,0,7,0,0},{0x14,0x7F,0x14,0x7F,0x14},{0x24,0x2A,0x7F,0x2A,0x12},
    {0x23,0x13,8,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},{0,5,3,0,0},{0,0x1C,0x22,0x41,0},{0,0x41,0x22,0x1C,0},
    {0x14,8,0x3E,8,0x14},{8,8,0x3E,8,8},{0,0x50,0x30,0,0},{8,8,8,8,8},{0,0x60,0x60,0,0},{0x20,0x10,8,4,2},
    {0x3E,0x51,0x49,0x45,0x3E},{0,0x42,0x7F,0x40,0},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{1,0x71,9,5,3},
    {0x36,0x49,0x49,0x49,0x36},{6,0x49,0x49,0x29,0x1E},{0,0x36,0x36,0,0},{0,0x56,0x36,0,0},{8,0x14,0x22,0x41,0},
    {0x14,0x14,0x14,0x14,0x14},{0,0x41,0x22,0x14,8},{2,1,0x51,9,6},{0x32,0x49,0x79,0x41,0x3E},{0x7E,0x11,0x11,0x11,0x7E},
    {0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},{0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},
    {0x7F,9,9,9,1},{0x3E,0x41,0x49,0x49,0x7A},{0x7F,8,8,8,0x7F},{0,0x41,0x7F,0x41,0},{0x20,0x40,0x41,0x3F,1},
    {0x7F,8,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},{0x7F,2,0xC,2,0x7F},{0x7F,4,8,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,9,9,9,6},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,9,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},{1,1,0x7F,1,1},
    {0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},{0x63,0x14,8,0x14,0x63},
    {7,8,0x70,8,7},{0x61,0x51,0x49,0x45,0x43},{0,0x7F,0x41,0x41,0},{0x55,0x2A,0x55,0x2A,0x55},{0,0x41,0x41,0x7F,0},
    {4,2,1,2,4},{0x40,0x40,0x40,0x40,0x40},{0,1,2,4,0},{0x20,0x54,0x54,0x54,0x78},{0x7F,0x48,0x44,0x44,0x38},
    {0x38,0x44,0x44,0x44,0x20},{0x38,0x44,0x44,0x48,0x7F},{0x38,0x54,0x54,0x54,0x18},{8,0x7E,9,1,2},{8,0x14,0x54,0x54,0x3C},
    {0x7F,8,4,4,0x78},{0,0x44,0x7D,0x40,0},{0x20,0x40,0x44,0x3D,0},{0x7F,0x10,0x28,0x44,0},{0,0x41,0x7F,0x40,0},
    {0x7C,4,0x18,4,0x78},{0x7C,8,4,4,0x78},{0x38,0x44,0x44,0x44,0x38},{0x7C,0x14,0x14,0x14,8},{8,0x14,0x14,0x18,0x7C},
    {0x7C,8,4,4,8},{0x48,0x54,0x54,0x54,0x20},{4,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x20,0x7C},{0x1C,0x20,0x40,0x20,0x1C},
    {0x3C,0x40,0x30,0x40,0x3C},{0x44,0x28,0x10,0x28,0x44},{0xC,0x50,0x50,0x50,0x3C},{0x44,0x64,0x54,0x4C,0x44},{0,8,0x36,0x41,0},
    {0,0,0x7F,0,0},{0,0x41,0x36,8,0},{8,8,0x2A,0x1C,8},{8,0x1C,0x2A,8,8},
};

void View::drawOverlay() const {
#ifdef _WIN32
    if (d3d_ready_) { const_cast<View*>(this)->drawOverlayD3D(); return; }
#endif
    if (!renderer_) return;
    const int scale = 3;
    const int char_w = 6 * scale, char_h = 8 * scale;
    int win_w = 0, win_h = 0;
    SDL_GetWindowSize(window_, &win_w, &win_h);
    int x = (win_w - int(overlay_msg_.size()) * char_w) / 2;
    int y = win_h / 2 - char_h / 2;
    SDL_SetRenderDrawColor(renderer_, 210, 210, 215, 255);
    for (const char c : overlay_msg_) {
        const int idx = int(uint8_t(c)) - 32;
        if (idx >= 0 && idx < 96) {
            for (int col = 0; col < 5; ++col) {
                for (int row = 0; row < 7; ++row) {
                    if (kFont[idx][col] & (1 << row)) {
                        SDL_Rect r{x + col * scale, y + row * scale, scale, scale};
                        SDL_RenderFillRect(renderer_, &r);
                    }
                }
            }
        }
        x += char_w;
    }
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
}


#ifdef _WIN32
namespace {
const char kShaderSrc[] = R"(
cbuffer Params : register(b0) { float slice; float3 tint; float4 rect; float4 tex; float4 ker; };
// tex.xy = source size in texels, tex.zw = 1/size. ker.x = downscale
// factor (source texels per output pixel, >= 1), ker.y = axis (0=H, 1=V).
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD; };
VSOut vsMain(uint id : SV_VertexID) {
    // rect is the letterbox destination in NDC (x, y of the top-left
    // corner, then width/height). Drawing the rectangle itself -- rather
    // than a fullscreen triangle behind a letterbox viewport -- is what
    // keeps uv 0..1 exactly on the visible area; the viewport version
    // sampled only the middle of the triangle and cropped the picture.
    // 4-vertex triangle strip: uv is exactly 0..1 over the rectangle.
    // (The old 0..2 fullscreen-triangle trick spilled a second, clamped-
    // sample triangle into the letterbox area whenever the window aspect
    // differed from the stream aspect -- the diagonal wedge seen on
    // maximise/resize.)
    float2 uv = float2((id << 1) & 2, id & 2) * 0.5;
    VSOut o;
    o.pos = float4(rect.x + uv.x * rect.z, rect.y - uv.y * rect.w, 0.0, 1.0);
    o.uv = uv;
    return o;
}
Texture2D tY : register(t0);
Texture2D tUV : register(t1);
SamplerState smp : register(s0);
float4 psNV12(VSOut i) : SV_Target {
    float y = tY.Sample(smp, i.uv).r;
    float2 c = tUV.Sample(smp, i.uv).rg;
    float yy = 1.16438356 * (y - 0.06274510);
    float u = c.x - 0.50196078;
    float v = c.y - 0.50196078;
    float3 rgb = float3(yy + 1.5748 * v, yy - 0.1873 * u - 0.4681 * v, yy + 1.8556 * u);
    return float4(saturate(rgb), 1.0);
}
Texture2D tBGRA : register(t2);
float4 psBGRA(VSOut i) : SV_Target { return tBGRA.Sample(smp, i.uv); }
SamplerState smpP : register(s1);
// Lanczos-3 (the OBS/Streamlabs downscale filter): sinc windowed by a
// sinc of radius 3, the kernel stretched by the downscale factor so a
// 2x reduction integrates ~13 taps instead of aliasing on 6. Two
// separable 1-D passes (H into an intermediate, V to the screen) do
// the work of the 13x13 2-D kernel at 13+13 texture fetches.
float lanczosW(float x) {
    x = abs(x);
    if (x < 1e-5) return 1.0;
    if (x >= 3.0) return 0.0;
    float px = 3.14159265 * x;
    return (sin(px) / px) * (sin(px / 3.0) / (px / 3.0));
}
float3 nv12At(int tx, int ty) {
    float2 uv = float2((float(tx) + 0.5) * tex.z, (float(ty) + 0.5) * tex.w);
    float y = tY.SampleLevel(smpP, uv, 0).r;
    float2 c = tUV.SampleLevel(smpP, uv, 0).rg;
    float yy = 1.16438356 * (y - 0.06274510);
    float u = c.x - 0.50196078;
    float v = c.y - 0.50196078;
    return saturate(float3(yy + 1.5748 * v, yy - 0.1873 * u - 0.4681 * v, yy + 1.8556 * u));
}
float4 psNV12LH(VSOut i) : SV_Target {
    float2 tc = i.uv * tex.xy - 0.5;
    float f = max(ker.x, 1.0);
    float sup = 3.0 * f;
    float t0 = ceil(tc.x - sup), t1 = floor(tc.x + sup);
    int ty = clamp(int(tc.y + 0.5), 0, int(tex.y) - 1);
    float3 acc = 0.0; float ws = 0.0;
    for (int k = 0; k < 40; ++k) {
        float t = t0 + float(k);
        if (t > t1) break;
        float w = lanczosW((tc.x - t) / f);
        if (w == 0.0) continue;
        acc += nv12At(clamp(int(t), 0, int(tex.x) - 1), ty) * w;
        ws += w;
    }
    return float4(ws > 0.0 ? acc / ws : nv12At(clamp(int(tc.x + 0.5), 0, int(tex.x) - 1), ty), 1.0);
}
float4 psBGRALH(VSOut i) : SV_Target {
    float2 tc = i.uv * tex.xy - 0.5;
    float f = max(ker.x, 1.0);
    float sup = 3.0 * f;
    float t0 = ceil(tc.x - sup), t1 = floor(tc.x + sup);
    int ty = clamp(int(tc.y + 0.5), 0, int(tex.y) - 1);
    float4 acc = 0.0; float ws = 0.0;
    for (int k = 0; k < 40; ++k) {
        float t = t0 + float(k);
        if (t > t1) break;
        float w = lanczosW((tc.x - t) / f);
        if (w == 0.0) continue;
        int tx = clamp(int(t), 0, int(tex.x) - 1);
        acc += tBGRA.SampleLevel(smpP, float2((float(tx) + 0.5) * tex.z, (float(ty) + 0.5) * tex.w), 0) * w;
        ws += w;
    }
    return float4(ws > 0.0 ? (acc / ws).rgb : tBGRA.Sample(smp, i.uv).rgb, 1.0);
}
Texture2D tRZ : register(t3);
float4 psBGRALV(VSOut i) : SV_Target {
    // Source here is the intermediate (tRZ): tex.xy = (dst width, src height).
    float2 tc = i.uv * tex.xy - 0.5;
    float f = max(ker.x, 1.0);
    float sup = 3.0 * f;
    float t0 = ceil(tc.y - sup), t1 = floor(tc.y + sup);
    int tx = clamp(int(tc.x + 0.5), 0, int(tex.x) - 1);
    float4 acc = 0.0; float ws = 0.0;
    for (int k = 0; k < 40; ++k) {
        float t = t0 + float(k);
        if (t > t1) break;
        float w = lanczosW((tc.y - t) / f);
        if (w == 0.0) continue;
        int ty = clamp(int(t), 0, int(tex.y) - 1);
        acc += tRZ.SampleLevel(smpP, float2((float(tx) + 0.5) * tex.z, (float(ty) + 0.5) * tex.w), 0) * w;
        ws += w;
    }
    return float4(ws > 0.0 ? (acc / ws).rgb : tRZ.Sample(smp, i.uv).rgb, 1.0);
}
float4 vsSolid(float2 p : POSITION) : SV_Position { return float4(p, 0.0, 1.0); }
float4 psSolid() : SV_Target { return float4(tint, 1.0); }
)";

bool CompileShader(const char* entry, const char* target, ID3DBlob** blob) {
    ID3DBlob* errors = nullptr;
    const HRESULT hr = D3DCompile(kShaderSrc, sizeof(kShaderSrc) - 1, nullptr, nullptr,
                                  nullptr, entry, target, 0, 0, blob, &errors);
    if (errors) errors->Release();
    return SUCCEEDED(hr);
}
}  // namespace

bool View::initD3D() {
    SDL_SysWMinfo wm {};
    SDL_VERSION(&wm.version);
    if (!SDL_GetWindowWMInfo(window_, &wm)) return false;
    const HWND hwnd = wm.info.win.window;

    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT |
                                     D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                 nullptr, 0,
                                 D3D11_SDK_VERSION, &d3d_device_, &level, &d3d_ctx_))) {
        return false;
    }
    // One frame of present latency, not the DXGI default of three: the
    // preview presents each real frame once on its stream time, so a
    // deeper present queue is pure added delay for no smoothing. Lives
    // on the device and applies to every swapchain it creates.
    if (IDXGIDevice1* dxdev1 = nullptr;
        SUCCEEDED(d3d_device_->QueryInterface(IID_PPV_ARGS(&dxdev1))) && dxdev1) {
        dxdev1->SetMaximumFrameLatency(1);
        dxdev1->Release();
    }
    IDXGIDevice* dxdev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory2* factory = nullptr;
    bool ok = SUCCEEDED(d3d_device_->QueryInterface(IID_PPV_ARGS(&dxdev))) &&
              SUCCEEDED(dxdev->GetAdapter(&adapter)) &&
              SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory)));
    if (ok) {
        DXGI_SWAP_CHAIN_DESC1 desc {};
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.BufferCount = 2;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.SampleDesc.Count = 1;
        ok = SUCCEEDED(factory->CreateSwapChainForHwnd(d3d_device_, hwnd, &desc,
                                                       nullptr, nullptr, &swap_));
    }
    if (dxdev) dxdev->Release();
    if (adapter) adapter->Release();
    if (factory) factory->Release();
    if (!ok) { releaseD3D(); return false; }

    ID3DBlob* blob = nullptr;
    if (ok && CompileShader("vsMain", "vs_5_0", &blob)) {
        ok = SUCCEEDED(d3d_device_->CreateVertexShader(blob->GetBufferPointer(),
                                                       blob->GetBufferSize(), nullptr, &vs_));
        blob->Release(); blob = nullptr;
    } else ok = false;
    if (ok && CompileShader("psNV12", "ps_5_0", &blob)) {
        ok = SUCCEEDED(d3d_device_->CreatePixelShader(blob->GetBufferPointer(),
                                                      blob->GetBufferSize(), nullptr, &ps_nv12_));
        blob->Release(); blob = nullptr;
    } else ok = false;
    if (ok && CompileShader("psBGRA", "ps_5_0", &blob)) {
        ok = SUCCEEDED(d3d_device_->CreatePixelShader(blob->GetBufferPointer(),
                                                      blob->GetBufferSize(), nullptr, &ps_bgra_));
        blob->Release(); blob = nullptr;
    } else ok = false;
    if (ok && CompileShader("psNV12LH", "ps_5_0", &blob)) {
        ok = SUCCEEDED(d3d_device_->CreatePixelShader(blob->GetBufferPointer(),
                                                      blob->GetBufferSize(), nullptr, &ps_lz_nv12h_));
        blob->Release(); blob = nullptr;
    } else ok = false;
    if (ok && CompileShader("psBGRALH", "ps_5_0", &blob)) {
        ok = SUCCEEDED(d3d_device_->CreatePixelShader(blob->GetBufferPointer(),
                                                      blob->GetBufferSize(), nullptr, &ps_lz_bgrah_));
        blob->Release(); blob = nullptr;
    } else ok = false;
    if (ok && CompileShader("psBGRALV", "ps_5_0", &blob)) {
        ok = SUCCEEDED(d3d_device_->CreatePixelShader(blob->GetBufferPointer(),
                                                      blob->GetBufferSize(), nullptr, &ps_lz_bgrav_));
        blob->Release(); blob = nullptr;
    } else ok = false;
    if (ok && CompileShader("vsSolid", "vs_5_0", &blob)) {
        ok = SUCCEEDED(d3d_device_->CreateVertexShader(blob->GetBufferPointer(),
                                                       blob->GetBufferSize(), nullptr, &vs_solid_));
        blob->Release(); blob = nullptr;
    } else ok = false;
    if (ok && CompileShader("psSolid", "ps_5_0", &blob)) {
        ok = SUCCEEDED(d3d_device_->CreatePixelShader(blob->GetBufferPointer(),
                                                      blob->GetBufferSize(), nullptr, &ps_solid_));
        blob->Release(); blob = nullptr;
    } else ok = false;
    if (!ok) { releaseD3D(); return false; }

    D3D11_BUFFER_DESC cbd {};
    cbd.ByteWidth = 64;
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(d3d_device_->CreateBuffer(&cbd, nullptr, &params_))) {
        releaseD3D();
        return false;
    }
    D3D11_SAMPLER_DESC sd {};
    // Anisotropic filtering only samples better than bilinear when the
    // texture carries a mip chain (the preview's staging textures build
    // one when --aniso is on); without it, this stays the historical
    // MIN_MAG_LINEAR_MIP_POINT sampler, byte for byte.
    sd.Filter = paniso_ > 1 ? D3D11_FILTER_ANISOTROPIC
                            : D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sd.MaxAnisotropy = paniso_ > 1 ? UINT(std::min(paniso_, 16)) : 1;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    if (FAILED(d3d_device_->CreateSamplerState(&sd, &samp_))) {
        releaseD3D();
        return false;
    }
    if (paniso_ > 1)
        std::fprintf(stderr, "[wireplay] D3D11 sampler: anisotropic x%d (mip chain on)\n",
                     std::min(paniso_, 16));
    if (msaa_ > 1)
        std::fprintf(stderr, "[wireplay] D3D11 MSAA: x%d requested (clamped at first frame)\n",
                     msaa_);
    {
        D3D11_SAMPLER_DESC pd {};
        pd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        pd.AddressU = pd.AddressV = pd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        d3d_device_->CreateSamplerState(&pd, &samp_point_);  // Lanczos taps
    }
    ID3D11Multithread* mt = nullptr;
    if (SUCCEEDED(d3d_ctx_->QueryInterface(IID_PPV_ARGS(&mt))) && mt) {
        mt->SetMultithreadProtected(TRUE);
        mt->Release();
    }
    d3d_ready_ = true;
    // Presentation gets its own thread: Present(1) blocks until vsync, and
    // on the stream thread that block stopped pipe reads, backed the
    // engine queue up (the queue_drops spikes) and delivered audio in
    // bursts that burst the app's 80 ms audio buffer (the crackle). The
    // stream thread now only hands textures over and keeps draining.
    stop_preview_ = false;
    preview_thread_ = std::thread([this] { previewLoop(); });
    return true;
}

bool View::ensureBackBuffer() {
    if (!swap_) return false;
    SDL_SysWMinfo wm {};
    SDL_VERSION(&wm.version);
    if (!SDL_GetWindowWMInfo(window_, &wm)) return false;
    RECT rc {};
    GetClientRect(wm.info.win.window, &rc);
    const int w = int(rc.right - rc.left), h = int(rc.bottom - rc.top);
    if (w <= 0 || h <= 0) return false;
    if (rtv_ && w == bb_w_ && h == bb_h_) return true;

    // Unbind before resizing: ResizeBuffers fails while the old back
    // buffer is still bound as a render target, which is exactly what
    // used to happen on maximise -- the resize silently never happened
    // and the picture stayed cropped at the window's birth size.
    ID3D11RenderTargetView* none[1] = {nullptr};
    d3d_ctx_->OMSetRenderTargets(1, none, nullptr);
    if (rtv_) { rtv_->Release(); rtv_ = nullptr; }

    // Trust the buffer's real size, not our bookkeeping: the swap chain
    // is born at the window's birth size and the first frame arrives
    // after setGeometry() has already resized the window.
    ID3D11Texture2D* back = nullptr;
    if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
    D3D11_TEXTURE2D_DESC bd {};
    back->GetDesc(&bd);
    back->Release();
    if (int(bd.Width) != w || int(bd.Height) != h) {
        if (FAILED(swap_->ResizeBuffers(0, UINT(w), UINT(h),
                                        DXGI_FORMAT_UNKNOWN, 0))) {
            return false;
        }
    }
    if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
    const HRESULT hr = d3d_device_->CreateRenderTargetView(back, nullptr, &rtv_);
    back->Release();
    if (FAILED(hr)) return false;
    bb_w_ = w;
    bb_h_ = h;
    ensureMsaa();
    return true;
}

ID3D11RenderTargetView* View::drawTarget() {
    return msaa_rtv_ ? msaa_rtv_ : rtv_;
}

void View::presentSwap() {
    if (msaa_rtv_ && msaa_tex_) {
        ID3D11Texture2D* back = nullptr;
        if (SUCCEEDED(swap_->GetBuffer(0, IID_PPV_ARGS(&back))) && back) {
            d3d_ctx_->ResolveSubresource(back, 0, msaa_tex_, 0,
                                         DXGI_FORMAT_B8G8R8A8_UNORM);
            back->Release();
        }
    }
    swap_->Present(0, 0);
}

// (Re)build the MSAA render target at backbuffer size. FLIP_DISCARD
// swapchains must stay single-sample, so AA renders into this offscreen
// target and resolves into the backbuffer in presentSwap(). The sample
// count is the largest supported count not above the request (many
// GPUs top out at 8x; a 16 request lands there and says so).
void View::ensureMsaa() {
    if (msaa_ <= 1 || bb_w_ <= 0 || bb_h_ <= 0) return;
    if (msaa_rtv_ && msaa_w_ == bb_w_ && msaa_h_ == bb_h_) return;
    if (msaa_rtv_) { msaa_rtv_->Release(); msaa_rtv_ = nullptr; }
    if (msaa_tex_) { msaa_tex_->Release(); msaa_tex_ = nullptr; }
    int chosen = 0;
    UINT quality = 0;
    for (int cand : {16, 8, 4, 2}) {
        UINT ql = 0;
        if (cand <= msaa_ &&
            SUCCEEDED(d3d_device_->CheckMultisampleQualityLevels(
                DXGI_FORMAT_B8G8R8A8_UNORM, UINT(cand), &ql)) && ql > 0) {
            chosen = cand; quality = ql; break;
        }
    }
    if (chosen <= 1) {
        std::fprintf(stderr, "[wireplay] D3D11 MSAA: no multisample support; off\n");
        msaa_active_ = 0;
        return;
    }
    D3D11_TEXTURE2D_DESC td {};
    td.Width = UINT(bb_w_); td.Height = UINT(bb_h_);
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = UINT(chosen);
    td.SampleDesc.Quality = quality - 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(d3d_device_->CreateTexture2D(&td, nullptr, &msaa_tex_))) return;
    if (FAILED(d3d_device_->CreateRenderTargetView(msaa_tex_, nullptr, &msaa_rtv_))) {
        msaa_tex_->Release(); msaa_tex_ = nullptr; return;
    }
    msaa_w_ = bb_w_; msaa_h_ = bb_h_;
    if (msaa_active_ != chosen) {
        msaa_active_ = chosen;
        std::fprintf(stderr, "[wireplay] D3D11 MSAA active: x%d%s\n", chosen,
                     chosen < msaa_ ? " (requested more; GPU max)" : "");
    }
}

bool View::ensureRz(int w, int h) {
    if (rz_tex_ && rz_w_ == w && rz_h_ == h) return true;
    if (rz_srv_) { rz_srv_->Release(); rz_srv_ = nullptr; }
    if (rz_rtv_) { rz_rtv_->Release(); rz_rtv_ = nullptr; }
    if (rz_tex_) { rz_tex_->Release(); rz_tex_ = nullptr; }
    rz_w_ = rz_h_ = 0;
    D3D11_TEXTURE2D_DESC td {};
    td.Width = UINT(w); td.Height = UINT(h);
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(d3d_device_->CreateTexture2D(&td, nullptr, &rz_tex_))) return false;
    if (FAILED(d3d_device_->CreateShaderResourceView(rz_tex_, nullptr, &rz_srv_))) return false;
    if (FAILED(d3d_device_->CreateRenderTargetView(rz_tex_, nullptr, &rz_rtv_))) return false;
    rz_w_ = w; rz_h_ = h;
    return true;
}

// Separable Lanczos-3 preview resize. Pass H shrinks the source into
// the intermediate (dst width x source height); pass V shrinks that to
// the letterbox rectangle on the (possibly MSAA) draw target. Caller
// presents. Any failure returns false and the caller takes the plain
// sampled path instead.
bool View::drawLanczosNv12(int dw, int dh, float rx, float ry, float rw, float rh,
                           int sw, int sh) {
    if (!ps_lz_nv12h_ || !ps_lz_bgrav_ || !samp_point_) return false;
    if (!ensureRz(dw, sh)) return false;
    ID3D11SamplerState* samplers[2] = {samp_, samp_point_};
    d3d_ctx_->OMSetRenderTargets(1, &rz_rtv_, nullptr);
    D3D11_VIEWPORT vp {};
    vp.Width = float(dw); vp.Height = float(sh); vp.MaxDepth = 1.0f;
    d3d_ctx_->RSSetViewports(1, &vp);
    d3d_ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    d3d_ctx_->VSSetShader(vs_, nullptr, 0);
    d3d_ctx_->PSSetShader(ps_lz_nv12h_, nullptr, 0);
    d3d_ctx_->PSSetSamplers(0, 2, samplers);
    ID3D11ShaderResourceView* const srvs[2] = {stage_srv_[0], stage_srv_[1]};
    d3d_ctx_->PSSetShaderResources(0, 2, srvs);
    setParams(0, 0, 0, 0, -1.0f, 1.0f, 2.0f, 2.0f, float(sw), float(sh),
              float(sw) / float(std::max(1, dw)), 0.0f);
    d3d_ctx_->Draw(4, 0);
    ID3D11ShaderResourceView* none2[2] = {nullptr, nullptr};
    d3d_ctx_->PSSetShaderResources(0, 2, none2);

    const float black[4] = {0, 0, 0, 1};
    ID3D11RenderTargetView* target = drawTarget();
    d3d_ctx_->OMSetRenderTargets(1, &target, nullptr);
    d3d_ctx_->ClearRenderTargetView(target, black);
    vp.Width = float(bb_w_); vp.Height = float(bb_h_);
    d3d_ctx_->RSSetViewports(1, &vp);
    d3d_ctx_->PSSetShader(ps_lz_bgrav_, nullptr, 0);
    ID3D11ShaderResourceView* srv = rz_srv_;
    d3d_ctx_->PSSetShaderResources(3, 1, &srv);
    setParams(0, 0, 0, 0, rx, ry, rw, rh, float(dw), float(sh),
              float(sh) / float(std::max(1, dh)), 1.0f);
    d3d_ctx_->Draw(4, 0);
    ID3D11ShaderResourceView* none[1] = {nullptr};
    d3d_ctx_->PSSetShaderResources(3, 1, none);
    return true;
}

bool View::drawLanczosBgra(ID3D11ShaderResourceView* src, int dw, int dh, float rx,
                           float ry, float rw, float rh, int sw, int sh) {
    if (!src || !ps_lz_bgrah_ || !ps_lz_bgrav_ || !samp_point_) return false;
    if (!ensureRz(dw, sh)) return false;
    ID3D11SamplerState* samplers[2] = {samp_, samp_point_};
    d3d_ctx_->OMSetRenderTargets(1, &rz_rtv_, nullptr);
    D3D11_VIEWPORT vp {};
    vp.Width = float(dw); vp.Height = float(sh); vp.MaxDepth = 1.0f;
    d3d_ctx_->RSSetViewports(1, &vp);
    d3d_ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    d3d_ctx_->VSSetShader(vs_, nullptr, 0);
    d3d_ctx_->PSSetShader(ps_lz_bgrah_, nullptr, 0);
    d3d_ctx_->PSSetSamplers(0, 2, samplers);
    d3d_ctx_->PSSetShaderResources(2, 1, &src);
    setParams(0, 0, 0, 0, -1.0f, 1.0f, 2.0f, 2.0f, float(sw), float(sh),
              float(sw) / float(std::max(1, dw)), 0.0f);
    d3d_ctx_->Draw(4, 0);
    ID3D11ShaderResourceView* none[1] = {nullptr};
    d3d_ctx_->PSSetShaderResources(2, 1, none);

    const float black[4] = {0, 0, 0, 1};
    ID3D11RenderTargetView* target = drawTarget();
    d3d_ctx_->OMSetRenderTargets(1, &target, nullptr);
    d3d_ctx_->ClearRenderTargetView(target, black);
    vp.Width = float(bb_w_); vp.Height = float(bb_h_);
    d3d_ctx_->RSSetViewports(1, &vp);
    d3d_ctx_->PSSetShader(ps_lz_bgrav_, nullptr, 0);
    ID3D11ShaderResourceView* srv = rz_srv_;
    d3d_ctx_->PSSetShaderResources(3, 1, &srv);
    setParams(0, 0, 0, 0, rx, ry, rw, rh, float(dw), float(sh),
              float(sh) / float(std::max(1, dh)), 1.0f);
    d3d_ctx_->Draw(4, 0);
    d3d_ctx_->PSSetShaderResources(3, 1, none);
    return true;
}

void View::setParams(float slice, float r, float g, float b, float rx, float ry,
                     float rw, float rh, float tw, float th, float kf, float axis) {
    D3D11_MAPPED_SUBRESOURCE map {};
    if (FAILED(d3d_ctx_->Map(params_, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return;
    const float values[16] = {slice, r, g, b, rx, ry, rw, rh,
                              tw, th, tw > 0 ? 1.0f / tw : 0.0f, th > 0 ? 1.0f / th : 0.0f,
                              kf, axis, 0.0f, 0.0f};
    std::memcpy(map.pData, values, sizeof(values));
    d3d_ctx_->Unmap(params_, 0);
    d3d_ctx_->PSSetConstantBuffers(0, 1, &params_);
    d3d_ctx_->VSSetConstantBuffers(0, 1, &params_);
}

void View::presentBlank() {
    if (!d3d_ready_) return;
    std::lock_guard<std::recursive_mutex> fxlock(fx_mutex_);
    if (!ensureBackBuffer()) return;
    const float black[4] = {0, 0, 0, 1};
    ID3D11RenderTargetView* target = drawTarget();
    d3d_ctx_->OMSetRenderTargets(1, &target, nullptr);
    d3d_ctx_->ClearRenderTargetView(target, black);
    drawOverlayD3D();
    presentSwap();
}

void View::drawOverlayD3D() {
    if (!d3d_ready_ || !ensureBackBuffer()) return;
    const int scale = 3;
    const int char_w = 6 * scale, char_h = 8 * scale;
    const int win_w = bb_w_, win_h = bb_h_;
    int x0 = (win_w - int(overlay_msg_.size()) * char_w) / 2;
    const int y0 = win_h / 2 - char_h / 2;
    std::vector<float> verts;
    auto quad = [&](int px, int py) {
        const float l = 2.0f * px / win_w - 1.0f, r = 2.0f * (px + scale) / win_w - 1.0f;
        const float t = 1.0f - 2.0f * py / win_h, b = 1.0f - 2.0f * (py + scale) / win_h;
        const float q[12] = {l, t, r, t, l, b, r, t, r, b, l, b};
        verts.insert(verts.end(), q, q + 12);
    };
    int x = x0;
    for (const char c : overlay_msg_) {
        const int idx = int(uint8_t(c)) - 32;
        if (idx >= 0 && idx < 96) {
            for (int col = 0; col < 5; ++col)
                for (int row = 0; row < 7; ++row)
                    if (kFont[idx][col] & (1 << row)) quad(x + col * scale, y0 + row * scale);
        }
        x += char_w;
    }
    (void)x0;
    if (verts.empty()) return;
    D3D11_BUFFER_DESC bd {};
    bd.ByteWidth = UINT(verts.size() * sizeof(float));
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init {verts.data(), 0, 0};
    ID3D11Buffer* vb = nullptr;
    if (FAILED(d3d_device_->CreateBuffer(&bd, &init, &vb))) return;
    D3D11_VIEWPORT vp {};
    vp.Width = float(win_w);
    vp.Height = float(win_h);
    vp.MaxDepth = 1.0f;
    d3d_ctx_->RSSetViewports(1, &vp);
    const UINT stride = 2 * sizeof(float), offset = 0;
    d3d_ctx_->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    // vsSolid takes POSITION only; a matching input layout is required.
    D3D11_INPUT_ELEMENT_DESC elem {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
                                   D3D11_INPUT_PER_VERTEX_DATA, 0};
    ID3DBlob* blob = nullptr;
    ID3D11InputLayout* layout = nullptr;
    if (CompileShader("vsSolid", "vs_5_0", &blob) && blob) {
        d3d_device_->CreateInputLayout(&elem, 1, blob->GetBufferPointer(),
                                       blob->GetBufferSize(), &layout);
        blob->Release();
    }
    d3d_ctx_->IASetInputLayout(layout);
    d3d_ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d3d_ctx_->VSSetShader(vs_solid_, nullptr, 0);
    d3d_ctx_->PSSetShader(ps_solid_, nullptr, 0);
    setParams(0, 0.82f, 0.82f, 0.85f);
    d3d_ctx_->Draw(UINT(verts.size() / 2), 0);
    if (layout) layout->Release();
    vb->Release();
}

void View::releaseD3D() {
    d3d_ready_ = false;  // new fx/present calls bail out from here on
    // Wait out any in-flight render sequence (preview present or camera
    // effects pass) before releasing the textures and shaders it uses.
    std::lock_guard<std::recursive_mutex> fxlock(fx_mutex_);
    for (auto*& srv : stage_srv_) { if (srv) { srv->Release(); srv = nullptr; } }
    if (stage_) { stage_->Release(); stage_ = nullptr; stage_w_ = stage_h_ = 0; }
    if (samp_) { samp_->Release(); samp_ = nullptr; }
    if (params_) { params_->Release(); params_ = nullptr; }
    if (ps_solid_) { ps_solid_->Release(); ps_solid_ = nullptr; }
    if (ps_nv12_) { ps_nv12_->Release(); ps_nv12_ = nullptr; }
    if (cpu_srv_) { cpu_srv_->Release(); cpu_srv_ = nullptr; }
    if (cpu_stage_) { cpu_stage_->Release(); cpu_stage_ = nullptr; cpu_stage_w_ = cpu_stage_h_ = 0; }
    if (ps_bgra_) { ps_bgra_->Release(); ps_bgra_ = nullptr; }
    if (msaa_rtv_) { msaa_rtv_->Release(); msaa_rtv_ = nullptr; }
    if (msaa_tex_) { msaa_tex_->Release(); msaa_tex_ = nullptr; }
    msaa_w_ = msaa_h_ = msaa_active_ = 0;
    if (fx_in_srv_) { fx_in_srv_->Release(); fx_in_srv_ = nullptr; }
    if (fx_in_) { fx_in_->Release(); fx_in_ = nullptr; }
    if (fx_out_rtv_) { fx_out_rtv_->Release(); fx_out_rtv_ = nullptr; }
    if (fx_out_) { fx_out_->Release(); fx_out_ = nullptr; }
    if (fx_msaa_rtv_) { fx_msaa_rtv_->Release(); fx_msaa_rtv_ = nullptr; }
    if (fx_msaa_) { fx_msaa_->Release(); fx_msaa_ = nullptr; }
    if (fx_read_) { fx_read_->Release(); fx_read_ = nullptr; }
    fx_w_ = fx_h_ = 0;
    cpu_mips_ = stage_mips_ = false;
    if (rz_srv_) { rz_srv_->Release(); rz_srv_ = nullptr; }
    if (rz_rtv_) { rz_rtv_->Release(); rz_rtv_ = nullptr; }
    if (rz_tex_) { rz_tex_->Release(); rz_tex_ = nullptr; }
    rz_w_ = rz_h_ = 0;
    if (ps_lz_nv12h_) { ps_lz_nv12h_->Release(); ps_lz_nv12h_ = nullptr; }
    if (ps_lz_bgrah_) { ps_lz_bgrah_->Release(); ps_lz_bgrah_ = nullptr; }
    if (ps_lz_bgrav_) { ps_lz_bgrav_->Release(); ps_lz_bgrav_ = nullptr; }
    if (samp_point_) { samp_point_->Release(); samp_point_ = nullptr; }
    if (vs_solid_) { vs_solid_->Release(); vs_solid_ = nullptr; }
    if (vs_) { vs_->Release(); vs_ = nullptr; }
    if (rtv_) { rtv_->Release(); rtv_ = nullptr; }
    if (swap_) { swap_->Release(); swap_ = nullptr; }
    if (d3d_ctx_) { d3d_ctx_->Release(); d3d_ctx_ = nullptr; }
    if (d3d_device_) { d3d_device_->Release(); d3d_device_ = nullptr; }
    bb_w_ = bb_h_ = 0;
}

// ---------------------------------------------------------------------------
// D3D12 presentation engine. See the member block for the interop design.
// Every failure path leaves the caller free to fall back to D3D11.
// ---------------------------------------------------------------------------
bool View::initD3D12() {
    SDL_SysWMinfo wm {};
    SDL_VERSION(&wm.version);
    if (!SDL_GetWindowWMInfo(window_, &wm)) return false;
    const HWND hwnd = wm.info.win.window;
    auto bail = [this] {
        releaseD3D12();
        releaseD3D();
        return false;
    };

    // The D3D11 device comes first: the decoder shares it (D3D11VA), and
    // the interop copy runs on it. Same creation flags as the D3D11
    // presenter, minus the swapchain.
    if (!d3d_device_) {
        D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                     D3D11_CREATE_DEVICE_BGRA_SUPPORT |
                                         D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                     nullptr, 0, D3D11_SDK_VERSION, &d3d_device_,
                                     &level, &d3d_ctx_)))
            return false;
        if (FAILED(d3d_device_->QueryInterface(IID_PPV_ARGS(&dev5_11_))) || !dev5_11_ ||
            FAILED(d3d_ctx_->QueryInterface(IID_PPV_ARGS(&ctx4_11_))) || !ctx4_11_)
            return bail();
        if (IDXGIDevice1* dxdev1 = nullptr;
            SUCCEEDED(d3d_device_->QueryInterface(IID_PPV_ARGS(&dxdev1))) && dxdev1) {
            dxdev1->SetMaximumFrameLatency(1);
            dxdev1->Release();
        }
        ID3D11Multithread* mt = nullptr;
        if (SUCCEEDED(d3d_ctx_->QueryInterface(IID_PPV_ARGS(&mt))) && mt) {
            mt->SetMultithreadProtected(TRUE);
            mt->Release();
        }
    }

    IDXGIDevice* dxdev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory2* factory = nullptr;
    bool ok = SUCCEEDED(d3d_device_->QueryInterface(IID_PPV_ARGS(&dxdev))) &&
              SUCCEEDED(dxdev->GetAdapter(&adapter)) && adapter;
    if (ok)
        ok = SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0,
                                         IID_PPV_ARGS(&d12_))) && d12_;
    if (ok) {
        D3D12_COMMAND_QUEUE_DESC qd {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ok = SUCCEEDED(d12_->CreateCommandQueue(&qd, IID_PPV_ARGS(&d12_queue_)));
    }
    if (ok)
        ok = SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory))) && factory;
    IDXGISwapChain1* swap1 = nullptr;
    if (ok) {
        DXGI_SWAP_CHAIN_DESC1 desc {};
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.BufferCount = 2;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.SampleDesc.Count = 1;
        ok = SUCCEEDED(factory->CreateSwapChainForHwnd(d12_queue_, hwnd, &desc,
                                                       nullptr, nullptr, &swap1)) &&
             SUCCEEDED(swap1->QueryInterface(IID_PPV_ARGS(&swap12_)));
    }
    if (swap1) swap1->Release();
    if (dxdev) dxdev->Release();
    if (adapter) adapter->Release();
    if (factory) factory->Release();
    if (!ok) return bail();

    D3D12_DESCRIPTOR_HEAP_DESC hd {};
    hd.NumDescriptors = 4;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    if (FAILED(d12_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap12_)))) return bail();
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(d12_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srv_heap12_)))) return bail();
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    hd.NumDescriptors = 2;
    if (FAILED(d12_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&samp_heap12_)))) return bail();
    rtv_inc12_ = d12_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    srv_inc12_ = d12_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    samp_inc12_ = d12_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    {
        D3D12_SAMPLER_DESC sd {};
        sd.Filter = paniso_ > 1 ? D3D12_FILTER_ANISOTROPIC
                                : D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sd.MaxAnisotropy = paniso_ > 1 ? UINT(std::min(paniso_, 16)) : 1;
        sd.MaxLOD = 3.402823466e+38f;
        d12_->CreateSampler(&sd, samp_heap12_->GetCPUDescriptorHandleForHeapStart());
        D3D12_SAMPLER_DESC pd {};
        pd.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        pd.AddressU = pd.AddressV = pd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        pd.MaxLOD = 3.402823466e+38f;
        D3D12_CPU_DESCRIPTOR_HANDLE h = samp_heap12_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += samp_inc12_;
        d12_->CreateSampler(&pd, h);
    }

    {
        D3D12_DESCRIPTOR_RANGE srv_ranges[2] {};
        srv_ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srv_ranges[0].NumDescriptors = 2;
        srv_ranges[0].BaseShaderRegister = 0;
        srv_ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srv_ranges[1].NumDescriptors = 2;
        srv_ranges[1].BaseShaderRegister = 2;
        srv_ranges[1].OffsetInDescriptorsFromTableStart = 2;
        D3D12_DESCRIPTOR_RANGE samp_range {};
        samp_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        samp_range.NumDescriptors = 2;
        D3D12_ROOT_PARAMETER params[3] {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 2;
        params[0].DescriptorTable.pDescriptorRanges = srv_ranges;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges = &samp_range;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[2].Descriptor.ShaderRegister = 0;
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rd {};
        rd.NumParameters = 3;
        rd.pParameters = params;
        ID3DBlob* sig = nullptr;
        ID3DBlob* err = nullptr;
        const HRESULT hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1,
                                                       &sig, &err);
        if (err) err->Release();
        if (FAILED(hr) ||
            FAILED(d12_->CreateRootSignature(0, sig->GetBufferPointer(),
                                             sig->GetBufferSize(),
                                             IID_PPV_ARGS(&root12_)))) {
            if (sig) sig->Release();
            return bail();
        }
        sig->Release();
    }

    if (!CompileShader("vsMain", "vs_5_0", &vs_blob12_)) return bail();
    {
        const char* entries[5] = {"psNV12", "psBGRA", "psNV12LH", "psBGRALH", "psBGRALV"};
        for (int i = 0; i < 5; ++i)
            if (!CompileShader(entries[i], "ps_5_0", &ps_blob12_[i])) return bail();
    }

    {
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = 256 * 64;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(d12_->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr, IID_PPV_ARGS(&cb12_))))
            return bail();
        D3D12_RANGE no_read {0, 0};
        if (FAILED(cb12_->Map(0, &no_read, reinterpret_cast<void**>(&cb12_ptr_))))
            return bail();
    }
    for (int i = 0; i < 2; ++i)
        if (FAILED(d12_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                IID_PPV_ARGS(&alloc12_[i]))))
            return bail();
    if (FAILED(d12_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc12_[0],
                                       nullptr, IID_PPV_ARGS(&list12_))))
        return bail();
    list12_->Close();
    if (FAILED(d12_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence12_))))
        return bail();
    fence_event12_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!fence_event12_) return bail();

    d3d12_ready_ = true;
    stop_preview_ = false;
    preview_thread_ = std::thread([this] { previewLoop(); });
    std::fprintf(stderr, "[wireplay] preview renderer: Direct3D 12 (vsync off, D3D11VA decode via interop)\n");
    if (msaa_ > 0 || aniso_ > 0)
        std::fprintf(stderr,
                     "[wireplay] note: the camera effects pass runs on the D3D11 "
                     "presenter only; under D3D12 the camera publishes native\n");
    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE View::rtvAt12(int slot) const {
    D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_heap12_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += size_t(slot) * rtv_inc12_;
    return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE View::srvCpuAt12(int slot) const {
    D3D12_CPU_DESCRIPTOR_HANDLE h = srv_heap12_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += size_t(slot) * srv_inc12_;
    return h;
}

void View::barrier12(ID3D12Resource* res, D3D12_RESOURCE_STATES before,
                     D3D12_RESOURCE_STATES after) {
    if (before == after) return;
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list12_->ResourceBarrier(1, &b);
}

ID3D12PipelineState* View::psoFor12(int kind, int samples) {
    if (samples > 1) {
        if (pso12_msaa_samples_ != samples) {
            for (auto*& pso : pso12_msaa_) if (pso) { pso->Release(); pso = nullptr; }
            pso12_msaa_samples_ = samples;
        }
        if (pso12_msaa_[kind]) return pso12_msaa_[kind];
    } else if (pso12_[kind]) {
        return pso12_[kind];
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd {};
    pd.pRootSignature = root12_;
    pd.VS = {vs_blob12_->GetBufferPointer(), vs_blob12_->GetBufferSize()};
    pd.PS = {ps_blob12_[kind]->GetBufferPointer(), ps_blob12_[kind]->GetBufferSize()};
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.DepthStencilState.DepthEnable = FALSE;
    pd.DepthStencilState.StencilEnable = FALSE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
    pd.SampleDesc.Count = UINT(samples > 1 ? samples : 1);
    pd.SampleMask = UINT_MAX;
    ID3D12PipelineState* pso = nullptr;
    if (FAILED(d12_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)))) return nullptr;
    if (samples > 1) pso12_msaa_[kind] = pso; else pso12_[kind] = pso;
    return pso;
}

void View::setParams12(float slice, float r, float g, float b, float rx, float ry,
                       float rw, float rh, float tw, float th, float kf, float axis) {
    const int slot = cb12_slot_;
    cb12_slot_ = (cb12_slot_ + 1) % 64;
    const float values[16] = {slice, r, g, b, rx, ry, rw, rh,
                              tw, th, tw > 0 ? 1.0f / tw : 0.0f, th > 0 ? 1.0f / th : 0.0f,
                              kf, axis, 0.0f, 0.0f};
    std::memcpy(cb12_ptr_ + size_t(slot) * 256, values, sizeof(values));
    list12_->SetGraphicsRootConstantBufferView(
        2, cb12_->GetGPUVirtualAddress() + size_t(slot) * 256);
}

void View::drawPass12(int kind, D3D12_CPU_DESCRIPTOR_HANDLE rtv, int vp_w, int vp_h,
                      float rx, float ry, float rw, float rh, float tw, float th,
                      float kf, float axis, int samples) {
    ID3D12PipelineState* pso = psoFor12(kind, samples);
    if (!pso) return;
    list12_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp {};
    vp.Width = float(vp_w);
    vp.Height = float(vp_h);
    vp.MaxDepth = 1.0f;
    D3D12_RECT sc {0, 0, LONG(vp_w), LONG(vp_h)};
    list12_->RSSetViewports(1, &vp);
    list12_->RSSetScissorRects(1, &sc);
    list12_->SetGraphicsRootSignature(root12_);
    ID3D12DescriptorHeap* heaps[2] = {srv_heap12_, samp_heap12_};
    list12_->SetDescriptorHeaps(2, heaps);
    list12_->SetGraphicsRootDescriptorTable(0, srv_heap12_->GetGPUDescriptorHandleForHeapStart());
    list12_->SetGraphicsRootDescriptorTable(1, samp_heap12_->GetGPUDescriptorHandleForHeapStart());
    list12_->SetPipelineState(pso);
    list12_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    setParams12(0, 0, 0, 0, rx, ry, rw, rh, tw, th, kf, axis);
    list12_->DrawInstanced(4, 1, 0, 0);
}

void View::waitIdle12() {
    if (!d12_queue_ || !fence12_) return;
    const uint64_t v = ++fence_seq12_;
    if (FAILED(d12_queue_->Signal(fence12_, v))) return;
    if (fence12_->GetCompletedValue() < v) {
        fence12_->SetEventOnCompletion(v, fence_event12_);
        WaitForSingleObject(fence_event12_, INFINITE);
    }
}

bool View::ensureBackBuffer12() {
    if (!swap12_) return false;
    SDL_SysWMinfo wm {};
    SDL_VERSION(&wm.version);
    if (!SDL_GetWindowWMInfo(window_, &wm)) return false;
    RECT rc {};
    GetClientRect(wm.info.win.window, &rc);
    const int w = int(rc.right - rc.left), h = int(rc.bottom - rc.top);
    if (w <= 0 || h <= 0) return false;
    if (back12_[0] && w == bb12_w_ && h == bb12_h_) return true;
    waitIdle12();
    for (auto*& back : back12_) if (back) { back->Release(); back = nullptr; }
    if (msaa12_) { msaa12_->Release(); msaa12_ = nullptr; msaa12_w_ = msaa12_h_ = 0; msaa12_samples_ = 0; }
    D3D12_RESOURCE_DESC bd {};
    {
        ID3D12Resource* probe = nullptr;
        if (SUCCEEDED(swap12_->GetBuffer(0, IID_PPV_ARGS(&probe))) && probe) {
            bd = probe->GetDesc();
            probe->Release();
        }
    }
    if (int(bd.Width) != w || int(bd.Height) != h) {
        const HRESULT hr = swap12_->ResizeBuffers(0, UINT(w), UINT(h),
                                                  DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) {
            static bool logged = false;
            if (!logged) {
                logged = true;
                std::fprintf(stderr, "[wireplay] D3D12: ResizeBuffers failed (hr=0x%08lX)\n",
                             (unsigned long)hr);
            }
            return false;
        }
    }
    for (int i = 0; i < 2; ++i) {
        if (FAILED(swap12_->GetBuffer(UINT(i), IID_PPV_ARGS(&back12_[i])))) return false;
        d12_->CreateRenderTargetView(back12_[i], nullptr, rtvAt12(i));
    }
    bb12_w_ = w;
    bb12_h_ = h;
    ensureMsaa12();
    return true;
}

void View::ensureMsaa12() {
    if (msaa_ <= 1 || bb12_w_ <= 0) return;
    if (msaa12_ && msaa12_w_ == bb12_w_ && msaa12_h_ == bb12_h_) return;
    if (msaa12_) { msaa12_->Release(); msaa12_ = nullptr; }
    int chosen = 0;
    UINT quality = 0;
    for (int cand : {16, 8, 4, 2}) {
        if (cand > msaa_) continue;
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS data {};
        data.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        data.SampleCount = UINT(cand);
        if (SUCCEEDED(d12_->CheckFeatureSupport(
                D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &data, sizeof(data))) &&
            data.NumQualityLevels > 0) {
            chosen = cand;
            quality = data.NumQualityLevels - 1;
            break;
        }
    }
    if (chosen <= 1) { msaa12_samples_ = 0; return; }
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = UINT64(bb12_w_);
    rd.Height = UINT(bb12_h_);
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    rd.SampleDesc.Count = UINT(chosen);
    rd.SampleDesc.Quality = quality;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (FAILED(d12_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd,
                                             D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                             IID_PPV_ARGS(&msaa12_))))
        return;
    d12_->CreateRenderTargetView(msaa12_, nullptr, rtvAt12(2));
    msaa12_w_ = bb12_w_;
    msaa12_h_ = bb12_h_;
    if (msaa12_samples_ != chosen) {
        msaa12_samples_ = chosen;
        std::fprintf(stderr, "[wireplay] D3D12 MSAA active: x%d%s\n", chosen,
                     chosen < msaa_ ? " (requested more; GPU max)" : "");
    }
}

bool View::beginFrame12() {
    if (!ensureBackBuffer12()) return false;
    cur12_ = int(swap12_->GetCurrentBackBufferIndex());
    const uint64_t v = frame_fence_val12_[cur12_];
    if (v && fence12_->GetCompletedValue() < v) {
        fence12_->SetEventOnCompletion(v, fence_event12_);
        WaitForSingleObject(fence_event12_, INFINITE);
    }
    if (FAILED(alloc12_[cur12_]->Reset())) return false;
    if (FAILED(list12_->Reset(alloc12_[cur12_], nullptr))) return false;
    return true;
}

void View::finishPresent12(bool from_msaa) {
    ID3D12Resource* back = back12_[cur12_];
    if (from_msaa && msaa12_) {
        barrier12(msaa12_, D3D12_RESOURCE_STATE_RENDER_TARGET,
                  D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
        barrier12(back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RESOLVE_DEST);
        list12_->ResolveSubresource(back, 0, msaa12_, 0, DXGI_FORMAT_B8G8R8A8_UNORM);
        barrier12(back, D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_PRESENT);
        barrier12(msaa12_, D3D12_RESOURCE_STATE_RESOLVE_SOURCE,
                  D3D12_RESOURCE_STATE_RENDER_TARGET);
    } else {
        barrier12(back, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    }
    list12_->Close();
    ID3D12CommandList* lists[1] = {list12_};
    d12_queue_->ExecuteCommandLists(1, lists);
    swap12_->Present(0, 0);  // no vsync: the stream is the clock
    const uint64_t v = ++fence_seq12_;
    d12_queue_->Signal(fence12_, v);
    frame_fence_val12_[cur12_] = v;
}

bool View::ensureStage12(int w, int h) {
    if (stage12_ && stage12_w_ == w && stage12_h_ == h) return true;
    waitIdle12();
    if (stage12_) { stage12_->Release(); stage12_ = nullptr; }
    if (stage12_11_) { stage12_11_->Release(); stage12_11_ = nullptr; }
    stage12_w_ = stage12_h_ = 0;
    auto fail12 = [](const char* what, HRESULT hr) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            std::fprintf(stderr, "[wireplay] D3D12 interop: %s failed (hr=0x%08lX)\n",
                         what, (unsigned long)hr);
        }
        return false;
    };
    D3D11_TEXTURE2D_DESC td {};
    td.Width = UINT(w);
    td.Height = UINT(h);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_NV12;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    // NTHANDLE is not optional here: IDXGIResource1::CreateSharedHandle
    // (the only handle D3D12's OpenSharedHandle accepts) rejects a plain
    // MISC_SHARED resource with E_INVALIDARG -- field-proven.
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    HRESULT hr = d3d_device_->CreateTexture2D(&td, nullptr, &stage12_11_);
    if (FAILED(hr)) return fail12("CreateTexture2D(shared NV12)", hr);
    IDXGIResource1* res = nullptr;
    HANDLE handle = nullptr;
    hr = stage12_11_->QueryInterface(IID_PPV_ARGS(&res));
    if (SUCCEEDED(hr) && res)
        hr = res->CreateSharedHandle(
            nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
            &handle);
    if (res) res->Release();
    if (FAILED(hr) || !handle) return fail12("CreateSharedHandle(texture)", hr);
    hr = d12_->OpenSharedHandle(handle, IID_PPV_ARGS(&stage12_));
    CloseHandle(handle);
    if (FAILED(hr) || !stage12_) return fail12("OpenSharedHandle(texture)", hr);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv {};
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    sv.Format = DXGI_FORMAT_R8_UNORM;
    sv.Texture2D.PlaneSlice = 0;
    d12_->CreateShaderResourceView(stage12_, &sv, srvCpuAt12(0));
    sv.Format = DXGI_FORMAT_R8G8_UNORM;
    sv.Texture2D.PlaneSlice = 1;
    d12_->CreateShaderResourceView(stage12_, &sv, srvCpuAt12(1));
    if (!fence_i12_) {
        hr = d12_->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence_i12_));
        if (FAILED(hr)) return fail12("CreateFence(shared)", hr);
        HANDLE fh = nullptr;
        hr = d12_->CreateSharedHandle(fence_i12_, nullptr, GENERIC_ALL, nullptr, &fh);
        if (FAILED(hr) || !fh) return fail12("CreateSharedHandle(fence)", hr);
        hr = dev5_11_->OpenSharedFence(fh, IID_PPV_ARGS(&fence_i11_));
        CloseHandle(fh);
        if (FAILED(hr) || !fence_i11_) return fail12("OpenSharedFence", hr);
    }
    stage12_w_ = w;
    stage12_h_ = h;
    interop_val_ = 0;
    return true;
}

bool View::ensureCpu12(int w, int h) {
    if (cpu12_ && cpu12_w_ == w && cpu12_h_ == h) return true;
    waitIdle12();
    if (cpu12_) { cpu12_->Release(); cpu12_ = nullptr; }
    if (cpu12_up_) {
        cpu12_up_->Unmap(0, nullptr);
        cpu12_up_->Release();
        cpu12_up_ = nullptr;
        cpu12_ptr_ = nullptr;
    }
    cpu12_w_ = cpu12_h_ = 0;
    cpu12_pitch_ = (UINT(w) * 4 + 255) & ~255u;
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = UINT64(w);
    rd.Height = UINT(h);
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    rd.SampleDesc.Count = 1;
    if (FAILED(d12_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd,
                                             D3D12_RESOURCE_STATE_COMMON, nullptr,
                                             IID_PPV_ARGS(&cpu12_))))
        return false;
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = UINT64(cpu12_pitch_) * UINT(h);
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(d12_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd,
                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                             IID_PPV_ARGS(&cpu12_up_))))
        return false;
    D3D12_RANGE no_read {0, 0};
    if (FAILED(cpu12_up_->Map(0, &no_read, reinterpret_cast<void**>(&cpu12_ptr_))))
        return false;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv {};
    sv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    d12_->CreateShaderResourceView(cpu12_, &sv, srvCpuAt12(2));
    cpu12_state_ = D3D12_RESOURCE_STATE_COMMON;
    cpu12_w_ = w;
    cpu12_h_ = h;
    return true;
}

bool View::ensureRz12(int w, int h) {
    if (rz12_ && rz12_w_ == w && rz12_h_ == h) return true;
    waitIdle12();
    if (rz12_) { rz12_->Release(); rz12_ = nullptr; }
    rz12_w_ = rz12_h_ = 0;
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = UINT64(w);
    rd.Height = UINT(h);
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (FAILED(d12_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd,
                                             D3D12_RESOURCE_STATE_COMMON, nullptr,
                                             IID_PPV_ARGS(&rz12_))))
        return false;
    d12_->CreateRenderTargetView(rz12_, nullptr, rtvAt12(3));
    D3D12_SHADER_RESOURCE_VIEW_DESC sv {};
    sv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    d12_->CreateShaderResourceView(rz12_, &sv, srvCpuAt12(3));
    rz12_state_ = D3D12_RESOURCE_STATE_COMMON;
    rz12_w_ = w;
    rz12_h_ = h;
    return true;
}

bool View::presentGpu12(void* texture, int slice, int w, int h) {
    if (!d3d12_ready_ || !texture || w <= 0 || h <= 0 || !ctx4_11_) return false;
    auto* tex = static_cast<ID3D11Texture2D*>(texture);
    D3D11_TEXTURE2D_DESC td {};
    tex->GetDesc(&td);
    if (td.Format != DXGI_FORMAT_NV12) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            std::fprintf(stderr, "[wireplay] D3D12: decoder texture format %d, not NV12\n",
                         int(td.Format));
        }
        return false;
    }
    if (!ensureStage12(w, h)) return false;
    if (interop_val_) ctx4_11_->Wait(fence_i11_, interop_val_);
    d3d_ctx_->CopySubresourceRegion(stage12_11_, 0, 0, 0, 0, tex, UINT(slice), nullptr);
    ctx4_11_->Signal(fence_i11_, interop_val_ + 1);

    if (!beginFrame12()) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            std::fprintf(stderr, "[wireplay] D3D12: beginFrame12 failed\n");
        }
        return false;
    }
    d12_queue_->Wait(fence_i12_, interop_val_ + 1);
    const bool use_msaa = msaa12_samples_ > 1 && msaa12_;
    const D3D12_CPU_DESCRIPTOR_HANDLE target = use_msaa ? rtvAt12(2) : rtvAt12(cur12_);
    const int samples = use_msaa ? msaa12_samples_ : 1;
    const float black[4] = {0, 0, 0, 1};
    barrier12(stage12_, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (!use_msaa)
        barrier12(back12_[cur12_], D3D12_RESOURCE_STATE_PRESENT,
                  D3D12_RESOURCE_STATE_RENDER_TARGET);
    const double scale = std::min(double(bb12_w_) / w, double(bb12_h_) / h);
    const int dw = std::max(1, int(w * scale)), dh = std::max(1, int(h * scale));
    const float rx = 2.0f * ((bb12_w_ - dw) / 2) / bb12_w_ - 1.0f;
    const float ry = 1.0f - 2.0f * ((bb12_h_ - dh) / 2) / bb12_h_;
    const float rw = 2.0f * dw / bb12_w_, rh = 2.0f * dh / bb12_h_;
    bool drew = false;
    if (scaling_ == Options::Scaling::Lanczos && ensureRz12(dw, h)) {
        barrier12(rz12_, D3D12_RESOURCE_STATES(rz12_state_), D3D12_RESOURCE_STATE_RENDER_TARGET);
        drawPass12(2, rtvAt12(3), dw, h, -1.0f, 1.0f, 2.0f, 2.0f, float(w), float(h),
                   float(w) / float(dw), 0.0f, 1);
        barrier12(rz12_, D3D12_RESOURCE_STATE_RENDER_TARGET,
                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        rz12_state_ = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        list12_->ClearRenderTargetView(target, black, 0, nullptr);
        drawPass12(4, target, bb12_w_, bb12_h_, rx, ry, rw, rh, float(dw), float(h),
                   float(h) / float(dh), 1.0f, samples);
        drew = true;
    }
    if (!drew) {
        list12_->ClearRenderTargetView(target, black, 0, nullptr);
        drawPass12(0, target, bb12_w_, bb12_h_, rx, ry, rw, rh, 0.0f, 0.0f, 1.0f, 0.0f,
                   samples);
    }
    barrier12(stage12_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    finishPresent12(use_msaa);
    d12_queue_->Signal(fence_i12_, interop_val_ + 2);
    interop_val_ += 2;
    ++presented_;
    return true;
}

bool View::presentCpu12(const AVFrame* f) {
    if (!d3d12_ready_ || !f || !f->data[0] || f->format != AV_PIX_FMT_BGRA ||
        f->width <= 0 || f->height <= 0)
        return false;
    const int w = f->width, h = f->height;
    if (!ensureCpu12(w, h)) return false;
    for (int y = 0; y < h; ++y)
        std::memcpy(cpu12_ptr_ + size_t(y) * cpu12_pitch_,
                    f->data[0] + size_t(y) * f->linesize[0], size_t(w) * 4);
    if (!beginFrame12()) return false;
    barrier12(cpu12_, D3D12_RESOURCE_STATES(cpu12_state_), D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = cpu12_;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src {};
    src.pResource = cpu12_up_;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    src.PlacedFootprint.Footprint.Width = UINT(w);
    src.PlacedFootprint.Footprint.Height = UINT(h);
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = cpu12_pitch_;
    D3D12_BOX box {0, 0, 0, UINT(w), UINT(h), 1};
    list12_->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    barrier12(cpu12_, D3D12_RESOURCE_STATE_COPY_DEST,
              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cpu12_state_ = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    const bool use_msaa = msaa12_samples_ > 1 && msaa12_;
    const D3D12_CPU_DESCRIPTOR_HANDLE target = use_msaa ? rtvAt12(2) : rtvAt12(cur12_);
    const int samples = use_msaa ? msaa12_samples_ : 1;
    const float black[4] = {0, 0, 0, 1};
    if (!use_msaa)
        barrier12(back12_[cur12_], D3D12_RESOURCE_STATE_PRESENT,
                  D3D12_RESOURCE_STATE_RENDER_TARGET);
    const double scale = std::min(double(bb12_w_) / w, double(bb12_h_) / h);
    const int dw = std::max(1, int(w * scale)), dh = std::max(1, int(h * scale));
    const float rx = 2.0f * ((bb12_w_ - dw) / 2) / bb12_w_ - 1.0f;
    const float ry = 1.0f - 2.0f * ((bb12_h_ - dh) / 2) / bb12_h_;
    const float rw = 2.0f * dw / bb12_w_, rh = 2.0f * dh / bb12_h_;
    bool drew = false;
    if (scaling_ == Options::Scaling::Lanczos && ensureRz12(dw, h)) {
        barrier12(rz12_, D3D12_RESOURCE_STATES(rz12_state_), D3D12_RESOURCE_STATE_RENDER_TARGET);
        drawPass12(3, rtvAt12(3), dw, h, -1.0f, 1.0f, 2.0f, 2.0f, float(w), float(h),
                   float(w) / float(dw), 0.0f, 1);
        barrier12(rz12_, D3D12_RESOURCE_STATE_RENDER_TARGET,
                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        rz12_state_ = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        list12_->ClearRenderTargetView(target, black, 0, nullptr);
        drawPass12(4, target, bb12_w_, bb12_h_, rx, ry, rw, rh, float(dw), float(h),
                   float(h) / float(dh), 1.0f, samples);
        drew = true;
    }
    if (!drew) {
        list12_->ClearRenderTargetView(target, black, 0, nullptr);
        drawPass12(1, target, bb12_w_, bb12_h_, rx, ry, rw, rh, 0.0f, 0.0f, 1.0f, 0.0f,
                   samples);
    }
    finishPresent12(use_msaa);
    ++presented_;
    return true;
}

void View::presentBlank12() {
    if (!d3d12_ready_ || !beginFrame12()) return;
    const float black[4] = {0, 0, 0, 1};
    const D3D12_CPU_DESCRIPTOR_HANDLE target = rtvAt12(cur12_);
    barrier12(back12_[cur12_], D3D12_RESOURCE_STATE_PRESENT,
              D3D12_RESOURCE_STATE_RENDER_TARGET);
    list12_->ClearRenderTargetView(target, black, 0, nullptr);
    finishPresent12(false);
}

void View::releaseD3D12() {
    const bool had12 = d3d12_ready_;
    d3d12_ready_ = false;
    if (d12_queue_ && fence12_) waitIdle12();
    (void)had12;
    if (stage12_) { stage12_->Release(); stage12_ = nullptr; }
    if (stage12_11_) { stage12_11_->Release(); stage12_11_ = nullptr; }
    if (fence_i11_) { fence_i11_->Release(); fence_i11_ = nullptr; }
    if (fence_i12_) { fence_i12_->Release(); fence_i12_ = nullptr; }
    if (ctx4_11_) { ctx4_11_->Release(); ctx4_11_ = nullptr; }
    if (dev5_11_) { dev5_11_->Release(); dev5_11_ = nullptr; }
    if (cpu12_) { cpu12_->Release(); cpu12_ = nullptr; }
    if (cpu12_up_) {
        cpu12_up_->Unmap(0, nullptr);
        cpu12_up_->Release();
        cpu12_up_ = nullptr;
        cpu12_ptr_ = nullptr;
    }
    if (rz12_) { rz12_->Release(); rz12_ = nullptr; }
    if (msaa12_) { msaa12_->Release(); msaa12_ = nullptr; }
    for (auto*& back : back12_) if (back) { back->Release(); back = nullptr; }
    for (auto*& pso : pso12_msaa_) if (pso) { pso->Release(); pso = nullptr; }
    for (auto*& pso : pso12_) if (pso) { pso->Release(); pso = nullptr; }
    for (auto*& blob : ps_blob12_) if (blob) { blob->Release(); blob = nullptr; }
    if (vs_blob12_) { vs_blob12_->Release(); vs_blob12_ = nullptr; }
    if (cb12_) {
        cb12_->Unmap(0, nullptr);
        cb12_->Release();
        cb12_ = nullptr;
        cb12_ptr_ = nullptr;
    }
    if (list12_) { list12_->Release(); list12_ = nullptr; }
    for (auto*& alloc : alloc12_) if (alloc) { alloc->Release(); alloc = nullptr; }
    if (fence_event12_) { CloseHandle(fence_event12_); fence_event12_ = nullptr; }
    if (fence12_) { fence12_->Release(); fence12_ = nullptr; }
    if (root12_) { root12_->Release(); root12_ = nullptr; }
    if (samp_heap12_) { samp_heap12_->Release(); samp_heap12_ = nullptr; }
    if (srv_heap12_) { srv_heap12_->Release(); srv_heap12_ = nullptr; }
    if (rtv_heap12_) { rtv_heap12_->Release(); rtv_heap12_ = nullptr; }
    if (swap12_) { swap12_->Release(); swap12_ = nullptr; }
    if (d12_queue_) { d12_queue_->Release(); d12_queue_ = nullptr; }
    if (d12_) { d12_->Release(); d12_ = nullptr; }
    bb12_w_ = bb12_h_ = 0;
    msaa12_w_ = msaa12_h_ = msaa12_samples_ = 0;
    stage12_w_ = stage12_h_ = 0;
    cpu12_w_ = cpu12_h_ = 0;
    rz12_w_ = rz12_h_ = 0;
    interop_val_ = 0;
    pso12_msaa_samples_ = 0;
}


#endif  // _WIN32

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
std::string jsonString(const std::string& s, const char* key) {
    const std::string needle = std::string("\"") + key + "\":\"";
    const size_t at = s.find(needle);
    if (at == std::string::npos) return {};
    const size_t b = at + needle.size();
    const size_t e = s.find('"', b);
    return e == std::string::npos ? s.substr(b) : s.substr(b, e - b);
}

std::string hex(const uint8_t* p, size_t n) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < n && i < 8; ++i) {
        out.push_back(digits[p[i] >> 4]);
        out.push_back(digits[p[i] & 0xF]);
    }
    return out;
}

std::vector<std::string> buildEngineArgv(const Options& opt) {
    std::vector<std::string> argv;
    if (!opt.engine_cmd.empty()) {
        argv = wp::splitCommandLine(opt.engine_cmd);
    } else {
        // Installed layout: python and the engine module ship next to the exe.
        argv = {"wireplay-engine"};
    }
    if (!opt.udid.empty()) { argv.push_back("--udid"); argv.push_back(opt.udid); }
    if (opt.no_audio) argv.push_back("--no-audio");
    argv.push_back("--backend");
    switch (opt.backend) {
        case wp::BackendPreference::kCoreDevice: argv.push_back("coredevice"); break;
        case wp::BackendPreference::kQuickTime:  argv.push_back("quicktime"); break;
        default:                                 argv.push_back("auto"); break;
    }
    if (opt.verbose) argv.push_back("-v");
    return argv;
}

// ---------------------------------------------------------------------------
// main loop
// ---------------------------------------------------------------------------
#ifdef _WIN32
// ---------------------------------------------------------------------------
// usbmux preflight (de-Pythonization step 2): app-lifecycle work that used
// to live in the engine before the transport opened. With Apple's usbmux
// service down, the phone exposes the capture configuration and then never
// starts the session -- silence in every open-sequence variant probed. So:
// check 127.0.0.1:27015 before launching the engine, try to start (or, when
// running-but-deaf, restart) the Apple Mobile Device Service, and fail
// loudly here instead of hanging inside the stream. The engine keeps its
// own check as a loud fatal (defense in depth); this just shortens the
// failure path and keeps service management out of the media process.
// ---------------------------------------------------------------------------
static bool usbmuxReachable(int timeout_ms) {
    static const bool wsa_ok = [] {
        WSADATA w{};
        return WSAStartup(MAKEWORD(2, 2), &w) == 0;
    }();
    if (!wsa_ok) return false;
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return false;
    u_long nonblock = 1;
    ioctlsocket(sock, FIONBIO, &nonblock);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(27015);
    addr.sin_addr.s_addr = htonl(0x7F000001u);  // 127.0.0.1
    bool ok = false;
    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
        ok = true;
    } else if (WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set wr;
        FD_ZERO(&wr);
        FD_SET(sock, &wr);
        timeval tv{};
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        if (select(0, nullptr, &wr, nullptr, &tv) > 0) {
            int err = 0;
            int len = sizeof(err);
            getsockopt(sock, SOL_SOCKET, SO_ERROR,
                       reinterpret_cast<char*>(&err), &len);
            ok = (err == 0);
        }
    }
    closesocket(sock);
    return ok;
}

static void tryStartAppleService() {
    // Best-effort, like the engine's version was: needs elevation to
    // actually work; when it cannot, the caller's message says what to
    // do by hand. Apple has renamed this service across bundles
    // (iTunes -> Apple Devices), so try every name it has shipped under.
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return;
    const wchar_t* names[] = {L"Apple Mobile Device Service",
                              L"AppleMobileDeviceService"};
    for (const wchar_t* name : names) {
        SC_HANDLE svc = OpenServiceW(
            scm, name, SERVICE_QUERY_STATUS | SERVICE_START | SERVICE_STOP);
        if (!svc) continue;  // no service by this name on this machine
        SERVICE_STATUS_PROCESS st{};
        DWORD needed = 0;
        if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                                 reinterpret_cast<BYTE*>(&st), sizeof(st),
                                 &needed)) {
            if (st.dwCurrentState == SERVICE_RUNNING) {
                // Running but not answering the socket: wedged. A restart
                // is the documented un-wedge.
                SERVICE_STATUS ss{};
                if (ControlService(svc, SERVICE_CONTROL_STOP, &ss)) {
                    for (int i = 0; i < 40; ++i) {
                        Sleep(250);
                        if (!QueryServiceStatusEx(
                                svc, SC_STATUS_PROCESS_INFO,
                                reinterpret_cast<BYTE*>(&st), sizeof(st),
                                &needed))
                            break;
                        if (st.dwCurrentState == SERVICE_STOPPED) break;
                    }
                }
            }
            StartServiceW(svc, 0, nullptr);
        }
        CloseServiceHandle(svc);
        break;  // the first name that exists is the service
    }
    CloseServiceHandle(scm);
}

static bool ensureUsbmuxPreflight() {
    if (usbmuxReachable(2000)) return true;
    std::fprintf(stderr,
                 "[wireplay] usbmux (Apple Mobile Device Service) is not "
                 "responding; trying to start it\n");
    tryStartAppleService();
    for (int waited = 0; waited < 15000; waited += 500) {
        if (usbmuxReachable(1000)) {
            std::fprintf(stderr, "[wireplay] usbmux is responding now\n");
            return true;
        }
        Sleep(500);
    }
    return false;
}
#else
static bool ensureUsbmuxPreflight() { return true; }
#endif

int run(const Options& opt) {
    View view;
    if (!opt.headless) {
        if (!view.open(opt)) return 1;
        view.message("Conectando...");
    } else if (SDL_Init(opt.no_audio ? SDL_INIT_VIDEO : (SDL_INIT_VIDEO | SDL_INIT_AUDIO)) != 0) {
        std::fprintf(stderr, "[wireplay] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    // Where frames come from, in priority order:
    //   --offline FILE  replay a recorded dump
    //   --stdin         someone piped the engine into us (the dev workflow)
    //   otherwise       we launch the engine ourselves
    wp::EngineProcess engine;
    FILE* engine_stdin = nullptr;
    FILE* engine_stdout = nullptr;
    FILE* offline_file = nullptr;
    if (!opt.offline.empty()) {
        // FrameReader cannot take an ifstream: hasPending() polls the file
        // descriptor of the underlying pipe. So this stays a FILE*.
        //
        // Opened directly rather than by redirecting stdin. This used to
        // freopen() the file onto stdin, which fails outright in a
        // WIN32_EXECUTABLE process: there is no console, so stdin is not a
        // usable stream to redirect. The window opened first and then closed
        // with no message anywhere, which is indistinguishable from a crash.
        // FrameReader only ever needed a FILE*, so the detour bought nothing.
#ifdef _MSC_VER
        const bool opened =
            fopen_s(&offline_file, opt.offline.c_str(), "rb") == 0 && offline_file;
#else
        offline_file = std::fopen(opt.offline.c_str(), "rb");
        const bool opened = offline_file != nullptr;
#endif
        if (!opened) {
            std::fprintf(stderr, "[wireplay] cannot open %s\n", opt.offline.c_str());
            return 1;
        }
        engine_stdout = offline_file;
    } else if (opt.use_stdin) {
        engine_stdout = stdin;
        engine_stdin = stdout;   // commands travel back down the same pipe pair
    } else {
        if (!ensureUsbmuxPreflight()) {
            std::fprintf(stderr,
                         "[wireplay] Apple's usbmux service (usbmuxd) is not "
                         "responding on 127.0.0.1:27015 and could not be "
                         "started. The phone cannot start a mirroring "
                         "session without it. Start the 'Apple Mobile Device "
                         "Service' (or reinstall the Apple Devices app / "
                         "iTunes) and try again.\n");
            return 1;
        }
        const std::vector<std::string> argv = buildEngineArgv(opt);
        if (!engine.start(argv)) {
            std::fprintf(stderr, "[wireplay] cannot start the engine: %s\n",
                         engine.error().c_str());
            std::fprintf(stderr,
                         "[wireplay] pass --engine \"PY wireplay_engine.py\" or put "
                         "wireplay-engine on PATH\n");
            return 1;
        }
        engine_stdout = engine.fromEngine();
        engine_stdin = engine.toEngine();
    }
    wp::setBinaryMode(engine_stdout);
    if (engine_stdin) wp::setBinaryMode(engine_stdin);

    wp::FrameReader reader(engine_stdout);

    // Opened on the first kVideoInit, which is where the engine declares the
    // codec: CoreDevice streams HEVC, the QuickTime USB protocol streams H.264,
    // and everything downstream of the decoder is identical for both.
    wp::VideoDecoder decoder;
    view.setHwMutex(&decoder.hwMutex());
    bool decoder_open = false;

    AudioOut audio;
    if (!opt.no_audio && !opt.headless) audio.open();
    audio.setMaxQueue(opt.max_queue_ms);
    audio.setDelayMs(int(opt.audio_delay_ms));

    // The virtual camera pulls from this bus; OBS loads the filter in its own
    // process. A failure here is not fatal: the window still works, and the
    // user may not have installed the camera at all.
    wp::FrameBus camera;
    if (!opt.no_camera) {
        if (camera.create(wp::defaultBusName())) {
            std::fprintf(stderr, "[wireplay] virtual camera bus ready\n");
        } else {
            std::fprintf(stderr, "[wireplay] virtual camera unavailable: %s\n",
                         camera.error().c_str());
        }
    }

    // std::ofstream rather than FILE*: the destructor closes it on every exit
    // path, so there is no fclose to forget and no fopen to deprecate.
    std::ofstream video_dump;
    if (!opt.dump_video.empty()) {
        video_dump.open(opt.dump_video, std::ios::binary);
        if (!video_dump)
            std::fprintf(stderr, "[wireplay] cannot open %s\n", opt.dump_video.c_str());
    }

    // Camera publishing runs on its own thread, latest frame wins. The
    // stream thread used to do a 13 MB bus copy under a cross-process
    // mutex inline; at 60 fps that (plus the conversion) ate a visible
    // slice of the frame budget, the engine queue backed up, and a
    // 100 ms consumer hiccup turned into 500 ms of standing latency.
    // Now the stream thread only refs the frame across (nearly free);
    // the copy and any consumer-side waiting happen here, and a slow
    // OBS can only ever cost the camera frames, never stream latency.
    AVFrame* camera_pending = nullptr;
    int camera_pending_fps = 60;
    // Camera-thread telemetry (read and reset by the 5 s status line): a frame
    // replaced before the thread took it is a frame the camera never saw, and
    // the maxima say whether the readback under the shared D3D11 device lock
    // is what the stream thread ends up waiting behind.
    std::atomic<uint64_t> cam_replaced {0};
    std::atomic<int64_t> cam_lock_wait_max_us {0}, cam_readback_max_us {0},
        cam_total_max_us {0};
    auto atomicMaxUs = [](std::atomic<int64_t>& a, int64_t v) {
        int64_t cur = a.load();
        while (v > cur && !a.compare_exchange_weak(cur, v)) {
        }
    };
    auto usSince = [](std::chrono::steady_clock::time_point a,
                      std::chrono::steady_clock::time_point b) {
        return int64_t(std::chrono::duration_cast<std::chrono::microseconds>(b - a).count());
    };
    // Output effects reach the camera unless explicitly opted out.
    const bool camera_fx =
        (opt.msaa > 0 || opt.aniso > 0) && !opt.no_camera_effects;
    if (opt.msaa > 0 || opt.aniso > 0) {
        std::fprintf(stderr,
                     "[wireplay] output effects: msaa=%d aniso=%d (preview%s)\n",
                     opt.msaa, opt.aniso,
                     camera_fx ? " + camera" : "; camera opted out");
    }
    std::mutex camera_mutex;
    std::condition_variable camera_cv;
    bool camera_stop = false;
    std::thread camera_thread;
    if (camera.valid()) {
        camera_thread = std::thread([&] {
            // Frames arrive either ready-made BGRA (CPU paths) or as
            // D3D11 GPU surfaces (the zero-copy preview path). GPU
            // frames are read back and converted HERE, on this thread:
            // av_hwframe_transfer_data + sws_scale for a 2688x1242
            // frame cost ~14 ms, and doing them on the stream thread
            // starved the pipe (field: queue saturated, drops, 200+
            // ms of latency). If this thread falls behind it simply
            // publishes the newest frame; the stream never waits.
            AVFrame* stage = av_frame_alloc();
            AVFrame* bgra = av_frame_alloc();
            AVFrame* bgra_fx = av_frame_alloc();
            SwsContext* sws = nullptr;
            int sws_w = 0, sws_h = 0;
            AVPixelFormat sws_fmt = AV_PIX_FMT_NONE;
            for (;;) {
                AVFrame* fr = nullptr;
                int fps = 60;
                {
                    std::unique_lock<std::mutex> lock(camera_mutex);
                    camera_cv.wait(lock, [&] { return camera_stop || camera_pending; });
                    if (camera_pending) {
                        fr = camera_pending;
                        camera_pending = nullptr;
                        fps = camera_pending_fps;
                    }
                }
                if (fr) {
                    const auto cam_t0 = std::chrono::steady_clock::now();
                    const AVFrame* src = fr;
                    bool have = fr->data[0] != nullptr;
                    if (have && fr->hw_frames_ctx) {
                        av_frame_unref(stage);
                        const auto lock_t0 = std::chrono::steady_clock::now();
                        std::lock_guard<std::recursive_mutex> lk(decoder.hwMutex());
                        const auto hw_t0 = std::chrono::steady_clock::now();
                        have = av_hwframe_transfer_data(stage, fr, 0) == 0;
                        const auto hw_t1 = std::chrono::steady_clock::now();
                        atomicMaxUs(cam_lock_wait_max_us, usSince(lock_t0, hw_t0));
                        atomicMaxUs(cam_readback_max_us, usSince(hw_t0, hw_t1));
                        src = stage;
                    }
                    if (have && src->data[0]) {
                        const AVFrame* ready = nullptr;
                        if (src->format == AV_PIX_FMT_BGRA) {
                            ready = src;
                        } else {
                            if (!sws || sws_w != src->width || sws_h != src->height ||
                                sws_fmt != AVPixelFormat(src->format)) {
                                sws_freeContext(sws);
                                sws = sws_getContext(
                                    src->width, src->height, AVPixelFormat(src->format),
                                    src->width, src->height, AV_PIX_FMT_BGRA,
                                    SWS_BILINEAR, nullptr, nullptr, nullptr);
                                sws_w = src->width;
                                sws_h = src->height;
                                sws_fmt = AVPixelFormat(src->format);
                            }
                            av_frame_unref(bgra);
                            bgra->format = AV_PIX_FMT_BGRA;
                            bgra->width = src->width;
                            bgra->height = src->height;
                            if (sws && av_frame_get_buffer(bgra, 32) == 0) {
                                sws_scale(sws, src->data, src->linesize, 0, src->height,
                                          bgra->data, bgra->linesize);
                                ready = bgra;
                            }
                        }
                        if (ready) {
                            // Output effects on the camera too, unless
                            // --no-camera-effects: the frame runs through
                            // the D3D11 output stage (mips+aniso/MSAA) at
                            // native resolution before publishing. If the
                            // pass is unavailable (non-D3D11 renderer) the
                            // untouched frame publishes, as always.
                            if (camera_fx && view.fxBgra(ready, bgra_fx)) {
                                camera.publish(bgra_fx->data[0], bgra_fx->width,
                                               bgra_fx->height, bgra_fx->linesize[0],
                                               fps, 1);
                            } else {
                                camera.publish(ready->data[0], ready->width,
                                               ready->height, ready->linesize[0],
                                               fps, 1);
                            }
                        }
                    }
                    atomicMaxUs(cam_total_max_us,
                                usSince(cam_t0, std::chrono::steady_clock::now()));
                    av_frame_free(&fr);
                }
                if (camera_stop) break;
            }
            sws_freeContext(sws);
            av_frame_free(&stage);
            av_frame_free(&bgra);
            av_frame_free(&bgra_fx);
        });
    }

    // Per-stage main-loop timing, printed with the 5 s status line.
    // Exists because "the app got slower with X on" kept being argued
    // from queue counters; this says where the milliseconds went.
    double stage_decode_ms = 0.0, stage_audio_ms = 0.0;
    // Averages hide stalls: a 150 ms hole every 10th frame moves a mean
    // by 1.5 ms. These are the worst cases and the stall counts, per window.
    double stage_decode_max_ms = 0.0, stage_audio_max_ms = 0.0;
    uint64_t loop_rx_gt25 = 0, loop_busy_gt25 = 0;
    double loop_rx_max_ms = 0.0, loop_busy_max_ms = 0.0;
    bool loop_have_prev = false;
    std::chrono::steady_clock::time_point loop_t_read {};
    uint64_t stage_video_n = 0, stage_audio_n = 0;
    auto stage_now = [] {
        return std::chrono::steady_clock::now();
    };
    auto stage_elapsed = [](std::chrono::steady_clock::time_point a) {
        return std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now() - a)
            .count();
    };

    uint32_t last_audio_ts = 0;
    const uint64_t t0 = SDL_GetTicks64();
    uint64_t last_log = t0;
    // Measured camera rate (frames published in the last second, refreshed
    // once per second). Starts at 60, the long-standing default.
    int camera_fps_num = 60;
    uint64_t camera_window_start = t0;
    uint64_t camera_window_frames = 0;
    int rc = 0;
    bool running = true;

    const bool engine_is_child = opt.offline.empty() && !opt.use_stdin;

    std::fprintf(stderr, "[wireplay] pipe wait: %s\n",
                 opt.pipe_wait_event ? "event (sleeps between frames)"
                                     : "poll (1 ms spin)");
    wp::Frame f;
    while (running) {
        // Wait for the next frame WITHOUT going deaf to the window. This
        // loop used to be a single blocking reader.read(): SDL events --
        // including the click on the X -- were only pumped after a frame
        // arrived, so with a silent phone the window froze for seconds
        // and the close click sat queued until the next status frame.
        // Now events are pumped every few ms while the pipe is empty.
        bool stream_over = false;
        while (!reader.hasPending()) {
            if (!opt.headless && !view.pumpEvents()) { stream_over = true; break; }
            if (engine_is_child && engine.exited(nullptr)) { stream_over = true; break; }
            if (opt.pipe_wait_event && !opt.headless) {
                // Sleep until SDL has an event or 5 ms pass, whichever
                // comes first. An arrived event is pushed back so the
                // regular pumpEvents() above consumes it in order; pipe
                // bytes are noticed on the next hasPending() re-check.
                // Replaces a 1000 wakeups/second spin between frames.
                SDL_Event ev;
                if (SDL_WaitEventTimeout(&ev, 5) == 1) SDL_PushEvent(&ev);
            } else {
                SDL_Delay(1);  // --pipe-wait poll: aggressive 1 ms spin
            }
        }
        if (stream_over) break;
        if (!reader.read(f)) break;
        {
            // Read-to-read gap: the wait for the pipe PLUS the previous
            // frame's processing. Compared with the busy time below, it says
            // whether the app was stuck (busy ~ gap) or starved (busy small).
            const auto rd = std::chrono::steady_clock::now();
            if (loop_have_prev) {
                const double gap_ms =
                    std::chrono::duration<double, std::milli>(rd - loop_t_read).count();
                if (gap_ms > 25.0) ++loop_rx_gt25;
                if (gap_ms > loop_rx_max_ms) loop_rx_max_ms = gap_ms;
            }
            loop_t_read = rd;
            loop_have_prev = true;
        }
        if (!opt.headless && !view.pumpEvents()) break;

        switch (f.type) {
        case wp::kVideoInit: {
            const uint8_t* p = f.payload;
            size_t rem = f.size;
            uint32_t w = 0, h = 0, flags = 0, codec = 0;
            if (!wp::readU32BE(p, rem, w) || !wp::readU32BE(p, rem, h) ||
                !wp::readU32BE(p, rem, flags) || !wp::readU32BE(p, rem, codec)) {
                break;
            }
            if (!decoder_open) {
                const wp::VideoCodec want =
                    codec == wp::kCodecH264 ? wp::kCodecH264 : wp::kCodecHevc;
                // Zero-copy: decode on the window's D3D11 device so the
                // preview samples the decoder's NV12 texture directly.
                // The CPU/BGRA copy is produced only when the virtual
                // camera is there to consume it.
                decoder.setD3D11Device(view.d3dDevice(), view.d3dContext());
                // No CPU copy on this thread for ANY threaded renderer:
                // the readback + BGRA conversion cost ~14 ms of every
                // 16.7 ms frame here (field-measured), leaving the pipe
                // to back up behind it -- the jitter, standing delay and
                // audio crackle the GL/Vulkan/CUDA modes kept after the
                // D3D11 path was cured. The preview thread samples GPU
                // frames directly (D3D11) or converts them there
                // (GL/Vulkan/CUDA); the camera thread converts from the
                // GPU frame itself, at its own pace. Only the SDL
                // fallback renderer, which presents inline on this
                // thread, still needs the decoder's own CPU copy.
                decoder.setCpuCopy(!(view.d3dReady() || view.d3d12Ready() ||
                                     view.vulkanReady() || view.glReady()));
                {
                    // NVDEC is an explicit opt-in (--decoder cuda):
                    // same decode silicon as D3D11VA on NVIDIA, but
                    // its frames live in CUDA memory and must return
                    // through the CPU, giving up the zero-copy
                    // preview. Auto mode therefore stays on D3D11VA
                    // and merely reports the adapter it found.
                    const bool want_cuda = opt.decoder_pref == 2;
                    decoder.setPreferCuda(want_cuda);
#ifdef _WIN32
                    if (auto* dev = static_cast<ID3D11Device*>(view.d3dDevice())) {
                        IDXGIDevice* dxdev = nullptr;
                        IDXGIAdapter* adapter = nullptr;
                        if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dxdev))) &&
                            SUCCEEDED(dxdev->GetAdapter(&adapter)) && adapter) {
                            DXGI_ADAPTER_DESC desc {};
                            if (SUCCEEDED(adapter->GetDesc(&desc))) {
                                std::fprintf(stderr,
                                             "[wireplay] GPU adapter: %ls (vendor 0x%04X)%s\n",
                                             desc.Description, desc.VendorId,
                                             desc.VendorId == 0x10DE
                                                 ? " -- NVIDIA; NVDEC available via --decoder cuda"
                                                 : "");
                            }
                            adapter->Release();
                        }
                        if (dxdev) dxdev->Release();
                    }
#endif
                }
                if (!decoder.open(want, opt.threads)) {
                    std::fprintf(stderr, "[wireplay] cannot open the %s decoder\n",
                                 want == wp::kCodecH264 ? "H.264" : "HEVC");
                    rc = 1;
                    running = false;
                    break;
                }
                decoder_open = true;
                std::fprintf(stderr, "[wireplay] decoder: %s (%s)\n",
                             want == wp::kCodecH264 ? "H.264" : "HEVC",
                             decoder.stats().hwaccel.c_str());
                if (opt.decoder_pref == 2 && !decoder.usingCuda()) {
                    // --decoder cuda was asked for and silently fell
                    // back (typically an FFmpeg build without the CUDA
                    // hwaccel compiled in). Say so out loud: a field
                    // session "testing NVDEC" that never engaged it
                    // once produced a phantom bug report.
                    std::fprintf(stderr,
                                 "[wireplay] WARNING: --decoder cuda requested, but "
                                 "NVDEC did not engage (this FFmpeg build likely "
                                 "lacks CUDA); using %s\n",
                                 decoder.stats().hwaccel.c_str());
                }
            }
            // Deliberately not parsed here. The geometry, profile and level all
            // come from the decoded first frame below, where FFmpeg -- which
            // actually has to decode the bitstream -- is the authority. A
            // hand-rolled SPS reader would be a second, independent opinion
            // that could disagree, and a wrong one sizes the window wrong.
            std::fprintf(stderr, "[wireplay] parameter sets: %u bytes, flags=%u\n",
                         uint32_t(rem), flags);
            break;
        }

        case wp::kVideoAU: {
            if (f.size < 6) break;
            if (!decoder_open) break;  // access unit before kVideoInit
            // Byte 4 is the keyframe flag and byte 5 the flags byte
            // (protocol: >I ts, B key, B flags). This used to read only
            // byte 5 -- which every sender writes as 0 -- so the
            // decoder was NEVER flushed on keyframes, and after any
            // dropped access unit it kept predicting from references
            // the sender had already discarded: damage that grows with
            // motion and survives until a self-sufficient frame.
            const uint8_t flags = f.payload[5] | f.payload[4];
            const uint8_t* data = f.payload + 6;
            const size_t size = f.size - 6;
            // Bytes 0..3 are the device's presentation time (90-kilohertz,
            // big-endian). They were written by every backend and read
            // by nobody; without them the preview cannot present on
            // the stream's cadence, only on arrival.
            const uint32_t pts90k = (uint32_t(f.payload[0]) << 24) |
                                    (uint32_t(f.payload[1]) << 16) |
                                    (uint32_t(f.payload[2]) << 8) |
                                    uint32_t(f.payload[3]);
            if (video_dump)
                video_dump.write(reinterpret_cast<const char*>(data), std::streamsize(size));

            const auto decode_t0 = stage_now();
            const int send_rc =
                decoder.sendAccessUnit(data, size, (flags & 1) != 0, int64_t(pts90k));
            const double dec_ms = stage_elapsed(decode_t0);
            stage_decode_ms += dec_ms;
            if (dec_ms > stage_decode_max_ms) stage_decode_max_ms = dec_ms;
            ++stage_video_n;
            if (send_rc <= 0) break;
            AVFrame* fr = decoder.frame();
            AVFrame* gpufr = decoder.gpuFrame();
            const int fw = (gpufr && gpufr->width > 0) ? gpufr->width : fr->width;
            const int fh = (gpufr && gpufr->height > 0) ? gpufr->height : fr->height;
            if (fw != view.width() || fh != view.height()) {
                // The decoded frame is the authority on geometry: it is the
                // crop FFmpeg actually applied, and it tracks rotation.
                std::fprintf(stderr, "[wireplay] geometry %dx%d -> %dx%d\n", view.width(),
                             view.height(), fw, fh);
                view.setGeometry(fw, fh);
            }
            // F8 pauses the preview only. The virtual camera is fed from
            // the decoded frame, at the phone's full resolution, whether
            // the preview is on screen, paused, minimised or maximised --
            // OBS must never inherit the preview window's state.
            if (!view.previewPaused()) {
                // GPU frames go to the preview thread (vsync must never
                // block this loop); the CPU path is the fallback for
                // software decode and stays inline.
                if (gpufr && gpufr->data[0] && gpufr->hw_frames_ctx &&
                    (view.d3dReady() || view.d3d12Ready() || view.vulkanReady() ||
                     view.glReady())) {
                    view.submitGpuFrame(gpufr);
                } else if (view.vulkanReady() || view.glReady() ||
                           view.d3dReady() || view.d3d12Ready()) {
                    view.submitCpuFrame(fr);
                } else {
                    view.present(fr);
                }
            }
            const AVFrame* camera_src =
                (gpufr && gpufr->data[0] && gpufr->hw_frames_ctx)
                    ? gpufr
                    : (fr->data[0] ? fr : nullptr);
            if (camera.valid() && camera_src) {
                // Hand a reference to the camera thread: it converts
                // and copies into the bus there (honouring linesize,
                // which swscale pads to its own alignment). The
                // advertised rate tracks the measured stream rate
                // (updated once a second below): a 120 Hz phone must be
                // able to offer 120 to OBS, not a hardcoded 60. The bus
                // only renegotiates its media type when the number
                // actually changes, so feeding it a live estimate is
                // safe.
                AVFrame* hand = av_frame_alloc();
                if (hand && av_frame_ref(hand, camera_src) == 0) {
                    std::lock_guard<std::mutex> lock(camera_mutex);
                    if (camera_pending) {
                        av_frame_free(&camera_pending);
                        ++cam_replaced;
                    }
                    camera_pending = hand;
                    camera_pending_fps = camera_fps_num;
                    camera_cv.notify_one();
                } else if (hand) {
                    av_frame_free(&hand);
                }
            }
            if (camera.valid()) {
                ++camera_window_frames;
                const uint64_t now_ms = SDL_GetTicks64();
                if (now_ms - camera_window_start >= 1000) {
                    const double secs = (now_ms - camera_window_start) / 1000.0;
                    camera_fps_num = std::max(1, int(camera_window_frames / secs + 0.5));
                    camera_window_frames = 0;
                    camera_window_start = now_ms;
                }
            }
            if (opt.frames_limit > 0 && view.presented() >= uint64_t(opt.frames_limit)) {
                running = false;
            }
            break;
        }

        case wp::kAudioInit: {
            const uint8_t* p = f.payload;
            size_t rem = f.size;
            uint32_t rate = 0;
            if (!wp::readU32BE(p, rem, rate) || rem < 3) break;
            const uint8_t channels = *p++;
            rem--;
            const uint32_t asc_len = (uint32_t(p[0]) << 8) | uint32_t(p[1]);
            p += 2;
            rem -= 2;
            if (rem < asc_len) break;
            if (audio.configure(p, asc_len, int(rate))) {
                std::fprintf(stderr, "[wireplay] audio: %u Hz %u ch, ASC=%s\n", rate,
                             uint32_t(channels), hex(p, asc_len).c_str());
            } else {
                std::fprintf(stderr, "[wireplay] AAC decoder unavailable; video only\n");
            }
            break;
        }

        case wp::kAudioPcmInit: {
            const uint8_t* p = f.payload;
            size_t rem = f.size;
            uint32_t rate = 0;
            if (!wp::readU32BE(p, rem, rate) || rem < 1) break;
            const uint8_t channels = *p;
            if (audio.configurePcm(int(rate), int(channels))) {
                std::fprintf(stderr, "[wireplay] audio: %u Hz %u ch, raw PCM\n", rate,
                             uint32_t(channels));
            } else {
                std::fprintf(stderr, "[wireplay] PCM audio unavailable; video only\n");
            }
            break;
        }

        case wp::kAudioAU: {
            if (f.size < 5) break;
            const uint32_t ts = (uint32_t(f.payload[0]) << 24) | (uint32_t(f.payload[1]) << 16) |
                                (uint32_t(f.payload[2]) << 8) | uint32_t(f.payload[3]);
            if (ts == last_audio_ts) break;  // duplicate
            last_audio_ts = ts;
            {
                const auto audio_t0 = stage_now();
                audio.pushAU(f.payload + 4, f.size - 4);
                const double aud_ms = stage_elapsed(audio_t0);
                stage_audio_ms += aud_ms;
                if (aud_ms > stage_audio_max_ms) stage_audio_max_ms = aud_ms;
                ++stage_audio_n;
            }
            break;
        }

        case wp::kStatus: {
            std::fprintf(stderr, "[engine] %.*s\n", int(f.size), f.payload);
            const std::string s = f.asString();
            const std::string state = jsonString(s, "state");
            const std::string detail = jsonString(s, "detail");
            view.message(state + (detail.empty() ? std::string() : ": " + detail));
            break;
        }

        case wp::kStats:
            if (opt.verbose) std::fprintf(stderr, "[stats] %.*s\n", int(f.size), f.payload);
            break;

        case wp::kDevice:
            std::fprintf(stderr, "[device] %.*s\n", int(f.size), f.payload);
            break;

        case wp::kFatal:
            std::fprintf(stderr, "[engine] FATAL %.*s\n", int(f.size), f.payload);
            rc = 2;
            running = false;
            break;

        case wp::kPing:
            if (engine_stdin) wp::writeFrame(engine_stdin, wp::kPong, f.payload, f.size);
            break;

        case wp::kEnd:
            running = false;
            break;

        default:
            break;
        }

        if (view.consumeRestoreRequest()) view.refreshGeometry();
        if (view.consumeKeyRequest() && engine_stdin) {
            std::fprintf(stderr, "[wireplay] requesting keyframe\n");
            wp::writeFrame(engine_stdin, wp::kRequestKey);
        }
        if (view.consumeRestartRequest() && engine_stdin) {
            std::fprintf(stderr, "[wireplay] requesting stream restart\n");
            wp::writeFrame(engine_stdin, wp::kRestartStream);
        }

        {
            const double busy_ms = std::chrono::duration<double, std::milli>(
                                       std::chrono::steady_clock::now() - loop_t_read)
                                       .count();
            if (busy_ms > 25.0) ++loop_busy_gt25;
            if (busy_ms > loop_busy_max_ms) loop_busy_max_ms = busy_ms;
        }

        const uint64_t now = SDL_GetTicks64();
        if (now - last_log > 5000) {
            std::fprintf(stderr,
                         "[wireplay] loop: rx_gap>25ms=%llu (max %.0f ms)  busy>25ms=%llu "
                         "(max %.0f ms)  decode max=%.1f ms  audio-push max=%.1f ms  "
                         "audio_cleared=%llu  audio_q=%d ms (started=%d prime=%u delay=%d)\n",
                         (unsigned long long)loop_rx_gt25, loop_rx_max_ms,
                         (unsigned long long)loop_busy_gt25, loop_busy_max_ms,
                         stage_decode_max_ms, stage_audio_max_ms,
                         (unsigned long long)audio.dropped(),
                         audio.queuedMs(), audio.started() ? 1 : 0,
                         audio.prime(), audio.delayMs());
            loop_rx_gt25 = loop_busy_gt25 = 0;
            loop_rx_max_ms = loop_busy_max_ms = 0.0;
            stage_decode_max_ms = stage_audio_max_ms = 0.0;
            if (camera.valid()) {
                std::fprintf(stderr,
                             "[wireplay] camera thread: published=%llu replaced=%llu  "
                             "readback max=%.1f ms (device-lock wait max=%.1f ms)  "
                             "frame total max=%.1f ms\n",
                             (unsigned long long)camera.published(),
                             (unsigned long long)cam_replaced.exchange(0),
                             double(cam_readback_max_us.exchange(0)) / 1000.0,
                             double(cam_lock_wait_max_us.exchange(0)) / 1000.0,
                             double(cam_total_max_us.exchange(0)) / 1000.0);
            }
            if (stage_video_n || stage_audio_n) {
                std::fprintf(stderr,
                             "[wireplay] stage ms/frame: decode=%.2f (n=%llu) "
                             "audio-push=%.2f (n=%llu)\n",
                             stage_video_n ? stage_decode_ms / stage_video_n : 0.0,
                             (unsigned long long)stage_video_n,
                             stage_audio_n ? stage_audio_ms / stage_audio_n : 0.0,
                             (unsigned long long)stage_audio_n);
                stage_decode_ms = stage_audio_ms = 0.0;
                stage_video_n = stage_audio_n = 0;
            }
            const wp::DecodeStats& st = decoder.stats();
            std::fprintf(stderr,
                         "[wireplay] %" PRIu64 " decoded, %" PRIu64 " errors, %" PRIu64
                         " corrupt, %" PRIu64
                         " presented, %dx%d, %s, audio %s, camera %" PRIu64
                         ", stash_drops %" PRIu64 ", %s\n",
                         st.frames_decoded, st.decode_errors, st.corrupt_frames,
                         view.presented(), st.width,
                         st.height, st.hwaccel.c_str(), audio.active() ? "on" : "off",
                         camera.published(), view.stashDrops(),
                         view.takePresStats().c_str());
            last_log = now;
        }
    }

    if (engine_stdin) wp::writeFrame(engine_stdin, wp::kAppEnd);

    // Captured before the teardown. view.close() calls SDL_Quit(), after which
    // SDL_GetTicks64() reads 0 and the elapsed-time subtraction below underflows
    // to roughly 1.8e19 seconds. That garbage was landing in the summary line on
    // exactly the runs where the summary was the only diagnostic available.
    const uint64_t t_end = SDL_GetTicks64();
    audio.close();
    view.close();
    engine.stop();

    // Stop the camera thread before reading its tally: close() unmaps
    // the header, so published() would report 0 and the summary would
    // always claim that nothing reached the virtual camera.
    {
        std::lock_guard<std::mutex> lock(camera_mutex);
        camera_stop = true;
    }
    camera_cv.notify_one();
    if (camera_thread.joinable()) camera_thread.join();
    if (camera_pending) av_frame_free(&camera_pending);
    const uint64_t camera_published = camera.published();
    camera.close();

    const wp::DecodeStats& st = decoder.stats();
    const uint64_t elapsed = t_end > t0 ? t_end - t0 : 0;
    std::fprintf(stderr,
                 "[wireplay] done: %" PRIu64 " decoded, %" PRIu64 " decode errors, %" PRIu64
                 " corrupt, %" PRIu64
                 " presented in %.1fs (%.1f fps avg), %s, %zu frames read, %" PRIu64
                 " frames published to the camera\n",
                 st.frames_decoded, st.decode_errors, st.corrupt_frames,
                 view.presented(), elapsed / 1000.0,
                 elapsed > 0 ? view.presented() * 1000.0 / double(elapsed) : 0.0,
                 st.hwaccel.c_str(), reader.frames_read, camera_published);
    if (offline_file) std::fclose(offline_file);
    return rc;
}

}  // namespace

#ifdef _WIN32
// The executable is built with WIN32_EXECUTABLE, so it has no console of its own
// and every diagnostic printed to stderr is discarded -- including the ones that
// explain why the mirror is black. When launched from a terminal, adopt that
// terminal's console so -v actually reaches the user. When double-clicked there
// is no parent console and this fails harmlessly, which preserves the reason the
// subsystem was set: no console window sitting next to the mirror.
void attachToParentConsole() {
    if (GetStdHandle(STD_ERROR_HANDLE) != nullptr) return;  // already has one
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
    // stderr only. stdout is a data channel in --stdin mode, where the engine is
    // piped in and commands travel back out through it, so redirecting stdout to
    // the console would break that workflow.
    FILE* unused = nullptr;
    freopen_s(&unused, "CONOUT$", "w", stderr);
    // freopen_s updates the CRT stream, but the engine is spawned with
    // GetStdHandle(STD_ERROR_HANDLE) as its stderr, and it is not documented
    // that the CRT pushes the new handle down to the OS standard-handle table.
    // Do it explicitly so the engine's own log reaches the same console instead
    // of vanishing the way ours did.
    SetStdHandle(STD_ERROR_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stderr))));
}
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    attachToParentConsole();
#endif
    Options opt;
    bool explicit_stdin = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        // Long options take one dash or two (-msaa == --msaa); short
        // flags (-v, -h) are untouched.
        if (a.size() > 2 && a[0] == '-' && a[1] != '-') a = "-" + a;
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") {
            usage(argv[0]);
            return 0;
        } else if (a == "--engine") {
            opt.engine_cmd = next("--engine");
        } else if (a == "--udid") {
            opt.udid = next("--udid");
        } else if (a == "--offline") {
            opt.offline = next("--offline");
        } else if (a == "--stdin") {
            explicit_stdin = true;
            opt.use_stdin = true;
        } else if (a == "--dump-video") {
            opt.dump_video = next("--dump-video");
        } else if (a == "--frames") {
            opt.frames_limit = std::atoi(next("--frames"));
        } else if (a == "--threads") {
            opt.threads = std::atoi(next("--threads"));
        } else if (a == "--max-queue-ms") {
            opt.max_queue_ms = std::atoi(next("--max-queue-ms"));
        } else if (a == "--no-audio") {
            opt.no_audio = true;
        } else if (a == "--fullscreen") {
            opt.fullscreen = true;
        } else if (a == "--always-on-top") {
            opt.always_on_top = true;
        } else if (a == "--headless") {
            opt.headless = true;
        } else if (a == "--no-camera") {
            opt.no_camera = true;
        } else if (a == "--msaa") {
            opt.msaa = std::atoi(next("--msaa"));
            if (opt.msaa != 0 && opt.msaa != 2 && opt.msaa != 4 && opt.msaa != 8 &&
                opt.msaa != 16) {
                std::fprintf(stderr, "--msaa: expected 0, 2, 4, 8 or 16, got %d\n",
                             opt.msaa);
                return 2;
            }
        } else if (a == "--aniso") {
            opt.aniso = std::atoi(next("--aniso"));
            if (opt.aniso < 0) opt.aniso = 0;
            if (opt.aniso > 16) opt.aniso = 16;
        } else if (a == "--no-camera-effects") {
            opt.no_camera_effects = true;
        } else if (a == "--scaling") {
            const std::string v = next("--scaling");
            if (v == "lanczos") {
                opt.scaling = Options::Scaling::Lanczos;
            } else if (v == "aniso") {
                opt.scaling = Options::Scaling::Aniso;
            } else if (v == "linear") {
                opt.scaling = Options::Scaling::Linear;
            } else {
                std::fprintf(stderr, "--scaling: expected lanczos, aniso or linear, got %s\n",
                             v.c_str());
                return 2;
            }
        } else if (a == "--decoder") {
            const std::string v = next("--decoder");
            if (v == "auto") {
                opt.decoder_pref = 0;
            } else if (v == "d3d11va") {
                opt.decoder_pref = 1;
            } else if (v == "cuda" || v == "nvdec") {
                opt.decoder_pref = 2;
            } else {
                std::fprintf(stderr, "--decoder: expected auto, d3d11va or cuda, got %s\n",
                             v.c_str());
                return 2;
            }
        } else if (a == "--present") {
            const std::string v = next("--present");
            if (v == "pts") {
                opt.present_pts = true;
            } else if (v == "arrival") {
                opt.present_pts = false;
            } else {
                std::fprintf(stderr, "--present: expected pts or arrival, got %s\n", v.c_str());
                return 2;
            }
        } else if (a == "--cushion") {
            const std::string v = next("--cushion");
            opt.cushion_ms = std::strtod(v.c_str(), nullptr);
            if (opt.cushion_ms < 0.0) opt.cushion_ms = 0.0;
            if (opt.cushion_ms > 250.0) opt.cushion_ms = 250.0;
        } else if (a == "--audio-delay") {
            const std::string v = next("--audio-delay");
            opt.audio_delay_ms = std::strtod(v.c_str(), nullptr);
            if (opt.audio_delay_ms < 0.0) opt.audio_delay_ms = 0.0;
            if (opt.audio_delay_ms > 500.0) opt.audio_delay_ms = 500.0;
        } else if (a == "--pipe-wait") {
            const std::string v = next("--pipe-wait");
            if (v == "event") {
                opt.pipe_wait_event = true;
            } else if (v == "poll") {
                opt.pipe_wait_event = false;
            } else {
                std::fprintf(stderr, "--pipe-wait: expected event or poll, got %s\n", v.c_str());
                return 2;
            }
        } else if (a == "--renderer") {
            const std::string v = next("--renderer");
            if (v == "vulkan") {
                opt.Vulkan = true;
            } else if (v == "opengl") {
                opt.opengl = true;
            } else if (v == "d3d11") {
                opt.force_d3d11 = true;
            } else if (v != "d3d12") {
                std::fprintf(stderr, "--renderer: expected d3d12, d3d11, vulkan or opengl, got %s\n",
                             v.c_str());
                return 2;
            }
        } else if (a == "--backend") {
            const std::string v = next("--backend");
            if (!wp::parseBackendName(v, &opt.backend)) {
                std::fprintf(stderr, "--backend: expected auto, coredevice or quicktime, got %s\n",
                             v.c_str());
                return 2;
            }
        } else if (a == "-v" || a == "--verbose") {
            opt.verbose = true;
        } else {
            std::fprintf(stderr, "unknown option: %s\n", a.c_str());
            usage(argv[0]);
            return 2;
        }
    }

    // Default to launching the engine next to us; --engine overrides the
    // command and --stdin/--offline opt out of launching entirely.
    if (!explicit_stdin && opt.offline.empty() && opt.engine_cmd.empty()) {
        opt.engine_cmd = "wireplay-engine";
    }
    return run(opt);
}
