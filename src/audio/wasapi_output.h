#pragma once

#include <atomic>
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

namespace audio {

enum class SampleFormat {
  Float32,
  Pcm16,
  Unsupported
};

class WasapiOutput {
public:
  // Invoked on the real-time render thread; must not allocate, block, or throw.
  using RenderCallback = bool (*)(float* out, uint32_t frames, uint32_t channels, void* user);

  WasapiOutput();
  ~WasapiOutput();

  WasapiOutput(const WasapiOutput&) = delete;
  WasapiOutput& operator=(const WasapiOutput&) = delete;

  // Call CoInitializeEx(COINIT_MULTITHREADED) before init_default_device and keep COM initialized.
  bool init_default_device(RenderCallback callback, void* user);
  bool start();
  void stop();
  void shutdown();

  uint32_t sample_rate() const { return sample_rate_; }
  uint16_t channels() const { return channels_; }
  SampleFormat sample_format() const { return sample_format_; }
  uint16_t bits_per_sample() const { return bits_per_sample_; }
  uint32_t buffer_frames() const { return buffer_frames_; }

private:
  void RenderLoop();
  void RenderAudio();

  RenderCallback callback_{nullptr};
  void* callback_user_{nullptr};

  Microsoft::WRL::ComPtr<IMMDevice> device_;
  Microsoft::WRL::ComPtr<IAudioClient> audio_client_;
  Microsoft::WRL::ComPtr<IAudioRenderClient> render_client_;

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

  std::unique_ptr<float[]> float_scratch_;
};

}  // namespace audio
