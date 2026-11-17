#include "PluginProcessor.h"
#include <juce_graphics/juce_graphics.h>
#include <array>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace
{
void require (bool condition, const std::string& message)
{
    if (! condition)
        throw std::runtime_error (message);
}

void requireNear (float actual, float expected, const std::string& name)
{
    require (std::isfinite (actual) && std::abs (actual - expected) < 0.001f,
             name + ": expected " + std::to_string (expected) + ", got " + std::to_string (actual));
}

float parameter (SculptLeadProcessor& processor, const char* id)
{
    return processor.apvts.getRawParameterValue (id)->load();
}

void setParameter (SculptLeadProcessor& processor, const char* id, float value)
{
    auto* param = processor.apvts.getParameter (id);
    require (param != nullptr, std::string ("Missing parameter: ") + id);
    param->setValueNotifyingHost (param->convertTo0to1 (value));
}

juce::MemoryBlock stateWith (const juce::String& morph, const juce::String& output,
                             const juce::String& glide, const char* root = "SCULPT_LEAD_STATE",
                             int schema = 1)
{
    juce::XmlElement xml (root);
    xml.setAttribute ("schemaVersion", schema);
    const std::array<const char*, 3> ids { "morph", "output", "glide" };
    const std::array<juce::String, 3> values { morph, output, glide };
    for (std::size_t i = 0; i < ids.size(); ++i)
    {
        auto* child = xml.createNewChildElement ("PARAM");
        child->setAttribute ("id", ids[i]);
        child->setAttribute ("value", values[i]);
    }
    juce::MemoryBlock result;
    juce::AudioProcessor::copyXmlToBinary (xml, result);
    return result;
}

void restore (SculptLeadProcessor& processor, const juce::MemoryBlock& state)
{
    processor.setStateInformation (state.getData(), static_cast<int> (state.getSize()));
}

bool silent (const juce::AudioBuffer<float>& buffer, int start, int length)
{
    return length == 0 || buffer.getMagnitude (start, length) == 0.0f;
}

void requireFinite (const juce::AudioBuffer<float>& buffer)
{
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            require (std::isfinite (buffer.getSample (channel, sample)), "Non-finite plugin output");
}

void testState (SculptLeadProcessor& processor)
{
    requireNear (parameter (processor, "morph"), 0.0f, "Default morph");
    requireNear (parameter (processor, "output"), 0.0f, "Default output");
    requireNear (parameter (processor, "glide"), 0.0f, "Default glide");
    require (processor.getLatencySamples() == 0, "Instrument must report zero added latency");
    require (processor.getTotalNumInputChannels() == 0 && processor.getTotalNumOutputChannels() == 2,
             "Expected a stereo instrument with no audio input");

    setParameter (processor, "morph", 73.2f);
    setParameter (processor, "output", -7.6f);
    setParameter (processor, "glide", 41.3f);
    juce::MemoryBlock saved;
    processor.getStateInformation (saved);
    require (saved.getSize() > 8, "Plugin returned empty saved state");

    setParameter (processor, "morph", 0.0f);
    setParameter (processor, "output", 0.0f);
    setParameter (processor, "glide", 0.0f);
    restore (processor, saved);
    processor.prepareToPlay (48000.0, 256);
    requireNear (parameter (processor, "morph"), 73.2f, "Restored morph");
    requireNear (parameter (processor, "output"), -7.6f, "Restored output");
    requireNear (parameter (processor, "glide"), 41.3f, "Restored glide");

    // Every invalid snapshot must be rejected as a whole: an otherwise valid new
    // morph must not leak through when a later parameter is malformed.
    const std::array<juce::MemoryBlock, 10> invalidStates {
        stateWith ("25", "nan", "10"),
        stateWith ("25", "-inf", "10"),
        stateWith ("25", "1e999", "10"),
        stateWith ("25", "5garbage", "10"),
        stateWith ("101", "0", "10"),
        stateWith ("25", "6.1", "10"),
        stateWith ("25", "-60.1", "10"),
        stateWith ("25", "0", "251"),
        stateWith ("25", "0", "10", "SCULPT_BASS_STATE"),
        stateWith ("25", "0", "10", "SCULPT_LEAD_STATE", 2)
    };
    for (const auto& invalid : invalidStates)
    {
        restore (processor, invalid);
        requireNear (parameter (processor, "morph"), 73.2f, "Invalid state changed morph");
        requireNear (parameter (processor, "output"), -7.6f, "Invalid state changed output");
        requireNear (parameter (processor, "glide"), 41.3f, "Invalid state changed glide");
    }

    const std::array<unsigned char, 12> noise { 0x11, 0x17, 0x92, 0x31, 0xff, 0, 1, 2, 3, 4, 5, 6 };
    processor.setStateInformation (noise.data(), static_cast<int> (noise.size()));
    processor.setStateInformation (nullptr, 64);
    processor.setStateInformation (saved.getData(), 7);
    processor.setStateInformation (saved.getData(), static_cast<int> (saved.getSize() / 2));
    requireNear (parameter (processor, "morph"), 73.2f, "Corrupt state changed morph");
    requireNear (parameter (processor, "output"), -7.6f, "Corrupt state changed output");

    restore (processor, stateWith ("1e2", "-60", "250"));
    requireNear (parameter (processor, "morph"), 100.0f, "Boundary morph");
    requireNear (parameter (processor, "output"), -60.0f, "Boundary output");
    requireNear (parameter (processor, "glide"), 250.0f, "Boundary glide");
    processor.releaseResources();
    std::cout << "PASS: state round-trip, finite values, bounds, corrupt data, zero latency\n";
}

void testTimestampAndBypass (SculptLeadProcessor& processor)
{
    setParameter (processor, "morph", 50.0f);
    setParameter (processor, "output", 0.0f);
    setParameter (processor, "glide", 0.0f);
    processor.prepareToPlay (48000.0, 256);
    juce::AudioBuffer<float> audio (2, 256);
    juce::MidiBuffer midi;

    const std::array<juce::uint8, 256> sysex {};
    midi.addEvent (juce::MidiMessage::createSysExMessage (sysex.data(), static_cast<int> (sysex.size())), 12);
    midi.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 96);
    processor.processBlock (audio, midi);
    require (silent (audio, 0, 96), "MIDI note sounded before its exact sample timestamp");
    require (audio.getMagnitude (96, 160) > 0.000001f, "MIDI note did not sound after its timestamp");
    require (midi.isEmpty(), "Instrument passed MIDI through to the host");
    require (processor.currentNote.load() == 60, "Wrong current note after note-on");
    requireFinite (audio);

    midi.addEvent (juce::MidiMessage::controllerEvent (1, 120, 0), 128);
    processor.processBlock (audio, midi);
    require (audio.getMagnitude (0, 128) > 0.000001f, "Panic was applied before its timestamp");
    require (silent (audio, 128, 128), "All Sound Off did not stop at its timestamp");

    midi.addEvent (juce::MidiMessage::noteOn (1, 64, 0.8f), 0);
    processor.processBlock (audio, midi);
    require (processor.currentNote.load() == 64, "Note did not start before bypass");
    midi.addEvent (juce::MidiMessage::noteOff (1, 64), 32);
    midi.addEvent (juce::MidiMessage::noteOn (2, 72, 0.8f), 64);
    processor.processBlockBypassed (audio, midi);
    require (silent (audio, 0, audio.getNumSamples()), "Bypassed instrument produced audio");
    require (midi.isEmpty() && processor.currentNote.load() == -1, "Bypass left a stale note or MIDI event");
    processor.processBlockBypassed (audio, midi);
    processor.processBlock (audio, midi);
    require (silent (audio, 0, audio.getNumSamples()), "Re-enabling bypass revived a stale note");

    midi.addEvent (juce::MidiMessage::noteOn (3, 67, 0.8f), 0);
    processor.processBlock (audio, midi);
    require (audio.getMagnitude (0, audio.getNumSamples()) > 0.000001f,
             "Fresh note after bypass did not sound");
    require (processor.currentNote.load() == 67, "Fresh note after bypass has wrong identity");

    midi.addEvent (juce::MidiMessage::noteOff (3, 67), 0);
    const int tailBlocks = static_cast<int> (std::ceil (processor.getTailLengthSeconds() * 48000.0 / 256.0)) + 1;
    for (int block = 0; block < tailBlocks; ++block)
        processor.processBlock (audio, midi);
    require (silent (audio, 0, audio.getNumSamples()), "Release exceeded the tail duration reported to the host");

    midi.addEvent (juce::MidiMessage::noteOn (3, 67, 0.8f), 0);
    processor.processBlock (audio, midi);
    processor.releaseResources();
    processor.prepareToPlay (48000.0, 256);
    processor.processBlock (audio, midi);
    require (silent (audio, 0, audio.getNumSamples()), "releaseResources left a stuck note");
    processor.releaseResources();
    std::cout << "PASS: MIDI sample offsets, SysEx, panic, bypass and restart\n";
}

