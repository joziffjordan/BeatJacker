// Beat Jacker analysis engine. Pure C++17, no JUCE dependency.
#pragma once
#include <array>
#include <cstddef>
#include <functional>
#include <vector>

namespace bj
{
enum Lane { Kick = 0, Snare, Clap, ClosedHat, OpenHat, Perc, NumLanes };

const char* laneName (int lane);
int laneMidiNote (int lane);

struct Hit
{
    double time = 0;      // seconds from start of file
    float strength = 0;   // onset strength, ~1.0 = a typical strong hit in this lane
    float velocity = 1;   // 0..1 loudness relative to the lane's loud hits
    float length = 0;     // seconds the sound rings (used for open hats)
};

struct Analysis
{
    bool valid = false;
    double sampleRate = 0;
    double duration = 0;      // seconds analysed
    double bpm = 120;
    double beatOrigin = 0;    // time (s) of the first beat on the grid
    std::array<std::vector<Hit>, NumLanes> lanes;
    std::vector<float> overview;   // waveform peaks, 0..1

    double beatTime (double beat) const { return beatOrigin + beat * 60.0 / bpm; }
    double numBeats() const { return (duration - beatOrigin) * bpm / 60.0; }
};

// progress(0..1) is called regularly; return false from it to cancel.
Analysis analyse (const float* mono, size_t numSamples, double sampleRate,
                  const std::function<bool (float)>& progress = {});

struct Note
{
    double beat = 0;         // position in beats from region start
    double lengthBeats = 0.25;
    int velocity = 100;      // 1..127
};

struct RegionSettings
{
    double startBeat = 0;     // region start, in beats from beatOrigin
    int numBeats = 32;
    int gridPerBeat = 4;      // 2 = 1/8, 4 = 1/16, 6 = 1/16T, 8 = 1/32
    float quantise = 0.6f;    // 0 = keep played timing, 1 = hard snap
    double tempoScale = 1.0;  // 0.5 / 1 / 2: reinterpret the detected tempo
};

std::vector<Note> regionNotes (const Analysis&, int lane, float threshold, const RegionSettings&);

// Measured 16th-note swing in the region: 50 = straight, 66 = triplet. -1 if unknown.
float measureSwing (const Analysis&, const std::array<float, NumLanes>& thresholds, const RegionSettings&);

// Picks a musically busy 8-bar (or shorter) region; returns start beat.
double suggestRegionStart (const Analysis&, const std::array<float, NumLanes>& thresholds, int numBeats);
}
