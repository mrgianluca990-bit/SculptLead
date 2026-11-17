#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace sculptlead {

enum class Instrument { clarinet, flute, trombone, trumpet };

struct Sample {
    Instrument instrument = Instrument::clarinet;
    int rootNote = 60;
    int velocityLayer = 0;
    float velocityCentre = .7f;
    int roundRobin = 0;
    double sampleRate = 48000.0;
    std::vector<float> samples;
    int loopStart = 0;
    int loopEnd = 0; // Exclusive. An invalid loop makes the region a one-shot.
    float gain = 1.0f;
};

struct Bank { std::vector<Sample> samples; };

struct Parameters {
    float morph = 0.0f;
    float outputDb = 0.0f;
    float glideMs = 0.0f;
};

// One melodic voice, always monophonic. Notes on different MIDI channels share
// its last-note-priority stack, while pedals, expression and pitch bend remain
// channel-specific. The short old/new sample crossfades are transitions of that
// voice, not additional held voices.
//
// Bank/prepare operations must run off the audio thread, with rendering stopped.
// A Bank stays immutable after installation. All MIDI and render operations are
// bounded and perform no allocation, locking or file access. Calls on an Engine
// must be serialized by its owner (normally the host's audio callback).
//
// Factory RR contains recorded attack alternatives where supplied by the bank.
// Legato is programmed: it preserves articulation/RR and crossfades sustained
// regions; it is not a library of recorded interval transitions.
class Engine {
public:
    Engine() { prepare(48000.0); }

    void prepare(double rate) {
        sampleRate_ = std::isfinite(rate) && rate >= 8000.0 && rate <= 768000.0
                    ? rate : 48000.0;
        parameterStep_ = coefficient(.012);
        controlStep_ = coefficient(.006);
        pitchStep_ = coefficient(.003);
        velocityStep_ = coefficient(.007);
        attackStep_ = coefficient(.002);
        releaseMultiplier_ = 1.0f - coefficient(.035);
        transientMultiplier_ = 1.0f - coefficient(.075);
        peakMultiplier_ = 1.0f - coefficient(.150);
        sampleFadeFrames_ = std::max(1, static_cast<int>(sampleRate_ * .008));
        for (std::size_t i = 0; i < noteFrequencies_.size(); ++i)
            noteFrequencies_[i] = 440.0 * std::exp2((static_cast<double>(i) - 69.0) / 12.0);
        for (int i = 0; i <= tableSize_; ++i)
            sineTable_[static_cast<std::size_t>(i)] = static_cast<float>(std::sin(twoPi_ * i / tableSize_));
        for (int i = 0; i <= bendTableSize_; ++i)
            bendTable_[static_cast<std::size_t>(i)] = static_cast<float>(std::exp2((-3.0 + 6.0 * i / bendTableSize_) / 12.0));
        buildDigitalTables();
        for (auto& region : regions_) updateRegionRate(region);
        setParameters(parameters_);
        reset();
    }

    void setBank(std::shared_ptr<const Bank> bank) {
        reset();
        regions_.clear();
        choices_.clear();
        bank_ = std::move(bank);
        if (!bank_) return;
        regions_.reserve(bank_->samples.size());
        for (const auto& sample : bank_->samples) {
            if (!validSample(sample)) continue;
            Region region;
            region.sample = &sample;
            region.gain = std::clamp(sample.gain, 0.0f, 8.0f);
            const auto size = static_cast<int>(std::min<std::size_t>(sample.samples.size(), static_cast<std::size_t>(std::numeric_limits<int>::max())));
            if (sample.loopStart >= 0 && sample.loopEnd > sample.loopStart && sample.loopEnd <= size
                && sample.loopEnd - sample.loopStart >= 16) {
                region.loopStart = sample.loopStart;
                region.loopEnd = sample.loopEnd;
                region.crossfade = std::max(0, std::min({
                    static_cast<int>(sample.sampleRate * .020),
                    (sample.loopEnd - sample.loopStart) / 4,
                    sample.loopStart
                }));
            }
            updateRegionRate(region);
            regions_.push_back(region);
        }
        if (regions_.empty()) return;
        choices_.resize(4 * 128 * velocityBuckets_);
        for (int instrument = 0; instrument < 4; ++instrument)
            for (int note = 0; note < 128; ++note)
                for (int velocity = 0; velocity < velocityBuckets_; ++velocity)
                    buildChoice(instrument, note, velocity);
    }

