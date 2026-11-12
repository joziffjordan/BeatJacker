#include "PluginEditor.h"

namespace
{
const juce::Colour kBg (0xff121317), kPanel (0xff1b1d24), kPanelLight (0xff262933), kLine (0xff343846),
                   kText (0xffe9e9ee), kDim (0xff8b8fa3), kAccent (0xffffd23f);
const int kLabelW = 236;

juce::Font font (float size, bool bold = false)
{
    return juce::Font (juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain));
}

void startFileDrag (const juce::File& file, juce::Component* from)
{
    if (file.existsAsFile())
        juce::DragAndDropContainer::performExternalDragDropOfFiles ({ file.getFullPathName() }, false, from);
}
}

namespace bjui
{
juce::Colour laneColour (int lane)
{
    static const juce::Colour c[bj::NumLanes] = { juce::Colour (0xffff5a5f), juce::Colour (0xffff9f1c), juce::Colour (0xfff25fa7),
                                                  juce::Colour (0xff2ec4b6), juce::Colour (0xff4ea8de), juce::Colour (0xffb388eb) };
    return c[juce::jlimit (0, (int) bj::NumLanes - 1, lane)];
}

//==============================================================================
void WaveformView::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat();
    g.setColour (kPanel);
    g.fillRoundedRectangle (r, 6.0f);

    const auto a = proc.getAnalysis();
    if (proc.isAnalysing())
    {
        const float p = proc.getProgress();
        auto bar = r.reduced (40.0f, r.getHeight() * 0.5f - 5.0f);
        g.setColour (kPanelLight);
        g.fillRoundedRectangle (bar, 5.0f);
        g.setColour (kAccent);
        g.fillRoundedRectangle (bar.withWidth (juce::jmax (10.0f, bar.getWidth() * p)), 5.0f);
        g.setColour (kText);
        g.setFont (font (14.0f, true));
        g.drawText ("Jacking the beat...  " + juce::String (juce::roundToInt (p * 100.0f)) + "%", r.withBottom (bar.getY() - 4.0f), juce::Justification::centredBottom);
        return;
    }
    if (a == nullptr)
    {
        g.setColour (proc.getError().isNotEmpty() ? juce::Colour (0xffff7a7a) : kDim);
        g.setFont (font (15.0f));
        g.drawText (proc.getError().isNotEmpty() ? proc.getError() : juce::String ("Drop a song here, or hit LOAD SONG"), r, juce::Justification::centred);
        return;
    }

    const float w = r.getWidth(), mid = r.getCentreY(), h = r.getHeight() * 0.42f;
    const float x0 = (float) (proc.regionStartSeconds() / a->duration) * w;
    const float x1 = (float) ((proc.regionStartSeconds() + proc.regionLengthSeconds()) / a->duration) * w;

    g.setColour (kAccent.withAlpha (0.16f));
    g.fillRect (juce::Rectangle<float> (x0, r.getY(), juce::jmax (2.0f, x1 - x0), r.getHeight()));

    const auto& ov = a->overview;
    for (int x = 0; x < (int) w; ++x)
    {
        const size_t i0 = (size_t) ((double) x / w * (double) ov.size());
        const size_t i1 = juce::jmax (i0 + 1, (size_t) ((double) (x + 1) / w * (double) ov.size()));
        float pk = 0.0f;
        for (size_t i = i0; i < i1 && i < ov.size(); ++i) pk = juce::jmax (pk, ov[i]);
        const bool inside = (float) x >= x0 && (float) x <= x1;
        g.setColour (inside ? kAccent : kDim.withAlpha (0.7f));
        g.fillRect ((float) x, mid - pk * h, 1.0f, juce::jmax (1.0f, pk * h * 2.0f));
    }
    g.setColour (kAccent);
    g.drawRect (juce::Rectangle<float> (x0, r.getY() + 1.0f, juce::jmax (2.0f, x1 - x0), r.getHeight() - 2.0f), 1.5f);

    g.setColour (kDim);
    g.setFont (font (11.0f));
    g.drawText ("Click or drag to choose which bars to jack", r.reduced (8.0f, 3.0f), juce::Justification::topRight);
}

