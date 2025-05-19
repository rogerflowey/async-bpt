// tasks.h
#pragma once

#include <coroutine>
#include <utility>      // For std::exchange, std::move
#include <exception>    // For std::terminate, std::current_exception, std::exception_ptr
#include <stdexcept>    // For std::runtime_error
#include <optional>     // For SharedState value
#include <vector>       // For SharedState continuations
#include <memory>       // For std::shared_ptr
#include <iostream>     // For debugging
#include <atomic>       // For unique promise IDs
#include <string>       // For promise names/types

namespace wutong {

// Forward declaration
template<typename T> struct Task;
template<typename T> class SharedTask;


// Helper to get a string representation of the promise type for logging
template<typename T>
std::string promise_type_name() {
    if constexpr (std::is_void_v<T>) {
        return "PromiseForVoidSpecial";
    } else {
        return "PromiseWithValue<T>";
    }
}


namespace detail {
  // Global unique ID for promises for easier tracking
  inline std::atomic<uint64_t> next_promise_id = 1;

  template<typename T>
  struct PromiseBase {
    uint64_t promise_id_; // Unique ID for this promise instance
    std::coroutine_handle<> continuation_ = nullptr;
    void* continuation_address_for_logging_ = nullptr; // Store address for logging
    std::exception_ptr exception_ptr_ = nullptr;
    std::string promise_name_for_logging_;


    PromiseBase() : promise_id_(next_promise_id++) {
        promise_name_for_logging_ = promise_type_name<T>() + "[" + std::to_string(promise_id_) + "]";
        // std::cout << promise_name_for_logging_ << " constructed. Handle: "
        //           << std::coroutine_handle<PromiseBase<T>>::from_promise(*this).address() << std::endl;
    }

    // No copy/move, promise types are not meant to be copied/moved by user
    PromiseBase(const PromiseBase&) = delete;
    PromiseBase& operator=(const PromiseBase&) = delete;


    std::suspend_never initial_suspend() noexcept {
      // std::cout << promise_name_for_logging_ << " initial_suspend. Coro started." << std::endl;
      return {};
    }

    std::suspend_always final_suspend() noexcept {
      continuation_address_for_logging_ = continuation_ ? continuation_.address() : nullptr;
      std::cout << promise_name_for_logging_ << " final_suspend. Attempting to resume continuation: "
                << continuation_address_for_logging_ << std::endl;
      if (continuation_) {
        std::cout << promise_name_for_logging_ << " Resuming continuation " << continuation_address_for_logging_ << std::endl;
        continuation_.resume();
        std::cout << promise_name_for_logging_ << " Continuation " << continuation_address_for_logging_ << " resumed." << std::endl;
      } else {
        std::cout << promise_name_for_logging_ << " No continuation to resume." << std::endl;
      }
      return {};
    }
    void unhandled_exception() {
      exception_ptr_ = std::current_exception();
      std::cout << promise_name_for_logging_ << " unhandled_exception captured." << std::endl;
    }
  };

  template<typename T>
  struct PromiseWithValue : PromiseBase<T> {
    std::optional<T> result_opt_;

    Task<T> get_return_object(); // Declaration
    void return_value(T value) {
        // std::cout << this->promise_name_for_logging_ << " return_value." << std::endl;
        result_opt_.emplace(std::move(value));
    }
    T get_result() {
        // std::cout << this->promise_name_for_logging_ << " get_result." << std::endl;
        if (this->exception_ptr_) {
            std::rethrow_exception(this->exception_ptr_);
        }
        if (!result_opt_.has_value()) {
            // This should ideally not happen if coroutine logic is correct
            std::cerr << this->promise_name_for_logging_ << " ERROR: get_result called but no value set." << std::endl;
            throw std::runtime_error("Promise result not set before get_result for " + this->promise_name_for_logging_);
        }
        return std::move(result_opt_.value());
    }
  };

