#pragma once


// #include <tasks.h>
#include <utility>
#include "stlite/vector.hpp"
#include <tasks.h>

namespace wutong {
    template<typename T> class SmartTask;
    namespace detail {

        template<typename T> struct SmartTaskSharedState;

        template<>
        struct SmartTaskSharedState<void> {
            uint64_t shared_state_id_;
            enum class State { PENDING, COMPLETED, EXCEPTION } current_state_ = State::PENDING;
            std::exception_ptr exception_ptr_;
            sjtu::vector<std::coroutine_handle<>> continuations_;


            SmartTaskSharedState() : shared_state_id_(g_shared_state_id_counter++) {
                LOG_DEBUG << "SmartTaskSharedStateID<void>: " << shared_state_id_ << ", Addr: " << static_cast<void*>(this) << " constructed." << std::endl;
            }

            ~SmartTaskSharedState() {
                LOG_DEBUG << "SmartTaskSharedStateID<void>: " << shared_state_id_ << ", Addr: " << static_cast<void*>(this) << " destructed. State: " << (int)current_state_
                          << ", Continuations pending: " << continuations_.size() << std::endl;
            }

            void set_completed() {
                LOG_DEBUG << "SmartTaskSharedStateID<void>: " << shared_state_id_ << " set_completed(). Current state: " << (int)current_state_ << std::endl;
                if (current_state_ != State::PENDING) {
                    LOG_WARN << "SmartTaskSharedStateID<void>: " << shared_state_id_ << " set_completed() called but state is not PENDING (" << (int)current_state_ << ")" << std::endl;
                    return;
                }
                current_state_ = State::COMPLETED;
                LOG_DEBUG << "  Resuming " << continuations_.size() << " continuations." << std::endl;
                sjtu::vector to_resume(std::move(continuations_));
                for (auto& h : to_resume) {
                    if (h) {
                        LOG_DEBUG << "    Resuming continuation " << h.address() << std::endl;
                        h.resume();
                    }
                }
            }

            void set_exception(std::exception_ptr ex) {
                LOG_DEBUG << "SmartTaskSharedStateID<void>: " << shared_state_id_ << " set_exception(). Current state: " << (int)current_state_ << std::endl;
                if (current_state_ != State::PENDING) {
                    LOG_WARN << "SmartTaskSharedStateID<void>: " << shared_state_id_ << " set_exception() called but state is not PENDING (" << (int)current_state_ << ")" << std::endl;
                    return;
                }
                exception_ptr_ = ex;
                current_state_ = State::EXCEPTION;
                LOG_DEBUG << "  Resuming " << continuations_.size() << " continuations." << std::endl;
                sjtu::vector to_resume(std::move(continuations_));
                for (auto& h : to_resume) {
                    if (h) {
                        LOG_DEBUG << "    Resuming continuation " << h.address() << std::endl;
                        h.resume();
                    }
                }
            }

            bool add_continuation_or_ready(std::coroutine_handle<> h) {
                LOG_DEBUG << "SmartTaskSharedStateID<void>: " << shared_state_id_ << " add_continuation_or_ready() for awaiting_coro: " << h.address()
                          << ". Current state: " << (int)current_state_ << std::endl;
                if (current_state_ != State::PENDING) {
                    LOG_DEBUG << "  Already completed. Returning false (don't suspend)." << std::endl;
                    return false;
                }
                continuations_.push_back(h);
                LOG_DEBUG << "  Pending. Added continuation (total: " << continuations_.size() << "). Returning true (suspend)." << std::endl;
                return true;
            }

            void get_value_or_rethrow() {
                LOG_DEBUG << "SmartTaskSharedStateID<void>: " << shared_state_id_ << " get_value_or_rethrow(). Current state: " << (int)current_state_ << std::endl;
                if (current_state_ == State::EXCEPTION) {
                    LOG_DEBUG << "  Rethrowing exception." << std::endl;
                    std::rethrow_exception(exception_ptr_);
                }
                if (current_state_ == State::COMPLETED) return;
                LOG_CRITICAL << "SmartTaskSharedStateID<void>: " << shared_state_id_ << " get_value_or_rethrow() called but state is not COMPLETED and no exception! State: " << (int)current_state_ << std::endl;
                throw std::logic_error("SmartTask<void> not completed and no exception.");
            }

            bool is_resolved() const {
                return current_state_ != State::PENDING;
            }
        };


        // --- 2. SmartTaskPromise ---
        template<typename T> struct SmartTaskPromise;

