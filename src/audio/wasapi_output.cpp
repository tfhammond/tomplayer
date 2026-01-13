#include "audio/wasapi_output.h"

#include <avrt.h>
#include <ksmedia.h>

namespace audio {
namespace {
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
}  // namespace

WasapiOutput::WasapiOutput() = default;

WasapiOutput::~WasapiOutput() {
  shutdown();
}

bool WasapiOutput::init_default_device(RenderCallback callback, void* user) {
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

  hr = audio_client_->GetMixFormat(&mix_format_);
  if (FAILED(hr)) {
    shutdown();
    return false;
  }

  sample_rate_ = mix_format_->nSamplesPerSec;
  channels_ = mix_format_->nChannels;
  bits_per_sample_ = mix_format_->wBitsPerSample;
  block_align_ = mix_format_->nBlockAlign;
  sample_format_ = DetectSampleFormat(mix_format_);

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
    const size_t samples = static_cast<size_t>(buffer_frames_) * channels_;
    float_scratch_ = std::make_unique<float[]>(samples);
  }

  return true;
}

bool WasapiOutput::start() {
  if (!audio_client_ || !render_client_ || !audio_event_ || !stop_event_) {
    return false;
  }

  if (running_.exchange(true)) {
    return false;
  }

  ResetEvent(stop_event_);
  render_thread_ = std::thread(&WasapiOutput::RenderLoop, this);

  const HRESULT hr = audio_client_->Start();
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
  if (!running_.exchange(false)) {
    return;
  }

  SetEvent(stop_event_);

  if (render_thread_.joinable()) {
    render_thread_.join();
  }

  if (audio_client_) {
    audio_client_->Stop();
    audio_client_->Reset();
  }
}

void WasapiOutput::shutdown() {
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

  buffer_frames_ = 0;
  sample_rate_ = 0;
  channels_ = 0;
  bits_per_sample_ = 0;
  block_align_ = 0;
  sample_format_ = SampleFormat::Unsupported;
}

void WasapiOutput::RenderLoop() {
  const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool com_ok = SUCCEEDED(com_hr);

  DWORD task_index = 0;
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
  UINT32 padding = 0;
  if (FAILED(audio_client_->GetCurrentPadding(&padding))) {
    return;
  }

  const UINT32 frames_available = buffer_frames_ - padding;
  if (frames_available == 0) {
    return;
  }

  BYTE* data = nullptr;
  if (FAILED(render_client_->GetBuffer(frames_available, &data)) || !data) {
    return;
  }

  bool wrote_audio = false;
  if (sample_format_ == SampleFormat::Float32) {
    float* out = reinterpret_cast<float*>(data);
    if (callback_) {
      wrote_audio = callback_(out, frames_available, channels_, callback_user_);
    }
  } else if (sample_format_ == SampleFormat::Pcm16 && float_scratch_) {
    float* scratch = float_scratch_.get();
    bool ok = false;
    if (callback_) {
      ok = callback_(scratch, frames_available, channels_, callback_user_);
    }
    if (ok) {
      const size_t samples = static_cast<size_t>(frames_available) * channels_;
      int16_t* out = reinterpret_cast<int16_t*>(data);
      for (size_t i = 0; i < samples; ++i) {
        float sample = scratch[i];
        if (sample > 1.0f) {
          sample = 1.0f;
        } else if (sample < -1.0f) {
          sample = -1.0f;
        }
        out[i] = static_cast<int16_t>(sample * 32767.0f);
      }
      wrote_audio = true;
    }
  }

  const DWORD flags = wrote_audio ? 0 : AUDCLNT_BUFFERFLAGS_SILENT;
  render_client_->ReleaseBuffer(frames_available, flags);
}

}  // namespace audio
