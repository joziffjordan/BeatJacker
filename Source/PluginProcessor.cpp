#include "PluginProcessor.h"
#include "PluginEditor.h"

static const int kGridPerBeat[] = { 2, 4, 6, 8 };
static const double kTempoScale[] = { 0.5, 1.0, 2.0 };

BeatJackerProcessor::BeatJackerProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      juce::Thread ("Beat Jacker analysis")
{
    formatManager.registerBasicFormats();
    renderOneShots (hostRate);

    const auto autoload = juce::SystemStats::getEnvironmentVariable ("BEATJACKER_AUTOLOAD", {});
    if (autoload.isNotEmpty())
        loadFile (juce::File (autoload));
}

BeatJackerProcessor::~BeatJackerProcessor()
{
    cancelPendingUpdate();
    stopThread (10000);
}

//==============================================================================
void BeatJackerProcessor::loadFile (const juce::File& file, bool keepSavedRegion)
{
    stopThread (10000);
    setPlaying (false);
    pendingFile = file;
    keepRegionOnLoad = keepSavedRegion;
    errorText.clear();
    progress = 0.0f;
    analysing = true;
    startThread();
    sendChangeMessage();
}

void BeatJackerProcessor::run()
{
    const auto file = pendingFile;
    juce::String error;
    std::shared_ptr<juce::AudioBuffer<float>> buffer;
    std::shared_ptr<const bj::Analysis> result;
    double rate = 44100.0;

    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));
    if (reader == nullptr)
    {
        error = "Couldn't read that file. Try WAV, AIFF, FLAC or MP3.";
    }
    else
    {
        rate = reader->sampleRate;
        const auto length = (int) juce::jmin<juce::int64> (reader->lengthInSamples, (juce::int64) (rate * 60.0 * 8.0));
        const int channels = juce::jlimit (1, 2, (int) reader->numChannels);
        buffer = std::make_shared<juce::AudioBuffer<float>> (channels, length);
        reader->read (buffer.get(), 0, length, 0, true, channels > 1);

        std::vector<float> mono ((size_t) length);
        for (int c = 0; c < channels; ++c)
        {
            const float* d = buffer->getReadPointer (c);
            for (int i = 0; i < length; ++i) mono[(size_t) i] += d[i] / (float) channels;
        }
        auto a = bj::analyse (mono.data(), mono.size(), rate, [this] (float p)
        {
            progress = p;
            return ! threadShouldExit();
        });
        if (threadShouldExit()) return;
        if (! a.valid) error = "Couldn't find a beat in that file (too short or silent?).";
        else result = std::make_shared<const bj::Analysis> (std::move (a));
    }

    {
        const juce::ScopedLock sl (resultLock);
        resultAnalysis = result;
        resultSource = result != nullptr ? buffer : nullptr;
        resultRate = rate;
        resultError = error;
        resultFile = file;
    }
    triggerAsyncUpdate();
}

void BeatJackerProcessor::handleAsyncUpdate()
{
    {
        const juce::ScopedLock sl (resultLock);
        analysis = resultAnalysis;
        source = resultSource;
        sourceRate = resultRate;
        errorText = resultError;
        currentFile = resultFile;
        resultAnalysis = nullptr;
        resultSource = nullptr;
    }
    analysing = false;
    if (analysis != nullptr)
    {
        if (! keepRegionOnLoad)
        {
            settings.tempoScaleIndex = 1;
            settings.startBeat = bj::suggestRegionStart (*analysis, settings.thresholds, settings.bars * 4);
        }
        const double maxStart = juce::jmax (0.0, std::floor (analysis->numBeats()) - 1.0);
        settings.startBeat = juce::jlimit (0.0, maxStart, settings.startBeat);
    }
    keepRegionOnLoad = false;
    rebuildPreview();
    sendChangeMessage();
}

//==============================================================================
double BeatJackerProcessor::effectiveBpm() const
{
    return analysis != nullptr ? analysis->bpm * kTempoScale[juce::jlimit (0, 2, settings.tempoScaleIndex)] : 120.0;
}

bj::RegionSettings BeatJackerProcessor::regionSettings() const
{
    bj::RegionSettings rs;
    rs.startBeat = settings.startBeat;
    rs.numBeats = settings.bars * 4;
    rs.gridPerBeat = kGridPerBeat[juce::jlimit (0, 3, settings.gridIndex)];
    rs.quantise = settings.quantise;
    rs.tempoScale = kTempoScale[juce::jlimit (0, 2, settings.tempoScaleIndex)];
    return rs;
}

double BeatJackerProcessor::regionStartSeconds() const
{
    return analysis != nullptr ? analysis->beatTime (settings.startBeat) : 0.0;
}

