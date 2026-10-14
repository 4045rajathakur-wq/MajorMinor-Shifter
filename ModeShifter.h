#pragma once
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <vector>

/*
    ModeShifter
    -----------
    STFT phase vocoder (4096 FFT, 75% overlap, identity phase locking).
    Every spectral peak is classified by pitch class relative to the key's tonic.
      Major -> Minor : 3rd, 6th, 7th degrees move DOWN 1 semitone
      Minor -> Major : 3rd, 6th, 7th degrees move UP   1 semitone
    Peaks (with their surrounding bins) are shifted individually; everything else passes through.
*/
class ModeShifter
{
public:
    static constexpr int kOrder = 12;
    static constexpr int kN = 1 << kOrder;   // 4096
    static constexpr int kHop = kN / 4;      // 75% overlap
    static constexpr int kBins = kN / 2 + 1;
    static constexpr int kLatency = kN;      // reported to the host

    struct Params
    {
        int key = -1;            // -1 = auto-detect, 0..11 = C..B (tonic of the SOURCE sample)
        int convert = 0;         // 0 = auto (flip detected mode), 1 = major->minor, 2 = minor->major
        float mix = 1.0f;
        float tuneCents = 0.0f;  // sample tuning offset vs A=440
        float maxHz = 2500.0f;   // peaks above this are left alone (mostly overtones / air)
        float thresholdDb = -45.0f;
        bool holdKey = false;    // freeze key detection
    };

    ModeShifter() : fft(kOrder) {}

    void prepare(double sampleRate)
    {
        sr = (float) sampleRate;
        window.resize(kN);
        for (int i = 0; i < kN; ++i)
            window[i] = 0.5f - 0.5f * std::cos(juce::MathConstants<float>::twoPi * (float) i / (float) kN);

        fftData.assign(2 * kN, 0.0f);
        mag.assign(kBins, 0.0f);
        ph.assign(kBins, 0.0f);
        freqBin.assign(kBins, 0.0f);
        spec.assign(2 * kBins, 0.0f);

        for (auto& ch : chans)
        {
            ch.inBuf.assign(kN, 0.0f);
            ch.outBuf.assign(kN, 0.0f);
            ch.dryBuf.assign(kN, 0.0f);
            ch.lastPhase.assign(kBins, 0.0f);
            ch.prevSynth.assign(kBins, 0.0f);
            ch.curSynth.assign(kBins, 0.0f);
            ch.prevActive.assign(kBins, 0);
            ch.curActive.assign(kBins, 0);
        }
        chroma.fill(0.0f);
        pos = 0;
        hopCount = 0;
        detRoot = -1;
        detMinor = 0;
        detConf = 0.0f;
    }

    int getDetectedRoot() const { return detRoot.load(); }
    bool isDetectedMinor() const { return detMinor.load() != 0; }
    float getDetectedConfidence() const { return detConf.load(); } // correlation, -1..1

    void process(float* const* data, int numCh, int numSamples, const Params& p)
    {
        numCh = std::min(numCh, 2);
        if (window.empty() || numCh <= 0) return;

        for (int n = 0; n < numSamples; ++n)
        {
            for (int c = 0; c < numCh; ++c)
            {
                auto& ch = chans[(size_t) c];
                const float in = data[c][n];
                const float dry = ch.dryBuf[(size_t) pos];   // delayed by kN, matches wet latency
                ch.dryBuf[(size_t) pos] = in;
                ch.inBuf[(size_t) pos] = in;
                const float wet = ch.outBuf[(size_t) pos];
                ch.outBuf[(size_t) pos] = 0.0f;
                data[c][n] = dry * (1.0f - p.mix) + wet * p.mix;
            }

            pos = (pos + 1) & (kN - 1);

            if (++hopCount >= kHop)
            {
                hopCount = 0;
                for (int c = 0; c < numCh; ++c)
                    processFrame(c, p);
            }
        }
    }

private:
    struct Channel
    {
        std::vector<float> inBuf, outBuf, dryBuf;
        std::vector<float> lastPhase, prevSynth, curSynth;
        std::vector<char> prevActive, curActive;
    };

    static constexpr float kTwoPi = juce::MathConstants<float>::twoPi;

    static int shiftFor(int pc, bool srcMinor)
    {
        if (! srcMinor) return (pc == 4 || pc == 9 || pc == 11) ? -1 : 0; // maj3, maj6, maj7 -> flat
        return (pc == 3 || pc == 8 || pc == 10) ? +1 : 0;                 // min3, min6, min7 -> natural
    }

    static int pitchClass(float midi, int root)
    {
        const int note = (int) std::lround(midi);
        return (((note - root) % 12) + 12) % 12;
    }

