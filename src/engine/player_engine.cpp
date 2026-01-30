#include "engine/player_engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <objbase.h>
#include <utility>
#include <vector>

namespace tomplayer::engine {

PlayerEngine::PlayerEngine() {
  ring_buffer_ = std::make_unique<AudioRingBuffer>(kDefaultSampleRateHz * 2,
                                                   kDefaultChannels);
  output_ = std::make_unique<tomplayer::wasapi::WasapiOutput>();
  // Start background threads immediately; they exit cleanly on Quit.
  engine_thread_ = std::thread(&PlayerEngine::EngineLoop, this);
  decode_thread_ = std::thread(&PlayerEngine::DecodeLoop, this);
}

PlayerEngine::~PlayerEngine() {
  quit();
  if (decode_thread_.joinable()) {
    decode_thread_.join();
  }
  if (engine_thread_.joinable()) {
    engine_thread_.join();
  }
}

void PlayerEngine::play() {
  Enqueue(PlayCommand{});
}

void PlayerEngine::pause() {
  Enqueue(PauseCommand{});
}

void PlayerEngine::resume() {
  Enqueue(ResumeCommand{});
}

void PlayerEngine::stop() {
  Enqueue(StopCommand{});
}

void PlayerEngine::seek_seconds(double seconds) {
  Enqueue(SeekCommand{seconds});
}

void PlayerEngine::replay() {
  Enqueue(ReplayCommand{});
}

void PlayerEngine::quit() {
  const bool already_stopped = !running_.exchange(false);
  if (already_stopped) {
    return;
  }
  Enqueue(QuitCommand{});
}

PlayerEngine::PlayerState PlayerEngine::get_state() const {
  return state_.load(std::memory_order_acquire);
}

PlayerEngine::Status PlayerEngine::get_status() const {
  Status snapshot;
  snapshot.state = state_.load(std::memory_order_acquire);
  snapshot.duration_seconds = duration_seconds_.load(std::memory_order_acquire);
  snapshot.buffered_seconds = buffered_seconds_.load(std::memory_order_acquire);
  snapshot.underrun_wake_count = 0;
  snapshot.underrun_frames_total = 0;
  if (output_) {
    snapshot.underrun_wake_count = output_->underrun_wake_count();
    snapshot.underrun_frames_total = output_->underrun_frame_count();
  }
  snapshot.dropped_frames = dropped_frames_.load(std::memory_order_acquire);
  snapshot.decode_epoch = decode_control_.epoch.load(std::memory_order_acquire);
  snapshot.decode_mode = decode_control_.mode.load(std::memory_order_acquire);
  snapshot.seek_target_frame =
      decode_control_.target_frame.load(std::memory_order_acquire);
  snapshot.decoded_frame_cursor =
      decoded_frame_cursor_.load(std::memory_order_acquire);
  snapshot.produced_frames_total =
      produced_frames_total_.load(std::memory_order_acquire);
  const uint32_t sample_rate = sample_rate_hz_.load(std::memory_order_acquire);
  const int64_t offset_frames =
      render_frame_offset_.load(std::memory_order_acquire);
  uint64_t rendered_frames = 0;
  if (output_) {
    rendered_frames = output_->rendered_frames_total();
  }
  if (sample_rate > 0) {
    snapshot.position_seconds =
        static_cast<double>(rendered_frames + offset_frames) /
        static_cast<double>(sample_rate);
  } else {
    snapshot.position_seconds = 0.0;
  }
  {
    std::lock_guard<std::mutex> lock(last_error_mutex_);
    snapshot.last_error = last_error_;
  }
  return snapshot;
}

void PlayerEngine::Enqueue(Command command) {
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    queue_.push_back(std::move(command));
  }
  queue_has_pending_.store(true, std::memory_order_release);
  queue_cv_.notify_one();
}

