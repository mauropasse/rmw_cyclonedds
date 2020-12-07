#ifndef STUB_PUBLISHER_HPP_
#define STUB_PUBLISHER_HPP_

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
  }

  rmw_qos_profile_t * get_qos_policies()
  {
    auto qos = const_cast<rmw_qos_profile_t *>(
      static_cast<const rmw_qos_profile_t *>(pub_qos_));
    return qos;
  }

private:
  const rmw_qos_profile_t * pub_qos_;
  const rosidl_message_type_support_t * type_supports_;
public:
  typedef uint64_t dds_instance_handle_t;
  dds_instance_handle_t pubiid;
};

#endif  // STUB_PUBLISHER_HPP_
