#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include "LeadEngine.h"

class SculptLeadProcessor final : public juce::AudioProcessor
{
public:
    SculptLeadProcessor();
    ~SculptLeadProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    using juce::AudioProcessor::processBlock;
    using juce::AudioProcessor::processBlockBypassed;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Sculpt Lead"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.5; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    bool areSamplesReady() const noexcept { return samplesReady; }
    const juce::String& getSampleMessage() const noexcept { return sampleMessage; }
    int getRoundRobinCount() const noexcept { return roundRobinCount; }

    juce::AudioProcessorValueTreeState apvts;
    std::atomic<float> meterLevel { 0.0f };
    std::atomic<int> roundRobinStep { -1 };
    std::atomic<int> currentNote { -1 };

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void loadBundledSamples();
    sculptlead::Parameters readParameters() const noexcept;
    void handleShortMidi (const juce::uint8*, int numBytes) noexcept;
    void renderSegment (juce::AudioBuffer<float>&, int start, int length) noexcept;
    void clearIndicators() noexcept;

    sculptlead::Engine engine;
    std::atomic<float>* morphParameter = nullptr;
    std::atomic<float>* outputParameter = nullptr;
    std::atomic<float>* glideParameter = nullptr;
    bool wasBypassed = false; // Only accessed by the audio thread or while suspended.

    // Set during construction; immutable before audio or editor access.
    bool samplesReady = false;
    int roundRobinCount = 0;
    juce::String sampleMessage;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SculptLeadProcessor)
};
