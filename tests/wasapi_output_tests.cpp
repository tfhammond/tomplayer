#ifndef TOMPLAYER_TESTING
#define TOMPLAYER_TESTING 1
#endif

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <vector>

#include <windows.h>
#include <ks.h>
#include <ksmedia.h>

#include "audio/wasapi_output.h"
#include "buffer/audio_ring_buffer.h"

namespace {
struct FakeRenderApi {
  UINT32 padding = 0;
  HRESULT padding_hr = S_OK;
  HRESULT get_buffer_hr = S_OK;
  BYTE* buffer = nullptr;
  int get_padding_calls = 0;
  int get_buffer_calls = 0;
  int release_calls = 0;
  UINT32 last_getbuffer_frames = 0;
  UINT32 last_release_frames = 0;
  DWORD last_release_flags = 0;

  static HRESULT GetCurrentPaddingThunk(void* ctx, UINT32* out_padding) {
    auto* self = static_cast<FakeRenderApi*>(ctx);
    self->get_padding_calls++;
    if (self->padding_hr != S_OK) {
      return self->padding_hr;
    }
    *out_padding = self->padding;
    return S_OK;
  }

  static HRESULT GetBufferThunk(void* ctx, UINT32 frames, BYTE** data) {
    auto* self = static_cast<FakeRenderApi*>(ctx);
    self->get_buffer_calls++;
    self->last_getbuffer_frames = frames;
    if (self->get_buffer_hr != S_OK) {
      return self->get_buffer_hr;
    }
    *data = self->buffer;
    return S_OK;
  }

  static HRESULT ReleaseBufferThunk(void* ctx, UINT32 frames, DWORD flags) {
    auto* self = static_cast<FakeRenderApi*>(ctx);
    self->release_calls++;
    self->last_release_frames = frames;
    self->last_release_flags = flags;
    return S_OK;
  }

  tomplayer::wasapi::detail::RenderApi api() {
    return {this, &GetCurrentPaddingThunk, &GetBufferThunk, &ReleaseBufferThunk};
  }
};

struct FakeStartStopApi {
  HRESULT start_hr = S_OK;
  HRESULT stop_hr = S_OK;
  HRESULT reset_hr = S_OK;
  int start_calls = 0;
  int stop_calls = 0;
  int reset_calls = 0;

  static HRESULT StartThunk(void* ctx) {
    auto* self = static_cast<FakeStartStopApi*>(ctx);
    self->start_calls++;
    return self->start_hr;
  }

  static HRESULT StopThunk(void* ctx) {
    auto* self = static_cast<FakeStartStopApi*>(ctx);
    self->stop_calls++;
    return self->stop_hr;
  }

  static HRESULT ResetThunk(void* ctx) {
    auto* self = static_cast<FakeStartStopApi*>(ctx);
    self->reset_calls++;
    return self->reset_hr;
  }

  tomplayer::wasapi::detail::StartStopApi api() {
    return {this, &StartThunk, &StopThunk, &ResetThunk};
  }
};

struct CallbackState {
  bool called = false;
  bool return_value = true;
  uint32_t frames = 0;
  uint32_t channels = 0;
  float fill_value = 0.0f;
};

bool FillCallback(float* out, uint32_t frames, uint32_t channels, void* user) {
  auto* state = static_cast<CallbackState*>(user);
  state->called = true;
  state->frames = frames;
  state->channels = channels;
  const uint32_t samples = frames * channels;
  for (uint32_t i = 0; i < samples; ++i) {
    out[i] = state->fill_value;
  }
  return state->return_value;
}

struct WinHandle {
  HANDLE handle = nullptr;
  explicit WinHandle(HANDLE value) : handle(value) {}
  ~WinHandle() {
    if (handle) {
      CloseHandle(handle);
    }
  }
  HANDLE release() {
    HANDLE temp = handle;
    handle = nullptr;
    return temp;
  }
};
}  // namespace

