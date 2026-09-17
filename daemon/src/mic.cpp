#include "mic.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#if defined(__x86_64__) || defined(__i386__)
#include <pmmintrin.h>
#include <xmmintrin.h>
#endif

namespace {

constexpr float kPi = 3.14159265358979323846f;

float dbToLin(float db) { return std::pow(10.f, db / 20.f); }
float linToDb(float v) { return 20.f * std::log10(std::max(v, 1e-7f)); }
// One-pole smoothing coefficient for a time constant in milliseconds.
float coefMs(float rate, float ms) { return ms <= 0 ? 1.f : 1.f - std::exp(-1.f / (rate * ms * 0.001f)); }
float mixf(float a, float b, float t) { return a + (b - a) * t; }
float semis(float st) { return std::exp2(st / 12.f); }   // semitones -> frequency ratio

// In-place iterative radix-2 complex FFT (n a power of two). inverse: the
// usual conjugate/scale variant.
void fft(float* re, float* im, int n, bool inverse) {
  for (int i = 1, j = 0; i < n; i++) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
  }
  for (int len = 2; len <= n; len <<= 1) {
    float ang = 2 * kPi / len * (inverse ? 1.f : -1.f);
    float wr = std::cos(ang), wi = std::sin(ang);
    for (int i = 0; i < n; i += len) {
      float cr = 1, ci = 0;
      for (int k = 0; k < len / 2; k++) {
        float ur = re[i + k], ui = im[i + k];
        float vr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci;
        float vi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
        re[i + k] = ur + vr; im[i + k] = ui + vi;
        re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
        float nr = cr * wr - ci * wi;
        ci = cr * wi + ci * wr;
        cr = nr;
      }
    }
  }
  if (inverse) for (int i = 0; i < n; i++) { re[i] /= n; im[i] /= n; }
}

}  // namespace

// ---------------------------------------------------------------------------
// Names

const std::vector<std::string>& MicSettings::stageNames() {
  // Index-aligned with enum MicStage.
  static const std::vector<std::string> v{ "hpf", "hum", "nr", "gate", "comp",
                                           "deess", "eq", "pitch", "fx", "verb" };
  return v;
}
const std::vector<std::string>& MicSettings::eqTypeNames() {
  static const std::vector<std::string> v{ "bell", "lowshelf", "highshelf", "highpass", "lowpass", "notch" };
  return v;
}
const std::vector<std::string>& MicSettings::voiceNames() {
  // These are colour effects, not pitch costumes: Pitch and Formant have their
  // own explicit controls in the preceding stage.
  static const std::vector<std::string> v{ "none", "tape", "ringmod", "megaphone" };
  return v;
}
const std::vector<std::string>& MicSettings::spaceNames() {
  static const std::vector<std::string> v{ "none", "room", "hall", "cathedral", "echo", "underwater", "trap" };
  return v;
}
const std::vector<std::string>& MicSettings::keyNames() {
  static const std::vector<std::string> v{ "c", "c#", "d", "d#", "e", "f", "f#", "g", "g#", "a", "a#", "b" };
  return v;
}
const std::vector<std::string>& MicSettings::scaleNames() {
  // Chromatic first, and the default: it allows every note, so it needs no key
  // and cannot push a word onto a note that was never in the sentence.
  static const std::vector<std::string> v{ "chromatic", "major", "minor", "pentatonic", "blues" };
  return v;
}