double BeatJackerProcessor::regionLengthSeconds() const
{
    return settings.bars * 4 * 60.0 / effectiveBpm();
}

std::vector<bj::Note> BeatJackerProcessor::notesFor (int lane) const
{
    if (analysis == nullptr) return {};
    return bj::regionNotes (*analysis, lane, settings.thresholds[(size_t) lane], regionSettings());
}

float BeatJackerProcessor::swing() const
{
    return analysis != nullptr ? bj::measureSwing (*analysis, settings.thresholds, regionSettings()) : -1.0f;
}

void BeatJackerProcessor::settingsChanged()
{
    rebuildPreview();
}

void BeatJackerProcessor::setPlaying (bool shouldPlay)
{
    if (shouldPlay) restart = true;
    playing = shouldPlay && analysis != nullptr;
}

void BeatJackerProcessor::rebuildPreview()
{
    std::unique_ptr<Preview> p;
    if (analysis != nullptr)
    {
        p = std::make_unique<Preview>();
        const double bpm = effectiveBpm();
        const double samplesPerBeat = hostRate * 60.0 / bpm;
        p->numBeats = settings.bars * 4;
        p->loopLength = juce::jmax (1, (int) std::lround (p->numBeats * samplesPerBeat));
        for (int lane = 0; lane < bj::NumLanes; ++lane)
            for (auto& n : notesFor (lane))
                p->events.push_back ({ juce::jlimit (0, p->loopLength - 1, (int) std::lround (n.beat * samplesPerBeat)),
                                       lane, 0.25f + 0.75f * (float) n.velocity / 127.0f });
        std::sort (p->events.begin(), p->events.end(), [] (const Event& a, const Event& b) { return a.sample < b.sample; });
        p->source = source;
        p->sourceStart = regionStartSeconds() * sourceRate;
        p->sourceRatio = sourceRate / hostRate;
        p->drums = settings.playDrums;
        p->playSource = settings.playSource;
    }
    else
    {
        playing = false;
    }
    {
        const juce::SpinLock::ScopedLockType sl (previewLock);
        std::swap (preview, p);
        ++previewGeneration;
    }
    // the old preview (now in p) is freed here, off the audio thread
}

//==============================================================================
namespace
{
struct Biquad
{
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    static Biquad make (bool highPass, double freq, double sr, double q)
    {
        const double w = juce::MathConstants<double>::twoPi * juce::jmin (freq, sr * 0.45) / sr;
        const double al = std::sin (w) / (2.0 * q), c = std::cos (w), a0 = 1.0 + al;
        Biquad f;
        if (highPass) { f.b0 = (float) ((1 + c) / 2 / a0); f.b1 = (float) (-(1 + c) / a0); f.b2 = f.b0; }
        else          { f.b0 = (float) (al / a0); f.b1 = 0; f.b2 = -f.b0; }   // band-pass
        f.a1 = (float) (-2 * c / a0);
        f.a2 = (float) ((1 - al) / a0);
        return f;
    }
    float run (float x)
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};
}

// Simple synthesised stand-in sounds so the jacked pattern can be auditioned.
void BeatJackerProcessor::renderOneShots (double sr)
{
    juce::Random rng (1234);
    for (int lane = 0; lane < bj::NumLanes; ++lane)
    {
        const double seconds = lane == bj::OpenHat ? 0.45 : (lane == bj::Kick ? 0.4 : 0.25);
        auto& out = oneShots[(size_t) lane];
        out.assign ((size_t) (seconds * sr), 0.0f);
        auto hp1 = Biquad::make (true, 7500.0, sr, 0.8), hp2 = hp1;
        auto bp = Biquad::make (false, lane == bj::Clap ? 1700.0 : 3200.0, sr, lane == bj::Clap ? 1.2 : 0.6);
        double phase = 0.0;
        for (size_t i = 0; i < out.size(); ++i)
        {
            const double t = (double) i / sr;
            const float noise = rng.nextFloat() * 2.0f - 1.0f;
            float v = 0.0f;
            switch (lane)
            {
                case bj::Kick:
                    phase += juce::MathConstants<double>::twoPi * (46.0 + 130.0 * std::exp (-t * 38.0)) / sr;
                    v = (float) (0.95 * std::sin (phase) * std::exp (-t * 11.0));
                    break;
                case bj::Snare:
                    v = (float) (0.5 * std::sin (juce::MathConstants<double>::twoPi * 185.0 * t) * std::exp (-t * 30.0))
                        + 1.4f * bp.run (noise) * (float) std::exp (-t * 20.0);
                    break;
                case bj::Clap:
                {
                    double env = 0.0;
                    for (double d : { 0.0, 0.012, 0.024 })
                        if (t >= d) env = juce::jmax (env, std::exp (-(t - d) * (d < 0.02 ? 160.0 : 24.0)));
                    v = 2.2f * bp.run (noise) * (float) env;
                    break;
                }
                case bj::ClosedHat: v = 1.6f * hp2.run (hp1.run (noise)) * (float) std::exp (-t * 85.0); break;
                case bj::OpenHat:   v = 1.4f * hp2.run (hp1.run (noise)) * (float) std::exp (-t * 10.0); break;
                default:
                    v = (float) (0.7 * std::sin (juce::MathConstants<double>::twoPi * 440.0 * t * (1.0 + 0.25 * std::exp (-t * 50.0))) * std::exp (-t * 28.0));
                    break;
            }
            const float fadeOut = juce::jmin (1.0f, (float) (out.size() - i) / (float) (0.005 * sr));
            out[i] = 0.42f * v * fadeOut;
        }
    }
}