    void updateKey(int c, const Params& p)
    {
        if (c != 0 || p.holdKey) return;

        static const float kMaj[12] = { 6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f, 2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f };
        static const float kMin[12] = { 6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f, 2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f };

        const float binHz = sr / (float) kN;
        float frame[12] = {};
        float total = 0.0f;

        for (int pk : peaks)
        {
            const float hz = freqBin[(size_t) pk] * binHz;
            if (hz < 65.0f || hz > 2000.0f) continue;
            const float midi = 69.0f + 12.0f * std::log2(hz / 440.0f) - p.tuneCents / 100.0f;
            frame[pitchClass(midi, 0)] += mag[(size_t) pk];
            total += mag[(size_t) pk];
        }
        if (total < 2.0f) return; // silence

        for (int i = 0; i < 12; ++i)
            chroma[(size_t) i] = chroma[(size_t) i] * 0.997f + frame[i] / total;

        auto corr = [&](int root, bool minor)
        {
            const float* prof = minor ? kMin : kMaj;
            double mx = 0, my = 0;
            for (int i = 0; i < 12; ++i) { mx += chroma[(size_t) i]; my += prof[(i - root + 12) % 12]; }
            mx /= 12; my /= 12;
            double sxy = 0, sxx = 0, syy = 0;
            for (int i = 0; i < 12; ++i)
            {
                const double a = chroma[(size_t) i] - mx, b = prof[(i - root + 12) % 12] - my;
                sxy += a * b; sxx += a * a; syy += b * b;
            }
            return (float) (sxy / std::sqrt(sxx * syy + 1e-12));
        };

        float best = -2.0f; int bestRoot = 0; bool bestMinor = false;
        for (int r = 0; r < 12; ++r)
        {
            corrMaj[(size_t) r] = corr(r, false);
            corrMin[(size_t) r] = corr(r, true);
            if (corrMaj[(size_t) r] > best) { best = corrMaj[(size_t) r]; bestRoot = r; bestMinor = false; }
            if (corrMin[(size_t) r] > best) { best = corrMin[(size_t) r]; bestRoot = r; bestMinor = true; }
        }

        // hysteresis: only switch if clearly better than the current key
        const int curRoot = detRoot.load();
        if (curRoot < 0)
        {
            detRoot = bestRoot; detMinor = bestMinor ? 1 : 0;
        }
        else
        {
            const float cur = detMinor.load() ? corrMin[(size_t) curRoot] : corrMaj[(size_t) curRoot];
            if (best > cur + 0.03f) { detRoot = bestRoot; detMinor = bestMinor ? 1 : 0; }
        }

        const int r = detRoot.load();
        detConf = detMinor.load() ? corrMin[(size_t) r] : corrMaj[(size_t) r];
    }