void WaveformView::moveRegionTo (float x)
{
    const auto a = proc.getAnalysis();
    if (a == nullptr || proc.isAnalysing()) return;
    // centre the region on the mouse, snapped to whole bars from the first beat
    const double t = (double) x / getWidth() * a->duration - proc.regionLengthSeconds() * 0.5;
    const double beat = (t - a->beatOrigin) * a->bpm / 60.0;
    const double first = std::fmod (bj::suggestRegionStart (*a, proc.settings.thresholds, 4), 4.0);
    const double maxStart = juce::jmax (0.0, std::floor (a->numBeats()) - 1.0);
    const double snapped = juce::jlimit (0.0, maxStart, std::round ((beat - first) / 4.0) * 4.0 + first);
    if (! juce::approximatelyEqual (snapped, proc.settings.startBeat))
    {
        proc.settings.startBeat = snapped;
        if (onRegionChanged) onRegionChanged();
    }
}

void WaveformView::mouseDown (const juce::MouseEvent& e) { moveRegionTo ((float) e.x); }
void WaveformView::mouseDrag (const juce::MouseEvent& e) { moveRegionTo ((float) e.x); }

//==============================================================================
LaneRow::LaneRow (BeatJackerProcessor& p, int l) : proc (p), lane (l)
{
    sens.setSliderStyle (juce::Slider::LinearHorizontal);
    sens.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    sens.setRange (0.0, 1.0, 0.01);
    sens.setColour (juce::Slider::thumbColourId, laneColour (lane));
    sens.setColour (juce::Slider::trackColourId, laneColour (lane).withAlpha (0.6f));
    sens.setColour (juce::Slider::backgroundColourId, kLine);
    sens.setTooltip ("Sensitivity: right picks up quieter hits, left keeps only the strong ones");
    sens.onValueChange = [this]
    {
        proc.settings.thresholds[(size_t) lane] = (float) (1.05 - sens.getValue());
        if (onChanged) onChanged();
    };
    addAndMakeVisible (sens);
    setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    refresh();
}

void LaneRow::refresh()
{
    sens.setValue (1.05 - proc.settings.thresholds[(size_t) lane], juce::dontSendNotification);
    repaint();
}

juce::Rectangle<int> LaneRow::gridArea() const { return getLocalBounds().withTrimmedLeft (kLabelW).reduced (0, 3); }

void LaneRow::resized()
{
    sens.setBounds (126, getHeight() / 2 - 10, kLabelW - 138, 20);
}

void LaneRow::paint (juce::Graphics& g)
{
    const auto col = laneColour (lane);
    auto label = getLocalBounds().withWidth (kLabelW - 8).reduced (0, 3).toFloat();
    g.setColour (kPanel);
    g.fillRoundedRectangle (label, 6.0f);
    g.setColour (col);
    g.fillRoundedRectangle (label.withWidth (5.0f), 2.5f);

    // grip dots
    g.setColour (kDim);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 2; ++j)
            g.fillEllipse (14.0f + (float) j * 6.0f, label.getCentreY() - 8.0f + (float) i * 6.0f, 3.0f, 3.0f);

    const auto notes = proc.notesFor (lane);
    g.setColour (kText);
    g.setFont (font (14.0f, true));
    g.drawText (bj::laneName (lane), 32, (int) label.getY() + 6, 92, 18, juce::Justification::centredLeft);
    g.setColour (kDim);
    g.setFont (font (11.0f));
    g.drawText (juce::String ((int) notes.size()) + " hits", 32, (int) label.getBottom() - 22, 92, 16, juce::Justification::centredLeft);

    const auto grid = gridArea().toFloat();
    g.setColour (kPanel);
    g.fillRoundedRectangle (grid, 6.0f);
    if (proc.getAnalysis() == nullptr) return;

    const auto rs = proc.regionSettings();
    const float beatW = grid.getWidth() / (float) rs.numBeats;
    for (int b = 0; b < rs.numBeats; ++b)
    {
        const float x = grid.getX() + (float) b * beatW;
        if (b % 4 == 0 && (b / 4) % 2 == 1)
        {
            g.setColour (kPanelLight.withAlpha (0.45f));
            g.fillRect (x, grid.getY(), beatW * 4.0f, grid.getHeight());
        }
        if (b > 0 && (beatW > 6.0f || b % 4 == 0))
        {
            g.setColour (b % 4 == 0 ? kLine.brighter (0.25f) : kLine.withAlpha (0.6f));
            g.fillRect (x, grid.getY(), 1.0f, grid.getHeight());
        }
    }

    const float cell = beatW / (float) rs.gridPerBeat;
    const float nw = juce::jlimit (3.0f, 14.0f, cell * 0.8f);
    for (auto& n : notes)
    {
        const float v = (float) n.velocity / 127.0f;
        const float h = (grid.getHeight() - 10.0f) * (0.3f + 0.7f * v);
        const float x = grid.getX() + (float) n.beat * beatW;
        g.setColour (col.withAlpha (0.45f + 0.55f * v));
        g.fillRoundedRectangle (x, grid.getBottom() - 5.0f - h, nw, h, 1.5f);
    }

    if (proc.isPlaying())
    {
        g.setColour (kText.withAlpha (0.85f));
        g.fillRect (grid.getX() + (float) proc.getPlayheadBeats() * beatW, grid.getY(), 1.5f, grid.getHeight());
    }
}