// Validates mix-format detection for PCM/float/extensible cases.
TEST_CASE("DetectSampleFormat handles all mix format branches") {
  using tomplayer::wasapi::SampleFormat;

  SECTION("nullptr is unsupported") {
    REQUIRE(tomplayer::wasapi::detail::DetectSampleFormat(nullptr) == SampleFormat::Unsupported);
  }

  SECTION("IEEE float 32-bit") {
    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    fmt.wBitsPerSample = 32;
    REQUIRE(tomplayer::wasapi::detail::DetectSampleFormat(&fmt) == SampleFormat::Float32);
  }

  SECTION("PCM 16-bit") {
    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.wBitsPerSample = 16;
    REQUIRE(tomplayer::wasapi::detail::DetectSampleFormat(&fmt) == SampleFormat::Pcm16);
  }

  SECTION("Extensible IEEE float 32-bit") {
    WAVEFORMATEXTENSIBLE fmt{};
    fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    fmt.Format.wBitsPerSample = 32;
    fmt.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    REQUIRE(tomplayer::wasapi::detail::DetectSampleFormat(&fmt.Format) == SampleFormat::Float32);
  }

  SECTION("Extensible PCM 16-bit") {
    WAVEFORMATEXTENSIBLE fmt{};
    fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    fmt.Format.wBitsPerSample = 16;
    fmt.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    REQUIRE(tomplayer::wasapi::detail::DetectSampleFormat(&fmt.Format) == SampleFormat::Pcm16);
  }

  SECTION("Extensible unsupported") {
    WAVEFORMATEXTENSIBLE fmt{};
    fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    fmt.Format.wBitsPerSample = 24;
    fmt.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    REQUIRE(tomplayer::wasapi::detail::DetectSampleFormat(&fmt.Format) == SampleFormat::Unsupported);
  }
}

// Verifies clamp/scaling rules for float-to-PCM16 conversion used on the render path.
TEST_CASE("ConvertFloatToPcm16 clamps and scales") {
  std::array<float, 6> input = {1.0f, -1.0f, 0.5f, -0.5f, 1.5f, -1.5f};
  std::array<int16_t, 6> output{};

  tomplayer::wasapi::detail::ConvertFloatToPcm16(input.data(), output.data(), output.size());

  REQUIRE(output[0] == 32767);
  REQUIRE(output[1] == -32767);
  REQUIRE(output[2] == 16383);
  REQUIRE(output[3] == -16383);
  REQUIRE(output[4] == 32767);
  REQUIRE(output[5] == -32767);
}

// Validates ring-buffer consumption zero-fills missing frames on underrun.
TEST_CASE("ConsumeRingBufferFloat zero-fills tail on underrun") {
  const uint32_t channels = 2;
  AudioRingBuffer buffer(4, channels);
  std::array<float, 4> input = {1.0f, 2.0f, 3.0f, 4.0f};
  std::array<float, 8> output{};
  std::atomic<uint64_t> underrun_wakes{0};
  std::atomic<uint64_t> underrun_frames{0};

  REQUIRE(buffer.write_frames(input.data(), 2) == 2);

  const uint32_t frames_read = tomplayer::wasapi::detail::ConsumeRingBufferFloat(
      &buffer,
      output.data(),
      4,
      channels,
      &underrun_wakes,
      &underrun_frames);

  REQUIRE(frames_read == 2);
  REQUIRE(output[0] == 1.0f);
  REQUIRE(output[1] == 2.0f);
  REQUIRE(output[2] == 3.0f);
  REQUIRE(output[3] == 4.0f);
  REQUIRE(output[4] == 0.0f);
  REQUIRE(output[5] == 0.0f);
  REQUIRE(output[6] == 0.0f);
  REQUIRE(output[7] == 0.0f);
  REQUIRE(underrun_wakes.load() == 1);
  REQUIRE(underrun_frames.load() == 2);
}

