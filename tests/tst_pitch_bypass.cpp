#include "mic.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {
constexpr int kRate = 48000;
constexpr int kBlock = 256;

MicSettings clean() {
  MicSettings s;
  s.enabled = true;
  s.highPass = false;
  s.humFilter = false;
  s.voiceIsolation = false;
  s.noiseGate = false;
  s.autoLevel = false;
  s.glueComp = false;
  s.deEsser = false;
  s.limiter = false;
  s.eq.clear();
  s.tape = s.ringMod = s.megaphone = false;
  s.pitch = s.formant = 0;
  s.doubler = false;
  s.space = "none";
  s.slapDelay = s.longDelay = false;
  s.autoTune = false;
  s.autoTuneAmount = 0;
  s.volume = .5f;
  return s;
}

std::vector<float> signal(int n, float hz = 173.f, int offset = 0) {
  std::vector<float> x(n);
  for (int i = 0; i < n; ++i) {
    float t = float(i + offset) / kRate;
    x[i] = .08f * (std::sin(2.f * float(M_PI) * hz * t) +
                   .31f * std::sin(2.f * float(M_PI) * 2.f * hz * t));
  }
  return x;
}

bool near(const std::vector<float>& a, const std::vector<float>& b, float eps = 2e-6f) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (!std::isfinite(a[i]) || !std::isfinite(b[i]) || std::fabs(a[i] - b[i]) > eps) return false;
  return true;
}

void prepare(MicProcessor& p, const MicSettings& s) {
  p.setRate(kRate);
  p.setSettings(s);
}

bool testLatchBypass() {
  MicSettings s = clean();
  s.autoTune = true;
  s.autoTuneAmount = 1;
  s.autoTuneNotes = {"c"};
  MicProcessor p;
  prepare(p, s);
  for (int i = 0; i < 80; ++i) {
    auto x = signal(kBlock, 173.f, i * kBlock);
    p.process(x.data(), kBlock);
  }
  s.autoTune = false;
  s.autoTuneKey = "g";
  s.autoTuneNotes = {"f#"};
  prepare(p, s);
  for (float hz : {211.f, 239.f, 181.f}) {
    auto in = signal(kBlock, hz);
    auto out = in;
    p.process(out.data(), kBlock);
    if (!near(in, out, 0.f)) return false;
  }
  return true;
}

bool testDoublerIsAuxOnly() {
  MicSettings s = clean();
  s.doubler = true;
  s.doublerMix = .63f;
  MicProcessor p;
  prepare(p, s);
  Doubler d;
  d.init(kRate);
  auto in = signal(kBlock * 80);
  for (int block = 0; block < 80; ++block) {
    std::vector<float> expected(in.begin() + block * kBlock, in.begin() + (block + 1) * kBlock);
    std::vector<float> aux(kBlock, 0.f);
    d.process(expected.data(), aux.data(), kBlock, s.doublerMix);
    for (int i = 0; i < kBlock; ++i) expected[i] += aux[i];
    std::vector<float> actual(in.begin() + block * kBlock, in.begin() + (block + 1) * kBlock);
    p.process(actual.data(), kBlock);
    if (!near(actual, expected)) return false;
  }
  return true;
}

bool testAmountZeroMatchesFixedPitch() {
  MicSettings fixed = clean();
  fixed.pitch = 2;
  MicSettings amountZero = fixed;
  amountZero.autoTune = true;
  amountZero.autoTuneAmount = 0;
  MicProcessor a, b;
  prepare(a, fixed);
  prepare(b, amountZero);
  for (int block = 0; block < 80; ++block) {
    auto xa = signal(kBlock, 173.f, block * kBlock);
    auto xb = xa;
    a.process(xa.data(), kBlock);
    b.process(xb.data(), kBlock);
    if (block > 10 && !near(xa, xb, 3e-5f)) return false;
  }
  return true;
}
bool testDoublerDoesNotReplayOldAudio() {
  auto s = clean();
  s.doubler = true;
  MicProcessor p;
  prepare(p, s);
  for (int block = 0; block < 80; ++block) {
    auto x = signal(kBlock, 173.f, block * kBlock);
    p.process(x.data(), kBlock);
  }
  s.doubler = false;
  p.setSettings(s);
  for (int block = 0; block < 80; ++block) {
    std::vector<float> silence(kBlock, 0.f);
    p.process(silence.data(), kBlock);
  }
  s.doubler = true;
  p.setSettings(s);
  for (int block = 0; block < 80; ++block) {
    std::vector<float> silence(kBlock, 0.f), out = silence;
    p.process(out.data(), kBlock);
    if (!near(out, silence, 0.f)) return false;
  }
  return true;
}

