#pragma once
#include <algorithm>
#include <atomic>
#include <string>
#include <vector>

// Microphone effects DSP. Everything here is
// plain DSP on a mono float stream; the PipeWire plumbing (which mic is read,
// which node apps see) lives in audio.cpp, the way capture.cpp/pwout.cpp do
// for video.
//
// The chain's DEFAULT order is: rumble filter -> hum notch -> voice isolation
// (spectral denoise) -> noise gate -> auto level (compressor) -> de-esser ->
// parametric EQ -> auto-tune/pitch/formant -> voice (ring mod / megaphone) ->
// space (reverb/echo), then volume + limiter, which is always last.
//
// That order is only a default: MicSettings::chain reorders the stages, because
// where a compressor sits relative to an EQ is a real decision and not one this
// file gets to make. Volume and the limiter stay pinned at the end — they are
// the output, not an insert. Every effect is bypassed
// when it is off; volume is the microphone's own gain and applies even with the
// master switch off, so at 0 dB with nothing on this is a passthrough.

// One band of the parametric EQ. Everything here maps straight onto a Biquad;
// `gain` is ignored by the shapes that have no gain (high-pass, low-pass,
// notch) and `q` by the shelves, which are fixed-slope.
struct EqBand {
  bool on = true;
  std::string type = "bell";   // one of MicSettings::eqTypeNames()
  float freq = 1000;           // Hz, 20 .. 20000
  float gain = 0;              // dB, -18 .. +18
  float q = 0.707f;            // 0.1 .. 10
  bool operator==(const EqBand& o) const {
    return on == o.on && type == o.type && freq == o.freq && gain == o.gain && q == o.q;
  }
};

// The reorderable stages, in their default order. Pitch, auto-tune and formant
// are one stage: formant is defined as an offset from pitch, so splitting them
// would let the chain express a formant shift that compensates for a pitch
// change that has not happened yet.
enum MicStage { StHpf, StHum, StNr, StGate, StComp, StDeEss, StEq, StPitch, StFx, StVerb, StCount };