  // Specialization for void
  struct PromiseForVoidSpecial : public PromiseBase<void> {
    Task<void> get_return_object(); // Declaration
    void return_void() {
      // std::cout << this->promise_name_for_logging_ << " return_void." << std::endl;
    }
    void get_result() {
        // std::cout << this->promise_name_for_logging_ << " get_result (void)." << std::endl;
        if (this->exception_ptr_) {
            std::rethrow_exception(this->exception_ptr_);
        }
    }
  };

} // namespace detail

// --- Task<T> Definition ---
template<typename T>
struct Task {
  public:
    using promise_type = detail::PromiseWithValue<T>;
    std::coroutine_handle<promise_type> handle;
    uint64_t promise_id_debug_ = 0; // Store promise ID for logging

    explicit Task(std::coroutine_handle<promise_type> h) : handle(h) {
        if (handle) {
            promise_id_debug_ = handle.promise().promise_id_;
            std::cout << "Task created for " << handle.promise().promise_name_for_logging_
                      << " (Task managing handle: " << handle.address() << ")" << std::endl;
        } else {
            std::cout << "Task created with null handle." << std::endl;
        }
    }

    Task(Task&& other) noexcept : handle(std::exchange(other.handle, nullptr)) {
        promise_id_debug_ = other.promise_id_debug_;
        other.promise_id_debug_ = 0; // Invalidate moved-from
        if (handle) {
            std::cout << "Task MOVED for promise_id=" << promise_id_debug_
                      << " (New Task managing handle: " << handle.address()
                      << ", old Task handle now null)" << std::endl;
        } else {
            std::cout << "Task MOVED, resulting in null handle." << std::endl;
        }
    }

    Task& operator=(Task&& other) noexcept {
        std::cout << "Task MOVE ASSIGNMENT: Current Task (promise_id=" << (handle ? std::to_string(handle.promise().promise_id_) : "null")
                  << ", handle=" << (handle ? handle.address() : nullptr) << ")"
                  << " being assigned from Task (promise_id=" << other.promise_id_debug_
                  << ", handle=" << (other.handle ? other.handle.address() : nullptr) << ")" << std::endl;
        if (this != &other) {
            if (handle) {
                std::cout << "Task MOVE ASSIGNMENT: Destroying existing handle " << handle.address()
                          << " for promise_id=" << handle.promise().promise_id_ << std::endl;
                // Critical check before destroying
                if (!handle.done()) {
                     std::cerr << "CRITICAL WARNING (in move assignment): Destroying handle " << handle.address()
                               << " for promise_id=" << handle.promise().promise_id_ << " which is NOT done." << std::endl;
                }
                handle.destroy();
            }
            handle = std::exchange(other.handle, nullptr);
            promise_id_debug_ = other.promise_id_debug_;
            other.promise_id_debug_ = 0;
        }
        return *this;
    }

    ~Task() {
        if (handle) {
            bool is_done = handle.done();
            std::cout << "Task::~Task(): Destroying handle " << handle.address()
                      << " for " << handle.promise().promise_name_for_logging_
                      << " (promise_id=" << promise_id_debug_ << ")"
                      << " (done: " << is_done << ")" << std::endl;

            if (!is_done) {
                std::cerr << "CRITICAL WARNING: Task::~Task(): Destroying handle " << handle.address()
                          << " for " << handle.promise().promise_name_for_logging_
                          << " which is NOT done." << std::endl;
            }
            // Check if the continuation being destroyed is one that some promise is about to resume
            // This requires a global map or more complex tracking, difficult to do robustly here.
            // We rely on seeing if a promise tries to resume a handle that matches one being destroyed.

            handle.destroy();
            std::cout << "Task::~Task(): Handle " << handle.address() << " (promise_id=" << promise_id_debug_ << ") destroyed." << std::endl;
        } else {
            // std::cout << "Task::~Task(): Handle is null (promise_id_debug_=" << promise_id_debug_ << ")." << std::endl;
        }
    }

    bool await_ready() const noexcept {
      bool ready = !handle || handle.done();
    //   if (handle) {
    //       std::cout << "Task for " << handle.promise().promise_name_for_logging_ << " await_ready: " << ready
    //                 << " (handle=" << handle.address() << ", done=" << handle.done() << ")" << std::endl;
    //   }
      return ready;
    }

