#pragma once

#include <coroutine>
#include <utility>      // For std::exchange
#include <exception>    // For std::terminate, std::current_exception, std::exception_ptr
#include <stdexcept>    // For std::runtime_error
#include <optional>     // For SharedState value
#include <vector>       // For SharedState continuations
#include <memory>       // For std::shared_ptr
#include <iostream>     // For debug cout/cerr
#include <atomic>       // For unique IDs
#include <iomanip>      // For std::setw, std::left for aligned logging (optional)

// Helper for consistent logging prefix
#define LOG_PREFIX_WIDTH 25 // Adjust as needed
#define LOG_DEBUG std::cout << std::left << std::setw(LOG_PREFIX_WIDTH) << __func__ << " | "
#define LOG_WARN  std::cerr << std::left << std::setw(LOG_PREFIX_WIDTH) << __func__ << " | WARN: "
#define LOG_CRITICAL std::cerr << std::left << std::setw(LOG_PREFIX_WIDTH) << __func__ << " | CRITICAL: "


namespace wutong {

// Forward declaration of Task
template<typename T>
struct Task;

// Forward declaration for SharedTask's promise
template<typename T>
class SharedTask;


namespace detail {

  // --- Promise IDs ---
  inline std::atomic<uint64_t> g_promise_id_counter = 0;
  inline std::atomic<uint64_t> g_shared_state_id_counter = 0;

  template<typename T>
  struct PromiseBase {
    uint64_t promise_id_; // Unique ID for this promise instance
    std::coroutine_handle<> continuation_ = nullptr;
    std::exception_ptr exception_ptr_ = nullptr;

    PromiseBase() : promise_id_(g_promise_id_counter++) {
        LOG_DEBUG << "PromiseID: " << promise_id_ << ", PromiseAddr: " << this << " constructed." << std::endl;
    }

    // Prevent copying/moving of promise base if it's ever attempted directly
    PromiseBase(const PromiseBase&) = delete;
    PromiseBase& operator=(const PromiseBase&) = delete;
    PromiseBase(PromiseBase&&) = delete;
    PromiseBase& operator=(PromiseBase&&) = delete;

    // MODIFICATION: Use suspend_never for eager start
    std::suspend_never initial_suspend() noexcept {
        LOG_DEBUG << "PromiseID: " << promise_id_ << ", PromiseAddr: " << this << " initial_suspend (eager)." << std::endl;
        return {};
    }

    std::suspend_always final_suspend() noexcept {
      LOG_DEBUG << "PromiseID: " << promise_id_ << ", PromiseAddr: " << this << " final_suspend. Continuation: "
                << (continuation_ ? continuation_.address() : nullptr) << std::endl;
      if (continuation_) {
        LOG_DEBUG << "PromiseID: " << promise_id_ << " resuming continuation " << continuation_.address() << std::endl;
        continuation_.resume();
      }
      return {};
    }
    void unhandled_exception() {
      exception_ptr_ = std::current_exception();
      LOG_WARN << "PromiseID: " << promise_id_ << ", PromiseAddr: " << this << " unhandled_exception captured." << std::endl;
    }
  };

  template<typename T>
  struct PromiseWithValue : PromiseBase<T> {
    std::optional<T> result_opt_;

    Task<T> get_return_object(); // Declaration below
    void return_value(T value) {
        LOG_DEBUG << "PromiseID: " << this->promise_id_ << ", PromiseAddr: " << this << " return_value set." << std::endl;
        result_opt_.emplace(std::move(value));
    }
    T get_result() {
        LOG_DEBUG << "PromiseID: " << this->promise_id_ << ", PromiseAddr: " << this << " get_result called." << std::endl;
        if (this->exception_ptr_) {
            LOG_DEBUG << "PromiseID: " << this->promise_id_ << " rethrowing exception." << std::endl;
            std::rethrow_exception(this->exception_ptr_);
        }
        if (!result_opt_.has_value()) {
            LOG_CRITICAL << "PromiseID: " << this->promise_id_ << " result not set before get_result!" << std::endl;
            throw std::runtime_error("Promise result not set before get_result");
        }
        return std::move(result_opt_.value()); // Move out the value
    }
  };