void PlayerEngine::EngineLoop() {
  // The engine thread is the sole owner of state transitions.
  const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool com_should_uninit = SUCCEEDED(com_hr);
  while (true) {
    Command command;
    bool has_command = false;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_cv_.wait_for(lock, std::chrono::milliseconds(50), [this] {
        return !queue_.empty();
      });
      if (!queue_.empty()) {
        command = std::move(queue_.front());
        queue_.pop_front();
        has_command = true;
        if (queue_.empty()) {
          queue_has_pending_.store(false, std::memory_order_release);
        }
      }
    }

    if (has_command) {
      if (std::holds_alternative<QuitCommand>(command)) {
        set_decode_mode(DecodeMode::Quit);
        bump_epoch();
        if (output_) {
          output_->stop();
          output_->shutdown();
          output_initialized_ = false;
        }
        break;
      }
      HandleCommand(command);
    }

    const uint32_t sample_rate = sample_rate_hz_.load(std::memory_order_acquire);
    const double buffered_seconds =
        ring_buffer_ && sample_rate > 0
            ? static_cast<double>(ring_buffer_->available_to_read_frames()) /
                  static_cast<double>(sample_rate)
            : 0.0;
    buffered_seconds_.store(buffered_seconds, std::memory_order_release);
  }

  if (com_should_uninit) {
    CoUninitialize();
  }
}

void PlayerEngine::HandleCommand(const Command& command) {
  // Placeholder transitions for v1 skeleton. Actual logic is engine-owned only.
  if (std::holds_alternative<PlayCommand>(command)) {
    state_.store(PlayerState::Starting, std::memory_order_release);
    //set_decode_mode(DecodeMode::Running);
    const uint32_t threshold_frames =
        static_cast<uint32_t>(sample_rate_hz_.load(std::memory_order_acquire) / 5);
    if (StartPlaybackWithPriming(threshold_frames, false)) {
      state_.store(PlayerState::Playing, std::memory_order_release);
    } else {
      state_.store(PlayerState::Error, std::memory_order_release);
    }
    return;
  }
  if (std::holds_alternative<PauseCommand>(command)) {
    CommitPaused();
    return;
  }
  if (std::holds_alternative<ResumeCommand>(command)) {
    state_.store(PlayerState::Starting, std::memory_order_release);
    //set_decode_mode(DecodeMode::Running);
    const uint32_t threshold_frames =
        static_cast<uint32_t>(sample_rate_hz_.load(std::memory_order_acquire) / 20);
    if (StartPlaybackWithPriming(threshold_frames, true)) {
      state_.store(PlayerState::Playing, std::memory_order_release);
    } else {
      state_.store(PlayerState::Error, std::memory_order_release);
    }
    return;
  }
  if (std::holds_alternative<StopCommand>(command)) {
    StopOutputAndResetRenderedFrames();
    state_.store(PlayerState::Stopped, std::memory_order_release);
    render_frame_offset_.store(0, std::memory_order_release);
    StopDecodeAndWaitIdle();
    ResetBufferingState();
    BeginNewDecodeEpochAndSetTarget(std::nullopt);
    return;
  }
  if (std::holds_alternative<SeekCommand>(command)) {
    const PlayerState prior_state = state_.load(std::memory_order_acquire);
    state_.store(PlayerState::Seeking, std::memory_order_release);
    const auto seek = std::get<SeekCommand>(command);
    const double clamped = std::max(0.0, seek.seconds);
    const int64_t frames =
        static_cast<int64_t>(std::llround(clamped *
                                          static_cast<double>(
                                              sample_rate_hz_.load(std::memory_order_acquire))));
    const DecodeMode desired_mode =
        prior_state == PlayerState::Paused ? DecodeMode::Paused : DecodeMode::Running;
    StopOutputAndResetRenderedFrames();
    render_frame_offset_.store(frames, std::memory_order_release);
    PauseDecodeAndWaitIdle();
    ResetBufferingState();
    BeginNewDecodeEpochAndSetTarget(frames);
    if (desired_mode == DecodeMode::Paused) {
      CommitPaused();
    } else {
      //set_decode_mode(DecodeMode::Running);
      state_.store(PlayerState::Starting, std::memory_order_release);
      const uint32_t threshold_frames =
          static_cast<uint32_t>(sample_rate_hz_.load(std::memory_order_acquire) / 5);
      if (StartPlaybackWithPriming(threshold_frames, false)) {
        state_.store(PlayerState::Playing, std::memory_order_release);
      } else {
        state_.store(PlayerState::Error, std::memory_order_release);
      }
    }
    return;
  }
  if (std::holds_alternative<ReplayCommand>(command)) {
    StopOutputAndResetRenderedFrames();
    state_.store(PlayerState::Starting, std::memory_order_release);
    render_frame_offset_.store(0, std::memory_order_release);
    ResetBufferingState();
    BeginNewDecodeEpochAndSetTarget(0);
    //set_decode_mode(DecodeMode::Running);
    const uint32_t threshold_frames =
        static_cast<uint32_t>(sample_rate_hz_.load(std::memory_order_acquire) / 5);
    if (StartPlaybackWithPriming(threshold_frames, false)) {
      state_.store(PlayerState::Playing, std::memory_order_release);
    } else {
      state_.store(PlayerState::Error, std::memory_order_release);
    }
    return;
  }
}