void LaneRow::mouseDown (const juce::MouseEvent&) { dragStarted = false; }

void LaneRow::mouseDrag (const juce::MouseEvent& e)
{
    if (dragStarted || e.getDistanceFromDragStart() < 6) return;
    dragStarted = true;
    startFileDrag (proc.writeLaneMidi (lane), this);
}

//==============================================================================
void DragAllTile::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat().reduced (1.0f);
    g.setColour (kAccent);
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (juce::Colours::black);
    g.setFont (font (13.0f, true));
    g.drawText ("DRAG ALL LANES", r, juce::Justification::centred);
}

void DragAllTile::mouseDrag (const juce::MouseEvent& e)
{
    if (dragStarted || e.getDistanceFromDragStart() < 6) return;
    dragStarted = true;
    startFileDrag (proc.writeAllMidi(), this);
}
}

//==============================================================================
BeatJackerEditor::BeatJackerEditor (BeatJackerProcessor& p)
    : AudioProcessorEditor (p), proc (p), waveform (p), dragAll (p)
{
    lnf.setColour (juce::TextButton::buttonColourId, kPanelLight);
    lnf.setColour (juce::TextButton::buttonOnColourId, kAccent);
    lnf.setColour (juce::TextButton::textColourOffId, kText);
    lnf.setColour (juce::TextButton::textColourOnId, juce::Colours::black);
    lnf.setColour (juce::ComboBox::backgroundColourId, kPanelLight);
    lnf.setColour (juce::ComboBox::outlineColourId, kLine);
    lnf.setColour (juce::ComboBox::textColourId, kText);
    lnf.setColour (juce::PopupMenu::backgroundColourId, kPanelLight);
    lnf.setColour (juce::PopupMenu::highlightedBackgroundColourId, kAccent);
    lnf.setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::black);
    lnf.setColour (juce::Slider::thumbColourId, kAccent);
    lnf.setColour (juce::Slider::trackColourId, kAccent.withAlpha (0.6f));
    lnf.setColour (juce::Slider::backgroundColourId, kLine);
    lnf.setColour (juce::Label::textColourId, kDim);
    setLookAndFeel (&lnf);

    addAndMakeVisible (waveform);
    waveform.onRegionChanged = [this] { settingsEdited(); };

    for (int i = 0; i < bj::NumLanes; ++i)
    {
        auto* row = rows.add (new bjui::LaneRow (proc, i));
        row->onChanged = [this] { settingsEdited(); };
        addAndMakeVisible (row);
    }

    loadButton.setColour (juce::TextButton::buttonColourId, kAccent);
    loadButton.setColour (juce::TextButton::textColourOffId, juce::Colours::black);
    loadButton.onClick = [this] { chooseFile(); };
    addAndMakeVisible (loadButton);

    playButton.setClickingTogglesState (true);
    playButton.onClick = [this]
    {
        proc.setPlaying (playButton.getToggleState());
        syncFromProcessor();
    };
    addAndMakeVisible (playButton);

    drumsButton.setClickingTogglesState (true);
    drumsButton.setTooltip ("Hear the jacked MIDI pattern with stand-in drum sounds");
    drumsButton.onClick = [this] { proc.settings.playDrums = drumsButton.getToggleState(); settingsEdited(); };
    addAndMakeVisible (drumsButton);

    sourceButton.setClickingTogglesState (true);
    sourceButton.setTooltip ("Hear the original song underneath, to check the pattern against it");
    sourceButton.onClick = [this] { proc.settings.playSource = sourceButton.getToggleState(); settingsEdited(); };
    addAndMakeVisible (sourceButton);

    tempoButton.setTooltip ("If the tempo was read at half or double speed, click to fix it");
    tempoButton.onClick = [this]
    {
        static const int next[] = { 1, 2, 0 };
        proc.settings.tempoScaleIndex = next[juce::jlimit (0, 2, proc.settings.tempoScaleIndex)];
        settingsEdited();
    };
    addAndMakeVisible (tempoButton);

    for (int b : { 1, 2, 4, 8, 16 })
        barsBox.addItem (juce::String (b) + (b == 1 ? " bar" : " bars"), b);
    barsBox.onChange = [this] { proc.settings.bars = barsBox.getSelectedId(); settingsEdited(); };
    addAndMakeVisible (barsBox);

    gridBox.addItemList ({ "1/8", "1/16", "1/16 triplet", "1/32" }, 1);
    gridBox.onChange = [this] { proc.settings.gridIndex = gridBox.getSelectedItemIndex(); settingsEdited(); };
    addAndMakeVisible (gridBox);

    quantSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    quantSlider.setRange (0.0, 100.0, 1.0);
    quantSlider.setTextValueSuffix ("%");
    quantSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 48, 20);
    quantSlider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    quantSlider.setColour (juce::Slider::textBoxTextColourId, kText);
    quantSlider.setTooltip ("0% keeps the song's own feel and swing, 100% snaps every hit to the grid");
    quantSlider.onValueChange = [this] { proc.settings.quantise = (float) quantSlider.getValue() / 100.0f; settingsEdited(); };
    addAndMakeVisible (quantSlider);

    for (auto* l : { &barsLabel, &gridLabel, &quantLabel, &hearLabel })
    {
        l->setFont (font (11.0f));
        l->setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (l);
    }
    barsLabel.setText ("LENGTH", juce::dontSendNotification);
    gridLabel.setText ("GRID", juce::dontSendNotification);
    quantLabel.setText ("QUANTISE", juce::dontSendNotification);
    hearLabel.setText ("PREVIEW", juce::dontSendNotification);

    addAndMakeVisible (dragAll);

    proc.addChangeListener (this);
    setSize (960, 620);
    syncFromProcessor();
    startTimerHz (30);
}

