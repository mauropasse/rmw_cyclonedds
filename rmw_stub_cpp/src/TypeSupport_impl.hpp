// Copyright 2016 Proyectos y Sistemas de Mantenimiento SL (eProsima).
// Copyright 2018 ADLINK Technology
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

#ifndef TYPESUPPORT_IMPL_HPP_
#define TYPESUPPORT_IMPL_HPP_

#include <cassert>
#include <functional>
#include <string>
#include <vector>

#include "TypeSupport.hpp"
#include "macros.hpp"
#include "rosidl_typesupport_introspection_cpp/field_types.hpp"
#include "rosidl_typesupport_introspection_cpp/message_introspection.hpp"
#include "rosidl_typesupport_introspection_cpp/service_introspection.hpp"

#include "rosidl_typesupport_introspection_c/message_introspection.h"
#include "rosidl_typesupport_introspection_c/service_introspection.h"

#include "rosidl_runtime_c/primitives_sequence_functions.h"
#include "rosidl_runtime_c/u16string_functions.h"

#include "u16string.hpp"

namespace rmw_stub_cpp
{

template<typename T>
struct GenericCSequence;

// multiple definitions of ambiguous primitive types
SPECIALIZE_GENERIC_C_SEQUENCE(bool, bool)
SPECIALIZE_GENERIC_C_SEQUENCE(byte, uint8_t)
SPECIALIZE_GENERIC_C_SEQUENCE(char, char)
SPECIALIZE_GENERIC_C_SEQUENCE(float32, float)
SPECIALIZE_GENERIC_C_SEQUENCE(float64, double)
SPECIALIZE_GENERIC_C_SEQUENCE(int8, int8_t)
SPECIALIZE_GENERIC_C_SEQUENCE(int16, int16_t)
SPECIALIZE_GENERIC_C_SEQUENCE(uint16, uint16_t)
SPECIALIZE_GENERIC_C_SEQUENCE(int32, int32_t)
SPECIALIZE_GENERIC_C_SEQUENCE(uint32, uint32_t)
SPECIALIZE_GENERIC_C_SEQUENCE(int64, int64_t)
SPECIALIZE_GENERIC_C_SEQUENCE(uint64, uint64_t)

template<typename MembersType>
TypeSupport<MembersType>::TypeSupport()
{
  name = "";
}

template<typename MembersType>
void TypeSupport<MembersType>::setName(const std::string & name)
{
  this->name = std::string(name);
}

template<typename T>
static inline T
align_int_(size_t __align, T __int) noexcept
{
  return (__int - 1u + __align) & ~(__align - 1);
}


template<typename MembersType>
std::string TypeSupport<MembersType>::getName()
{
  return name;
}

}  // namespace rmw_stub_cpp

#endif  // TYPESUPPORT_IMPL_HPP_
