#include "audio/wasapi_output.h"

#include "buffer/audio_ring_buffer.h"

#include <avrt.h>
#include <ksmedia.h>

#include <cassert>
#include <cstring>

namespace tomplayer {
namespace wasapi {
namespace detail {
SampleFormat DetectSampleFormat(const WAVEFORMATEX* format) {
  if (!format) {
    return SampleFormat::Unsupported;
  }

  if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && format->wBitsPerSample == 32) {
    return SampleFormat::Float32;
  }

  if (format->wFormatTag == WAVE_FORMAT_PCM && format->wBitsPerSample == 16) {
    return SampleFormat::Pcm16;
  }

  if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
    const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
    if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) &&
        format->wBitsPerSample == 32) {
      return SampleFormat::Float32;
    }
    if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) &&
        format->wBitsPerSample == 16) {
      return SampleFormat::Pcm16;
    }
  }

  return SampleFormat::Unsupported;
}

uint32_t ConsumeRingBufferFloat(AudioRingBuffer* ring_buffer,
                                float* dst_interleaved,
                                uint32_t frames_requested,
                                uint32_t channels,
                                std::atomic<uint64_t>* underrun_wakes,
                                std::atomic<uint64_t>* underrun_frames) {
  if (frames_requested == 0 || channels == 0) {
    return 0;
  }
  assert(dst_interleaved != nullptr);

  uint32_t frames_read = 0;
  if (ring_buffer) {
    frames_read = ring_buffer->read_frames(dst_interleaved, frames_requested);
  }

  if (frames_read < frames_requested) {
    const size_t sample_offset =
        static_cast<size_t>(frames_read) * channels;
    const size_t samples_to_zero =
        static_cast<size_t>(frames_requested - frames_read) * channels;
    std::memset(dst_interleaved + sample_offset, 0, samples_to_zero * sizeof(float));

    if (underrun_wakes) {
      underrun_wakes->fetch_add(1, std::memory_order_relaxed);
    }
    if (underrun_frames) {
      underrun_frames->fetch_add(frames_requested - frames_read,
                                 std::memory_order_relaxed);
    }
  }

  return frames_read;
}

bool SelectFloat32MixFormat(const FormatSupportApi& api,
                            const WAVEFORMATEX* device_mix_format,
                            WAVEFORMATEXTENSIBLE* float32_format) {
  if (!api.IsFormatSupported || !device_mix_format || !float32_format) {
    return false;
  }
  if (device_mix_format->nSamplesPerSec == 0 || device_mix_format->nChannels == 0) {
    return false;
  }

  WAVEFORMATEXTENSIBLE requested{};
  requested.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  requested.Format.nChannels = device_mix_format->nChannels;
  requested.Format.nSamplesPerSec = device_mix_format->nSamplesPerSec;
  requested.Format.wBitsPerSample = 32;
  requested.Format.nBlockAlign =
      static_cast<WORD>(requested.Format.nChannels * sizeof(float));
  requested.Format.nAvgBytesPerSec =
      requested.Format.nSamplesPerSec * requested.Format.nBlockAlign;
  requested.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
  requested.Samples.wValidBitsPerSample = 32;
  requested.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

  if (device_mix_format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
    const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(device_mix_format);
    requested.dwChannelMask = ext->dwChannelMask;
  } else {
    requested.dwChannelMask = 0;
  }

  WAVEFORMATEX* closest = nullptr;
  const HRESULT hr = api.IsFormatSupported(api.context,
                                           AUDCLNT_SHAREMODE_SHARED,
                                           &requested.Format,
                                           &closest);
  if (closest) {
    CoTaskMemFree(closest);
  }
  if (hr != S_OK) {
    return false;
  }

  *float32_format = requested;
  return true;
}

}  // namespace detail

WasapiOutput::WasapiOutput() = default;

WasapiOutput::~WasapiOutput() {
  shutdown();
}


// NOTE: ring_buffer_ is a non-owning pointer and is intentionally *not* synchronized.
// Contract: set_ring_buffer() must be called exactly once before start(), and never
// while the render thread is running. The AudioRingBuffer must outlive WasapiOutput.
// This is safe because std::thread start publishes prior writes; violating this
// contract would introduce a data race in Release builds.
void WasapiOutput::set_ring_buffer(AudioRingBuffer* ring_buffer) {
  assert(!running_.load(std::memory_order_relaxed));
  ring_buffer_ = ring_buffer;
}

