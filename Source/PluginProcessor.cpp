#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "LeadSampleData.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <utility>

namespace
{
constexpr const char* stateType = "SCULPT_LEAD_STATE";
constexpr int stateSchema = 1;
constexpr int maximumStateBytes = 64 * 1024;

struct SharedBankCache
{
    std::mutex mutex;
    std::weak_ptr<const sculptlead::Bank> bank;
    int roundRobinCount = 0;
};

SharedBankCache& sharedBankCache()
{
    static SharedBankCache cache;
    return cache;
}

juce::String basename (const juce::String& path)
{
    return path.replaceCharacter ('\\', '/').fromLastOccurrenceOf ("/", false, false);
}

const char* findBundledResource (const juce::String& fileName, int& size)
{
    const auto wanted = basename (fileName);
    for (int i = 0; i < LeadSamples::namedResourceListSize; ++i)
        if (wanted == basename (juce::String (LeadSamples::originalFilenames[i])))
            return LeadSamples::getNamedResource (LeadSamples::namedResourceList[i], size);

    size = 0;
    return nullptr;
}

bool numeric (const juce::var& value, double low, double high, double& result)
{
    if (! (value.isInt() || value.isInt64() || value.isDouble()))
        return false;

    result = static_cast<double> (value);
    return std::isfinite (result) && result >= low && result <= high;
}

bool integer (const juce::var& value, int low, int high, int& result)
{
    double number = 0.0;
    if (! numeric (value, static_cast<double> (low), static_cast<double> (high), number)
        || std::floor (number) < number)
        return false;

    result = static_cast<int> (number);
    return true;
}

bool readInstrument (const juce::var& name, sculptlead::Instrument& instrument)
{
    if (! name.isString())
        return false;

    const auto text = name.toString();
    if (text == "clarinet") instrument = sculptlead::Instrument::clarinet;
    else if (text == "flute") instrument = sculptlead::Instrument::flute;
    else if (text == "trombone") instrument = sculptlead::Instrument::trombone;
    else if (text == "trumpet") instrument = sculptlead::Instrument::trumpet;
    else return false;
    return true;
}

bool safeSamplePath (const juce::String& path)
{
    if (path.isEmpty() || path.length() > 240 || path.startsWithChar ('/')
        || path.containsChar ('\\') || path.containsChar (':') || ! path.endsWith (".wav"))
        return false;

    const auto parts = juce::StringArray::fromTokens (path, "/", "");
    for (const auto& part : parts)
        if (part.isEmpty() || part == "." || part == "..")
            return false;

    return path.containsOnly ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-/.");
}

// XML attributes arrive as strings. Validate their complete lexical form before
// using JUCE's locale-independent number parser ("25garbage" must not become 25).
bool stateNumber (const juce::var& value, double low, double high, double& result)
{
    if (! value.isString())
        return numeric (value, low, high, result);

    const auto text = value.toString().trim();
    if (text.isEmpty() || text.length() > 64)
        return false;

    int i = 0;
    const int length = text.length();
    const auto isDigit = [] (juce::juce_wchar c) { return c >= '0' && c <= '9'; };
    if (text[i] == '+' || text[i] == '-') ++i;
    bool digits = false;
    while (i < length && isDigit (text[i])) { digits = true; ++i; }
    if (i < length && text[i] == '.')
    {
        ++i;
        while (i < length && isDigit (text[i])) { digits = true; ++i; }
    }
    if (! digits) return false;
    if (i < length && (text[i] == 'e' || text[i] == 'E'))
    {
        ++i;
        if (i < length && (text[i] == '+' || text[i] == '-')) ++i;
        const int exponentStart = i;
        while (i < length && isDigit (text[i])) ++i;
        if (i == exponentStart) return false;
    }
    if (i != length) return false;

    result = text.getDoubleValue();
    return std::isfinite (result) && result >= low && result <= high;
}

float safeParameter (const std::atomic<float>* value, float low, float high, float fallback) noexcept
{
    const float loaded = value->load (std::memory_order_relaxed);
    return std::isfinite (loaded) ? juce::jlimit (low, high, loaded) : fallback;
}
}

juce::AudioProcessorValueTreeState::ParameterLayout SculptLeadProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout parameters;
    parameters.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "morph", 1 }, "Morph",
        NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f,
        AudioParameterFloatAttributes().withLabel ("%")));
    parameters.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "output", 1 }, "Output",
        NormalisableRange<float> (-60.0f, 6.0f, 0.1f), 0.0f,
        AudioParameterFloatAttributes().withLabel ("dB")));
    parameters.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "glide", 1 }, "Glide",
        NormalisableRange<float> (0.0f, 250.0f, 0.1f), 0.0f,
        AudioParameterFloatAttributes().withLabel ("ms")));
    return parameters;
}