BeatJackerEditor::~BeatJackerEditor()
{
    proc.removeChangeListener (this);
    setLookAndFeel (nullptr);
}

void BeatJackerEditor::paint (juce::Graphics& g)
{
    g.fillAll (kBg);

    g.setColour (kAccent);
    g.setFont (font (24.0f, true));
    g.drawText ("BEAT JACKER", 16, 12, 200, 30, juce::Justification::centredLeft);

    const auto a = proc.getAnalysis();
    g.setFont (font (13.0f));
    g.setColour (kDim);
    const auto name = proc.getFile().existsAsFile() ? proc.getFile().getFileNameWithoutExtension() : juce::String ("No song loaded");
    g.drawText (name, 330, 12, getWidth() - 330 - 262, 30, juce::Justification::centredLeft, true);

    if (a != nullptr)
    {
        const float sw = proc.swing();
        g.setColour (kText);
        g.setFont (font (20.0f, true));
        g.drawText (juce::String (proc.effectiveBpm(), 1) + " BPM", getWidth() - 252, 8, 130, 24, juce::Justification::centredRight);
        g.setColour (kDim);
        g.setFont (font (11.0f));
        g.drawText (sw < 0.0f ? juce::String ("swing: n/a") : "swing " + juce::String (juce::roundToInt (sw)) + "%",
                    getWidth() - 252, 30, 130, 14, juce::Justification::centredRight);
    }

    g.setColour (kDim);
    g.setFont (font (11.0f));
    g.drawText ("Drag a lane into your DAW to drop its MIDI on a track", kLabelW + 16, getHeight() - 92, 400, 14, juce::Justification::centredLeft);
}

