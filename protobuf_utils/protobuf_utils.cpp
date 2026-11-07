///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2025 anderewrey
///

#include "protobuf_utils/protobuf_utils.h"

#include <string>

#include <google/protobuf/text_format.h>  // NOLINT(build/include_order)

namespace {
// Configured once, at construction, so the shared instance is only read afterwards: PrintToString() is const,
// and ToString() runs on any thread, on several at once in the servers.
class SingleLinePrinter final : public google::protobuf::TextFormat::Printer {
 public:
  SingleLinePrinter() { SetSingleLineMode(true); }
};
}  // anonymous namespace

std::string protobuf_utils::ToString(const google::protobuf::Message& message) {
  static const SingleLinePrinter printer;
  std::string output;
  printer.PrintToString(message, &output);
  return output;
}