struct MicSettings {
  bool enabled = true;                 // master switch; off = plain mic (volume still applies)
  bool voiceIsolation = false;         // spectral noise suppression (fans, keyboards, street)
  float voiceIsolationIntensity = 0.6f;
  bool noiseGate = false;              // silence between sentences
  float noiseGateIntensity = 0.55f;    // 0 = barely closes, 1 = aggressive
  bool autoLevel = false;              // compressor + makeup gain: even loudness
  float autoLevelIntensity = 0.6f;
  bool glueComp = false;               // second, gentler compressor after the main pass
  float glueCompIntensity = 0.4f;
  bool deEsser = false;                // tames "s" and "t" without dulling the rest
  float deEsserIntensity = 0.5f;
  bool highPass = true;                // 80 Hz rumble/handling filter
  bool humFilter = false;              // narrow notches on mains hum (50/60 Hz and harmonics)
  float volume = 0.5f;                 // 0..1 mapped to -18..+18 dB (0.5 = 0 dB); always applied
  // Bands run in the order given. Eight is the cap: past that the popup cannot
  // show them and nobody is EQing a webcam microphone with nine.
  static constexpr int kMaxEqBands = 8;
  std::vector<EqBand> eq;              // empty = flat, and no filter runs at all
  // Stage ids in the order they should run. Empty is the default order; unknown
  // ids are dropped and missing ones appended, so a partial or stale list still
  // produces a complete chain rather than a silent gap.
  std::vector<std::string> chain;
  std::string voice = "none";          // legacy exclusive FX selector (migrated on load)
  bool tape = false, ringMod = false, megaphone = false;
  float tapeMix = 0.45f, ringModMix = 1, megaphoneMix = 1;
  // Pitch and formant are separate on purpose. Moving them together is what a
  // plain resampler does and what "chipmunk" was; moving them apart is what
  // makes a voice bigger or smaller without moving the note, and a note move
  // that still sounds like the same throat.
  float pitch = 0;                     // semitones, -12..+12
  float formant = 0;                   // semitones, -12..+12
  bool doubler = false;                // parallel corrected/delayed vocal aux
  float doublerMix = 0.35f;
  // These are genuine send returns, not alternate reverb modes.  A short
  // slap keeps the lead intimate; the long delay is the rhythmic tail behind
  // it.  They can run with any Space algorithm or with no reverb at all.
  bool slapDelay = false, longDelay = false;
  float slapDelayMix = 0.18f, slapDelayTime = 0.4f;
  float longDelayMix = 0.28f, longDelayTime = 0.4f;
  float longDelayFeedback = 0.32f, longDelayTone = 0.45f;
  // Dry/wet blends, wrapped around whole stages. 1 is the stage exactly as it
  // was before there was a knob, so every default is a no-op; below that, dry
  // comes back. The compressor's is parallel compression and the pitch one is a
  // doubler, which is the whole reason those two are worth having.
  float compMix = 1;                   // 0 = untouched, 1 = fully compressed
  float pitchMix = 1;                  // pitch + formant blend
  float voiceMix = 1;                  // ring mod / megaphone blend
  float spaceMix = 1;                  // reverb blend, on top of the preset's own balance
  std::string space = "none";          // reverb / echo, one of spaceNames()
  // The selected space is its character; these are the controls that shape it.
  // Midpoint values reproduce the original fixed presets.
  float spaceSize = 0.5f;              // compact .. expansive
  float spaceDecay = 0.5f;             // short .. lingering feedback tail
  float spaceTone = 0.5f;              // dark .. bright wet signal
  float spacePreDelay = 0;             // 0 .. 120 ms before the reflections
  float spaceMod = 0;                  // legacy combined modulation control
  float spaceDiffusion = 0.5f;         // sparse early reflections .. dense tail
  float spaceLowCut = 0;               // remove low end from the wet path
  float spaceModRate = 0.5f;           // slow .. fast movement
  float spaceModDepth = 0;             // still .. strongly moving tail
  bool autoTune = false;               // snap the sung (or spoken) pitch onto a musical scale
  // Split the way a pitch corrector's controls always are, rather than folded
  // into one "strength": they are independent, and anyone who has used one
  // reaches for retune speed and correction amount by name.
  float autoTuneSpeed = 0.5f;          // retune speed: 0 = ~200 ms glide, 1 = ~2 ms snap
  float autoTuneAmount = 1.0f;         // how far towards the note: 0 = none, 1 = all the way
  std::string autoTuneKey = "c";       // root the scale is built on, one of keyNames()
  std::string autoTuneScale = "chromatic";  // which notes are allowed, one of scaleNames()

  bool operator==(const MicSettings& o) const {
    return enabled == o.enabled && voiceIsolation == o.voiceIsolation && voiceIsolationIntensity == o.voiceIsolationIntensity &&
           noiseGate == o.noiseGate && noiseGateIntensity == o.noiseGateIntensity && autoLevel == o.autoLevel &&
           autoLevelIntensity == o.autoLevelIntensity && glueComp == o.glueComp && glueCompIntensity == o.glueCompIntensity &&
           deEsser == o.deEsser && deEsserIntensity == o.deEsserIntensity &&
           highPass == o.highPass && humFilter == o.humFilter && volume == o.volume &&
           eq == o.eq && chain == o.chain && voice == o.voice && space == o.space &&
           tape == o.tape && ringMod == o.ringMod && megaphone == o.megaphone &&
           tapeMix == o.tapeMix && ringModMix == o.ringModMix && megaphoneMix == o.megaphoneMix &&
           pitch == o.pitch && formant == o.formant && doubler == o.doubler && doublerMix == o.doublerMix &&
           slapDelay == o.slapDelay && longDelay == o.longDelay && slapDelayMix == o.slapDelayMix &&
           slapDelayTime == o.slapDelayTime && longDelayMix == o.longDelayMix && longDelayTime == o.longDelayTime &&
           longDelayFeedback == o.longDelayFeedback && longDelayTone == o.longDelayTone &&
           compMix == o.compMix && pitchMix == o.pitchMix && voiceMix == o.voiceMix && spaceMix == o.spaceMix &&
           spaceSize == o.spaceSize && spaceDecay == o.spaceDecay && spaceTone == o.spaceTone &&
           spacePreDelay == o.spacePreDelay && spaceMod == o.spaceMod &&
           spaceDiffusion == o.spaceDiffusion && spaceLowCut == o.spaceLowCut &&
           spaceModRate == o.spaceModRate && spaceModDepth == o.spaceModDepth &&
           autoTune == o.autoTune && autoTuneSpeed == o.autoTuneSpeed && autoTuneAmount == o.autoTuneAmount &&
           autoTuneKey == o.autoTuneKey && autoTuneScale == o.autoTuneScale;
  }
  bool operator!=(const MicSettings& o) const { return !(*this == o); }
  // Valid values ("none" first in each).
  static const std::vector<std::string>& eqTypeNames();
  static const std::vector<std::string>& stageNames();   // ids, in default order
  static const std::vector<std::string>& voiceNames();
  static const std::vector<std::string>& spaceNames();
  static const std::vector<std::string>& keyNames();     // "c", "c#", ... "b"
  static const std::vector<std::string>& scaleNames();   // chromatic first: it needs no key at all
};