void BeatJackerEditor::resized()
{
    auto r = getLocalBounds().reduced (16, 0);
    auto top = r.removeFromTop (54);
    loadButton.setBounds (196, 14, 118, 28);
    tempoButton.setBounds (getWidth() - 16 - 92, 14, 92, 28);
    juce::ignoreUnused (top);

    waveform.setBounds (r.removeFromTop (84));
    r.removeFromTop (8);

    auto bottom = r.removeFromBottom (74);
    r.removeFromBottom (18);
    const int rowH = r.getHeight() / bj::NumLanes;
    for (auto* row : rows)
        row->setBounds (r.removeFromTop (rowH));

    bottom.removeFromTop (6);
    auto labels = bottom.removeFromTop (16);
    auto ctl = bottom.removeFromTop (32);
    auto place = [&] (juce::Label& l, juce::Component& c, int w, int gap = 14)
    {
        l.setBounds (labels.removeFromLeft (w));
        c.setBounds (ctl.removeFromLeft (w));
        labels.removeFromLeft (gap);
        ctl.removeFromLeft (gap);
    };
    hearLabel.setBounds (labels.removeFromLeft (232));
    playButton.setBounds (ctl.removeFromLeft (76));
    ctl.removeFromLeft (6);
    drumsButton.setBounds (ctl.removeFromLeft (72));
    ctl.removeFromLeft (6);
    sourceButton.setBounds (ctl.removeFromLeft (72));
    labels.removeFromLeft (18);
    ctl.removeFromLeft (18);
    place (barsLabel, barsBox, 96);
    place (gridLabel, gridBox, 116);
    place (quantLabel, quantSlider, 210, 18);
    dragAll.setBounds (ctl.removeFromRight (150));
}

void BeatJackerEditor::syncFromProcessor()
{
    const auto& s = proc.settings;
    const bool ready = proc.getAnalysis() != nullptr && ! proc.isAnalysing();
    barsBox.setSelectedId (s.bars, juce::dontSendNotification);
    gridBox.setSelectedItemIndex (s.gridIndex, juce::dontSendNotification);
    quantSlider.setValue (s.quantise * 100.0, juce::dontSendNotification);
    drumsButton.setToggleState (s.playDrums, juce::dontSendNotification);
    sourceButton.setToggleState (s.playSource, juce::dontSendNotification);
    static const char* tempoText[] = { "Tempo x0.5", "Tempo x1", "Tempo x2" };
    tempoButton.setButtonText (tempoText[juce::jlimit (0, 2, s.tempoScaleIndex)]);
    playButton.setToggleState (proc.isPlaying(), juce::dontSendNotification);
    playButton.setButtonText (proc.isPlaying() ? "STOP" : "PLAY");
    for (auto* c : std::initializer_list<juce::Component*> { &playButton, &drumsButton, &sourceButton, &tempoButton, &barsBox, &gridBox, &quantSlider, &dragAll })
        c->setEnabled (ready);
    loadButton.setEnabled (! proc.isAnalysing());
    for (auto* row : rows) row->refresh();
    waveform.repaint();
    repaint();
}

void BeatJackerEditor::settingsEdited()
{
    proc.settingsChanged();
    syncFromProcessor();
}

void BeatJackerEditor::changeListenerCallback (juce::ChangeBroadcaster*) { syncFromProcessor(); }

void BeatJackerEditor::timerCallback()
{
    const bool analysing = proc.isAnalysing();
    if (analysing || wasAnalysing) waveform.repaint();
    wasAnalysing = analysing;

    const double ph = proc.isPlaying() ? proc.getPlayheadBeats() : -1.0;
    if (! juce::approximatelyEqual (ph, lastPlayhead))
    {
        lastPlayhead = ph;
        for (auto* row : rows) row->repaint (row->gridArea());
    }
}

void BeatJackerEditor::chooseFile()
{
    chooser = std::make_unique<juce::FileChooser> ("Choose a song to jack", proc.getFile().existsAsFile() ? proc.getFile().getParentDirectory()
                                                                                                             : juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                   proc.getWildcard());
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto f = fc.getResult();
                              if (f.existsAsFile()) proc.loadFile (f);
                          });
}

bool BeatJackerEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    return files.size() == 1 && ! proc.isAnalysing()
        && juce::File (files[0]).hasFileExtension ("wav;aif;aiff;flac;mp3;ogg;m4a;aac;wma");
}

void BeatJackerEditor::filesDropped (const juce::StringArray& files, int, int)
{
    if (files.size() > 0) proc.loadFile (juce::File (files[0]));
}
