#include "audio_ring.hpp"

#include <algorithm>
#include <cassert>
#include <vector>

namespace {
std::vector<float> sequence(unsigned first, int count) {
  std::vector<float> result(count);
  for (int i = 0; i < count; ++i) result[i] = (float)(first + (unsigned)i);
  return result;
}

bool allZero(const std::vector<float>& values) {
  return std::all_of(values.begin(), values.end(), [](float value) { return value == 0.f; });
}

// Once priming ends, every output sample must be the untouched input sequence,
// independent of which callback PipeWire runs first in a graph cycle.
void testOrdering(int quantum, bool captureFirst) {
  AudioRing ring;
  AudioRing::Reader reader;
  ring.reset();
  ring.arm(reader);
  std::vector<float> output(quantum);
  unsigned written = 1, expected = 1;
  bool started = false;

  for (int cycle = 0; cycle < 40; ++cycle) {
    auto input = sequence(written, quantum);
    if (captureFirst) { ring.write(input.data(), quantum); written += (unsigned)quantum; }
    ring.read(reader, output.data(), quantum, true);
    if (!started && !allZero(output)) started = true;
    if (started) {
      assert(output.front() == (float)expected);
      assert(output.back() == (float)(expected + (unsigned)quantum - 1));
      expected += (unsigned)quantum;
    } else {
      assert(allZero(output));
    }
    if (!captureFirst) { ring.write(input.data(), quantum); written += (unsigned)quantum; }
  }
  assert(started);
  assert(ring.underrunSamples(reader) == 0);
}

void testTwoReadersMatch() {
  constexpr int quantum = 256;
  AudioRing ring;
  AudioRing::Reader source, monitor;
  ring.reset();
  ring.arm(source);
  ring.arm(monitor);
  std::vector<float> a(quantum), b(quantum);
  unsigned next = 1;
  for (int cycle = 0; cycle < 20; ++cycle) {
    auto input = sequence(next, quantum);
    next += quantum;
    ring.write(input.data(), quantum);
    ring.read(source, a.data(), quantum, true);
    ring.read(monitor, b.data(), quantum, false);
    assert(a == b);
  }
}

void testRearmAfterPhysicalWrap() {
  constexpr int quantum = 512;
  AudioRing ring;
  AudioRing::Reader reader;
  ring.reset();
  ring.arm(reader);
  unsigned next = 1;
  // Advance far enough to wrap the backing array and make old history stale.
  for (int block = 0; block < 80; ++block) {
    auto input = sequence(next, quantum);
    next += quantum;
    ring.write(input.data(), quantum);
  }
  ring.arm(reader);
  std::vector<float> output(quantum);
  ring.read(reader, output.data(), quantum, true);
  assert(allZero(output));
  unsigned anchor = next;
  for (int block = 0; block < 3; ++block) {
    auto input = sequence(next, quantum);
    next += quantum;
    ring.write(input.data(), quantum);
  }
  ring.read(reader, output.data(), quantum, true);
  assert(output.front() == (float)anchor);
  assert(output.back() == (float)(anchor + quantum - 1));
  assert(ring.underrunSamples(reader) == 0);
}

void testPostPrimeUnderrunTelemetry() {
  constexpr int quantum = 1024;
  AudioRing ring;
  AudioRing::Reader reader;
  ring.reset();
  ring.arm(reader);
  auto input = sequence(1, quantum + (int)AudioRing::kPreRoll);
  ring.write(input.data(), (int)input.size());
  std::vector<float> output(quantum);
  ring.read(reader, output.data(), quantum, true);
  ring.read(reader, output.data(), quantum, true);
  assert(ring.underrunSamples(reader) == 64);
}
}  // namespace

int main() {
  for (int quantum : {128, 256, 1024, 2048}) {
    testOrdering(quantum, true);
    testOrdering(quantum, false);
  }
  testTwoReadersMatch();
  testRearmAfterPhysicalWrap();
  testPostPrimeUnderrunTelemetry();
  return 0;
}