// Exercises render-path contract boundaries without any real WASAPI objects.
TEST_CASE("RenderAudioCore honors padding and buffer contract") {
  FakeRenderApi fake;
  CallbackState callback_state;
  callback_state.fill_value = 0.25f;
  const uint32_t buffer_frames = 8;
  const uint32_t channels = 2;

  SECTION("early return on padding failure") {
    fake.padding_hr = E_FAIL;
    tomplayer::wasapi::detail::RenderAudioCore(fake.api(),
                                   &FillCallback,
                                   &callback_state,
                                   buffer_frames,
                                   channels,
                                   tomplayer::wasapi::SampleFormat::Float32,
                                   nullptr);
    REQUIRE(fake.get_padding_calls == 1);
    REQUIRE(fake.get_buffer_calls == 0);
    REQUIRE(fake.release_calls == 0);
  }

  SECTION("padding >= buffer frames returns without GetBuffer") {
    fake.padding = buffer_frames;
    tomplayer::wasapi::detail::RenderAudioCore(fake.api(),
                                   &FillCallback,
                                   &callback_state,
                                   buffer_frames,
                                   channels,
                                   tomplayer::wasapi::SampleFormat::Float32,
                                   nullptr);
    REQUIRE(fake.get_buffer_calls == 0);
    REQUIRE(fake.release_calls == 0);
  }

  SECTION("padding greater than buffer frames returns without GetBuffer") {
    fake.padding = buffer_frames + 1;
    tomplayer::wasapi::detail::RenderAudioCore(fake.api(),
                                   &FillCallback,
                                   &callback_state,
                                   buffer_frames,
                                   channels,
                                   tomplayer::wasapi::SampleFormat::Float32,
                                   nullptr);
    REQUIRE(fake.get_buffer_calls == 0);
    REQUIRE(fake.release_calls == 0);
  }

  SECTION("GetBuffer failure avoids ReleaseBuffer") {
    std::array<float, 16> buffer{};
    fake.buffer = reinterpret_cast<BYTE*>(buffer.data());
    fake.get_buffer_hr = E_FAIL;
    tomplayer::wasapi::detail::RenderAudioCore(fake.api(),
                                   &FillCallback,
                                   &callback_state,
                                   buffer_frames,
                                   channels,
                                   tomplayer::wasapi::SampleFormat::Float32,
                                   nullptr);
    REQUIRE(fake.get_buffer_calls == 1);
    REQUIRE(fake.release_calls == 0);
  }

  SECTION("GetBuffer null avoids ReleaseBuffer") {
    fake.buffer = nullptr;
    tomplayer::wasapi::detail::RenderAudioCore(fake.api(),
                                   &FillCallback,
                                   &callback_state,
                                   buffer_frames,
                                   channels,
                                   tomplayer::wasapi::SampleFormat::Float32,
                                   nullptr);
    REQUIRE(fake.get_buffer_calls == 1);
    REQUIRE(fake.release_calls == 0);
  }

  SECTION("float path writes and releases without silent flag") {
    std::array<float, 16> buffer{};
    fake.buffer = reinterpret_cast<BYTE*>(buffer.data());
    fake.padding = 2;
    callback_state.return_value = true;

    tomplayer::wasapi::detail::RenderAudioCore(fake.api(),
                                   &FillCallback,
                                   &callback_state,
                                   buffer_frames,
                                   channels,
                                   tomplayer::wasapi::SampleFormat::Float32,
                                   nullptr);

    REQUIRE(fake.get_buffer_calls == 1);
    REQUIRE(fake.last_getbuffer_frames == buffer_frames - fake.padding);
    REQUIRE(callback_state.called);
    REQUIRE(fake.release_calls == 1);
    REQUIRE(fake.last_release_flags == 0);
    REQUIRE(buffer[0] == 0.25f);
  }

  SECTION("callback false renders silence") {
    std::array<float, 16> buffer{};
    fake.buffer = reinterpret_cast<BYTE*>(buffer.data());
    callback_state.return_value = false;

    tomplayer::wasapi::detail::RenderAudioCore(fake.api(),
                                   &FillCallback,
                                   &callback_state,
                                   buffer_frames,
                                   channels,
                                   tomplayer::wasapi::SampleFormat::Float32,
                                   nullptr);
    REQUIRE(fake.release_calls == 1);
    REQUIRE(fake.last_release_flags == AUDCLNT_BUFFERFLAGS_SILENT);
  }

  SECTION("missing callback renders silence") {
    std::array<float, 16> buffer{};
    fake.buffer = reinterpret_cast<BYTE*>(buffer.data());

    tomplayer::wasapi::detail::RenderAudioCore(fake.api(),
                                   nullptr,
                                   nullptr,
                                   buffer_frames,
                                   channels,
                                   tomplayer::wasapi::SampleFormat::Float32,
                                   nullptr);
    REQUIRE(fake.release_calls == 1);
    REQUIRE(fake.last_release_flags == AUDCLNT_BUFFERFLAGS_SILENT);
  }

  SECTION("unsupported format renders silence") {
    std::array<float, 16> buffer{};
    fake.buffer = reinterpret_cast<BYTE*>(buffer.data());

    tomplayer::wasapi::detail::RenderAudioCore(fake.api(),
                                   &FillCallback,
                                   &callback_state,
                                   buffer_frames,
                                   channels,
                                   tomplayer::wasapi::SampleFormat::Unsupported,
                                   nullptr);
    REQUIRE(fake.release_calls == 1);
    REQUIRE(fake.last_release_flags == AUDCLNT_BUFFERFLAGS_SILENT);
  }
}

