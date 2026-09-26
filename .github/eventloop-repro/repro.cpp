// Standalone reproducer for missing synchronization in EventLoop's EventManager::processEvent():
// it pushes onto the event queue and notifies the condition variable without holding m_mutex,
// while EventManager::eventLoop() reads and pops that same queue under m_mutex.
// Uses only EventLoop's public API, no gRPC.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "EventLoop.h"

namespace {

std::atomic<long> handled{0};

bool WaitForHandled(long target, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (handled.load() < target) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::yield();
  }
  return true;
}

}  // namespace

int main() {
  EventLoop::RegisterEvent("tick", [](EventLoop::Event*) { handled.fetch_add(1); });
  EventLoop::SetMode(EventLoop::Mode::NON_BLOCK);
  EventLoop::Run();

  // Part 1 - lost wakeup. One producer, one event in flight at a time, so producers never race
  // each other. The producer can push and notify between the loop's predicate check and its wait(),
  // and the event then stays queued until the next TriggerEvent() wakes the loop.
  constexpr int kSequential = 20000;
  int stalls = 0;
  for (int i = 0; i < kSequential; ++i) {
    const long target = handled.load() + 1;
    EventLoop::TriggerEvent("tick");
    if (!WaitForHandled(target, std::chrono::milliseconds(200))) {
      ++stalls;
      EventLoop::TriggerEvent("tick");  // wakes the loop, which then handles both events
      WaitForHandled(target + 1, std::chrono::seconds(5));
    }
  }
  std::printf("part 1: %d of %d sequential events stalled >= 200 ms (lost wakeup)\n", stalls,
              kSequential);

  // Part 2 - concurrent producers push onto the same unsynchronized std::queue.
  constexpr int kProducers = 8;
  constexpr int kPerProducer = 20000;
  const long base = handled.load();
  std::vector<std::thread> producers;
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([] {
      for (int i = 0; i < kPerProducer; ++i) EventLoop::TriggerEvent("tick");
    });
  }
  for (auto& t : producers) t.join();
  const long expected = base + static_cast<long>(kProducers) * kPerProducer;
  WaitForHandled(expected, std::chrono::seconds(10));
  const long missing = expected - handled.load();
  std::printf("part 2: %ld of %d concurrently triggered events never handled\n", missing,
              kProducers * kPerProducer);

  EventLoop::Halt();
  return (stalls > 0 || missing != 0) ? 1 : 0;
}