    void setParameters(const Parameters& parameters) noexcept {
        parameters_.morph = finiteClamp(parameters.morph, 0.0f, 1.0f, 0.0f);
        parameters_.outputDb = finiteClamp(parameters.outputDb, -60.0f, 12.0f, 0.0f);
        parameters_.glideMs = finiteClamp(parameters.glideMs, 0.0f, 250.0f, 0.0f);
        targetOutput_ = std::pow(10.0f, parameters_.outputDb * .05f);
        // Zero glide still has a very short click-prevention transition.
        glideStep_ = coefficient(std::max(.001, static_cast<double>(parameters_.glideMs) * .001));
    }

    void reset() noexcept {
        for (auto& channel : notes_) for (auto& note : channel) note = {};
        for (auto& channel : channels_) channel = {};
        sampleVoices_ = {};
        analogFilter_ = {};
        danceFilter_ = {};
        activeNote_ = -1;
        activeChannel_ = -1;
        order_ = 0;
        nextRoundRobin_ = 0;
        activeRoundRobin_ = 0;
        displayedRoundRobin_ = 0;
        envelope_ = 0.0f;
        transient_ = 0.0f;
        peak_ = 0.0f;
        morph_ = parameters_.morph;
        output_ = targetOutput_;
        expression_ = targetExpression_ = 1.0f;
        modulation_ = targetModulation_ = 0.0f;
        bend_ = targetBend_ = 0.0f;
        velocity_ = targetVelocity_ = 1.0f;
        frequency_ = targetFrequency_ = 440.0;
        registerNote_ = 69.0f;
        phase_ = phaseDetuned_ = vibratoPhase_ = 0.0;
        filterCountdown_ = 0;
    }