// Confirms PCM16 output conversion path produces expected samples and flags.
TEST_CASE("RenderAudioCore converts float to PCM16") {
  FakeRenderApi fake;
  CallbackState callback_state;
  callback_state.fill_value = 0.5f;
  const uint32_t buffer_frames = 2;
  const uint32_t channels = 2;

  std::array<int16_t, 4> buffer{};
  std::array<float, 4> scratch{};
  fake.buffer = reinterpret_cast<BYTE*>(buffer.data());

  tomplayer::wasapi::detail::RenderAudioCore(fake.api(),
                                 &FillCallback,
                                 &callback_state,
                                 buffer_frames,
                                 channels,
                                 tomplayer::wasapi::SampleFormat::Pcm16,
                                 scratch.data());

  REQUIRE(fake.release_calls == 1);
  REQUIRE(fake.last_release_flags == 0);
  REQUIRE(buffer[0] == 16383);
  REQUIRE(buffer[1] == 16383);
  REQUIRE(buffer[2] == 16383);
  REQUIRE(buffer[3] == 16383);
}

// Ensures missing conversion scratch forces silent rendering for PCM16 formats.
TEST_CASE("RenderAudioCore uses silent flag when PCM16 scratch is missing") {
  FakeRenderApi fake;
  CallbackState callback_state;
  const uint32_t buffer_frames = 4;
  const uint32_t channels = 1;
  std::array<int16_t, 4> buffer{};
  fake.buffer = reinterpret_cast<BYTE*>(buffer.data());

  tomplayer::wasapi::detail::RenderAudioCore(fake.api(),
                                 &FillCallback,
                                 &callback_state,
                                 buffer_frames,
                                 channels,
                                 tomplayer::wasapi::SampleFormat::Pcm16,
                                 nullptr);

  REQUIRE(fake.release_calls == 1);
  REQUIRE(fake.last_release_flags == AUDCLNT_BUFFERFLAGS_SILENT);
}

// Covers lifecycle paths that can be validated without COM or real devices.
TEST_CASE("WasapiOutput lifecycle without COM objects is safe") {
  tomplayer::wasapi::WasapiOutput output;

  SECTION("start fails without initialization") {
    REQUIRE_FALSE(output.start());
  }

  SECTION("start/stop idempotence with test seam") {
    FakeStartStopApi fake;
    WinHandle audio_event(CreateEvent(nullptr, FALSE, FALSE, nullptr));
    WinHandle stop_event(CreateEvent(nullptr, TRUE, FALSE, nullptr));
    REQUIRE(audio_event.handle != nullptr);
    REQUIRE(stop_event.handle != nullptr);

    output.set_start_stop_api_for_test(fake.api(), audio_event.release(), stop_event.release());

    REQUIRE(output.start());
    REQUIRE(output.is_running_for_test());
    REQUIRE_FALSE(output.start());

    output.stop();
    REQUIRE_FALSE(output.is_running_for_test());
    output.stop();

    REQUIRE(fake.start_calls == 1);
    REQUIRE(fake.stop_calls == 1);
    REQUIRE(fake.reset_calls == 1);

    output.shutdown();
  }

  SECTION("start failure unwinds cleanly") {
    FakeStartStopApi fake;
    fake.start_hr = E_FAIL;
    WinHandle audio_event(CreateEvent(nullptr, FALSE, FALSE, nullptr));
    WinHandle stop_event(CreateEvent(nullptr, TRUE, FALSE, nullptr));
    REQUIRE(audio_event.handle != nullptr);
    REQUIRE(stop_event.handle != nullptr);

    output.set_start_stop_api_for_test(fake.api(), audio_event.release(), stop_event.release());

    REQUIRE_FALSE(output.start());
    REQUIRE_FALSE(output.is_running_for_test());
    REQUIRE(fake.start_calls == 1);
    REQUIRE(fake.stop_calls == 0);
    REQUIRE(fake.reset_calls == 0);

    output.shutdown();
  }

  SECTION("shutdown is safe to call twice") {
    output.shutdown();
    output.shutdown();
  }
}
