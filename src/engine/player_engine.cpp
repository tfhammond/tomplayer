#include "engine/player_engine.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace tomplayer::engine {

PlayerEngine::PlayerEngine() {
  engine_thread_ = std::thread(&PlayerEngine::EngineLoop, this);
}

PlayerEngine::~PlayerEngine() {
  quit();
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
  snapshot.position_seconds = position_seconds_.load(std::memory_order_acquire);
  snapshot.duration_seconds = duration_seconds_.load(std::memory_order_acquire);
  snapshot.buffered_seconds = buffered_seconds_.load(std::memory_order_acquire);
  snapshot.underrun_count = underrun_count_.load(std::memory_order_acquire);
  snapshot.dropped_frames = dropped_frames_.load(std::memory_order_acquire);
  snapshot.decode_epoch = decode_control_.epoch.load(std::memory_order_acquire);
  snapshot.decode_mode = decode_control_.mode.load(std::memory_order_acquire);
  snapshot.seek_target_frame =
      decode_control_.target_frame.load(std::memory_order_acquire);
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
  queue_cv_.notify_one();
}

void PlayerEngine::EngineLoop() {
  // The engine thread is the sole owner of state transitions.
  auto last_tick = std::chrono::steady_clock::now();
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
      }
    }

    if (has_command) {
      if (std::holds_alternative<QuitCommand>(command)) {
        set_decode_mode(DecodeMode::Quit);
        bump_epoch();
        break;
      }
      HandleCommand(command);
    }

    const auto now = std::chrono::steady_clock::now();
    const std::chrono::duration<double> elapsed = now - last_tick;
    if (state_.load(std::memory_order_acquire) == PlayerState::Playing) {
      const double current = position_seconds_.load(std::memory_order_acquire);
      position_seconds_.store(current + elapsed.count(), std::memory_order_release);
    }
    last_tick = now;
  }
}

void PlayerEngine::HandleCommand(const Command& command) {
  // Placeholder transitions for v1 skeleton. Actual logic is engine-owned only.
  if (std::holds_alternative<PlayCommand>(command)) {
    state_.store(PlayerState::Playing, std::memory_order_release);
    set_decode_mode(DecodeMode::Running);
    return;
  }
  if (std::holds_alternative<PauseCommand>(command)) {
    state_.store(PlayerState::Paused, std::memory_order_release);
    set_decode_mode(DecodeMode::Paused);
    return;
  }
  if (std::holds_alternative<ResumeCommand>(command)) {
    state_.store(PlayerState::Playing, std::memory_order_release);
    set_decode_mode(DecodeMode::Running);
    return;
  }
  if (std::holds_alternative<StopCommand>(command)) {
    state_.store(PlayerState::Stopped, std::memory_order_release);
    position_seconds_.store(0.0, std::memory_order_release);
    bump_epoch();
    set_decode_mode(DecodeMode::Stopped);
    set_target_frame(-1);
    return;
  }
  if (std::holds_alternative<SeekCommand>(command)) {
    constexpr int64_t placeholder_sample_rate_hz = 48000;
    state_.store(PlayerState::Seeking, std::memory_order_release);
    const auto seek = std::get<SeekCommand>(command);
    const double clamped = std::max(0.0, seek.seconds);
    position_seconds_.store(clamped, std::memory_order_release);
    const int64_t frames =
        static_cast<int64_t>(clamped * static_cast<double>(placeholder_sample_rate_hz));
    bump_epoch();
    set_target_frame(frames);
    set_decode_mode(DecodeMode::Running);
    state_.store(PlayerState::Playing, std::memory_order_release);
    return;
  }
  if (std::holds_alternative<ReplayCommand>(command)) {
    state_.store(PlayerState::Starting, std::memory_order_release);
    position_seconds_.store(0.0, std::memory_order_release);
    bump_epoch();
    set_target_frame(0);
    set_decode_mode(DecodeMode::Running);
    state_.store(PlayerState::Playing, std::memory_order_release);
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

}  // namespace tomplayer::engine