        template<>
        struct SmartTaskPromise<void> : public wutong::detail::PromiseBase<void> {
            std::shared_ptr<SmartTaskSharedState<void>> shared_state_ptr_;

            SmartTaskPromise() : shared_state_ptr_(std::make_shared<SmartTaskSharedState<void>>()) {
                LOG_DEBUG << "SmartTaskPromiseID<void>: " << this->promise_id_ << ", PromiseAddr: " << static_cast<void*>(this)
                          << " constructed. Owns SmartTaskSharedStateID: " << shared_state_ptr_->shared_state_id_
                          << ", SharedStateAddr: " << static_cast<void*>(shared_state_ptr_.get()) << std::endl;
            }

            ~SmartTaskPromise() {
                LOG_DEBUG << "SmartTaskPromiseID<void>: " << this->promise_id_ << ", PromiseAddr: " << static_cast<void*>(this)
                          << " destructed. SmartTaskSharedStateID: " << shared_state_ptr_->shared_state_id_
                          << " (use_count: " << shared_state_ptr_.use_count() << ")" << std::endl;
            }

            SmartTask<void> get_return_object();

            void return_void() {
                LOG_DEBUG << "SmartTaskPromiseID<void>: " << this->promise_id_ << " return_void. Forwarding to SmartTaskSharedStateID: "
                          << shared_state_ptr_->shared_state_id_ << std::endl;
                shared_state_ptr_->set_completed();
            }
            void unhandled_exception() {
                LOG_WARN << "SmartTaskPromiseID<void>: " << this->promise_id_ << " unhandled_exception. Forwarding to SmartTaskSharedStateID: "
                         << shared_state_ptr_->shared_state_id_ << std::endl;
                shared_state_ptr_->set_exception(std::current_exception());
            }

            std::suspend_never final_suspend() noexcept {
                LOG_DEBUG << "SmartTaskPromiseID<void>: " << this->promise_id_ << ", PromiseAddr: " << static_cast<void*>(this)
                          << " final_suspend (suspend_never). Coro frame will be destroyed. SharedState (ID "
                          << shared_state_ptr_->shared_state_id_ << ", Addr " << static_cast<void*>(shared_state_ptr_.get())
                          << ") lives on via shared_ptr (use_count: " << shared_state_ptr_.use_count() << ")." << std::endl;
                return {};
            }
        };

    }

    template<>
    /** @brief A smart task that can automatically handle lifetime, enables: detached tasks, share tasks
    */
    class SmartTask<void> {
    public:
        using promise_type = wutong::detail::SmartTaskPromise<void>;

        std::shared_ptr<wutong::detail::SmartTaskSharedState<void>> state_ptr_;

        SmartTask() : state_ptr_(nullptr) {}

        explicit SmartTask(std::shared_ptr<wutong::detail::SmartTaskSharedState<void>> state)
            : state_ptr_(std::move(state)) {
            LOG_DEBUG << "SmartTaskObj<void>: " << static_cast<void*>(this) << " constructed. Points to SmartTaskSharedStateID: "
                      << (state_ptr_ ? state_ptr_->shared_state_id_ : -1ull) << ", Addr: " << (state_ptr_ ? static_cast<void*>(state_ptr_.get()) : nullptr)
                      << " (use_count: " << (state_ptr_ ? state_ptr_.use_count() : 0) << ")" << std::endl;
        }

        ~SmartTask() {
            LOG_DEBUG << "SmartTaskObj<void>: " << static_cast<void*>(this) << " destructed. Pointed to SmartTaskSharedStateID: "
                      << (state_ptr_ ? state_ptr_->shared_state_id_ : -1ull)
                      << " (use_count before this dtor affects it: " << (state_ptr_ ? state_ptr_.use_count() : 0) << ")" << std::endl;
        }

        SmartTask(const SmartTask& other) : state_ptr_(other.state_ptr_) {
            LOG_DEBUG << "SmartTaskObj<void>: " << static_cast<void*>(this) << " copy constructed from SmartTaskObj: " << static_cast<const void*>(&other)
                      << ". Points to SmartTaskSharedStateID: " << (state_ptr_ ? state_ptr_->shared_state_id_ : -1ull)
                      << " (use_count: " << (state_ptr_ ? state_ptr_.use_count() : 0) << ")" << std::endl;
        }
        SmartTask& operator=(const SmartTask& other) {
            LOG_DEBUG << "SmartTaskObj<void>: " << static_cast<void*>(this) << " copy assigned from SmartTaskObj: " << static_cast<const void*>(&other) << std::endl;
            if (this != &other) {
                state_ptr_ = other.state_ptr_;
                LOG_DEBUG << "  Now points to SmartTaskSharedStateID: " << (state_ptr_ ? state_ptr_->shared_state_id_ : -1ull)
                          << " (use_count: " << (state_ptr_ ? state_ptr_.use_count() : 0) << ")" << std::endl;
            }
            return *this;
        }

