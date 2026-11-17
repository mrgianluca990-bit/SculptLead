#include "PluginProcessor.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace
{
constexpr double sampleRate = 48000.0;
constexpr int blockSize = 256;

struct Event
{
    int frame;
    juce::MidiMessage message;
};

void setParameter (SculptLeadProcessor& processor, const char* id, float value)
{
    auto* parameter = processor.apvts.getParameter (id);
    if (parameter == nullptr) throw std::runtime_error ("Missing parameter");
    parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
}

void addNote (std::vector<Event>& events, double start, double duration, int note, float velocity)
{
    events.push_back ({ static_cast<int> (start * sampleRate), juce::MidiMessage::noteOn (1, note, velocity) });
    events.push_back ({ static_cast<int> ((start + duration) * sampleRate), juce::MidiMessage::noteOff (1, note) });
}

std::vector<Event> phrase()
{
    std::vector<Event> events;
    // Separated attacks, overlapping legato, held-note return, and four registers.
    addNote (events, 0.20, 0.37, 62, 0.72f);
    addNote (events, 0.70, 0.37, 62, 0.72f);
    addNote (events, 1.20, 0.37, 62, 0.72f);
    addNote (events, 1.70, 0.37, 62, 0.72f);
    addNote (events, 2.30, 0.70, 65, 0.80f);
    addNote (events, 2.90, 0.70, 67, 0.80f);
    addNote (events, 3.50, 0.70, 69, 0.80f);
    addNote (events, 4.10, 0.70, 72, 0.80f);
    addNote (events, 5.00, 1.50, 67, 0.80f);
    addNote (events, 5.50, 0.50, 74, 0.80f);
    addNote (events, 6.80, 0.75, 50, 0.80f);
    addNote (events, 7.80, 0.75, 60, 0.80f);
    addNote (events, 8.80, 0.75, 72, 0.80f);
    addNote (events, 9.80, 0.75, 84, 0.80f);
    return events;
}

void render (SculptLeadProcessor& processor, const juce::File& file, float morph,
             double duration, std::vector<Event> events, bool sweep)
{
    setParameter (processor, "morph", morph);
    setParameter (processor, "output", 0.0f);
    setParameter (processor, "glide", 0.0f);
    processor.prepareToPlay (sampleRate, blockSize);
    std::stable_sort (events.begin(), events.end(), [] (const Event& a, const Event& b)
    { return a.frame < b.frame; });

    const int frames = static_cast<int> (duration * sampleRate);
    juce::AudioBuffer<float> result (2, frames);
    juce::AudioBuffer<float> block (2, blockSize);
    juce::MidiBuffer midi;
    midi.ensureSize (1024);
    std::size_t next = 0;
    double sumSquares = 0.0;
    float peak = 0.0f;
    for (int position = 0; position < frames; position += blockSize)
    {
        const int count = std::min (blockSize, frames - position);
        if (sweep)
            setParameter (processor, "morph", static_cast<float> (
                juce::jlimit (0.0, 1.0, (static_cast<double> (position) / sampleRate - 0.3) / (duration - 1.3)) * 100.0));
        midi.clear();
        while (next < events.size() && events[next].frame < position + count)
        {
            midi.addEvent (events[next].message, events[next].frame - position);
            ++next;
        }
        // The final partial block is represented as a non-owning view.
        juce::AudioBuffer<float> view (block.getArrayOfWritePointers(), 2, count);
        processor.processBlock (view, midi);
        for (int channel = 0; channel < 2; ++channel)
            result.copyFrom (channel, position, view, channel, 0, count);
        for (int sample = 0; sample < count; ++sample)
        {
            const float value = view.getSample (0, sample);
            if (! std::isfinite (value)) throw std::runtime_error ("Non-finite preview output");
            peak = std::max (peak, std::abs (value));
            sumSquares += static_cast<double> (value) * value;
        }
    }
    processor.releaseResources();
    auto stream = file.createOutputStream();
    if (! stream || ! stream->setPosition (0) || stream->truncate().failed())
        throw std::runtime_error ("Cannot create preview WAV");
    juce::WavAudioFormat format;
    std::unique_ptr<juce::AudioFormatWriter> writer (
        format.createWriterFor (stream.release(), sampleRate, 2, 24, {}, 0));
    if (! writer || ! writer->writeFromAudioSampleBuffer (result, 0, frames))
        throw std::runtime_error ("Cannot write preview WAV");
    writer.reset();
    const auto db = [] (double value) { return 20.0 * std::log10 (std::max (1.0e-12, value)); };
    std::cout << file.getFileName() << ",peak_dbfs=" << db (peak)
              << ",rms_dbfs=" << db (std::sqrt (sumSquares / frames)) << '\n';
}

void benchmark (SculptLeadProcessor& processor)
{
    // Measures the actual plug-in callback with the decoded bank already loaded.
    // This is elapsed process time on this machine, not a DAW or macOS CPU meter.
    constexpr int blocks = 4000;
    const double audioSeconds = blocks * blockSize / sampleRate;
    juce::AudioBuffer<float> block (2, blockSize);
    juce::MidiBuffer midi;
    midi.ensureSize (1024);
    for (const float morph : { 0.0f, 12.5f, 25.0f, 37.5f, 50.0f, 62.5f, 75.0f, 87.5f, 100.0f })
    {
        setParameter (processor, "morph", morph);
        setParameter (processor, "output", 0.0f);
        processor.prepareToPlay (sampleRate, blockSize);
        midi.addEvent (juce::MidiMessage::noteOn (1, 67, 0.85f), 0);
        processor.processBlock (block, midi);
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < blocks; ++i)
            processor.processBlock (block, midi);
        const double elapsed = std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count();
        std::cout << "benchmark,morph=" << morph << ",audio_seconds=" << audioSeconds
                  << ",elapsed_seconds=" << elapsed
                  << ",elapsed_realtime_percent=" << elapsed / audioSeconds * 100.0 << '\n';
        processor.releaseResources();
    }
}
}

int main (int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI initialiseJuce;
    try
    {
        if (argc != 2) throw std::runtime_error ("Usage: SculptLeadPreview /absolute/output/directory");
        const juce::File directory (juce::String::fromUTF8 (argv[1]));
        if (directory.createDirectory().failed()) throw std::runtime_error ("Cannot create preview directory");
        SculptLeadProcessor processor;
        if (! processor.areSamplesReady()) throw std::runtime_error (processor.getSampleMessage().toStdString());
        std::cout << std::fixed << std::setprecision (4);
        const std::array<const char*, 5> names { "01-Reed.wav", "02-Brass.wav", "03-Analog.wav", "04-Dance.wav", "05-Digital.wav" };
        for (std::size_t i = 0; i < names.size(); ++i)
            render (processor, directory.getChildFile (names[i]), static_cast<float> (i) * 25.0f, 11.3, phrase(), false);

        std::vector<Event> held;
        addNote (held, 0.2, 10.6, 67, 0.80f);
        render (processor, directory.getChildFile ("06-Morph-continuo.wav"), 0.0f, 11.5, held, true);

        std::vector<Event> dynamics;
        for (int i = 0; i < 8; ++i)
            addNote (dynamics, 0.2 + i * 0.75, 0.48, 67, 0.28f + static_cast<float> (i) * 0.09f);
        render (processor, directory.getChildFile ("07-Reed-dinamica-RR.wav"), 0.0f, 6.8, dynamics, false);
        render (processor, directory.getChildFile ("08-Brass-dinamica-RR.wav"), 25.0f, 6.8, dynamics, false);
        benchmark (processor);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Preview failed: " << error.what() << '\n';
        return 1;
    }
}