// ---------------------------------------------------------------------------
// Building blocks

// Direct-form-2 transposed biquad, with the usual RBJ designers.
struct Biquad {
  float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  float z1 = 0, z2 = 0;
  inline float run(float x) {
    float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
  void reset() { z1 = z2 = 0; }
  void bypass() { b0 = 1; b1 = b2 = a1 = a2 = 0; reset(); }
  void highPass(float rate, float f, float q = 0.707f);
  void bandPass(float rate, float f, float q);   // constant 0 dB peak gain
  void lowPass(float rate, float f, float q = 0.707f);
  void peak(float rate, float f, float q, float gainDb);
  void notch(float rate, float f, float q);
  void lowShelf(float rate, float f, float gainDb);
  void highShelf(float rate, float f, float gainDb);
};

// Spectral noise suppression: 2048-point FFT, 75% overlap, per-bin noise floor
// tracked with min-statistics and a Wiener-style gain. The frame has to be
// long enough to resolve the harmonics of a voice (85-255 Hz apart): at 512
// points the bins are 94 Hz wide, speech and noise land in the same bin, and
// measuring the result showed the "intensity" slider making the
// signal-to-noise ratio slightly *worse*. 2048 points (23 Hz bins) is +3 dB
// and improves with intensity, at 32 ms of latency while it is on.
class SpectralDenoise {
public:
  void init(int rate);
  void reset();
  void setIntensity(float v) { intensity_ = v; }
  // Processes n samples in place through the overlap-add buffer.
  void process(float* x, int n);

private:
  void frame();  // one analysis/synthesis frame from in_
  static constexpr int kN = 2048;
  static constexpr int kHop = 512;
  int rate_ = 48000;
  float intensity_ = 0.6f;
  std::vector<float> win_, in_, out_, re_, im_, noise_, gain_;
  std::vector<float> q_;   // finished samples waiting to be handed back
  int qr_ = 0, qw_ = 0, qn_ = 0;
  int fill_ = 0;      // samples of the newest hop staged into in_
  bool ready_ = false;
};

// Formant shift: moves the spectral envelope without moving the pitch.
//
// Every frame, the log magnitude spectrum goes through a cepstrum and comes
// back with only its low quefrencies kept. What survives is the envelope — the
// resonances of throat and mouth — with the vocal folds' harmonic comb removed.
// That separation is the whole trick: the envelope can then be slid up or down
// and applied back as a gain curve, so the note stays exactly where it was
// while the voice sounds like it came out of a bigger or smaller head.
//
// Magnitudes only, original phases kept: this is a time-varying filter, which
// is what a formant shift actually is, and it cannot smear transients the way
// a phase vocoder can.
class FormantShifter {
public:
  void init(int rate);
  void reset();
  void setRatio(float r) { ratio_ = std::clamp(r, 0.5f, 2.f); }   // >1 raises the formants
  void process(float* x, int n);

private:
  void frame();
  static constexpr int kN = 1024;
  static constexpr int kHop = 256;
  // Cepstral coefficients kept. Enough to resolve formants ~500 Hz apart, and
  // still well below the quefrency of any pitch this is used on (a 480 Hz voice
  // peaks at 100), so the envelope never starts tracking the harmonics it is
  // supposed to be ignoring.
  static constexpr int kQuef = 100;
  float ratio_ = 1;
  std::vector<float> win_, in_, out_, re_, im_, cre_, cim_, env_, q_;
  int qr_ = 0, qw_ = 0, qn_ = 0, fill_ = 0;
};

// Monophonic pitch detector, for auto-tune: YIN's cumulative-mean-normalised
// difference function over a 4x decimated copy of the signal. A voice lives
// between 70 and 500 Hz, so 12 kHz resolves it with room to spare and the lag
// search costs a sixteenth of what it would at 48 kHz — which matters, because
// this runs inside the capture callback like everything else here.
class PitchTracker {
public:
  void init(int rate);
  void reset();
  void push(const float* x, int n);   // feed the block that is about to be corrected
  // Period of the last analysis in samples at the full rate; 0 when what it
  // heard was not a note (silence, a consonant, a keyboard).
  float period() const { return period_; }

private:
  void analyse();
  static constexpr int kWin = 512;    // analysis window, in decimated samples
  static constexpr int kHop = 128;    // ~11 ms between analyses at 48 kHz
  static constexpr float kMinHz = 70.f, kMaxHz = 500.f;
  // Below this the dip is a period; above kVoiced there is no periodicity worth
  // tuning. Both are YIN's own numbers, and they hold up on speech.
  static constexpr float kThresh = 0.15f, kVoiced = 0.45f;
  int rate_ = 48000, decim_ = 4, minLag_ = 24, maxLag_ = 172, fill_ = 0, phase_ = 0;
  float dRate_ = 12000.f, period_ = 0;
  Biquad anti_;   // anti-alias, before throwing three samples in four away
  std::vector<float> w_, diff_, cum_;
};

// Period-synchronous pitch shift: a delay line read by two heads exactly one
// detected period apart and crossfaded linearly as the read point drifts.
//
// Splicing on the period is the whole point. PitchShifter above separates its
// heads by a fixed sixth of a second, which is fine for a voice changer's big
// ratios but combs audibly near unity — and near unity is exactly where tuning
// lives. One period apart, the two heads carry the same waveform, so any pair
// of weights summing to one puts it back together.
class PitchCorrector {
public:
  void init(int rate);
  void reset();
  // `period` in samples (<= 0: nothing periodic to splice on, so it parks and
  // becomes a plain delay), `ratio` the wanted output pitch over the input's.
  void process(float* x, int n, float period, float ratio);
  int latency() const { return (int)base_; }   // constant while it is in the chain

private:
  std::vector<float> buf_;
  size_t w_ = 0;
  float phase_ = 0;    // 0..1 of a period: how far the read point has drifted
  float base_ = 0;     // the delay both heads sit behind
  float park_ = 0;     // per-sample drift back onto a period boundary when idle
};

// Schroeder reverb (4 combs + 2 allpass) and a feedback delay, sharing the
// same "space" stage.
class Space {
public:
  void init(int rate);
  void reset();
  void configure(const std::string& kind, float size = 0.5f, float decay = 0.5f,
                 float tone = 0.5f, float preDelay = 0, float diffusion = 0.5f,
                 float lowCut = 0, float modRate = 0.5f, float modDepth = 0);
  void process(float* x, int n);

private:
  // Every line is allocated once, for the longest preset; configure() only
  // moves `len` inside it. Nothing here may allocate: it runs on the audio
  // thread, from setSettings.
  struct Line { std::vector<float> buf; size_t len = 16, idx = 0; float fb = 0; };
  void setLine(Line& l, float ms, float fb);
  int rate_ = 48000;
  std::string kind_ = "none";
  Line comb_[4], all_[2], echo_, pre_;
  float mix_ = 0, damp_ = 0, trim_ = 1, lp_[4] = { 0, 0, 0, 0 };
  // Underwater: the echo line read through a slowly wandering tap, plus a lid
  // on the top end.
  Biquad lid_, lowCut_;
  float lfo_ = 0, lfoStep_ = 0, wobble_ = 0, mod_ = 0;
  bool preOn_ = false;
};

// A dedicated mono double: two gently wandering short taps.  Unlike blending
// an entire pitch-corrected lead back into itself, neither tap shares the same
// delay, so it thickens without the obvious comb-filter hollowing.
class Doubler {
public:
  void init(int rate);
  void reset();
  void process(const float* in, float* out, int n, float mix);
private:
  std::vector<float> buf_;
  size_t w_ = 0;
  int rate_ = 48000;
  float phaseA_ = 0, phaseB_ = 0;
};

// Parallel vocal delay returns.  The reverb remains its own processor, so a
// user can keep a small room while independently choosing slap and long tail.
class VocalDelay {
public:
  void init(int rate);
  void reset();
  void configure(bool slapOn, float slapTime, bool longOn, float longTime,
                 float feedback, float tone);
  void process(const float* in, float* out, int n, float slapMix, float longMix);
private:
  struct Line { std::vector<float> buf; size_t len = 16, idx = 0; float fb = 0, state = 0; };
  void setLine(Line& line, float ms, float feedback);
  int rate_ = 48000;
  Line slap_, long_;
  bool slapOn_ = false, longOn_ = false;
  float damp_ = 0.55f;
};

// ---------------------------------------------------------------------------

// The whole chain. Owned and driven by the PipeWire capture callback in
// audio.cpp: setRate/setSettings are called from that same thread.
class MicProcessor {
public:
  void setRate(int rate);
  void setSettings(const MicSettings& s);
  void reset();
  // Mono, in place. Also updates the levels below.
  void process(float* x, int n);
  // Peak levels of the last blocks, 0..1, for the panel's meter (any thread).
  float inLevel() const { return inLevel_.load(); }
  float outLevel() const { return outLevel_.load(); }
  void clearLevels() { inHold_ = outHold_ = 0; inLevel_ = 0; outLevel_ = 0; }

private:
  void rebuild();   // re-design the filters after a rate or settings change
  float snapHz(float f) const;   // nearest frequency the current scale allows

