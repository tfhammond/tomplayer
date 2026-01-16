#include "buffer/audio_ring_buffer.h"

#include <algorithm>
#include <cassert>
#include <cstring>

AudioRingBuffer::AudioRingBuffer(uint32_t capacity_frames, uint32_t channels)
    : capacity_frames_(capacity_frames),
      channels_(channels),
      storage_(static_cast<size_t>(capacity_frames) * channels) {}

uint32_t AudioRingBuffer::available_to_write_frames() const {
  const uint64_t read_pos =
      read_pos_frames_.load(std::memory_order_acquire);
  const uint64_t write_pos =
      write_pos_frames_.load(std::memory_order_relaxed);
  const uint32_t available_read =
      available_to_read_frames_impl(write_pos, read_pos);
  return capacity_frames_ - available_read;
}

uint32_t AudioRingBuffer::write_frames(const float* src_interleaved,
                                       uint32_t frames_requested) {
  if (frames_requested > 0) {
    assert(src_interleaved != nullptr);
  }
  if (!storage_.size() || capacity_frames_ == 0 || channels_ == 0) {
    return 0;
  }

  const uint64_t read_pos =
      read_pos_frames_.load(std::memory_order_acquire);
  const uint64_t write_pos =
      write_pos_frames_.load(std::memory_order_relaxed);
  const uint32_t available_read =
      available_to_read_frames_impl(write_pos, read_pos);
  const uint32_t available_write = capacity_frames_ - available_read;

  const uint32_t frames_to_write = std::min(frames_requested, available_write);
  if (frames_to_write == 0) {
    if (frames_requested != 0) {
      overrun_count_.fetch_add(1, std::memory_order_relaxed);
    }
    return 0;
  }

  const uint32_t write_index =
      static_cast<uint32_t>(write_pos % capacity_frames_);
  const uint32_t frames_until_end = capacity_frames_ - write_index;
  const uint32_t first_chunk = std::min(frames_to_write, frames_until_end);
  const uint32_t second_chunk = frames_to_write - first_chunk;

  const size_t sample_offset =
      static_cast<size_t>(write_index) * channels_;
  const size_t first_samples = static_cast<size_t>(first_chunk) * channels_;
  const size_t second_samples = static_cast<size_t>(second_chunk) * channels_;

  std::memcpy(storage_.data() + sample_offset,
              src_interleaved,
              first_samples * sizeof(float));
  if (second_chunk > 0) {
    std::memcpy(storage_.data(),
                src_interleaved + first_samples,
                second_samples * sizeof(float));
  }

  write_pos_frames_.store(write_pos + frames_to_write,
                          std::memory_order_release);

  if (frames_to_write < frames_requested) {
    overrun_count_.fetch_add(1, std::memory_order_relaxed);
  }

  return frames_to_write;
}

uint32_t AudioRingBuffer::available_to_read_frames() const {
  const uint64_t write_pos =
      write_pos_frames_.load(std::memory_order_acquire);
  const uint64_t read_pos =
      read_pos_frames_.load(std::memory_order_relaxed);
  return available_to_read_frames_impl(write_pos, read_pos);
}

uint32_t AudioRingBuffer::read_frames(float* dst_interleaved,
                                      uint32_t frames_requested) {
  if (frames_requested > 0) {
    assert(dst_interleaved != nullptr);
  }
  if (!storage_.size() || capacity_frames_ == 0 || channels_ == 0) {
    return 0;
  }

  const uint64_t write_pos =
      write_pos_frames_.load(std::memory_order_acquire);
  const uint64_t read_pos =
      read_pos_frames_.load(std::memory_order_relaxed);
  const uint32_t available_read =
      available_to_read_frames_impl(write_pos, read_pos);

  const uint32_t frames_to_read = std::min(frames_requested, available_read);
  if (frames_to_read == 0) {
    if (frames_requested != 0) {
      underrun_count_.fetch_add(1, std::memory_order_relaxed);
    }
    return 0;
  }

  const uint32_t read_index =
      static_cast<uint32_t>(read_pos % capacity_frames_);
  const uint32_t frames_until_end = capacity_frames_ - read_index;
  const uint32_t first_chunk = std::min(frames_to_read, frames_until_end);
  const uint32_t second_chunk = frames_to_read - first_chunk;

  const size_t sample_offset =
      static_cast<size_t>(read_index) * channels_;
  const size_t first_samples = static_cast<size_t>(first_chunk) * channels_;
  const size_t second_samples = static_cast<size_t>(second_chunk) * channels_;

  std::memcpy(dst_interleaved,
              storage_.data() + sample_offset,
              first_samples * sizeof(float));
  if (second_chunk > 0) {
    std::memcpy(dst_interleaved + first_samples,
                storage_.data(),
                second_samples * sizeof(float));
  }

  read_pos_frames_.store(read_pos + frames_to_read,
                         std::memory_order_release);

  if (frames_to_read < frames_requested) {
    underrun_count_.fetch_add(1, std::memory_order_relaxed);
  }

  return frames_to_read;
}

void AudioRingBuffer::reset() {
  // Safe only when no producer/consumer threads are active on the buffer.
#ifndef NDEBUG
  assert(available_to_read_frames() == 0);
  assert(available_to_write_frames() == capacity_frames_);
#endif
  write_pos_frames_.store(0, std::memory_order_relaxed);
  read_pos_frames_.store(0, std::memory_order_relaxed);
  underrun_count_.store(0, std::memory_order_relaxed);
  overrun_count_.store(0, std::memory_order_relaxed);
  invariant_violation_count_.store(0, std::memory_order_relaxed);
}

uint64_t AudioRingBuffer::underrun_count() const {
  return underrun_count_.load(std::memory_order_relaxed);
}

uint64_t AudioRingBuffer::overrun_count() const {
  return overrun_count_.load(std::memory_order_relaxed);
}

uint64_t AudioRingBuffer::invariant_violation_count() const {
  return invariant_violation_count_.load(std::memory_order_relaxed);
}

uint32_t AudioRingBuffer::available_to_read_frames_impl(uint64_t write_pos_frames,
                                                        uint64_t read_pos_frames) const {
#ifndef NDEBUG
  assert(write_pos_frames >= read_pos_frames);
  assert(write_pos_frames - read_pos_frames <= capacity_frames_);
#else
  if (write_pos_frames < read_pos_frames) {
    invariant_violation_count_.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }
#endif

  const uint64_t available = write_pos_frames - read_pos_frames;
#ifndef NDEBUG
  return static_cast<uint32_t>(available);
#else
  if (available > capacity_frames_) {
    invariant_violation_count_.fetch_add(1, std::memory_order_relaxed);
    return capacity_frames_;
  }
  return static_cast<uint32_t>(available);
#endif
}
