#pragma once
#include "PluginProcessor.h"

namespace bjui
{
juce::Colour laneColour (int lane);

class WaveformView : public juce::Component
{
public:
    explicit WaveformView (BeatJackerProcessor& p) : proc (p) {}
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    std::function<void()> onRegionChanged;

private:
    void moveRegionTo (float x);
    BeatJackerProcessor& proc;
};

class LaneRow : public juce::Component
{
public:
    LaneRow (BeatJackerProcessor&, int lane);
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void refresh();
    juce::Rectangle<int> gridArea() const;
    std::function<void()> onChanged;

private:
    BeatJackerProcessor& proc;
    const int lane;
    juce::Slider sens;
    bool dragStarted = false;
};

class DragAllTile : public juce::Component
{
public:
    explicit DragAllTile (BeatJackerProcessor& p) : proc (p) { setMouseCursor (juce::MouseCursor::DraggingHandCursor); }
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override { dragStarted = false; }
    void mouseDrag (const juce::MouseEvent&) override;

private:
    BeatJackerProcessor& proc;
    bool dragStarted = false;
};
}

class BeatJackerEditor : public juce::AudioProcessorEditor,
                         public juce::FileDragAndDropTarget,
                         private juce::ChangeListener,
                         private juce::Timer
{
public:
    explicit BeatJackerEditor (BeatJackerProcessor&);
    ~BeatJackerEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int, int) override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void syncFromProcessor();
    void settingsEdited();
    void chooseFile();

    BeatJackerProcessor& proc;
    juce::LookAndFeel_V4 lnf;
    juce::TextButton loadButton { "LOAD SONG" }, playButton { "PLAY" }, drumsButton { "Jacked" }, sourceButton { "Original" }, tempoButton { "Tempo x1" };
    juce::ComboBox barsBox, gridBox;
    juce::Slider quantSlider;
    juce::Label barsLabel, gridLabel, quantLabel, hearLabel;
    bjui::WaveformView waveform;
    juce::OwnedArray<bjui::LaneRow> rows;
    bjui::DragAllTile dragAll;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::TooltipWindow tooltips { this, 700 };
    double lastPlayhead = -1.0;
    bool wasAnalysing = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BeatJackerEditor)
};