SculptLeadProcessor::SculptLeadProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, stateType, createLayout())
{
    morphParameter = apvts.getRawParameterValue ("morph");
    outputParameter = apvts.getRawParameterValue ("output");
    glideParameter = apvts.getRawParameterValue ("glide");
    setLatencySamples (0);
    loadBundledSamples();
}

void SculptLeadProcessor::loadBundledSamples()
{
    // This lock and all decoding are constructor-only. Instances share the same
    // immutable decoded bank; the audio callback does not lock, allocate or read files.
    auto& cache = sharedBankCache();
    const std::lock_guard<std::mutex> lock (cache.mutex);
    if (auto bank = cache.bank.lock())
    {
        engine.setBank (std::move (bank));
        roundRobinCount = cache.roundRobinCount;
        samplesReady = true;
        return;
    }

    std::set<std::string> resourceNames;
    for (int i = 0; i < LeadSamples::namedResourceListSize; ++i)
        if (! resourceNames.insert (basename (juce::String (LeadSamples::originalFilenames[i])).toStdString()).second)
        {
            sampleMessage = "Nomi duplicati nella libreria campioni";
            return;
        }

    int manifestSize = 0;
    const auto* manifestData = findBundledResource ("manifest.json", manifestSize);
    if (manifestData == nullptr || manifestSize <= 0 || manifestSize > 2 * 1024 * 1024)
    {
        sampleMessage = "Libreria campioni mancante";
        return;
    }

    juce::var manifest;
    const auto parsed = juce::JSON::parse (juce::String::fromUTF8 (manifestData, manifestSize), manifest);
    const auto regions = manifest.getProperty ("regions", {});
    const auto* entries = regions.getArray();
    int schema = 0;
    if (parsed.failed() || ! manifest.isObject() || ! integer (manifest["schema"], 1, 1, schema)
        || entries == nullptr || entries->isEmpty() || entries->size() > 2048)
    {
        sampleMessage = "Manifest della libreria non valido";
        return;
    }

    juce::WavAudioFormat wav;
    auto bank = std::make_shared<sculptlead::Bank>();
    bank->samples.reserve (static_cast<std::size_t> (entries->size()));
    std::set<std::string> sampleNames;
    std::set<std::tuple<int, int, int, int>> mappings;
    std::array<bool, 4> instrumentsPresent {};
    std::size_t totalFrames = 0;
    int largestRoundRobin = 0;

    for (const auto& entry : *entries)
    {
        sculptlead::Sample sample;
        double velocityCentre = 0.0, expectedRate = 0.0, gainDb = 0.0;
        if (! entry.isObject() || ! readInstrument (entry["instrument"], sample.instrument)
            || ! integer (entry["rootNote"], 0, 127, sample.rootNote)
            || ! integer (entry["velocityLayer"], 0, 127, sample.velocityLayer)
            || ! numeric (entry["velocityCentre"], 0.0, 1.0, velocityCentre)
            || ! integer (entry["roundRobin"], 0, 7, sample.roundRobin)
            || ! numeric (entry["sampleRate"], 48000.0, 48000.0, expectedRate)
            || ! integer (entry["loopStart"], 0, 48000 * 12, sample.loopStart)
            || ! integer (entry["loopEnd"], 32, 48000 * 12, sample.loopEnd)
            || ! numeric (entry["gainDb"], -36.0, 12.0, gainDb)
            || ! entry["file"].isString())
        {
            sampleMessage = "Mappatura campioni non valida";
            return;
        }

        const auto filename = entry["file"].toString();
        const auto instrument = static_cast<int> (sample.instrument);
        if (! safeSamplePath (filename)
            || ! sampleNames.insert (basename (filename).toStdString()).second
            || ! mappings.insert ({ instrument, sample.rootNote, sample.velocityLayer, sample.roundRobin }).second)
        {
            sampleMessage = "Mappatura duplicata o percorso non valido: " + filename;
            return;
        }

        int dataSize = 0;
        const auto* data = findBundledResource (filename, dataSize);
        if (data == nullptr || dataSize <= 0)
        {
            sampleMessage = "Campione mancante: " + filename;
            return;
        }

        std::unique_ptr<juce::AudioFormatReader> reader (wav.createReaderFor (
            new juce::MemoryInputStream (data, static_cast<std::size_t> (dataSize), false), true));
        if (reader == nullptr || reader->lengthInSamples < 32
            || reader->lengthInSamples > 48000 * 12 || reader->numChannels != 1
            || ! std::isfinite (reader->sampleRate) || std::abs (reader->sampleRate - expectedRate) > 0.0
            || sample.loopEnd > reader->lengthInSamples || sample.loopEnd - sample.loopStart < 32)
        {
            sampleMessage = "Campione o loop non valido: " + filename;
            return;
        }

        const int length = static_cast<int> (reader->lengthInSamples);
        totalFrames += static_cast<std::size_t> (length);
        if (totalFrames > 64U * 1024U * 1024U)
        {
            sampleMessage = "Libreria campioni troppo grande";
            return;
        }

        juce::AudioBuffer<float> decoded (1, length);
        if (! reader->read (&decoded, 0, length, 0, true, false))
        {
            sampleMessage = "Lettura campione fallita: " + filename;
            return;
        }

        const auto* audio = decoded.getReadPointer (0);
        bool audible = false;
        for (int i = 0; i < length; ++i)
        {
            if (! std::isfinite (audio[i]))
            {
                sampleMessage = "Dati audio non validi: " + filename;
                return;
            }
            audible = audible || std::abs (audio[i]) > 1.0e-8f;
        }
        if (! audible)
        {
            sampleMessage = "Campione completamente silenzioso: " + filename;
            return;
        }

        sample.samples.assign (audio, audio + length);
        sample.sampleRate = expectedRate;
        sample.velocityCentre = static_cast<float> (velocityCentre);
        sample.gain = juce::Decibels::decibelsToGain (static_cast<float> (gainDb));
        instrumentsPresent[static_cast<std::size_t> (instrument)] = true;
        largestRoundRobin = std::max (largestRoundRobin, sample.roundRobin + 1);
        bank->samples.push_back (std::move (sample));
    }

    if (! std::all_of (instrumentsPresent.begin(), instrumentsPresent.end(), [] (bool present) { return present; }))
    {
        sampleMessage = "Libreria incompleta: servono tutti i quattro fiati";
        return;
    }

    cache.bank = bank;
    cache.roundRobinCount = largestRoundRobin;
    engine.setBank (std::move (bank));
    samplesReady = true;
    roundRobinCount = largestRoundRobin;
    sampleMessage.clear();
}

