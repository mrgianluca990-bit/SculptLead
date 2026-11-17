#include "PluginEditor.h"
#include <cmath>

namespace
{
const juce::Colour warm  { 0xffe8935a };
const juce::Colour amber { 0xfff0c060 };
const juce::Colour cool  { 0xff5fc8c0 };
const juce::Colour cream { 0xfff2ead2 };
constexpr float kA0 = juce::degreesToRadians (-135.0f);
constexpr float kA1 = juce::degreesToRadians ( 135.0f);
}

void SculptLeadLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h,
                                            float position, float angleStart, float angleEnd, juce::Slider&)
{
    auto bounds = juce::Rectangle<float> (static_cast<float> (x), static_cast<float> (y),
                                         static_cast<float> (w), static_cast<float> (h)).reduced (3.0f);
    const auto centre = bounds.getCentre();
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const float knobRadius = radius * 0.78f;

    juce::Path arc;
    arc.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, angleStart, angleEnd, true);
    g.setGradientFill (juce::ColourGradient (warm, bounds.getX(), centre.y,
                                           cool, bounds.getRight(), centre.y, false));
    g.strokePath (arc, juce::PathStrokeType (radius > 60.0f ? 2.5f : 1.5f,
                                            juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    g.setColour (juce::Colour (0xff121212));
    g.fillEllipse (centre.x - knobRadius, centre.y - knobRadius, knobRadius * 2.0f, knobRadius * 2.0f);
    g.setColour (juce::Colour (0xff2c2c2c));
    g.drawEllipse (centre.x - knobRadius, centre.y - knobRadius, knobRadius * 2.0f, knobRadius * 2.0f, 2.0f);

    juce::Path tick;
    tick.addRoundedRectangle (-1.5f, -knobRadius * 0.88f, 3.0f, knobRadius * 0.28f, 1.5f);
    g.setColour (cream);
    g.fillPath (tick, juce::AffineTransform::rotation (angleStart + position * (angleEnd - angleStart))
                         .translated (centre.x, centre.y));
}

SculptLeadEditor::SculptLeadEditor (SculptLeadProcessor& leadProcessor)
    : AudioProcessorEditor (&leadProcessor), proc (leadProcessor),
      morphAtt (leadProcessor.apvts, "morph", morph), volumeAtt (leadProcessor.apvts, "output", volume)
{
    for (auto* slider : { &morph, &volume })
    {
        slider->setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider->setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider->setRotaryParameters (kA0, kA1, true);
        slider->setLookAndFeel (&laf);
        slider->setWantsKeyboardFocus (true);
        addAndMakeVisible (*slider);
    }

    morph.setName ("Morph");
    morph.setTitle ("Reed, Brass, Analog, Dance, Digital");
    morph.setDoubleClickReturnValue (true, 0.0);
    morph.setTextValueSuffix (" %");
    morph.setTooltip (juce::String::fromUTF8 (
        "REED 0%: clarinetto nei bassi, flauto negli acuti, con sovrapposizione. "
        "BRASS 25%: trombone nei bassi, tromba negli acuti. ANALOG 50%, DANCE 75%, DIGITAL 100%. "
        "Sempre MONO: priorità all'ultima nota; note sovrapposte in legato. "
        "Glide regolabile dall'host. Doppio clic: REED."));
    if (! proc.areSamplesReady())
        morph.setTooltip (proc.getSampleMessage());

    volume.setName ("Output");
    volume.setDoubleClickReturnValue (true, 0.0);
    volume.setTextValueSuffix (" dB");
    volume.setTooltip ("Volume di uscita. Doppio clic: 0 dB.");
    volume.setPopupDisplayEnabled (true, false, this);

    setName ("Sculpt Lead");
    setDescription (juce::String::fromUTF8 (
        "Strumento monofonico: priorità all'ultima nota, con ritorno alla precedente ancora premuta."));
    setOpaque (true);
    setSize (768, 512);
    startTimerHz (30);
}

SculptLeadEditor::~SculptLeadEditor()
{
    stopTimer();
    morph.setLookAndFeel (nullptr);
    volume.setLookAndFeel (nullptr);
}

void SculptLeadEditor::resized()
{
    // Preserve the established Sculpt Drums/Bass interface geometry.
    morph.setBounds (184, 50, 400, 400);
    volume.setBounds (48, 392, 84, 84);
}

void SculptLeadEditor::timerCallback()
{
    const float db = juce::Decibels::gainToDecibels (proc.meterLevel.load (std::memory_order_relaxed), -60.0f);
    const float target = juce::jmap (juce::jlimit (-40.0f, 3.0f, db), -40.0f, 3.0f, 0.0f, 1.0f);
    needle += (target - needle) * 0.35f;
    repaint();
}

void SculptLeadEditor::drawVU (juce::Graphics& g, juce::Rectangle<float> bounds)
{
    g.setColour (juce::Colour (0xff1a1a1a));
    g.fillRoundedRectangle (bounds.expanded (4.0f), 8.0f);
    g.setColour (juce::Colour (0xfff0e6c8));
    g.fillRoundedRectangle (bounds, 5.0f);

    const juce::Point<float> pivot (bounds.getCentreX(), bounds.getBottom() - 4.0f);
    const float length = bounds.getHeight() * 0.78f;
    const float span = juce::degreesToRadians (48.0f);

    g.setColour (juce::Colour (0xff3a3328));
    for (int i = 0; i <= 10; ++i)
    {
        const float angle = -span + 2.0f * span * static_cast<float> (i) / 10.0f;
        const auto direction = juce::Point<float> (std::sin (angle), -std::cos (angle));
        g.drawLine (juce::Line<float> (pivot + direction * (length * 0.86f), pivot + direction * length),
                    i >= 8 ? 2.0f : 1.0f);
    }
    g.setFont (juce::Font (juce::FontOptions (13.0f)));
    g.drawText ("VU", bounds.withTrimmedTop (bounds.getHeight() * 0.45f), juce::Justification::centred);

    const float angle = -span + 2.0f * span * needle;
    g.setColour (juce::Colours::black);
    g.drawLine (juce::Line<float> (pivot, pivot + juce::Point<float> (std::sin (angle), -std::cos (angle))
                                                    * length * 0.95f), 1.6f);
    g.fillEllipse (pivot.x - 5.0f, pivot.y - 5.0f, 10.0f, 10.0f);
}

void SculptLeadEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff0d0d0d));
    g.setColour (cream);
    g.setFont (juce::Font (juce::FontOptions (15.0f)));
    g.drawText ("S C U L P T   L E A D", getLocalBounds().removeFromTop (44), juce::Justification::centred);

    g.setColour (cream.withAlpha (0.5f));
    g.setFont (juce::Font (juce::FontOptions (10.0f)));
    g.drawText ("MONO", 682, 13, 58, 19, juce::Justification::centredRight);

    g.setColour (proc.areSamplesReady() ? cream.withAlpha (0.5f) : warm);
    g.setFont (juce::Font (juce::FontOptions (11.0f)));
    g.drawText (proc.areSamplesReady() ? juce::String (morph.getValue(), 1) + " %" : proc.getSampleMessage(),
                20, 35, getWidth() - 40, 14, juce::Justification::centred);

    const int count = juce::jlimit (1, 8, proc.getRoundRobinCount());
    const int step = proc.roundRobinStep.load (std::memory_order_relaxed);
    for (int i = 0; i < count; ++i)
    {
        const float x = 384.0f + (static_cast<float> (i) - static_cast<float> (count - 1) * 0.5f) * 22.0f;
        const bool active = i == step;
        const float radius = active ? 4.0f : 2.5f;
        g.setColour (cream.withAlpha (active ? 1.0f : 0.35f));
        g.fillEllipse (x - radius, 462.0f - radius, radius * 2.0f, radius * 2.0f);
    }

    const float segment = static_cast<float> (morph.getValue()) * 0.04f;
    const juce::Colour colours[5] { warm, amber, juce::Colour (0xffc5cf88), juce::Colour (0xff83c9a8), cool };
    const char* labels[5] { "REED", "BRASS", "ANALOG", "DANCE", "DIGITAL" };
    for (int i = 0; i < 5; ++i)
    {
        const float weight = juce::jmax (0.0f, 1.0f - std::abs (segment - static_cast<float> (i)));
        g.setColour (colours[i].withAlpha (0.25f + 0.75f * weight));
        g.fillRoundedRectangle (219.0f + static_cast<float> (i) * 68.0f, 486.0f, 58.0f, 2.5f, 1.2f);
        g.setColour (colours[i].withAlpha (0.48f + 0.52f * weight));
        g.setFont (juce::Font (juce::FontOptions (9.5f)));
        g.drawText (labels[i], 214 + i * 68, 492, 68, 13, juce::Justification::centred);
    }

    drawVU (g, { 590.0f, 408.0f, 150.0f, 80.0f });
}
