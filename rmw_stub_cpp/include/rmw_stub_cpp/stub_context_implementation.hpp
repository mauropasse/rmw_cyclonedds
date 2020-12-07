#ifndef STUB_CONTEXT_IMPLEMENTATION_HPP_
#define STUB_CONTEXT_IMPLEMENTATION_HPP_

struct rmw_context_impl_t
{
  /// Pointer to `rmw_dds_common::Context`.
  void * common;
  /// Pointer to `rmw_fastrtps_shared_cpp::CustomParticipantInfo`.
  void * participant_info;

  /* Participant reference count*/
  size_t node_count{0};
  std::mutex initialization_mutex;

  /* Shutdown flag */
  bool is_shutdown{false};

  /* suffix for GUIDs to construct unique client/service ids
     (protected by initialization_mutex) */
  uint32_t client_service_id;

  rmw_context_impl_t()
  : common(), client_service_id(0)
  {
    /* destructor relies on these being initialized properly */
  }

  // Initializes the participant, if it wasn't done already.
  // node_count is increased
  rmw_ret_t
  init(rmw_init_options_t * options, size_t domain_id);

  // Destroys the participant, when node_count reaches 0.
  rmw_ret_t
  fini();

  ~rmw_context_impl_t()
  {
    if (0u != this->node_count) {
      RCUTILS_SAFE_FWRITE_TO_STDERR(
        "Not all nodes were finished before finishing the context\n."
        "Ensure `rcl_node_fini` is called for all nodes before `rcl_context_fini`,"
        "to avoid leaking.\n");
    }
  }

private:
  void
  clean_up();
};

#endif  // STUB_CONTEXT_IMPLEMENTATION_HPP_