    bool await_suspend(std::coroutine_handle<> awaiting_coroutine) noexcept {
      if (!handle) {
          std::cerr << "Task (promise_id=" << promise_id_debug_ << ") await_suspend: ERROR! Awaiting on a null/moved-from Task." << std::endl;
          return false; // Should not suspend, likely error state
      }
      std::cout << "Task for " << handle.promise().promise_name_for_logging_ << " await_suspend: Awaiting coroutine is " << awaiting_coroutine.address()
                << ". Setting its continuation. Current handle " << handle.address() << " done: " << handle.done() << std::endl;
      handle.promise().continuation_ = awaiting_coroutine;
      handle.promise().continuation_address_for_logging_ = awaiting_coroutine.address();


      if (handle.done()) {
          std::cout << "Task for " << handle.promise().promise_name_for_logging_ << " await_suspend: Already done. Returning false (don't suspend awaiter)." << std::endl;
          return false;
      }
      std::cout << "Task for " << handle.promise().promise_name_for_logging_ << " await_suspend: Not done. Returning true (suspend awaiter " << awaiting_coroutine.address() << ")." << std::endl;
      return true;
    }

    T await_resume() {
      if (!handle) {
          std::cerr << "Task (promise_id=" << promise_id_debug_ << ") await_resume: ERROR! Awaiting on a null/moved-from Task." << std::endl;
          throw std::runtime_error("Awaiting a moved-from or null Task (promise_id_debug_=" + std::to_string(promise_id_debug_) + ")");
      }
    //   std::cout << "Task for " << handle.promise().promise_name_for_logging_ << " await_resume." << std::endl;
      return handle.promise().get_result();
    }
};

// --- Task<void> Specialization ---
template<>
struct Task<void> {
  public:
    using promise_type = detail::PromiseForVoidSpecial;
    std::coroutine_handle<promise_type> handle;
    uint64_t promise_id_debug_ = 0;

    explicit Task(std::coroutine_handle<promise_type> h) : handle(h) {
        if (handle) {
            promise_id_debug_ = handle.promise().promise_id_;
            std::cout << "Task<void> created for " << handle.promise().promise_name_for_logging_
                      << " (Task managing handle: " << handle.address() << ")" << std::endl;
        } else {
            std::cout << "Task<void> created with null handle." << std::endl;
        }
    }
    Task(Task&& other) noexcept : handle(std::exchange(other.handle, nullptr)) {
        promise_id_debug_ = other.promise_id_debug_;
        other.promise_id_debug_ = 0;
        if (handle) {
             std::cout << "Task<void> MOVED for promise_id=" << promise_id_debug_
                       << " (New Task managing handle: " << handle.address()
                       << ", old Task handle now null)" << std::endl;
        } else {
            std::cout << "Task<void> MOVED, resulting in null handle." << std::endl;
        }
    }
    Task& operator=(Task&& other) noexcept {
        std::cout << "Task<void> MOVE ASSIGNMENT: Current Task (promise_id=" << (handle ? std::to_string(handle.promise().promise_id_) : "null")
                  << ", handle=" << (handle ? handle.address() : nullptr) << ")"
                  << " being assigned from Task (promise_id=" << other.promise_id_debug_
                  << ", handle=" << (other.handle ? other.handle.address() : nullptr) << ")" << std::endl;
        if (this != &other) {
            if (handle) {
                 std::cout << "Task<void> MOVE ASSIGNMENT: Destroying existing handle " << handle.address()
                           << " for promise_id=" << handle.promise().promise_id_ << std::endl;
                if (!handle.done()) {
                     std::cerr << "CRITICAL WARNING (in move assignment): Destroying Task<void> handle " << handle.address()
                               << " for promise_id=" << handle.promise().promise_id_ << " which is NOT done." << std::endl;
                }
                handle.destroy();
            }
            handle = std::exchange(other.handle, nullptr);
            promise_id_debug_ = other.promise_id_debug_;
            other.promise_id_debug_ = 0;
        }
        return *this;
    }
    ~Task() {
        if (handle) {
            bool is_done = handle.done();
            std::cout << "Task<void>::~Task(): Destroying handle " << handle.address()
                      << " for " << handle.promise().promise_name_for_logging_
                      << " (promise_id=" << promise_id_debug_ << ")"
                      << " (done: " << is_done << ")" << std::endl;
            if (!is_done) {
                 std::cerr << "CRITICAL WARNING: Task<void>::~Task(): Destroying handle " << handle.address()
                           << " for " << handle.promise().promise_name_for_logging_
                           << " which is NOT done." << std::endl;
            }
            handle.destroy();
            std::cout << "Task<void>::~Task(): Handle " << handle.address() << " (promise_id=" << promise_id_debug_ << ") destroyed." << std::endl;
        } else {
            // std::cout << "Task<void>::~Task(): Handle is null (promise_id_debug_=" << promise_id_debug_ << ")." << std::endl;
        }
    }

