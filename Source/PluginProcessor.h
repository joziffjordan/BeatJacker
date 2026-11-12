#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "Engine.h"

class BeatJackerProcessor : public juce::AudioProcessor,
                            public juce::ChangeBroadcaster,
                            private juce::Thread,
                            private juce::AsyncUpdater
{
public:
    struct Settings
    {
        std::array<float, bj::NumLanes> thresholds { 0.40f, 0.30f, 0.30f, 0.22f, 0.22f, 0.45f };
        double startBeat = 0.0;     // in detected beats from the first grid beat
        int bars = 8;
        int gridIndex = 1;          // 0 = 1/8, 1 = 1/16, 2 = 1/16T, 3 = 1/32
        float quantise = 0.6f;
        int tempoScaleIndex = 1;    // 0 = half, 1 = as detected, 2 = double
        bool playDrums = true;
        bool playSource = false;
    };

    BeatJackerProcessor();
    ~BeatJackerProcessor() override;

    // ---- message-thread API used by the editor
    void loadFile (const juce::File&, bool keepSavedRegion = false);
    bool isAnalysing() const { return analysing.load(); }
    float getProgress() const { return progress.load(); }
    juce::String getError() const { return errorText; }
    juce::File getFile() const { return currentFile; }
    juce::String getWildcard() { return formatManager.getWildcardForAllFormats(); }

    std::shared_ptr<const bj::Analysis> getAnalysis() const { return analysis; }
    Settings settings;
    void settingsChanged();                       // call after editing `settings`
    bj::RegionSettings regionSettings() const;
    double effectiveBpm() const;
    double regionStartSeconds() const;
    double regionLengthSeconds() const;
    std::vector<bj::Note> notesFor (int lane) const;
    float swing() const;

    juce::File writeLaneMidi (int lane) const;
    juce::File writeAllMidi() const;

    void setPlaying (bool);
    bool isPlaying() const { return playing.load(); }
    double getPlayheadBeats() const { return playheadBeats.load(); }

    // ---- AudioProcessor
    void prepareToPlay (double, int) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "Beat Jacker"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

private:
    struct Event { int sample; int lane; float gain; };
    struct Preview
    {
        std::vector<Event> events;
        int loopLength = 0;
        int numBeats = 0;
        std::shared_ptr<juce::AudioBuffer<float>> source;
        double sourceStart = 0.0, sourceRatio = 1.0;
        bool drums = true, playSource = false;
    };
    struct Voice { int lane = -1; int pos = 0; float gain = 0.0f; };

    void run() override;
    void handleAsyncUpdate() override;
    void rebuildPreview();
    void renderOneShots (double sampleRate);
    juce::MidiMessageSequence laneSequence (int lane, bool withTempo) const;

    juce::AudioFormatManager formatManager;
    juce::File currentFile, pendingFile;
    juce::String errorText;
    std::atomic<bool> analysing { false };
    std::atomic<float> progress { 0.0f };
    bool keepRegionOnLoad = false;

    std::shared_ptr<const bj::Analysis> analysis;
    std::shared_ptr<juce::AudioBuffer<float>> source;
    double sourceRate = 44100.0;

    juce::CriticalSection resultLock;
    std::shared_ptr<const bj::Analysis> resultAnalysis;
    std::shared_ptr<juce::AudioBuffer<float>> resultSource;
    double resultRate = 44100.0;
    juce::String resultError;
    juce::File resultFile;

    // audio thread
    juce::SpinLock previewLock;
    std::unique_ptr<Preview> preview;
    std::atomic<int> previewGeneration { 0 };
    int seenGeneration = -1;
    size_t nextEvent = 0;
    int playPos = 0;
    std::array<Voice, 24> voices;
    std::array<std::vector<float>, bj::NumLanes> oneShots;
    double hostRate = 44100.0;
    std::atomic<bool> playing { false };
    std::atomic<bool> restart { false };
    std::atomic<double> playheadBeats { 0.0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BeatJackerProcessor)
};
