#include "../Source/LeadEngine.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>

// This observes real allocations, including MIDI and automation, instead of
// merely checking the engine's source for calls to new/malloc.
static std::atomic<std::size_t> allocationCount {0};
static std::atomic<bool> countAllocations {false};

#if defined(__GNUC__) || defined(__clang__)
#define SCULPT_TEST_NOINLINE __attribute__((noinline))
#else
#define SCULPT_TEST_NOINLINE
#endif

SCULPT_TEST_NOINLINE void* operator new(std::size_t size) {
    if (countAllocations.load(std::memory_order_relaxed)) allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size == 0 ? 1 : size)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
SCULPT_TEST_NOINLINE void operator delete(void* p) noexcept { std::free(p); }
SCULPT_TEST_NOINLINE void operator delete[](void* p) noexcept { std::free(p); }
SCULPT_TEST_NOINLINE void operator delete(void* p, std::size_t) noexcept { std::free(p); }
SCULPT_TEST_NOINLINE void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#if defined(__cpp_aligned_new)
void* operator new(std::size_t size, std::align_val_t alignment) {
    if (countAllocations.load(std::memory_order_relaxed)) allocationCount.fetch_add(1, std::memory_order_relaxed);
    void* p = nullptr;
    if (posix_memalign(&p, static_cast<std::size_t>(alignment), size == 0 ? 1 : size) == 0) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size, std::align_val_t alignment) { return ::operator new(size, alignment); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
#endif
#undef SCULPT_TEST_NOINLINE

namespace {
using sculptlead::Bank;
using sculptlead::Engine;
using sculptlead::Instrument;
using sculptlead::Parameters;
using sculptlead::Sample;
constexpr double pi = 3.1415926535897932384626433832795;

void require(bool condition, const char* explanation) {
    if (!condition) throw std::runtime_error(explanation);
}

// A deliberately known pitched source tests real resampling/loop playback.
// This is only a test fixture; no synthetic acoustic fallback ships in the VI.
std::shared_ptr<const Bank> makeBank() {
    auto bank = std::make_shared<Bank>();
    for (int instrument = 0; instrument < 4; ++instrument)
        for (int dynamic = 0; dynamic < 2; ++dynamic)
            for (int rr = 0; rr < 2; ++rr) {
                Sample sample;
                sample.instrument = static_cast<Instrument>(instrument);
                sample.rootNote = 69;
                sample.velocityLayer = dynamic;
                sample.velocityCentre = dynamic ? .85f : .35f;
                sample.roundRobin = rr;
                sample.sampleRate = 48000.0;
                sample.loopStart = 12000;
                sample.loopEnd = 24000; // Exactly 110 cycles, with sustained preroll.
                sample.samples.resize(30000);
                for (std::size_t i = 0; i < sample.samples.size(); ++i) {
                    const double phase = 2.0 * pi * 440.0 * static_cast<double>(i) / 48000.0 + rr * .08;
                    const double attack = std::min(1.0, static_cast<double>(i) / 240.0);
                    const double amplitude = dynamic ? .17 : .085;
                    sample.samples[i] = static_cast<float>(attack * amplitude *
                        (std::sin(phase) + (.04 + .01 * instrument) * std::sin(2.0 * phase)));
                }
                bank->samples.push_back(std::move(sample));
            }
    return bank;
}

void settle(Engine& engine, double seconds, double rate = 48000.0) {
    std::array<float, 256> left {}, right {};
    int remaining = static_cast<int>(seconds * rate);
    while (remaining > 0) {
        const int frames = std::min(remaining, 256);
        engine.render(left.data(), right.data(), frames);
        for (int i = 0; i < frames; ++i) {
            require(std::isfinite(left[static_cast<std::size_t>(i)]), "non-finite sample during playback");
            require(left[static_cast<std::size_t>(i)] == right[static_cast<std::size_t>(i)], "mono voice moved away from the centre");
            require(std::abs(left[static_cast<std::size_t>(i)]) < .981f, "peak protection failed");
        }
        remaining -= frames;
    }
}

std::vector<float> capture(Engine& engine, double seconds, double rate = 48000.0) {
    std::vector<float> data(static_cast<std::size_t>(seconds * rate));
    std::array<float, 256> right {};
    for (std::size_t offset = 0; offset < data.size();) {
        const int count = static_cast<int>(std::min<std::size_t>(256, data.size() - offset));
        engine.render(data.data() + offset, right.data(), count);
        offset += static_cast<std::size_t>(count);
    }
    return data;
}

double rms(const std::vector<float>& data) {
    double power = 0.0;
    for (float x : data) power += static_cast<double>(x) * x;
    return data.empty() ? 0.0 : std::sqrt(power / static_cast<double>(data.size()));
}

std::vector<double> positiveCrossings(const std::vector<float>& data) {
    std::vector<double> crossings;
    for (std::size_t i = 1; i < data.size(); ++i)
        if (data[i - 1] <= 0.0f && data[i] > 0.0f)
            crossings.push_back(static_cast<double>(i - 1) - data[i - 1] / static_cast<double>(data[i] - data[i - 1]));
    return crossings;
}

double measuredFrequency(const std::vector<float>& data, double rate) {
    const auto crossings = positiveCrossings(data);
    require(crossings.size() > 3, "not enough audible cycles to measure pitch");
    return rate * static_cast<double>(crossings.size() - 1) / (crossings.back() - crossings.front());
}

void priorityAndArticulation(const std::shared_ptr<const Bank>& bank) {
    Engine engine;
    engine.setBank(bank);
    engine.noteOn(1, 60, .8f);
    settle(engine, .03);
    const int firstRR = engine.getRoundRobinIndex();
    engine.noteOn(1, 64, .7f);
    require(engine.getCurrentNote() == 64, "newest note must have priority");
    require(engine.getRoundRobinIndex() == firstRR, "overlapping legato changed recorded RR");
    engine.noteOn(1, 67, .9f);
    engine.noteOff(1, 64);
    require(engine.getCurrentNote() == 67, "releasing a background key stopped the current key");
    engine.noteOff(1, 67);
    require(engine.getCurrentNote() == 60, "releasing the newest key did not return to the still-held key");
    require(engine.getRoundRobinIndex() == firstRR, "legato return advanced RR");
    engine.noteOff(1, 60);
    require(engine.getCurrentNote() == -1, "final key release left a key active");
    settle(engine, .01);
    engine.noteOn(1, 60, .8f);
    require(engine.getRoundRobinIndex() != firstRR, "attack after a gap did not advance RR");
    engine.noteOff(1, 60);
    settle(engine, .6);
    require(!engine.isActive(), "release tail did not finish");
    require(rms(capture(engine, .1)) == 0.0, "finished release was not silent");
}

void repeatedNotesAndChannels(const std::shared_ptr<const Bank>& bank) {
    Engine engine;
    engine.setBank(bank);
    engine.noteOn(1, 60, .8f);
    const int rr = engine.getRoundRobinIndex();
    engine.noteOn(1, 60, .8f);
    require(engine.getRoundRobinIndex() != rr, "a repeated same-key attack did not rearticulate");
    engine.noteOff(1, 60);
    require(engine.getCurrentNote() == 60, "first of two matching note-offs killed both repeated note-ons");
    engine.noteOff(1, 60);
    require(engine.getCurrentNote() == -1, "second matching note-off left a repeated note stuck");

    engine.noteOn(1, 64, .8f);
    engine.noteOn(2, 64, .8f);
    engine.noteOff(1, 64);
    require(engine.getCurrentNote() == 64, "same-pitch note-off on a different channel killed the active note");
    engine.noteOff(3, 64);
    require(engine.getCurrentNote() == 64, "unmatched note-off changed priority");
    engine.noteOff(2, 64);
    require(engine.getCurrentNote() == -1, "channel-specific note-off did not release its note");
    engine.noteOn(0, 60, 1.0f);
    engine.noteOn(17, 60, 1.0f);
    engine.noteOn(1, -1, 1.0f);
    engine.noteOn(1, 128, 1.0f);
    require(engine.getCurrentNote() == -1, "invalid MIDI input was accepted");
}

void pedalsAndPanic() {
    Engine engine;
    engine.setParameters({.75f, 0.0f, 0.0f});
    engine.noteOn(1, 60, .8f);
    engine.controller(1, 64, 127);
    engine.noteOff(1, 60);
    require(engine.getCurrentNote() == 60, "sustain pedal did not retain released note");
    engine.noteOn(2, 67, .8f);
    engine.noteOff(2, 67);
    require(engine.getCurrentNote() == 60, "last-note priority did not return to sustained note");
    engine.controller(2, 64, 0);
    require(engine.getCurrentNote() == 60, "another channel's pedal released the sustained note");
    engine.controller(1, 64, 0);
    require(engine.getCurrentNote() == -1, "pedal release left a stuck note");

    engine.noteOn(1, 60, .8f);
    engine.controller(1, 64, 127);
    engine.controller(1, 123, 0);
    require(engine.getCurrentNote() == 60, "CC123 did not honour sustain");
    engine.controller(1, 64, 0);
    require(engine.getCurrentNote() == -1, "CC123 sustained key survived pedal release");

    engine.noteOn(1, 60, .8f);
    engine.noteOn(2, 67, .8f);
    settle(engine, .02);
    engine.controller(1, 120, 0);
    require(engine.getCurrentNote() == 67 && engine.isActive(), "CC120 on an inactive channel killed another channel");
    engine.noteOff(2, 67);
    engine.controller(1, 120, 0);
    require(engine.isActive(), "CC120 on another channel killed the current release tail");
    engine.controller(2, 120, 0);
    require(!engine.isActive(), "CC120 failed to kill the channel's release tail immediately");
    require(rms(capture(engine, .02)) == 0.0, "CC120 left audible output");

    engine.noteOn(1, 60, .8f);
    engine.noteOn(2, 67, .8f);
    engine.controller(2, 120, 0);
    require(engine.getCurrentNote() == 60, "channel panic did not restore another held channel");
    engine.controller(1, 123, 0);
    require(engine.getCurrentNote() == -1, "All Notes Off failed without pedal");
    engine.reset();
    require(!engine.isActive(), "reset did not clear the voice");
}

void samplePitchLoopsAndRates(const std::shared_ptr<const Bank>& bank) {
    for (const double rate : {44100.0, 48000.0, 96000.0}) {
        Engine engine;
        engine.setBank(bank);
        engine.prepare(rate);
        engine.noteOn(1, 69, .9f);
        settle(engine, .2, rate);
        const auto initial = capture(engine, .3, rate);
        require(std::abs(measuredFrequency(initial, rate) - 440.0) < .1, "sample source/output-rate conversion detuned A4");
        const double initialRms = rms(initial);
        settle(engine, 6.0, rate);
        const auto held = capture(engine, .4, rate);
        require(rms(held) > .05 && std::abs(rms(held) / initialRms - 1.0) < .04, "held acoustic loop decayed, stopped or changed level");
        float maxDelta = 0.0f;
        for (std::size_t i = 1; i < held.size(); ++i) maxDelta = std::max(maxDelta, std::abs(held[i] - held[i - 1]));
        require(maxDelta < .025f, "loop wrap created a click larger than the source waveform slope");
        engine.noteOn(1, 81, .9f);
        settle(engine, .08, rate);
        require(std::abs(measuredFrequency(capture(engine, .25, rate), rate) - 880.0) < .15, "legato sample pitch did not transpose one octave");
        engine.noteOff(1, 81);
        settle(engine, .08, rate);
        require(std::abs(measuredFrequency(capture(engine, .25, rate), rate) - 440.0) < .1, "held-note return produced the wrong pitch");
        engine.noteOff(1, 69);
        settle(engine, .6, rate);
        require(!engine.isActive(), "release duration depended incorrectly on output sample rate");
    }
}

void expressionPitchAndVibrato(const std::shared_ptr<const Bank>& bank) {
    Engine engine;
    engine.setBank(bank);
    engine.noteOn(1, 69, .9f);
    settle(engine, .1);
    engine.pitchWheel(1, 16383);
    settle(engine, .08);
    require(std::abs(measuredFrequency(capture(engine, .3), 48000.0) - 493.8833) < .15, "maximum pitch bend is not +2 semitones");
    engine.pitchWheel(1, 0);
    settle(engine, .08);
    require(std::abs(measuredFrequency(capture(engine, .3), 48000.0) - 391.9954) < .15, "minimum pitch bend is not -2 semitones");
    engine.pitchWheel(2, 16383);
    settle(engine, .03);
    require(std::abs(measuredFrequency(capture(engine, .2), 48000.0) - 391.9954) < .15, "other channel's pitch bend changed the active voice");
    const double before = rms(capture(engine, .1));
    engine.controller(2, 11, 0);
    settle(engine, .1);
    require(rms(capture(engine, .1)) > before * .95, "expression from inactive channel muted active note");
    engine.controller(1, 11, 0);
    settle(engine, .15);
    require(rms(capture(engine, .1)) < 1.0e-7, "CC11 zero did not silence the expression envelope");
    engine.controller(1, 11, 127);
    engine.pitchWheel(1, 8192);
    engine.noteOn(1, 45, .9f);
    engine.controller(1, 1, 127);
    settle(engine, .2);
    const auto crossings = positiveCrossings(capture(engine, 1.1));
    double shortest = 1.0e9, longest = 0.0;
    for (std::size_t i = 1; i < crossings.size(); ++i) {
        shortest = std::min(shortest, crossings[i] - crossings[i - 1]);
        longest = std::max(longest, crossings[i] - crossings[i - 1]);
    }
    require(longest - shortest > 5.0 && longest - shortest < 12.0, "CC1 did not produce a controlled musical vibrato");
}

void controllersBeforeAttack(const std::shared_ptr<const Bank>& bank) {
    Engine engine;
    engine.setBank(bank);
    engine.setParameters({.5f, 0.0f, 0.0f});
    engine.reset();
    engine.controller(1, 11, 0);
    engine.noteOn(1, 69, .9f);
    require(rms(capture(engine, .05)) == 0.0, "CC11 set to zero before note-on produced an attack burst");

    engine.setParameters({0.0f, 0.0f, 0.0f});
    engine.reset();
    engine.controller(1, 1, 0);
    engine.pitchWheel(1, 16383);
    engine.noteOn(1, 69, .9f);
    // Measure the very first periods, before a 3 ms smoothing transient could
    // disappear. A known sampled sine makes this independent of synth spectra.
    auto crossings = positiveCrossings(capture(engine, .012));
    require(crossings.size() >= 4, "pre-note pitch test did not produce enough cycles");
    const double firstPeriodHz = 48000.0 / (crossings[1] - crossings[0]);
    require(std::abs(firstPeriodHz - 493.8833) < .25, "pre-note pitch bend was not applied from the first audible cycle");

    // Finish the first channel completely. A new channel must start with its
    // own controllers, rather than leaking channel 1's last bend/expression.
    engine.noteOff(1, 69);
    settle(engine, .6);
    engine.controller(2, 11, 0);
    engine.pitchWheel(2, 0);
    engine.controller(2, 1, 127);
    engine.noteOn(2, 69, .9f);
    require(rms(capture(engine, .05)) == 0.0, "new silent channel inherited the previous channel's expression at attack");
    engine.controller(2, 120, 0);
    engine.controller(2, 11, 127);
    engine.controller(2, 1, 0);
    engine.noteOn(2, 69, .9f);
    crossings = positiveCrossings(capture(engine, .015));
    require(crossings.size() >= 4, "new-channel pitch test did not produce enough cycles");
    require(std::abs(48000.0 / (crossings[1] - crossings[0]) - 391.9954) < .25,
            "new channel's pre-note pitch bend was not applied from its first audible cycle");
}

void glideIsLegato() {
    Engine engine;
    engine.setParameters({1.0f, 0.0f, 100.0f});
    engine.reset();
    engine.noteOn(1, 57, .9f);
    settle(engine, .1);
    engine.noteOn(1, 69, .9f);
    settle(engine, .02);
    // FM can contain extra zero crossings; use the stable lower-band analog
    // voice for a time-domain pitch estimate while retaining the glide state.
    engine.setParameters({.5f, 0.0f, 100.0f});
    settle(engine, .06);
    const double middle = measuredFrequency(capture(engine, .04), 48000.0);
    require(middle > 310.0 && middle < 400.0, "100 ms glide jumped immediately or failed to move toward the new key");
    settle(engine, .7);
    require(std::abs(measuredFrequency(capture(engine, .2), 48000.0) - 440.0) < 1.0, "glide did not settle to the target pitch");
}

void morphAndSynthBalance(const std::shared_ptr<const Bank>& bank) {
    Engine engine;
    engine.setBank(bank);
    engine.noteOn(1, 69, .9f);
    settle(engine, .2);
    std::array<float, 64> left {}, right {};
    float previous = 0.0f, biggestStep = 0.0f;
    for (int block = 0; block < 1800; ++block) {
        const float morph = block < 900 ? static_cast<float>(block) / 899.0f : (block & 1 ? 0.0f : 1.0f);
        engine.setParameters({morph, (block % 31 == 0) ? -12.0f : 0.0f, 0.0f});
        engine.render(left.data(), right.data(), static_cast<int>(left.size()));
        for (float value : left) {
            require(std::isfinite(value) && std::abs(value) < .981f, "rapid morph/volume automation produced invalid audio");
            biggestStep = std::max(biggestStep, std::abs(value - previous));
            previous = value;
        }
    }
    require(biggestStep < .09f, "macro automation produced an excessive discontinuity");

    for (int note : {48, 60, 72, 84}) {
        double quietest = 1.0, loudest = 0.0;
        for (float morph : {.5f, .75f, 1.0f}) {
            engine.reset();
            engine.setParameters({morph, 0.0f, 0.0f});
            engine.noteOn(1, note, .9f);
            settle(engine, .35);
            const double level = rms(capture(engine, .35));
            require(level > .035 && level < .22, "synth endpoint was silent, weak, or excessively loud");
            quietest = std::min(quietest, level);
            loudest = std::max(loudest, level);
        }
        require(loudest / quietest < 2.0, "Analog/Dance/Digital sustain levels differ by more than 6 dB");
    }
}

void invalidBankAndParameters() {
    Engine engine;
    engine.noteOn(1, 69, .8f);
    require(rms(capture(engine, .1)) == 0.0, "missing acoustic bank silently substituted synthetic samples");
    auto invalid = std::make_shared<Bank>();
    Sample sample;
    sample.samples.assign(100, std::numeric_limits<float>::quiet_NaN());
    invalid->samples.push_back(sample);
    sample.samples.assign(100, .1f);
    sample.sampleRate = std::numeric_limits<double>::infinity();
    invalid->samples.push_back(sample);
    engine.setBank(invalid);
    engine.noteOn(1, 60, .8f);
    require(rms(capture(engine, .1)) == 0.0, "invalid samples were not rejected");
    engine.setParameters({std::numeric_limits<float>::quiet_NaN(),
                          std::numeric_limits<float>::infinity(), -1.0f});
    settle(engine, .03);
    engine.setParameters({1.0f, 0.0f, 0.0f});
    settle(engine, .1);
    require(rms(capture(engine, .1)) > .03, "synth did not work independently of missing/invalid acoustic files");
}

void realtimeAllocationsAndStress(const std::shared_ptr<const Bank>& bank) {
    Engine engine;
    engine.setBank(bank);
    std::array<float, 128> left {}, right {};
    allocationCount.store(0, std::memory_order_relaxed);
    bool finite = true;
    countAllocations.store(true, std::memory_order_relaxed);
    for (int block = 0; block < 3000; ++block) {
        const int channel = block % 16 + 1;
        const int note = block * 17 % 128;
        engine.setParameters({static_cast<float>(block % 101) / 100.0f,
                              static_cast<float>(block % 25 - 12), static_cast<float>(block % 251)});
        engine.noteOn(channel, note, .1f + .9f * static_cast<float>(block % 127) / 127.0f);
        engine.controller(channel, 1, block % 128);
        engine.controller(channel, 11, 63 + block % 65);
        engine.pitchWheel(channel, block * 97 % 16384);
        engine.controller(channel, 64, block % 3 == 0 ? 127 : 0);
        engine.render(left.data(), right.data(), static_cast<int>(left.size()));
        for (float x : left) finite = finite && std::isfinite(x) && std::abs(x) < .981f;
        engine.noteOff(channel, note);
        if (block % 19 == 0) engine.controller(channel, 123, 0);
        if (block % 47 == 0) engine.controller(channel, 120, 0);
        if (block % 173 == 0) engine.reset();
    }
    engine.render(nullptr, nullptr, 8);
    engine.render(left.data(), left.data(), 8);
    engine.render(left.data(), right.data(), 0);
    countAllocations.store(false, std::memory_order_relaxed);
    require(allocationCount.load(std::memory_order_relaxed) == 0, "MIDI/render/automation allocated memory on the realtime thread");
    require(finite, "dense extreme MIDI/automation caused non-finite or unbounded output");
    for (int channel = 1; channel <= 16; ++channel) engine.controller(channel, 120, 0);
    require(!engine.isActive(), "multi-channel panic left an active voice");
}

void reportBenchmark(const std::shared_ptr<const Bank>& bank) {
    Engine engine;
    engine.setBank(bank);
    engine.setParameters({.125f, 0.0f, 0.0f}); // Reed/Brass crossover: three audible sample readers.
    engine.noteOn(1, 65, .9f);
    std::array<float, 128> left {}, right {};
    const auto started = std::chrono::steady_clock::now();
    for (int block = 0; block < 3750; ++block) engine.render(left.data(), right.data(), 128);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::cout << "  Informational CPU: " << std::fixed << std::setprecision(3) << seconds
              << " s for 10 s audio at 48 kHz (" << seconds * 10.0
              << "% of one core on this test host; not a macOS measurement)\n";
}
} // namespace

int main() {
    try {
        const auto bank = makeBank();
        priorityAndArticulation(bank);
        std::cout << "PASS mono priority, legato, RR and release\n";
        repeatedNotesAndChannels(bank);
        std::cout << "PASS repeated notes and MIDI channel isolation\n";
        pedalsAndPanic();
        std::cout << "PASS sustain, CC120/123 and tail panic\n";
        samplePitchLoopsAndRates(bank);
        std::cout << "PASS real sample resampling and sustained loops at 44.1/48/96 kHz\n";
        expressionPitchAndVibrato(bank);
        std::cout << "PASS pitch wheel, expression and vibrato\n";
        controllersBeforeAttack(bank);
        std::cout << "PASS pre-note controllers and channel changes after silence\n";
        glideIsLegato();
        std::cout << "PASS legato glide\n";
        morphAndSynthBalance(bank);
        std::cout << "PASS morph continuity and Analog/Dance/FM balance\n";
        invalidBankAndParameters();
        std::cout << "PASS missing/invalid bank and non-finite parameter handling\n";
        realtimeAllocationsAndStress(bank);
        std::cout << "PASS realtime allocation check and dense MIDI stress\n";
        reportBenchmark(bank);
        std::cout << "All Sculpt Lead core tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        countAllocations.store(false, std::memory_order_relaxed);
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