void testFactoryRender (SculptLeadProcessor& processor)
{
    require (processor.areSamplesReady(), "Factory bank failed: " + processor.getSampleMessage().toStdString());
    require (processor.getRoundRobinCount() >= 2, "Factory bank does not expose round robin");
    juce::AudioBuffer<float> audio (2, 256);
    juce::MidiBuffer midi;
    for (float morph : { 0.0f, 25.0f, 50.0f, 75.0f, 100.0f })
    {
        setParameter (processor, "morph", morph);
        setParameter (processor, "output", 0.0f);
        setParameter (processor, "glide", 0.0f);
        for (int note : { 48, 72, 84 })
        {
            processor.prepareToPlay (48000.0, 256);
            midi.addEvent (juce::MidiMessage::noteOn (1, note, 0.85f), 0);
            float peak = 0.0f;
            for (int block = 0; block < 96; ++block)
            {
                processor.processBlock (audio, midi);
                requireFinite (audio);
                peak = std::max (peak, audio.getMagnitude (0, audio.getNumSamples()));
            }
            require (peak > 0.0001f, "Silent factory timbre at morph=" + std::to_string (morph)
                                      + ", note=" + std::to_string (note));
            processor.releaseResources();
        }
    }
    std::cout << "PASS: embedded factory bank, five morph anchors and three registers\n";
}

