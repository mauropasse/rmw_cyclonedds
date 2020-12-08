// Copyright 2019 ADLINK Technology Limited.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <cassert>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <map>
#include <set>
#include <functional>
#include <atomic>
#include <memory>
#include <vector>
#include <string>
#include <tuple>
#include <utility>
#include <regex>
#include <limits>

#include "rcutils/filesystem.h"
#include "rcutils/format_string.h"
#include "rcutils/get_env.h"
#include "rcutils/logging_macros.h"
#include "rcutils/strdup.h"

#include "rmw/allocators.h"
#include "rmw/convert_rcutils_ret_to_rmw_ret.h"
#include "rmw/error_handling.h"
#include "rmw/event.h"
#include "rmw/get_node_info_and_types.h"
#include "rmw/get_service_names_and_types.h"
#include "rmw/get_topic_names_and_types.h"
#include "rmw/listener_event_types.h"
#include "rmw/names_and_types.h"
#include "rmw/rmw.h"
#include "rmw/sanity_checks.h"
#include "rmw/validate_namespace.h"
#include "rmw/validate_node_name.h"

#include "rcpputils/scope_exit.hpp"
#include "rmw/impl/cpp/macros.hpp"
#include "rmw/impl/cpp/key_value.hpp"

#include "rmw/get_topic_endpoint_info.h"
#include "rmw/incompatible_qos_events_statuses.h"
#include "rmw/topic_endpoint_info_array.h"

#include "rmw_dds_common/context.hpp"
#include "rmw_dds_common/graph_cache.hpp"
#include "rmw_dds_common/msg/participant_entities_info.hpp"

#include "rosidl_typesupport_cpp/message_type_support.hpp"

#include "rmw_stub_cpp/stub_context_implementation.hpp"
#include "rmw_stub_cpp/stub_guard_condition.hpp"
#include "rmw_stub_cpp/stub_publisher.hpp"
#include "rmw_stub_cpp/stub_node.hpp"

using namespace std::literals::chrono_literals;

using rmw_dds_common::msg::ParticipantEntitiesInfo;

#define RET_ERR_X(msg, code) do {RMW_SET_ERROR_MSG(msg); code;} while (0)
#define RET_NULL_X(var, code) do {if (!var) {RET_ERR_X(#var " is null", code);}} while (0)
#define RET_NULL(var) RET_NULL_X(var, return RMW_RET_ERROR)

const char * const stub_identifier = "rmw_stub_cpp";
const char * const stub_serialization_format = "cdr";

extern "C" const char * rmw_get_implementation_identifier()
{
  return stub_identifier;
}

extern "C" rmw_ret_t rmw_init_options_init(
  rmw_init_options_t * init_options,
  rcutils_allocator_t allocator)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(init_options, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ALLOCATOR(&allocator, return RMW_RET_INVALID_ARGUMENT);
  if (NULL != init_options->implementation_identifier) {
    RMW_SET_ERROR_MSG("expected zero-initialized init_options");
    return RMW_RET_INVALID_ARGUMENT;
  }
  init_options->instance_id = 0;
  init_options->implementation_identifier = stub_identifier;
  init_options->allocator = allocator;
  init_options->impl = nullptr;
  init_options->localhost_only = RMW_LOCALHOST_ONLY_DEFAULT;
  init_options->domain_id = RMW_DEFAULT_DOMAIN_ID;
  init_options->enclave = NULL;
  init_options->security_options = rmw_get_zero_initialized_security_options();
  return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_init_options_copy(const rmw_init_options_t * src, rmw_init_options_t * dst)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(src, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(dst, RMW_RET_INVALID_ARGUMENT);
  if (NULL == src->implementation_identifier) {
    RMW_SET_ERROR_MSG("expected initialized dst");
    return RMW_RET_INVALID_ARGUMENT;
  }
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    src,
    src->implementation_identifier,
    stub_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  if (NULL != dst->implementation_identifier) {
    RMW_SET_ERROR_MSG("expected zero-initialized dst");
    return RMW_RET_INVALID_ARGUMENT;
  }
  const rcutils_allocator_t * allocator = &src->allocator;

  rmw_init_options_t tmp = *src;
  tmp.enclave = rcutils_strdup(tmp.enclave, *allocator);
  if (NULL != src->enclave && NULL == tmp.enclave) {
    return RMW_RET_BAD_ALLOC;
  }
  tmp.security_options = rmw_get_zero_initialized_security_options();
  rmw_ret_t ret =
    rmw_security_options_copy(&src->security_options, allocator, &tmp.security_options);
  if (RMW_RET_OK != ret) {
    allocator->deallocate(tmp.enclave, allocator->state);
    return ret;
  }
  *dst = tmp;
  return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_init_options_fini(rmw_init_options_t * init_options)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(init_options, RMW_RET_INVALID_ARGUMENT);

  if (NULL == init_options->implementation_identifier) {
    RMW_SET_ERROR_MSG("expected initialized init_options");
    return RMW_RET_INVALID_ARGUMENT;
  }

  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    init_options,
    init_options->implementation_identifier,
    stub_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);

  rcutils_allocator_t * allocator = &init_options->allocator;

  RCUTILS_CHECK_ALLOCATOR(allocator, return RMW_RET_INVALID_ARGUMENT);

  allocator->deallocate(init_options->enclave, allocator->state);
  rmw_ret_t ret = rmw_security_options_fini(&init_options->security_options, allocator);
  *init_options = rmw_get_zero_initialized_init_options();
  return ret;
}

