#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <variant>

#include "buffer/audio_ring_buffer.h"

namespace tomplayer::engine {

// Summary: Playback state machine owned exclusively by the engine thread.
// Preconditions: PlayerEngine must outlive any threads that call its public API.
// Postconditions: State transitions are applied only on the engine thread.
// Errors: None; commands are queued and processed asynchronously.
class PlayerEngine {
public:
  // Summary: Decode control modes issued by the engine thread.
  // Preconditions: None.
  // Postconditions: Used for decode-thread coordination.
  // Errors: None.
  enum class DecodeMode { Stopped, Running, Paused, Quit };

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

  // Summary: Snapshot of playback state for UI consumers.
  // Preconditions: None.
  // Postconditions: Returned values are a point-in-time copy.
  // Errors: None.
  struct Status {
    PlayerState state = PlayerState::Idle;
    double position_seconds = 0.0;
    double duration_seconds = 0.0;
    double buffered_seconds = 0.0;
    uint64_t underrun_count = 0;
    uint64_t dropped_frames = 0;
    uint64_t decode_epoch = 0;
    DecodeMode decode_mode = DecodeMode::Stopped;
    int64_t seek_target_frame = -1;
    int64_t decoded_frame_cursor = 0;
    uint64_t produced_frames_total = 0;
    std::string last_error;
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

  // Summary: Return a snapshot of playback status suitable for UI display.
  // Preconditions: None.
  // Postconditions: Does not mutate state; returns a copy.
  // Errors: None.
  Status get_status() const;

private:
  // Placeholder device format for the stub pipeline.
  static constexpr uint32_t kSampleRateHz = 48000;
  static constexpr uint32_t kChannels = 2;
  static constexpr uint32_t kCapacityFrames = kSampleRateHz * 2;

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
  void bump_epoch();
  void set_decode_mode(DecodeMode mode);
  void set_target_frame(int64_t frame);
  void DecodeLoop();
  void WaitForDecodeIdle();
  void DrainRingBuffer();
  void SetDecodeIdle(bool idle);

  // Decode control is owned by the engine thread; atomics provide snapshots to readers.
  // Epoch is a generation counter: any change that invalidates in-flight decode work
  // increments it so a future decode thread can restart work safely.
  struct DecodeControl {
    std::atomic<uint64_t> epoch{0};
    std::atomic<DecodeMode> mode{DecodeMode::Stopped};
    
    // Unit: PCM frames (one time step across all channels). -1 means no target.
    std::atomic<int64_t> target_frame{-1};
  };

  std::atomic<PlayerState> state_{PlayerState::Idle};
  std::atomic<double> position_seconds_{0.0};
  std::atomic<double> duration_seconds_{0.0};
  std::atomic<double> buffered_seconds_{0.0};
  std::atomic<uint64_t> underrun_count_{0};
  std::atomic<uint64_t> dropped_frames_{0};
  std::atomic<bool> running_{true};

  // Protected by last_error_mutex_ because std::string is not atomic.
  // Mutable to allow locking in const accessors.
  mutable std::mutex last_error_mutex_;
  std::string last_error_;
  DecodeControl decode_control_{};
  std::atomic<int64_t> decoded_frame_cursor_{0};
  std::atomic<uint64_t> produced_frames_total_{0};
  // Frame = one time-step across all channels (interleaved float32 layout).
  AudioRingBuffer ring_buffer_{kCapacityFrames, kChannels};

  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::deque<Command> queue_;

  std::atomic<bool> decode_idle_{true};
  std::mutex decode_idle_mutex_;
  std::condition_variable decode_idle_cv_;

  std::thread engine_thread_;
  std::thread decode_thread_;
};

}  // namespace tomplayer::engine
