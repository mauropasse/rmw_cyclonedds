#ifndef STUB_GUARD_CONDITION_HPP_
#define STUB_GUARD_CONDITION_HPP_

#include <condition_variable>
#include <mutex>

#include "rmw/listener_event_types.h"

class StubGuardCondition
{
public:
  StubGuardCondition() {
    std::cout << "Create StubGuardCondition" << std::endl;
  }

  void
  trigger()
  {
    std::unique_lock<std::mutex> lock_mutex(listener_callback_mutex_);

    std::cout << "trigger guard condition!!" << std::endl;

    if(listener_callback_)
    {
      listener_callback_(user_data_, { waitable_handle_, WAITABLE_EVENT });
    } else {
      triggered_ = true;
      unread_count_++;
    }
  }

  bool
  has_triggered()
  {
    std::unique_lock<std::mutex> lock_mutex(listener_callback_mutex_);

    bool has_triggered = triggered_;

    triggered_ = !triggered_;

    return has_triggered;
  }

  // Provide handlers to perform an action when a
  // new event from this listener has ocurred
  void
  set_callback(
    const void * user_data,
    rmw_listener_cb_t callback,
    const void * waitable_handle,
    bool use_previous_events)
  {
    std::unique_lock<std::mutex> lock_mutex(listener_callback_mutex_);

    if(user_data && waitable_handle && callback)
    {
      user_data_ = user_data;
      listener_callback_ = callback;
      waitable_handle_ = waitable_handle;
    } else {
      // Unset callback: If any of the pointers is NULL, do not use callback.
      user_data_ = nullptr;
      listener_callback_ = nullptr;
      waitable_handle_ = nullptr;
      return;
    }

    if (use_previous_events) {
      // Push events arrived before setting the executor's callback
      for(uint64_t i = 0; i < unread_count_; i++) {
        listener_callback_(user_data_, { waitable_handle_, WAITABLE_EVENT });
      }
    }

    // Reset unread count
    unread_count_ = 0;
  }

private:
  // Wait set
  bool triggered_{false};
  // Events executor
  rmw_listener_cb_t listener_callback_{nullptr};
  const void * waitable_handle_{nullptr};
  const void * user_data_{nullptr};
  std::mutex listener_callback_mutex_;
  uint64_t unread_count_ = 0;
};

#endif  // STUB_GUARD_CONDITION_HPP_