    bool await_ready() const noexcept {
      bool ready = !handle || handle.done();
    //   if (handle) {
    //       std::cout << "Task<void> for " << handle.promise().promise_name_for_logging_ << " await_ready: " << ready
    //                 << " (handle=" << handle.address() << ", done=" << handle.done() << ")" << std::endl;
    //   }
      return ready;
    }
    bool await_suspend(std::coroutine_handle<> awaiting_coroutine) noexcept {
      if (!handle) {
          std::cerr << "Task<void> (promise_id=" << promise_id_debug_ << ") await_suspend: ERROR! Awaiting on a null/moved-from Task." << std::endl;
          return false;
      }
      std::cout << "Task<void> for " << handle.promise().promise_name_for_logging_ << " await_suspend: Awaiting coroutine is " << awaiting_coroutine.address()
                << ". Setting its continuation. Current handle " << handle.address() << " done: " << handle.done() << std::endl;
      handle.promise().continuation_ = awaiting_coroutine;
      handle.promise().continuation_address_for_logging_ = awaiting_coroutine.address();

      if (handle.done()) {
          std::cout << "Task<void> for " << handle.promise().promise_name_for_logging_ << " await_suspend: Already done. Returning false (don't suspend awaiter)." << std::endl;
          return false;
      }
      std::cout << "Task<void> for " << handle.promise().promise_name_for_logging_ << " await_suspend: Not done. Returning true (suspend awaiter " << awaiting_coroutine.address() << ")." << std::endl;
      return true;
    }
    void await_resume() {
      if (!handle) {
          std::cerr << "Task<void> (promise_id=" << promise_id_debug_ << ") await_resume: ERROR! Awaiting on a null/moved-from Task." << std::endl;
          throw std::runtime_error("Awaiting a moved-from or null Task<void> (promise_id_debug_=" + std::to_string(promise_id_debug_) + ")");
      }
    //   std::cout << "Task<void> for " << handle.promise().promise_name_for_logging_ << " await_resume." << std::endl;
      handle.promise().get_result();
    }
};

// --- Definitions for get_return_object ---
namespace detail {
  template<typename T>
  Task<T> PromiseWithValue<T>::get_return_object() {
    // std::cout << this->promise_name_for_logging_ << " get_return_object (Task<T>)." << std::endl;
    return Task<T>{std::coroutine_handle<PromiseWithValue<T>>::from_promise(*this)};
  }

  inline Task<void> PromiseForVoidSpecial::get_return_object() {
    // std::cout << this->promise_name_for_logging_ << " get_return_object (Task<void>)." << std::endl;
    return Task<void>{std::coroutine_handle<PromiseForVoidSpecial>::from_promise(*this)};
  }
} // namespace detail


// ---  SharedTask ---
// (Assuming SharedTask and its promise are in the same file or included)
// Add similar promise_id_ and logging to SharedTaskPromise if needed,
// though the current issue seems focused on wutong::Task.
// For brevity, I'll omit SharedTask logging changes unless specifically requested.
// Remember we fixed SharedTaskPromise::final_suspend to std::suspend_never.

namespace detail {
    template <typename T>
    struct SharedState {
        enum class State { PENDING, VALUE, EXCEPTION } current_state_ = State::PENDING;
        std::optional<T> value_;
        std::exception_ptr exception_ptr_;
        std::vector<std::coroutine_handle<>> continuations_; // These are awaiters of SharedTask

