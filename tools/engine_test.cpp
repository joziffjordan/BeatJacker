// Synthesises a house loop with a known drum pattern over bass + chords,
// runs the engine on it and reports how many hits were found per lane.
#include "../Source/Engine.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

struct Biquad
{
    enum Type { HP, BP };
    float b0, b1, b2, a1, a2, z1 = 0, z2 = 0;
    Biquad (Type t, double f, double sr, double q = 0.8)
    {
        const double w = 2 * M_PI * f / sr, al = std::sin (w) / (2 * q), c = std::cos (w), a0 = 1 + al;
        if (t == HP) { b0 = (float) ((1 + c) / 2 / a0); b1 = (float) (-(1 + c) / a0); b2 = b0; }
        else { b0 = (float) (al / a0); b1 = 0; b2 = -b0; }
        a1 = (float) (-2 * c / a0); a2 = (float) ((1 - al) / a0);
    }
    float run (float x) { const float y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
};

int main (int argc, char** argv)
{
    const double sr = 44100.0, bpm = 126.0, swing = 0.58;
    const int bars = 32;
    const double beat = 60.0 / bpm, lead = argc > 1 ? std::atof (argv[1]) : 0.0;
    const size_t n = (size_t) ((bars * 4 * beat + lead + 1.0) * sr);
    std::vector<float> x (n, 0.0f);
    std::mt19937 rng (7);
    std::uniform_real_distribution<float> uni (-1.0f, 1.0f);
    std::array<std::vector<double>, bj::NumLanes> truth;

    auto add = [&] (double t, int lane, float gain)
    {
        truth[(size_t) lane].push_back (t);
        const size_t s0 = (size_t) (t * sr);
        Biquad hpf (Biquad::HP, 8000, sr), hpf2 (Biquad::HP, 8000, sr), bpf (Biquad::BP, lane == bj::Clap ? 1800 : 3000, sr);
        const int len = (int) (0.4 * sr);
        double ph = 0;
        for (int i = 0; i < len && s0 + (size_t) i < n; ++i)
        {
            const double tt = i / sr;
            float v = 0;
            const float nz = uni (rng);
            const float hp = 3.0f * hpf2.run (hpf.run (nz));
            const float bp = 2.0f * bpf.run (nz);
            switch (lane)
            {
                case bj::Kick:      ph += 2 * M_PI * (48 + 110 * std::exp (-tt * 45)) / sr; v = (float) (std::sin (ph) * std::exp (-tt * 14)); break;
                case bj::Snare:     v = (float) (0.7 * std::sin (2 * M_PI * 190 * tt) * std::exp (-tt * 28) + 0.6 * bp * std::exp (-tt * 22)); break;
                case bj::Clap:      { double e = 0; for (double d : { 0.0, 0.011, 0.022 }) if (tt >= d) e = std::max (e, std::exp (-(tt - d) * (d < 0.02 ? 180 : 28))); v = (float) (0.8 * bp * e); } break;
                case bj::ClosedHat: v = (float) (0.5 * hp * std::exp (-tt * 90)); break;
                case bj::OpenHat:   v = (float) (0.45 * hp * std::exp (-tt * 9)); break;
                default:            v = (float) (0.6 * std::sin (2 * M_PI * 520 * tt * (1 + 0.3 * std::exp (-tt * 60))) * std::exp (-tt * 35)); break;
            }
            x[s0 + (size_t) i] += gain * v;
        }
    };

    for (int bar = 0; bar < bars; ++bar)
        for (int s = 0; s < 16; ++s)
        {
            const double t = lead + (bar * 4 + s / 4 + ((s % 4) / 2) * 0.5 + ((s % 2) ? swing * 0.5 : 0.0)) * beat;
            if (s % 4 == 0) add (t, bj::Kick, 0.9f);
            if (s == 4 || s == 12) add (t, bar % 8 < 4 ? bj::Clap : bj::Snare, 0.6f);
            if (s % 4 == 2) add (t, bj::OpenHat, 0.5f);
            else if (s % 4 != 0 || true) { if (s % 4 != 2) add (t, bj::ClosedHat, (s % 2) ? 0.3f : 0.5f); }
            if (s == 3 || s == 11 || s == 15) add (t, bj::Perc, 0.45f);
        }

    // music bed: off-beat bass + sustained saw chords
    for (size_t i = 0; i < n; ++i)
    {
        const double t = i / sr, bt = std::fmod ((t - lead) / beat + 64.0, 1.0);
        const double bassEnv = bt > 0.5 ? std::min (1.0, (bt - 0.5) * 40) * std::exp (-(bt - 0.5) * 4) : 0.0;
        float v = (float) (0.35 * bassEnv * std::sin (2 * M_PI * 55 * t));
        for (double f : { 220.0, 277.2, 329.6, 415.3 })
            v += (float) (0.035 * (2.0 * std::fmod (t * f, 1.0) - 1.0));
        x[i] += v;
    }

    if (argc > 2)   // optional: write the test loop as a 16-bit mono WAV
    {
        FILE* f = std::fopen (argv[2], "wb");
        const unsigned bytes = (unsigned) n * 2, hdr[] = { 0x46464952u, 36 + bytes, 0x45564157u, 0x20746d66u, 16, 0x00010001u, (unsigned) sr, (unsigned) sr * 2, 0x00100002u, 0x61746164u, bytes };
        std::fwrite (hdr, 4, 11, f);
        for (float v : x) { const short q = (short) std::lround (std::max (-1.0f, std::min (1.0f, v * 0.5f)) * 32767.0f); std::fwrite (&q, 2, 1, f); }
        std::fclose (f);
    }

    auto A = bj::analyse (x.data(), n, sr);
    if (! A.valid) { std::puts ("analysis failed"); return 1; }
    std::printf ("bpm %.2f (true %.2f)  origin %.4f s (beat = %.4f)\n", A.bpm, bpm, A.beatOrigin, beat);

    const float thr[bj::NumLanes] = { 0.4f, 0.3f, 0.3f, 0.22f, 0.22f, 0.45f };
    bool ok = std::abs (A.bpm - bpm) < 0.2;
    for (int l = 0; l < bj::NumLanes; ++l)
    {
        int found = 0, matched = 0; double err = 0;
        std::vector<char> used (truth[(size_t) l].size(), 0);
        for (auto& h : A.lanes[(size_t) l])
        {
            if (h.strength < thr[l]) continue;
            ++found;
            for (size_t i = 0; i < used.size(); ++i)
                if (! used[i] && std::abs (truth[(size_t) l][i] - h.time) < 0.03) { used[i] = 1; ++matched; err += h.time - truth[(size_t) l][i]; break; }
        }
        const double recall = truth[(size_t) l].empty() ? 1 : (double) matched / truth[(size_t) l].size();
        const double precision = found ? (double) matched / found : 1;
        std::printf ("%-10s true %4zu  found %4d  recall %.2f  precision %.2f  mean offset %+.1f ms\n",
                     bj::laneName (l), truth[(size_t) l].size(), found, recall, precision, matched ? 1000 * err / matched : 0.0);
        if (recall < 0.8 || precision < 0.8) ok = false;
    }
    std::array<float, bj::NumLanes> t; for (int l = 0; l < bj::NumLanes; ++l) t[(size_t) l] = thr[l];
    bj::RegionSettings rs; rs.startBeat = bj::suggestRegionStart (A, t, 32);
    std::printf ("suggested start beat %.0f, swing %.1f%% (true %.1f%%)\n", rs.startBeat, bj::measureSwing (A, t, rs), swing * 100);
    std::puts (ok ? "PASS" : "CHECK");
    return 0;
}
