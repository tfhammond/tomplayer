#ifndef TOMPLAYER_TESTING
#define TOMPLAYER_TESTING 1
#endif

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <windows.h>
#include <ks.h>
#include <ksmedia.h>

#include "audio/wasapi_output.h"
#include "buffer/audio_ring_buffer.h"

namespace {
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

// Confirms PCM16 path converts zero-filled tail to zeros in endpoint buffer.
TEST_CASE("ConsumeRingBufferFloat underrun yields zero PCM16 tail") {
  const uint32_t channels = 2;
  AudioRingBuffer buffer(4, channels);
  std::array<float, 4> input = {1.0f, -1.0f, 0.5f, -0.5f};
  std::array<float, 8> scratch{};
  std::array<int16_t, 8> output{};
  std::atomic<uint64_t> underrun_wakes{0};
  std::atomic<uint64_t> underrun_frames{0};

  REQUIRE(buffer.write_frames(input.data(), 2) == 2);

  const uint32_t frames_read = tomplayer::wasapi::detail::ConsumeRingBufferFloat(
      &buffer,
      scratch.data(),
      4,
      channels,
      &underrun_wakes,
      &underrun_frames);

  REQUIRE(frames_read == 2);

  tomplayer::wasapi::detail::ConvertFloatToPcm16(
      scratch.data(),
      output.data(),
      output.size());

  REQUIRE(output[0] == 32767);
  REQUIRE(output[1] == -32767);
  REQUIRE(output[2] == 16383);
  REQUIRE(output[3] == -16383);
  REQUIRE(output[4] == 0);
  REQUIRE(output[5] == 0);
  REQUIRE(output[6] == 0);
  REQUIRE(output[7] == 0);
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

    AudioRingBuffer buffer(1, 2);
    output.set_ring_buffer(&buffer);
    output.set_channels_for_test(2);
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

    AudioRingBuffer buffer(1, 2);
    output.set_ring_buffer(&buffer);
    output.set_channels_for_test(2);
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
