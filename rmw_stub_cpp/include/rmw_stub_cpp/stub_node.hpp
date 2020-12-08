#ifndef STUB_NODE_HPP_
#define STUB_NODE_HPP_

#include "rmw_stub_cpp/stub_guard_condition.hpp"

class StubNode
{
public:
  StubNode()
  {
    std::cout << "Create StubNode" << std::endl;
    graph_guard_condition = new rmw_guard_condition_t;
    node_guard_condition = new StubGuardCondition;
  }

  ~StubNode()
  {
    delete node_guard_condition;
    delete graph_guard_condition;
  }

  StubGuardCondition * get_node_guard_condition()
  {
    return node_guard_condition;
  }

  rmw_guard_condition_t * get_node_graph_guard_condition()
  {
    return graph_guard_condition;
  }

private:
    StubGuardCondition * node_guard_condition{nullptr};
    rmw_guard_condition_t * graph_guard_condition{nullptr};
};

#endif  // STUB_NODE_HPP_
