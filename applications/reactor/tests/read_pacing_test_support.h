///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#pragma once

#include <gtest/gtest.h>

#include <concepts>
#include <type_traits>

#include "applications/reactor/reactor_client.h"

/// Shared pieces for the reading reactors' tests, which run the same scenarios once per ReadPacing.
/// The pacing is a template parameter of the reactor, so the tests select it with GoogleTest typed
/// tests over the tags below, rather than with a runtime parameter.
namespace pacing_test {

using RpcReactor::Client::ReadPacing;

/// One ReadPacing value carried as a type, so it can be an element of a GoogleTest type list.
template <ReadPacing kPacing>
struct PacingTag : std::integral_constant<ReadPacing, kPacing> {};

/// One named tag per pacing. A distinct type, rather than a bare std::integral_constant, is what makes
/// GoogleTest print `TypeParam = pacing_test::TurnByTurn`, and CMake's gtest_discover_tests() name each
/// CTest case `Suite.Test<pacing_test::TurnByTurn>`, so a failure report says which pacing broke.
struct Continuous : PacingTag<ReadPacing::kContinuous> {};
struct PerMessage : PacingTag<ReadPacing::kPerMessage> {};
struct TurnByTurn : PacingTag<ReadPacing::kTurnByTurn> {};

/// Every pacing: for the behaviours that must not depend on who arms the next read.
using AllPacings = ::testing::Types<Continuous, PerMessage, TurnByTurn>;

/// The pacings that can hold a delivered message: kTurnByTurn always, kPerMessage when read_ok returns true.
using HeldPacings = ::testing::Types<PerMessage, TurnByTurn>;

/// What a read_ok callback returns to keep its message held: true under kPerMessage, and nothing under
/// kTurnByTurn, where holding is the only outcome. Lets one callback body serve both held pacings, since
/// `return KeepHeld<kPacing>();` is valid in a function returning void.
template <ReadPacing kPacing>
requires(kPacing != ReadPacing::kContinuous)
constexpr auto KeepHeld() {
  if constexpr (kPacing == ReadPacing::kPerMessage) {
    return true;
  } else {
    return;
  }
}

/// Whether `ReactorT` offers GetResponse(), which only exists under Delivery::kPull with a pacing that holds.
template <class ReactorT, class MessageT>
concept HasGetResponse = requires(ReactorT& reactor, MessageT& message) {
  { reactor.GetResponse(message) } -> std::same_as<bool>;
};

/// Whether `ReactorT` offers ResumeRead(), which only exists under Delivery::kPush with a pacing that holds.
template <class ReactorT>
concept HasResumeRead = requires(ReactorT& reactor) {
  { reactor.ResumeRead() } -> std::same_as<bool>;
};

}  // namespace pacing_test
