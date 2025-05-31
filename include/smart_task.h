// smart_task.h
#pragma once

#include <coroutine>
#include <exception>
#include <memory>   
#include <optional> 
#include <tasks.h>
#include <utility>
#include "stlite/vector.hpp"


namespace wutong {
    template<typename T> class SmartTask;
    namespace detail {

        template<typename T> struct SmartTaskSharedState;

        template<>
        struct SmartTaskSharedState<void> {
            enum class State { PENDING, COMPLETED, EXCEPTION } current_state_ = State::PENDING;
            std::exception_ptr exception_ptr_;
            sjtu::vector<std::coroutine_handle<>> continuations_;


            SmartTaskSharedState() = default;

            void set_completed() {
                if (current_state_ != State::PENDING) return;
                current_state_ = State::COMPLETED;
                sjtu::vector to_resume(std::move(continuations_));
                for (auto& h : to_resume) if (h) h.resume();
            }

            void set_exception(std::exception_ptr ex) {
                if (current_state_ != State::PENDING) return;
                exception_ptr_ = ex;
                current_state_ = State::EXCEPTION;
                sjtu::vector to_resume(std::move(continuations_));
                for (auto& h : to_resume) if (h) h.resume();
            }

            bool add_continuation_or_ready(std::coroutine_handle<> h) {
                if (current_state_ != State::PENDING) return false;
                continuations_.push_back(h);
                return true;
            }

            void get_value_or_rethrow() {
                if (current_state_ == State::EXCEPTION) std::rethrow_exception(exception_ptr_);
                if (current_state_ == State::COMPLETED) return;
                // This should ideally not be reached if await_ready/await_suspend logic is correct
                // and value is only fetched after resolution.
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

            SmartTaskPromise() : shared_state_ptr_(std::make_shared<SmartTaskSharedState<void>>()) {}

            SmartTask<void> get_return_object();

            void return_void() { shared_state_ptr_->set_completed(); }
            void unhandled_exception() { shared_state_ptr_->set_exception(std::current_exception()); }

            auto final_suspend() noexcept {
                struct FinalAwaiterSelfDestroy {
                    bool await_ready() const noexcept { return false; } // Must suspend to allow destruction logic.

                    void await_suspend(std::coroutine_handle<SmartTaskPromise<void>> h) const noexcept {
                        h.destroy(); // Destroy the coroutine frame.
                    }
                    void await_resume() const noexcept {} // Should not be called.
                };
                return FinalAwaiterSelfDestroy{};
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
            : state_ptr_(std::move(state)) {}

        ~SmartTask() = default;

        SmartTask(const SmartTask&) = default;//allows copy, the task is shared and can be waited by several coroutine
        SmartTask& operator=(const SmartTask&) = default;

        bool await_ready() const noexcept {
            if (!state_ptr_) return true;
            return state_ptr_->is_resolved();
        }

        bool await_suspend(std::coroutine_handle<> h) noexcept {
            if (!state_ptr_) return false;
            return state_ptr_->add_continuation_or_ready(h);
        }

        void await_resume() {
            if (!state_ptr_) throw std::logic_error("Awaiting an invalid SmartTask<void> (moved-from or default-constructed).");
            state_ptr_->get_value_or_rethrow();
        }

        bool is_valid() const { return state_ptr_ != nullptr; }
    };
}

namespace wutong::detail {
    inline SmartTask<void> SmartTaskPromise<void>::get_return_object() {
        return SmartTask<void>(shared_state_ptr_);
    }
}


namespace std {
    template <typename... Args>
    struct coroutine_traits<wutong::SmartTask<void>, Args...> {
        using promise_type = wutong::detail::SmartTaskPromise<void>;
    };
}