void PlayerEngine::bump_epoch() {
  decode_control_.epoch.fetch_add(1, std::memory_order_acq_rel);
}

void PlayerEngine::set_decode_mode(DecodeMode mode) {
  decode_control_.mode.store(mode, std::memory_order_release);
}

void PlayerEngine::set_target_frame(int64_t frame) {
  decode_control_.target_frame.store(frame, std::memory_order_release);
}

void PlayerEngine::StopOutputAndResetRenderedFrames() {
  if (!output_) {
    return;
  }
  // Keep the render clock consistent with explicit stop/seek/replay transitions.
  output_->stop();
  output_->reset_rendered_frames();
}

void PlayerEngine::PauseDecodeAndWaitIdle() {
  set_decode_mode(DecodeMode::Paused);
  WaitForDecodeIdle();
}

void PlayerEngine::StopDecodeAndWaitIdle() {
  set_decode_mode(DecodeMode::Stopped);
  WaitForDecodeIdle();
}

void PlayerEngine::ResetBufferingState() {
  DrainRingBuffer();
  if (ring_buffer_) {
    ring_buffer_->reset();
  }
  buffered_seconds_.store(0.0, std::memory_order_release);
}

void PlayerEngine::BeginNewDecodeEpochAndSetTarget(std::optional<int64_t> target_frame) {
  bump_epoch();
  set_target_frame(target_frame.value_or(-1));
}

void PlayerEngine::CommitPaused() {
  // Paused implies the output is stopped; decode mode is paused but buffers are retained.
  if (output_) {
    output_->stop();
  }
  state_.store(PlayerState::Paused, std::memory_order_release);
  set_decode_mode(DecodeMode::Paused);
}

bool PlayerEngine::StartPlaybackWithPriming(uint32_t threshold_frames, bool allow_empty) {
  if (!EnsureOutputInitialized()) {
    return false;
  }
  set_decode_mode(DecodeMode::Running);
  return PrimeAndStart(threshold_frames, allow_empty);
}

void PlayerEngine::DecodeLoop() {
  constexpr int64_t chunk_frames = 1024;
  uint64_t local_epoch = decode_control_.epoch.load(std::memory_order_acquire);
  int64_t local_cursor_frame = 0;
  decoded_frame_cursor_.store(local_cursor_frame, std::memory_order_release);
  uint32_t local_channels = channels_.load(std::memory_order_acquire);
  std::vector<float> silence(static_cast<size_t>(chunk_frames) * local_channels, 0.0f);

  while (true) {
    const DecodeMode mode = decode_control_.mode.load(std::memory_order_acquire);
    if (mode == DecodeMode::Quit) {
      SetDecodeIdle(true);
      break;
    }

    const uint64_t current_epoch =
        decode_control_.epoch.load(std::memory_order_acquire);
    if (current_epoch != local_epoch) {
      local_epoch = current_epoch;
      const int64_t target =
          decode_control_.target_frame.load(std::memory_order_acquire);
      local_cursor_frame = target >= 0 ? target : 0;
      decoded_frame_cursor_.store(local_cursor_frame, std::memory_order_release);
    }

    if (mode == DecodeMode::Stopped || mode == DecodeMode::Paused) {
      SetDecodeIdle(true);
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }

    if (mode == DecodeMode::Running) {
      SetDecodeIdle(false);
      if (!ring_buffer_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }
      const uint32_t current_rate = sample_rate_hz_.load(std::memory_order_acquire);
      if (current_rate == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }
      const uint32_t current_channels = channels_.load(std::memory_order_acquire);
      if (current_channels != local_channels) {
        local_channels = current_channels;
        silence.assign(static_cast<size_t>(chunk_frames) * local_channels, 0.0f);
      }
      const uint32_t written = ring_buffer_->write_frames(
          silence.data(),
          static_cast<uint32_t>(chunk_frames));
      if (written < static_cast<uint32_t>(chunk_frames)) {
        dropped_frames_.fetch_add(static_cast<uint64_t>(chunk_frames - written),
                                  std::memory_order_acq_rel);
      }
      if (written == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }

      {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
      }
      buffer_cv_.notify_all();

      local_cursor_frame += written;
      decoded_frame_cursor_.store(local_cursor_frame, std::memory_order_release);
      produced_frames_total_.fetch_add(static_cast<uint64_t>(written),
                                       std::memory_order_acq_rel);

      const auto written_duration =
          std::chrono::duration<double>(static_cast<double>(written) /
                                        static_cast<double>(current_rate));
      std::this_thread::sleep_for(written_duration);
    }
  }
}