bool testAmountZeroBypasses() {
  auto s = clean();
  s.autoTune = true;
  s.autoTuneNotes = {"c"};
  MicProcessor p;
  prepare(p, s);
  for (int block = 0; block < 80; ++block) {
    auto in = signal(kBlock, 211.f, block * kBlock);
    auto out = in;
    p.process(out.data(), kBlock);
    if (!near(in, out, 0.f)) return false;
  }
  return true;
}

bool testAmountZeroPreservesDelayTail() {
  auto s = clean();
  s.autoTune = true;
  s.autoTuneAmount = 1;
  s.autoTuneNotes = {"c"};
  s.longDelay = true;
  s.longDelayTime = 0;
  s.longDelayFeedback = .8f;
  s.longDelayMix = .8f;
  MicProcessor p;
  prepare(p, s);
  for (int block = 0; block < 100; ++block) {
    auto x = signal(kBlock, 173.f, block * kBlock);
    p.process(x.data(), kBlock);
  }

  s.autoTuneAmount = 0;
  p.setSettings(s);
  float peak = 0;
  for (int block = 0; block < 12; ++block) {
    std::vector<float> out(kBlock, 0.f);
    p.process(out.data(), kBlock);
    for (float sample : out) peak = std::max(peak, std::fabs(sample));
  }
  return peak > 1e-3f;
}

bool testOffWithFixedPitch() {
  auto s = clean();
  s.autoTune = true;
  s.autoTuneAmount = 1;
  s.autoTuneSpeed = 0; // A slow glide must not survive the off switch.
  s.autoTuneNotes = {"c"};
  s.pitch = s.formant = 12; // Plain octave shift, no formant processor.
  MicProcessor p;
  prepare(p, s);
  int offset = 0;
  for (int block = 0; block < 400; ++block) {
    auto x = signal(kBlock, 220.f, offset);
    p.process(x.data(), kBlock);
    offset += kBlock;
  }
  s.autoTune = false;
  p.setSettings(s);
  std::vector<float> out;
  for (int block = 0; block < 20; ++block) {
    auto x = signal(kBlock, 220.f, offset);
    p.process(x.data(), kBlock);
    offset += kBlock;
    out.insert(out.end(), x.begin(), x.end());
  }
  // Measure the rendered frequency during the first 100 ms after switching
  // off. Ignore the initial delayed samples already in the shifter buffer.
  std::vector<int> crossings;
  for (int i = 1800; i < int(out.size()); ++i)
    if (out[i - 1] <= 0 && out[i] > 0) crossings.push_back(i);
  if (crossings.size() < 10) return false;
  float hz = float(kRate) * (crossings.size() - 1) / (crossings.back() - crossings.front());
  std::printf("Fixed pitch after correction off: %.2f Hz (expected 440)\n", hz);
  return std::fabs(hz - 440.f) < 5.f;
}
}  // namespace

int main() {
  bool ok = true;
  auto check = [&](const char* name, bool result) {
    std::printf("%s: %s\n", name, result ? "PASS" : "FAIL");
    ok &= result;
  };
  check("Disabled latch bypass", testLatchBypass());
  check("Doubler preserves dry lead", testDoublerIsAuxOnly());
  check("Zero amount with fixed pitch", testAmountZeroMatchesFixedPitch());
  check("Zero amount bypass", testAmountZeroBypasses());
  check("Zero amount preserves delay tail", testAmountZeroPreservesDelayTail());
  check("Doubler clears bypassed audio", testDoublerDoesNotReplayOldAudio());
  check("Correction off with fixed pitch", testOffWithFixedPitch());
  if (!ok) std::fprintf(stderr, "pitch bypass regression test failed\n");
  return ok ? 0 : 1;
}
