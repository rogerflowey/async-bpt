#pragma once

#include <atomic> // For unique IDs
#include <coroutine>
#include <exception> // For std::terminate, std::current_exception, std::exception_ptr
#include <functional>
#include <iomanip>   // For std::setw for aligned logging
#include <iostream>  // For std::cout, std::cerr, std::endl, std::left
#include <memory>    // For std::shared_ptr
#include <optional>  // For SharedState value
#include <stdexcept> // For std::runtime_error
#include <string>    // For std::to_string
#include <utility>   // For std::exchange
#include <vector>    // For SharedState continuations
//#define TASK_DEBUG

#define LOG_PREFIX_WIDTH 35

#define TASK_DEBUG false

#define LOG_DEBUG if(TASK_DEBUG) std::cout << std::left << std::setw(LOG_PREFIX_WIDTH) << __func__ << " | "

#define LOG_WARN if(TASK_DEBUG) std::cerr << std::left << std::setw(LOG_PREFIX_WIDTH) << __func__ << " | WARN: "
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
        LOG_DEBUG << "PromiseID: " << promise_id_ << ", PromiseAddr: " << static_cast<void*>(this) << " constructed." << std::endl;
    }

    PromiseBase(const PromiseBase&) = delete;
    PromiseBase& operator=(const PromiseBase&) = delete;
    PromiseBase(PromiseBase&&) = delete;
    PromiseBase& operator=(PromiseBase&&) = delete;

    ~PromiseBase() {
        LOG_DEBUG << "PromiseID: " << promise_id_ << ", PromiseAddr: " << static_cast<void*>(this) << " destructed." << std::endl;
    }

    std::suspend_never initial_suspend() noexcept {
        LOG_DEBUG << "PromiseID: " << promise_id_ << ", PromiseAddr: " << static_cast<void*>(this) << " initial_suspend (eager)." << std::endl;
        return {};
    }

    struct FinalAwaiter {
        PromiseBase<T>* promise_ptr_;
        bool await_ready() const noexcept {
            LOG_DEBUG << "PromiseID: " << promise_ptr_->promise_id_ << " FinalAwaiter::await_ready. Always false to suspend." << std::endl;
            return false;
        }
        std::coroutine_handle<> await_suspend(std::coroutine_handle<> /*h*/) const noexcept {
            LOG_DEBUG << "PromiseID: " << promise_ptr_->promise_id_ << " FinalAwaiter::await_suspend. Continuation: "
                      << (promise_ptr_->continuation_ ? promise_ptr_->continuation_.address() : nullptr) << std::endl;
            if (promise_ptr_->continuation_) {
                LOG_DEBUG << "PromiseID: " << promise_ptr_->promise_id_ << " resuming continuation " << promise_ptr_->continuation_.address() << std::endl;
                return promise_ptr_->continuation_; // Resume continuation
            }
            LOG_DEBUG << "PromiseID: " << promise_ptr_->promise_id_ << " no continuation to resume. Suspending (no-op)." << std::endl;
            return std::noop_coroutine(); // No continuation, suspend (frame will be destroyed by Task destructor)
        }
        void await_resume() const noexcept {
            LOG_DEBUG << "PromiseID: " << promise_ptr_->promise_id_ << " FinalAwaiter::await_resume (should not be called if await_suspend returns a handle)." << std::endl;
        }
    };


    FinalAwaiter final_suspend() noexcept {
      LOG_DEBUG << "PromiseID: " << promise_id_ << ", PromiseAddr: " << static_cast<void*>(this) << " final_suspend. Will return FinalAwaiter." << std::endl;
      return {this};
    }

    void unhandled_exception() {
      exception_ptr_ = std::current_exception();
      LOG_WARN << "PromiseID: " << promise_id_ << ", PromiseAddr: " << static_cast<void*>(this) << " unhandled_exception captured."<<exception_ptr_.__cxa_exception_type() << std::endl;
    }
  };

  template<typename T>
  struct PromiseWithValue : PromiseBase<T> {
    std::optional<T> result_opt_;

    Task<T> get_return_object();
    void return_value(T value) {
        LOG_DEBUG << "PromiseID: " << this->promise_id_ << ", PromiseAddr: " << static_cast<void*>(this) << " return_value set." << std::endl;
        result_opt_.emplace(std::move(value));
    }
    T get_result() {
        LOG_DEBUG << "PromiseID: " << this->promise_id_ << ", PromiseAddr: " << static_cast<void*>(this) << " get_result called." << std::endl;
        if (this->exception_ptr_) {
            LOG_DEBUG << "PromiseID: " << this->promise_id_ << " rethrowing exception." << std::endl;
            std::rethrow_exception(this->exception_ptr_);
        }
        if (!result_opt_.has_value()) {
            LOG_CRITICAL << "PromiseID: " << this->promise_id_ << " result not set before get_result!" << std::endl;
            throw std::runtime_error("Promise result not set before get_result for PromiseID: " + std::to_string(this->promise_id_));
        }
        T result = std::move(result_opt_.value());
        result_opt_.reset();
        return result;
    }
  };

  struct PromiseForVoidSpecial : public PromiseBase<void> {
    Task<void> get_return_object();
    void return_void() {
      LOG_DEBUG << "PromiseID: " << this->promise_id_ << ", PromiseAddr: " << static_cast<void*>(this) << " return_void." << std::endl;
    }
    void get_result() {
        LOG_DEBUG << "PromiseID: " << this->promise_id_ << ", PromiseAddr: " << static_cast<void*>(this) << " get_result (void) called." << std::endl;
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
        LOG_DEBUG << "TaskObj<T>: " << static_cast<void*>(this) << " constructed. Manages Handle: " << (handle ? handle.address() : nullptr)
                  << ", PromiseID: " << (handle ? handle.promise().promise_id_ : -1ull)
                  << ", PromiseAddr: " << (handle ? static_cast<void*>(&handle.promise()) : nullptr) << std::endl;
    }

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    Task(Task&& other) noexcept : handle(std::exchange(other.handle, nullptr)) {
        LOG_DEBUG << "TaskObj<T>: " << static_cast<void*>(this) << " move constructed from TaskObj: " << static_cast<void*>(&other)
                  << ". New Handle: " << (handle ? handle.address() : nullptr)
                  << (handle ? " (PromiseID: " + std::to_string(handle.promise().promise_id_) + ")" : "")
                  << ". Old TaskObj " << static_cast<void*>(&other) << " now has null handle." << std::endl;
    }
    Task& operator=(Task&& other) noexcept {
        LOG_DEBUG << "TaskObj<T>: " << static_cast<void*>(this) << " move assigned from TaskObj: " << static_cast<void*>(&other) << std::endl;
        if (this != &other) {
            if (handle) {
                 uint64_t old_promise_id = handle.promise().promise_id_;
                 bool old_is_done = handle.done();
                 //LOG_WARN  << "TaskObj<T>: " << static_cast<void*>(this) << " (move assignment) destroying its existing Handle: "
                 //          << handle.address() << " (PromiseID: " << old_promise_id << ", Done: " << std::boolalpha << old_is_done << ")" << std::endl;
                 if (!old_is_done) {
                    LOG_CRITICAL << "TaskObj<T>: " << static_cast<void*>(this) << " (move assignment) destroying UNDONE Handle: "
                                 << handle.address() << " (PromiseID: " << old_promise_id << ")" << std::endl;
                 }
                 handle.destroy();
            }
            handle = std::exchange(other.handle, nullptr);
            LOG_DEBUG << "  New Handle: " << (handle ? handle.address() : nullptr)
                      << (handle ? " (PromiseID: " + std::to_string(handle.promise().promise_id_) + ")" : "")
                      << ". Old TaskObj " << static_cast<void*>(&other) << " now has null handle." << std::endl;
        }
        return *this;
    }


    ~Task() {
        if (handle) {
            bool is_done = handle.done();
            auto* promise_ptr = static_cast<void*>(&handle.promise());
            uint64_t promise_id = handle.promise().promise_id_;

            LOG_DEBUG << "TaskObj<T>: " << static_cast<void*>(this) << " destroying. Manages Handle: " << handle.address()
                      << ", PromiseID: " << promise_id
                      << ", PromiseAddr: " << promise_ptr
                      << ", Done: " << std::boolalpha << is_done << std::endl;

            if (!is_done) {
                LOG_CRITICAL << "TaskObj<T>: " << static_cast<void*>(this) << " destroying Handle " << handle.address()
                             << " (PromiseID: " << promise_id << ", PromiseAddr: " << promise_ptr << ")"
                             << " for a coroutine that is NOT DONE." << std::endl;
            }
            LOG_DEBUG << "  Calling handle.destroy() for Handle: " << handle.address() << " (PromiseID: " << promise_id << ")" << std::endl;
            handle.destroy();
            LOG_DEBUG << "  handle.destroy() returned for Handle: " << handle.address() << " (PromiseID: " << promise_id << ")" << std::endl;

        } else {
            LOG_DEBUG << "TaskObj<T>: " << static_cast<void*>(this) << " destroying. Handle is null (normal for moved-from Task)." << std::endl;
        }
    }

    bool await_ready() const noexcept {
      bool ready = !handle || handle.done();
      LOG_DEBUG << "TaskObj<T>: " << static_cast<const void*>(this) << " await_ready() called. Handle: " << (handle ? handle.address() : nullptr)
                << (handle ? " (PromiseID: " + std::to_string(handle.promise().promise_id_) + ", Done: " + (handle.done() ? "true" : "false") + ")" : "")
                << ". Returning: " << std::boolalpha << ready << std::endl;
      return ready;
    }

    bool await_suspend(std::coroutine_handle<> awaiting_coroutine) noexcept {
      LOG_DEBUG << "TaskObj<T>: " << static_cast<void*>(this) << " await_suspend() by awaiting_coro: " << awaiting_coroutine.address()
                << ". My Handle: " << handle.address() << " (PromiseID: " << handle.promise().promise_id_ << ")" << std::endl;

      handle.promise().continuation_ = awaiting_coroutine;
      LOG_DEBUG << "  Set continuation. My Handle " << handle.address() << " is " << (handle.done() ? "done" : "not done")
                << ". Returning true (suspend awaiter, will be resumed by my final_suspend)." << std::endl;
      return true;
    }

    T await_resume() {
      LOG_DEBUG << "TaskObj<T>: " << static_cast<void*>(this) << " await_resume() called. My Handle: " << (handle ? handle.address() : nullptr)
                << (handle ? " (PromiseID: " + std::to_string(handle.promise().promise_id_) + ")" : "") << std::endl;
      if (!handle) {
        LOG_CRITICAL << "TaskObj<T>: " << static_cast<void*>(this) << " Awaiting a moved-from or null Task!" << std::endl;
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
        LOG_DEBUG << "TaskObj<void>: " << static_cast<void*>(this) << " constructed. Manages Handle: " << (handle ? handle.address() : nullptr)
                  << ", PromiseID: " << (handle ? handle.promise().promise_id_ : -1ull)
                  << ", PromiseAddr: " << (handle ? static_cast<void*>(&handle.promise()) : nullptr) << std::endl;
    }

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    Task(Task&& other) noexcept : handle(std::exchange(other.handle, nullptr)) {
        LOG_DEBUG << "TaskObj<void>: " << static_cast<void*>(this) << " move constructed from TaskObj: " << static_cast<void*>(&other)
                  << ". New Handle: " << (handle ? handle.address() : nullptr)
                  << (handle ? " (PromiseID: " + std::to_string(handle.promise().promise_id_) + ")" : "")
                  << ". Old TaskObj " << static_cast<void*>(&other) << " now has null handle." << std::endl;
    }
     Task& operator=(Task&& other) noexcept {
        LOG_DEBUG << "TaskObj<void>: " << static_cast<void*>(this) << " move assigned from TaskObj: " << static_cast<void*>(&other) << std::endl;
        if (this != &other) {
            if (handle) {
                 uint64_t old_promise_id = handle.promise().promise_id_;
                 bool old_is_done = handle.done();
                 LOG_WARN  << "TaskObj<void>: " << static_cast<void*>(this) << " (move assignment) destroying its existing Handle: "
                           << handle.address() << " (PromiseID: " << old_promise_id << ", Done: " << std::boolalpha << old_is_done << ")" << std::endl;
                 if (!old_is_done) {
                    LOG_CRITICAL << "TaskObj<void>: " << static_cast<void*>(this) << " (move assignment) destroying UNDONE Handle: "
                                 << handle.address() << " (PromiseID: " << old_promise_id << ")" << std::endl;
                 }
                 handle.destroy();
            }
            handle = std::exchange(other.handle, nullptr);
            LOG_DEBUG << "  New Handle: " << (handle ? handle.address() : nullptr)
                      << (handle ? " (PromiseID: " + std::to_string(handle.promise().promise_id_) + ")" : "")
                      << ". Old TaskObj " << static_cast<void*>(&other) << " now has null handle." << std::endl;
        }
        return *this;
    }

    ~Task() {
        if (handle) {
            bool is_done = handle.done();
            auto* promise_ptr = static_cast<void*>(&handle.promise());
            uint64_t promise_id = handle.promise().promise_id_;

            LOG_DEBUG << "TaskObj<void>: " << static_cast<void*>(this) << " destroying. Manages Handle: " << handle.address()
                      << ", PromiseID: " << promise_id
                      << ", PromiseAddr: " << promise_ptr
                      << ", Done: " << std::boolalpha << is_done << std::endl;

            if (!is_done) {
                 LOG_CRITICAL << "TaskObj<void>: " << static_cast<void*>(this) << " destroying Handle " << handle.address()
                             << " (PromiseID: " << promise_id << ", PromiseAddr: " << promise_ptr << ")"
                             << " for a coroutine that is NOT DONE." << std::endl;
            }

            LOG_DEBUG << "  Calling handle.destroy() for Handle: " << handle.address() << " (PromiseID: " << promise_id << ")" << std::endl;
            handle.destroy();
            LOG_DEBUG << "  handle.destroy() returned for Handle: " << handle.address() << " (PromiseID: " << promise_id << ")" << std::endl;
        } else {
            LOG_DEBUG << "TaskObj<void>: " << static_cast<void*>(this) << " destroying. Handle is null (normal for moved-from Task)." << std::endl;
        }
    }

    bool await_ready() const noexcept {
      bool ready = !handle || handle.done();
      LOG_DEBUG << "TaskObj<void>: " << static_cast<const void*>(this) << " await_ready() called. Handle: " << (handle ? handle.address() : nullptr)
                << (handle ? " (PromiseID: " + std::to_string(handle.promise().promise_id_) + ", Done: " + (handle.done() ? "true" : "false") + ")" : "")
                << ". Returning: " << std::boolalpha << ready << std::endl;
      return ready;
    }

    bool await_suspend(std::coroutine_handle<> awaiting_coroutine) noexcept {
      LOG_DEBUG << "TaskObj<void>: " << static_cast<void*>(this) << " await_suspend() by awaiting_coro: " << awaiting_coroutine.address()
                << ". My Handle: " << handle.address() << " (PromiseID: " << handle.promise().promise_id_ << ")" << std::endl;

      handle.promise().continuation_ = awaiting_coroutine;
      LOG_DEBUG << "  Set continuation. My Handle " << handle.address() << " is " << (handle.done() ? "done" : "not done")
                << ". Returning true (suspend awaiter, will be resumed by my final_suspend)." << std::endl;
      return true;
    }

    void await_resume() {
      LOG_DEBUG << "TaskObj<void>: " << static_cast<void*>(this) << " await_resume() called. My Handle: " << (handle ? handle.address() : nullptr)
                << (handle ? " (PromiseID: " + std::to_string(handle.promise().promise_id_) + ")" : "") << std::endl;
      if (!handle) {
        LOG_CRITICAL << "TaskObj<void>: " << static_cast<void*>(this) << " Awaiting a moved-from or null Task!" << std::endl;
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
    LOG_DEBUG << "PromiseID: " << this->promise_id_ << ", PromiseAddr: " << static_cast<void*>(this)
              << " get_return_object() -> Task<T> for Handle: " << coro_handle.address() << std::endl;
    return Task<T>{coro_handle};
  }

  inline Task<void> PromiseForVoidSpecial::get_return_object() {
    auto coro_handle = std::coroutine_handle<PromiseForVoidSpecial>::from_promise(*this);
    LOG_DEBUG << "PromiseID: " << this->promise_id_ << ", PromiseAddr: " << static_cast<void*>(this)
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
            LOG_DEBUG << "SharedStateID: " << shared_state_id_ << ", Addr: " << static_cast<void*>(this) << " constructed." << std::endl;
        }
        ~SharedState() {
            LOG_DEBUG << "SharedStateID: " << shared_state_id_ << ", Addr: " << static_cast<void*>(this) << " destructed. State: " << (int)current_state_
                      << ", Continuations pending: " << continuations_.size() << std::endl;
        }


        void set_value(T val) {
            LOG_DEBUG << "SharedStateID: " << shared_state_id_ << " set_value(). Current state: " << (int)current_state_ << std::endl;
            if (current_state_ == State::PENDING) {
                value_ = std::move(val);
                current_state_ = State::VALUE;
                LOG_DEBUG << "  Resuming " << continuations_.size() << " continuations." << std::endl;
                for (int i=continuations_.size()-1;i>=0;--i) {
                    auto &h = continuations_[i];
                    if (h) {
                        LOG_DEBUG << "    Resuming continuation " << h.address() << std::endl;
                        h.resume();
                    }
                }
                continuations_.clear();
            } else {
                LOG_WARN << "SharedStateID: " << shared_state_id_ << " set_value() called but state is not PENDING (" << (int)current_state_ << ")" << std::endl;
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
            } else {
                 LOG_WARN << "SharedStateID: " << shared_state_id_ << " set_exception() called but state is not PENDING (" << (int)current_state_ << ")" << std::endl;
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
            LOG_DEBUG << "  Pending. Added continuation (total: " << continuations_.size() << "). Returning true (suspend)." << std::endl;
            return true;
        }
        T get_value_or_rethrow() {
            LOG_DEBUG << "SharedStateID: " << shared_state_id_ << " get_value_or_rethrow(). Current state: " << (int)current_state_ << std::endl;
            if (current_state_ == State::EXCEPTION) {
                LOG_DEBUG << "  Rethrowing exception." << std::endl;
                std::rethrow_exception(exception_ptr_);
            }
            if (current_state_ != State::VALUE || !value_) {
                 LOG_CRITICAL << "SharedStateID: " << shared_state_id_ << " get_value_or_rethrow() called but state is not VALUE or value_ is not set! State: " << (int)current_state_ << std::endl;
                 throw std::runtime_error("SharedState value not set and no exception for SharedStateID: " + std::to_string(shared_state_id_));
            }
            return *value_;
        }
    };

    template <>
    struct SharedState<void> {
        uint64_t shared_state_id_;
        enum class State { PENDING, COMPLETED, EXCEPTION } current_state_ = State::PENDING;
        std::exception_ptr exception_ptr_;
        std::vector<std::coroutine_handle<>> continuations_;

        SharedState() : shared_state_id_(g_shared_state_id_counter++) {
            LOG_DEBUG << "SharedStateID<void>: " << shared_state_id_ << ", Addr: " << static_cast<void*>(this) << " constructed." << std::endl;
        }
        ~SharedState() {
            LOG_DEBUG << "SharedStateID<void>: " << shared_state_id_ << ", Addr: " << static_cast<void*>(this) << " destructed. State: " << (int)current_state_
                      << ", Continuations pending: " << continuations_.size() << std::endl;
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
            } else {
                LOG_WARN << "SharedStateID<void>: " << shared_state_id_ << " set_completed() called but state is not PENDING (" << (int)current_state_ << ")" << std::endl;
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
            } else {
                LOG_WARN << "SharedStateID<void>: " << shared_state_id_ << " set_exception() called but state is not PENDING (" << (int)current_state_ << ")" << std::endl;
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
            LOG_DEBUG << "  Pending. Added continuation (total: " << continuations_.size() << "). Returning true (suspend)." << std::endl;
            return true;
        }
        void get_value_or_rethrow() {
            LOG_DEBUG << "SharedStateID<void>: " << shared_state_id_ << " get_value_or_rethrow(). Current state: " << (int)current_state_ << std::endl;
            if (current_state_ == State::EXCEPTION) {
                LOG_DEBUG << "  Rethrowing exception." << std::endl;
                std::rethrow_exception(exception_ptr_);
            }
            if (current_state_ != State::COMPLETED) {
                 LOG_CRITICAL << "SharedStateID<void>: " << shared_state_id_ << " get_value_or_rethrow() called but state is not COMPLETED and no exception! State: " << (int)current_state_ << std::endl;
                 throw std::runtime_error("SharedState<void> not completed and no exception for SharedStateID: " + std::to_string(shared_state_id_));
            }
        }
    };

    template <typename T>
    struct SharedTaskPromise {
        uint64_t promise_id_;
        std::shared_ptr<SharedState<T>> shared_state_ptr_;

        SharedTaskPromise() : promise_id_(g_promise_id_counter++), shared_state_ptr_(std::make_shared<SharedState<T>>()) {
            LOG_DEBUG << "SharedTaskPromiseID: " << promise_id_ << ", PromiseAddr: " << static_cast<void*>(this)
                      << " constructed. Owns SharedStateID: " << shared_state_ptr_->shared_state_id_
                      << ", SharedStateAddr: " << static_cast<void*>(shared_state_ptr_.get()) << std::endl;
        }
        ~SharedTaskPromise() {
             LOG_DEBUG << "SharedTaskPromiseID: " << promise_id_ << ", PromiseAddr: " << static_cast<void*>(this)
                      << " destructed. SharedStateID: " << shared_state_ptr_->shared_state_id_
                      << " (use_count: " << shared_state_ptr_.use_count() << ")" << std::endl;
        }


        SharedTask<T> get_return_object();

        std::suspend_never initial_suspend() noexcept {
            LOG_DEBUG << "SharedTaskPromiseID: " << promise_id_ << " initial_suspend (eager)." << std::endl;
            return {};
        }
        std::suspend_never final_suspend() noexcept {
            LOG_DEBUG << "SharedTaskPromiseID: " << promise_id_ << ", PromiseAddr: " << static_cast<void*>(this)
                      << " final_suspend (suspend_never). Coro frame will be destroyed. SharedState (ID "
                      << shared_state_ptr_->shared_state_id_ << ", Addr " << static_cast<void*>(shared_state_ptr_.get())
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
            LOG_DEBUG << "SharedTaskPromiseID<void>: " << promise_id_ << ", PromiseAddr: " << static_cast<void*>(this)
                      << " constructed. Owns SharedStateID: " << shared_state_ptr_->shared_state_id_
                      << ", SharedStateAddr: " << static_cast<void*>(shared_state_ptr_.get()) << std::endl;
        }
        ~SharedTaskPromise() {
             LOG_DEBUG << "SharedTaskPromiseID<void>: " << promise_id_ << ", PromiseAddr: " << static_cast<void*>(this)
                      << " destructed. SharedStateID: " << shared_state_ptr_->shared_state_id_
                      << " (use_count: " << shared_state_ptr_.use_count() << ")" << std::endl;
        }

        SharedTask<void> get_return_object();

        std::suspend_never initial_suspend() noexcept {
            LOG_DEBUG << "SharedTaskPromiseID<void>: " << promise_id_ << " initial_suspend (eager)." << std::endl;
            return {};
        }
        std::suspend_never final_suspend() noexcept {
            LOG_DEBUG << "SharedTaskPromiseID<void>: " << promise_id_ << ", PromiseAddr: " << static_cast<void*>(this)
                      << " final_suspend (suspend_never). Coro frame will be destroyed. SharedState (ID "
                      << shared_state_ptr_->shared_state_id_ << ", Addr " << static_cast<void*>(shared_state_ptr_.get())
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
    using promise_type = detail::SharedTaskPromise<T>;

    SharedTask() = default;
    explicit SharedTask(std::shared_ptr<detail::SharedState<T>> state) : state_(std::move(state)) {
        LOG_DEBUG << "SharedTaskObj<T>: " << static_cast<void*>(this) << " constructed. Points to SharedStateID: "
                  << (state_ ? state_->shared_state_id_ : -1ull) << ", Addr: " << (state_ ? static_cast<void*>(state_.get()) : nullptr)
                  << " (use_count: " << (state_ ? state_.use_count() : 0) << ")" << std::endl;
    }

    SharedTask(const SharedTask& other) : state_(other.state_) {
        LOG_DEBUG << "SharedTaskObj<T>: " << static_cast<void*>(this) << " copy constructed from SharedTaskObj: " << static_cast<const void*>(&other)
                  << ". Points to SharedStateID: " << (state_ ? state_->shared_state_id_ : -1ull)
                  << " (use_count: " << (state_ ? state_.use_count() : 0) << ")" << std::endl;
    }
    SharedTask& operator=(const SharedTask& other) {
        LOG_DEBUG << "SharedTaskObj<T>: " << static_cast<void*>(this) << " copy assigned from SharedTaskObj: " << static_cast<const void*>(&other) << std::endl;
        if (this != &other) {
            state_ = other.state_;
            LOG_DEBUG << "  Now points to SharedStateID: " << (state_ ? state_->shared_state_id_ : -1ull)
                      << " (use_count: " << (state_ ? state_.use_count() : 0) << ")" << std::endl;
        }
        return *this;
    }
    SharedTask(SharedTask&& other) noexcept : state_(std::move(other.state_)) {
        LOG_DEBUG << "SharedTaskObj<T>: " << static_cast<void*>(this) << " move constructed from SharedTaskObj: " << static_cast<void*>(&other)
                  << ". Points to SharedStateID: " << (state_ ? state_->shared_state_id_ : -1ull)
                  << ". other.state_ is now " << (other.state_ ? "valid" : "null")
                  << " (use_count after move: " << (state_ ? state_.use_count() : 0) << ")" << std::endl;
    }
    SharedTask& operator=(SharedTask&& other) noexcept {
        LOG_DEBUG << "SharedTaskObj<T>: " << static_cast<void*>(this) << " move assigned from SharedTaskObj: " << static_cast<void*>(&other) << std::endl;
        if (this != &other) {
            state_ = std::move(other.state_);
            LOG_DEBUG << "  Now points to SharedStateID: " << (state_ ? state_->shared_state_id_ : -1ull)
                      << ". other.state_ is now " << (other.state_ ? "valid" : "null")
                      << " (use_count after move: " << (state_ ? state_.use_count() : 0) << ")" << std::endl;
        }
        return *this;
    }
     ~SharedTask() {
        LOG_DEBUG << "SharedTaskObj<T>: " << static_cast<void*>(this) << " destructed. Pointed to SharedStateID: "
                  << (state_ ? state_->shared_state_id_ : -1ull)
                  << " (use_count before this dtor affects it: " << (state_ ? state_.use_count() : 0) << ")" << std::endl;
    }


    bool await_ready() const noexcept {
        if (!state_) {
            LOG_WARN << "SharedTaskObj<T>: " << static_cast<const void*>(this) << " await_ready() on null state. Returning true (ready)." << std::endl;
            return true;
        }
        bool ready = state_->current_state_ != detail::SharedState<T>::State::PENDING;
        LOG_DEBUG << "SharedTaskObj<T>: " << static_cast<const void*>(this) << " (SharedStateID: " << state_->shared_state_id_
                  << ") await_ready(). State: " << (int)state_->current_state_ << ". Returning: " << std::boolalpha << ready << std::endl;
        return ready;
    }

    bool await_suspend(std::coroutine_handle<> awaiting_coro) noexcept {
        if (!state_) {
            LOG_WARN << "SharedTaskObj<T>: " << static_cast<void*>(this) << " await_suspend() on null state. Returning false (don't suspend)." << std::endl;
            return false;
        }
        LOG_DEBUG << "SharedTaskObj<T>: " << static_cast<void*>(this) << " (SharedStateID: " << state_->shared_state_id_
                  << ") await_suspend() by awaiting_coro: " << awaiting_coro.address() << std::endl;
        bool should_suspend = state_->add_continuation_and_check_state(awaiting_coro);
        LOG_DEBUG << "  SharedState::add_continuation_and_check_state returned: " << std::boolalpha << should_suspend << ". Returning this value." << std::endl;
        return should_suspend;
    }

    T await_resume() {
        LOG_DEBUG << "SharedTaskObj<T>: " << static_cast<void*>(this) << " await_resume()." << std::endl;
        if (!state_) {
            LOG_CRITICAL << "SharedTaskObj<T>: " << static_cast<void*>(this) << " Awaiting a SharedTask with null state!" << std::endl;
            throw std::runtime_error("Awaiting a SharedTask with null state");
        }
        LOG_DEBUG << "  Forwarding to SharedStateID: " << state_->shared_state_id_ << " get_value_or_rethrow()." << std::endl;
        return state_->get_value_or_rethrow();
    }
};

template <>
class SharedTask<void> {
    std::shared_ptr<detail::SharedState<void>> state_;
public:
    using promise_type = detail::SharedTaskPromise<void>;

    SharedTask() = default;
    explicit SharedTask(std::shared_ptr<detail::SharedState<void>> state) : state_(std::move(state)) {
        LOG_DEBUG << "SharedTaskObj<void>: " << static_cast<void*>(this) << " constructed. Points to SharedStateID: "
                  << (state_ ? state_->shared_state_id_ : -1ull) << ", Addr: " << (state_ ? static_cast<void*>(state_.get()) : nullptr)
                  << " (use_count: " << (state_ ? state_.use_count() : 0) << ")" << std::endl;
    }
    SharedTask(const SharedTask& other) : state_(other.state_) {
        LOG_DEBUG << "SharedTaskObj<void>: " << static_cast<void*>(this) << " copy constructed from SharedTaskObj: " << static_cast<const void*>(&other)
                  << ". Points to SharedStateID: " << (state_ ? state_->shared_state_id_ : -1ull)
                  << " (use_count: " << (state_ ? state_.use_count() : 0) << ")" << std::endl;
    }
    SharedTask& operator=(const SharedTask& other) {
        LOG_DEBUG << "SharedTaskObj<void>: " << static_cast<void*>(this) << " copy assigned from SharedTaskObj: " << static_cast<const void*>(&other) << std::endl;
        if (this != &other) {
            state_ = other.state_;
            LOG_DEBUG << "  Now points to SharedStateID: " << (state_ ? state_->shared_state_id_ : -1ull)
                      << " (use_count: " << (state_ ? state_.use_count() : 0) << ")" << std::endl;
        }
        return *this;
    }
    SharedTask(SharedTask&& other) noexcept : state_(std::move(other.state_)) {
        LOG_DEBUG << "SharedTaskObj<void>: " << static_cast<void*>(this) << " move constructed from SharedTaskObj: " << static_cast<void*>(&other)
                  << ". Points to SharedStateID: " << (state_ ? state_->shared_state_id_ : -1ull)
                  << ". other.state_ is now " << (other.state_ ? "valid" : "null")
                  << " (use_count after move: " << (state_ ? state_.use_count() : 0) << ")" << std::endl;
    }
    SharedTask& operator=(SharedTask&& other) noexcept {
        LOG_DEBUG << "SharedTaskObj<void>: " << static_cast<void*>(this) << " move assigned from SharedTaskObj: " << static_cast<void*>(&other) << std::endl;
        if (this != &other) {
            state_ = std::move(other.state_);
            LOG_DEBUG << "  Now points to SharedStateID: " << (state_ ? state_->shared_state_id_ : -1ull)
                      << ". other.state_ is now " << (other.state_ ? "valid" : "null")
                      << " (use_count after move: " << (state_ ? state_.use_count() : 0) << ")" << std::endl;
        }
        return *this;
    }
     ~SharedTask() {
        LOG_DEBUG << "SharedTaskObj<void>: " << static_cast<void*>(this) << " destructed. Pointed to SharedStateID: "
                  << (state_ ? state_->shared_state_id_ : -1ull)
                  << " (use_count before this dtor affects it: " << (state_ ? state_.use_count() : 0) << ")" << std::endl;
    }


    bool await_ready() const noexcept {
        if (!state_) {
            LOG_WARN << "SharedTaskObj<void>: " << static_cast<const void*>(this) << " await_ready() on null state. Returning true (ready)." << std::endl;
            return true;
        }
        bool ready = state_->current_state_ != detail::SharedState<void>::State::PENDING;
        LOG_DEBUG << "SharedTaskObj<void>: " << static_cast<const void*>(this) << " (SharedStateID: " << state_->shared_state_id_
                  << ") await_ready(). State: " << (int)state_->current_state_ << ". Returning: " << std::boolalpha << ready << std::endl;
        return ready;
    }
    bool await_suspend(std::coroutine_handle<> awaiting_coro) noexcept {
        if (!state_) {
            LOG_WARN << "SharedTaskObj<void>: " << static_cast<void*>(this) << " await_suspend() on null state. Returning false (don't suspend)." << std::endl;
            return false;
        }
        LOG_DEBUG << "SharedTaskObj<void>: " << static_cast<void*>(this) << " (SharedStateID: " << state_->shared_state_id_
                  << ") await_suspend() by awaiting_coro: " << awaiting_coro.address() << std::endl;
        bool should_suspend = state_->add_continuation_and_check_state(awaiting_coro);
        LOG_DEBUG << "  SharedState::add_continuation_and_check_state returned: " << std::boolalpha << should_suspend << ". Returning this value." << std::endl;
        return should_suspend;
    }
    void await_resume() {
        LOG_DEBUG << "SharedTaskObj<void>: " << static_cast<void*>(this) << " await_resume()." << std::endl;
        if (!state_) {
            LOG_CRITICAL << "SharedTaskObj<void>: " << static_cast<void*>(this) << " Awaiting a SharedTask with null state!" << std::endl;
            throw std::runtime_error("Awaiting a SharedTask<void> with null state");
        }
        LOG_DEBUG << "  Forwarding to SharedStateID: " << state_->shared_state_id_ << " get_value_or_rethrow()." << std::endl;
        state_->get_value_or_rethrow();
    }
};

// Definitions for SharedTaskPromise::get_return_object
namespace detail {
    template<typename T>
    SharedTask<T> SharedTaskPromise<T>::get_return_object() {
        LOG_DEBUG << "SharedTaskPromiseID: " << promise_id_ << ", PromiseAddr: " << static_cast<void*>(this)
                  << " get_return_object() -> SharedTask<T> for SharedStateID: " << shared_state_ptr_->shared_state_id_ << std::endl;
        return SharedTask<T>(shared_state_ptr_);
    }

    inline SharedTask<void> SharedTaskPromise<void>::get_return_object() {
        LOG_DEBUG << "SharedTaskPromiseID<void>: " << promise_id_ << ", PromiseAddr: " << static_cast<void*>(this)
                  << " get_return_object() -> SharedTask<void> for SharedStateID: " << shared_state_ptr_->shared_state_id_ << std::endl;
        return SharedTask<void>(shared_state_ptr_);
    }
} // namespace detail

    template <typename T>
class CriteriaVariable {
public:
    using CriteriaFunc = std::function<bool(const T &)>;

private:
    T value_;
    CriteriaFunc criteria_func_;
    std::vector<std::coroutine_handle<>> waiting_coroutines_;
    bool is_evaluating_criteria_ = false; // Prevents re-entrant evaluation during coroutine resumption

public:
    /**
     * @brief Constructs a CriteriaVariable.
     * @param initial_value The initial value to hold.
     * @param criteria A function that takes a const reference to the value and returns true if the criteria are met.
     */
    template<typename U = T> // SFINAE-friendly for perfect forwarding of initial_value
    CriteriaVariable(U&& initial_value, CriteriaFunc criteria)
        : value_(std::forward<U>(initial_value)), criteria_func_(std::move(criteria)) {
        LOG_DEBUG<<"Constructed. Initial criteria met: " << std::boolalpha << (criteria_func_ ? criteria_func_(value_) : false);
    }

    // Deleted copy/move constructors and assignment operators.
    // If move semantics are needed, careful handling of waiting_coroutines_ is required.
    // Typically, these objects are stack-allocated within a coroutine or managed via shared_ptr.
    CriteriaVariable(const CriteriaVariable&) = delete;
    CriteriaVariable& operator=(const CriteriaVariable&) = delete;
    CriteriaVariable(CriteriaVariable&&) = delete; 
    CriteriaVariable& operator=(CriteriaVariable&&) = delete;

    /**
     * @brief Proxy object for modifying the managed value.
     *
     * Returned by `operator->()` and `operator*()` of `CriteriaVariable`.
     * When this proxy is destructed (at the end of the full expression where
     * it was created), it triggers the evaluation of the criteria on the
     * `CriteriaVariable` object.
     */
    class ValueMutatorProxy {
    public:
        ValueMutatorProxy(CriteriaVariable* owner) : owner_(owner) {
        }

        ~ValueMutatorProxy() {
            if (owner_) {
                owner_->evaluate_criteria_and_notify_waiters();
            }
        }

        // Proxies are typically short-lived and tied to an expression; disallow copy/move.
        ValueMutatorProxy(const ValueMutatorProxy&) = delete;
        ValueMutatorProxy& operator=(const ValueMutatorProxy&) = delete;
        ValueMutatorProxy(ValueMutatorProxy&&) = delete;
        ValueMutatorProxy& operator=(ValueMutatorProxy&&) = delete;

        T* operator->() {
            return &(owner_->value_);
        }

        T& operator*() {
            return owner_->value_;
        }

    private:
        CriteriaVariable* owner_;
    };

    /**
     * @brief Provides access to modify the internal value via pointer-like semantics.
     *        e.g., `my_criteria_var->member = newValue;`
     *        Criteria are checked when the returned proxy is destructed.
     * @return A ValueMutatorProxy for modifying the value.
     */
    ValueMutatorProxy operator->() {
        return ValueMutatorProxy(this);
    }

    /**
     * @brief Provides access to modify the internal value via reference-like semantics.
     *        e.g., `(*my_criteria_var) = newValue;`
     *        Criteria are checked when the returned proxy is destructed.
     * @return A ValueMutatorProxy for modifying the value.
     */
    ValueMutatorProxy operator*() { 
        return ValueMutatorProxy(this);
    }
    
    /**
     * @brief Manually triggers the evaluation of criteria and notifies waiters if met.
     *        This is primarily called by the `ValueMutatorProxy`'s destructor but can
     *        be called manually if the value is modified by other means (not recommended
     *        without understanding implications).
     */
    void evaluate_criteria_and_notify_waiters() {
        if (is_evaluating_criteria_) {
            LOG_DEBUG<<"Re-entrant call to evaluate_criteria_and_notify_waiters detected. Skipping.";
            return;
        }
        
        is_evaluating_criteria_ = true; 

        LOG_DEBUG<<"Evaluating criteria. Current value (details depend on T). Waiters: " << waiting_coroutines_.size();
        
        if (!criteria_func_) {
             LOG_DEBUG<<"Warning: Criteria function is null.";
             is_evaluating_criteria_ = false;
             return;
        }
        bool criteria_met_now = criteria_func_(value_);

        if (criteria_met_now && !waiting_coroutines_.empty()) {
            LOG_DEBUG<<"Criteria MET. Moving " << waiting_coroutines_.size() << " waiters for resumption.";
            std::vector<std::coroutine_handle<>> to_resume_local = std::move(waiting_coroutines_);
            // waiting_coroutines_ is now empty.

            LOG_DEBUG<<"Resuming " << to_resume_local.size() << " waiters.";
            for (std::coroutine_handle<> h : to_resume_local) { // Iterate by value (copy of handle)
                if (h) {
                    LOG_DEBUG<<"Resuming coroutine at address " << h.address();
                    h.resume(); 
                }
            }
        } else if (criteria_met_now) {
            LOG_DEBUG<<"Criteria MET. No waiters to resume.";
        }
        else {
            LOG_DEBUG<<"Criteria NOT met.";
        }
        is_evaluating_criteria_ = false;
    }

    // --- Awaitable interface ---

    /**
     * @brief Checks if the criteria are currently met. Part of the awaitable interface.
     * @return `true` if criteria are met (co_await will not suspend), `false` otherwise.
     */
    bool await_ready() const {
        if (!criteria_func_) {
            LOG_DEBUG<<"await_ready: Criteria function is null. Assuming not ready.";
            return false;
        }
        bool ready = criteria_func_(value_);
        LOG_DEBUG<<"await_ready called. Criteria met: " << std::boolalpha << ready;
        return ready;
    }

    /**
     * @brief Suspends the awaiting coroutine if criteria are not met. Part of the awaitable interface.
     * @param awaiting_coroutine The handle of the coroutine that is `co_await`ing.
     * @return `true` to suspend the coroutine, `false` to continue without suspension (if criteria met concurrently).
     */
    bool await_suspend(std::coroutine_handle<> awaiting_coroutine) {
        if (!criteria_func_) {
             LOG_DEBUG<<"await_suspend: Criteria function is null. Suspending.";
             waiting_coroutines_.push_back(awaiting_coroutine);
             return true; // Suspend
        }

        // Re-check criteria. In a single-threaded model, this is mainly for robustness
        // if other resumed coroutines could have affected this object's state.
        if (criteria_func_(value_)) {
            LOG_DEBUG<<"await_suspend: Criteria met (re-check). Not suspending coroutine " << awaiting_coroutine.address();
            return false; // Don't suspend
        }

        LOG_DEBUG<<"await_suspend: Storing coroutine " << awaiting_coroutine.address() << " for later. Suspending.";
        waiting_coroutines_.push_back(awaiting_coroutine);
        return true; // Suspend
    }

    /**
     * @brief Called when the coroutine resumes after suspension. Part of the awaitable interface.
     */
    void await_resume() const {
        LOG_DEBUG<<"await_resume: Resumed. Criteria was met at the point of the resumption signal.";
    }

    /**
     * @brief Gets a copy of the current value.
     * @return A copy of the held value.
     */
    T get_value() const { 
        return value_;
    }

    /**
     * @brief Provides const access to the internal value without triggering criteria evaluation.
     * Useful for inspection.
     * @return A const reference to the held value.
     */
    const T& peek_value() const {
        return value_;
    }
};


} // namespace wutong

// Coroutine traits for wutong::Task and wutong::SharedTask
namespace std {
    template <typename T, typename... Args>
    struct coroutine_traits<wutong::Task<T>, Args...> {
        using promise_type = typename wutong::Task<T>::promise_type;
    };

    template <typename... Args>
    struct coroutine_traits<wutong::Task<void>, Args...> {
        using promise_type = wutong::Task<void>::promise_type;
    };

    template <typename T, typename... Args>
    struct coroutine_traits<wutong::SharedTask<T>, Args...> {
        using promise_type = typename wutong::SharedTask<T>::promise_type;
    };
    template <typename... Args>
    struct coroutine_traits<wutong::SharedTask<void>, Args...> {
        using promise_type = typename wutong::SharedTask<void>::promise_type;
    };
}