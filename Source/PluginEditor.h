#pragma once

#include "PluginProcessor.h"

struct SculptLeadLookAndFeel : public juce::LookAndFeel_V4
{
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float position,
                           float angleStart, float angleEnd, juce::Slider&) override;
};

class SculptLeadEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit SculptLeadEditor (SculptLeadProcessor&);
    ~SculptLeadEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void drawVU (juce::Graphics&, juce::Rectangle<float>);

    SculptLeadProcessor& proc;
    SculptLeadLookAndFeel laf;
    juce::Slider morph, volume;
    juce::AudioProcessorValueTreeState::SliderAttachment morphAtt, volumeAtt;
    juce::TooltipWindow tooltip { this, 700 };
    float needle = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SculptLeadEditor)
};
