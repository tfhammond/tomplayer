#include "audio/wasapi_output.h"

#include <avrt.h>
#include <ksmedia.h>

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

void ConvertFloatToPcm16(const float* in, int16_t* out, std::size_t samples) {
  for (std::size_t i = 0; i < samples; ++i) {
    float sample = in[i];
    if (sample > 1.0f) {
      sample = 1.0f;
    } else if (sample < -1.0f) {
      sample = -1.0f;
    }
    out[i] = static_cast<int16_t>(sample * 32767.0f);
  }
}

void RenderAudioCore(const RenderApi& api,
                     RenderCallback callback,
                     void* callback_user,
                     uint32_t buffer_frames,
                     uint32_t channels,
                     SampleFormat format,
                     float* float_scratch) {
  if (!api.GetCurrentPadding || !api.GetBuffer || !api.ReleaseBuffer) {
    return;
  }

  UINT32 padding = 0;
  if (FAILED(api.GetCurrentPadding(api.context, &padding))) {
    return;
  }

  if (padding >= buffer_frames) {
    return;
  }

  const UINT32 frames_available = buffer_frames - padding;
  if (frames_available == 0) {
    return;
  }

  BYTE* data = nullptr;
  if (FAILED(api.GetBuffer(api.context, frames_available, &data)) || !data) {
    return;
  }

  bool wrote_audio = false;
  if (format == SampleFormat::Float32) {
    float* out = reinterpret_cast<float*>(data);
    if (callback) {
      wrote_audio = callback(out, frames_available, channels, callback_user);
    }
  } else if (format == SampleFormat::Pcm16 && float_scratch) {
    bool ok = false;
    if (callback) {
      ok = callback(float_scratch, frames_available, channels, callback_user);
    }
    if (ok) {
      const std::size_t samples =
          static_cast<std::size_t>(frames_available) * channels;
      ConvertFloatToPcm16(float_scratch, reinterpret_cast<int16_t*>(data), samples);
      wrote_audio = true;
    }
  }

  const DWORD flags = wrote_audio ? 0 : AUDCLNT_BUFFERFLAGS_SILENT;
  api.ReleaseBuffer(api.context, frames_available, flags);
}
}  // namespace detail

WasapiOutput::WasapiOutput() = default;

WasapiOutput::~WasapiOutput() {
  shutdown();
}

bool WasapiOutput::init_default_device(RenderCallback callback, void* user) {
  // Do setup here so the render path stays allocation-free and deterministic.
  if (audio_client_) {
    return false;
  }

  callback_ = callback;
  callback_user_ = user;

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

  sample_rate_ = mix_format_->nSamplesPerSec;
  channels_ = mix_format_->nChannels;
  bits_per_sample_ = mix_format_->wBitsPerSample;
  block_align_ = mix_format_->nBlockAlign;
  sample_format_ = detail::DetectSampleFormat(mix_format_);

  hr = audio_client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                 0, 0, mix_format_, nullptr);
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

  if (sample_format_ == SampleFormat::Pcm16) {
    // Allocate conversion scratch up-front to keep the render path allocation-free.
    const size_t samples = static_cast<size_t>(buffer_frames_) * channels_;
    float_scratch_ = std::make_unique<float[]>(samples);
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
  // Render client access stays on the render thread to avoid cross-thread COM calls.
  if (!start_stop_api_.Start || !audio_event_ || !stop_event_) {
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

  callback_ = nullptr;
  callback_user_ = nullptr;
  float_scratch_.reset();
  render_api_ = {};
  start_stop_api_ = {};
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
  const bool com_ok = SUCCEEDED(com_hr);

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

  if (com_ok) {
    CoUninitialize();
  }
}

void WasapiOutput::RenderAudio() {
  // Shared-mode contract: only fill available frames and always release the buffer.
  detail::RenderAudioCore(render_api_,
                          callback_,
                          callback_user_,
                          buffer_frames_,
                          channels_,
                          sample_format_,
                          float_scratch_.get());
}

#if defined(TOMPLAYER_TESTING)
void WasapiOutput::set_start_stop_api_for_test(const detail::StartStopApi& api,
                                               HANDLE audio_event,
                                               HANDLE stop_event) {
  start_stop_api_ = api;
  audio_event_ = audio_event;
  stop_event_ = stop_event;
}
#endif

}  // namespace wasapi
}  // namespace tomplayer
