///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2024-2025 anderewrey
///

#pragma once

#include <string>
#include <google/protobuf/message.h>

namespace protobuf_utils {
std::string ToString(const google::protobuf::Message& message);
}  // namespace protobuf_utils