bool WasapiOutput::init_default_device() {
  // Do setup here so the render path stays allocation-free and deterministic.
  if (audio_client_) {
    return false; 
  }

  Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(enumerator.GetAddressOf()));
  if (FAILED(hr)) {
    shutdown();
    return false;
  }

  hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
  if (FAILED(hr)) {
    shutdown();
    return false;
  }

  hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                         reinterpret_cast<void**>(audio_client_.GetAddressOf()));
  if (FAILED(hr)) {
    shutdown();
    return false;
  }

  // Shared mode dictates the device mix format; conversions must honor it.
  hr = audio_client_->GetMixFormat(&mix_format_);
  if (FAILED(hr)) {
    shutdown();
    return false;
  }

  format_support_api_.context = audio_client_.Get();
  format_support_api_.IsFormatSupported =
      [](void* context,
         AUDCLNT_SHAREMODE share_mode,
         const WAVEFORMATEX* format,
         WAVEFORMATEX** closest) -> HRESULT {
        return static_cast<IAudioClient*>(context)->IsFormatSupported(share_mode,
                                                                      format,
                                                                      closest);
      };

  WAVEFORMATEXTENSIBLE float32_format{};
  if (!detail::SelectFloat32MixFormat(format_support_api_, mix_format_, &float32_format)) {
    shutdown();
    return false;
  }
  sample_rate_ = float32_format.Format.nSamplesPerSec;
  channels_ = float32_format.Format.nChannels;
  bits_per_sample_ = float32_format.Format.wBitsPerSample;
  block_align_ = float32_format.Format.nBlockAlign;
  sample_format_ = SampleFormat::Float32;

  hr = audio_client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                 0, 0, &float32_format.Format, nullptr);
  if (FAILED(hr)) {
    shutdown();
    return false;
  }

  hr = audio_client_->GetBufferSize(&buffer_frames_);
  if (FAILED(hr)) {
    shutdown();
    return false;
  }

  hr = audio_client_->GetService(__uuidof(IAudioRenderClient),
                                 reinterpret_cast<void**>(render_client_.GetAddressOf()));
  if (FAILED(hr)) {
    shutdown();
    return false;
  }

  audio_event_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
  stop_event_ = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  if (!audio_event_ || !stop_event_) {
    shutdown();
    return false;
  }

  hr = audio_client_->SetEventHandle(audio_event_);
  if (FAILED(hr)) {
    shutdown();
    return false;
  }

  render_api_context_.audio_client = audio_client_.Get();
  render_api_context_.render_client = render_client_.Get();
  render_api_.context = &render_api_context_;
  render_api_.GetCurrentPadding = [](void* context, UINT32* padding) -> HRESULT {
    auto* ctx = static_cast<RenderApiContext*>(context);
    return ctx->audio_client->GetCurrentPadding(padding);
  };
  render_api_.GetBuffer = [](void* context, UINT32 frames, BYTE** data) -> HRESULT {
    auto* ctx = static_cast<RenderApiContext*>(context);
    return ctx->render_client->GetBuffer(frames, data);
  };
  render_api_.ReleaseBuffer = [](void* context, UINT32 frames, DWORD flags) -> HRESULT {
    auto* ctx = static_cast<RenderApiContext*>(context);
    return ctx->render_client->ReleaseBuffer(frames, flags);
  };

  start_stop_api_.context = audio_client_.Get();
  start_stop_api_.Start = [](void* context) -> HRESULT {
    return static_cast<IAudioClient*>(context)->Start();
  };
  start_stop_api_.Stop = [](void* context) -> HRESULT {
    return static_cast<IAudioClient*>(context)->Stop();
  };
  start_stop_api_.Reset = [](void* context) -> HRESULT {
    return static_cast<IAudioClient*>(context)->Reset();
  };

  return true;
}

bool WasapiOutput::start() {
  // Render thread performs GetCurrentPadding/GetBuffer/ReleaseBuffer; Start/Stop/Reset are invoked on the caller thread.
  if (!start_stop_api_.Start || !audio_event_ || !stop_event_) {
    return false;
  }
  assert(ring_buffer_ != nullptr);
  if (!ring_buffer_) {
    return false;
  }
  assert(ring_buffer_->channels() == channels_);
  if (ring_buffer_->channels() != channels_) {
    return false;
  }

  if (running_.exchange(true)) {
    return false;
  }

  ResetEvent(stop_event_);
  render_thread_ = std::thread(&WasapiOutput::RenderLoop, this);

  const HRESULT hr = start_stop_api_.Start(start_stop_api_.context);
  if (FAILED(hr)) {
    running_ = false;
    SetEvent(stop_event_);
    if (render_thread_.joinable()) {
      render_thread_.join();
    }
    return false;
  }

  return true;
}