        void set_value(T val) {
            // std::cout << "SharedState setting value. Num continuations: " << continuations_.size() << std::endl;
            if (current_state_ == State::PENDING) {
                value_ = std::move(val);
                current_state_ = State::VALUE;
                for (auto& h : continuations_) {
                    if (h) {
                        // std::cout << "SharedState resuming continuation " << h.address() << std::endl;
                        h.resume();
                    }
                }
                continuations_.clear();
            }
        }
        void set_exception(std::exception_ptr ex) {
            // std::cout << "SharedState setting exception. Num continuations: " << continuations_.size() << std::endl;
            if (current_state_ == State::PENDING) {
                exception_ptr_ = ex;
                current_state_ = State::EXCEPTION;
                for (auto& h : continuations_) { if (h) h.resume(); }
                continuations_.clear();
            }
        }
        bool add_continuation_and_check_state(std::coroutine_handle<> awaiting_coro) {
            // std::cout << "SharedState adding continuation " << awaiting_coro.address() << ". Current state: " << (int)current_state_ << std::endl;
            if (current_state_ != State::PENDING) {
                return false; // Already completed, resume immediately
            }
            continuations_.push_back(awaiting_coro);
            return true; // Suspend
        }
        T get_value_or_rethrow() {
            if (current_state_ == State::EXCEPTION) {
                std::rethrow_exception(exception_ptr_);
            }
            if (!value_) { // Should not happen if state is VALUE
                 std::cerr << "SharedState ERROR: get_value_or_rethrow called but no value and no exception." << std::endl;
                 throw std::logic_error("SharedState inconsistent: VALUE state but no value");
            }
            return *value_;
        }
    };

    template <> // void specialization for SharedState
    struct SharedState<void> {
        enum class State { PENDING, COMPLETED, EXCEPTION } current_state_ = State::PENDING;
        std::exception_ptr exception_ptr_;
        std::vector<std::coroutine_handle<>> continuations_;

        void set_completed() {
            // std::cout << "SharedState<void> setting completed. Num continuations: " << continuations_.size() << std::endl;
            if (current_state_ == State::PENDING) {
                current_state_ = State::COMPLETED;
                for (auto& h : continuations_) { if (h) h.resume(); }
                continuations_.clear();
            }
        }
        void set_exception(std::exception_ptr ex) {
            // std::cout << "SharedState<void> setting exception. Num continuations: " << continuations_.size() << std::endl;
            if (current_state_ == State::PENDING) {
                exception_ptr_ = ex;
                current_state_ = State::EXCEPTION;
                for (auto& h : continuations_) { if (h) h.resume(); }
                continuations_.clear();
            }
        }
        bool add_continuation_and_check_state(std::coroutine_handle<> awaiting_coro) {
            // std::cout << "SharedState<void> adding continuation " << awaiting_coro.address() << ". Current state: " << (int)current_state_ << std::endl;
            if (current_state_ != State::PENDING) { return false; }
            continuations_.push_back(awaiting_coro);
            return true;
        }
        void get_value_or_rethrow() {
            if (current_state_ == State::EXCEPTION) {
                std::rethrow_exception(exception_ptr_);
            }
        }
    };


    template <typename T>
    struct SharedTaskPromise {
        std::shared_ptr<SharedState<T>> shared_state_ptr_;
        // uint64_t promise_id_; // Can add unique ID here too if needed for SharedTask producers

        SharedTaskPromise() : shared_state_ptr_(std::make_shared<SharedState<T>>()) {
            // promise_id_ = next_promise_id++;
            // std::cout << "SharedTaskPromise<T>[" << promise_id_ << "] constructed." << std::endl;
        }

        SharedTask<T> get_return_object();

        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_never final_suspend() noexcept { // Fixed earlier
            // std::cout << "SharedTaskPromise<T>[" << promise_id_ << "] final_suspend." << std::endl;
            return {};
        }
        void return_value(T value) {
            // std::cout << "SharedTaskPromise<T>[" << promise_id_ << "] return_value." << std::endl;
            if (shared_state_ptr_) shared_state_ptr_->set_value(std::move(value));
        }
        void unhandled_exception() {
            // std::cout << "SharedTaskPromise<T>[" << promise_id_ << "] unhandled_exception." << std::endl;
            if (shared_state_ptr_) shared_state_ptr_->set_exception(std::current_exception());
        }
    };

    template <> // void specialization for SharedTaskPromise
    struct SharedTaskPromise<void> {
        std::shared_ptr<SharedState<void>> shared_state_ptr_;
        // uint64_t promise_id_;

