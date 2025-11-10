///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2024-2025 anderewrey
///

#include "protobuf_utils/protobuf_utils.h"

#include <google/protobuf/text_format.h>

std::string protobuf_utils::ToString(const google::protobuf::Message& message) {
  static google::protobuf::TextFormat::Printer printer;
  printer.SetSingleLineMode(true);
  std::string output;
  printer.PrintToString(message, &output);
  return output;
}
