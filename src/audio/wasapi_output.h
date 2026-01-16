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

// Summary: Render callback invoked on the real-time audio thread.
// Preconditions: must be fast, allocation-free, and non-blocking; out is non-null.
// Postconditions: writes up to frames * channels samples into out.
// Errors: return false to request silence.
using RenderCallback = bool (*)(float* out, uint32_t frames, uint32_t channels, void* user);

// Summary: Supported mix formats for the device render buffer.
// Preconditions: none.
// Postconditions: none.
// Errors: Unsupported means the render path will output silence.
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

// Summary: WASAPI shared-mode output wrapper with event-driven render thread.
// Preconditions: COM initialized on calling thread before init_default_device.
// Postconditions: start/stop control render thread lifecycle deterministically.
// Errors: methods return false on initialization or start failures.
class WasapiOutput {
public:
  // Summary: Render callback type used by this output.
  // Preconditions: same as RenderCallback.
  // Postconditions: none.
  // Errors: return false to request silence.
  using RenderCallback = ::tomplayer::wasapi::RenderCallback;

  // Summary: Construct an uninitialized output object.
  // Preconditions: none.
  // Postconditions: must call init_default_device before start.
  // Errors: none.
  WasapiOutput();

  // Summary: Shutdown and release resources.
  // Preconditions: none.
  // Postconditions: stop() has been called and resources released.
  // Errors: none.
  ~WasapiOutput();

  WasapiOutput(const WasapiOutput&) = delete;
  WasapiOutput& operator=(const WasapiOutput&) = delete;

  // Summary: Initialize using the default render device in shared mode.
  // Preconditions: COM initialized and remains active for the caller thread.
  // Postconditions: mix format cached and render thread can be started.
  // Errors: returns false on WASAPI or COM failures; object is reset.
  bool init_default_device(RenderCallback callback, void* user);

  // Summary: Start event-driven rendering on a dedicated thread.
  // Preconditions: init_default_device has succeeded and start() not already running.
  // Postconditions: render thread active and audio client started.
  // Errors: returns false on start failure without leaving the thread running.
  bool start();

  // Summary: Stop rendering and join the render thread.
  // Preconditions: none (safe if not running).
  // Postconditions: no render callbacks execute after return.
  // Errors: none.
  void stop();

  // Summary: Stop and release all COM resources and OS handles.
  // Preconditions: none.
  // Postconditions: object returns to uninitialized state.
  // Errors: none.
  void shutdown();

  // Summary: Device mix sample rate in Hz.
  // Preconditions: init_default_device succeeded.
  // Postconditions: none.
  // Errors: returns 0 if uninitialized.
  uint32_t sample_rate() const { return sample_rate_; }

  // Summary: Device channel count.
  // Preconditions: init_default_device succeeded.
  // Postconditions: none.
  // Errors: returns 0 if uninitialized.
  uint16_t channels() const { return channels_; }

  // Summary: Device sample format.
  // Preconditions: init_default_device succeeded.
  // Postconditions: none.
  // Errors: Unsupported if format is not handled.
  SampleFormat sample_format() const { return sample_format_; }

  // Summary: Bits per sample of the mix format.
  // Preconditions: init_default_device succeeded.
  // Postconditions: none.
  // Errors: returns 0 if uninitialized.
  uint16_t bits_per_sample() const { return bits_per_sample_; }

  // Summary: Size of the WASAPI buffer in frames.
  // Preconditions: init_default_device succeeded.
  // Postconditions: none.
  // Errors: returns 0 if uninitialized.
  uint32_t buffer_frames() const { return buffer_frames_; }

#if defined(TOMPLAYER_TESTING)
  void set_start_stop_api_for_test(const detail::StartStopApi& api,
                                   HANDLE audio_event,
                                   HANDLE stop_event);
  bool is_running_for_test() const { return running_.load(std::memory_order_relaxed); }
#endif

private:
  // Summary: Render thread body; waits on the WASAPI event and renders audio.
  // Preconditions: start() has initialized the event handles.
  // Postconditions: exits when stop() signals.
  // Errors: none.
  void RenderLoop();

  // Summary: Single render cycle (padding -> get buffer -> fill -> release).
  // Preconditions: render thread only; render_api_ is valid.
  // Postconditions: buffer released or method returns early.
  // Errors: on failure, returns without rendering (silence handled by caller).
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
