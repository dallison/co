//
//  coroutine.h
//  coroutines
//
//  Created by David Allison on 3/13/23.
//

#ifndef coroutine_h
#define coroutine_h

#include "bitset.h"
#include <functional>
#include <list>
#include <poll.h>
#include <setjmp.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace co {

class CoroutineMachine;
class Coroutine;

using CoroutineFunctor = std::function<void(Coroutine *)>;
using CompletionCallback = std::function<void(Coroutine*)>;

constexpr size_t kCoDefaultStackSize = 8192;

enum class CoroutineState {
  kCoNew,
  kCoReady,
  kCoRunning,
  kCoYielded,
  kCoWaiting,
  kCoDead,
};

// This is a Coroutine.  It executes its functor (pointer to a function).
// It has its own stack.
// The functor must be convertible to a function address.
class Coroutine {
public:
  Coroutine(CoroutineMachine &machine, CoroutineFunctor functor, bool autostart = true,
            size_t stack_size = kCoDefaultStackSize, void *user_data = nullptr);

  ~Coroutine();

  // Start a coroutine running if it is not already running,
  void Start();

  // Yield control to another coroutine.
  void Yield();

  // Call another coroutine and store the result.
  void Call(Coroutine &callee, void *result, size_t result_size);
  // Yield control and store value.
  void YieldValue(void *value);

  // Wait for a file descriptor to become ready.
  void Wait(int fd, int event_mask);

  void TriggerEvent();
  void ClearEvent();
  void Exit();
  void Resume();

  void SetName(const std::string &name) { name_ = name; }
  const std::string &GetName() const { return name_; }

  void SetUserData(void *user_data) { user_data_ = user_data; }
  void *GetUserData() const { return user_data_; }

  bool IsAlive(Coroutine &query);

  uint64_t LastTick() const { return last_tick_; }
  CoroutineMachine &Machine() const { return machine_; }

  CoroutineState State() const { return state_; }

  void Show();
  struct pollfd *GetPollFd();

  int64_t Id() const { return id_; }

private:
  CoroutineMachine &machine_;
  size_t id_;                // Coroutine ID.
  std::string name_;         // Optional name.
  CoroutineFunctor functor_; // Coroutine body.
  CoroutineState state_;
  void *stack_;                     // Stack, allocated from malloc.
  void *yielded_address_ = nullptr; // Address at which we've yielded.
  size_t stack_size_;
  jmp_buf resume_;              // Program environemnt for resuming.
  jmp_buf exit_;                // Program environemt to exit.
  struct pollfd event_fd_;      // Pollfd for event.
  struct pollfd wait_fd_;       // Pollfd for waiting for an fd.
  Coroutine *caller_ = nullptr; // If being called, who is calling us.
  void *result_ = nullptr;      // Where to put result in YieldValue.
  size_t result_size_ = 0;      // Length of value to store.
  void *user_data_;             // User data, not owned by this.
  uint64_t last_tick_ = 0;      // Tick count of last resume.
};

struct PollState {
  std::vector<struct pollfd> pollfds;
  std::vector<Coroutine *> coroutines;
};

class CoroutineMachine {
public:
  CoroutineMachine();
  ~CoroutineMachine();

  void Stop();
  size_t AllocateId();

  void AddCoroutine(Coroutine *c);
  void RemoveCoroutine(Coroutine *c);
  void StartCoroutine(Coroutine *c);

  void GetPollState(PollState *poll_state);

  void ProcessPoll(PollState *poll_state);

  void Run();

  uint64_t TickCount() const { return tick_count_; }

  bool IdExists(int id) const { return coroutine_ids_.Contains(id); }

  // Print the state of all the coroutines to stderr.
  void Show();

  jmp_buf& YieldBuf() { return yield_; }
  
  void SetCompletionCallback(CompletionCallback callback) {
    completion_callback_ = callback;
  }

private:
  void BuildPollFds(PollState *poll_state);
  Coroutine *ChooseRunnable(PollState *poll_state, int num_ready);

  Coroutine *GetRunnableCoroutine(PollState *poll_state, int num_ready);

  std::list<Coroutine *> coroutines_;
  BitSet coroutine_ids_;
  ssize_t last_freed_coroutine_id_ = -1;
  std::vector<Coroutine*> ready_coroutines_;
  jmp_buf yield_;
  bool running_ = false;
  PollState poll_state_;
  struct pollfd interrupt_fd_;
  uint64_t tick_count_ = 0;
  CompletionCallback completion_callback_;
};

} // namespace co
#endif /* coroutine_h */