namespace {
// Semitones from the root that each scale allows.
const std::vector<int>& scaleSteps(const std::string& s) {
  static const std::vector<int> chromatic{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
  static const std::vector<int> major{ 0, 2, 4, 5, 7, 9, 11 };
  static const std::vector<int> minor{ 0, 2, 3, 5, 7, 8, 10 };   // natural minor
  static const std::vector<int> pentatonic{ 0, 2, 4, 7, 9 };     // major pentatonic
  static const std::vector<int> blues{ 0, 3, 5, 6, 7, 10 };
  if (s == "major") return major;
  if (s == "minor") return minor;
  if (s == "pentatonic") return pentatonic;
  if (s == "blues") return blues;
  return chromatic;
}
int keyIndex(const std::string& k) {
  const auto& n = MicSettings::keyNames();
  auto it = std::find(n.begin(), n.end(), k);
  return it == n.end() ? 0 : (int)(it - n.begin());
}
}  // namespace

// ---------------------------------------------------------------------------
// Biquad (RBJ cookbook)

void Biquad::highPass(float rate, float f, float q) {
  float w = 2 * kPi * std::min(f, rate * 0.45f) / rate, c = std::cos(w), s = std::sin(w), al = s / (2 * q);
  float a0 = 1 + al;
  b0 = (1 + c) / 2 / a0; b1 = -(1 + c) / a0; b2 = b0;
  a1 = -2 * c / a0; a2 = (1 - al) / a0;
}
void Biquad::bandPass(float rate, float f, float q) {
  float w = 2 * kPi * std::min(f, rate * 0.45f) / rate, c = std::cos(w), sn = std::sin(w), al = sn / (2 * q);
  float a0 = 1 + al;
  b0 = al / a0; b1 = 0; b2 = -al / a0;
  a1 = -2 * c / a0; a2 = (1 - al) / a0;
}
void Biquad::lowPass(float rate, float f, float q) {
  float w = 2 * kPi * std::min(f, rate * 0.45f) / rate, c = std::cos(w), s = std::sin(w), al = s / (2 * q);
  float a0 = 1 + al;
  b0 = (1 - c) / 2 / a0; b1 = (1 - c) / a0; b2 = b0;
  a1 = -2 * c / a0; a2 = (1 - al) / a0;
}
void Biquad::peak(float rate, float f, float q, float gainDb) {
  float A = std::pow(10.f, gainDb / 40.f);
  float w = 2 * kPi * std::min(f, rate * 0.45f) / rate, c = std::cos(w), s = std::sin(w), al = s / (2 * q);
  float a0 = 1 + al / A;
  b0 = (1 + al * A) / a0; b1 = -2 * c / a0; b2 = (1 - al * A) / a0;
  a1 = -2 * c / a0; a2 = (1 - al / A) / a0;
}
void Biquad::notch(float rate, float f, float q) {
  float w = 2 * kPi * std::min(f, rate * 0.45f) / rate, c = std::cos(w), sn = std::sin(w), al = sn / (2 * q);
  float a0 = 1 + al;
  b0 = 1 / a0; b1 = -2 * c / a0; b2 = 1 / a0;
  a1 = -2 * c / a0; a2 = (1 - al) / a0;
}
void Biquad::lowShelf(float rate, float f, float gainDb) {
  float A = std::pow(10.f, gainDb / 40.f);
  float w = 2 * kPi * std::min(f, rate * 0.45f) / rate, c = std::cos(w), s = std::sin(w);
  float al = s / 2 * std::sqrt((A + 1 / A) * (1 / 0.9f - 1) + 2);
  float sq = 2 * std::sqrt(A) * al;
  float a0 = (A + 1) + (A - 1) * c + sq;
  b0 = A * ((A + 1) - (A - 1) * c + sq) / a0;
  b1 = 2 * A * ((A - 1) - (A + 1) * c) / a0;
  b2 = A * ((A + 1) - (A - 1) * c - sq) / a0;
  a1 = -2 * ((A - 1) + (A + 1) * c) / a0;
  a2 = ((A + 1) + (A - 1) * c - sq) / a0;
}
void Biquad::highShelf(float rate, float f, float gainDb) {
  float A = std::pow(10.f, gainDb / 40.f);
  float w = 2 * kPi * std::min(f, rate * 0.45f) / rate, c = std::cos(w), s = std::sin(w);
  float al = s / 2 * std::sqrt((A + 1 / A) * (1 / 0.9f - 1) + 2);
  float sq = 2 * std::sqrt(A) * al;
  float a0 = (A + 1) - (A - 1) * c + sq;
  b0 = A * ((A + 1) + (A - 1) * c + sq) / a0;
  b1 = -2 * A * ((A - 1) + (A + 1) * c) / a0;
  b2 = A * ((A + 1) + (A - 1) * c - sq) / a0;
  a1 = 2 * ((A - 1) - (A + 1) * c) / a0;
  a2 = ((A + 1) - (A - 1) * c - sq) / a0;
}

// ---------------------------------------------------------------------------
// Spectral denoise

void SpectralDenoise::init(int rate) {
  rate_ = rate;
  win_.resize(kN);
  for (int i = 0; i < kN; i++) win_[i] = 0.5f * (1 - std::cos(2 * kPi * i / kN));
  in_.assign(kN, 0.f);
  out_.assign(kN, 0.f);
  re_.assign(kN, 0.f);
  im_.assign(kN, 0.f);
  noise_.assign(kN / 2 + 1, 0.f);
  gain_.assign(kN / 2 + 1, 1.f);
  q_.assign(4 * kN, 0.f);
  reset();
}

void SpectralDenoise::reset() {
  std::fill(in_.begin(), in_.end(), 0.f);
  std::fill(out_.begin(), out_.end(), 0.f);
  std::fill(noise_.begin(), noise_.end(), 0.f);
  std::fill(gain_.begin(), gain_.end(), 1.f);
  std::fill(q_.begin(), q_.end(), 0.f);
  fill_ = 0;
  // Prime the output queue with the algorithmic latency (one frame minus one
  // hop) so a sample is always there to hand back: the delay is heard once,
  // when the effect is switched on, instead of as a dropout every frame.
  // No priming: the first hop after switching on comes out as silence (once),
  // which is cheaper than carrying an extra hop of latency for ever.
  qr_ = 0; qw_ = 0; qn_ = 0;
  ready_ = false;
}

// One frame: window, FFT, per-bin gain from the tracked noise floor, IFFT,
// window again and overlap-add. The oldest hop of the accumulator is then
// finished and goes to the output queue.
void SpectralDenoise::frame() {
  // The noise floor is an average of past magnitudes; if anything ever pushes
  // it to inf/NaN the Wiener gain becomes NaN and stays NaN, which is silence
  // for ever. Start it over instead.
  if (!std::isfinite(noise_[0]) || !std::isfinite(noise_[kN / 4])) { reset(); return; }
  for (int i = 0; i < kN; i++) { re_[i] = in_[i] * win_[i]; im_[i] = 0; }
  fft(re_.data(), im_.data(), kN, false);
  const int bins = kN / 2 + 1;
  // 0 -> gentle, 1 -> aggressive: how far above the noise floor a bin must be
  // to survive, and how much of it is left when it does not.
  const float over = 1.f + 2.5f * intensity_;
  const float floorG = std::max(0.02f, 0.30f * (1.f - intensity_));
  for (int k = 0; k < bins; k++) {
    float mag = std::sqrt(re_[k] * re_[k] + im_[k] * im_[k]);
    float& n = noise_[k];
    if (!ready_) n = mag;
    else if (mag < n) n += 0.30f * (mag - n);       // follow a quieter floor quickly
    else n += 0.0006f * (mag - n);                  // creep up (speech must not become the floor)
    float g = mag > 1e-9f ? (mag - over * n) / mag : 0.f;
    g = std::clamp(g, floorG, 1.f);
    gain_[k] = mixf(gain_[k], g, 0.5f);             // over time: no chattering bins
  }
  // Smooth across neighbours too: isolated surviving bins are what "musical
  // noise" is made of.
  float prev = gain_[0];
  for (int k = 1; k < bins - 1; k++) {
    float sm = 0.25f * prev + 0.5f * gain_[k] + 0.25f * gain_[k + 1];
    prev = gain_[k];
    gain_[k] = sm;
  }
  for (int k = 0; k < bins; k++) {
    re_[k] *= gain_[k]; im_[k] *= gain_[k];
    if (k > 0 && k < kN / 2) { re_[kN - k] = re_[k]; im_[kN - k] = -im_[k]; }
  }
  fft(re_.data(), im_.data(), kN, true);
  // Hann analysis + Hann synthesis at 75% overlap sums to 1.5.
  const float norm = 2.f / 3.f;
  for (int i = 0; i < kN; i++) out_[i] += re_[i] * win_[i] * norm;
  ready_ = true;
  // The oldest hop is complete: queue it and slide the accumulator.
  for (int i = 0; i < kHop; i++) {
    q_[qw_] = out_[i];
    qw_ = (qw_ + 1) % (int)q_.size();
    if (qn_ < (int)q_.size()) qn_++; else qr_ = (qr_ + 1) % (int)q_.size();
  }
  std::memmove(out_.data(), out_.data() + kHop, (kN - kHop) * sizeof(float));
  std::fill(out_.end() - kHop, out_.end(), 0.f);
}

void SpectralDenoise::process(float* x, int n) {
  if (in_.empty()) return;
  for (int i = 0; i < n; i++) {
    in_[kN - kHop + fill_] = x[i];
    if (++fill_ == kHop) {
      frame();
      // Slide the analysis window by one hop; the next hop is written into
      // the space that leaves at the end.
      std::memmove(in_.data(), in_.data() + kHop, (kN - kHop) * sizeof(float));
      fill_ = 0;
    }
    if (qn_ > 0) {
      x[i] = q_[qr_];
      qr_ = (qr_ + 1) % (int)q_.size();
      qn_--;
    } else {
      x[i] = 0;   // cannot happen after reset(), which primes the queue
    }
  }
}

// ---------------------------------------------------------------------------
// Pitch detection (YIN) and pitch correction

void PitchTracker::init(int rate) {
  rate_ = rate > 0 ? rate : 48000;
  decim_ = std::max(1, (int)std::lround(rate_ / 12000.0));
  dRate_ = (float)rate_ / decim_;
  minLag_ = std::max(2, (int)std::floor(dRate_ / kMaxHz));
  // The difference function needs a whole window's worth of overlap left over
  // at the longest lag, so the lag can never eat more than most of the window.
  maxLag_ = std::min(kWin - kHop, (int)std::ceil(dRate_ / kMinHz));
  anti_.lowPass((float)rate_, std::min(0.45f * dRate_, 3800.f));
  w_.assign(kWin, 0.f);
  diff_.assign(maxLag_ + 2, 0.f);
  cum_.assign(maxLag_ + 2, 0.f);
  reset();
}

void PitchTracker::reset() {
  std::fill(w_.begin(), w_.end(), 0.f);
  anti_.reset();
  fill_ = 0;
  phase_ = 0;
  period_ = 0;
}

void PitchTracker::push(const float* x, int n) {
  if (w_.empty()) return;
  for (int i = 0; i < n; i++) {
    float y = anti_.run(x[i]);
    if (++phase_ < decim_) continue;
    phase_ = 0;
    w_[fill_++] = y;
    if (fill_ < kWin) continue;
    analyse();
    std::memmove(w_.data(), w_.data() + kHop, (kWin - kHop) * sizeof(float));
    fill_ = kWin - kHop;
  }
}

void PitchTracker::analyse() {
  const int N = kWin - maxLag_;
  double e = 0;
  for (int i = 0; i < kWin; i++) e += (double)w_[i] * w_[i];
  // Around -54 dBFS and below there is no note in there, only the room; and
  // running the search on a noise floor is how an idle microphone ends up being
  // "tuned" to whatever the fan is doing.
  if (e / kWin < 4e-6) { period_ = 0; return; }

  diff_[0] = 0;
  for (int tau = 1; tau <= maxLag_; tau++) {
    float sum = 0;
    for (int j = 0; j < N; j++) {
      float d = w_[j] - w_[j + tau];
      sum += d * d;
    }
    diff_[tau] = sum;
  }
  // YIN's cumulative mean normalisation. Without it the trivial minimum at
  // tau = 0 wins every time; with it, one absolute threshold works at any level.
  float run = 0;
  cum_[0] = 1;
  for (int tau = 1; tau <= maxLag_; tau++) {
    run += diff_[tau];
    cum_[tau] = run > 0 ? diff_[tau] * tau / run : 1.f;
  }

  int best = -1;
  for (int tau = minLag_; tau <= maxLag_; tau++) {
    if (cum_[tau] >= kThresh) continue;
    // Take the *first* dip under the threshold, not the deepest: the deepest is
    // as often as not the octave below, and an octave error is the one mistake
    // a listener always hears.
    while (tau + 1 <= maxLag_ && cum_[tau + 1] < cum_[tau]) tau++;
    best = tau;
    break;
  }
  if (best < 0) {   // nothing convincing: the global minimum, and only if it is close
    best = minLag_;
    for (int tau = minLag_ + 1; tau <= maxLag_; tau++) if (cum_[tau] < cum_[best]) best = tau;
    if (cum_[best] > kVoiced) { period_ = 0; return; }
  }

  // Parabolic interpolation around the dip: one decimated sample is about 8
  // cents at 200 Hz, and 8 cents of wobble is audible on a held note.
  float p = (float)best;
  if (best > minLag_ && best < maxLag_) {
    float y0 = cum_[best - 1], y1 = cum_[best], y2 = cum_[best + 1];
    float den = y0 - 2 * y1 + y2;
    if (std::fabs(den) > 1e-9f) p += std::clamp(0.5f * (y0 - y2) / den, -1.f, 1.f);
  }
  period_ = p * decim_;
}

void PitchCorrector::init(int rate) {
  // 60 Hz is below anything the tracker will report, so head B can never run
  // past the write pointer and head A never off the end of the line.
  base_ = std::max(64.f, (float)(rate > 0 ? rate : 48000) / 60.f);
  buf_.assign((size_t)(2 * base_) + 64, 0.f);
  // Half a second to walk back onto a period boundary: that is a hundredth of a
  // semitone of drift, and it only ever happens while nothing is being tuned.
  park_ = 1.f / std::max(1.f, 0.5f * (float)(rate > 0 ? rate : 48000));
  reset();
}

void PitchCorrector::reset() {
  std::fill(buf_.begin(), buf_.end(), 0.f);
  w_ = 0;
  phase_ = 0;
}

void PitchCorrector::process(float* x, int n, float period, float ratio) {
  if (buf_.empty()) return;
  const float L = (float)buf_.size();
  const float P = std::clamp(period, 16.f, base_);
  const bool shifting = period > 0 && std::fabs(ratio - 1.f) > 1e-4f;
  // Reading the line more slowly than it is written drops the pitch: the delay
  // has to grow by a whole period every 1/(1-ratio) samples, and this phase
  // (0..1 of a period) is the count of how far through that it is.
  const float d = shifting ? (1.f - ratio) / P : 0.f;
  for (int i = 0; i < n; i++) {
    buf_[w_] = x[i];
    w_ = (w_ + 1) % buf_.size();
    if (shifting) {
      phase_ += d;
      phase_ -= std::floor(phase_);
    } else if (phase_ > 0) {
      // Nothing to correct: drift onto a period boundary and stay there. At 0
      // (and at 1, which is the same place) one head has all the weight, so an
      // idle auto-tune is a plain delay rather than a two-tap comb.
      float goal = phase_ < 0.5f ? 0.f : 1.f;
      phase_ += std::copysign(std::min(park_, std::fabs(goal - phase_)), goal - phase_);
      if (phase_ >= 1.f) phase_ = 0;
    }
    auto tap = [&](float delay) {
      float pos = (float)w_ - delay;
      while (pos < 0) pos += L;
      while (pos >= L) pos -= L;
      int i0 = (int)pos;
      float fr = pos - i0;
      return mixf(buf_[i0], buf_[(i0 + 1) % buf_.size()], fr);
    };
    float a = base_ + phase_ * P;
    x[i] = (1.f - phase_) * tap(a) + phase_ * tap(a - P);
  }
}

// ---------------------------------------------------------------------------
// Formant shifting

void FormantShifter::init(int rate) {
  win_.resize(kN);
  for (int i = 0; i < kN; i++) win_[i] = 0.5f * (1 - std::cos(2 * kPi * i / kN));
  in_.assign(kN, 0.f);
  out_.assign(kN, 0.f);
  re_.assign(kN, 0.f);
  im_.assign(kN, 0.f);
  cre_.assign(kN, 0.f);
  cim_.assign(kN, 0.f);
  env_.assign(kN / 2 + 1, 0.f);
  q_.assign(4 * kN, 0.f);
  reset();
}

void FormantShifter::reset() {
  std::fill(in_.begin(), in_.end(), 0.f);
  std::fill(out_.begin(), out_.end(), 0.f);
  std::fill(q_.begin(), q_.end(), 0.f);
  fill_ = 0;
  qr_ = 0; qw_ = 0; qn_ = 0;
}

void FormantShifter::frame() {
  for (int i = 0; i < kN; i++) { re_[i] = in_[i] * win_[i]; im_[i] = 0; }
  fft(re_.data(), im_.data(), kN, false);
  const int bins = kN / 2 + 1;

  // Log magnitude, extended to a full even sequence so its transform is real.
  for (int k = 0; k < bins; k++) {
    float mag = std::sqrt(re_[k] * re_[k] + im_[k] * im_[k]);
    cre_[k] = std::log(std::max(mag, 1e-7f));
    cim_[k] = 0;
  }
  for (int k = bins; k < kN; k++) { cre_[k] = cre_[kN - k]; cim_[k] = 0; }
  fft(cre_.data(), cim_.data(), kN, true);            // -> cepstrum
  for (int i = kQuef; i <= kN - kQuef; i++) { cre_[i] = 0; cim_[i] = 0; }   // lifter
  fft(cre_.data(), cim_.data(), kN, false);           // -> log envelope
  for (int k = 0; k < bins; k++) env_[k] = cre_[k];

  // The correction is the difference between the envelope this bin should have
  // and the one it has. Applied to the complex spectrum it is just a gain, so
  // the harmonics — and with them the pitch — come through untouched.
  for (int k = 0; k < bins; k++) {
    float src = k / ratio_;
    float e;
    if (src <= 0) e = env_[0];
    else if (src >= bins - 1) e = env_[bins - 1];
    else { int i0 = (int)src; e = mixf(env_[i0], env_[i0 + 1], src - i0); }
    float g = std::exp(std::clamp(e - env_[k], -3.f, 3.f));   // +-26 dB is already far past useful
    re_[k] *= g; im_[k] *= g;
    if (k > 0 && k < kN / 2) { re_[kN - k] = re_[k]; im_[kN - k] = -im_[k]; }
  }
  fft(re_.data(), im_.data(), kN, true);
  const float norm = 2.f / 3.f;   // Hann analysis + Hann synthesis at 75% overlap sums to 1.5
  for (int i = 0; i < kN; i++) out_[i] += re_[i] * win_[i] * norm;

  for (int i = 0; i < kHop; i++) {
    q_[qw_] = out_[i];
    qw_ = (qw_ + 1) % (int)q_.size();
    if (qn_ < (int)q_.size()) qn_++; else qr_ = (qr_ + 1) % (int)q_.size();
  }
  std::memmove(out_.data(), out_.data() + kHop, (kN - kHop) * sizeof(float));
  std::fill(out_.end() - kHop, out_.end(), 0.f);
}

void FormantShifter::process(float* x, int n) {
  if (in_.empty()) return;
  for (int i = 0; i < n; i++) {
    in_[kN - kHop + fill_] = x[i];
    if (++fill_ == kHop) {
      frame();
      std::memmove(in_.data(), in_.data() + kHop, (kN - kHop) * sizeof(float));
      fill_ = 0;
    }
    if (qn_ > 0) { x[i] = q_[qr_]; qr_ = (qr_ + 1) % (int)q_.size(); qn_--; }
    else x[i] = 0;   // the first frame after switching on, once
  }
}

// ---------------------------------------------------------------------------
// Reverb / echo

void Space::setLine(Line& l, float ms, float fb) {
  size_t n = std::max<size_t>(16, (size_t)(rate_ * ms * 0.001f));
  if (n > l.buf.size()) n = l.buf.size();   // init() sized it for the longest preset
  l.len = std::max<size_t>(16, n);
  l.idx = 0;
  l.fb = fb;
  std::fill(l.buf.begin(), l.buf.end(), 0.f);
}

void Space::init(int rate) {
  rate_ = rate;
  // Allocate for the largest user-shaped room/echo once. configure() runs on
  // the audio thread, so it may only move a line inside this storage.
  const size_t comb = (size_t)(rate_ * 0.180f) + 16, all = (size_t)(rate_ * 0.016f) + 16;
  const size_t echo = (size_t)(rate_ * 0.600f) + 16, pre = (size_t)(rate_ * 0.120f) + 16;
  for (int i = 0; i < 4; i++) comb_[i].buf.assign(comb, 0.f);
  all_[0].buf.assign(all, 0.f);
  all_[1].buf.assign(all, 0.f);
  echo_.buf.assign(echo, 0.f);
  pre_.buf.assign(pre, 0.f);
  configure(kind_);
}

void Space::reset() {
  for (auto* l : { &comb_[0], &comb_[1], &comb_[2], &comb_[3], &all_[0], &all_[1], &echo_, &pre_ }) {
    std::fill(l->buf.begin(), l->buf.end(), 0.f);
    l->idx = 0;
  }
  lp_[0] = lp_[1] = lp_[2] = lp_[3] = 0;
  lid_.reset();
  lowCut_.reset();
}

void Space::configure(const std::string& kind, float size, float decay, float tone, float preDelay,
                      float diffusion, float lowCut, float modRate, float modDepth) {
  kind_ = kind;
  lfo_ = 0;
  trim_ = 1.f;
  size = std::clamp(size, 0.f, 1.f);
  decay = std::clamp(decay, 0.f, 1.f);
  tone = std::clamp(tone, 0.f, 1.f);
  preDelay = std::clamp(preDelay, 0.f, 1.f);
  diffusion = std::clamp(diffusion, 0.f, 1.f);
  lowCut = std::clamp(lowCut, 0.f, 1.f);
  modRate = std::clamp(modRate, 0.f, 1.f);
  mod_ = std::clamp(modDepth, 0.f, 1.f);
  if (lowCut <= 0.001f) lowCut_.bypass();
  else lowCut_.highPass(rate_, 35.f + lowCut * 465.f, 0.707f);
  // A logarithmic range feels even-handed: both halves of Size represent a
  // similar ratio, rather than reserving all of the large rooms for the end.
  const float userScale = std::exp2((size - 0.5f) * 1.4f);
  preOn_ = preDelay > 0.001f;
  if (preOn_) setLine(pre_, preDelay * 120.f, 0.f);
  else { pre_.idx = 0; std::fill(pre_.buf.begin(), pre_.buf.end(), 0.f); }
  if (kind == "underwater") {
    setLine(echo_, 90.f * userScale, std::clamp(0.45f + (decay - 0.5f) * 0.22f, 0.2f, 0.85f));
    lid_.lowPass(rate_, 900.f * (0.5f + tone), 0.7f);
    lfoStep_ = 2 * kPi * (0.25f + modRate * 2.f) / rate_;
    wobble_ = 0.35f + mod_ * 0.22f;
    mix_ = 0.85f;
    trim_ = 2.2f;   // most of the voice went into the lid; put the level back
    return;
  }
  if (kind == "echo") {
    setLine(echo_, 320.f * userScale, std::clamp(0.38f + (decay - 0.5f) * 0.24f, 0.15f, 0.82f));
    if (tone == 0.5f) lid_.bypass();
    else lid_.lowPass(rate_, 1200.f + tone * 14800.f, 0.7f);
    lfoStep_ = 2 * kPi * (0.08f + modRate * 0.92f) / rate_;
    mix_ = 0.38f;
    trim_ = 1.2f;
    return;
  }
  bool hall = kind == "hall", cathedral = kind == "cathedral", trap = kind == "trap";
  const float scale = (cathedral ? 2.3f : trap ? 1.8f : hall ? 1.6f : 1.f) * userScale;
  const float baseFb = cathedral ? 0.91f : trap ? 0.86f : hall ? 0.84f : 0.70f;
  const float fb = std::clamp(baseFb + (decay - 0.5f) * 0.16f, 0.3f, 0.97f);
  const float combMs[4] = { 29.7f, 37.1f, 41.1f, 43.7f };
  for (int i = 0; i < 4; i++) setLine(comb_[i], combMs[i] * scale, fb);
  const float diffuseFb = 0.2f + diffusion * 0.6f;
  setLine(all_[0], 5.0f, diffuseFb);
  setLine(all_[1], 1.7f, diffuseFb);
  // Trap is a real parallel delay plus the room, not an echo label pasted on
  // a reverb. Its long, filtered repeats sit behind the dense tail.
  setLine(echo_, trap ? 380.f * userScale : 10.f,
          trap ? std::clamp(0.30f + (decay - 0.5f) * 0.16f, 0.16f, 0.65f) : 0.f);
  const float baseDamp = cathedral ? 0.45f : trap ? 0.38f : hall ? 0.35f : 0.25f;
  damp_ = std::clamp(baseDamp + (0.5f - tone) * 0.38f, 0.02f, 0.9f);
  lfoStep_ = 2 * kPi * (0.08f + modRate * 0.92f) / rate_;
  // Anything we do not know is a passthrough, not a surprise room.
  bool known = kind == "room" || hall || cathedral || trap;   // (echo and underwater returned above)
  mix_ = !known ? 0.f : (cathedral ? 0.42f : trap ? 0.38f : hall ? 0.32f : 0.20f);
  // Mixing wet against dry costs level; hand it back so picking a room is not
  // also picking "quieter".
  trim_ = !known ? 1.f : (cathedral ? 1.35f : trap ? 1.32f : hall ? 1.35f : 1.2f);
}

void Space::process(float* x, int n) {
  if (kind_ == "none" || mix_ <= 0 || comb_[0].len > comb_[0].buf.size() || echo_.len > echo_.buf.size()) return;
  if (kind_ == "underwater") {
    const float half = echo_.len * 0.5f;
    for (int i = 0; i < n; i++) {
      const float dry = x[i];
      float wetIn = dry;
      if (preOn_) {
        wetIn = pre_.buf[pre_.idx];
        pre_.buf[pre_.idx] = dry;
        pre_.idx = (pre_.idx + 1) % pre_.len;
      }
      lfo_ += lfoStep_;
      if (lfo_ > 2 * kPi) lfo_ -= 2 * kPi;
      // Read the line at a wandering distance behind the write head: the
      // moving delay is what bends the pitch about, like a wobbly tape.
      float back = half * (1.f + wobble_ * std::sin(lfo_));
      float pos = (float)echo_.idx - back;
      while (pos < 0) pos += (float)echo_.len;
      size_t i0 = (size_t)pos % echo_.len, i1 = (i0 + 1) % echo_.len;
      float fr = pos - std::floor(pos);
      float d = mixf(echo_.buf[i0], echo_.buf[i1], fr);
      echo_.buf[echo_.idx] = wetIn + d * echo_.fb;
      echo_.idx = (echo_.idx + 1) % echo_.len;
      x[i] = mixf(dry, lowCut_.run(lid_.run(d)), mix_) * trim_;
    }
    return;
  }
  if (kind_ == "echo") {
    for (int i = 0; i < n; i++) {
      const float dry = x[i];
      float wetIn = dry;
      if (preOn_) {
        wetIn = pre_.buf[pre_.idx];
        pre_.buf[pre_.idx] = dry;
        pre_.idx = (pre_.idx + 1) % pre_.len;
      }
      lfo_ += lfoStep_;
      if (lfo_ > 2 * kPi) lfo_ -= 2 * kPi;
      float d = echo_.buf[echo_.idx];
      echo_.buf[echo_.idx] = wetIn + d * echo_.fb;
      echo_.idx = (echo_.idx + 1) % echo_.len;
      x[i] = (dry * (1 - mix_ * 0.5f) + lowCut_.run(lid_.run(d)) * mix_) * trim_;
    }
    return;
  }
  for (int i = 0; i < n; i++) {
    const float dry = x[i];
    float wetIn = dry;
    if (preOn_) {
      wetIn = pre_.buf[pre_.idx];
      pre_.buf[pre_.idx] = dry;
      pre_.idx = (pre_.idx + 1) % pre_.len;
    }
    lfo_ += lfoStep_;
    if (lfo_ > 2 * kPi) lfo_ -= 2 * kPi;
    float echo = 0;
    if (kind_ == "trap") {
      echo = echo_.buf[echo_.idx];
      echo_.buf[echo_.idx] = wetIn + echo * echo_.fb;
      echo_.idx = (echo_.idx + 1) % echo_.len;
    }
    float in = wetIn * 0.25f, acc = 0;
    for (int c = 0; c < 4; c++) {
      Line& l = comb_[c];
      float y = l.buf[l.idx];
      lp_[c] = mixf(y, lp_[c], damp_);          // damped feedback: a room, not a spring
      const float drift = 1.f + mod_ * 0.025f * std::sin(lfo_ + c * 1.7f);
      l.buf[l.idx] = in + lp_[c] * l.fb * drift;
      l.idx = (l.idx + 1) % l.len;
      acc += y;
    }
    for (int a = 0; a < 2; a++) {
      Line& l = all_[a];
      float y = l.buf[l.idx];
      float v = acc + y * -l.fb;
      l.buf[l.idx] = v;
      l.idx = (l.idx + 1) % l.len;
      acc = y + v * l.fb;
    }
    x[i] = mixf(dry, lowCut_.run(acc + echo * 0.52f), mix_) * trim_;
  }
}

// ---------------------------------------------------------------------------
// Dedicated vocal auxes

void Doubler::init(int rate) {
  rate_ = rate;
  buf_.assign((size_t)(rate_ * 0.080f) + 16, 0.f);
  reset();
}

void Doubler::reset() {
  std::fill(buf_.begin(), buf_.end(), 0.f);
  w_ = 0; phaseA_ = phaseB_ = 0;
}

void Doubler::process(const float* in, float* out, int n, float mix) {
  if (buf_.empty() || mix <= 0) return;
  mix = std::clamp(mix, 0.f, 1.f);
  const float stepA = 2 * kPi * 0.23f / rate_, stepB = 2 * kPi * 0.37f / rate_;
  auto tap = [&](float back) {
    float pos = (float)w_ - back;
    while (pos < 0) pos += (float)buf_.size();
    size_t i0 = (size_t)pos % buf_.size(), i1 = (i0 + 1) % buf_.size();
    return mixf(buf_[i0], buf_[i1], pos - std::floor(pos));
  };
  for (int i = 0; i < n; i++) {
    buf_[w_] = in[i];
    phaseA_ += stepA; if (phaseA_ > 2 * kPi) phaseA_ -= 2 * kPi;
    phaseB_ += stepB; if (phaseB_ > 2 * kPi) phaseB_ -= 2 * kPi;
    const float a = tap((0.017f + 0.0022f * std::sin(phaseA_)) * rate_);
    const float b = tap((0.031f + 0.0034f * std::sin(phaseB_)) * rate_);
    // It is intentionally quiet: the dry lead stays in front and the two
    // delayed takes make a wider-feeling, thicker mono image around it.
    out[i] += (a + b) * (0.38f * mix);
    w_ = (w_ + 1) % buf_.size();
  }
}

void VocalDelay::setLine(Line& line, float ms, float feedback) {
  size_t length = std::max<size_t>(16, (size_t)(rate_ * ms * 0.001f));
  length = std::min(length, line.buf.size());
  line.len = std::max<size_t>(16, length);
  line.idx = 0; line.fb = feedback; line.state = 0;
  std::fill(line.buf.begin(), line.buf.end(), 0.f);
}

void VocalDelay::init(int rate) {
  rate_ = rate;
  slap_.buf.assign((size_t)(rate_ * 0.180f) + 16, 0.f);
  long_.buf.assign((size_t)(rate_ * 1.100f) + 16, 0.f);
  reset();
}

void VocalDelay::reset() {
  for (Line* line : { &slap_, &long_ }) {
    std::fill(line->buf.begin(), line->buf.end(), 0.f);
    line->idx = 0; line->state = 0;
  }
}

void VocalDelay::configure(bool slapOn, float slapTime, bool longOn, float longTime,
                           float feedback, float tone) {
  slapOn_ = slapOn; longOn_ = longOn;
  slapTime = std::clamp(slapTime, 0.f, 1.f);
  longTime = std::clamp(longTime, 0.f, 1.f);
  feedback = std::clamp(feedback, 0.f, 1.f);
  tone = std::clamp(tone, 0.f, 1.f);
  // 70–160 ms gives an audible reflection rather than a chorus; 220–1000 ms
  // covers a quick rhythmic repeat through a slow tail without pretending BPM
  // exists when the microphone has no transport clock.
  setLine(slap_, 70.f + slapTime * 90.f, 0.12f);
  setLine(long_, 220.f + longTime * 780.f, std::clamp(0.08f + feedback * 0.72f, 0.08f, 0.80f));
  // More damping makes every repeat darker, the way a filtered send should
  // retreat behind a vocal instead of building brittle high-frequency rings.
  damp_ = 0.30f + (1.f - tone) * 0.58f;
}

void VocalDelay::process(const float* in, float* out, int n, float slapMix, float longMix) {
  slapMix = std::clamp(slapMix, 0.f, 1.f);
  longMix = std::clamp(longMix, 0.f, 1.f);
  if ((!slapOn_ || slapMix <= 0) && (!longOn_ || longMix <= 0)) return;
  for (int i = 0; i < n; i++) {
    if (slapOn_) {
      const float d = slap_.buf[slap_.idx];
      slap_.state = mixf(d, slap_.state, 0.45f);
      slap_.buf[slap_.idx] = in[i] + slap_.state * slap_.fb;
      slap_.idx = (slap_.idx + 1) % slap_.len;
      out[i] += slap_.state * slapMix;
    }
    if (longOn_) {
      const float d = long_.buf[long_.idx];
      long_.state = mixf(d, long_.state, damp_);
      long_.buf[long_.idx] = in[i] + long_.state * long_.fb;
      long_.idx = (long_.idx + 1) % long_.len;
      out[i] += long_.state * longMix;
    }
  }
}

// ---------------------------------------------------------------------------
// The chain

void MicProcessor::setRate(int rate) {
  if (rate <= 0) return;
  if (inited_ && rate == rate_) return;
  rate_ = rate;
  inited_ = true;
  dry_.assign(8192, 0.f);
  denoise_.init(rate_);
  formant_.init(rate_);
  tracker_.init(rate_);
  corrector_.init(rate_);
  doubler_.init(rate_);
  space_.init(rate_);
  delay_.init(rate_);
  dirty_ = true;
  reset();
}

void MicProcessor::setSettings(const MicSettings& s) {
  if (s == s_) return;
  bool spaceChanged = s.space != s_.space || s.spaceSize != s_.spaceSize ||
                      s.spaceDecay != s_.spaceDecay || s.spaceTone != s_.spaceTone ||
                      s.spacePreDelay != s_.spacePreDelay || s.spaceMod != s_.spaceMod ||
                      s.spaceDiffusion != s_.spaceDiffusion || s.spaceLowCut != s_.spaceLowCut ||
                      s.spaceModRate != s_.spaceModRate || s.spaceModDepth != s_.spaceModDepth;
  bool delayChanged = s.slapDelay != s_.slapDelay || s.slapDelayTime != s_.slapDelayTime ||
                      s.longDelay != s_.longDelay || s.longDelayTime != s_.longDelayTime ||
                      s.longDelayFeedback != s_.longDelayFeedback || s.longDelayTone != s_.longDelayTone;
  // Anything that holds audio (the pitch delay, the overlap-add queues) freezes
  // while it is bypassed and would play that stale audio out when switched back
  // on — a minute later, in the middle of a call. So the chain is emptied when a
  // stage STARTS or STOPS running.
  //
  // That is not the same as its parameter moving, and conflating the two was
  // audible: a slider sends a value every 50 ms while it is dragged, each one
  // emptied the overlap-add queues, and an emptied queue hands back a hop of
  // silence. Measured at 47x the steady-state sample step — a click on every
  // step of the drag. Space and voice are gone from this list entirely:
  // Space::configure already zeroes its own delay lines, and the voice stage
  // holds nothing but biquads.
  auto pitchRuns = [](const MicSettings& m) { return m.autoTune || m.pitch != 0; };
  auto formantRuns = [](const MicSettings& m) { return m.formant != m.pitch; };
  bool tailChanged = s.enabled != s_.enabled ||
                     s.voiceIsolation != s_.voiceIsolation ||
                     pitchRuns(s) != pitchRuns(s_) ||
                     formantRuns(s) != formantRuns(s_);
  s_ = s;
  dirty_ = true;
  if (spaceChanged) space_.configure(s_.space, s_.spaceSize, s_.spaceDecay,
                                     s_.spaceTone, s_.spacePreDelay, s_.spaceDiffusion,
                                     s_.spaceLowCut, s_.spaceModRate, s_.spaceModDepth);
  if (delayChanged) delay_.configure(s_.slapDelay, s_.slapDelayTime, s_.longDelay,
                                     s_.longDelayTime, s_.longDelayFeedback, s_.longDelayTone);
  if (tailChanged) reset();
}

void MicProcessor::reset() {
  for (int i = 0; i < kHum; i++) hum_[i].reset();
  hp_.reset(); voiceHp_.reset(); voiceLp_.reset();
  for (int i = 0; i < MicSettings::kMaxEqBands; i++) eq_[i].reset();
  denoise_.reset();
  formant_.reset();
  tracker_.reset();
  corrector_.reset();
  doubler_.reset();
  tuneRatio_ = 1; tunePeriod_ = 0;
  space_.reset();
  delay_.reset();
  gateEnv_ = 0; gateGain_ = 0; compEnv_ = 0; compGain_ = 1; glueEnv_ = 0; glueGain_ = 1;
  essHi_ = 0; essAll_ = 0; essGain_ = 1;
  ringPhase_ = 0;
  gainPrimed_ = false;   // re-prime the glide rather than sweep up from wherever it was
  inHold_ = outHold_ = 0;
  inLevel_ = 0; outLevel_ = 0;
}

// Re-designs every filter from the current settings. Cheap and only run when
// something changed.
void MicProcessor::rebuild() {
  dirty_ = false;
  const float r = (float)rate_;
  hp_.highPass(r, 80.f);

  // Design each enabled band onto its own biquad. Only the ones that are on
  // take a slot, so a bypassed band costs nothing per sample instead of running
  // as a unity filter.
  eqN_ = 0;
  for (const EqBand& b : s_.eq) {
    if (eqN_ >= MicSettings::kMaxEqBands) break;
    if (!b.on) continue;
    Biquad& f = eq_[eqN_];
    const float fr = std::clamp(b.freq, 20.f, r * 0.45f);
    const float q = std::clamp(b.q, 0.1f, 10.f);
    const float g = std::clamp(b.gain, -18.f, 18.f);
    if (b.type == "lowshelf") f.lowShelf(r, fr, g);
    else if (b.type == "highshelf") f.highShelf(r, fr, g);
    else if (b.type == "highpass") f.highPass(r, fr, q);
    else if (b.type == "lowpass") f.lowPass(r, fr, q);
    else if (b.type == "notch") f.notch(r, fr, q);
    else f.peak(r, fr, q, g);
    eqN_++;
  }

  voiceHp_.bypass(); voiceLp_.bypass();
  ringHz_ = s_.ringMod ? 55.f : 0.f;
  ringMix_ = 1.f; drive_ = s_.megaphone ? 6.f : 0.f; driveOut_ = 0.22f;
  outTrim_ = s_.ringMod ? mixf(1.f, 1.4f, std::clamp(s_.ringModMix, 0.f, 1.f)) : 1.f;
  if (s_.megaphone) { voiceHp_.highPass(r, 600.f, 0.9f); voiceLp_.lowPass(r, 3600.f, 0.9f); }

  pitchRatio_ = semis(std::clamp(s_.pitch, -12.f, 12.f));
  // The shifter moves the whole spectrum, formants included, so holding the
  // formants still under a Pitch change already costs a shift of -pitch. The
  // Formant knob is an offset on top of that, which is why 0/0 is a bypass and
  // Pitch alone keeps the voice sounding like the same person.
  formantRatio_ = semis(std::clamp(s_.formant, -12.f, 12.f) - std::clamp(s_.pitch, -12.f, 12.f));

  // 50/60 and their first harmonic only: a notch at 150 or 180 Hz would sit
  // right on a low speaking voice, and that is where hum has the least energy.
  const float humF[kHum] = { 50.f, 100.f, 60.f, 120.f };
  // Q 35 was a 1.4 Hz notch: the mains wanders further than that in an hour,
  // and a float biquad cannot hold a null that narrow anyway (measured -26 dB
  // where the design says -200). Q 12 is ~4 Hz wide and actually lands on the
  // hum.
  for (int i = 0; i < kHum; i++) hum_[i].notch(r, humF[i], 12.f);

  // Resolve the running order. A name that is not a stage is dropped and any
  // stage the list forgot is appended in default order, so whatever arrives —
  // a stale config, a half-written list, an empty one — still runs every stage
  // exactly once.
  {
    const auto& names = MicSettings::stageNames();
    bool seen[StCount] = { false };
    orderN_ = 0;
    for (const std::string& id : s_.chain) {
      auto it = std::find(names.begin(), names.end(), id);
      if (it == names.end()) continue;
      int idx = (int)(it - names.begin());
      if (seen[idx]) continue;
      seen[idx] = true;
      order_[orderN_++] = idx;
    }
    for (int i = 0; i < StCount; i++) if (!seen[i]) order_[orderN_++] = i;
  }

  // Auto-tune: which notes the scale allows, how far towards them to travel and
  // how long to take.
  {
    tuneAmount_ = std::clamp(s_.autoTuneAmount, 0.f, 1.f);
    // Retune speed is logarithmic — 200 ms to 2 ms across the slider. Linear
    // would spend most of its travel in times too slow to hear apart, and cross
    // the whole musically interesting range in the last few percent.
    tuneTau_ = 0.20f * std::pow(0.01f, std::clamp(s_.autoTuneSpeed, 0.f, 1.f));
    const int root = keyIndex(s_.autoTuneKey);
    for (int i = 0; i < 12; i++) tuneMask_[i] = false;
    for (int step : scaleSteps(s_.autoTuneScale)) tuneMask_[(root + step) % 12] = true;
  }

  essLp_.lowPass(r, 5500.f, 0.707f);   // de-esser split: everything above this is "s"

  outGain_ = dbToLin((std::clamp(s_.volume, 0.f, 1.f) - 0.5f) * 36.f);
  // Volume and the voice trim are applied per sample at the output. Jumping
  // either between blocks steps the waveform — classic zipper noise, measured at
  // 25x the steady-state step while dragging the volume slider. Glide instead.
  gainCoef_ = coefMs(r, 15.f);
  if (!gainPrimed_) { gainNow_ = outGain_ * outTrim_; gainPrimed_ = true; }
}

// Nearest frequency the key and scale allow. A whole tone either way is as far
// as it will ever have to look — the widest gap in any scale here is the three
// semitones in the pentatonic — but six keeps it honest if one is added later.
float MicProcessor::snapHz(float f) const {
  if (!(f > 20.f && f < 2000.f)) return f;
  const float note = 69.f + 12.f * std::log2(f / 440.f);   // MIDI note, fractional
  const int near = (int)std::lround(note);
  int best = near;
  float bestD = 1e9f;
  for (int k = near - 6; k <= near + 6; k++) {
    if (!tuneMask_[((k % 12) + 12) % 12]) continue;
    float d = std::fabs((float)k - note);
    if (d < bestD) { bestD = d; best = k; }
  }
  if (bestD > 100.f) return f;   // an empty mask cannot happen, but do no harm if it does
  return 440.f * std::exp2((best - 69) / 12.f);
}

void MicProcessor::process(float* x, int n) {
  if (n <= 0) return;
  if (!inited_) setRate(rate_);   // no format callback yet: size the buffers now
  if (dirty_) rebuild();

#if defined(__x86_64__) || defined(__i386__)
  // Everything here decays exponentially and never quite reaches zero (the
  // reverb combs worst of all). Without this, a few seconds of silence fills
  // the delay lines with denormals and every sample after that takes a
  // microcode assist — in the one callback that must not overrun.
  unsigned mxcsr = _mm_getcsr();
  _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
  _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif

  // A stage with a mix runs on x as usual; these two put the dry side aside
  // first and fold it back after. Capture/blend pairs never nest, so one buffer
  // serves all of them.
  const bool canMix = (int)dry_.size() >= n;
  auto captureDry = [&]() { if (canMix) std::memcpy(dry_.data(), x, (size_t)n * sizeof(float)); };
  auto blendDry = [&](float mix) {
    if (!canMix) return;
    const float m = std::clamp(mix, 0.f, 1.f);
    if (m >= 0.999f) return;
    for (int i = 0; i < n; i++) x[i] = mixf(dry_[i], x[i], m);
  };

  float inPeak = 0;
  for (int i = 0; i < n; i++) {
    // A single wild sample used to live on in the filter memories: inf and NaN
    // for ever, and a merely huge value (1e19 is an ordinary bit pattern for
    // uninitialised memory) as ten seconds of full-scale noise out of the
    // narrow notches. Nothing above full scale is real audio.
    if (!std::isfinite(x[i])) x[i] = 0;
    else if (x[i] > 4.f) x[i] = 4.f;
    else if (x[i] < -4.f) x[i] = -4.f;
    inPeak = std::max(inPeak, std::fabs(x[i]));
  }

  if (s_.enabled) {
// Stages run in whatever order the chain is in. Each body below is the same
    // code it was when the order was fixed; only the sequencing moved, so a
    // reordered chain is not a different implementation of the same effects.
    for (int k = 0; k < orderN_; k++) {
      switch (order_[k]) {
      case StHpf: {
        if (s_.highPass) for (int i = 0; i < n; i++) x[i] = hp_.run(x[i]);
        break;
      }
      case StHum: {
        if (s_.humFilter)
          for (int i = 0; i < n; i++) {
            float y = x[i];
            for (int h = 0; h < kHum; h++) y = hum_[h].run(y);
            x[i] = y;
          }
        break;
      }
      case StNr: {
        if (s_.voiceIsolation) {
          denoise_.setIntensity(std::clamp(s_.voiceIsolationIntensity, 0.f, 1.f));
          denoise_.process(x, n);
        }
        break;
      }
      case StGate: {
        if (s_.noiseGate) {
          const float att = coefMs((float)rate_, 1.f), rel = coefMs((float)rate_, 80.f);
          const float gAtt = coefMs((float)rate_, 3.f), gRel = coefMs((float)rate_, 120.f);
          // A real room sits around -35 dBFS, so a range topping out at -26 dB
          // meant the default never closed. -60..-15 dB puts the useful part of
          // the slider in the middle.
          const float open = dbToLin(mixf(-60.f, -15.f, std::clamp(s_.noiseGateIntensity, 0.f, 1.f)));
          const float close = open * 0.5f;   // 6 dB of hysteresis
          for (int i = 0; i < n; i++) {
            float a = std::fabs(x[i]);
            gateEnv_ += (a > gateEnv_ ? att : rel) * (a - gateEnv_);
            float want = gateEnv_ > (gateGain_ > 0.5f ? close : open) ? 1.f : 0.f;
            gateGain_ += (want > gateGain_ ? gAtt : gRel) * (want - gateGain_);
            x[i] *= gateGain_;
          }
        }
        break;
      }
      case StComp: {
        if (s_.autoLevel) {
          captureDry();   // parallel compression: the dry side comes back below
          const float t = std::clamp(s_.autoLevelIntensity, 0.f, 1.f);
          const float thrDb = mixf(-14.f, -32.f, t), ratio = mixf(2.f, 6.f, t);
          const float makeup = dbToLin(-thrDb * (1.f - 1.f / ratio) * 0.85f);
          const float att = coefMs((float)rate_, 6.f), rel = coefMs((float)rate_, 140.f);
          const float makeupDb = linToDb(makeup);
          for (int i = 0; i < n; i++) {
            float a = std::fabs(x[i]);
            compEnv_ += (a > compEnv_ ? att : rel) * (a - compEnv_);
            float envDb = linToDb(compEnv_);
            float overDb = envDb - thrDb;
            float grDb = overDb > 0 ? -overDb * (1.f - 1.f / ratio) : 0.f;
            // Make-up gain belongs to the voice, not to the room: it fades in
            // between -45 and -30 dBFS, so "even loudness" cannot turn into a
            // 16 dB louder fan. (Nothing else brings the floor back down: the
            // noise gate is a separate switch and off by default.)
            float lift = std::clamp((envDb + 45.f) / 15.f, 0.f, 1.f);
            float want = dbToLin(grDb + makeupDb * lift);
            compGain_ += (want < compGain_ ? att : rel) * (want - compGain_);
            x[i] *= compGain_;
          }
          blendDry(s_.compMix);
        }
        // A light second pass catches what the fast main compressor misses and
        // glues the vocal into a dense beat without turning every syllable flat.
        if (s_.glueComp) {
          const float t = std::clamp(s_.glueCompIntensity, 0.f, 1.f);
          const float thrDb = mixf(-12.f, -24.f, t), ratio = mixf(1.5f, 3.f, t);
          const float att = coefMs((float)rate_, 14.f), rel = coefMs((float)rate_, 110.f);
          for (int i = 0; i < n; i++) {
            float a = std::fabs(x[i]);
            glueEnv_ += (a > glueEnv_ ? att : rel) * (a - glueEnv_);
            float overDb = linToDb(glueEnv_) - thrDb;
            float grDb = overDb > 0 ? -overDb * (1.f - 1.f / ratio) : 0.f;
            glueGain_ += (grDb < linToDb(glueGain_) ? att : rel) * (dbToLin(grDb) - glueGain_);
            x[i] *= glueGain_;
          }
        }
        break;
      }
      case StDeEss: {
        if (s_.deEsser) {
          // Split at 5.5 kHz (lo + hi is exactly the input, so an idle de-esser
          // changes nothing) and turn the top down whenever it carries more of the
          // sound than speech ever does — which is what an "s" is.
          const float att = coefMs((float)rate_, 1.f), rel = coefMs((float)rate_, 45.f);
          const float share = mixf(0.55f, 0.18f, std::clamp(s_.deEsserIntensity, 0.f, 1.f));
          for (int i = 0; i < n; i++) {
            float lo = essLp_.run(x[i]), hi = x[i] - lo;
            float ah = std::fabs(hi), af = std::fabs(x[i]);
            essHi_ += (ah > essHi_ ? att : rel) * (ah - essHi_);
            essAll_ += (af > essAll_ ? att : rel) * (af - essAll_);
            // Below this the high band is hiss, not sibilance: a ratio test alone
            // would duck a quiet bright microphone's whole top end, permanently.
            float allow = share * essAll_ + 1e-6f;
            float g = (essHi_ > allow && essHi_ > 0.02f) ? allow / essHi_ : 1.f;
            if (g < 0.25f) g = 0.25f;   // 12 dB is plenty; more sounds lisped
            essGain_ += (g < essGain_ ? att : rel) * (g - essGain_);
            x[i] = lo + hi * essGain_;
          }
        }
        break;
      }
      case StEq: {
        if (eqN_ > 0)
          for (int i = 0; i < n; i++) {
            float y = x[i];
            for (int b = 0; b < eqN_; b++) y = eq_[b].run(y);
            x[i] = y;
          }
        // The pitch work sits after the EQ and before the voice effects: it corrects
        // the pitch that is actually there, and a voice effect after it is then free
        // to do what it likes to the result.
        // Auto-tune's correction and the Pitch knob are the same operation, so they
        // multiply into one ratio and go through the shifter once. Two shifters in
        // series would be two lots of artefacts for no extra control.
        break;
      }
      case StPitch: {
        const bool wantPitch = s_.autoTune || pitchRatio_ != 1.f;
        const bool wantFormant = std::fabs(formantRatio_ - 1.f) > 1e-3f;
        const bool wantDoubler = s_.doubler;
        // One capture around both, so the blend is against the voice as it went in
        // rather than against a half-shifted intermediate.
        if (wantPitch || wantFormant || wantDoubler) captureDry();
        if (wantPitch || wantDoubler) {
          tracker_.push(x, n);
          const float p = tracker_.period();
          // Smooth the period rather than take it raw: it is the distance between
          // the corrector's two heads, and stepping that in the middle of a note is
          // audible even when the pitch it reports is right.
          if (p > 0) tunePeriod_ = tunePeriod_ > 0 ? mixf(tunePeriod_, p, 0.35f) : p;
          float want = 1.f;
          if (s_.autoTune && p > 0 && tunePeriod_ > 0) {
            const float f = (float)rate_ / tunePeriod_;
            // ^amount travels that fraction of the distance to the note; the clamp is
            // the seatbelt for the octave error the detector will make eventually.
            want = std::clamp(std::pow(snapHz(f) / f, tuneAmount_), 0.891f, 1.122f);
          }
          // Glide once per block, so the correction slides into the note — and back
          // to 1 through an unvoiced consonant — instead of stepping.
          const float c = 1.f - std::exp(-(float)n / std::max(1.f, tuneTau_ * rate_));
          tuneRatio_ += c * (want - tuneRatio_);
          // An unvoiced stretch has no period to splice on. Auto-tune wants to stop
          // there — its correction has already glided back to 1 — but a fixed Pitch
          // does not: a consonant that jumps back to its original octave is worse
          // than one spliced on a stale period, so hold the last one.
          const float per = tunePeriod_ > 0 ? tunePeriod_ : (float)rate_ / 150.f;
          corrector_.process(x, n, per, wantPitch ? tuneRatio_ * pitchRatio_ : 1.f);
        }
        if (wantFormant) {
          formant_.setRatio(formantRatio_);
          formant_.process(x, n);
        }
        // The shifted side is a few tens of milliseconds behind the dry one, so
        // anything under 100% is a doubler rather than a crossfade. That is what it
        // is for: at 50% with Pitch a semitone or two off, one voice becomes two.
        if (wantPitch || wantFormant) blendDry(s_.pitchMix);
        // The doubler gets the unprocessed vocal as its own aux source; it is
        // never a delayed duplicate of the entire corrected lead.
        if (wantDoubler) doubler_.process(dry_.data(), x, n, s_.doublerMix);
        break;
      }
      case StFx: {
        if (s_.tape) {
          captureDry();
          for (int i = 0; i < n; i++) x[i] = std::tanh(x[i] * 2.2f) / 2.2f;
          blendDry(s_.tapeMix);
        }
        if (s_.ringMod) {
          captureDry();
          if (ringHz_ > 0) {  // ring modulation
            const float step = 2 * kPi * ringHz_ / rate_;
            for (int i = 0; i < n; i++) {
              ringPhase_ += step;
              if (ringPhase_ > 2 * kPi) ringPhase_ -= 2 * kPi;
              float m = std::sin(ringPhase_);
              x[i] = mixf(x[i], x[i] * m, ringMix_);
            }
          }
          blendDry(s_.ringModMix);
        }
        if (s_.megaphone) {
          captureDry();
          const float norm = 1.f / std::tanh(drive_);
          for (int i = 0; i < n; i++) x[i] = std::tanh(voiceLp_.run(voiceHp_.run(x[i])) * drive_) * norm * 0.8f * driveOut_;
          blendDry(s_.megaphoneMix);
        }
        break;
      }
      case StVerb: {
        const bool hasSpace = s_.space != "none";
        const bool hasDelay = s_.slapDelay || s_.longDelay;
        if (hasSpace || hasDelay) {
          // Wrapped round the outside rather than reaching into Space::mix_, which
          // is the preset's own wet balance and tuned along with its trim. So 100%
          // here is the room exactly as designed, and the knob takes it back
          // towards dry from there.
          captureDry();
          if (hasSpace) {
            space_.process(x, n);
            blendDry(s_.spaceMix);
          }
          // Sends take the captured lead, not the already reverberated signal:
          // delay repeats stay defined instead of recursively washing out.
          if (hasDelay) delay_.process(dry_.data(), x, n, s_.slapDelayMix, s_.longDelayMix);
        }
        break;
      }
      }
    }
  }

  float outPeak = 0;
  for (int i = 0; i < n; i++) {
    // Volume is the microphone's own gain, not an effect: it applies even with
    // the master switch off (that is the only knob left in that case).
    gainNow_ += gainCoef_ * (outGain_ * outTrim_ - gainNow_);
    float y = x[i] * gainNow_;
    // Gentle soft limiter so an effect (or the makeup gain) can never clip hard.
    float a = std::fabs(y);
    if (a > 0.8f) y = std::copysign(0.8f + 0.2f * std::tanh((a - 0.8f) / 0.2f), y);
    if (!std::isfinite(y)) y = 0;
    x[i] = y;
    outPeak = std::max(outPeak, std::fabs(y));
  }

  // Meter: jump to a new peak, fall back slowly so the panel can show it.
  const float fall = std::pow(0.75f, (float)n / 1024.f);
  inHold_ = std::max(inPeak, inHold_ * fall);
  outHold_ = std::max(outPeak, outHold_ * fall);
  inLevel_ = std::min(1.f, inHold_);
  outLevel_ = std::min(1.f, outHold_);

#if defined(__x86_64__) || defined(__i386__)
  _mm_setcsr(mxcsr);   // the callback borrowed the thread; hand it back as found
#endif
}