    void noteOn(int channel1to16, int note, float velocity) noexcept {
        if (!validChannel(channel1to16) || note < 0 || note > 127) return;
        if (!std::isfinite(velocity) || velocity <= 0.0f) {
            noteOff(channel1to16, note);
            return;
        }
        const int channel = channel1to16 - 1;
        auto& held = notes_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(note)];
        if (held.count < std::numeric_limits<std::uint16_t>::max()) ++held.count;
        held.sustained = false;
        held.velocity = std::clamp(velocity, .001f, 1.0f);
        held.order = ++order_;
        const bool articulate = activeNote_ < 0
                             || (activeNote_ == note && activeChannel_ == channel);
        startNote(channel, note, held.velocity, articulate);
    }

    void noteOff(int channel1to16, int note) noexcept {
        if (!validChannel(channel1to16) || note < 0 || note > 127) return;
        const int channel = channel1to16 - 1;
        auto& held = notes_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(note)];
        if (held.count == 0) return; // Stale/unmatched note-off cannot kill another channel.
        --held.count;
        if (held.count == 0)
            held.sustained = channels_[static_cast<std::size_t>(channel)].pedal;
        updatePriority();
    }

    void controller(int channel1to16, int cc, int value) noexcept {
        if (!validChannel(channel1to16)) return;
        const int channel = channel1to16 - 1;
        value = std::clamp(value, 0, 127);
        auto& state = channels_[static_cast<std::size_t>(channel)];
        if (cc == 1) state.modulation = static_cast<float>(value) / 127.0f;
        else if (cc == 11) state.expression = static_cast<float>(value) / 127.0f;
        else if (cc == 64) {
            const bool down = value >= 64;
            if (state.pedal && !down)
                for (auto& held : notes_[static_cast<std::size_t>(channel)]) held.sustained = false;
            state.pedal = down;
            updatePriority();
        } else if (cc == 120) {
            for (auto& held : notes_[static_cast<std::size_t>(channel)]) held = {};
            state.pedal = false;
            if (activeChannel_ == channel) {
                stopSound(); // All Sound Off is immediate, including the release tail.
                updatePriority();
            }
        } else if (cc == 123) {
            for (auto& held : notes_[static_cast<std::size_t>(channel)]) {
                if (held.count > 0) held.sustained = state.pedal;
                held.count = 0;
            }
            updatePriority(); // All Notes Off honours this channel's sustain pedal.
        } else if (cc == 121) {
            state = {};
            for (auto& held : notes_[static_cast<std::size_t>(channel)]) held.sustained = false;
            updatePriority();
        }
        if (channel == activeChannel_) updateControllerTargets();
    }

    void pitchWheel(int channel1to16, int value0to16383) noexcept {
        if (!validChannel(channel1to16)) return;
        const int value = std::clamp(value0to16383, 0, 16383);
        auto& state = channels_[static_cast<std::size_t>(channel1to16 - 1)];
        state.bend = value >= 8192 ? 2.0f * static_cast<float>(value - 8192) / 8191.0f
                                  : 2.0f * static_cast<float>(value - 8192) / 8192.0f;
        if (channel1to16 - 1 == activeChannel_) targetBend_ = state.bend;
    }

    void render(float* left, float* right, int count) noexcept {
        if (count <= 0) return;
        for (int frame = 0; frame < count; ++frame) {
            morph_ += parameterStep_ * (parameters_.morph - morph_);
            output_ += parameterStep_ * (targetOutput_ - output_);
            expression_ += controlStep_ * (targetExpression_ - expression_);
            modulation_ += controlStep_ * (targetModulation_ - modulation_);
            bend_ += pitchStep_ * (targetBend_ - bend_);
            velocity_ += velocityStep_ * (targetVelocity_ - velocity_);
            if (activeNote_ >= 0) envelope_ += attackStep_ * (1.0f - envelope_);
            else envelope_ *= releaseMultiplier_;
            if (activeNote_ < 0 && envelope_ < 1.0e-5f) {
                envelope_ = 0.0f;
                if (left) left[frame] = 0.0f;
                if (right) right[frame] = 0.0f;
                peak_ *= peakMultiplier_;
                continue;
            }

            frequency_ += static_cast<double>(glideStep_) * (targetFrequency_ - frequency_);
            registerNote_ += pitchStep_ * (static_cast<float>(activeNote_ >= 0 ? activeNote_ : lastNote_) - registerNote_);
            vibratoPhase_ += 5.25 / sampleRate_;
            wrapPhase(vibratoPhase_);
            const float vibrato = .18f * modulation_ * sine(vibratoPhase_);
            const double frequency = frequency_ * bendRatio(bend_ + vibrato);
            const double increment = frequency / sampleRate_;
            const double detune = 1.0032 + .00025 * ((activeRoundRobin_ & 1u) ? 1.0 : -1.0);
            const double detunedIncrement = increment * detune;
            if (--filterCountdown_ <= 0) {
                updateFilters(frequency);
                filterCountdown_ = 16;
            }

            const float position = std::clamp(morph_, 0.0f, 1.0f) * 4.0f;
            const int first = std::min(3, static_cast<int>(position));
            const float blend = position - static_cast<float>(first);
            const float gainA = sine(.25 + .25 * static_cast<double>(blend));
            const float gainB = sine(.25 * static_cast<double>(blend));
            float value = 0.0f;
            if (gainA > 1.0e-6f) value += gainA * renderLayer(first, frequency, increment, detunedIncrement);
            if (gainB > 1.0e-6f) value += gainB * renderLayer(first + 1, frequency, increment, detunedIncrement);

            // Inaudible sample regions keep their timeline, so morphing a held
            // synth note into Reed/Brass reveals a sustain rather than a new attack.
            for (auto& voice : sampleVoices_) advanceVoice(voice, frequency);
            phase_ += increment;
            phaseDetuned_ += detunedIncrement;
            wrapPhase(phase_);
            wrapPhase(phaseDetuned_);
            transient_ *= transientMultiplier_;

            const float antiAliasFundamental = smoothUnit(static_cast<float>((sampleRate_ * .49 - frequency) / (sampleRate_ * .04)));
            value *= envelope_ * velocity_ * expression_ * output_ * antiAliasFundamental;
            value = protectPeak(value);
            if (left) left[frame] = value;
            if (right) right[frame] = value; // Deliberately mono and centre-stable.
            peak_ = std::max(std::abs(value), peak_ * peakMultiplier_);
        }
        if (peak_ < 1.0e-8f) peak_ = 0.0f;
    }

    int getCurrentNote() const noexcept { return activeNote_; }
    int getRoundRobinIndex() const noexcept { return displayedRoundRobin_; }
    float getPeak() const noexcept { return peak_; }
    bool isActive() const noexcept { return activeNote_ >= 0 || envelope_ >= 1.0e-5f; }

