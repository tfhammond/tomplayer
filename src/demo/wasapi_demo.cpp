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

namespace demo {
namespace {
constexpr float kTwoPi = 6.28318530717958647692f;

struct DemoOptions {
  int repeat = 3;
  double seconds = 2.0;
  bool stress = false;
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

bool RenderSine(float* out, uint32_t frames, uint32_t channels, void* user) {
  auto* state = static_cast<SineState*>(user);
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
  return true;
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

  const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(com_hr)) {
    std::cerr << "CoInitializeEx failed: 0x" << std::hex << com_hr << "\n";
    return 1;
  }

  tomplayer::wasapi::WasapiOutput output;
  SineState sine;
  if (!output.init_default_device(&RenderSine, &sine)) {
    std::cerr << "Failed to initialize WASAPI output.\n";
    CoUninitialize();
    return 1;
  }

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

  for (int i = 0; i < options.repeat; ++i) {
    if (!output.start()) {
      std::cerr << "Failed to start audio.\n";
      break;
    }

    std::this_thread::sleep_for(std::chrono::duration<double>(options.seconds));
    output.stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

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