void BeatJackerProcessor::prepareToPlay (double sampleRate, int)
{
    hostRate = sampleRate;
    renderOneShots (sampleRate);
    for (auto& v : voices) v.lane = -1;
    rebuildPreview();
}

bool BeatJackerProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

void BeatJackerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();
    midi.clear();

    const juce::SpinLock::ScopedTryLockType lock (previewLock);
    if (! lock.isLocked() || preview == nullptr || ! playing.load())
    {
        for (auto& v : voices) v.lane = -1;
        return;
    }
    const Preview& p = *preview;
    const int gen = previewGeneration.load();
    if (restart.exchange (false)) { playPos = 0; seenGeneration = -1; }
    if (gen != seenGeneration)
    {
        seenGeneration = gen;
        playPos %= p.loopLength;
        nextEvent = (size_t) (std::lower_bound (p.events.begin(), p.events.end(), playPos,
                                                [] (const Event& e, int s) { return e.sample < s; }) - p.events.begin());
    }

    const int numSamples = buffer.getNumSamples();
    const int numOut = buffer.getNumChannels();
    const juce::AudioBuffer<float>* src = (p.playSource && p.source != nullptr) ? p.source.get() : nullptr;

    for (int i = 0; i < numSamples; ++i)
    {
        if (p.drums)
        {
            while (nextEvent < p.events.size() && p.events[nextEvent].sample <= playPos)
            {
                const Event& e = p.events[nextEvent++];
                if (e.lane == bj::ClosedHat || e.lane == bj::OpenHat)
                    for (auto& v : voices)
                        if (v.lane == bj::OpenHat) v.lane = -1;   // choke
                Voice* slot = &voices[0];
                for (auto& v : voices)
                {
                    if (v.lane < 0) { slot = &v; break; }
                    if (v.pos > slot->pos) slot = &v;
                }
                *slot = { e.lane, 0, e.gain };
            }
            float mix = 0.0f;
            for (auto& v : voices)
            {
                if (v.lane < 0) continue;
                const auto& shot = oneShots[(size_t) v.lane];
                if (v.pos >= (int) shot.size()) { v.lane = -1; continue; }
                mix += shot[(size_t) v.pos++] * v.gain;
            }
            for (int c = 0; c < numOut; ++c) buffer.addSample (c, i, mix);
        }

        if (src != nullptr)
        {
            const double sp = p.sourceStart + playPos * p.sourceRatio;
            const int i0 = (int) sp;
            if (i0 >= 0 && i0 + 1 < src->getNumSamples())
            {
                const float f = (float) (sp - i0);
                for (int c = 0; c < numOut; ++c)
                {
                    const float* d = src->getReadPointer (juce::jmin (c, src->getNumChannels() - 1));
                    buffer.addSample (c, i, 0.5f * (d[i0] * (1.0f - f) + d[i0 + 1] * f));
                }
            }
        }

        if (++playPos >= p.loopLength) { playPos = 0; nextEvent = 0; }
    }
    for (int c = 0; c < numOut; ++c)
        juce::FloatVectorOperations::clip (buffer.getWritePointer (c), buffer.getReadPointer (c), -1.0f, 1.0f, numSamples);
    playheadBeats = (double) playPos / (double) p.loopLength * (double) p.numBeats;
}

