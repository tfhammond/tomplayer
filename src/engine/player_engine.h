#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <thread>
#include <variant>

namespace tomplayer::engine {

// Summary: Playback state machine owned exclusively by the engine thread.
// Preconditions: PlayerEngine must outlive any threads that call its public API.
// Postconditions: State transitions are applied only on the engine thread.
// Errors: None; commands are queued and processed asynchronously.
class PlayerEngine {
public:
  // Summary: Discrete playback states owned by the engine thread.
  // Preconditions: None.
  // Postconditions: Used for read-only observation from other threads.
  // Errors: None.
  enum class PlayerState {
    Idle,
    Stopped,
    Starting,
    Playing,
    Paused,
    Seeking,
    Stopping,
    Finished,
    Error
  };

  PlayerEngine();
  ~PlayerEngine();

  PlayerEngine(const PlayerEngine&) = delete;
  PlayerEngine& operator=(const PlayerEngine&) = delete;

  // Summary: Enqueue a Play command.
  // Preconditions: None.
  // Postconditions: Command is queued for the engine thread.
  // Errors: None.
  void play();

  // Summary: Enqueue a Pause command.
  // Preconditions: None.
  // Postconditions: Command is queued for the engine thread.
  // Errors: None.
  void pause();

  // Summary: Enqueue a Resume command.
  // Preconditions: None.
  // Postconditions: Command is queued for the engine thread.
  // Errors: None.
  void resume();

  // Summary: Enqueue a Stop command.
  // Preconditions: None.
  // Postconditions: Command is queued for the engine thread.
  // Errors: None.
  void stop();

  // Summary: Enqueue a Seek command (seconds).
  // Preconditions: seconds is finite.
  // Postconditions: Command is queued for the engine thread.
  // Errors: None.
  void seek_seconds(double seconds);

  // Summary: Enqueue a Replay command.
  // Preconditions: None.
  // Postconditions: Command is queued for the engine thread.
  // Errors: None.
  void replay();

  // Summary: Enqueue a Quit command to stop the engine thread.
  // Preconditions: None.
  // Postconditions: Engine thread will exit; destructor will join.
  // Errors: None.
  void quit();

  // Summary: Return the last committed playback state.
  // Preconditions: None.
  // Postconditions: Does not mutate state.
  // Errors: None.
  PlayerState get_state() const;

private:
  struct PlayCommand {};
  struct PauseCommand {};
  struct ResumeCommand {};
  struct StopCommand {};
  struct SeekCommand {
    double seconds = 0.0;
  };
  struct ReplayCommand {};
  struct QuitCommand {};

  using Command = std::variant<PlayCommand,
                               PauseCommand,
                               ResumeCommand,
                               StopCommand,
                               SeekCommand,
                               ReplayCommand,
                               QuitCommand>;

  void Enqueue(Command command);
  void EngineLoop();
  void HandleCommand(const Command& command);

  std::atomic<PlayerState> state_{PlayerState::Idle};
  std::atomic<bool> running_{true};

  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::deque<Command> queue_;

  std::thread engine_thread_;
};

}  // namespace tomplayer::engine