        SmartTask(SmartTask&& other) noexcept : state_ptr_(std::move(other.state_ptr_)) {
            LOG_DEBUG << "SmartTaskObj<void>: " << static_cast<void*>(this) << " move constructed from SmartTaskObj: " << static_cast<void*>(&other)
                      << ". Points to SmartTaskSharedStateID: " << (state_ptr_ ? state_ptr_->shared_state_id_ : -1ull)
                      << ". other.state_ptr_ is now " << (other.state_ptr_ ? "valid" : "null")
                      << " (use_count after move: " << (state_ptr_ ? state_ptr_.use_count() : 0) << ")" << std::endl;
        }
        SmartTask& operator=(SmartTask&& other) noexcept {
            LOG_DEBUG << "SmartTaskObj<void>: " << static_cast<void*>(this) << " move assigned from SmartTaskObj: " << static_cast<void*>(&other) << std::endl;
            if (this != &other) {
                state_ptr_ = std::move(other.state_ptr_);
                LOG_DEBUG << "  Now points to SmartTaskSharedStateID: " << (state_ptr_ ? state_ptr_->shared_state_id_ : -1ull)
                          << ". other.state_ptr_ is now " << (other.state_ptr_ ? "valid" : "null")
                          << " (use_count after move: " << (state_ptr_ ? state_ptr_.use_count() : 0) << ")" << std::endl;
            }
            return *this;
        }

        bool await_ready() const noexcept {
            if (!state_ptr_) {
                LOG_WARN << "SmartTaskObj<void>: " << static_cast<const void*>(this) << " await_ready() on null state. Returning true (ready)." << std::endl;
                return true;
            }
            bool ready = state_ptr_->is_resolved();
            LOG_DEBUG << "SmartTaskObj<void>: " << static_cast<const void*>(this) << " (SmartTaskSharedStateID: " << state_ptr_->shared_state_id_
                      << ") await_ready(). State resolved: " << std::boolalpha << ready << ". Returning: " << std::boolalpha << ready << std::endl;
            return ready;
        }

        bool await_suspend(std::coroutine_handle<> h) noexcept {
            if (!state_ptr_) {
                LOG_WARN << "SmartTaskObj<void>: " << static_cast<void*>(this) << " await_suspend() on null state. Returning false (don't suspend)." << std::endl;
                return false;
            }
            LOG_DEBUG << "SmartTaskObj<void>: " << static_cast<void*>(this) << " (SmartTaskSharedStateID: " << state_ptr_->shared_state_id_
                      << ") await_suspend() by awaiting_coro: " << h.address() << std::endl;
            bool should_suspend = state_ptr_->add_continuation_or_ready(h);
            LOG_DEBUG << "  SharedState::add_continuation_or_ready returned: " << std::boolalpha << should_suspend << ". Returning this value." << std::endl;
            return should_suspend;
        }

        void await_resume() {
            LOG_DEBUG << "SmartTaskObj<void>: " << static_cast<void*>(this) << " await_resume()." << std::endl;
            if (!state_ptr_) {
                LOG_CRITICAL << "SmartTaskObj<void>: " << static_cast<void*>(this) << " Awaiting an invalid SmartTask<void> (moved-from or default-constructed)." << std::endl;
                throw std::logic_error("Awaiting an invalid SmartTask<void> (moved-from or default-constructed).");
            }
            LOG_DEBUG << "  Forwarding to SmartTaskSharedStateID: " << state_ptr_->shared_state_id_ << " get_value_or_rethrow()." << std::endl;
            state_ptr_->get_value_or_rethrow();
        }

        bool is_valid() const { return state_ptr_ != nullptr; }
    };
}

namespace wutong::detail {
    inline SmartTask<void> SmartTaskPromise<void>::get_return_object() {
        LOG_DEBUG << "SmartTaskPromiseID<void>: " << this->promise_id_ << ", PromiseAddr: " << static_cast<void*>(this)
                  << " get_return_object() -> SmartTask<void> for SmartTaskSharedStateID: " << shared_state_ptr_->shared_state_id_ << std::endl;
        return SmartTask<void>(shared_state_ptr_);
    }
}