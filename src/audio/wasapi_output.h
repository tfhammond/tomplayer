#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <windows.h>
#include <wrl/client.h>

namespace tomplayer {
namespace wasapi {

using RenderCallback = bool (*)(float* out, uint32_t frames, uint32_t channels, void* user);

enum class SampleFormat {
  Float32,
  Pcm16,
  Unsupported
};

namespace detail {
// Test seam for render-path unit tests; production wires COM calls into these slots.
struct RenderApi {
  void* context{nullptr};
  HRESULT (*GetCurrentPadding)(void* context, UINT32* padding) = nullptr;
  HRESULT (*GetBuffer)(void* context, UINT32 frames, BYTE** data) = nullptr;
  HRESULT (*ReleaseBuffer)(void* context, UINT32 frames, DWORD flags) = nullptr;
};

// Test seam for start/stop without creating real COM interfaces.
struct StartStopApi {
  void* context{nullptr};
  HRESULT (*Start)(void* context) = nullptr;
  HRESULT (*Stop)(void* context) = nullptr;
  HRESULT (*Reset)(void* context) = nullptr;
};

SampleFormat DetectSampleFormat(const WAVEFORMATEX* format);
void ConvertFloatToPcm16(const float* in, int16_t* out, std::size_t samples);
void RenderAudioCore(const RenderApi& api,
                     RenderCallback callback,
                     void* callback_user,
                     uint32_t buffer_frames,
                     uint32_t channels,
                     SampleFormat format,
                     float* float_scratch);
}  // namespace detail

class WasapiOutput {
public:
  // Runs on the real-time render thread; keep it allocation-free and non-blocking.
  using RenderCallback = ::tomplayer::wasapi::RenderCallback;

  WasapiOutput();
  ~WasapiOutput();

  WasapiOutput(const WasapiOutput&) = delete;
  WasapiOutput& operator=(const WasapiOutput&) = delete;

  // COM must stay initialized on the caller thread while COM interfaces are in use.
  bool init_default_device(RenderCallback callback, void* user);
  bool start();
  void stop();
  void shutdown();

  uint32_t sample_rate() const { return sample_rate_; }
  uint16_t channels() const { return channels_; }
  SampleFormat sample_format() const { return sample_format_; }
  uint16_t bits_per_sample() const { return bits_per_sample_; }
  uint32_t buffer_frames() const { return buffer_frames_; }

#if defined(TOMPLAYER_TESTING)
  void set_start_stop_api_for_test(const detail::StartStopApi& api,
                                   HANDLE audio_event,
                                   HANDLE stop_event);
  bool is_running_for_test() const { return running_.load(std::memory_order_relaxed); }
#endif

private:
  // Only the render thread may touch the render client.
  void RenderLoop();
  void RenderAudio();

  RenderCallback callback_{nullptr};
  void* callback_user_{nullptr};

  Microsoft::WRL::ComPtr<IMMDevice> device_;
  Microsoft::WRL::ComPtr<IAudioClient> audio_client_;
  Microsoft::WRL::ComPtr<IAudioRenderClient> render_client_;

  struct RenderApiContext {
    IAudioClient* audio_client{nullptr};
    IAudioRenderClient* render_client{nullptr};
  };

  WAVEFORMATEX* mix_format_{nullptr};

  HANDLE audio_event_{nullptr};
  HANDLE stop_event_{nullptr};

  std::thread render_thread_;
  std::atomic<bool> running_{false};

  uint32_t buffer_frames_{0};
  uint32_t sample_rate_{0};
  uint16_t channels_{0};
  uint16_t bits_per_sample_{0};
  uint16_t block_align_{0};
  SampleFormat sample_format_{SampleFormat::Unsupported};

  detail::RenderApi render_api_{};
  detail::StartStopApi start_stop_api_{};
  RenderApiContext render_api_context_{};

  // Scratch space keeps PCM16 conversion off the heap during render.
  std::unique_ptr<float[]> float_scratch_;
};

}  // namespace wasapi
}  // namespace tomplayer
