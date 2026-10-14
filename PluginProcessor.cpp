#include "PluginProcessor.h"
#include "PluginEditor.h"

MajorMinorProcessor::MajorMinorProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "PARAMS", createLayout())
{
}

juce::AudioProcessorValueTreeState::ParameterLayout MajorMinorProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout l;

    l.add(std::make_unique<AudioParameterChoice>(
        ParameterID { "key", 1 }, "Key (tonic of sample)",
        StringArray { "Auto", "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }, 0));

    l.add(std::make_unique<AudioParameterChoice>(
        ParameterID { "convert", 1 }, "Convert",
        StringArray { "Auto (flip mode)", "Major to Minor", "Minor to Major" }, 0));

    l.add(std::make_unique<AudioParameterFloat>(
        ParameterID { "mix", 1 }, "Mix", NormalisableRange<float>(0.0f, 1.0f, 0.001f), 1.0f));

    l.add(std::make_unique<AudioParameterFloat>(
        ParameterID { "tune", 1 }, "Sample Tuning (cents)", NormalisableRange<float>(-50.0f, 50.0f, 0.1f), 0.0f));

    l.add(std::make_unique<AudioParameterFloat>(
        ParameterID { "maxhz", 1 }, "Max Frequency (Hz)", NormalisableRange<float>(500.0f, 5000.0f, 1.0f, 0.5f), 2500.0f));

    l.add(std::make_unique<AudioParameterFloat>(
        ParameterID { "thresh", 1 }, "Peak Threshold (dB)", NormalisableRange<float>(-70.0f, -20.0f, 0.1f), -45.0f));

    l.add(std::make_unique<AudioParameterBool>(
        ParameterID { "hold", 1 }, "Hold Detected Key", false));

    return l;
}

void MajorMinorProcessor::prepareToPlay(double sampleRate, int)
{
    shifter.prepare(sampleRate);
    setLatencySamples(ModeShifter::kLatency);
}

bool MajorMinorProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return out == layouts.getMainInputChannelSet();
}

void MajorMinorProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    for (int c = getTotalNumInputChannels(); c < getTotalNumOutputChannels(); ++c)
        buffer.clear(c, 0, buffer.getNumSamples());

    ModeShifter::Params p;
    p.key = (int) apvts.getRawParameterValue("key")->load() - 1;
    p.convert = (int) apvts.getRawParameterValue("convert")->load();
    p.mix = apvts.getRawParameterValue("mix")->load();
    p.tuneCents = apvts.getRawParameterValue("tune")->load();
    p.maxHz = apvts.getRawParameterValue("maxhz")->load();
    p.thresholdDb = apvts.getRawParameterValue("thresh")->load();

    p.holdKey = apvts.getRawParameterValue("hold")->load() > 0.5f;

    shifter.process(buffer.getArrayOfWritePointers(), buffer.getNumChannels(), buffer.getNumSamples(), p);
}

juce::String MajorMinorProcessor::getDetectedKeyText() const
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const int r = shifter.getDetectedRoot();
    if (r < 0) return "Detected key: --";
    const bool minor = shifter.isDetectedMinor();
    const int rel = minor ? (r + 3) % 12 : (r + 9) % 12; // relative major / minor
    const int conf = (int) std::lround(100.0f * juce::jlimit(0.0f, 1.0f, shifter.getDetectedConfidence()));
    return juce::String("Detected: ") + names[r] + (minor ? " minor" : " major")
         + " (relative " + names[rel] + (minor ? " major" : " minor") + ")  "
         + juce::String(conf) + "% match"; 
}

juce::AudioProcessorEditor* MajorMinorProcessor::createEditor()
{
    return new MajorMinorEditor(*this);
}

void MajorMinorProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary(*xml, destData);
}

void MajorMinorProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary(data, sizeInBytes))
        if (xml->hasTagName(apvts.state.getType()))
            apvts.replaceState(juce::ValueTree::fromXml(*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MajorMinorProcessor();
}
