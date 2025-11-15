///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2024-2025 anderewrey
///

#include "protobuf_utils/protobuf_utils.h"

#include <string>

#include <google/protobuf/text_format.h>  // NOLINT(build/include_order)

std::string protobuf_utils::ToString(const google::protobuf::Message& message) {
  static google::protobuf::TextFormat::Printer printer;
  printer.SetSingleLineMode(true);
  std::string output;
  printer.PrintToString(message, &output);
  return output;
}