void WasapiOutput::stop() {
  // Quiesce the render thread before stopping the audio client.
  if (!running_.exchange(false)) {
    return;
  }

  SetEvent(stop_event_);

  if (render_thread_.joinable()) {
    render_thread_.join();
  }

  if (start_stop_api_.Stop && start_stop_api_.Reset) {
    start_stop_api_.Stop(start_stop_api_.context);
    start_stop_api_.Reset(start_stop_api_.context);
  }
}

void WasapiOutput::shutdown() {
  // Centralized cleanup lets init failures unwind safely.
  stop();

  if (mix_format_) {
    CoTaskMemFree(mix_format_);
    mix_format_ = nullptr;
  }

  if (audio_event_) {
    CloseHandle(audio_event_);
    audio_event_ = nullptr;
  }

  if (stop_event_) {
    CloseHandle(stop_event_);
    stop_event_ = nullptr;
  }

  render_client_.Reset();
  audio_client_.Reset();
  device_.Reset();

  render_api_ = {};
  start_stop_api_ = {};
  format_support_api_ = {};
  render_api_context_ = {};

  buffer_frames_ = 0;
  sample_rate_ = 0;
  channels_ = 0;
  bits_per_sample_ = 0;
  block_align_ = 0;
  sample_format_ = SampleFormat::Unsupported;
}

void WasapiOutput::RenderLoop() {
  // Event-driven wait avoids busy spinning and keeps RT behavior predictable.
  const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  // RPC_E_CHANGED_MODE means COM is already initialized with a different model.
  // We uninitialize only when CoInitializeEx succeeds
  // S_OK/S_FALSE both require CoUninitialize to balance CoInitializeEx.
  const bool com_should_uninit = SUCCEEDED(com_hr);

  DWORD task_index = 0;
  // MMCSS keeps the render loop prioritized without spinning.
  HANDLE mmcss_handle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);

  HANDLE wait_handles[2] = {audio_event_, stop_event_};

  while (running_) {
    const DWORD wait_result = WaitForMultipleObjects(2, wait_handles, FALSE, INFINITE);
    if (wait_result == WAIT_OBJECT_0 + 1) {
      break;
    }
    if (wait_result != WAIT_OBJECT_0) {
      break;
    }
    if (!running_) {
      break;
    }

    RenderAudio();
  }

  if (mmcss_handle) {
    AvRevertMmThreadCharacteristics(mmcss_handle);
  }

  if (com_should_uninit) {
    CoUninitialize();
  }
}

void WasapiOutput::RenderAudio() {
  if (!render_api_.GetCurrentPadding || !render_api_.GetBuffer || !render_api_.ReleaseBuffer) {
    return;
  }

  UINT32 padding = 0;
  if (FAILED(render_api_.GetCurrentPadding(render_api_.context, &padding))) {
    return;
  }

  if (padding >= buffer_frames_) {
    return;
  }

  const UINT32 frames_available = buffer_frames_ - padding;
  if (frames_available == 0) {
    return;
  }

  BYTE* data = nullptr;
  if (FAILED(render_api_.GetBuffer(render_api_.context, frames_available, &data)) || !data) {
    return;
  }

  if (sample_format_ != SampleFormat::Float32) {
    render_api_.ReleaseBuffer(render_api_.context, frames_available, AUDCLNT_BUFFERFLAGS_SILENT);
    return;
  }

  float* out = reinterpret_cast<float*>(data);
  const uint32_t frames_read = detail::ConsumeRingBufferFloat(ring_buffer_,
                                                              out,
                                                              frames_available,
                                                              channels_,
                                                              &underrun_wake_count_,
                                                              &underrun_frame_count_);

  const DWORD flags = frames_read == 0 ? AUDCLNT_BUFFERFLAGS_SILENT : 0;
  render_api_.ReleaseBuffer(render_api_.context, frames_available, flags);
}

#if defined(TOMPLAYER_TESTING)
void WasapiOutput::set_start_stop_api_for_test(const detail::StartStopApi& api,
                                               HANDLE audio_event,
                                               HANDLE stop_event) {
  start_stop_api_ = api;
  audio_event_ = audio_event;
  stop_event_ = stop_event;
}

void WasapiOutput::set_channels_for_test(uint16_t channels) {
  channels_ = channels;
}
#endif

}  // namespace wasapi
}  // namespace tomplayer
