#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

// AudioRingBuffer
// - Single-producer/single-consumer only; multiple producers/consumers are misuse.
// - Frame-based semantics (frame = one sample per channel at a single time step).
// - Interleaved PCM float32 storage (e.g., stereo is LRLR...).
// - Real-time constraints: no allocations, locks, or blocking in read/write.
// - Invariant: write_pos_frames >= read_pos_frames and (write_pos_frames - read_pos_frames) <= capacity.
class AudioRingBuffer {
public:
  // Summary: Construct a fixed-capacity ring buffer sized in frames.
  // Preconditions: capacity_frames > 0; channels > 0.
  // Postconditions: storage is allocated for capacity_frames * channels.
  // Errors: none (construction failure throws on allocation).
  AudioRingBuffer(uint32_t capacity_frames, uint32_t channels);

  // Summary: Return how many frames can be written without overwriting.
  // Preconditions: none.
  // Postconditions: does not modify state.
  // Errors: none.
  uint32_t available_to_write_frames() const;

  // Summary: Write up to frames_requested from interleaved source.
  // Preconditions: src_interleaved points to frames_requested * channels samples.
  // Postconditions: advances write position by frames_written.
  // Errors: may drop data; returns frames actually written.
  uint32_t write_frames(const float* src_interleaved, uint32_t frames_requested);

  // Summary: Return how many frames can be read without underrun.
  // Preconditions: none.
  // Postconditions: does not modify state.
  // Errors: none.
  uint32_t available_to_read_frames() const;

  // Summary: Read up to frames_requested into interleaved destination.
  // Preconditions: dst_interleaved points to frames_requested * channels samples.
  // Postconditions: advances read position by frames_read.
  // Errors: may output fewer frames; returns frames actually read.
  uint32_t read_frames(float* dst_interleaved, uint32_t frames_requested);

  // Summary: Reset read/write positions and counters.
  // Preconditions: only call when no producer/consumer threads are in read/write.
  // Postconditions: positions and counters are cleared.
  // Errors: none.
  void reset();

  // Summary: Number of read requests not fully satisfied (partials and zeros).
  // Preconditions: none.
  // Postconditions: does not modify state.
  // Errors: none.
  uint64_t underrun_count() const;

  // Summary: Number of write requests not fully satisfied (partials and zeros).
  // Preconditions: none.
  // Postconditions: does not modify state.
  // Errors: none.
  uint64_t overrun_count() const;

  // Summary: Count of invariant violations (debug asserts; release fail-soft clamp).
  // Preconditions: none.
  // Postconditions: does not modify state.
  // Errors: non-zero indicates misuse or concurrent reset.
  uint64_t invariant_violation_count() const;

private:
  uint32_t available_to_read_frames_impl(uint64_t write_pos_frames,
                                         uint64_t read_pos_frames) const;

  uint32_t capacity_frames_{0};
  uint32_t channels_{0};
  std::vector<float> storage_;

  std::atomic<uint64_t> write_pos_frames_{0};
  std::atomic<uint64_t> read_pos_frames_{0};
  std::atomic<uint64_t> underrun_count_{0};
  std::atomic<uint64_t> overrun_count_{0};
  // Mutable so const diagnostic reads can record invariant violations in release.
  mutable std::atomic<uint64_t> invariant_violation_count_{0};
};