private:
    static constexpr double twoPi_ = 6.283185307179586476925286766559;
    static constexpr int tableSize_ = 2048;
    static constexpr int bendTableSize_ = 1024;
    static constexpr int velocityBuckets_ = 16;
    static constexpr int maximumRR_ = 8;
    static constexpr std::array<int, 6> digitalHarmonics_ {{32, 16, 8, 4, 2, 1}};

    struct HeldNote {
        std::uint16_t count = 0;
        bool sustained = false;
        std::uint64_t order = 0;
        float velocity = .7f;
    };
    struct Channel {
        bool pedal = false;
        float expression = 1.0f;
        float modulation = 0.0f;
        float bend = 0.0f;
    };
    struct Region {
        const Sample* sample = nullptr;
        double ratePerHz = 0.0;
        int loopStart = 0;
        int loopEnd = 0;
        int crossfade = 0;
        float gain = 1.0f;
    };
    struct Choice {
        std::array<int, maximumRR_> regions {};
        int count = 0;
    };
    struct Head {
        const Region* region = nullptr;
        double position = 0.0;
    };
    struct SampleVoice {
        Head current;
        Head previous;
        int fadeRemaining = 0;
    };
    struct Filter {
        float a1 = 1.0f, a2 = 0.0f, a3 = 0.0f;
        float ic1 = 0.0f, ic2 = 0.0f;
        void tune(double cutoff, double rate, float damping) noexcept {
            const float g = static_cast<float>(std::tan(.5 * twoPi_ * std::clamp(cutoff / rate, .0001, .22)));
            a1 = 1.0f / (1.0f + g * (g + damping));
            a2 = g * a1;
            a3 = g * a2;
        }
        float process(float input) noexcept {
            const float v3 = input - ic2;
            const float v1 = a1 * ic1 + a2 * v3;
            const float v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2.0f * v1 - ic1;
            ic2 = 2.0f * v2 - ic2;
            // The tiny guards prevent denormal work when morphing from silence.
            if (std::abs(ic1) < 1.0e-20f) ic1 = 0.0f;
            if (std::abs(ic2) < 1.0e-20f) ic2 = 0.0f;
            return v2;
        }
    };

    static float finiteClamp(float x, float lo, float hi, float fallback) noexcept {
        return std::isfinite(x) ? std::clamp(x, lo, hi) : fallback;
    }
    static bool validChannel(int channel) noexcept { return channel >= 1 && channel <= 16; }
    float coefficient(double seconds) const noexcept {
        return static_cast<float>(1.0 - std::exp(-1.0 / (seconds * sampleRate_)));
    }
    static float smoothUnit(float t) noexcept {
        t = std::clamp(t, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }
    static void wrapPhase(double& phase) noexcept {
        if (phase >= 1.0) phase -= std::floor(phase);
    }
    float sine(double phase) const noexcept {
        phase -= std::floor(phase);
        return lookup(sineTable_, phase);
    }
    template <std::size_t Size>
    static float lookup(const std::array<float, Size>& table, double phase) noexcept {
        const double scaled = phase * static_cast<double>(Size - 1);
        const int index = std::min(static_cast<int>(Size - 2), static_cast<int>(scaled));
        const float fraction = static_cast<float>(scaled - index);
        return table[static_cast<std::size_t>(index)]
             + fraction * (table[static_cast<std::size_t>(index + 1)] - table[static_cast<std::size_t>(index)]);
    }
    float bendRatio(float semitones) const noexcept {
        const double scaled = (static_cast<double>(std::clamp(semitones, -3.0f, 3.0f)) + 3.0) / 6.0;
        return lookup(bendTable_, scaled);
    }
    static bool validSample(const Sample& s) noexcept {
        const int instrument = static_cast<int>(s.instrument);
        if (instrument < 0 || instrument > 3 || s.rootNote < 0 || s.rootNote > 127
            || !std::isfinite(s.sampleRate) || s.sampleRate < 1000.0 || s.sampleRate > 768000.0
            || !std::isfinite(s.velocityCentre) || !std::isfinite(s.gain)
            || s.samples.size() < 16 || s.samples.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return false;
        bool audible = false;
        for (const float x : s.samples) {
            if (!std::isfinite(x)) return false;
            audible = audible || std::abs(x) > 1.0e-8f;
        }
        return audible;
    }
    void updateRegionRate(Region& region) noexcept {
        region.ratePerHz = region.sample->sampleRate
                         / (sampleRate_ * noteFrequencies_[static_cast<std::size_t>(region.sample->rootNote)]);
    }
    static std::size_t choiceOffset(int instrument, int note, int bucket) noexcept {
        return static_cast<std::size_t>((instrument * 128 + note) * velocityBuckets_ + bucket);
    }
    void buildChoice(int instrument, int note, int bucket) noexcept {
        auto& choice = choices_[choiceOffset(instrument, note, bucket)];
        const float velocity = (static_cast<float>(bucket) + .5f) / static_cast<float>(velocityBuckets_);
        int chosenRoot = -1;
        int rootDistance = 128;
        float chosenCentre = 0.0f;
        float centreDistance = 2.0f;
        for (const auto& region : regions_) {
            const auto& sample = *region.sample;
            if (static_cast<int>(sample.instrument) != instrument) continue;
            const int distance = std::abs(sample.rootNote - note);
            const float dynamicDistance = std::abs(sample.velocityCentre - velocity);
            if (distance < rootDistance || (distance == rootDistance && sample.rootNote < chosenRoot)) {
                chosenRoot = sample.rootNote;
                rootDistance = distance;
                centreDistance = 2.0f;
            }
            if (sample.rootNote == chosenRoot && dynamicDistance < centreDistance) {
                chosenCentre = sample.velocityCentre;
                centreDistance = dynamicDistance;
            }
        }
        if (chosenRoot < 0) return;
        for (std::size_t i = 0; i < regions_.size() && choice.count < maximumRR_; ++i) {
            const auto& sample = *regions_[i].sample;
            if (static_cast<int>(sample.instrument) == instrument && sample.rootNote == chosenRoot
                && std::abs(sample.velocityCentre - chosenCentre) < 1.0e-5f)
                choice.regions[static_cast<std::size_t>(choice.count++)] = static_cast<int>(i);
        }
        std::sort(choice.regions.begin(), choice.regions.begin() + choice.count,
                  [this](int a, int b) { return regions_[static_cast<std::size_t>(a)].sample->roundRobin
                                             < regions_[static_cast<std::size_t>(b)].sample->roundRobin; });
    }
    const Region* chooseRegion(int instrument, int note, float velocity) const noexcept {
        if (choices_.empty()) return nullptr;
        const int bucket = std::clamp(static_cast<int>(velocity * velocityBuckets_), 0, velocityBuckets_ - 1);
        const auto& choice = choices_[choiceOffset(instrument, note, bucket)];
        if (choice.count == 0) return nullptr;
        const auto index = choice.regions[static_cast<std::size_t>(activeRoundRobin_ % static_cast<unsigned>(choice.count))];
        return &regions_[static_cast<std::size_t>(index)];
    }
    void startNote(int channel, int note, float velocity, bool articulate) noexcept {
        const bool wasSilent = envelope_ < 1.0e-5f && activeNote_ < 0;
        if (articulate) {
            activeRoundRobin_ = nextRoundRobin_++;
            transient_ = 1.0f;
        }
        activeNote_ = lastNote_ = note;
        activeChannel_ = channel;
        targetFrequency_ = noteFrequencies_[static_cast<std::size_t>(note)];
        // Acoustic dynamics are in the recorded velocity layers; this mild
        // continuous law fills the gaps without erasing their natural contrast.
        targetVelocity_ = .45f + .55f * std::sqrt(velocity);
        if (wasSilent) {
            frequency_ = targetFrequency_;
            registerNote_ = static_cast<float>(note);
            velocity_ = targetVelocity_;
            phase_ = 0.0;
            phaseDetuned_ = .003;
            analogFilter_.ic1 = analogFilter_.ic2 = 0.0f;
            danceFilter_.ic1 = danceFilter_.ic2 = 0.0f;
        }
        for (int instrument = 0; instrument < 4; ++instrument) {
            const auto* region = chooseRegion(instrument, note, velocity);
            auto& voice = sampleVoices_[static_cast<std::size_t>(instrument)];
            if (!articulate && region == voice.current.region) continue;
            voice.previous = voice.current;
            voice.current = {region, 0.0};
            if (!articulate && region && region->loopEnd > region->loopStart) {
                double fraction = 0.0;
                const auto& old = voice.previous;
                if (old.region && old.region->loopEnd > old.region->loopStart && old.position >= old.region->loopStart)
                    fraction = std::fmod(old.position - old.region->loopStart,
                                         old.region->loopEnd - old.region->loopStart)
                             / (old.region->loopEnd - old.region->loopStart);
                voice.current.position = region->loopStart + fraction * (region->loopEnd - region->loopStart);
            }
            voice.fadeRemaining = wasSilent ? 0 : sampleFadeFrames_;
            if (wasSilent) voice.previous = {};
        }
        const int displayInstrument = parameters_.morph < .125f ? (note < 73 ? 0 : 1)
                                    : parameters_.morph < .375f ? (note < 63 ? 2 : 3) : -1;
        const auto* display = displayInstrument >= 0 ? sampleVoices_[static_cast<std::size_t>(displayInstrument)].current.region : nullptr;
        displayedRoundRobin_ = display ? display->sample->roundRobin : static_cast<int>(activeRoundRobin_ & 1u);
        updateControllerTargets();
        if (wasSilent) {
            // The channel's pre-note controllers are already established.
            // Smoothing from a previous channel/default here would cause an
            // audible burst at CC11=0 and a brief wrong-pitch note onset.
            expression_ = targetExpression_;
            bend_ = targetBend_;
            modulation_ = targetModulation_;
        }
        filterCountdown_ = 0;
    }
    void updateControllerTargets() noexcept {
        if (activeChannel_ < 0) return;
        const auto& state = channels_[static_cast<std::size_t>(activeChannel_)];
        targetExpression_ = state.expression;
        targetModulation_ = state.modulation;
        targetBend_ = state.bend;
    }
    void updatePriority() noexcept {
        int latestNote = -1, latestChannel = -1;
        std::uint64_t latestOrder = 0;
        for (int channel = 0; channel < 16; ++channel)
            for (int note = 0; note < 128; ++note) {
                const auto& held = notes_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(note)];
                if ((held.count > 0 || held.sustained) && held.order >= latestOrder) {
                    latestOrder = held.order;
                    latestNote = note;
                    latestChannel = channel;
                }
            }
        if (latestNote < 0) {
            activeNote_ = -1;
            // Keep activeChannel_ for controls applied to the release tail.
        } else if (latestNote != activeNote_ || latestChannel != activeChannel_) {
            const auto& held = notes_[static_cast<std::size_t>(latestChannel)][static_cast<std::size_t>(latestNote)];
            startNote(latestChannel, latestNote, held.velocity, false);
        }
    }
    void stopSound() noexcept {
        envelope_ = transient_ = peak_ = 0.0f;
        activeNote_ = -1;
        activeChannel_ = -1;
        sampleVoices_ = {};
        analogFilter_.ic1 = analogFilter_.ic2 = 0.0f;
        danceFilter_.ic1 = danceFilter_.ic2 = 0.0f;
    }

    static float readRaw(const Sample& sample, double position) noexcept {
        if (position < 0.0 || position >= static_cast<double>(sample.samples.size())) return 0.0f;
        const int i = static_cast<int>(position);
        const auto size = static_cast<int>(sample.samples.size());
        const float a = sample.samples[static_cast<std::size_t>(std::max(0, i - 1))];
        const float b = sample.samples[static_cast<std::size_t>(i)];
        const float c = sample.samples[static_cast<std::size_t>(std::min(size - 1, i + 1))];
        const float d = sample.samples[static_cast<std::size_t>(std::min(size - 1, i + 2))];
        const float t = static_cast<float>(position - i);
        return b + .5f * t * (c - a + t * (2.0f * a - 5.0f * b + 4.0f * c - d
                                       + t * (3.0f * (b - c) + d - a)));
    }
    static float readHead(const Head& head) noexcept {
        if (!head.region) return 0.0f;
        const auto& region = *head.region;
        float value = readRaw(*region.sample, head.position);
        if (region.crossfade > 0 && head.position >= region.loopEnd - region.crossfade && head.position < region.loopEnd) {
            const double offset = head.position - (region.loopEnd - region.crossfade);
            const float t = smoothUnit(static_cast<float>(offset / region.crossfade));
            const float preroll = readRaw(*region.sample, region.loopStart - region.crossfade + offset);
            value += t * (preroll - value);
        }
        return value * region.gain;
    }
    float readVoice(const SampleVoice& voice) const noexcept {
        const float current = readHead(voice.current);
        if (voice.fadeRemaining <= 0) return current;
        const float t = smoothUnit(1.0f - static_cast<float>(voice.fadeRemaining) / static_cast<float>(sampleFadeFrames_));
        return readHead(voice.previous) * (1.0f - t) + current * t;
    }
    static void advanceHead(Head& head, double frequency) noexcept {
        if (!head.region) return;
        const auto& region = *head.region;
        head.position += frequency * region.ratePerHz;
        if (region.loopEnd > region.loopStart && head.position >= region.loopEnd)
            head.position = region.loopStart + std::fmod(head.position - region.loopEnd, region.loopEnd - region.loopStart);
        else if (head.position >= static_cast<double>(region.sample->samples.size()))
            head.position = static_cast<double>(region.sample->samples.size());
    }
    void advanceVoice(SampleVoice& voice, double frequency) noexcept {
        advanceHead(voice.current, frequency);
        if (voice.fadeRemaining > 0) {
            advanceHead(voice.previous, frequency);
            if (--voice.fadeRemaining == 0) voice.previous = {};
        }
    }
    float acousticFamily(int firstInstrument, float lowNote, float highNote) const noexcept {
        const float blend = smoothUnit((registerNote_ - lowNote) / (highNote - lowNote));
        const auto& low = sampleVoices_[static_cast<std::size_t>(firstInstrument)];
        const auto& high = sampleVoices_[static_cast<std::size_t>(firstInstrument + 1)];
        if (!low.current.region) return readVoice(high);
        if (!high.current.region) return readVoice(low);
        float result = 0.0f;
        if (blend < 1.0f) result += sine(.25 + .25 * blend) * readVoice(low);
        if (blend > 0.0f) result += sine(.25 * blend) * readVoice(high);
        return result;
    }
    static float polyBlep(double phase, double increment) noexcept {
        const double dt = std::clamp(increment, 1.0e-8, .49);
        if (phase < dt) {
            const double x = phase / dt;
            return static_cast<float>(2.0 * x - x * x - 1.0);
        }
        if (phase > 1.0 - dt) {
            const double x = (phase - 1.0) / dt;
            return static_cast<float>(x * x + 2.0 * x + 1.0);
        }
        return 0.0f;
    }
    static float saw(double phase, double increment) noexcept {
        return static_cast<float>(1.0 - 2.0 * phase) + polyBlep(phase, increment);
    }
    static float pulse(double phase, double increment, double width) noexcept {
        double shifted = phase - width;
        if (shifted < 0.0) shifted += 1.0;
        return (phase < width ? 1.0f : -1.0f) + polyBlep(phase, increment) - polyBlep(shifted, increment)
             - static_cast<float>(2.0 * width - 1.0);
    }
    void updateFilters(double frequency) noexcept {
        const double key = std::min(frequency, 2500.0);
        analogFilter_.tune((800.0 + key * 2.4) * (.65 + .55 * velocity_) + transient_ * 2300.0, sampleRate_, 1.45f);
        danceFilter_.tune(1600.0 + key * 3.0 + transient_ * 4400.0 + velocity_ * 1200.0, sampleRate_, 1.20f);
    }
    float renderLayer(int layer, double frequency, double increment, double detunedIncrement) noexcept {
        if (layer == 0) return acousticFamily(0, 67.0f, 79.0f);
        if (layer == 1) return acousticFamily(2, 58.0f, 67.0f);
        const float fundamental = sine(phase_);
        if (layer == 2) {
            const float wave = .60f * saw(phase_, increment) + .15f * pulse(phase_, increment, .5) + .25f * fundamental;
            return .245f * analogFilter_.process(wave);
        }
        if (layer == 3) {
            const float octaveGain = smoothUnit(static_cast<float>((sampleRate_ * .46 - 2.0 * frequency) / (sampleRate_ * .06)));
            const float wave = .40f * saw(phase_, increment) + .28f * saw(phaseDetuned_, detunedIncrement)
                             + .18f * pulse(phase_, increment, .43) + .14f * fundamental
                             + .08f * transient_ * octaveGain * sine(2.0 * phase_);
            // Mild bounded drive; no hard clipping or large loudness jump.
            const float filtered = danceFilter_.process(wave);
            return .285f * filtered / (1.0f + .12f * std::abs(filtered));
        }
        const float fm = digital(frequency);
        const float octaveGain = smoothUnit(static_cast<float>((sampleRate_ * .46 - 2.0 * frequency) / (sampleRate_ * .06)));
        return .205f * fm + .022f * transient_ * octaveGain * sine(2.0 * phase_);
    }

    static double besselJ(int n, double x) noexcept {
        double term = 1.0;
        for (int i = 1; i <= n; ++i) term *= (x * .5) / i;
        double sum = term;
        for (int k = 1; k < 24; ++k) {
            term *= -(x * x * .25) / (static_cast<double>(k) * (n + k));
            sum += term;
            if (std::abs(term) < 1.0e-15) break;
        }
        return sum;
    }
    void buildDigitalTables() noexcept {
        // Exact finite FM sidebands for sin(theta + 1.15 sin(3 theta)),
        // mixed with a stable fundamental. Mip levels remove sidebands before
        // they cross Nyquist. This avoids running an alias-prone bright FM
        // oscillator directly at high MIDI pitches.
        std::array<double, 33> harmonics {};
        harmonics[1] = .35;
        for (int k = -8; k <= 8; ++k) {
            double amplitude = besselJ(std::abs(k), 1.15);
            if (k < 0 && ((-k) & 1)) amplitude = -amplitude;
            const int h = 1 + 3 * k;
            harmonics[static_cast<std::size_t>(std::abs(h))] += .65 * amplitude * (h < 0 ? -1.0 : 1.0);
        }
        for (std::size_t level = 0; level < digitalTables_.size(); ++level) {
            for (int i = 0; i < tableSize_; ++i) {
                double value = 0.0;
                const double theta = twoPi_ * i / tableSize_;
                for (int h = 1; h <= digitalHarmonics_[level]; ++h)
                    value += harmonics[static_cast<std::size_t>(h)] * std::sin(theta * h);
                digitalTables_[level][static_cast<std::size_t>(i)] = static_cast<float>(value);
            }
            digitalTables_[level][tableSize_] = digitalTables_[level][0];
        }
    }
    float digital(double frequency) const noexcept {
        for (std::size_t level = 0; level + 1 < digitalTables_.size(); ++level) {
            const double boundary = sampleRate_ * .46 / digitalHarmonics_[level];
            if (frequency <= boundary) {
                const float blend = smoothUnit(static_cast<float>(2.0 * frequency / boundary - 1.0));
                const float a = lookup(digitalTables_[level], phase_);
                return a + blend * (lookup(digitalTables_[level + 1], phase_) - a);
            }
        }
        return lookup(digitalTables_.back(), phase_);
    }
    static float protectPeak(float value) noexcept {
        if (!std::isfinite(value)) return 0.0f;
        const float absolute = std::abs(value);
        if (absolute <= .80f) return value;
        const float excess = absolute - .80f;
        const float limited = .80f + .18f * excess / (.18f + excess);
        return std::copysign(limited, value);
    }

    std::shared_ptr<const Bank> bank_;
    std::vector<Region> regions_;
    std::vector<Choice> choices_;
    std::array<std::array<HeldNote, 128>, 16> notes_ {};
    std::array<Channel, 16> channels_ {};
    std::array<SampleVoice, 4> sampleVoices_ {};
    std::array<double, 128> noteFrequencies_ {};
    std::array<float, tableSize_ + 1> sineTable_ {};
    std::array<float, bendTableSize_ + 1> bendTable_ {};
    std::array<std::array<float, tableSize_ + 1>, 6> digitalTables_ {};
    Parameters parameters_;
    Filter analogFilter_, danceFilter_;
    double sampleRate_ = 48000.0;
    double frequency_ = 440.0, targetFrequency_ = 440.0;
    double phase_ = 0.0, phaseDetuned_ = 0.0, vibratoPhase_ = 0.0;
    std::uint64_t order_ = 0;
    std::uint32_t nextRoundRobin_ = 0, activeRoundRobin_ = 0;
    int activeNote_ = -1, lastNote_ = 69, activeChannel_ = -1;
    int displayedRoundRobin_ = 0, filterCountdown_ = 0, sampleFadeFrames_ = 384;
    float morph_ = 0.0f, output_ = 1.0f, targetOutput_ = 1.0f;
    float expression_ = 1.0f, targetExpression_ = 1.0f;
    float modulation_ = 0.0f, targetModulation_ = 0.0f;
    float bend_ = 0.0f, targetBend_ = 0.0f;
    float velocity_ = 1.0f, targetVelocity_ = 1.0f;
    float registerNote_ = 69.0f;
    float envelope_ = 0.0f, transient_ = 0.0f, peak_ = 0.0f;
    float parameterStep_ = 0.0f, controlStep_ = 0.0f, pitchStep_ = 0.0f;
    float velocityStep_ = 0.0f, glideStep_ = 0.0f, attackStep_ = 0.0f;
    float releaseMultiplier_ = 0.0f, transientMultiplier_ = 0.0f, peakMultiplier_ = 0.0f;
};

} // namespace sculptlead
