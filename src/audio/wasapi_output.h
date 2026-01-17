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

class AudioRingBuffer;

namespace tomplayer {
namespace wasapi {

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
// Read frames into dst and zero-fill any underrun tail; updates counters if provided.
uint32_t ConsumeRingBufferFloat(AudioRingBuffer* ring_buffer,
                                float* dst_interleaved,
                                uint32_t frames_requested,
                                uint32_t channels,
                                std::atomic<uint64_t>* underrun_wakes,
                                std::atomic<uint64_t>* underrun_frames);
}  // namespace detail

// Summary: WASAPI shared-mode output wrapper with event-driven render thread.
// Preconditions: COM initialized on calling thread before init_default_device.
// Postconditions: start/stop control render thread lifecycle deterministically.
// Errors: methods return false on initialization or start failures.
class WasapiOutput {
public:
  WasapiOutput();

  // Summary: Shutdown and release resources.
  // Preconditions: none.
  // Postconditions: stop() has been called and resources released.
  // Errors: none.
  ~WasapiOutput();

  WasapiOutput(const WasapiOutput&) = delete;
  WasapiOutput& operator=(const WasapiOutput&) = delete;

  // COM must stay initialized on the caller thread while COM interfaces are in use.
  bool init_default_device();

  // Set the ring buffer used by the render thread.
  // Preconditions: must be called before start(); buffer outlives stop()/shutdown().
  void set_ring_buffer(AudioRingBuffer* ring_buffer);

  // Start requires init_default_device, a non-null ring buffer, and matching channels.
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
  // Summary: Number of render wakes that saw a short read.
  // Preconditions: none.
  // Postconditions: does not modify state.
  // Errors: none.
  uint64_t underrun_wake_count() const { return underrun_wake_count_.load(std::memory_order_relaxed); }

  // Summary: Number of frames zero-filled due to underrun.
  // Preconditions: none.
  // Postconditions: does not modify state.
  // Errors: none.
  uint64_t underrun_frame_count() const { return underrun_frame_count_.load(std::memory_order_relaxed); }

#if defined(TOMPLAYER_TESTING)
  void set_start_stop_api_for_test(const detail::StartStopApi& api,
                                   HANDLE audio_event,
                                   HANDLE stop_event);
  void set_channels_for_test(uint16_t channels);
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

  AudioRingBuffer* ring_buffer_{nullptr};
  std::atomic<uint64_t> underrun_wake_count_{0};
  std::atomic<uint64_t> underrun_frame_count_{0};

  // Scratch space keeps PCM16 conversion off the heap during render.
  std::unique_ptr<float[]> float_scratch_;
};

}  // namespace wasapi
}  // namespace tomplayer