  MicSettings s_;
  int rate_ = 48000;
  bool dirty_ = true;
  bool inited_ = false;   // the buffers below are sized for rate_
  Biquad hp_, voiceHp_, voiceLp_, essLp_;
  Biquad eq_[MicSettings::kMaxEqBands];
  int eqN_ = 0;                   // how many of eq_ are designed and running
  int order_[StCount];            // the stage sequence this block will run
  int orderN_ = 0;
  // Mains hum: 50 and 60 Hz plus two harmonics each, narrow enough to leave
  // even a deep voice alone. Both mains frequencies are notched, so nobody has
  // to know which country's wiring their buzz comes from.
  static constexpr int kHum = 4;
  Biquad hum_[kHum];
  SpectralDenoise denoise_;
  FormantShifter formant_;
  PitchTracker tracker_;
  PitchCorrector corrector_;
  Doubler doubler_;
  Space space_;
  VocalDelay delay_;
  float gateEnv_ = 0, gateGain_ = 0;      // envelope follower and the gate's current gain
  float compEnv_ = 0, compGain_ = 1, glueEnv_ = 0, glueGain_ = 1;
  float essHi_ = 0, essAll_ = 0, essGain_ = 1;   // de-esser: high band, whole band, current cut
  float ringPhase_ = 0, ringHz_ = 0, ringMix_ = 0;
  float drive_ = 0, driveOut_ = 1;         // >0: soft-clip drive (megaphone, monster) and its make-down
  float outTrim_ = 1;                      // per-voice / per-space level match
  float outGain_ = 1;            // target output gain (volume x voice trim)
  float gainNow_ = 1;            // and the glided value actually applied
  float gainCoef_ = 1;
  bool gainPrimed_ = false;
  // Scratch for the dry side of a blend. Sized once, never on the audio thread;
  // a block bigger than this simply runs without its mix rather than allocating
  // inside the callback.
  std::vector<float> dry_;
  float pitchRatio_ = 1, formantRatio_ = 1;   // from the Pitch / Formant knobs
  // Auto-tune: the correction being applied, the period it splices on (smoothed,
  // because that is the distance between the corrector's heads), how far towards
  // the note to travel and how long to take about it.
  float tuneRatio_ = 1, tunePeriod_ = 0, tuneAmount_ = 1, tuneTau_ = 0.01f;
  bool tuneMask_[12] = { false };   // pitch classes the key and scale allow
  std::atomic<float> inLevel_{0}, outLevel_{0};
  // Level meters decay so a short peak stays readable in the panel.
  float inHold_ = 0, outHold_ = 0;
};
