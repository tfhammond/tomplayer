// Ring buffer unit tests validate correctness, interleaving, and SPSC safety.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <cmath>
#include <thread>
#include <vector>

#include "buffer/audio_ring_buffer.h"

namespace {
constexpr uint32_t kChannelStride = 1000;

std::vector<float> MakePattern(uint32_t frames, uint32_t base) {
  constexpr uint32_t channels = 2;
  std::vector<float> data(static_cast<size_t>(frames) * channels);
  for (uint32_t frame = 0; frame < frames; ++frame) {
    for (uint32_t ch = 0; ch < channels; ++ch) {
      const uint32_t value = base + frame + ch * kChannelStride;
      data[static_cast<size_t>(frame) * channels + ch] = static_cast<float>(value);
    }
  }
  return data;
}
}  // namespace

// Verifies round-trip write/read preserves interleaved data exactly.
TEST_CASE("AudioRingBuffer round-trip preserves samples") {
  constexpr uint32_t channels = 2;
  AudioRingBuffer buffer(16, channels);

  auto input = MakePattern(10, 0);
  std::vector<float> output(input.size(), 0.0f);

  REQUIRE(buffer.write_frames(input.data(), 10) == 10);
  REQUIRE(buffer.read_frames(output.data(), 10) == 10);

  REQUIRE(output == input);
}

// Forces wrap-around by interleaving reads/writes across the end boundary.
TEST_CASE("AudioRingBuffer wrap-around preserves order") {
  constexpr uint32_t channels = 2;
  AudioRingBuffer buffer(8, channels);

  auto first = MakePattern(6, 0);   // frames 0..5
  auto second = MakePattern(6, 6);  // frames 6..11

  REQUIRE(buffer.write_frames(first.data(), 6) == 6);

  std::vector<float> temp(static_cast<size_t>(4) * channels);
  REQUIRE(buffer.read_frames(temp.data(), 4) == 4);  // consume frames 0..3

  REQUIRE(buffer.write_frames(second.data(), 6) == 6);

  std::vector<float> output(static_cast<size_t>(8) * channels);
  REQUIRE(buffer.read_frames(output.data(), 8) == 8);

  auto expected = MakePattern(8, 4);  // frames 4..11
  REQUIRE(output == expected);
}

// Confirms underrun/overrun counters when reads/writes cannot be satisfied.
TEST_CASE("AudioRingBuffer underrun/overrun counters increment") {
  constexpr uint32_t channels = 2;
  AudioRingBuffer buffer(4, channels);
  std::vector<float> temp(static_cast<size_t>(4) * channels, 0.0f);

  REQUIRE(buffer.read_frames(temp.data(), 1) == 0);
  REQUIRE(buffer.underrun_count() == 1);

  auto input = MakePattern(4, 0);
  REQUIRE(buffer.write_frames(input.data(), 4) == 4);

  REQUIRE(buffer.write_frames(input.data(), 1) == 0);
  REQUIRE(buffer.overrun_count() == 1);
}

// Validates that short writes return the available space without corrupting order.
TEST_CASE("AudioRingBuffer write allows partial progress") {
  constexpr uint32_t channels = 2;
  AudioRingBuffer buffer(4, channels);
  auto input = MakePattern(3, 0);
  auto extra = MakePattern(2, 100);

  REQUIRE(buffer.write_frames(input.data(), 3) == 3);
  const uint64_t overrun_before = buffer.overrun_count();

  REQUIRE(buffer.write_frames(extra.data(), 2) == 1);
  REQUIRE(buffer.overrun_count() == overrun_before + 1);

  std::vector<float> output(static_cast<size_t>(4) * channels, 0.0f);
  REQUIRE(buffer.read_frames(output.data(), 4) == 4);

  auto expected = MakePattern(4, 0);
  for (uint32_t ch = 0; ch < channels; ++ch) {
    expected[static_cast<size_t>(3) * channels + ch] =
        extra[ch];
  }
  REQUIRE(output == expected);
}