//==============================================================================
juce::MidiMessageSequence BeatJackerProcessor::laneSequence (int lane, bool withTempo) const
{
    const int ppq = 960;
    juce::MidiMessageSequence seq;
    seq.addEvent (juce::MidiMessage::textMetaEvent (3, bj::laneName (lane)), 0.0);
    if (withTempo)
    {
        seq.addEvent (juce::MidiMessage::tempoMetaEvent ((int) std::lround (60.0e6 / effectiveBpm())), 0.0);
        seq.addEvent (juce::MidiMessage::timeSignatureMetaEvent (4, 4), 0.0);
    }
    const int note = bj::laneMidiNote (lane);
    const double end = settings.bars * 4 * ppq;
    for (auto& n : notesFor (lane))
    {
        const double on = std::round (n.beat * ppq);
        const double off = juce::jmin (end, on + juce::jmax (30.0, std::round (n.lengthBeats * ppq)));
        seq.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) n.velocity), on);
        seq.addEvent (juce::MidiMessage::noteOff (1, note), off);
    }
    seq.addEvent (juce::MidiMessage::endOfTrack(), end);   // keeps the clip a whole number of bars
    seq.updateMatchedPairs();
    return seq;
}

static juce::File midiTempFile (const juce::String& name)
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("BeatJacker");
    dir.createDirectory();
    return dir.getChildFile (name + ".mid");
}

juce::File BeatJackerProcessor::writeLaneMidi (int lane) const
{
    if (analysis == nullptr || notesFor (lane).empty()) return {};
    juce::MidiFile mf;
    mf.setTicksPerQuarterNote (960);
    mf.addTrack (laneSequence (lane, true));
    auto file = midiTempFile (bj::laneName (lane));
    file.deleteFile();
    juce::FileOutputStream out (file);
    if (! out.openedOk() || ! mf.writeTo (out, 0)) return {};
    return file;
}

juce::File BeatJackerProcessor::writeAllMidi() const
{
    if (analysis == nullptr) return {};
    juce::MidiFile mf;
    mf.setTicksPerQuarterNote (960);
    juce::MidiMessageSequence tempo;
    tempo.addEvent (juce::MidiMessage::tempoMetaEvent ((int) std::lround (60.0e6 / effectiveBpm())), 0.0);
    tempo.addEvent (juce::MidiMessage::timeSignatureMetaEvent (4, 4), 0.0);
    mf.addTrack (tempo);
    int used = 0;
    for (int lane = 0; lane < bj::NumLanes; ++lane)
        if (! notesFor (lane).empty()) { mf.addTrack (laneSequence (lane, false)); ++used; }
    if (used == 0) return {};
    auto file = midiTempFile ("Jacked Beat");
    file.deleteFile();
    juce::FileOutputStream out (file);
    if (! out.openedOk() || ! mf.writeTo (out, 1)) return {};
    return file;
}

//==============================================================================
void BeatJackerProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    juce::XmlElement xml ("BeatJacker");
    xml.setAttribute ("file", currentFile.getFullPathName());
    xml.setAttribute ("startBeat", settings.startBeat);
    xml.setAttribute ("bars", settings.bars);
    xml.setAttribute ("grid", settings.gridIndex);
    xml.setAttribute ("quantise", settings.quantise);
    xml.setAttribute ("tempoScale", settings.tempoScaleIndex);
    xml.setAttribute ("playDrums", settings.playDrums);
    xml.setAttribute ("playSource", settings.playSource);
    for (int i = 0; i < bj::NumLanes; ++i)
        xml.setAttribute ("thr" + juce::String (i), settings.thresholds[(size_t) i]);
    copyXmlToBinary (xml, dest);
}

void BeatJackerProcessor::setStateInformation (const void* data, int size)
{
    auto xml = getXmlFromBinary (data, size);
    if (xml == nullptr || ! xml->hasTagName ("BeatJacker")) return;
    Settings s;
    s.startBeat = xml->getDoubleAttribute ("startBeat", s.startBeat);
    s.bars = juce::jlimit (1, 16, xml->getIntAttribute ("bars", s.bars));
    s.gridIndex = juce::jlimit (0, 3, xml->getIntAttribute ("grid", s.gridIndex));
    s.quantise = juce::jlimit (0.0f, 1.0f, (float) xml->getDoubleAttribute ("quantise", s.quantise));
    s.tempoScaleIndex = juce::jlimit (0, 2, xml->getIntAttribute ("tempoScale", s.tempoScaleIndex));
    s.playDrums = xml->getBoolAttribute ("playDrums", s.playDrums);
    s.playSource = xml->getBoolAttribute ("playSource", s.playSource);
    for (int i = 0; i < bj::NumLanes; ++i)
        s.thresholds[(size_t) i] = juce::jlimit (0.05f, 1.05f, (float) xml->getDoubleAttribute ("thr" + juce::String (i), s.thresholds[(size_t) i]));
    settings = s;

    const juce::File file (xml->getStringAttribute ("file"));
    if (file.existsAsFile())
        loadFile (file, true);
    else
        sendChangeMessage();
}

juce::AudioProcessorEditor* BeatJackerProcessor::createEditor() { return new BeatJackerEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new BeatJackerProcessor(); }
