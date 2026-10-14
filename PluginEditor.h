#pragma once
#include "PluginProcessor.h"

class MajorMinorEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit MajorMinorEditor(MajorMinorProcessor& p)
        : juce::AudioProcessorEditor(p), proc(p), generic(p)
    {
        addAndMakeVisible(generic);
        addAndMakeVisible(label);
        label.setJustificationType(juce::Justification::centred);
        label.setFont(juce::FontOptions(15.0f));
        setSize(420, 380);
        startTimerHz(4);
        timerCallback();
    }

    void resized() override
    {
        auto r = getLocalBounds();
        label.setBounds(r.removeFromTop(36));
        generic.setBounds(r);
    }

private:
    void timerCallback() override
    {
        label.setText(proc.getDetectedKeyText(), juce::dontSendNotification);
    }

    MajorMinorProcessor& proc;
    juce::GenericAudioProcessorEditor generic;
    juce::Label label;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MajorMinorEditor)
};
