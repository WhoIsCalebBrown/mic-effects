#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

// A single-writer, multiple-reader audio ring. The PipeWire capture callback is
// the only writer; each output callback owns one Reader. Reader operations are
// lock-free and allocation-free, as required by PipeWire's real-time thread.
class AudioRing {
public:
  static constexpr int kCapacity = 1 << 15;
  static constexpr unsigned kPreRoll = 960;  // 20 ms at 48 kHz, beyond one output block

  struct Reader {
    // Cursor and priming flag share one atomic word so arm() cannot expose a
    // new cursor with old playback state to a concurrent process callback.
    std::atomic<uint64_t> state{1ull << 32};
    std::atomic<uint64_t> underrunSamples{0};
  };
  static_assert(std::atomic<uint64_t>::is_always_lock_free,
                "audio reader state must remain lock-free in the real-time callback");

  void reset() {
    samples_.assign(kCapacity, 0.f);
    write_.store(0, std::memory_order_relaxed);
  }

  // Start a new reader at "now". It will output silence until it has one whole
  // requested block plus kPreRoll buffered. Keeping the extra pre-roll after
  // the first read makes callback ordering harmless without growing latency.
  void arm(Reader& reader) const {
    reader.state.store(kPriming | write_.load(std::memory_order_acquire), std::memory_order_release);
  }

  void write(const float* input, int count) {
    if (!input || count <= 0) return;
    unsigned w = write_.load(std::memory_order_relaxed);
    for (int i = 0; i < count; ++i) samples_[(w + (unsigned)i) & (kCapacity - 1)] = input[i];
    write_.store(w + (unsigned)count, std::memory_order_release);
  }

  // Always fills and returns the (possibly capacity-clamped) request. Planned
  // silence while priming is not an underrun; starvation after playback has
  // started is, and is exposed separately for the virtual microphone.
  int read(Reader& reader, float* output, int count, bool countUnderruns) const {
    if (!output || count <= 0) return 0;
    count = std::min(count, kCapacity / 4);

    uint64_t state = reader.state.load(std::memory_order_acquire);
    // Snapshot the reader first. If arm() wins before this load, its cursor was
    // taken from an earlier write head and this later writer snapshot cannot be
    // behind it. If arm() wins afterward, the CAS below leaves it untouched.
    unsigned w = write_.load(std::memory_order_acquire);
    unsigned r = (unsigned)state;
    unsigned available = w - r;

    if (state & kPriming) {
      const unsigned target = (unsigned)count + kPreRoll;
      if (available < target) {
        std::memset(output, 0, (size_t)count * sizeof(float));
        return count;
      }
    }

    // A stopped or slower reader must not replay old, overwritten samples.
    // Retain the same bounded pre-roll used at startup instead of jumping all
    // the way to the live edge and immediately risking another underrun.
    if (available > kCapacity / 2) {
      const unsigned target = std::min<unsigned>((unsigned)count + kPreRoll, kCapacity / 2);
      r = w - target;
      available = target;
    }

    int take = std::min<int>(count, (int)available);
    for (int i = 0; i < take; ++i) output[i] = samples_[(r + (unsigned)i) & (kCapacity - 1)];
    if (take < count) {
      std::memset(output + take, 0, (size_t)(count - take) * sizeof(float));
      if (countUnderruns)
        reader.underrunSamples.fetch_add((uint64_t)(count - take), std::memory_order_relaxed);
    }
    // Do not overwrite a concurrent arm() from a new consumer session.
    reader.state.compare_exchange_strong(state, r + (unsigned)take,
                                         std::memory_order_release, std::memory_order_relaxed);
    return count;
  }

  uint64_t underrunSamples(const Reader& reader) const {
    return reader.underrunSamples.load(std::memory_order_relaxed);
  }

private:
  static constexpr uint64_t kPriming = 1ull << 32;
  std::vector<float> samples_;
  std::atomic<unsigned> write_{0};
};