void saveScreenshot (SculptLeadProcessor& processor, const juce::String& path)
{
    setParameter (processor, "morph", 0.0f);
    setParameter (processor, "output", 0.0f);
    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
    require (editor != nullptr && editor->getWidth() == 768 && editor->getHeight() == 512,
             "Editor dimensions do not match the Sculpt family");
    const auto image = editor->createComponentSnapshot (editor->getLocalBounds());
    require (image.isValid(), "Could not capture plugin editor");
    const juce::File file (path);
    require (file.getParentDirectory().createDirectory().wasOk(), "Could not create screenshot directory");
    auto output = file.createOutputStream();
    require (output != nullptr, "Could not open screenshot file");
    output->setPosition (0);
    output->truncate();
    juce::PNGImageFormat png;
    require (png.writeImageToStream (image, *output), "Could not save screenshot PNG");
    output->flush();
    require (output->getStatus().wasOk(), "Screenshot write failed");
    std::cout << "Saved editor screenshot: " << path << '\n';
}
}

int main (int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI initialiseJuce;
    try
    {
        SculptLeadProcessor processor;
        require (processor.areSamplesReady(), "Factory bank failed: " + processor.getSampleMessage().toStdString());
        testState (processor);
        testTimestampAndBypass (processor);
        testFactoryRender (processor);
        if (argc == 3 && std::string (argv[1]) == "--screenshot")
            saveScreenshot (processor, juce::String::fromUTF8 (argv[2]));
        else
            require (argc == 1, "Usage: SculptLeadStateTests [--screenshot /absolute/path.png]");
        std::cout << "All Sculpt Lead plugin integration tests passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
