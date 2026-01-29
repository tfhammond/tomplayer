#include "demo/wasapi_demo.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <thread>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <objbase.h>

#include "audio/wasapi_output.h"
#include "buffer/audio_ring_buffer.h"
#include "engine/player_engine.h"

namespace demo {
namespace {
constexpr float kTwoPi = 6.28318530717958647692f;

struct DemoOptions {
  int repeat = 3;
  double seconds = 2.0;
  bool stress = false;
  bool engine_smoke = false;
  float frequency = 440.0f;
  bool show_help = false;
};

struct SineState {
  float phase = 0.0f;
  float phase_increment = 0.0f;
  float amplitude = 0.2f;
};

void PrintUsage(std::string_view exe_name) {
  std::cout << "Usage: " << exe_name << " [options]\n"
            << "  --repeat N     Number of start/stop cycles (default: 3)\n"
            << "  --seconds N    Seconds per cycle (default: 2.0)\n"
            << "  --frequency N  Tone frequency in Hz (default: 440)\n"
            << "  --stress       Run CPU load during playback\n"
            << "  --engine_smoke Run PlayerEngine smoke test\n"
            << "  --help         Show this help\n";
}

bool ParseArgs(int argc, char* argv[], DemoOptions* options) {
  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      options->show_help = true;
      return true;
    }
    if (arg == "--repeat" && i + 1 < argc) {
      options->repeat = static_cast<int>(std::strtol(argv[++i], nullptr, 10));
      if (options->repeat < 1) {
        options->repeat = 1;
      }
      continue;
    }
    if (arg == "--seconds" && i + 1 < argc) {
      options->seconds = std::strtod(argv[++i], nullptr);
      if (options->seconds <= 0.0) {
        options->seconds = 0.5;
      }
      continue;
    }
    if (arg == "--frequency" && i + 1 < argc) {
      options->frequency = static_cast<float>(std::strtod(argv[++i], nullptr));
      if (options->frequency < 1.0f) {
        options->frequency = 440.0f;
      }
      continue;
    }
    if (arg == "--stress") {
      options->stress = true;
      continue;
    }
    if (arg == "--engine_smoke") {
      options->engine_smoke = true;
      continue;
    }

    return false;
  }

  return true;
}

const char* SampleFormatToString(tomplayer::wasapi::SampleFormat format) {
  switch (format) {
    case tomplayer::wasapi::SampleFormat::Float32:
      return "float32";
    case tomplayer::wasapi::SampleFormat::Pcm16:
      return "pcm16";
    default:
      return "unsupported";
  }
}

void FillSine(float* out, uint32_t frames, uint32_t channels, SineState* state) {
  float phase = state->phase;
  const float increment = state->phase_increment;

  for (uint32_t frame = 0; frame < frames; ++frame) {
    const float sample = std::sin(phase) * state->amplitude;
    phase += increment;
    if (phase >= kTwoPi) {
      phase -= kTwoPi;
    }
    const uint32_t base = frame * channels;
    for (uint32_t ch = 0; ch < channels; ++ch) {
      out[base + ch] = sample;
    }
  }

  state->phase = phase;
}

void StressWorker(std::atomic<bool>* running) {
  volatile double value = 0.0;
  while (running->load(std::memory_order_relaxed)) {
    value += 0.000001;
    if (value > 1000.0) {
      value = 0.0;
    }
  }
}

void PrintEngineStatus(const char* label,
                       const tomplayer::engine::PlayerEngine& engine) {
  const auto status = engine.get_status();
  std::cout << label
            << " state=" << static_cast<int>(status.state)
            << " position=" << status.position_seconds
            << " decode_epoch=" << status.decode_epoch
            << " decode_mode=" << static_cast<int>(status.decode_mode)
            << " seek_target_frame=" << status.seek_target_frame;
  if (!status.last_error.empty()) {
    std::cout << " error=" << status.last_error;
  }
  std::cout << "\n";
}
}  // namespace

