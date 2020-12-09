#ifndef STUB_PUBLISHER_HPP_
#define STUB_PUBLISHER_HPP_

#include <mutex>

class StubPublisher
{
public:
  StubPublisher(
    const rmw_qos_profile_t * qos_policies,
    const rosidl_message_type_support_t * type_supports,
    const char * topic_name)
  {
    pub_qos_ = qos_policies;
    type_supports_ = type_supports;
    static uint64_t id = 0;
    pubiid = id++;
    topic_name_ = std::string(topic_name);
  }

  void get_qos_policies(rmw_qos_profile_t * qos)
  {
    *qos = *pub_qos_;
  }

  size_t get_subscription_count()
  {
     std::lock_guard<std::mutex> lock(mutex_);
     return matched_subscriptions_.size();
  }

  void add_matched_subscription_id(uint64_t intra_process_subscriber_id)
  {
     std::lock_guard<std::mutex> lock(mutex_);
     matched_subscriptions_.push_back(intra_process_subscriber_id);
  }

public:
  typedef uint64_t dds_instance_handle_t;
  dds_instance_handle_t pubiid;

private:
  const rmw_qos_profile_t * pub_qos_;
  const rosidl_message_type_support_t * type_supports_;
  std::mutex mutex_;
  std::vector<uint64_t> matched_subscriptions_;
  std::string topic_name_;

};

#endif  // STUB_PUBLISHER_HPP_