void PlayerEngine::WaitForDecodeIdle() {
  if (decode_idle_.load(std::memory_order_acquire)) {
    return;
  }
  std::unique_lock<std::mutex> lock(decode_idle_mutex_);
  decode_idle_cv_.wait(lock, [this] {
    return decode_idle_.load(std::memory_order_acquire);
  });
}

void PlayerEngine::DrainRingBuffer() {
  constexpr uint32_t kDrainChunkFrames = 1024;
  if (!ring_buffer_) {
    return;
  }
  const uint32_t channels = channels_.load(std::memory_order_acquire);
  std::vector<float> scratch(static_cast<size_t>(kDrainChunkFrames) * channels, 0.0f);
  while (true) {
    const uint32_t available = ring_buffer_->available_to_read_frames();
    if (available == 0) {
      break;
    }
    const uint32_t to_read =
        available < kDrainChunkFrames ? available : kDrainChunkFrames;
    ring_buffer_->read_frames(scratch.data(), to_read);
  }
}

void PlayerEngine::SetDecodeIdle(bool idle) {
  const bool was_idle = decode_idle_.exchange(idle, std::memory_order_release);
  if (idle && !was_idle) {
    decode_idle_cv_.notify_all();
  }
}

void PlayerEngine::SetLastError(const char* message) {
  std::lock_guard<std::mutex> lock(last_error_mutex_);
  last_error_ = message ? message : "";
}

bool PlayerEngine::EnsureOutputInitialized() {
  if (output_initialized_) {
    return true;
  }
  if (!output_) {
    output_ = std::make_unique<tomplayer::wasapi::WasapiOutput>();
  }
  if (!output_->init_default_device()) {
    SetLastError("Failed to initialize WASAPI output.");
    return false;
  }

  const uint32_t device_rate = output_->sample_rate();
  const uint32_t device_channels = output_->channels();
  if (device_rate == 0 || device_channels == 0) {
    SetLastError("Invalid WASAPI mix format.");
    return false;
  }

  sample_rate_hz_.store(device_rate, std::memory_order_release);
  channels_.store(device_channels, std::memory_order_release);

  set_decode_mode(DecodeMode::Paused);
  WaitForDecodeIdle();
  DrainRingBuffer();
  ring_buffer_->reset();
  //ring_buffer_ = std::make_unique<AudioRingBuffer>(device_rate * 2, device_channels);
  output_->set_ring_buffer(ring_buffer_.get());
  buffered_seconds_.store(0.0, std::memory_order_release);
  render_frame_offset_.store(0, std::memory_order_release);
  output_->reset_rendered_frames();

  output_initialized_ = true;
  return true;
}

bool PlayerEngine::PrimeAndStart(uint32_t threshold_frames, bool allow_empty) {
  if (!output_) {
    return false;
  }
  if (!ring_buffer_) {
    return false;
  }

  const uint32_t target = threshold_frames;


  while (ring_buffer_->available_to_read_frames() < target) {
    if (allow_empty && ring_buffer_->available_to_read_frames() == 0) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  if (!output_->start()) {
    SetLastError("Failed to start WASAPI output.");
    return false;
  }

  return true;
}

bool PlayerEngine::BeginPriming(uint32_t target, bool allow_empty) {
  
  if (!EnsureOutputInitialized()) {
    state_.store(PlayerState::Error);
    priming_active_ = false;
    return false;
  }

  set_decode_mode(DecodeMode::Running);
  priming_active_ = true;
  priming_target_frames_ = target;
  priming_allow_empty_ = allow_empty;
  return true;
}

void PlayerEngine::AdvancePriming(){
  
  if (!priming_active_ || !ring_buffer_ || !output_) {
    return ;
  }

  const uint32_t available = ring_buffer_->available_to_read_frames();
  if (priming_allow_empty_){
  }
  else if (available < priming_target_frames_) {
    return;
  }

  if (!output_->start()) {
    SetLastError("Failed to start WASAPI output.");
    state_.store(PlayerState::Error);
  } else {
    state_.store(PlayerState::Playing);
  }
  priming_active_ = false;


}



}  // namespace tomplayer::engine