    void processFrame(int c, const Params& p)
    {
        auto& ch = chans[(size_t) c];
        float* d = fftData.data();
        const float binHz = sr / (float) kN;

        // ---- analysis ----
        for (int i = 0; i < kN; ++i)
            d[i] = ch.inBuf[(size_t) ((pos + i) & (kN - 1))] * window[(size_t) i];
        std::fill(d + kN, d + 2 * kN, 0.0f);
        fft.performRealOnlyForwardTransform(d, true);

        float maxMag = 0.0f;
        for (int k = 0; k < kBins; ++k)
        {
            const float re = d[2 * k], im = d[2 * k + 1];
            mag[(size_t) k] = std::sqrt(re * re + im * im);
            ph[(size_t) k] = std::atan2(im, re);
            maxMag = std::max(maxMag, mag[(size_t) k]);

            const float expected = kTwoPi * (float) k * (float) kHop / (float) kN;
            float delta = ph[(size_t) k] - ch.lastPhase[(size_t) k] - expected;
            delta = std::remainder(delta, kTwoPi);
            freqBin[(size_t) k] = (float) k + delta * (float) kN / (kTwoPi * (float) kHop);
            ch.lastPhase[(size_t) k] = ph[(size_t) k];
        }

        // ---- peak picking ----
        peaks.clear();
        const float peakFloor = maxMag * 1e-4f;
        for (int k = 2; k < kBins - 2; ++k)
        {
            const float m = mag[(size_t) k];
            if (m > peakFloor && m > mag[(size_t) k - 1] && m >= mag[(size_t) k + 1]
                && m > mag[(size_t) k - 2] && m >= mag[(size_t) k + 2])
                peaks.push_back(k);
        }

        updateKey(c, p);

        // ---- resolve key / direction ----
        int root = p.key >= 0 ? p.key : detRoot.load();
        if (root < 0) root = 0;
        bool srcMinor;
        if (p.convert == 1) srcMinor = false;
        else if (p.convert == 2) srcMinor = true;
        else srcMinor = (p.key >= 0) ? (corrMin[(size_t) root] > corrMaj[(size_t) root]) : (detMinor.load() != 0);

        // ---- spectral remapping ----
        std::fill(spec.begin(), spec.end(), 0.0f);
        std::fill(ch.curActive.begin(), ch.curActive.end(), (char) 0);

        if (peaks.empty())
        {
            for (int k = 0; k < kBins; ++k) { spec[(size_t) 2 * k] = d[2 * k]; spec[(size_t) 2 * k + 1] = d[2 * k + 1]; }
        }
        else
        {
            const float thr = maxMag * std::pow(10.0f, p.thresholdDb / 20.0f);
            const int np = (int) peaks.size();

            auto boundary = [&](int a, int b)
            {
                int best = peaks[(size_t) a];
                for (int k = peaks[(size_t) a]; k <= peaks[(size_t) b]; ++k)
                    if (mag[(size_t) k] < mag[(size_t) best]) best = k;
                return best;
            };

            int lo = 0;
            for (int i = 0; i < np; ++i)
            {
                const int pk = peaks[(size_t) i];
                const int hi = (i == np - 1) ? kBins - 1 : boundary(i, i + 1);

                int s = 0;
                const float hz = freqBin[(size_t) pk] * binHz;
                if (hz >= 40.0f && hz <= p.maxHz && mag[(size_t) pk] >= thr)
                {
                    const float midi = 69.0f + 12.0f * std::log2(hz / 440.0f) - p.tuneCents / 100.0f;
                    s = shiftFor(pitchClass(midi, root), srcMinor);
                }

                if (s == 0)
                {
                    for (int k = lo; k <= hi; ++k)
                    {
                        spec[(size_t) 2 * k] += d[2 * k];
                        spec[(size_t) 2 * k + 1] += d[2 * k + 1];
                    }
                }
                else
                {
                    const float ratio = std::pow(2.0f, (float) s / 12.0f);
                    const int target = (int) std::lround(freqBin[(size_t) pk] * ratio);

                    float synthPh = ph[(size_t) pk];
                    for (int dd : { 0, -1, 1 })
                    {
                        const int t = target + dd;
                        if (t >= 0 && t < kBins && ch.prevActive[(size_t) t])
                        {
                            const float advance = kTwoPi * (float) kHop * freqBin[(size_t) pk] * ratio / (float) kN;
                            synthPh = std::remainder(ch.prevSynth[(size_t) t] + advance, kTwoPi);
                            break;
                        }
                    }
                    if (target >= 0 && target < kBins)
                    {
                        ch.curSynth[(size_t) target] = synthPh;
                        ch.curActive[(size_t) target] = 1;
                    }

                    const int shift = target - pk;
                    for (int k = lo; k <= hi; ++k)
                    {
                        const int t = k + shift;
                        if (t < 0 || t >= kBins) continue;
                        const float newPh = synthPh + (ph[(size_t) k] - ph[(size_t) pk]);
                        spec[(size_t) 2 * t] += mag[(size_t) k] * std::cos(newPh);
                        spec[(size_t) 2 * t + 1] += mag[(size_t) k] * std::sin(newPh);
                    }
                }
                lo = hi + 1;
            }
        }

        std::swap(ch.prevSynth, ch.curSynth);
        std::swap(ch.prevActive, ch.curActive);

        // ---- synthesis ----
        spec[1] = 0.0f;
        spec[(size_t) 2 * (kBins - 1) + 1] = 0.0f;
        for (int k = 0; k < kBins; ++k) { d[2 * k] = spec[(size_t) 2 * k]; d[2 * k + 1] = spec[(size_t) 2 * k + 1]; }
        for (int k = 1; k < kN / 2; ++k)
        {
            d[2 * (kN - k)] = spec[(size_t) 2 * k];
            d[2 * (kN - k) + 1] = -spec[(size_t) 2 * k + 1];
        }
        fft.performRealOnlyInverseTransform(d);

        // Hann * Hann at 75% overlap sums to 1.5. (JUCE's inverse FFT is already 1/N normalised.)
        constexpr float kScale = 1.0f / 1.5f;
        for (int i = 0; i < kN; ++i)
            ch.outBuf[(size_t) ((pos + i) & (kN - 1))] += d[i] * window[(size_t) i] * kScale;
    }

    juce::dsp::FFT fft;
    float sr = 44100.0f;
    int pos = 0, hopCount = 0;

    std::array<Channel, 2> chans;
    std::vector<float> window, fftData, mag, ph, freqBin, spec;
    std::vector<int> peaks;

    std::array<float, 12> chroma {};
    std::array<float, 12> corrMaj {}, corrMin {};
    std::atomic<int> detRoot { -1 }, detMinor { 0 };
    std::atomic<float> detConf { 0.0f };
};