extern "C" rmw_ret_t rmw_shutdown(rmw_context_t * context)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(context, RMW_RET_INVALID_ARGUMENT);

  RMW_CHECK_FOR_NULL_WITH_MSG(
    context->impl,
    "expected initialized context",
    return RMW_RET_INVALID_ARGUMENT);

  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    context,
    context->implementation_identifier,
    stub_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);

  context->impl->is_shutdown = true;

  return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_context_fini(rmw_context_t * context)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(context, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_FOR_NULL_WITH_MSG(
    context->impl,
    "expected initialized context",
    return RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    context,
    context->implementation_identifier,
    stub_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  if (!context->impl->is_shutdown) {
    RMW_SET_ERROR_MSG("context has not been shutdown");
    return RMW_RET_INVALID_ARGUMENT;
  }
  rmw_ret_t ret = rmw_init_options_fini(&context->options);
  delete context->impl;
  *context = rmw_get_zero_initialized_context();
  return ret;
}

extern "C" const char * rmw_get_serialization_format()
{
  return stub_serialization_format;
}


extern "C" rmw_ret_t rmw_set_log_severity(rmw_log_severity_t severity)
{
  RCUTILS_LOG_ERROR_NAMED("rmw_stub.cpp","rmw_set_log_severity not supported");
  return RMW_RET_UNSUPPORTED;
}

extern "C" rmw_ret_t rmw_subscription_set_listener_callback(
  const void * user_data,
  rmw_listener_cb_t callback,
  const void * subscription_handle,
  rmw_subscription_t * rmw_subscription)
{
  (void)user_data;
  (void)callback;
  (void)subscription_handle;
  (void)rmw_subscription;
  // auto subscription = static_cast<CddsSubscription *>(rmw_subscription->data);
  // subscription->setCallback(user_data, callback, subscription_handle);
  RCUTILS_LOG_ERROR_NAMED(
    "rmw_stub.cpp",
    "rmw_subscription_set_listener_callback: not supported (yet)");
  return RMW_RET_UNSUPPORTED;
}

extern "C" rmw_ret_t rmw_service_set_listener_callback(
  const void * user_data,
  rmw_listener_cb_t callback,
  const void * service_handle,
  rmw_service_t * rmw_service)
{
  (void)user_data;
  (void)callback;
  (void)service_handle;
  (void)rmw_service;
  // auto service = static_cast<CddsService *>(rmw_service->data);
  // service->setCallback(user_data, callback, service_handle);
  RCUTILS_LOG_ERROR_NAMED(
    "rmw_stub.cpp",
    "rmw_service_set_listener_callback: not supported (yet)");
  return RMW_RET_UNSUPPORTED;
}

extern "C" rmw_ret_t rmw_client_set_listener_callback(
  const void * user_data,
  rmw_listener_cb_t callback,
  const void * client_handle,
  rmw_client_t * rmw_client)
{
  (void)user_data;
  (void)callback;
  (void)client_handle;
  (void)rmw_client;
  // auto client = static_cast<CddsClient *>(rmw_client->data);
  // client->setCallback(user_data, callback, client_handle);
  RCUTILS_LOG_ERROR_NAMED(
    "rmw_stub.cpp",
    "rmw_client_set_listener_callback: not supported (yet)");
  return RMW_RET_UNSUPPORTED;
}

extern "C" rmw_ret_t rmw_event_set_listener_callback(
  const void * user_data,
  rmw_listener_cb_t callback,
  const void * waitable_handle,
  rmw_event_t * rmw_event,
  bool use_previous_events)
{
  (void)user_data;
  (void)callback;
  (void)waitable_handle;
  (void)rmw_event;
  (void)use_previous_events;
  // auto event = static_cast<CddsEvent *>(rmw_event->data);
  // event->setCallback(user_data, callback,
  //                              waitable_handle, use_previous_events);
  RCUTILS_LOG_ERROR_NAMED(
    "rmw_stub.cpp",
    "rmw_event_set_listener_callback: not supported (yet)");
  return RMW_RET_UNSUPPORTED;
}

rmw_ret_t
rmw_context_impl_t::init(rmw_init_options_t * options, size_t domain_id)
{
  std::lock_guard<std::mutex> guard(initialization_mutex);
  if (0u != this->node_count) {
    // initialization has already been done
    this->node_count++;
    return RMW_RET_OK;
  }

  // Initialization to do once:
  // ..nothing yet

  ++this->node_count;
  return RMW_RET_OK;
}

void
rmw_context_impl_t::clean_up()
{
  //discovery_thread_stop(common);
  //check_destroy_domain(domain_id);
}

rmw_ret_t
rmw_context_impl_t::fini()
{
  std::lock_guard<std::mutex> guard(initialization_mutex);
  if (0u != --this->node_count) {
    // destruction shouldn't happen yet
    return RMW_RET_OK;
  }
  this->clean_up();
  return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_init(const rmw_init_options_t * options, rmw_context_t * context)
{
  rmw_ret_t ret;

  RCUTILS_CHECK_ARGUMENT_FOR_NULL(options, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(context, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_FOR_NULL_WITH_MSG(
    options->implementation_identifier,
    "expected initialized init options",
    return RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    options,
    options->implementation_identifier,
    stub_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_FOR_NULL_WITH_MSG(
    options->enclave,
    "expected non-null enclave",
    return RMW_RET_INVALID_ARGUMENT);
  if (NULL != context->implementation_identifier) {
    RMW_SET_ERROR_MSG("expected a zero-initialized context");
    return RMW_RET_INVALID_ARGUMENT;
  }

  if (options->domain_id >= UINT32_MAX && options->domain_id != RMW_DEFAULT_DOMAIN_ID) {
    RCUTILS_LOG_ERROR_NAMED(
      "rmw_stub_cpp", "rmw_create_node: domain id out of range");
    return RMW_RET_INVALID_ARGUMENT;
  }

  auto restore_context = rcpputils::make_scope_exit(
    [context]() {*context = rmw_get_zero_initialized_context();});

  context->instance_id = options->instance_id;
  context->implementation_identifier = stub_identifier;
  // No custom handling of RMW_DEFAULT_DOMAIN_ID. Simply use a reasonable domain id.
  context->actual_domain_id =
    RMW_DEFAULT_DOMAIN_ID != options->domain_id ? options->domain_id : 0u;

  context->impl = new (std::nothrow) rmw_context_impl_t();
  if (nullptr == context->impl) {
    RMW_SET_ERROR_MSG("failed to allocate context impl");
    return RMW_RET_BAD_ALLOC;
  }
  auto cleanup_impl = rcpputils::make_scope_exit(
    [context]() {delete context->impl;});

  if ((ret = rmw_init_options_copy(options, &context->options)) != RMW_RET_OK) {
    return ret;
  }

  cleanup_impl.cancel();
  restore_context.cancel();
  return RMW_RET_OK;
}

// /////////////////////////////////////////////////////////////////////////////////////////
// ///////////                                                                   ///////////
// ///////////    NODES                                                          ///////////
// ///////////                                                                   ///////////
// /////////////////////////////////////////////////////////////////////////////////////////

extern "C" rmw_node_t * rmw_create_node(
  rmw_context_t * context, const char * name, const char * namespace_)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(context, nullptr);

  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    context,
    context->implementation_identifier,
    stub_identifier,
    return nullptr);

  RMW_CHECK_FOR_NULL_WITH_MSG(
    context->impl,
    "expected initialized context",
    return nullptr);

  if (context->impl->is_shutdown) {
    RCUTILS_SET_ERROR_MSG("context has been shutdown");
    return nullptr;
  }

  int validation_result = RMW_NODE_NAME_VALID;
  rmw_ret_t ret = rmw_validate_node_name(name, &validation_result, nullptr);
  if (RMW_RET_OK != ret) {
    return nullptr;
  }
  if (RMW_NODE_NAME_VALID != validation_result) {
    const char * reason = rmw_node_name_validation_result_string(validation_result);
    RMW_SET_ERROR_MSG_WITH_FORMAT_STRING("invalid node name: %s", reason);
    return nullptr;
  }
  validation_result = RMW_NAMESPACE_VALID;
  ret = rmw_validate_namespace(namespace_, &validation_result, nullptr);
  if (RMW_RET_OK != ret) {
    return nullptr;
  }
  if (RMW_NAMESPACE_VALID != validation_result) {
    const char * reason = rmw_node_name_validation_result_string(validation_result);
    RMW_SET_ERROR_MSG_WITH_FORMAT_STRING("invalid node namespace: %s", reason);
    return nullptr;
  }

  ret = context->impl->init(&context->options, context->actual_domain_id);
  if (RMW_RET_OK != ret) {
    return nullptr;
  }

  auto finalize_context = rcpputils::make_scope_exit(
    [context]() {context->impl->fini();});

  auto * stub_node = new StubNode();

  rmw_node_t * node = rmw_node_allocate();

  node->name = static_cast<const char *>(rmw_allocate(sizeof(char) * strlen(name) + 1));
  memcpy(const_cast<char *>(node->name), name, strlen(name) + 1);

  node->namespace_ = static_cast<const char *>(rmw_allocate(sizeof(char) * strlen(namespace_) + 1));
  memcpy(const_cast<char *>(node->namespace_), namespace_, strlen(namespace_) + 1);

  node->implementation_identifier = stub_identifier;
  node->data = stub_node;
  node->context = context;
  return node;
}

extern "C" rmw_ret_t rmw_destroy_node(rmw_node_t * node)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    node,
    node->implementation_identifier,
    stub_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);

  auto stub_node = static_cast<StubNode *>(node->data);

  rmw_context_t * context = node->context;
  rcutils_allocator_t allocator = context->options.allocator;
  allocator.deallocate(const_cast<char *>(node->name), allocator.state);
  allocator.deallocate(const_cast<char *>(node->namespace_), allocator.state);
  allocator.deallocate(node, allocator.state);

  delete stub_node;
  context->impl->fini();
  return RMW_RET_OK;
}

extern "C" const rmw_guard_condition_t * rmw_node_get_graph_guard_condition(const rmw_node_t * node)
{
  auto stub_node = static_cast<StubNode *>(node->data);

  return stub_node->get_node_graph_guard_condition();
}

// /////////////////////////////////////////////////////////////////////////////////////////
// ///////////                                                                   ///////////
// ///////////    (DE)SERIALIZATION                                              ///////////
// ///////////                                                                   ///////////
// /////////////////////////////////////////////////////////////////////////////////////////

extern "C" rmw_ret_t rmw_get_serialized_message_size(
  const rosidl_message_type_support_t * type_support,
  const rosidl_runtime_c__Sequence__bound * message_bounds, size_t * size)
{
  static_cast<void>(type_support);
  static_cast<void>(message_bounds);
  static_cast<void>(size);

  RMW_SET_ERROR_MSG("rmw_get_serialized_message_size: unimplemented");
  return RMW_RET_UNSUPPORTED;
}

extern "C" rmw_ret_t rmw_serialize(
  const void * ros_message,
  const rosidl_message_type_support_t * type_support,
  rmw_serialized_message_t * serialized_message)
{
  RMW_SET_ERROR_MSG("rmw_serialize: unimplemented");
  return RMW_RET_UNSUPPORTED;
}

extern "C" rmw_ret_t rmw_deserialize(
  const rmw_serialized_message_t * serialized_message,
  const rosidl_message_type_support_t * type_support,
  void * ros_message)
{
  RMW_SET_ERROR_MSG("rmw_deserialize: unimplemented");
  return RMW_RET_UNSUPPORTED;
}

// /////////////////////////////////////////////////////////////////////////////////////////
// ///////////                                                                   ///////////
// ///////////    PUBLICATIONS                                                   ///////////
// ///////////                                                                   ///////////
// /////////////////////////////////////////////////////////////////////////////////////////

extern "C" rmw_ret_t rmw_publish(
  const rmw_publisher_t * publisher, const void * ros_message,
  rmw_publisher_allocation_t * allocation)
{
  static_cast<void>(allocation);    // unused
  RMW_CHECK_FOR_NULL_WITH_MSG(
    publisher, "publisher handle is null",
    return RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    publisher, publisher->implementation_identifier, stub_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_FOR_NULL_WITH_MSG(
    ros_message, "ros message handle is null",
    return RMW_RET_INVALID_ARGUMENT);
  // auto pub = static_cast<CddsPublisher *>(publisher->data);
  // assert(pub);
  // if (dds_write(pub->enth, ros_message) >= 0) {
  //   return RMW_RET_OK;
  // } else {
  //   RMW_SET_ERROR_MSG("failed to publish data");
  //   return RMW_RET_ERROR;
  // }
  RMW_SET_ERROR_MSG("rmw_publish not implemented for rmw_stub_cpp");
  return RMW_RET_UNSUPPORTED;
}

extern "C" rmw_ret_t rmw_publish_serialized_message(
  const rmw_publisher_t * publisher,
  const rmw_serialized_message_t * serialized_message, rmw_publisher_allocation_t * allocation)
{
  RMW_SET_ERROR_MSG("rmw_publish_serialized_message: unimplemented");

  return RMW_RET_UNSUPPORTED;
}

extern "C" rmw_ret_t rmw_publish_loaned_message(
  const rmw_publisher_t * publisher,
  void * ros_message,
  rmw_publisher_allocation_t * allocation)
{
  (void) publisher;
  (void) ros_message;
  (void) allocation;

  RMW_SET_ERROR_MSG("rmw_publish_loaned_message not implemented for rmw_stub_cpp");
  return RMW_RET_UNSUPPORTED;
}

extern "C" rmw_ret_t rmw_init_publisher_allocation(
  const rosidl_message_type_support_t * type_support,
  const rosidl_runtime_c__Sequence__bound * message_bounds, rmw_publisher_allocation_t * allocation)
{
  static_cast<void>(type_support);
  static_cast<void>(message_bounds);
  static_cast<void>(allocation);
  RMW_SET_ERROR_MSG("rmw_init_publisher_allocation: unimplemented");
  return RMW_RET_UNSUPPORTED;
}

extern "C" rmw_ret_t rmw_fini_publisher_allocation(rmw_publisher_allocation_t * allocation)
{
  static_cast<void>(allocation);
  RMW_SET_ERROR_MSG("rmw_fini_publisher_allocation: unimplemented");
  return RMW_RET_UNSUPPORTED;
}

static rmw_publisher_t * create_publisher(
  const rmw_qos_profile_t * qos_policies,
  const rmw_publisher_options_t * publisher_options,
  const rosidl_message_type_support_t * type_supports,
  const char * topic_name)
{
  auto * pub = new StubPublisher(qos_policies, type_supports, topic_name);

  rmw_publisher_t * rmw_publisher = rmw_publisher_allocate();

  rmw_publisher->implementation_identifier = stub_identifier;
  rmw_publisher->data = pub;
  rmw_publisher->options = *publisher_options;
  rmw_publisher->can_loan_messages = false;
  rmw_publisher->topic_name = reinterpret_cast<char *>(rmw_allocate(strlen(topic_name) + 1));

  memcpy(const_cast<char *>(rmw_publisher->topic_name), topic_name, strlen(topic_name) + 1);

  return rmw_publisher;
}

extern "C" rmw_publisher_t * rmw_create_publisher(
  const rmw_node_t * node, const rosidl_message_type_support_t * type_supports,
  const char * topic_name, const rmw_qos_profile_t * qos_policies,
  const rmw_publisher_options_t * publisher_options
)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(node, nullptr);

  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    node,
    node->implementation_identifier,
    stub_identifier,
    return nullptr);

  RMW_CHECK_ARGUMENT_FOR_NULL(type_supports, nullptr);

  RMW_CHECK_ARGUMENT_FOR_NULL(topic_name, nullptr);

  if (0 == strlen(topic_name)) {
    RMW_SET_ERROR_MSG("topic_name argument is an empty string");
    return nullptr;
  }

  RMW_CHECK_ARGUMENT_FOR_NULL(qos_policies, nullptr);

  if (!qos_policies->avoid_ros_namespace_conventions) {
    int validation_result = RMW_TOPIC_VALID;
    rmw_ret_t ret = rmw_validate_full_topic_name(topic_name, &validation_result, nullptr);
    if (RMW_RET_OK != ret) {
      return nullptr;
    }
    if (RMW_TOPIC_VALID != validation_result) {
      const char * reason = rmw_full_topic_name_validation_result_string(validation_result);
      RMW_SET_ERROR_MSG_WITH_FORMAT_STRING("invalid topic name: %s", reason);
      return nullptr;
    }
  }

  RMW_CHECK_ARGUMENT_FOR_NULL(publisher_options, nullptr);

  rmw_publisher_t * pub = create_publisher(qos_policies,
                                    publisher_options,
                                    type_supports,
                                    topic_name);

  if (pub == nullptr) {
    return nullptr;
  }

  return pub;
}

