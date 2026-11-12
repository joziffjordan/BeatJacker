# Beat Jacker

Drop in a song. Beat Jacker pulls the drum pattern out of it and gives you one MIDI lane per
element (Kick, Snare, Clap, Closed Hat, Open Hat, Perc) to drag onto your own tracks and fill
with your own sounds.

VST3 + AU (Mac), VST3 (Windows), plus a standalone app. v0.1.

## Using it

1. Load it on a MIDI/instrument track and hit **LOAD SONG** (or drop a WAV, AIFF, FLAC or MP3 on it).
2. It finds the tempo and picks a busy 8-bar section. Click or drag on the waveform to move it; **LENGTH** sets 1-16 bars.
3. Each lane has a sensitivity slider: right picks up quieter hits, left keeps only the strong ones.
4. **PLAY** loops the section. **Jacked** plays the MIDI with stand-in drum sounds, **Original** plays the song underneath so you can check one against the other.
5. **QUANTISE**: 0% keeps the song's own timing and swing, 100% snaps to the **GRID**. Velocities follow how hard each hit was.
6. Drag a lane into your DAW to drop its MIDI clip on a track. **DRAG ALL LANES** gives one multi-track MIDI file.

Notes land on C1 kick, D1 snare, D#1 clap, F#1 closed hat, A#1 open hat, C#1 perc. Set your project
tempo to the BPM shown. If it read the tempo at half or double speed, click the **Tempo** button.

## What to expect

It works by signal processing (no AI stem model), so it is a strong first pass, not a perfect transcription:

- Kicks, claps/snares and hats on a clear drum mix come out well. Dense mixes need the sliders.
- A loud, punchy bass note can show up as a kick; pull the Kick slider left.
- A hat that lands exactly with a clap or snare is usually dropped.
- Snare vs clap is a best guess; the pattern is right even when the label is not.
- Vocals and synth stabs can leak into Perc, which is why its default is the least sensitive.
- Only the first 8 minutes of a file are analysed.

## Building

Needs CMake 3.22+ and a compiler (Xcode on Mac, Visual Studio 2022 on Windows). JUCE downloads itself.

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --config Release

Plugins end up in `build/BeatJacker_artefacts/Release/`. Copy them to:

- Mac: `~/Library/Audio/Plug-Ins/VST3/` and `~/Library/Audio/Plug-Ins/Components/`
- Windows: `C:\Program Files\Common Files\VST3\`

No compiler handy? Push this folder to a GitHub repo: the included workflow builds Mac and Windows
versions and attaches them to the run as downloads. Downloaded Mac builds are unsigned, so run
`xattr -cr "Beat Jacker.vst3"` (and the `.component`) once after copying.

`cmake --build build --target engine_test` builds a self-test that runs the analysis on a synthetic
house loop and reports hits found per lane.