int RunWasapiDemo(int argc, char* argv[]) {
  DemoOptions options;
  if (!ParseArgs(argc, argv, &options)) {
    PrintUsage(argv[0]);
    return 1;
  }
  if (options.show_help) {
    PrintUsage(argv[0]);
    return 0;
  }

  if (options.engine_smoke) {
    tomplayer::engine::PlayerEngine engine;
    PrintEngineStatus("startup", engine);

    engine.play();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    PrintEngineStatus("after play", engine);

    engine.seek_seconds(10.0);
    engine.seek_seconds(30.0);
    engine.seek_seconds(5.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    PrintEngineStatus("after seeks", engine);

    engine.pause();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    PrintEngineStatus("after pause", engine);

    engine.resume();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    PrintEngineStatus("after resume", engine);

    engine.stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    PrintEngineStatus("after stop", engine);

    engine.quit();
    return 0;
  }

  const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(com_hr)) {
    std::cerr << "CoInitializeEx failed: 0x" << std::hex << com_hr << "\n";
    return 1;
  }

  tomplayer::wasapi::WasapiOutput output;
  if (!output.init_default_device()) {
    std::cerr << "Failed to initialize WASAPI output.\n";
    CoUninitialize();
    return 1;
  }

  const uint32_t channels = output.channels();
  const uint32_t capacity_frames =
      std::max(1u, output.buffer_frames() * 4);
  AudioRingBuffer ring_buffer(capacity_frames, channels);
  output.set_ring_buffer(&ring_buffer);

  SineState sine;
  sine.phase_increment =
      kTwoPi * options.frequency / static_cast<float>(output.sample_rate());

  std::cout << "Mix format: " << output.sample_rate() << " Hz, " << output.channels()
            << " ch, " << SampleFormatToString(output.sample_format()) << "\n";

  if (output.sample_format() == tomplayer::wasapi::SampleFormat::Unsupported) {
    std::cout << "Mix format unsupported; rendering silence.\n";
  }

  std::atomic<bool> stress_running{false};
  std::vector<std::thread> stress_threads;
  if (options.stress) {
    stress_running.store(true);
    const unsigned int thread_count =
        std::max(1u, std::thread::hardware_concurrency());
    stress_threads.reserve(thread_count);
    for (unsigned int i = 0; i < thread_count; ++i) {
      stress_threads.emplace_back(StressWorker, &stress_running);
    }
  }

  std::atomic<bool> producer_running{true};
  std::atomic<bool> playback_active{false};
  std::atomic<bool> producer_idle{true};

  std::thread producer([&]() {
    const uint32_t chunk_frames = 256;
    std::vector<float> chunk(static_cast<size_t>(chunk_frames) * channels);
    while (producer_running.load(std::memory_order_relaxed)) {
      if (!playback_active.load(std::memory_order_acquire)) {
        producer_idle.store(true, std::memory_order_release);
        std::this_thread::yield();
        continue;
      }
      producer_idle.store(false, std::memory_order_release);

      const uint32_t writable = ring_buffer.available_to_write_frames();
      if (writable < chunk_frames) {
        std::this_thread::yield();
        continue;
      }

      FillSine(chunk.data(), chunk_frames, channels, &sine);
      ring_buffer.write_frames(chunk.data(), chunk_frames);
    }
  });

  const uint32_t drain_chunk_frames = 256;
  std::vector<float> drain(static_cast<size_t>(drain_chunk_frames) * channels);

  for (int i = 0; i < options.repeat; ++i) {
    playback_active.store(false, std::memory_order_release);
    while (!producer_idle.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    while (true) {
      const uint32_t available = ring_buffer.available_to_read_frames();
      if (available == 0) {
        break;
      }
      const uint32_t to_read =
          available < drain_chunk_frames ? available : drain_chunk_frames;
      ring_buffer.read_frames(drain.data(), to_read);
    }
    ring_buffer.reset();

    playback_active.store(true, std::memory_order_release);
    if (!output.start()) {
      std::cerr << "Failed to start audio.\n";
      break;
    }

    std::this_thread::sleep_for(std::chrono::duration<double>(options.seconds));
    output.stop();
  }

  playback_active.store(false, std::memory_order_release);
  producer_running.store(false, std::memory_order_release);
  producer.join();

  if (options.stress) {
    stress_running.store(false);
    for (auto& thread : stress_threads) {
      thread.join();
    }
  }

  output.shutdown();
  CoUninitialize();
  return 0;
}
}  // namespace demo
