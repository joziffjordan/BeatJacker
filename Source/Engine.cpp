#include "Engine.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <numeric>

namespace bj
{
static const char* kNames[NumLanes] = { "Kick", "Snare", "Clap", "Closed Hat", "Open Hat", "Perc" };
static const int kNotes[NumLanes] = { 36, 38, 39, 42, 46, 37 };

const char* laneName (int lane) { return kNames[std::clamp (lane, 0, (int) NumLanes - 1)]; }
int laneMidiNote (int lane) { return kNotes[std::clamp (lane, 0, (int) NumLanes - 1)]; }

namespace
{
struct FFT
{
    int n;
    std::vector<int> rev;
    std::vector<std::complex<float>> w;

    explicit FFT (int size) : n (size), rev ((size_t) size), w ((size_t) size / 2)
    {
        int bits = 0;
        while ((1 << bits) < n) ++bits;
        for (int i = 0; i < n; ++i)
        {
            int r = 0;
            for (int b = 0; b < bits; ++b)
                if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            rev[(size_t) i] = r;
        }
        for (int i = 0; i < n / 2; ++i)
            w[(size_t) i] = std::polar (1.0f, (float) (-2.0 * M_PI * i / n));
    }

    void run (std::complex<float>* a) const
    {
        for (int i = 0; i < n; ++i)
            if (i < rev[(size_t) i]) std::swap (a[i], a[rev[(size_t) i]]);
        for (int len = 2; len <= n; len <<= 1)
        {
            const int half = len / 2, step = n / len;
            for (int i = 0; i < n; i += len)
                for (int j = 0; j < half; ++j)
                {
                    const auto u = a[i + j];
                    const auto v = a[i + j + half] * w[(size_t) (j * step)];
                    a[i + j] = u + v;
                    a[i + j + half] = u - v;
                }
        }
    }
};

struct Cand
{
    int t;
    float v;   // raw flux
    float s;   // normalised strength
};

float medianOf (float* b, int n)
{
    std::nth_element (b, b + n / 2, b + n);
    return b[n / 2];
}

// strongest candidate within +-w frames of t (nullptr if none)
const Cand* nearby (const std::vector<Cand>& c, int t, int w, float minStrength = 0.0f)
{
    auto it = std::lower_bound (c.begin(), c.end(), t - w, [] (const Cand& a, int x) { return a.t < x; });
    const Cand* best = nullptr;
    for (; it != c.end() && it->t <= t + w; ++it)
        if (it->s >= minStrength && (best == nullptr || it->s > best->s))
            best = &*it;
    return best;
}
}

Analysis analyse (const float* x, size_t n, double sr, const std::function<bool (float)>& progress)
{
    Analysis A;
    A.sampleRate = sr;
    auto report = [&] (float p) { return ! progress || progress (p); };

    const size_t maxSamples = (size_t) (sr * 60.0 * 8.0);   // analyse at most 8 minutes
    n = std::min (n, maxSamples);
    A.duration = (double) n / sr;
    if (n < (size_t) (sr * 2.0))
        return A;

    // ---- waveform overview
    {
        const size_t points = 2000;
        A.overview.assign (points, 0.0f);
        float mx = 1.0e-9f;
        for (size_t i = 0; i < n; ++i)
        {
            const float a = std::abs (x[i]);
            auto& o = A.overview[std::min (points - 1, i * points / n)];
            o = std::max (o, a);
            mx = std::max (mx, a);
        }
        for (auto& o : A.overview) o /= mx;
    }

    // ---- spectrogram
    const int N = sr > 60000.0 ? 2048 : 1024;
    const int hop = N / 4;
    const double fps = sr / hop;
    const int T = (int) (n / (size_t) hop) + 1;
    auto binOf = [&] (double hz) { return std::clamp ((int) std::lround (hz * N / sr), 1, N / 2); };
    const int K = binOf (16000.0) + 1;

    std::vector<float> S ((size_t) T * (size_t) K), M ((size_t) T * (size_t) K);
    {
        FFT fft (N);
        std::vector<float> win ((size_t) N);
        for (int i = 0; i < N; ++i)
            win[(size_t) i] = 0.5f - 0.5f * std::cos ((float) (2.0 * M_PI * i / N));
        std::vector<std::complex<float>> buf ((size_t) N);
        float gmax = 0.0f;
        for (int t = 0; t < T; ++t)
        {
            const long start = (long) t * hop - N / 2;
            for (int i = 0; i < N; ++i)
            {
                const long idx = start + i;
                buf[(size_t) i] = (idx >= 0 && idx < (long) n) ? x[idx] * win[(size_t) i] : 0.0f;
            }
            fft.run (buf.data());
            float* row = &S[(size_t) t * (size_t) K];
            for (int k = 0; k < K; ++k)
            {
                row[k] = std::abs (buf[(size_t) k]);
                gmax = std::max (gmax, row[k]);
            }
            if ((t & 1023) == 0 && ! report (0.35f * t / T)) return A;
        }
        if (gmax <= 0.0f) return A;
        const float g = 1.0f / gmax;
        for (auto& v : S) v *= g;
    }

    // ---- harmonic / percussive separation (median filtering), above 150 Hz only.
    // Kicks ring long enough to look "harmonic", so the low band is left raw.
    const int kH = binOf (150.0);
    const int kF = binOf (1500.0);   // below this, tonal drums (toms, congas) are too narrow for the frequency median
    {
        const int fw = 8;
        float tmp[64];
        for (int t = 0; t < T; ++t)
        {
            const float* row = &S[(size_t) t * (size_t) K];
            float* out = &M[(size_t) t * (size_t) K];
            for (int k = 0; k < kH; ++k) out[k] = row[k];
            for (int k = kH; k < K; ++k)
            {
                const int lo = std::max (1, k - fw), hi = std::min (K - 1, k + fw);
                const int len = hi - lo + 1;
                std::copy (row + lo, row + hi + 1, tmp);
                out[k] = medianOf (tmp, len);
            }
            if ((t & 1023) == 0 && ! report (0.35f + 0.2f * t / T)) return A;
        }

        const int tw = std::max (4, (int) std::lround (0.09 * fps));
        std::vector<float> col ((size_t) T);
        std::vector<float> wbuf ((size_t) (2 * tw + 1));
        for (int k = kH; k < K; ++k)
        {
            for (int t = 0; t < T; ++t) col[(size_t) t] = S[(size_t) t * (size_t) K + (size_t) k];
            for (int t = 0; t < T; ++t)
            {
                const int lo = std::max (0, t - tw), hi = std::min (T - 1, t + tw);
                const int len = hi - lo + 1;
                std::copy (col.begin() + lo, col.begin() + hi + 1, wbuf.begin());
                const float h = medianOf (wbuf.data(), len);
                float& m = M[(size_t) t * (size_t) K + (size_t) k];
                const float sv = col[(size_t) t];
                const float pf = k < kF ? 0.5f * sv : m;
                m = sv * (pf * pf) / (pf * pf + h * h + 1.0e-12f);
            }
            if ((k & 15) == 0 && ! report (0.55f + 0.33f * (k - kH) / (K - kH))) return A;
        }
    }

    // ---- band envelopes
    enum { bLow, bBody, bPerc, bMid, bHigh, NB };
    const int bandLo[NB] = { binOf (35), binOf (150), binOf (300), binOf (1500), binOf (7000) };
    const int bandHi[NB] = { binOf (120), binOf (350), binOf (1200), binOf (6000), K - 1 };
    std::array<std::vector<float>, NB> flux, en;
    for (auto& f : flux) f.assign ((size_t) T, 0.0f);
    for (auto& e : en) e.assign ((size_t) T, 0.0f);
    {
        const float G = 1000.0f;
        for (size_t i = 0; i < S.size(); ++i) S[i] = std::log1p (G * M[i]);   // S now holds log magnitudes
        for (int t = 0; t < T; ++t)
        {
            const float* p = &M[(size_t) t * (size_t) K];
            const float* l = &S[(size_t) t * (size_t) K];
            const float* l2 = t >= 2 ? &S[(size_t) (t - 2) * (size_t) K] : nullptr;
            for (int b = 0; b < NB; ++b)
            {
                float e = 0.0f, f = 0.0f;
                for (int k = bandLo[b]; k <= bandHi[b]; ++k)
                {
                    e += p[k] * p[k];
                    f += std::max (0.0f, l[k] - (l2 != nullptr ? l2[k] : 0.0f));
                }
                const float nb = (float) (bandHi[b] - bandLo[b] + 1);
                en[(size_t) b][(size_t) t] = std::sqrt (e / nb);
                flux[(size_t) b][(size_t) t] = f / nb;
            }
        }
    }
    S = {}; M = {};
    if (! report (0.9f)) return A;

    // ---- peak picking
    const int pw = std::max (2, (int) std::lround (0.03 * fps));
    const int lw = (int) std::lround (0.3 * fps);
    auto pickRaw = [&] (const std::vector<float>& e)
    {
        std::vector<Cand> c;
        std::vector<double> pre ((size_t) T + 1, 0.0);
        for (int t = 0; t < T; ++t) pre[(size_t) t + 1] = pre[(size_t) t] + e[(size_t) t];
        for (int t = 0; t < T - 1; ++t)
        {
            const float v = e[(size_t) t];
            if (v <= 0.0f) continue;
            bool isMax = true;
            for (int d = 1; d <= pw && isMax; ++d)
            {
                if (t - d >= 0 && e[(size_t) (t - d)] >= v) isMax = false;
                if (t + d < T && e[(size_t) (t + d)] > v) isMax = false;
            }
            if (! isMax) continue;
            const int lo = std::max (0, t - lw), hi = std::min (T, t + lw);
            const float mean = (float) ((pre[(size_t) hi] - pre[(size_t) lo]) / (hi - lo));
            if (v > 1.3f * mean + 1.0e-5f)
                c.push_back ({ t, v, 0.0f });
        }
        return c;
    };
    // "strong hit" level: the level reached about once every two seconds
    auto refLevel = [&] (const std::vector<Cand>& c)
    {
        if (c.empty()) return 0.0f;
        std::vector<float> v;
        v.reserve (c.size());
        for (auto& k : c) v.push_back (k.v);
        const size_t rank = std::min (v.size() - 1, (size_t) std::max (8.0, A.duration * 0.5));
        std::nth_element (v.begin(), v.begin() + (long) rank, v.end(), std::greater<float>());
        return v[rank];
    };

    std::array<std::vector<Cand>, NB> cands;
    std::array<float, NB> refs {};
    float maxRef = 0.0f;
    for (int b = 0; b < NB; ++b)
    {
        cands[(size_t) b] = pickRaw (flux[(size_t) b]);
        refs[(size_t) b] = refLevel (cands[(size_t) b]);
        maxRef = std::max (maxRef, refs[(size_t) b]);
    }
    if (maxRef <= 0.0f) return A;
    for (int b = 0; b < NB; ++b)
    {
        const float ref = std::max (refs[(size_t) b], 0.25f * maxRef);
        refs[(size_t) b] = ref;
        auto& c = cands[(size_t) b];
        for (auto& k : c) k.s = k.v / ref;
        c.erase (std::remove_if (c.begin(), c.end(), [] (const Cand& k) { return k.s < 0.08f; }), c.end());
    }

    // ---- tempo + beat phase
    {
        std::vector<float> env ((size_t) T), envLow ((size_t) T);
        for (int t = 0; t < T; ++t)
        {
            envLow[(size_t) t] = flux[bLow][(size_t) t] / refs[bLow];
            env[(size_t) t] = envLow[(size_t) t] + flux[bMid][(size_t) t] / refs[bMid] + flux[bHigh][(size_t) t] / refs[bHigh];
        }
        const int maxLag = (int) (fps * 60.0 / 60.0) * 4 + 2;
        std::vector<double> acf ((size_t) maxLag + 1, 0.0);
        for (int lag = 1; lag <= maxLag; ++lag)
        {
            double s = 0.0;
            for (int t = lag; t < T; ++t) s += (double) env[(size_t) t] * env[(size_t) (t - lag)];
            acf[(size_t) lag] = s / (T - lag);
        }
        auto acfAt = [&] (double lag)
        {
            const int i = (int) lag;
            if (i + 1 > maxLag) return 0.0;
            const double f = lag - i;
            return acf[(size_t) i] * (1.0 - f) + acf[(size_t) i + 1] * f;
        };
        double bestBpm = 120.0, bestScore = -1.0;
        for (double bpm = 60.0; bpm <= 200.0; bpm += 0.25)
        {
            const double lag = fps * 60.0 / bpm;
            const double prior = std::exp (-0.5 * std::pow (std::log2 (bpm / 122.0) / 0.55, 2.0));
            const double sc = (acfAt (lag) + 0.5 * acfAt (2.0 * lag) + 0.25 * acfAt (4.0 * lag)) * prior;
            if (sc > bestScore) { bestScore = sc; bestBpm = bpm; }
        }

        auto envAt = [&] (const std::vector<float>& e, double pos)
        {
            const int i = (int) pos;
            if (i < 0 || i + 1 >= T) return 0.0f;
            const float f = (float) (pos - i);
            return e[(size_t) i] * (1.0f - f) + e[(size_t) i + 1] * f;
        };
        auto combScore = [&] (double bpm, double& phaseOut)
        {
            const double period = fps * 60.0 / bpm;
            double best = -1.0;
            for (double ph = 0.0; ph < period; ph += 1.0)
            {
                double s = 0.0;
                for (double pos = ph; pos < T - 1; pos += period)
                    s += envAt (env, pos) + 2.0 * envAt (envLow, pos);
                if (s > best) { best = s; phaseOut = ph; }
            }
            return best;
        };
        double phase = 0.0, refined = bestBpm, refinedScore = -1.0;
        for (double bpm = bestBpm * 0.97; bpm <= bestBpm * 1.03; bpm += 0.02)
        {
            double ph = 0.0;
            const double sc = combScore (bpm, ph);
            if (sc > refinedScore) { refinedScore = sc; refined = bpm; phase = ph; }
        }
        if (std::abs (refined - std::round (refined)) < 0.12)
        {
            refined = std::round (refined);
            combScore (refined, phase);
        }
        // sub-frame phase refinement
        {
            const double period = fps * 60.0 / refined;
            double best = -1.0, bestPh = phase;
            for (double ph = phase - 1.0; ph <= phase + 1.0; ph += 0.1)
            {
                double s = 0.0;
                for (double pos = ph; pos < T - 1; pos += period)
                    s += envAt (env, pos) + 2.0 * envAt (envLow, pos);
                if (s > best) { best = s; bestPh = ph; }
            }
            phase = bestPh;
        }
        A.bpm = refined;
        double origin = phase * hop / sr;
        const double beatLen = 60.0 / refined;
        while (origin < 0.0) origin += beatLen;
        while (origin >= beatLen) origin -= beatLen;
        A.beatOrigin = origin;
    }
    if (! report (0.95f)) return A;

    // ---- classification
    const int cw = std::max (2, (int) std::lround (0.025 * fps));
    auto timeOf = [&] (int t) { return (double) t * hop / sr; };
    auto peakEnergy = [&] (int band, int t)
    {
        float e = 0.0f;
        for (int i = t; i <= std::min (T - 1, t + 4); ++i) e = std::max (e, en[(size_t) band][(size_t) i]);
        return e;
    };
    auto riseOf = [&] (int band, int t)
    {
        return std::max (0.0f, peakEnergy (band, t) - en[(size_t) band][(size_t) std::max (0, t - 3)]);
    };
    auto push = [&] (int lane, int band, const Cand& c, float length = 0.0f)
    {
        Hit h;
        h.time = timeOf (c.t);
        h.strength = c.s;
        h.velocity = peakEnergy (band, c.t);   // normalised below
        h.length = length;
        A.lanes[(size_t) lane].push_back (h);
    };

    auto fluxNear = [&] (int band, int t)
    {
        float f = 0.0f;
        for (int i = std::max (0, t - cw); i <= std::min (T - 1, t + cw); ++i) f = std::max (f, flux[(size_t) band][(size_t) i]);
        return f / refs[(size_t) band];
    };

    // kicks: low-band onsets. A kick also has a broadband transient, a bass note mostly does not,
    // so low onsets with nothing
    // arriving just above them (150-350 Hz) are pushed down the strength scale.
    {
        std::vector<Cand> kicks = cands[bLow];
        for (auto& c : kicks)
        {
            const float tr = fluxNear (bBody, c.t);
            const float f = std::clamp (tr / 0.5f, 0.3f, 1.0f);
            c.v = c.s * f * f;
        }
        const float ref = std::max (refLevel (kicks), 1.0e-6f);
        for (auto& c : kicks) { c.s = c.v / ref; if (c.s >= 0.08f) push (Kick, bLow, c); }
        cands[bLow] = kicks;
        cands[bLow].erase (std::remove_if (cands[bLow].begin(), cands[bLow].end(), [] (const Cand& k) { return k.s < 0.08f; }), cands[bLow].end());
    }

    auto twoMeans = [] (std::vector<float> fs, float minGap, float minShare, float& split)
    {
        if (fs.size() < 8) return false;
        std::sort (fs.begin(), fs.end());
        float c0 = fs[fs.size() / 10], c1 = fs[fs.size() - 1 - fs.size() / 10];
        size_t n0 = 0;
        for (int it = 0; it < 12; ++it)
        {
            double s0 = 0, s1 = 0; n0 = 0;
            for (float f : fs)
                if (std::abs (f - c0) <= std::abs (f - c1)) { s0 += f; ++n0; } else s1 += f;
            if (n0 == 0 || n0 == fs.size()) return false;
            c0 = (float) (s0 / n0);
            c1 = (float) (s1 / (fs.size() - n0));
        }
        const float share = (float) std::min (n0, fs.size() - n0) / (float) fs.size();
        if (c1 - c0 < minGap || share < minShare) return false;
        split = 0.5f * (c0 + c1);
        return true;
    };

    // hat-like vs snare/clap-like: where the burst's energy sits (above 7 kHz vs 1.5-6 kHz)
    auto brightness = [&] (int t) { return std::log ((riseOf (bHigh, t) + 1.0e-6f) / (riseOf (bMid, t) + 1.0e-6f)); };
    float brightSplit = std::log (1.2f);
    {
        std::vector<float> qs;
        for (auto& c : cands[bMid])
            if (auto* h = nearby (cands[bHigh], c.t, cw))
                if (std::max (c.s, h->s) >= 0.3f) qs.push_back (brightness (c.t));
        float sp = 0.0f;
        if (twoMeans (qs, std::log (3.0f), 0.08f, sp)) brightSplit = sp;
    }

    // snare / clap: mid-band noise bursts
    std::vector<int> burstFrames;
    {
        std::vector<float> kb;
        for (auto& c : cands[bLow])
            if (c.s > 0.5f && nearby (cands[bMid], c.t, cw, 0.2f) == nullptr)
                kb.push_back (riseOf (bBody, c.t));
        const float kickBody = kb.empty() ? 0.0f : medianOf (kb.data(), (int) kb.size());

        struct Mid { const Cand* c; float f; };
        std::vector<Mid> mids;
        for (auto& c : cands[bMid])
        {
            if (nearby (cands[bHigh], c.t, cw) != nullptr && brightness (c.t) > brightSplit)
                continue;   // the lower edge of a hi-hat
            float body = riseOf (bBody, c.t);
            if (nearby (cands[bLow], c.t, cw, 0.3f) != nullptr) body = std::max (0.0f, body - kickBody);
            const float mid = riseOf (bMid, c.t) + 1.0e-6f;
            mids.push_back ({ &c, std::log ((body + 0.1f * mid) / mid) });
            if (c.s >= 0.2f) burstFrames.push_back (c.t);
        }
        std::vector<float> fs;
        for (auto& m : mids) if (m.c->s >= 0.3f) fs.push_back (m.f);
        float split = std::log (0.4f);
        if (! twoMeans (fs, 1.5f, 0.15f, split) && ! fs.empty())
        {
            std::sort (fs.begin(), fs.end());
            split = fs[fs.size() / 2] > std::log (0.4f) ? -1.0e9f : 1.0e9f;   // all snare or all clap
        }
        for (auto& m : mids)
            push (m.f > split ? Snare : Clap, bMid, *m.c);
    }

    // hi-hats: high-band bursts, open vs closed by how long they ring
    {
        std::vector<Cand> hc;
        for (auto& c : cands[bHigh])
            if (nearby (cands[bMid], c.t, cw) == nullptr || brightness (c.t) > brightSplit)
                hc.push_back (c);
        const auto& eh = en[bHigh];
        for (size_t i = 0; i < hc.size(); ++i)
        {
            const Cand& c = hc[i];
            int next = T - 1;
            for (size_t j = i + 1; j < hc.size(); ++j)
                if (hc[j].s >= 0.2f) { next = hc[j].t - 2; break; }
            const float pk = peakEnergy (bHigh, c.t);
            const int limit = std::min ({ T - 1, next, c.t + (int) (0.6 * fps) });
            int t = c.t;
            while (t < limit && (t < c.t + 2 || eh[(size_t) t] > 0.3f * pk)) ++t;
            const float ring = (float) ((t - c.t) * hop / sr);
            if (ring >= 0.08f) push (OpenHat, bHigh, c, ring);
            else push (ClosedHat, bHigh, c);
        }
    }

    // perc: mid-range hits that are not a kick, snare or clap
    for (auto& c : cands[bPerc])
    {
        if (nearby (cands[bLow], c.t, cw, 0.2f) != nullptr) continue;
        auto it = std::lower_bound (burstFrames.begin(), burstFrames.end(), c.t - cw);
        if (it != burstFrames.end() && *it <= c.t + cw) continue;
        push (Perc, bPerc, c);
    }

    // ---- velocities relative to each lane's loud hits
    for (auto& lane : A.lanes)
    {
        std::vector<float> e;
        for (auto& h : lane) if (h.strength >= 0.3f) e.push_back (h.velocity);
        float ref = 1.0e-9f;
        if (! e.empty())
        {
            const size_t r = (size_t) (0.9 * (e.size() - 1));
            std::nth_element (e.begin(), e.begin() + (long) r, e.end());
            ref = std::max (ref, e[r]);
        }
        for (auto& h : lane)
            h.velocity = std::clamp (std::pow (h.velocity / ref, 0.7f), 0.15f, 1.0f);
    }

    A.valid = true;
    report (1.0f);
    return A;
}

std::vector<Note> regionNotes (const Analysis& A, int lane, float threshold, const RegionSettings& rs)
{
    std::vector<Note> out;
    if (! A.valid) return out;
    const double bpm = A.bpm * rs.tempoScale;
    const double t0 = A.beatTime (rs.startBeat);
    const double g = (double) rs.gridPerBeat;
    const double early = 0.5 / g;
    const double minGap = 0.5 / g;
    for (auto& h : A.lanes[(size_t) lane])
    {
        if (h.strength < threshold) continue;
        const double b = (h.time - t0) * bpm / 60.0;
        if (b < -early) continue;
        if (b >= rs.numBeats) break;
        const double q = std::round (b * g) / g;
        if (q >= rs.numBeats) continue;
        const double pos = std::clamp (b + (q - b) * (double) rs.quantise, 0.0, (double) rs.numBeats - 0.01);
        Note n;
        n.beat = pos;
        n.velocity = std::clamp ((int) std::lround (20.0f + 107.0f * h.velocity), 1, 127);
        n.lengthBeats = lane == OpenHat ? std::clamp ((double) h.length * bpm / 60.0, 0.25, 1.0) : 0.25;
        if (! out.empty() && pos - out.back().beat < minGap)
        {
            if (n.velocity > out.back().velocity) { n.beat = out.back().beat; out.back() = n; }
            continue;
        }
        out.push_back (n);
    }
    return out;
}

float measureSwing (const Analysis& A, const std::array<float, NumLanes>& thr, const RegionSettings& rsIn)
{
    RegionSettings rs = rsIn;
    rs.quantise = 0.0f;
    double evenSum = 0, oddSum = 0;
    int evenN = 0, oddN = 0;
    for (int lane = 0; lane < NumLanes; ++lane)
        for (auto& n : regionNotes (A, lane, thr[(size_t) lane], rs))
        {
            const double p = n.beat * 4.0;
            const double idx = std::round (p);
            const double off = p - idx;
            if (std::abs (off) > 0.45) continue;
            if (((long) idx) % 2 == 0) { evenSum += off; ++evenN; } else { oddSum += off; ++oddN; }
        }
    if (oddN < 4 || evenN < 4) return -1.0f;
    return (float) (50.0 + 50.0 * (oddSum / oddN - evenSum / evenN));
}

double suggestRegionStart (const Analysis& A, const std::array<float, NumLanes>& thr, int numBeats)
{
    if (! A.valid) return 0.0;
    const int total = (int) std::floor (A.numBeats());
    if (total <= numBeats) return 0.0;
    std::vector<int> perBeat ((size_t) total + 1, 0);
    int firstBeat = -1;
    for (int lane = 0; lane < NumLanes; ++lane)
        for (auto& h : A.lanes[(size_t) lane])
        {
            if (h.strength < thr[(size_t) lane]) continue;
            const double b = (h.time - A.beatOrigin) * A.bpm / 60.0;
            const int i = (int) std::floor (b + 0.125);
            if (i < 0 || i > total) continue;
            ++perBeat[(size_t) i];
            if (lane == Kick && firstBeat < 0 && std::abs (b - std::round (b)) < 0.15) firstBeat = (int) std::lround (b);
        }
    if (firstBeat < 0) firstBeat = 0;
    std::vector<int> pre ((size_t) total + 2, 0);
    for (int i = 0; i <= total; ++i) pre[(size_t) i + 1] = pre[(size_t) i] + perBeat[(size_t) i];
    int best = firstBeat % 16, bestCount = -1;
    for (int s = firstBeat % 16; s + numBeats <= total; s += 16)
    {
        const int c = pre[(size_t) (s + numBeats)] - pre[(size_t) s];
        if (c > bestCount * 1.05 + 1) { bestCount = c; best = s; }   // prefer the earliest busy section
    }
    return (double) best;
}
}