        SharedTaskPromise() : shared_state_ptr_(std::make_shared<SharedState<void>>()) {
            // promise_id_ = next_promise_id++;
            // std::cout << "SharedTaskPromise<void>[" << promise_id_ << "] constructed." << std::endl;
        }

        SharedTask<void> get_return_object();

        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_never final_suspend() noexcept { // Fixed earlier
            // std::cout << "SharedTaskPromise<void>[" << promise_id_ << "] final_suspend." << std::endl;
            return {};
        }
        void return_void() {
            // std::cout << "SharedTaskPromise<void>[" << promise_id_ << "] return_void." << std::endl;
            if (shared_state_ptr_) shared_state_ptr_->set_completed();
        }
        void unhandled_exception() {
            // std::cout << "SharedTaskPromise<void>[" << promise_id_ << "] unhandled_exception." << std::endl;
            if (shared_state_ptr_) shared_state_ptr_->set_exception(std::current_exception());
        }
    };

} // namespace detail


template <typename T>
class SharedTask {
    std::shared_ptr<detail::SharedState<T>> state_;

public:
    // Default constructor for cases where a SharedTask might be declared but not immediately assigned.
    SharedTask() = default;

    explicit SharedTask(std::shared_ptr<detail::SharedState<T>> state) : state_(std::move(state)) {}

    SharedTask(const SharedTask&) = default;
    SharedTask& operator=(const SharedTask&) = default;
    SharedTask(SharedTask&&) = default;
    SharedTask& operator=(SharedTask&&) = default;

    bool await_ready() const noexcept {
        if (!state_) return true; // Awaiting a default-constructed/moved-from SharedTask is ready.
        return state_->current_state_ != detail::SharedState<T>::State::PENDING;
    }

    bool await_suspend(std::coroutine_handle<> awaiting_coro) noexcept {
        if (!state_) return false; // No state to suspend on.
        return state_->add_continuation_and_check_state(awaiting_coro);
    }

    T await_resume() {
        if (!state_) {
            // This case should ideally be caught by await_ready or if !state_ means it's a no-op.
            // Depending on semantics, could throw or return default T.
            // For now, let's assume if state_ is null, it's an error or a completed no-op.
            // std::cerr << "SharedTask::await_resume() called on a null state_." << std::endl;
            if constexpr (!std::is_void_v<T>) {
                 // This might be problematic if T is not default-constructible.
                 // Consider throwing an exception if state_ is null and await_ready didn't catch it.
                return T{};
            } else {
                return;
            }
        }
        return state_->get_value_or_rethrow();
    }
};

template <> // void specialization for SharedTask
class SharedTask<void> {
    std::shared_ptr<detail::SharedState<void>> state_;
public:
    SharedTask() = default;
    explicit SharedTask(std::shared_ptr<detail::SharedState<void>> state) : state_(std::move(state)) {}
    SharedTask(const SharedTask&) = default;
    SharedTask& operator=(const SharedTask&) = default;
    SharedTask(SharedTask&&) = default;
    SharedTask& operator=(SharedTask&&) = default;

    bool await_ready() const noexcept {
        if (!state_) return true;
        return state_->current_state_ != detail::SharedState<void>::State::PENDING;
    }
    bool await_suspend(std::coroutine_handle<> awaiting_coro) noexcept {
        if (!state_) return false;
        return state_->add_continuation_and_check_state(awaiting_coro);
    }
    void await_resume() {
        if (!state_) return;
        state_->get_value_or_rethrow();
    }
};


namespace detail {
    template<typename T>
    SharedTask<T> SharedTaskPromise<T>::get_return_object() {
        // std::cout << "SharedTaskPromise<T>[" << promise_id_ << "] get_return_object." << std::endl;
        return SharedTask<T>(shared_state_ptr_);
    }

    inline SharedTask<void> SharedTaskPromise<void>::get_return_object() {
        // std::cout << "SharedTaskPromise<void>[" << promise_id_ << "] get_return_object." << std::endl;
        return SharedTask<void>(shared_state_ptr_);
    }
} // namespace detail


} // namespace wutong

// Coroutine traits for SharedTask
namespace std {
    template <typename T, typename... Args>
    struct coroutine_traits<wutong::SharedTask<T>, Args...> {
        using promise_type = wutong::detail::SharedTaskPromise<T>;
    };
}