// Validates that short reads return the available data without touching the rest.
TEST_CASE("AudioRingBuffer read allows partial progress") {
  constexpr uint32_t channels = 2;
  AudioRingBuffer buffer(4, channels);
  auto input = MakePattern(2, 0);
  std::vector<float> output(static_cast<size_t>(3) * channels, -1.0f);

  REQUIRE(buffer.write_frames(input.data(), 2) == 2);
  const uint64_t underrun_before = buffer.underrun_count();

  REQUIRE(buffer.read_frames(output.data(), 3) == 2);
  REQUIRE(buffer.underrun_count() == underrun_before + 1);

  std::vector<float> expected_prefix(input.begin(), input.end());
  for (size_t i = 0; i < expected_prefix.size(); ++i) {
    REQUIRE(output[i] == expected_prefix[i]);
  }
  for (size_t i = expected_prefix.size(); i < output.size(); ++i) {
    REQUIRE(output[i] == -1.0f);
  }
}

// Confirms interleaving order and per-channel stride across wrap-around.
TEST_CASE("AudioRingBuffer interleaving preserved across wrap-around") {
  constexpr uint32_t channels = 2;
  AudioRingBuffer buffer(5, channels);

  auto first = MakePattern(4, 0);   // frames 0..3
  auto second = MakePattern(4, 4);  // frames 4..7

  REQUIRE(buffer.write_frames(first.data(), 4) == 4);

  std::vector<float> temp(static_cast<size_t>(3) * channels);
  REQUIRE(buffer.read_frames(temp.data(), 3) == 3);  // consume frames 0..2

  REQUIRE(buffer.write_frames(second.data(), 4) == 4);

  std::vector<float> output(static_cast<size_t>(5) * channels);
  REQUIRE(buffer.read_frames(output.data(), 5) == 5);

  auto expected = MakePattern(5, 3);  // frames 3..7
  REQUIRE(output == expected);
}

// Validates boundary behavior when hitting exact capacity, including after wrap-around.
TEST_CASE("AudioRingBuffer exact-capacity boundaries") {
  const uint32_t channels = 2;
  const uint32_t capacity = 4;

  SECTION("exact fill and drain") {
    AudioRingBuffer buffer(capacity, channels);
    auto input = MakePattern(capacity, 0);
    std::vector<float> output(input.size(), 0.0f);

    REQUIRE(buffer.write_frames(input.data(), capacity) == capacity);
    REQUIRE(buffer.write_frames(input.data(), 1) == 0);
    REQUIRE(buffer.overrun_count() == 1);

    REQUIRE(buffer.read_frames(output.data(), capacity) == capacity);
    REQUIRE(output == input);

    REQUIRE(buffer.read_frames(output.data(), 1) == 0);
    REQUIRE(buffer.underrun_count() == 1);
  }

  SECTION("exact capacity after wrap-around") {
    AudioRingBuffer buffer(capacity, channels);
    auto input = MakePattern(capacity, 0);
    auto refill = MakePattern(2, capacity);

    std::vector<float> temp(static_cast<size_t>(2) * channels);
    std::vector<float> output(static_cast<size_t>(capacity) * channels);

    REQUIRE(buffer.write_frames(input.data(), capacity) == capacity);
    REQUIRE(buffer.read_frames(temp.data(), 2) == 2);
    REQUIRE(buffer.write_frames(refill.data(), 2) == 2);

    REQUIRE(buffer.write_frames(input.data(), 1) == 0);
    REQUIRE(buffer.overrun_count() == 1);

    REQUIRE(buffer.read_frames(output.data(), capacity) == capacity);
    auto expected = MakePattern(capacity, 2);  // frames 2..5
    REQUIRE(output == expected);

    REQUIRE(buffer.read_frames(output.data(), 1) == 0);
    REQUIRE(buffer.underrun_count() == 1);
  }
}

