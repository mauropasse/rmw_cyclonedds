#ifndef STUB_SUBSCRIPTION_HPP_
#define STUB_SUBSCRIPTION_HPP_

#include <mutex>

class StubSubscription
{
public:
  StubSubscription(
    const rmw_qos_profile_t * qos_policies,
    const rmw_subscription_options_t * subscription_options,
    const rosidl_message_type_support_t * type_supports,
    const char * topic_name)
  {
    // Check what we need/don't need for a basic IPC comms
    sub_qos_ = qos_policies;
    sub_options_ = subscription_options;
    type_supports_ = type_supports;
    static uint64_t id = 0;
    subiid = id++;
    topic_name_ = std::string(topic_name);
  }

  void get_qos_policies(rmw_qos_profile_t * qos)
  {
    *qos = *sub_qos_;
  }

  size_t get_publisher_count()
  {
     std::lock_guard<std::mutex> lock(mutex_);
     return matched_publishers_.size();
  }

  void add_matched_publisher_id(uint64_t intra_process_publisher_id)
  {
     std::lock_guard<std::mutex> lock(mutex_);
     matched_publishers_.push_back(intra_process_publisher_id);
  }

  // Provide handlers to perform an action when a
  // new event from this listener has ocurred
  void
  set_callback(
    const void * user_data,
    rmw_listener_cb_t callback,
    const void * waitable_handle)
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

    // Push events arrived before setting the executor's callback
    for(uint64_t i = 0; i < unread_count_; i++) {
      listener_callback_(user_data_, { waitable_handle_, WAITABLE_EVENT });
    }

    // Reset unread count
    unread_count_ = 0;
  }

public:
  typedef uint64_t dds_instance_handle_t;
  dds_instance_handle_t subiid;

private:
  const rmw_qos_profile_t * sub_qos_;
  const rmw_subscription_options_t * sub_options_;
  const rosidl_message_type_support_t * type_supports_;
  std::mutex mutex_;
  std::vector<uint64_t> matched_publishers_;
  std::string topic_name_;

  // Events executor
  rmw_listener_cb_t listener_callback_{nullptr};
  const void * waitable_handle_{nullptr};
  const void * user_data_{nullptr};
  std::mutex listener_callback_mutex_;
  uint64_t unread_count_ = 0;
};

#endif  // STUB_SUBSCRIPTION_HPP_
