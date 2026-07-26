///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2025 anderewrey
///

#ifndef PROTOBUF_UTILS_PROTOBUF_UTILS_H_
#define PROTOBUF_UTILS_PROTOBUF_UTILS_H_

#include <string>

namespace google::protobuf { class Message; }

namespace protobuf_utils {
std::string ToString(const google::protobuf::Message& message);
}  // namespace protobuf_utils

#endif  // PROTOBUF_UTILS_PROTOBUF_UTILS_H_