bool SculptLeadProcessor::isBusesLayoutSupported (const BusesLayout& layout) const
{
    return layout.getMainInputChannelSet().isDisabled()
        && layout.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

sculptlead::Parameters SculptLeadProcessor::readParameters() const noexcept
{
    sculptlead::Parameters parameters;
    parameters.morph = safeParameter (morphParameter, 0.0f, 100.0f, 0.0f) * 0.01f;
    parameters.outputDb = safeParameter (outputParameter, -60.0f, 6.0f, 0.0f);
    parameters.glideMs = safeParameter (glideParameter, 0.0f, 250.0f, 0.0f);
    return parameters;
}

void SculptLeadProcessor::clearIndicators() noexcept
{
    meterLevel.store (0.0f, std::memory_order_relaxed);
    roundRobinStep.store (-1, std::memory_order_relaxed);
    currentNote.store (-1, std::memory_order_relaxed);
}

void SculptLeadProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused (samplesPerBlock);
    engine.setParameters (readParameters());
    engine.prepare (sampleRate);
    wasBypassed = false;
    clearIndicators();
}

void SculptLeadProcessor::releaseResources()
{
    engine.reset();
    wasBypassed = false;
    clearIndicators();
}

void SculptLeadProcessor::handleShortMidi (const juce::uint8* bytes, int numBytes) noexcept
{
    // Parse only complete channel messages. SysEx is ignored without ever making
    // a MidiMessage, so a long SysEx cannot cause an audio-thread heap allocation.
    if (bytes == nullptr || numBytes != 3 || bytes[0] < 0x80 || bytes[0] >= 0xf0
        || bytes[1] > 127 || bytes[2] > 127)
        return;

    const int channel = (bytes[0] & 0x0f) + 1;
    switch (bytes[0] & 0xf0)
    {
        case 0x80: engine.noteOff (channel, bytes[1]); break;
        case 0x90:
            if (bytes[2] == 0) engine.noteOff (channel, bytes[1]);
            else engine.noteOn (channel, bytes[1], static_cast<float> (bytes[2]) / 127.0f);
            break;
        case 0xb0: engine.controller (channel, bytes[1], bytes[2]); break;
        case 0xe0: engine.pitchWheel (channel, bytes[1] | (bytes[2] << 7)); break;
        default: break;
    }
}