extern "C" rmw_ret_t rmw_get_gid_for_publisher(const rmw_publisher_t * publisher, rmw_gid_t * gid)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(publisher, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    publisher,
    publisher->implementation_identifier,
    stub_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_ARGUMENT_FOR_NULL(gid, RMW_RET_INVALID_ARGUMENT);

  gid->implementation_identifier = stub_identifier;

  memset(gid->data, 0, sizeof(gid->data));

  auto stub_pub = static_cast<const StubPublisher *>(publisher->data);

  assert(sizeof(stub_pub->pubiid) <= sizeof(gid->data));

  memcpy(gid->data, &stub_pub->pubiid, sizeof(stub_pub->pubiid));

  return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_compare_gids_equal(
  const rmw_gid_t * gid1, const rmw_gid_t * gid2,
  bool * result)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(gid1, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    gid1,
    gid1->implementation_identifier,
    stub_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_ARGUMENT_FOR_NULL(gid2, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    gid2,
    gid2->implementation_identifier,
    stub_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_ARGUMENT_FOR_NULL(result, RMW_RET_INVALID_ARGUMENT);
  /* alignment is potentially lost because of the translation to an array of bytes, so use
     memcmp instead of a simple integer comparison */
  *result = memcmp(gid1->data, gid2->data, sizeof(gid1->data)) == 0;
  return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_publisher_count_matched_subscriptions(
  const rmw_publisher_t * publisher,
  size_t * subscription_count)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(publisher, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    publisher,
    publisher->implementation_identifier,
    stub_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_ARGUMENT_FOR_NULL(subscription_count, RMW_RET_INVALID_ARGUMENT);

  auto stub_pub = static_cast<StubPublisher *>(publisher->data);

  *subscription_count = stub_pub->get_subscription_count();

  return RMW_RET_OK;
}

rmw_ret_t rmw_publisher_assert_liveliness(const rmw_publisher_t * publisher)
{
  RET_NULL(publisher);
  // // RET_WRONG_IMPLID(publisher);
  // auto pub = static_cast<CddsPublisher *>(publisher->data);
  // if (dds_assert_liveliness(pub->enth) < 0) {
  //   return RMW_RET_ERROR;
  // }
  RCUTILS_LOG_ERROR_NAMED(
    "rmw_stub.cpp",
    "rmw_publisher_assert_liveliness not supported (yet)");
  return RMW_RET_UNSUPPORTED;
}

rmw_ret_t rmw_publisher_get_actual_qos(const rmw_publisher_t * publisher, rmw_qos_profile_t * qos)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(publisher, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    publisher,
    publisher->implementation_identifier,
    stub_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_ARGUMENT_FOR_NULL(qos, RMW_RET_INVALID_ARGUMENT);

  auto stub_pub = static_cast<StubPublisher *>(publisher->data);

  qos = stub_pub->get_qos_policies();

  return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_borrow_loaned_message(
  const rmw_publisher_t * publisher,
  const rosidl_message_type_support_t * type_support,
  void ** ros_message)
{
  (void) publisher;
  (void) type_support;
  (void) ros_message;
  RMW_SET_ERROR_MSG("rmw_borrow_loaned_message not implemented for rmw_stub_cpp");
  RCUTILS_LOG_ERROR_NAMED(
    "rmw_stub.cpp",
    "rmw_borrow_loaned_message not supported (yet)");
  return RMW_RET_UNSUPPORTED;
}

extern "C" rmw_ret_t rmw_return_loaned_message_from_publisher(
  const rmw_publisher_t * publisher,
  void * loaned_message)
{
  (void) publisher;
  (void) loaned_message;
  RMW_SET_ERROR_MSG(
    "rmw_return_loaned_message_from_publisher not implemented for rmw_stub_cpp");
  return RMW_RET_UNSUPPORTED;
}

extern "C" rmw_ret_t rmw_destroy_publisher(rmw_node_t * node, rmw_publisher_t * publisher)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(publisher, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    node,
    node->implementation_identifier,
    stub_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    publisher,
    publisher->implementation_identifier,
    stub_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);

  // rmw_ret_t ret = RMW_RET_OK;

  // rmw_error_state_t error_state;
  // {
  //   auto common = &node->context->impl->common;
  //   const auto cddspub = static_cast<const CddsPublisher *>(publisher->data);
  //   std::lock_guard<std::mutex> guard(common->node_update_mutex);
  //   rmw_dds_common::msg::ParticipantEntitiesInfo msg =
  //     common->graph_cache.dissociate_writer(
  //     cddspub->gid, common->gid, node->name,
  //     node->namespace_);
  //   rmw_ret_t publish_ret =
  //     rmw_publish(common->pub, static_cast<void *>(&msg), nullptr);
  //   if (RMW_RET_OK != publish_ret) {
  //     error_state = *rmw_get_error_state();
  //     ret = publish_ret;
  //     rmw_reset_error();
  //   }
  // }

  // rmw_ret_t inner_ret = destroy_publisher(publisher);
  // if (RMW_RET_OK != inner_ret) {
  //   if (RMW_RET_OK != ret) {
  //     RMW_SAFE_FWRITE_TO_STDERR(rmw_get_error_string().str);
  //     RMW_SAFE_FWRITE_TO_STDERR(" during '" RCUTILS_STRINGIFY(__function__) "'\n");
  //   } else {
  //     error_state = *rmw_get_error_state();
  //     ret = inner_ret;
  //   }
  //   rmw_reset_error();
  // }

  // if (RMW_RET_OK != ret) {
  //   rmw_set_error_state(error_state.message, error_state.file, error_state.line_number);
  // }

    RMW_SET_ERROR_MSG(
      "rmw_destroy_publisher not implemented for rmw_stub_cpp");
    return RMW_RET_UNSUPPORTED;
}


// /////////////////////////////////////////////////////////////////////////////////////////
// ///////////                                                                   ///////////
// ///////////    SUBSCRIPTIONS                                                  ///////////
// ///////////                                                                   ///////////
// /////////////////////////////////////////////////////////////////////////////////////////

extern "C" rmw_ret_t rmw_init_subscription_allocation(
  const rosidl_message_type_support_t * type_support,
  const rosidl_runtime_c__Sequence__bound * message_bounds,
  rmw_subscription_allocation_t * allocation)
{
  static_cast<void>(type_support);
  static_cast<void>(message_bounds);
  static_cast<void>(allocation);
  RMW_SET_ERROR_MSG("rmw_init_subscription_allocation: unimplemented");
  return RMW_RET_UNSUPPORTED;
}

extern "C" rmw_ret_t rmw_fini_subscription_allocation(rmw_subscription_allocation_t * allocation)
{
  static_cast<void>(allocation);
  RMW_SET_ERROR_MSG("rmw_fini_subscription_allocation: unimplemented");
  return RMW_RET_UNSUPPORTED;
}

extern "C" rmw_subscription_t * rmw_create_subscription(
  const rmw_node_t * node, const rosidl_message_type_support_t * type_supports,
  const char * topic_name, const rmw_qos_profile_t * qos_policies,
  const rmw_subscription_options_t * subscription_options)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(node, nullptr);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    node,
    node->implementation_identifier,
    stub_identifier,
    return nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(type_supports, nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(topic_name, nullptr);
  if (0 == strlen(topic_name)) {
    RMW_SET_ERROR_MSG("topic_name argument is an empty string");
    return nullptr;
  }
  RMW_CHECK_ARGUMENT_FOR_NULL(qos_policies, nullptr);
  if (!qos_policies->avoid_ros_namespace_conventions) {
    int validation_result = RMW_TOPIC_VALID;
    rmw_ret_t ret = rmw_validate_full_topic_name(topic_name, &validation_result, nullptr);
    if (RMW_RET_OK != ret) {
      return nullptr;
    }
    if (RMW_TOPIC_VALID != validation_result) {
      const char * reason = rmw_full_topic_name_validation_result_string(validation_result);
      RMW_SET_ERROR_MSG_WITH_FORMAT_STRING("invalid topic_name argument: %s", reason);
      return nullptr;
    }
  }
  RMW_CHECK_ARGUMENT_FOR_NULL(subscription_options, nullptr);

  rmw_subscription_t * sub;
  // sub = create_subscription(
  //   node->context->impl->ppant, node->context->impl->dds_sub,
  //   type_supports, topic_name, qos_policies,
  //   subscription_options);
  if (sub == nullptr) {
    return nullptr;
  }
  auto cleanup_subscription = rcpputils::make_scope_exit(
    [sub]() {
      rmw_error_state_t error_state = *rmw_get_error_state();
      rmw_reset_error();
      // if (RMW_RET_OK != destroy_subscription(sub)) {
      //   RMW_SAFE_FWRITE_TO_STDERR(rmw_get_error_string().str);
      //   RMW_SAFE_FWRITE_TO_STDERR(" during '" RCUTILS_STRINGIFY(__function__) "' cleanup\n");
      //   rmw_reset_error();
      // }
      rmw_set_error_state(error_state.message, error_state.file, error_state.line_number);
    });

  // Update graph
  auto common = &node->context->impl->common;
  //const auto cddssub = static_cast<const CddsSubscription *>(sub->data);
  // std::lock_guard<std::mutex> guard(common->node_update_mutex);
  // rmw_dds_common::msg::ParticipantEntitiesInfo msg =
  //   common->graph_cache.associate_reader(cddssub->gid, common->gid, node->name, node->namespace_);
  // if (RMW_RET_OK != rmw_publish(
  //     common->pub,
  //     static_cast<void *>(&msg),
  //     nullptr))
  // {
  //   static_cast<void>(common->graph_cache.dissociate_reader(
  //     cddssub->gid, common->gid, node->name, node->namespace_));
  //   return nullptr;
  // }

  // cleanup_subscription.cancel();
  return sub;
}

extern "C" rmw_ret_t rmw_subscription_count_matched_publishers(
  const rmw_subscription_t * subscription, size_t * publisher_count)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(subscription, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    subscription,
    subscription->implementation_identifier,
    stub_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_ARGUMENT_FOR_NULL(publisher_count, RMW_RET_INVALID_ARGUMENT);

  // auto sub = static_cast<CddsSubscription *>(subscription->data);
  // dds_subscription_matched_status_t status;
  // if (dds_get_subscription_matched_status(sub->enth, &status) < 0) {
  //   return RMW_RET_ERROR;
  // }
  // *publisher_count = status.current_count;
  RCUTILS_LOG_ERROR_NAMED(
    "rmw_stub.cpp",
    "not supported (yet)");
  return RMW_RET_UNSUPPORTED;
}

extern "C" rmw_ret_t rmw_subscription_get_actual_qos(
  const rmw_subscription_t * subscription,
  rmw_qos_profile_t * qos)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(subscription, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    subscription,
    subscription->implementation_identifier,
    stub_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_ARGUMENT_FOR_NULL(qos, RMW_RET_INVALID_ARGUMENT);

  // auto sub = static_cast<CddsSubscription *>(subscription->data);
  // if (get_readwrite_qos(sub->enth, qos)) {
  //   return RMW_RET_OK;
  // }
  return RMW_RET_ERROR;
}


extern "C" rmw_ret_t rmw_destroy_subscription(rmw_node_t * node, rmw_subscription_t * subscription)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(subscription, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    node,
    node->implementation_identifier,
    stub_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    subscription,
    subscription->implementation_identifier,
    stub_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);

  rmw_ret_t ret = RMW_RET_OK;
  rmw_error_state_t error_state;
  rmw_error_string_t error_string;
  {
    auto common = &node->context->impl->common;
    // const auto cddssub = static_cast<const CddsSubscription *>(subscription->data);
    // std::lock_guard<std::mutex> guard(common->node_update_mutex);
    // rmw_dds_common::msg::ParticipantEntitiesInfo msg =
    //   common->graph_cache.dissociate_writer(
    //   cddssub->gid, common->gid, node->name,
    //   node->namespace_);
    // ret = rmw_publish(common->pub, static_cast<void *>(&msg), nullptr);
    if (RMW_RET_OK != ret) {
      error_state = *rmw_get_error_state();
      error_string = rmw_get_error_string();
      rmw_reset_error();
    }
  }

  rmw_ret_t local_ret; // = destroy_subscription(subscription);
  if (RMW_RET_OK != local_ret) {
    if (RMW_RET_OK != ret) {
      RMW_SAFE_FWRITE_TO_STDERR(error_string.str);
      RMW_SAFE_FWRITE_TO_STDERR(" during '" RCUTILS_STRINGIFY(__function__) "'\n");
    }
    ret = local_ret;
  } else if (RMW_RET_OK != ret) {
    rmw_set_error_state(error_state.message, error_state.file, error_state.line_number);
  }

  return ret;
}


extern "C" rmw_ret_t rmw_take(
  const rmw_subscription_t * subscription, void * ros_message,
  bool * taken, rmw_subscription_allocation_t * allocation)
{
  static_cast<void>(allocation);
  // return rmw_take_int(subscription, ros_message, taken, nullptr);
  return RMW_RET_UNSUPPORTED;
}

extern "C" rmw_ret_t rmw_take_with_info(
  const rmw_subscription_t * subscription, void * ros_message,
  bool * taken, rmw_message_info_t * message_info,
  rmw_subscription_allocation_t * allocation)
{
  static_cast<void>(allocation);
  RMW_CHECK_ARGUMENT_FOR_NULL(message_info, RMW_RET_INVALID_ARGUMENT);
  // return rmw_take_int(subscription, ros_message, taken, message_info);
  return RMW_RET_UNSUPPORTED;
}

extern "C" rmw_ret_t rmw_take_sequence(
  const rmw_subscription_t * subscription, size_t count,
  rmw_message_sequence_t * message_sequence,
  rmw_message_info_sequence_t * message_info_sequence,
  size_t * taken, rmw_subscription_allocation_t * allocation)
{
  static_cast<void>(allocation);
  // return rmw_take_seq(subscription, count, message_sequence, message_info_sequence, taken);
  return RMW_RET_UNSUPPORTED;
}

extern "C" rmw_ret_t rmw_take_serialized_message(
  const rmw_subscription_t * subscription,
  rmw_serialized_message_t * serialized_message,
  bool * taken,
  rmw_subscription_allocation_t * allocation)
{
  static_cast<void>(allocation);
  // return rmw_take_ser_int(subscription, serialized_message, taken, nullptr);
  return RMW_RET_UNSUPPORTED;
}

extern "C" rmw_ret_t rmw_take_serialized_message_with_info(
  const rmw_subscription_t * subscription,
  rmw_serialized_message_t * serialized_message, bool * taken, rmw_message_info_t * message_info,
  rmw_subscription_allocation_t * allocation)
{
  static_cast<void>(allocation);

  RMW_CHECK_ARGUMENT_FOR_NULL(
    message_info, RMW_RET_INVALID_ARGUMENT);

  // return rmw_take_ser_int(subscription, serialized_message, taken, message_info);
  return RMW_RET_UNSUPPORTED;
}

extern "C" rmw_ret_t rmw_take_loaned_message(
  const rmw_subscription_t * subscription,
  void ** loaned_message,
  bool * taken,
  rmw_subscription_allocation_t * allocation)
{
  (void) subscription;
  (void) loaned_message;
  (void) taken;
  (void) allocation;
  RMW_SET_ERROR_MSG("rmw_take_loaned_message not implemented for rmw_stub_cpp");
  return RMW_RET_UNSUPPORTED;
}

extern "C" rmw_ret_t rmw_take_loaned_message_with_info(
  const rmw_subscription_t * subscription,
  void ** loaned_message,
  bool * taken,
  rmw_message_info_t * message_info,
  rmw_subscription_allocation_t * allocation)
{
  (void) subscription;
  (void) loaned_message;
  (void) taken;
  (void) message_info;
  (void) allocation;
  RMW_SET_ERROR_MSG("rmw_take_loaned_message_with_info not implemented for rmw_stub_cpp");
  return RMW_RET_UNSUPPORTED;
}

extern "C" rmw_ret_t rmw_return_loaned_message_from_subscription(
  const rmw_subscription_t * subscription,
  void * loaned_message)
{
  (void) subscription;
  (void) loaned_message;
  RMW_SET_ERROR_MSG(
    "rmw_return_loaned_message_from_subscription not implemented for rmw_stub_cpp");
  return RMW_RET_UNSUPPORTED;
}

/////////////////////////////////////////////////////////////////////////////////////////
///////////                                                                   ///////////
///////////    EVENTS                                                         ///////////
///////////                                                                   ///////////
/////////////////////////////////////////////////////////////////////////////////////////

extern "C" rmw_ret_t rmw_publisher_event_init(
  rmw_event_t * rmw_event, const rmw_publisher_t * publisher, rmw_event_type_t event_type)
{
  RET_NULL(publisher);
  // RET_WRONG_IMPLID(publisher);
  // return init_rmw_event(
  //   rmw_event,
  //   publisher->implementation_identifier,
  //   publisher->data,
  //   event_type);
  return RMW_RET_UNSUPPORTED;
}

extern "C" rmw_ret_t rmw_subscription_event_init(
  rmw_event_t * rmw_event, const rmw_subscription_t * subscription, rmw_event_type_t event_type)
{
  RET_NULL(subscription);
  // RET_WRONG_IMPLID(subscription);
  // return init_rmw_event(
  //   rmw_event,
  //   subscription->implementation_identifier,
  //   subscription->data,
  //   event_type);
  return RMW_RET_UNSUPPORTED;
}

extern "C" rmw_ret_t rmw_take_event(
  const rmw_event_t * event_handle, void * event_info,
  bool * taken)
{
  RET_NULL(event_handle);
  // RET_WRONG_IMPLID(event_handle);
  RET_NULL(taken);
  RET_NULL(event_info);

  RCUTILS_LOG_ERROR_NAMED(
    "rmw_stub.cpp",
    "rmw_take_event not implemented");
  return RMW_RET_UNSUPPORTED;
}

/////////////////////////////////////////////////////////////////////////////////////////
///////////                                                                   ///////////
///////////    GUARDS AND WAITSETS                                            ///////////
///////////                                                                   ///////////
/////////////////////////////////////////////////////////////////////////////////////////

extern "C" rmw_guard_condition_t * rmw_create_guard_condition(rmw_context_t * context)
{
  (void)context;

  auto * guard_condition_implem = new StubGuardCondition();

  rmw_guard_condition_t * guard_condition_handle = new rmw_guard_condition_t;
  guard_condition_handle->implementation_identifier = stub_identifier;
  guard_condition_handle->data = guard_condition_implem;

  return guard_condition_handle;
}

extern "C" rmw_ret_t rmw_destroy_guard_condition(rmw_guard_condition_t * rmw_guard_condition)
{
  RET_NULL(rmw_guard_condition);
  auto stub_guard_condition = static_cast<StubGuardCondition *>(rmw_guard_condition->data);
  delete stub_guard_condition;

  return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_guard_condition_set_listener_callback(
  const void * user_data,
  rmw_listener_cb_t callback,
  const void * guard_condition_handle,
  rmw_guard_condition_t * rmw_guard_condition,
  bool use_previous_events)
{
  RET_NULL(rmw_guard_condition);
  auto stub_guard_condition = static_cast<StubGuardCondition *>(rmw_guard_condition->data);
  stub_guard_condition->set_callback(user_data, callback, guard_condition_handle, use_previous_events);

  return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_trigger_guard_condition(
  const rmw_guard_condition_t * rmw_guard_condition)
{
  RET_NULL(rmw_guard_condition);
  auto stub_guard_condition = static_cast<StubGuardCondition *>(rmw_guard_condition->data);
  stub_guard_condition->trigger();

  return RMW_RET_OK;
}

extern "C" rmw_wait_set_t * rmw_create_wait_set(rmw_context_t * context, size_t max_conditions)
{
  (void)max_conditions;
  RMW_CHECK_ARGUMENT_FOR_NULL(context, nullptr);

  rmw_wait_set_t * wait_set = rmw_wait_set_allocate();
  wait_set->implementation_identifier = stub_identifier;
  wait_set->data = nullptr;
  return wait_set;
}

extern "C" rmw_ret_t rmw_destroy_wait_set(rmw_wait_set_t * wait_set)
{
  RET_NULL(wait_set);

  rmw_free(wait_set->data);
  rmw_wait_set_free(wait_set);

  return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_wait(
  rmw_subscriptions_t * subs, rmw_guard_conditions_t * gcs,
  rmw_services_t * srvs, rmw_clients_t * cls, rmw_events_t * evs,
  rmw_wait_set_t * wait_set, const rmw_time_t * wait_timeout)
{
  RCUTILS_LOG_ERROR_NAMED(
    "rmw_stub.cpp",
    "rmw_wait not supported");
  return RMW_RET_UNSUPPORTED;
}

/////////////////////////////////////////////////////////////////////////////////////////
///////////                                                                   ///////////
///////////    CLIENTS AND SERVERS                                            ///////////
///////////                                                                   ///////////
/////////////////////////////////////////////////////////////////////////////////////////

extern "C" rmw_ret_t rmw_take_response(
  const rmw_client_t * client,
  rmw_service_info_t * request_header, void * ros_response,
  bool * taken)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(client, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    client,
    client->implementation_identifier, stub_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  // auto info = static_cast<CddsClient *>(client->data);
  // dds_time_t source_timestamp;
  rmw_ret_t ret; // = rmw_take_response_request(
  //   &info->client, request_header, ros_response, taken,
  //   &source_timestamp, info->client.pub->pubiid);

  RCUTILS_LOG_ERROR_NAMED(
    "rmw_stub.cpp",
    "rmw_take_response not supported");
  return RMW_RET_UNSUPPORTED;
}


extern "C" rmw_ret_t rmw_take_request(
  const rmw_service_t * service,
  rmw_service_info_t * request_header, void * ros_request,
  bool * taken)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(service, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    service,
    service->implementation_identifier, stub_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);

  RCUTILS_LOG_ERROR_NAMED(
    "rmw_stub.cpp",
    "rmw_take_request not supported");
  return RMW_RET_UNSUPPORTED;
  // auto info = static_cast<CddsService *>(service->data);
  // return rmw_take_response_request(
  //   &info->service, request_header, ros_request, taken, nullptr,
  //   false);
}

enum class client_present_t
{
  FAILURE,  // an error occurred when checking
  MAYBE,    // reader not matched, writer still present
  YES,      // reader matched
  GONE      // neither reader nor writer
};


extern "C" rmw_ret_t rmw_send_response(
  const rmw_service_t * service,
  rmw_request_id_t * request_header, void * ros_response)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(service, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    service,
    service->implementation_identifier, stub_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_ARGUMENT_FOR_NULL(request_header, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(ros_response, RMW_RET_INVALID_ARGUMENT);
  // CddsService * info = static_cast<CddsService *>(service->data);
  // cdds_request_header_t header;
  // dds_instance_handle_t reqwrih;
  // static_assert(
  //   sizeof(request_header->writer_guid) == sizeof(header.guid) + sizeof(reqwrih),
  //   "request header size assumptions not met");
  // memcpy(
  //   static_cast<void *>(&header.guid), static_cast<const void *>(request_header->writer_guid),
  //   sizeof(header.guid));
  // memcpy(
  //   static_cast<void *>(&reqwrih),
  //   static_cast<const void *>(request_header->writer_guid + sizeof(header.guid)), sizeof(reqwrih));
  // header.seq = request_header->sequence_number;
  // Block until the response reader has been matched by the response writer (this is a
  // workaround: rmw_service_server_is_available should keep returning false until this
  // is a given).
  // TODO(eboasson): rmw_service_server_is_available should block the request instead (#191)
  // client_present_t st;
  // std::chrono::system_clock::time_point tnow = std::chrono::system_clock::now();
  // std::chrono::system_clock::time_point tend = tnow + 100ms;
  // while ((st =
  //   check_for_response_reader(
  //     info->service,
  //     reqwrih)) == client_present_t::MAYBE && tnow < tend)
  // {
  //   dds_sleepfor(DDS_MSECS(10));
  //   tnow = std::chrono::system_clock::now();
  // }
  // switch (st) {
  //   case client_present_t::FAILURE:
  //     break;
  //   case client_present_t::MAYBE:
  //     return RMW_RET_TIMEOUT;
  //   case client_present_t::YES:
  //     return rmw_send_response_request(&info->service, header, ros_response);
  //   case client_present_t::GONE:
  //     return RMW_RET_OK;
  // }
  // return RMW_RET_ERROR;

  RCUTILS_LOG_ERROR_NAMED(
    "rmw_stub.cpp",
    "rmw_send_response not supported");
  return RMW_RET_UNSUPPORTED;
}

extern "C" rmw_ret_t rmw_send_request(
  const rmw_client_t * client, const void * ros_request,
  int64_t * sequence_id)
{
  static std::atomic_uint next_request_id;
  RMW_CHECK_ARGUMENT_FOR_NULL(client, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    client,
    client->implementation_identifier, stub_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_ARGUMENT_FOR_NULL(ros_request, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(sequence_id, RMW_RET_INVALID_ARGUMENT);

//   auto info = static_cast<CddsClient *>(client->data);
//   cdds_request_header_t header;
//   header.guid = info->client.pub->pubiid;
//   header.seq = *sequence_id = ++next_request_id;

// #if REPORT_BLOCKED_REQUESTS
//   {
//     std::lock_guard<std::mutex> lock(info->lock);
//     info->reqtime[header.seq] = dds_time();
//   }
// #endif

//   return rmw_send_response_request(&info->client, header, ros_request);
  RCUTILS_LOG_ERROR_NAMED(
    "rmw_stub.cpp",
    "rmw_send_request not supported");
  return RMW_RET_UNSUPPORTED;
}

extern "C" rmw_client_t * rmw_create_client(
  const rmw_node_t * node,
  const rosidl_service_type_support_t * type_supports,
  const char * service_name,
  const rmw_qos_profile_t * qos_policies)
{
//   CddsClient * info = new CddsClient();
// #if REPORT_BLOCKED_REQUESTS
//   info->lastcheck = 0;
// #endif
//   if (
//     rmw_init_cs(
//       &info->client, node, type_supports, service_name, qos_policies, false) != RMW_RET_OK)
//   {
//     delete (info);
//     return nullptr;
//   }
  rmw_client_t * rmw_client = rmw_client_allocate();
  RET_NULL_X(rmw_client, goto fail_client);
  rmw_client->implementation_identifier = stub_identifier;
  //rmw_client->data = info;
  rmw_client->service_name = reinterpret_cast<const char *>(rmw_allocate(strlen(service_name) + 1));
  RET_NULL_X(rmw_client->service_name, goto fail_service_name);
  memcpy(const_cast<char *>(rmw_client->service_name), service_name, strlen(service_name) + 1);

  {
    // Update graph
    auto common = &node->context->impl->common;
    // std::lock_guard<std::mutex> guard(common->node_update_mutex);
    // static_cast<void>(common->graph_cache.associate_writer(
    //   info->client.pub->gid, common->gid,
    //   node->name, node->namespace_));
    // rmw_dds_common::msg::ParticipantEntitiesInfo msg =
    //   common->graph_cache.associate_reader(
    //   info->client.sub->gid, common->gid, node->name,
    //   node->namespace_);
    // if (RMW_RET_OK != rmw_publish(
    //     common->pub,
    //     static_cast<void *>(&msg),
    //     nullptr))
    // {
    //   static_cast<void>(destroy_client(node, rmw_client));
    //   return nullptr;
    // }
  }

  return rmw_client;
fail_service_name:
  rmw_client_free(rmw_client);
fail_client:
  // rmw_fini_cs(&info->client);
  // delete info;
  return nullptr;
}

extern "C" rmw_ret_t rmw_destroy_client(rmw_node_t * node, rmw_client_t * client)
{
  // return destroy_client(node, client);
  RCUTILS_LOG_ERROR_NAMED(
    "rmw_stub.cpp",
    "rmw_destroy_client not supported");
  return RMW_RET_UNSUPPORTED;
}

extern "C" rmw_service_t * rmw_create_service(
  const rmw_node_t * node,
  const rosidl_service_type_support_t * type_supports,
  const char * service_name,
  const rmw_qos_profile_t * qos_policies)
{
  // CddsService * info = new CddsService();
  // if (
  //   rmw_init_cs(
  //     &info->service, node, type_supports, service_name, qos_policies, true) != RMW_RET_OK)
  // {
  //   delete (info);
  //   return nullptr;
  // }
  rmw_service_t * rmw_service = rmw_service_allocate();
  RET_NULL_X(rmw_service, goto fail_service);
  rmw_service->implementation_identifier = stub_identifier;
  //rmw_service->data = info;
  rmw_service->service_name =
    reinterpret_cast<const char *>(rmw_allocate(strlen(service_name) + 1));
  RET_NULL_X(rmw_service->service_name, goto fail_service_name);
  //memcpy(const_cast<char *>(rmw_service->service_name), service_name, strlen(service_name) + 1);

  {
    // Update graph
    //auto common = &node->context->impl->common;
    // std::lock_guard<std::mutex> guard(common->node_update_mutex);
    // static_cast<void>(common->graph_cache.associate_writer(
    //   info->service.pub->gid, common->gid,
    //   node->name, node->namespace_));
    // rmw_dds_common::msg::ParticipantEntitiesInfo msg =
    //   common->graph_cache.associate_reader(
    //   info->service.sub->gid, common->gid, node->name,
    //   node->namespace_);
    // if (RMW_RET_OK != rmw_publish(
    //     common->pub,
    //     static_cast<void *>(&msg),
    //     nullptr))
    // {
    //   static_cast<void>(destroy_service(node, rmw_service));
    //   return nullptr;
    // }
  }

  return rmw_service;
fail_service_name:
  rmw_service_free(rmw_service);
fail_service:
  //rmw_fini_cs(&info->service);
  // delete info;
  return nullptr;
}

extern "C" rmw_ret_t rmw_destroy_service(rmw_node_t * node, rmw_service_t * service)
{
  // return destroy_service(node, service);
  return RMW_RET_UNSUPPORTED;
}

/////////////////////////////////////////////////////////////////////////////////////////
///////////                                                                   ///////////
///////////    INTROSPECTION                                                  ///////////
///////////                                                                   ///////////
/////////////////////////////////////////////////////////////////////////////////////////

extern "C" rmw_ret_t rmw_get_node_names(
  const rmw_node_t * node,
  rcutils_string_array_t * node_names,
  rcutils_string_array_t * node_namespaces)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    node,
    node->implementation_identifier,
    stub_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  if (RMW_RET_OK != rmw_check_zero_rmw_string_array(node_names)) {
    return RMW_RET_INVALID_ARGUMENT;
  }
  if (RMW_RET_OK != rmw_check_zero_rmw_string_array(node_namespaces)) {
    return RMW_RET_INVALID_ARGUMENT;
  }

  // auto common_context = &node->context->impl->common;
  // rcutils_allocator_t allocator = rcutils_get_default_allocator();
  // return common_context->graph_cache.get_node_names(
  //   node_names,
  //   node_namespaces,
  //   nullptr,
  //   &allocator);
  RCUTILS_LOG_ERROR_NAMED(
    "rmw_stub.cpp",
    "rmw_get_node_names");
  return RMW_RET_UNSUPPORTED;
}

extern "C" rmw_ret_t rmw_get_node_names_with_enclaves(
  const rmw_node_t * node,
  rcutils_string_array_t * node_names,
  rcutils_string_array_t * node_namespaces,
  rcutils_string_array_t * enclaves)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    node,
    node->implementation_identifier,
    stub_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  if (RMW_RET_OK != rmw_check_zero_rmw_string_array(node_names)) {
    return RMW_RET_INVALID_ARGUMENT;
  }
  if (RMW_RET_OK != rmw_check_zero_rmw_string_array(node_namespaces)) {
    return RMW_RET_INVALID_ARGUMENT;
  }
  if (RMW_RET_OK != rmw_check_zero_rmw_string_array(enclaves)) {
    return RMW_RET_INVALID_ARGUMENT;
  }

  // auto common_context = &node->context->impl->common;
  // rcutils_allocator_t allocator = rcutils_get_default_allocator();
  // return common_context->graph_cache.get_node_names(
  //   node_names,
  //   node_namespaces,
  //   enclaves,
  //   &allocator);

  RCUTILS_LOG_ERROR_NAMED(
    "rmw_stub.cpp",
    "rmw_get_node_names_with_enclaves");
  return RMW_RET_UNSUPPORTED;
}

extern "C" rmw_ret_t rmw_get_topic_names_and_types(
  const rmw_node_t * node,
  rcutils_allocator_t * allocator,
  bool no_demangle, rmw_names_and_types_t * tptyp)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    node,
    node->implementation_identifier,
    stub_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RCUTILS_CHECK_ALLOCATOR_WITH_MSG(
    allocator, "allocator argument is invalid", return RMW_RET_INVALID_ARGUMENT);
  if (RMW_RET_OK != rmw_names_and_types_check_zero(tptyp)) {
    return RMW_RET_INVALID_ARGUMENT;
  }
  RCUTILS_LOG_ERROR_NAMED(
    "rmw_stub.cpp",
    "rmw_get_topic_names_and_types unsupported");
  return RMW_RET_UNSUPPORTED;
}

extern "C" rmw_ret_t rmw_get_service_names_and_types(
  const rmw_node_t * node,
  rcutils_allocator_t * allocator,
  rmw_names_and_types_t * sntyp)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    node,
    node->implementation_identifier,
    stub_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RCUTILS_CHECK_ALLOCATOR_WITH_MSG(
    allocator, "allocator argument is invalid", return RMW_RET_INVALID_ARGUMENT);
  if (RMW_RET_OK != rmw_names_and_types_check_zero(sntyp)) {
    return RMW_RET_INVALID_ARGUMENT;
  }

  //auto common_context = &node->context->impl->common;
  // return common_context->graph_cache.get_names_and_types(
  //   _demangle_service_from_topic,
  //   _demangle_service_type_only,
  //   allocator,
  //   sntyp);
  RCUTILS_LOG_ERROR_NAMED(
    "rmw_stub.cpp",
    "rmw_get_service_names_and_types unsupported");
  return RMW_RET_UNSUPPORTED;
}

extern "C" rmw_ret_t rmw_service_server_is_available(
  const rmw_node_t * node,
  const rmw_client_t * client,
  bool * is_available)
{
  RET_NULL(node);
  // RET_WRONG_IMPLID(node);
  RET_NULL(client);
  // RET_WRONG_IMPLID(client);
  RET_NULL(is_available);
  //*is_available = false;

  // auto info = static_cast<CddsClient *>(client->data);
  // auto common_context = &node->context->impl->common;

  // std::string sub_topic_name, pub_topic_name;
  // if (get_topic_name(info->client.pub->enth, pub_topic_name) < 0 ||
  //   get_topic_name(info->client.sub->enth, sub_topic_name) < 0)
  // {
  //   RMW_SET_ERROR_MSG("rmw_service_server_is_available: failed to get topic names");
  //   return RMW_RET_ERROR;
  // }

  // size_t number_of_request_subscribers = 0;
  // rmw_ret_t ret =
  //   common_context->graph_cache.get_reader_count(pub_topic_name, &number_of_request_subscribers);
  // if (ret != RMW_RET_OK || 0 == number_of_request_subscribers) {
  //   return ret;
  // }
  // size_t number_of_response_publishers = 0;
  // ret =
  //   common_context->graph_cache.get_writer_count(sub_topic_name, &number_of_response_publishers);
  // if (ret != RMW_RET_OK || 0 == number_of_response_publishers) {
  //   return ret;
  // }
  // return check_for_service_reader_writer(info->client, is_available);
  // return RMW_RET_ERROR;
  RCUTILS_LOG_ERROR_NAMED(
    "rmw_stub.cpp",
    "rmw_service_server_is_available");
  return RMW_RET_UNSUPPORTED;
}

extern "C" rmw_ret_t rmw_count_publishers(
  const rmw_node_t * node, const char * topic_name,
  size_t * count)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    node,
    node->implementation_identifier,
    stub_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_ARGUMENT_FOR_NULL(topic_name, RMW_RET_INVALID_ARGUMENT);
  // int validation_result = RMW_TOPIC_VALID;
  // rmw_ret_t ret = rmw_validate_full_topic_name(topic_name, &validation_result, nullptr);
  // if (RMW_RET_OK != ret) {
  //   return ret;
  // }
  // if (RMW_TOPIC_VALID != validation_result) {
  //   const char * reason = rmw_full_topic_name_validation_result_string(validation_result);
  //   RMW_SET_ERROR_MSG_WITH_FORMAT_STRING("topic_name argument is invalid: %s", reason);
  //   return RMW_RET_INVALID_ARGUMENT;
  // }
  // RMW_CHECK_ARGUMENT_FOR_NULL(count, RMW_RET_INVALID_ARGUMENT);

  // auto common_context = &node->context->impl->common;
  // const std::string mangled_topic_name = make_fqtopic(ROS_TOPIC_PREFIX, topic_name, "", false);
  // return common_context->graph_cache.get_writer_count(mangled_topic_name, count);
      RCUTILS_LOG_ERROR_NAMED(
    "rmw_stub.cpp",
    "rmw_count_publishers");
  return RMW_RET_UNSUPPORTED;
}

extern "C" rmw_ret_t rmw_count_subscribers(
  const rmw_node_t * node, const char * topic_name,
  size_t * count)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    node,
    node->implementation_identifier,
    stub_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_ARGUMENT_FOR_NULL(topic_name, RMW_RET_INVALID_ARGUMENT);
  // int validation_result = RMW_TOPIC_VALID;
  // rmw_ret_t ret = rmw_validate_full_topic_name(topic_name, &validation_result, nullptr);
  // if (RMW_RET_OK != ret) {
  //   return ret;
  // }
  // if (RMW_TOPIC_VALID != validation_result) {
  //   const char * reason = rmw_full_topic_name_validation_result_string(validation_result);
  //   RMW_SET_ERROR_MSG_WITH_FORMAT_STRING("topic_name argument is invalid: %s", reason);
  //   return RMW_RET_INVALID_ARGUMENT;
  // }
  // RMW_CHECK_ARGUMENT_FOR_NULL(count, RMW_RET_INVALID_ARGUMENT);

  // auto common_context = &node->context->impl->common;
  // const std::string mangled_topic_name = make_fqtopic(ROS_TOPIC_PREFIX, topic_name, "", false);
  // return common_context->graph_cache.get_reader_count(mangled_topic_name, count);
      RCUTILS_LOG_ERROR_NAMED(
    "rmw_stub.cpp",
    "rmw_count_subscribers");
    return RMW_RET_UNSUPPORTED;
}



extern "C" rmw_ret_t rmw_get_subscriber_names_and_types_by_node(
  const rmw_node_t * node,
  rcutils_allocator_t * allocator,
  const char * node_name,
  const char * node_namespace,
  bool no_demangle,
  rmw_names_and_types_t * tptyp)
{
  // return get_topic_names_and_types_by_node(
  //   node, allocator, node_name, node_namespace,
  //   _demangle_ros_topic_from_topic, _demangle_if_ros_type,
  //   no_demangle, get_reader_names_and_types_by_node, tptyp);
    RCUTILS_LOG_ERROR_NAMED(
    "rmw_stub.cpp",
    "rmw_get_subscriber_names_and_types_by_node");
  return RMW_RET_UNSUPPORTED;
}

extern "C" rmw_ret_t rmw_get_publisher_names_and_types_by_node(
  const rmw_node_t * node,
  rcutils_allocator_t * allocator,
  const char * node_name,
  const char * node_namespace,
  bool no_demangle,
  rmw_names_and_types_t * tptyp)
{
  // return get_topic_names_and_types_by_node(
  //   node, allocator, node_name, node_namespace,
  //   _demangle_ros_topic_from_topic, _demangle_if_ros_type,
  //   no_demangle, get_writer_names_and_types_by_node, tptyp);

  RCUTILS_LOG_ERROR_NAMED(
    "rmw_stub.cpp",
    "rmw_get_publisher_names_and_types_by_node");
  return RMW_RET_UNSUPPORTED;

}

extern "C" rmw_ret_t rmw_get_service_names_and_types_by_node(
  const rmw_node_t * node,
  rcutils_allocator_t * allocator,
  const char * node_name,
  const char * node_namespace,
  rmw_names_and_types_t * sntyp)
{
  // return get_topic_names_and_types_by_node(
  //   node,
  //   allocator,
  //   node_name,
  //   node_namespace,
  //   _demangle_service_request_from_topic,
  //   _demangle_service_type_only,
  //   false,
  //   get_reader_names_and_types_by_node,
  //   sntyp);
  RCUTILS_LOG_ERROR_NAMED(
    "rmw_stub.cpp",
    "rmw_get_service_names_and_types_by_node");
  return RMW_RET_UNSUPPORTED;
}

extern "C" rmw_ret_t rmw_get_client_names_and_types_by_node(
  const rmw_node_t * node,
  rcutils_allocator_t * allocator,
  const char * node_name,
  const char * node_namespace,
  rmw_names_and_types_t * sntyp)
{
  // return get_topic_names_and_types_by_node(
  //   node,
  //   allocator,
  //   node_name,
  //   node_namespace,
  //   _demangle_service_reply_from_topic,
  //   _demangle_service_type_only,
  //   false,
  //   get_reader_names_and_types_by_node,
  //   sntyp);
        RCUTILS_LOG_ERROR_NAMED(
    "rmw_stub.cpp",
    "rmw_get_client_names_and_types_by_node");
    return RMW_RET_UNSUPPORTED;
}

extern "C" rmw_ret_t rmw_get_publishers_info_by_topic(
  const rmw_node_t * node,
  rcutils_allocator_t * allocator,
  const char * topic_name,
  bool no_mangle,
  rmw_topic_endpoint_info_array_t * publishers_info)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    node,
    node->implementation_identifier,
    stub_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RCUTILS_CHECK_ALLOCATOR_WITH_MSG(
    allocator, "allocator argument is invalid", return RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(topic_name, RMW_RET_INVALID_ARGUMENT);
  if (RMW_RET_OK != rmw_topic_endpoint_info_array_check_zero(publishers_info)) {
    return RMW_RET_INVALID_ARGUMENT;
  }

  // auto common_context = &node->context->impl->common;
  // std::string mangled_topic_name = topic_name;
  // DemangleFunction demangle_type = _identity_demangle;
  // if (!no_mangle) {
  //   mangled_topic_name = make_fqtopic(ROS_TOPIC_PREFIX, topic_name, "", false);
  //   demangle_type = _demangle_if_ros_type;
  // }
  // return common_context->graph_cache.get_writers_info_by_topic(
  //   mangled_topic_name,
  //   demangle_type,
  //   allocator,
  //   publishers_info);
      RCUTILS_LOG_ERROR_NAMED(
    "rmw_stub.cpp",
    "rmw_get_publishers_info_by_topic");
  return RMW_RET_UNSUPPORTED;
}

extern "C" rmw_ret_t rmw_get_subscriptions_info_by_topic(
  const rmw_node_t * node,
  rcutils_allocator_t * allocator,
  const char * topic_name,
  bool no_mangle,
  rmw_topic_endpoint_info_array_t * subscriptions_info)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    node,
    node->implementation_identifier,
    stub_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RCUTILS_CHECK_ALLOCATOR_WITH_MSG(
    allocator, "allocator argument is invalid", return RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(topic_name, RMW_RET_INVALID_ARGUMENT);
  if (RMW_RET_OK != rmw_topic_endpoint_info_array_check_zero(subscriptions_info)) {
    return RMW_RET_INVALID_ARGUMENT;
  }

  // auto common_context = &node->context->impl->common;
  // std::string mangled_topic_name = topic_name;
  // DemangleFunction demangle_type = _identity_demangle;
  // if (!no_mangle) {
  //   mangled_topic_name = make_fqtopic(ROS_TOPIC_PREFIX, topic_name, "", false);
  //   demangle_type = _demangle_if_ros_type;
  // }
  // return common_context->graph_cache.get_readers_info_by_topic(
  //   mangled_topic_name,
  //   demangle_type,
  //   allocator,
  //   subscriptions_info);
    RCUTILS_LOG_ERROR_NAMED(
    "rmw_stub.cpp",
    "rmw_get_subscriptions_info_by_topic");
  return RMW_RET_UNSUPPORTED;
}