  struct PromiseForVoidSpecial : public PromiseBase<void> {
    Task<void> get_return_object(); // Declaration below
    void return_void() {
      LOG_DEBUG << "PromiseID: " << this->promise_id_ << ", PromiseAddr: " << this << " return_void." << std::endl;
    }
    void get_result() {
        LOG_DEBUG << "PromiseID: " << this->promise_id_ << ", PromiseAddr: " << this << " get_result (void) called." << std::endl;
        if (this->exception_ptr_) {
            LOG_DEBUG << "PromiseID: " << this->promise_id_ << " rethrowing exception." << std::endl;
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

    explicit Task(std::coroutine_handle<promise_type> h) : handle(h) {
        LOG_DEBUG << "TaskObj: " << this << " constructed. Manages Handle: " << (handle ? handle.address() : nullptr)
                  << ", PromiseID: " << (handle ? handle.promise().promise_id_ : -1) // Use -1 or similar for null handle case
                  << ", PromiseAddr: " << (handle ? (void*)&handle.promise() : nullptr) << std::endl;
    }

    Task(Task&& other) noexcept : handle(std::exchange(other.handle, nullptr)) {
        LOG_DEBUG << "TaskObj: " << this << " move constructed from TaskObj: " << &other
                  << ". New Handle: " << (handle ? handle.address() : nullptr)
                  << ", Old TaskObj " << &other << " now has null handle." << std::endl;
    }
    Task& operator=(Task&& other) noexcept {
        LOG_DEBUG << "TaskObj: " << this << " move assigned from TaskObj: " << &other << std::endl;
        if (this != &other) {
            if (handle) { // If current task holds a handle, it will be destroyed
                 LOG_WARN << "TaskObj: " << this << " (move assignment) destroying its existing Handle: "
                          << handle.address() << " (PromiseID: " << handle.promise().promise_id_ << ")" << std::endl;
                 // This implicit destruction needs careful thought. Usually, a Task is awaited or explicitly managed.
                 // If this task was not 'done', its destruction here is problematic.
                 if (!handle.done()) {
                    LOG_CRITICAL << "TaskObj: " << this << " (move assignment) destroying UNDONE Handle: "
                                 << handle.address() << " (PromiseID: " << handle.promise().promise_id_ << ")" << std::endl;
                 }
                 handle.destroy();
            }
            handle = std::exchange(other.handle, nullptr);
            LOG_DEBUG << "  New Handle: " << (handle ? handle.address() : nullptr)
                      << ", Old TaskObj " << &other << " now has null handle." << std::endl;
        }
        return *this;
    }


    ~Task() {
        if (handle) {
            bool is_done = handle.done();
            auto* promise_ptr = (void*)&handle.promise();
            uint64_t promise_id = handle.promise().promise_id_;

            LOG_DEBUG << "TaskObj: " << this << " destroying. Manages Handle: " << handle.address()
                      << ", PromiseID: " << promise_id
                      << ", PromiseAddr: " << promise_ptr
                      << ", Done: " << std::boolalpha << is_done << std::endl;

            if (!is_done) {
                LOG_CRITICAL << "TaskObj: " << this << " destroying Handle " << handle.address()
                             << " (PromiseID: " << promise_id << ", PromiseAddr: " << promise_ptr << ")"
                             << " for a coroutine that is NOT DONE. This will lead to a crash if the coroutine was suspended on async I/O." << std::endl;
                // Consider std::terminate() here if this is an unrecoverable state for your app
            }

            if (is_done && handle.promise().continuation_ == nullptr) {
                 LOG_DEBUG << "  Handle " << handle.address() << " (PromiseID: " << promise_id
                           << ") was done and its continuation was nullptr (e.g., top-level task or not awaited by wutong::Task)." << std::endl;
            }
            
            LOG_DEBUG << "  Calling handle.destroy() for Handle: " << handle.address() << " (PromiseID: " << promise_id << ")" << std::endl;
            handle.destroy();
            LOG_DEBUG << "  handle.destroy() returned for Handle: " << handle.address() << " (PromiseID: " << promise_id << ")" << std::endl;

        } else {
            LOG_DEBUG << "TaskObj: " << this << " destroying. Handle is null (normal for moved-from Task)." << std::endl;
        }
    }

    void start() { /* Eager start makes this less critical */ }

    bool await_ready() const noexcept {
      bool ready = !handle || handle.done();
      LOG_DEBUG << "TaskObj: " << this << " await_ready() called. Handle: " << (handle ? handle.address() : nullptr)
                << (handle ? ", PromiseID: " + std::to_string(handle.promise().promise_id_) : "")
                << ". Ready: " << std::boolalpha << ready << std::endl;
      return ready;
    }

    bool await_suspend(std::coroutine_handle<> awaiting_coroutine) noexcept {
      LOG_DEBUG << "TaskObj: " << this << " await_suspend() called by awaiting_coro: " << awaiting_coroutine.address()
                << ". My Handle: " << handle.address() << " (PromiseID: " << handle.promise().promise_id_ << ")" << std::endl;
      handle.promise().continuation_ = awaiting_coroutine;
      if (handle.done()) {
          LOG_DEBUG << "  My Handle " << handle.address() << " is already done. Returning false (don't suspend awaiter)." << std::endl;
          return false; 
      }
      LOG_DEBUG << "  My Handle " << handle.address() << " is not done. Returning true (suspend awaiter)." << std::endl;
      return true; 
    }

    T await_resume() {
      LOG_DEBUG << "TaskObj: " << this << " await_resume() called. My Handle: " << (handle ? handle.address() : nullptr)
                << (handle ? ", PromiseID: " + std::to_string(handle.promise().promise_id_) : "") << std::endl;
      if (!handle) {
        LOG_CRITICAL << "TaskObj: " << this << " Awaiting a moved-from or null Task!" << std::endl;
        throw std::runtime_error("Awaiting a moved-from or null Task");
      }
      return handle.promise().get_result();
    }
};

// --- Task<void> Specialization ---
template<>
struct Task<void> {
  public:
    using promise_type = detail::PromiseForVoidSpecial;
    std::coroutine_handle<promise_type> handle;

    explicit Task(std::coroutine_handle<promise_type> h) : handle(h) {
        LOG_DEBUG << "TaskObj<void>: " << this << " constructed. Manages Handle: " << (handle ? handle.address() : nullptr)
                  << ", PromiseID: " << (handle ? handle.promise().promise_id_ : -1)
                  << ", PromiseAddr: " << (handle ? (void*)&handle.promise() : nullptr) << std::endl;
    }
    Task(Task&& other) noexcept : handle(std::exchange(other.handle, nullptr)) {
        LOG_DEBUG << "TaskObj<void>: " << this << " move constructed from TaskObj: " << &other
                  << ". New Handle: " << (handle ? handle.address() : nullptr)
                  << ", Old TaskObj " << &other << " now has null handle." << std::endl;
    }
     Task& operator=(Task&& other) noexcept {
        LOG_DEBUG << "TaskObj<void>: " << this << " move assigned from TaskObj: " << &other << std::endl;
        if (this != &other) {
            if (handle) {
                 LOG_WARN << "TaskObj<void>: " << this << " (move assignment) destroying its existing Handle: "
                          << handle.address() << " (PromiseID: " << handle.promise().promise_id_ << ")" << std::endl;
                 if (!handle.done()) {
                    LOG_CRITICAL << "TaskObj<void>: " << this << " (move assignment) destroying UNDONE Handle: "
                                 << handle.address() << " (PromiseID: " << handle.promise().promise_id_ << ")" << std::endl;
                 }
                 handle.destroy();
            }
            handle = std::exchange(other.handle, nullptr);
            LOG_DEBUG << "  New Handle: " << (handle ? handle.address() : nullptr)
                      << ", Old TaskObj " << &other << " now has null handle." << std::endl;
        }
        return *this;
    }

    ~Task() {
        if (handle) {
            bool is_done = handle.done();
            auto* promise_ptr = (void*)&handle.promise();
            uint64_t promise_id = handle.promise().promise_id_;

            LOG_DEBUG << "TaskObj<void>: " << this << " destroying. Manages Handle: " << handle.address()
                      << ", PromiseID: " << promise_id
                      << ", PromiseAddr: " << promise_ptr
                      << ", Done: " << std::boolalpha << is_done << std::endl;

            if (!is_done) {
                 LOG_CRITICAL << "TaskObj<void>: " << this << " destroying Handle " << handle.address()
                             << " (PromiseID: " << promise_id << ", PromiseAddr: " << promise_ptr << ")"
                             << " for a coroutine that is NOT DONE. This will lead to a crash if the coroutine was suspended on async I/O." << std::endl;
            }
            if (is_done && handle.promise().continuation_ == nullptr) {
                 LOG_DEBUG << "  Handle " << handle.address() << " (PromiseID: " << promise_id
                           << ") was done and its continuation was nullptr." << std::endl;
            }

            LOG_DEBUG << "  Calling handle.destroy() for Handle: " << handle.address() << " (PromiseID: " << promise_id << ")" << std::endl;
            handle.destroy();
            LOG_DEBUG << "  handle.destroy() returned for Handle: " << handle.address() << " (PromiseID: " << promise_id << ")" << std::endl;
        } else {
            LOG_DEBUG << "TaskObj<void>: " << this << " destroying. Handle is null (normal for moved-from Task)." << std::endl;
        }
    }

    void start() { /* Eager start makes this less critical */ }

    bool await_ready() const noexcept {
      bool ready = !handle || handle.done();
      LOG_DEBUG << "TaskObj<void>: " << this << " await_ready() called. Handle: " << (handle ? handle.address() : nullptr)
                << (handle ? ", PromiseID: " + std::to_string(handle.promise().promise_id_) : "")
                << ". Ready: " << std::boolalpha << ready << std::endl;
      return ready;
    }

    bool await_suspend(std::coroutine_handle<> awaiting_coroutine) noexcept {
      LOG_DEBUG << "TaskObj<void>: " << this << " await_suspend() called by awaiting_coro: " << awaiting_coroutine.address()
                << ". My Handle: " << handle.address() << " (PromiseID: " << handle.promise().promise_id_ << ")" << std::endl;
      handle.promise().continuation_ = awaiting_coroutine;
      if (handle.done()) {
          LOG_DEBUG << "  My Handle " << handle.address() << " is already done. Returning false (don't suspend awaiter)." << std::endl;
          return false;
      }
      LOG_DEBUG << "  My Handle " << handle.address() << " is not done. Returning true (suspend awaiter)." << std::endl;
      return true;
    }

    void await_resume() {
      LOG_DEBUG << "TaskObj<void>: " << this << " await_resume() called. My Handle: " << (handle ? handle.address() : nullptr)
                << (handle ? ", PromiseID: " + std::to_string(handle.promise().promise_id_) : "") << std::endl;
      if (!handle) {
        LOG_CRITICAL << "TaskObj<void>: " << this << " Awaiting a moved-from or null Task!" << std::endl;
        throw std::runtime_error("Awaiting a moved-from or null Task");
      }
      handle.promise().get_result();
    }
};

// --- Definitions for get_return_object ---
namespace detail {
  template<typename T>
  Task<T> PromiseWithValue<T>::get_return_object() {
    auto coro_handle = std::coroutine_handle<PromiseWithValue<T>>::from_promise(*this);
    LOG_DEBUG << "PromiseID: " << this->promise_id_ << ", PromiseAddr: " << this
              << " get_return_object() -> Task for Handle: " << coro_handle.address() << std::endl;
    return Task<T>{coro_handle};
  }

  inline Task<void> PromiseForVoidSpecial::get_return_object() {
    auto coro_handle = std::coroutine_handle<PromiseForVoidSpecial>::from_promise(*this);
    LOG_DEBUG << "PromiseID: " << this->promise_id_ << ", PromiseAddr: " << this
              << " get_return_object() -> Task<void> for Handle: " << coro_handle.address() << std::endl;
    return Task<void>{coro_handle};
  }
} // namespace detail


// ---  SharedTask ---
namespace detail {
    template <typename T>
    struct SharedState {
        uint64_t shared_state_id_;
        enum class State { PENDING, VALUE, EXCEPTION } current_state_ = State::PENDING;
        std::optional<T> value_;
        std::exception_ptr exception_ptr_;
        std::vector<std::coroutine_handle<>> continuations_;

        SharedState() : shared_state_id_(g_shared_state_id_counter++) {
            LOG_DEBUG << "SharedStateID: " << shared_state_id_ << ", Addr: " << this << " constructed." << std::endl;
        }
        ~SharedState() {
            LOG_DEBUG << "SharedStateID: " << shared_state_id_ << ", Addr: " << this << " destructed. State: " << (int)current_state_
                      << ", Continuations: " << continuations_.size() << std::endl;
        }


        void set_value(T val) {
            LOG_DEBUG << "SharedStateID: " << shared_state_id_ << " set_value(). Current state: " << (int)current_state_ << std::endl;
            if (current_state_ == State::PENDING) {
                value_ = std::move(val);
                current_state_ = State::VALUE;
                LOG_DEBUG << "  Resuming " << continuations_.size() << " continuations." << std::endl;
                for (auto& h : continuations_) { if (h) {
                    LOG_DEBUG << "    Resuming continuation " << h.address() << std::endl;
                    h.resume();
                }}
                continuations_.clear();
            }
        }
        void set_exception(std::exception_ptr ex) {
            LOG_DEBUG << "SharedStateID: " << shared_state_id_ << " set_exception(). Current state: " << (int)current_state_ << std::endl;
            if (current_state_ == State::PENDING) {
                exception_ptr_ = ex;
                current_state_ = State::EXCEPTION;
                LOG_DEBUG << "  Resuming " << continuations_.size() << " continuations." << std::endl;
                for (auto& h : continuations_) { if (h) {
                     LOG_DEBUG << "    Resuming continuation " << h.address() << std::endl;
                    h.resume();
                }}
                continuations_.clear();
            }
        }
        bool add_continuation_and_check_state(std::coroutine_handle<> awaiting_coro) {
            LOG_DEBUG << "SharedStateID: " << shared_state_id_ << " add_continuation_and_check_state() for awaiting_coro: " << awaiting_coro.address()
                      << ". Current state: " << (int)current_state_ << std::endl;
            if (current_state_ != State::PENDING) {
                LOG_DEBUG << "  Already completed. Returning false (don't suspend)." << std::endl;
                return false; 
            }
            continuations_.push_back(awaiting_coro);
            LOG_DEBUG << "  Pending. Added continuation. Returning true (suspend)." << std::endl;
            return true; 
        }
        T get_value_or_rethrow() {
            LOG_DEBUG << "SharedStateID: " << shared_state_id_ << " get_value_or_rethrow(). Current state: " << (int)current_state_ << std::endl;
            if (current_state_ == State::EXCEPTION) {
                LOG_DEBUG << "  Rethrowing exception." << std::endl;
                std::rethrow_exception(exception_ptr_);
            }
            if (!value_) { // Should ideally be checked by current_state_ == State::VALUE
                 LOG_CRITICAL << "SharedStateID: " << shared_state_id_ << " get_value_or_rethrow() called but no value and no exception!" << std::endl;
                 throw std::runtime_error("SharedState value not set and no exception");
            }
            return *value_; // For T where T might be non-copyable, consider returning T& or T&& if value_ is std::move'd out
        }
    };

    template <>
    struct SharedState<void> {
        uint64_t shared_state_id_;
        enum class State { PENDING, COMPLETED, EXCEPTION } current_state_ = State::PENDING;
        std::exception_ptr exception_ptr_;
        std::vector<std::coroutine_handle<>> continuations_;

        SharedState() : shared_state_id_(g_shared_state_id_counter++) {
            LOG_DEBUG << "SharedStateID<void>: " << shared_state_id_ << ", Addr: " << this << " constructed." << std::endl;
        }
        ~SharedState() {
            LOG_DEBUG << "SharedStateID<void>: " << shared_state_id_ << ", Addr: " << this << " destructed. State: " << (int)current_state_
                      << ", Continuations: " << continuations_.size() << std::endl;
        }

        void set_completed() {
            LOG_DEBUG << "SharedStateID<void>: " << shared_state_id_ << " set_completed(). Current state: " << (int)current_state_ << std::endl;
            if (current_state_ == State::PENDING) {
                current_state_ = State::COMPLETED;
                LOG_DEBUG << "  Resuming " << continuations_.size() << " continuations." << std::endl;
                for (auto& h : continuations_) { if (h) {
                    LOG_DEBUG << "    Resuming continuation " << h.address() << std::endl;
                    h.resume();
                }}
                continuations_.clear();
            }
        }
        void set_exception(std::exception_ptr ex) {
            LOG_DEBUG << "SharedStateID<void>: " << shared_state_id_ << " set_exception(). Current state: " << (int)current_state_ << std::endl;
            if (current_state_ == State::PENDING) {
                exception_ptr_ = ex;
                current_state_ = State::EXCEPTION;
                LOG_DEBUG << "  Resuming " << continuations_.size() << " continuations." << std::endl;
                for (auto& h : continuations_) { if (h) {
                    LOG_DEBUG << "    Resuming continuation " << h.address() << std::endl;
                    h.resume();
                }}
                continuations_.clear();
            }
        }
        bool add_continuation_and_check_state(std::coroutine_handle<> awaiting_coro) {
            LOG_DEBUG << "SharedStateID<void>: " << shared_state_id_ << " add_continuation_and_check_state() for awaiting_coro: " << awaiting_coro.address()
                      << ". Current state: " << (int)current_state_ << std::endl;
            if (current_state_ != State::PENDING) {
                LOG_DEBUG << "  Already completed. Returning false (don't suspend)." << std::endl;
                return false;
            }
            continuations_.push_back(awaiting_coro);
            LOG_DEBUG << "  Pending. Added continuation. Returning true (suspend)." << std::endl;
            return true;
        }
        void get_value_or_rethrow() {
            LOG_DEBUG << "SharedStateID<void>: " << shared_state_id_ << " get_value_or_rethrow(). Current state: " << (int)current_state_ << std::endl;
            if (current_state_ == State::EXCEPTION) {
                LOG_DEBUG << "  Rethrowing exception." << std::endl;
                std::rethrow_exception(exception_ptr_);
            }
        }
    };

    template <typename T>
    struct SharedTaskPromise {
        uint64_t promise_id_; // Unique ID for this promise instance
        std::shared_ptr<SharedState<T>> shared_state_ptr_;

        SharedTaskPromise() : promise_id_(g_promise_id_counter++), shared_state_ptr_(std::make_shared<SharedState<T>>()) {
            LOG_DEBUG << "SharedTaskPromiseID: " << promise_id_ << ", PromiseAddr: " << this 
                      << " constructed. Owns SharedStateID: " << shared_state_ptr_->shared_state_id_
                      << ", SharedStateAddr: " << shared_state_ptr_.get() << std::endl;
        }

        SharedTask<T> get_return_object(); // Declaration

        std::suspend_never initial_suspend() noexcept {
            LOG_DEBUG << "SharedTaskPromiseID: " << promise_id_ << " initial_suspend (eager)." << std::endl;
            return {};
        }
        std::suspend_never final_suspend() noexcept {
            LOG_DEBUG << "SharedTaskPromiseID: " << promise_id_ << ", PromiseAddr: " << this
                      << " final_suspend (suspend_never). Coro frame will be destroyed. SharedState (ID "
                      << shared_state_ptr_->shared_state_id_ << ", Addr " << shared_state_ptr_.get()
                      << ") lives on via shared_ptr (use_count: " << shared_state_ptr_.use_count() << ")." << std::endl;
            return {};
        }
        void return_value(T value) {
            LOG_DEBUG << "SharedTaskPromiseID: " << promise_id_ << " return_value. Forwarding to SharedStateID: "
                      << shared_state_ptr_->shared_state_id_ << std::endl;
            shared_state_ptr_->set_value(std::move(value));
        }
        void unhandled_exception() {
            LOG_WARN << "SharedTaskPromiseID: " << promise_id_ << " unhandled_exception. Forwarding to SharedStateID: "
                     << shared_state_ptr_->shared_state_id_ << std::endl;
            shared_state_ptr_->set_exception(std::current_exception());
        }
    };

    template <>
    struct SharedTaskPromise<void> {
        uint64_t promise_id_;
        std::shared_ptr<SharedState<void>> shared_state_ptr_;

        SharedTaskPromise() : promise_id_(g_promise_id_counter++), shared_state_ptr_(std::make_shared<SharedState<void>>()) {
            LOG_DEBUG << "SharedTaskPromiseID<void>: " << promise_id_ << ", PromiseAddr: " << this 
                      << " constructed. Owns SharedStateID: " << shared_state_ptr_->shared_state_id_
                      << ", SharedStateAddr: " << shared_state_ptr_.get() << std::endl;
        }

        SharedTask<void> get_return_object(); // Declaration

        std::suspend_never initial_suspend() noexcept {
            LOG_DEBUG << "SharedTaskPromiseID<void>: " << promise_id_ << " initial_suspend (eager)." << std::endl;
            return {};
        }
        std::suspend_never final_suspend() noexcept {
            LOG_DEBUG << "SharedTaskPromiseID<void>: " << promise_id_ << ", PromiseAddr: " << this
                      << " final_suspend (suspend_never). Coro frame will be destroyed. SharedState (ID "
                      << shared_state_ptr_->shared_state_id_ << ", Addr " << shared_state_ptr_.get()
                      << ") lives on via shared_ptr (use_count: " << shared_state_ptr_.use_count() << ")." << std::endl;
            return {};
        }
        void return_void() {
            LOG_DEBUG << "SharedTaskPromiseID<void>: " << promise_id_ << " return_void. Forwarding to SharedStateID: "
                      << shared_state_ptr_->shared_state_id_ << std::endl;
            shared_state_ptr_->set_completed();
        }
        void unhandled_exception() {
            LOG_WARN << "SharedTaskPromiseID<void>: " << promise_id_ << " unhandled_exception. Forwarding to SharedStateID: "
                     << shared_state_ptr_->shared_state_id_ << std::endl;
            shared_state_ptr_->set_exception(std::current_exception());
        }
    };

} // namespace detail


template <typename T>
class SharedTask {
    std::shared_ptr<detail::SharedState<T>> state_;

public:
    SharedTask() = default; // Creates a SharedTask with null state_
    explicit SharedTask(std::shared_ptr<detail::SharedState<T>> state) : state_(std::move(state)) {
        LOG_DEBUG << "SharedTaskObj: " << this << " constructed. Points to SharedStateID: "
                  << (state_ ? state_->shared_state_id_ : -1) << ", Addr: " << (state_ ? state_.get() : nullptr) << std::endl;
    }

    // Copy/move ops
    SharedTask(const SharedTask& other) : state_(other.state_) {
        LOG_DEBUG << "SharedTaskObj: " << this << " copy constructed from SharedTaskObj: " << &other
                  << ". Points to SharedStateID: " << (state_ ? state_->shared_state_id_ : -1)
                  << " (use_count: " << state_.use_count() << ")" << std::endl;
    }
    SharedTask& operator=(const SharedTask& other) {
        LOG_DEBUG << "SharedTaskObj: " << this << " copy assigned from SharedTaskObj: " << &other << std::endl;
        if (this != &other) {
            state_ = other.state_;
            LOG_DEBUG << "  Now points to SharedStateID: " << (state_ ? state_->shared_state_id_ : -1)
                      << " (use_count: " << state_.use_count() << ")" << std::endl;
        }
        return *this;
    }
    SharedTask(SharedTask&& other) noexcept : state_(std::move(other.state_)) {
        LOG_DEBUG << "SharedTaskObj: " << this << " move constructed from SharedTaskObj: " << &other
                  << ". Points to SharedStateID: " << (state_ ? state_->shared_state_id_ : -1)
                  << ". other.state_ is now " << (other.state_ ? "valid" : "null") << std::endl;
    }
    SharedTask& operator=(SharedTask&& other) noexcept {
        LOG_DEBUG << "SharedTaskObj: " << this << " move assigned from SharedTaskObj: " << &other << std::endl;
        if (this != &other) {
            state_ = std::move(other.state_);
            LOG_DEBUG << "  Now points to SharedStateID: " << (state_ ? state_->shared_state_id_ : -1)
                      << ". other.state_ is now " << (other.state_ ? "valid" : "null") << std::endl;
        }
        return *this;
    }
     ~SharedTask() {
        LOG_DEBUG << "SharedTaskObj: " << this << " destructed. Pointed to SharedStateID: "
                  << (state_ ? state_->shared_state_id_ : -1)
                  << " (use_count before this dtor: " << state_.use_count() << ")" << std::endl;
        // shared_ptr dtor will handle decrementing use_count of state_
    }


    bool await_ready() const noexcept {
        if (!state_) {
            LOG_WARN << "SharedTaskObj: " << this << " await_ready() on null state. Returning true (ready)." << std::endl;
            return true;
        }
        bool ready = state_->current_state_ != detail::SharedState<T>::State::PENDING;
        LOG_DEBUG << "SharedTaskObj: " << this << " (SharedStateID: " << state_->shared_state_id_
                  << ") await_ready(). State: " << (int)state_->current_state_ << ". Ready: " << std::boolalpha << ready << std::endl;
        return ready;
    }

    bool await_suspend(std::coroutine_handle<> awaiting_coro) noexcept {
        if (!state_) {
            LOG_WARN << "SharedTaskObj: " << this << " await_suspend() on null state. Returning false (don't suspend)." << std::endl;
            return false; // Don't suspend if state is null
        }
        LOG_DEBUG << "SharedTaskObj: " << this << " (SharedStateID: " << state_->shared_state_id_
                  << ") await_suspend() by awaiting_coro: " << awaiting_coro.address() << std::endl;
        return state_->add_continuation_and_check_state(awaiting_coro);
    }

    T await_resume() {
        LOG_DEBUG << "SharedTaskObj: " << this << " await_resume()." << std::endl;
        if (!state_) {
            LOG_CRITICAL << "SharedTaskObj: " << this << " Awaiting a SharedTask with null state!" << std::endl;
            if constexpr (!std::is_void_v<T>) {
                 throw std::runtime_error("Awaiting a SharedTask with null state");
                 // return T{}; // Or throw, depending on desired behavior for this invalid state
            } else {
                 throw std::runtime_error("Awaiting a SharedTask<void> with null state");
                 // return;
            }
        }
        LOG_DEBUG << "  Forwarding to SharedStateID: " << state_->shared_state_id_ << " get_value_or_rethrow()." << std::endl;
        return state_->get_value_or_rethrow();
    }
};

template <>
class SharedTask<void> { // Specialization for void
    std::shared_ptr<detail::SharedState<void>> state_;
public:
    SharedTask() = default;
    explicit SharedTask(std::shared_ptr<detail::SharedState<void>> state) : state_(std::move(state)) {
        LOG_DEBUG << "SharedTaskObj<void>: " << this << " constructed. Points to SharedStateID: "
                  << (state_ ? state_->shared_state_id_ : -1) << ", Addr: " << (state_ ? state_.get() : nullptr) << std::endl;
    }
    // Copy/move ops (similar logging as Task<T>)
    SharedTask(const SharedTask& other) : state_(other.state_) {
        LOG_DEBUG << "SharedTaskObj<void>: " << this << " copy constructed from SharedTaskObj: " << &other
                  << ". Points to SharedStateID: " << (state_ ? state_->shared_state_id_ : -1)
                  << " (use_count: " << state_.use_count() << ")" << std::endl;
    }
    SharedTask& operator=(const SharedTask& other) {
        LOG_DEBUG << "SharedTaskObj<void>: " << this << " copy assigned from SharedTaskObj: " << &other << std::endl;
        if (this != &other) {
            state_ = other.state_;
            LOG_DEBUG << "  Now points to SharedStateID: " << (state_ ? state_->shared_state_id_ : -1)
                      << " (use_count: " << state_.use_count() << ")" << std::endl;
        }
        return *this;
    }
    SharedTask(SharedTask&& other) noexcept : state_(std::move(other.state_)) {
        LOG_DEBUG << "SharedTaskObj<void>: " << this << " move constructed from SharedTaskObj: " << &other
                  << ". Points to SharedStateID: " << (state_ ? state_->shared_state_id_ : -1)
                  << ". other.state_ is now " << (other.state_ ? "valid" : "null") << std::endl;
    }
    SharedTask& operator=(SharedTask&& other) noexcept {
        LOG_DEBUG << "SharedTaskObj<void>: " << this << " move assigned from SharedTaskObj: " << &other << std::endl;
        if (this != &other) {
            state_ = std::move(other.state_);
            LOG_DEBUG << "  Now points to SharedStateID: " << (state_ ? state_->shared_state_id_ : -1)
                      << ". other.state_ is now " << (other.state_ ? "valid" : "null") << std::endl;
        }
        return *this;
    }
     ~SharedTask() {
        LOG_DEBUG << "SharedTaskObj<void>: " << this << " destructed. Pointed to SharedStateID: "
                  << (state_ ? state_->shared_state_id_ : -1)
                  << " (use_count before this dtor: " << state_.use_count() << ")" << std::endl;
    }


    bool await_ready() const noexcept {
        if (!state_) {
            LOG_WARN << "SharedTaskObj<void>: " << this << " await_ready() on null state. Returning true (ready)." << std::endl;
            return true;
        }
        bool ready = state_->current_state_ != detail::SharedState<void>::State::PENDING;
        LOG_DEBUG << "SharedTaskObj<void>: " << this << " (SharedStateID: " << state_->shared_state_id_
                  << ") await_ready(). State: " << (int)state_->current_state_ << ". Ready: " << std::boolalpha << ready << std::endl;
        return ready;
    }
    bool await_suspend(std::coroutine_handle<> awaiting_coro) noexcept {
        if (!state_) {
            LOG_WARN << "SharedTaskObj<void>: " << this << " await_suspend() on null state. Returning false (don't suspend)." << std::endl;
            return false;
        }
        LOG_DEBUG << "SharedTaskObj<void>: " << this << " (SharedStateID: " << state_->shared_state_id_
                  << ") await_suspend() by awaiting_coro: " << awaiting_coro.address() << std::endl;
        return state_->add_continuation_and_check_state(awaiting_coro);
    }
    void await_resume() {
        LOG_DEBUG << "SharedTaskObj<void>: " << this << " await_resume()." << std::endl;
        if (!state_) {
            LOG_CRITICAL << "SharedTaskObj<void>: " << this << " Awaiting a SharedTask with null state!" << std::endl;
            throw std::runtime_error("Awaiting a SharedTask<void> with null state");
            // return;
        }
        LOG_DEBUG << "  Forwarding to SharedStateID: " << state_->shared_state_id_ << " get_value_or_rethrow()." << std::endl;
        state_->get_value_or_rethrow();
    }
};

// Definitions for SharedTaskPromise::get_return_object
namespace detail {
    template<typename T>
    SharedTask<T> SharedTaskPromise<T>::get_return_object() {
        LOG_DEBUG << "SharedTaskPromiseID: " << promise_id_ << ", PromiseAddr: " << this
                  << " get_return_object() -> SharedTask for SharedStateID: " << shared_state_ptr_->shared_state_id_ << std::endl;
        return SharedTask<T>(shared_state_ptr_);
    }

    inline SharedTask<void> SharedTaskPromise<void>::get_return_object() {
        LOG_DEBUG << "SharedTaskPromiseID<void>: " << promise_id_ << ", PromiseAddr: " << this
                  << " get_return_object() -> SharedTask<void> for SharedStateID: " << shared_state_ptr_->shared_state_id_ << std::endl;
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

// Undefine logging macros if they are not meant to be used outside this file
// #undef LOG_PREFIX_WIDTH
// #undef LOG_DEBUG
// #undef LOG_WARN
// #undef LOG_CRITICAL