void SculptLeadProcessor::renderSegment (juce::AudioBuffer<float>& audio, int start, int length) noexcept
{
    if (length > 0)
        engine.render (audio.getWritePointer (0, start), audio.getWritePointer (1, start), length);
}

void SculptLeadProcessor::processBlock (juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    audio.clear();

    if (wasBypassed)
    {
        engine.reset();
        wasBypassed = false;
    }

    if (audio.getNumChannels() < 2 || ! samplesReady)
    {
        engine.reset();
        midi.clear();
        clearIndicators();
        return;
    }

    engine.setParameters (readParameters());
    const int blockLength = audio.getNumSamples();
    int cursor = 0;
    for (const auto metadata : midi)
    {
        const int position = juce::jlimit (cursor, blockLength, metadata.samplePosition);
        renderSegment (audio, cursor, position - cursor);
        handleShortMidi (metadata.data, metadata.numBytes);
        cursor = position;
    }
    renderSegment (audio, cursor, blockLength - cursor);
    midi.clear();

    meterLevel.store (blockLength > 0 ? audio.getMagnitude (0, blockLength) : 0.0f,
                      std::memory_order_relaxed);
    roundRobinStep.store (engine.getRoundRobinIndex(), std::memory_order_relaxed);
    currentNote.store (engine.getCurrentNote(), std::memory_order_relaxed);
}

void SculptLeadProcessor::processBlockBypassed (juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi)
{
    // An instrument has no dry signal. Bypass releases the mono note stack and
    // discards incoming notes; resuming requires a fresh key press, never a ghost note.
    if (! wasBypassed)
        engine.reset();
    wasBypassed = true;
    audio.clear();
    midi.clear();
    clearIndicators();
}

juce::AudioProcessorEditor* SculptLeadProcessor::createEditor()
{
    return new SculptLeadEditor (*this);
}

void SculptLeadProcessor::getStateInformation (juce::MemoryBlock& data)
{
    auto state = apvts.copyState();
    state.setProperty ("schemaVersion", stateSchema, nullptr);
    if (const auto xml = state.createXml())
        copyXmlToBinary (*xml, data);
}

void SculptLeadProcessor::setStateInformation (const void* data, int size)
{
    if (data == nullptr || size <= 8 || size > maximumStateBytes)
        return;

    const auto xml = getXmlFromBinary (data, size);
    if (xml == nullptr || ! xml->hasTagName (stateType))
        return;

    const auto restored = juce::ValueTree::fromXml (*xml);
    if (! restored.isValid() || restored.getNumChildren() != 3)
        return;

    double schema = 0.0;
    if (! stateNumber (restored.getProperty ("schemaVersion", stateSchema), stateSchema, stateSchema, schema))
        return;

    constexpr std::array<const char*, 3> ids { "morph", "output", "glide" };
    constexpr std::array<double, 3> minima { 0.0, -60.0, 0.0 };
    constexpr std::array<double, 3> maxima { 100.0, 6.0, 250.0 };
    std::array<double, 3> values {};
    std::array<bool, 3> seen {};

    for (const auto& child : restored)
    {
        if (! child.hasType ("PARAM") || child.getNumChildren() != 0)
            return;

        const auto id = child.getProperty ("id").toString();
        const auto it = std::find_if (ids.begin(), ids.end(), [&id] (const char* expected) { return id == expected; });
        if (it == ids.end())
            return;

        const auto index = static_cast<std::size_t> (std::distance (ids.begin(), it));
        if (seen[index] || ! stateNumber (child.getProperty ("value"), minima[index], maxima[index], values[index]))
            return;
        seen[index] = true;
    }

    if (! std::all_of (seen.begin(), seen.end(), [] (bool present) { return present; }))
        return;

    // Validate the complete snapshot before updating any live parameter. APVTS
    // publishes values through its atomics; the host/UI thread never touches Engine.
    juce::ValueTree clean (stateType);
    clean.setProperty ("schemaVersion", stateSchema, nullptr);
    for (std::size_t i = 0; i < ids.size(); ++i)
    {
        juce::ValueTree parameter ("PARAM");
        parameter.setProperty ("id", ids[i], nullptr);
        parameter.setProperty ("value", static_cast<float> (values[i]), nullptr);
        clean.addChild (parameter, -1, nullptr);
    }
    apvts.replaceState (clean);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SculptLeadProcessor();
}