// Exercises SPSC atomics under contention with a bounded counter pattern.
TEST_CASE("AudioRingBuffer SPSC stress preserves order without overruns") {
  constexpr uint32_t channels = 2;
  constexpr uint32_t capacity_frames = 2048;
  constexpr uint32_t max_counter = 1u << 20;  // < 2^24, exact in float32
  const std::vector<uint32_t> chunk_sizes = {1, 7, 64, 127};

  struct Failure {
    std::atomic<bool> failed{false};
    std::atomic<uint32_t> expected{0};
    std::atomic<uint32_t> got{0};
    std::atomic<uint32_t> code{0};
  };

  enum FailureCode : uint32_t {
    kWriteShort = 1,
    kReadShort = 2,
    kNaN = 3,
    kMismatch = 4
  };

  for (uint32_t chunk_frames : chunk_sizes) {
    for (int repeat = 0; repeat < 3; ++repeat) {
      AudioRingBuffer buffer(capacity_frames, channels);
      Failure failure;
      std::atomic<bool> producer_done{false};

      std::thread producer([&]() {
        std::vector<float> chunk(static_cast<size_t>(chunk_frames) * channels);
        uint32_t counter = 0;

        while (counter < max_counter) {
          const uint32_t remaining = max_counter - counter;
          const uint32_t frames_to_write =
              remaining < chunk_frames ? remaining : chunk_frames;

          if (buffer.available_to_write_frames() < frames_to_write) {
            std::this_thread::yield();
            continue;
          }

          for (uint32_t frame = 0; frame < frames_to_write; ++frame) {
            const float value = static_cast<float>(counter + frame);
            const size_t base = static_cast<size_t>(frame) * channels;
            for (uint32_t ch = 0; ch < channels; ++ch) {
              chunk[base + ch] =
                  value + static_cast<float>(ch * kChannelStride);
            }
          }

          const uint32_t written =
              buffer.write_frames(chunk.data(), frames_to_write);
          if (written != frames_to_write) {
            if (!failure.failed.exchange(true)) {
              failure.code.store(kWriteShort);
              failure.expected.store(frames_to_write);
              failure.got.store(written);
            }
            break;
          }

          counter += frames_to_write;
        }

        producer_done.store(true, std::memory_order_release);
      });

      std::thread consumer([&]() {
        std::vector<float> chunk(static_cast<size_t>(chunk_frames) * channels);
        uint32_t expected = 0;

        while (true) {
          const uint32_t available = buffer.available_to_read_frames();
          if (available == 0) {
            if (producer_done.load(std::memory_order_acquire)) {
              break;
            }
            std::this_thread::yield();
            continue;
          }

          const uint32_t frames_to_read =
              available < chunk_frames ? available : chunk_frames;
          const uint32_t frames_read =
              buffer.read_frames(chunk.data(), frames_to_read);
          if (frames_read != frames_to_read) {
            if (!failure.failed.exchange(true)) {
              failure.code.store(kReadShort);
              failure.expected.store(frames_to_read);
              failure.got.store(frames_read);
            }
            return;
          }

          for (uint32_t frame = 0; frame < frames_read; ++frame) {
            const float expected_base = static_cast<float>(expected + frame);
            const size_t base = static_cast<size_t>(frame) * channels;
            for (uint32_t ch = 0; ch < channels; ++ch) {
              const float sample = chunk[base + ch];
              if (!std::isfinite(sample)) {
                if (!failure.failed.exchange(true)) {
                  failure.code.store(kNaN);
                  failure.expected.store(expected + frame);
                  failure.got.store(0xFFFFFFFFu);
                }
                return;
              }
              const float expected_sample =
                  expected_base + static_cast<float>(ch * kChannelStride);
              if (sample != expected_sample) {
                if (!failure.failed.exchange(true)) {
                  failure.code.store(kMismatch);
                  failure.expected.store(expected + frame);
                  failure.got.store(static_cast<uint32_t>(sample));
                }
                return;
              }
            }
          }

          expected += frames_read;
        }
      });

      producer.join();
      consumer.join();

      if (failure.failed.load()) {
        INFO("chunk=" << chunk_frames << " repeat=" << repeat);
        INFO("code=" << failure.code.load()
                     << " expected=" << failure.expected.load()
                     << " got=" << failure.got.load());
      }
      REQUIRE_FALSE(failure.failed.load());
      REQUIRE(buffer.overrun_count() == 0);
      REQUIRE(buffer.underrun_count() == 0);
    }
  }
}
