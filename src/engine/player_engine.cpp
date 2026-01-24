#include "engine/player_engine.h"

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

void PlayerEngine::Enqueue(Command command) {
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    queue_.push_back(std::move(command));
  }
  queue_cv_.notify_one();
}

void PlayerEngine::EngineLoop() {
  // The engine thread is the sole owner of state transitions.
  while (true) {
    Command command;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_cv_.wait(lock, [this] { return !queue_.empty(); });
      command = std::move(queue_.front());
      queue_.pop_front();
    }

    if (std::holds_alternative<QuitCommand>(command)) {
      break;
    }

    HandleCommand(command);
  }
}

void PlayerEngine::HandleCommand(const Command& command) {
  // Placeholder transitions for v1 skeleton. Actual logic is engine-owned only.
  if (std::holds_alternative<PlayCommand>(command)) {
    state_.store(PlayerState::Playing, std::memory_order_release);
    return;
  }
  if (std::holds_alternative<PauseCommand>(command)) {
    state_.store(PlayerState::Paused, std::memory_order_release);
    return;
  }
  if (std::holds_alternative<ResumeCommand>(command)) {
    state_.store(PlayerState::Playing, std::memory_order_release);
    return;
  }
  if (std::holds_alternative<StopCommand>(command)) {
    state_.store(PlayerState::Stopped, std::memory_order_release);
    return;
  }
  if (std::holds_alternative<SeekCommand>(command)) {
    state_.store(PlayerState::Seeking, std::memory_order_release);
    state_.store(PlayerState::Playing, std::memory_order_release);
    return;
  }
  if (std::holds_alternative<ReplayCommand>(command)) {
    state_.store(PlayerState::Starting, std::memory_order_release);
    state_.store(PlayerState::Playing, std::memory_order_release);
    return;
  }
}

}  // namespace tomplayer::engine
