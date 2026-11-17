// Measures how close the copy for viewers is to a perfect copy, with test tones.
// Compares the two ways BridgeOutput corrects clock drift:
//   - same rate: no converting, one sample skipped now and then (BridgeOutput::copyWithSlip),
//   - different rates: JUCE's LagrangeInterpolator at speed 1 + drift.
// Passes (exit 0) when copyWithSlip keeps the level and beats the converter at every tone.

#include "../Source/BridgeOutput.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
    constexpr double rate = 48000.0, level = 0.5;
    constexpr int block = 480;    // 10 ms, like Windows' normal mode
    const double twoPi = juce::MathConstants<double>::twoPi;

    struct Result { double levelDb, differenceDb, worstDb; };

    double toDb (double ratio) { return 20.0 * std::log10 (juce::jmax (ratio, 1.0e-12)); }

    // Compares a result with the perfect copy: over the whole run and per 10 ms.
    Result compare (const std::vector<double>& out, const std::vector<double>& perfect, size_t skip)
    {
        double errorSum = 0, outSum = 0, perfectSum = 0, worst = 0;
        const auto toneRms = level / std::sqrt (2.0);

        for (auto start = skip; start + block <= out.size(); start += block)
        {
            double blockError = 0;

            for (auto i = start; i < start + block; ++i)
            {
                const auto e = out[i] - perfect[i];
                blockError += e * e;
                outSum += out[i] * out[i];
                perfectSum += perfect[i] * perfect[i];
            }

            errorSum += blockError;
            worst = std::max (worst, std::sqrt (blockError / block));
        }

        const auto count = (double) (out.size() - skip);
        return { toDb (std::sqrt (outSum / perfectSum)), toDb (std::sqrt (errorSum / count) / toneRms), toDb (worst / toneRms) };
    }

    std::vector<float> makeTone (double frequency, int length)
    {
        std::vector<float> tone ((size_t) length);

        for (int i = 0; i < length; ++i)
            tone[(size_t) i] = (float) (level * std::sin (twoPi * frequency * i / rate));

        return tone;
    }

    Result converter (double frequency, double drift)
    {
        const auto ratio = 1.0 + drift;
        const auto in = makeTone (frequency, 480000);
        const auto outLength = (int) ((in.size() - 200) / ratio);
        std::vector<float> converted ((size_t) outLength);
        juce::LagrangeInterpolator interpolator;
        interpolator.process (ratio, in.data(), converted.data(), outLength);

        // The perfect copy at the same speed. Its start (the converter's own delay) is
        // found by fitting; its level is the original level.
        const auto w = twoPi * frequency * ratio / rate;
        double cc = 0, ss = 0, cs = 0, yc = 0, ys = 0;

        for (int n = 1000; n < outLength; ++n)
        {
            const auto c = std::cos (w * n), s = std::sin (w * n), y = (double) converted[(size_t) n];
            cc += c * c; ss += s * s; cs += c * s; yc += y * c; ys += y * s;
        }

        const auto det = cc * ss - cs * cs;
        const auto a = (yc * ss - ys * cs) / det, b = (ys * cc - yc * cs) / det, amplitude = std::sqrt (a * a + b * b);
        std::vector<double> out ((size_t) outLength), perfect ((size_t) outLength);

        for (int n = 0; n < outLength; ++n)
        {
            out[(size_t) n] = converted[(size_t) n];
            perfect[(size_t) n] = level * (a * std::cos (w * n) + b * std::sin (w * n)) / amplitude;
        }

        return compare (out, perfect, 1000);
    }

    Result skipping (double frequency, double drift)
    {
        const auto in = makeTone (frequency, 480000);
        const auto fade = juce::jmin (BridgeOutput::slipFadeSamples, block - 10);
        const auto start = juce::jmax (3, (block - fade) / 2);

        std::vector<double> out, perfect;
        std::vector<float> blockOut (block);
        double owed = 0.0;
        int position = 0;

        while (position + block + 2 < (int) in.size())
        {
            // The FIFO gains drift * block samples per block; skip one each time a whole one built up.
            owed += drift * block;
            int slip = 0;

            if (owed >= 1.0)
            {
                slip = 1;
                owed -= 1.0;
            }

            BridgeOutput::copyWithSlip (in.data() + position, blockOut.data(), block, slip);

            for (int i = 0; i < block; ++i)
            {
                // Where a perfect copy reads: whole samples, sliding by one during the skip.
                const auto shift = slip == 0 || i < start ? 0.0
                                 : i < start + fade       ? slip * (double) (i - start + 1) / (fade + 1)
                                                          : (double) slip;

                out.push_back (blockOut[(size_t) i]);
                perfect.push_back (level * std::sin (twoPi * frequency * (position + i + shift) / rate));
            }

            position += block + slip;
        }

        return compare (out, perfect, 0);
    }
}

int main()
{
    const double frequencies[] = { 1000, 5000, 8000, 10000, 12000, 14000, 16000, 18000, 20000 };
    bool passed = true;

    for (const auto ppm : { 20.0, 50.0, 100.0 })
    {
        std::printf ("\n=== Clocks %.0f ppm apart (a skip %.1f times per second)\n", ppm, ppm * 1.0e-6 * rate);
        std::printf ("            ---- converter (different rates) ----   ---- unconverted + skips (same rate) ----\n");
        std::printf ("  tone      level   difference  worst 10 ms         level   difference  worst 10 ms\n");

        for (const auto f : frequencies)
        {
            const auto c = converter (f, ppm * 1.0e-6);
            const auto s = skipping (f, ppm * 1.0e-6);
            const auto ok = std::abs (s.levelDb) < 0.05 && s.differenceDb < c.differenceDb && s.worstDb < c.worstDb;
            passed = passed && ok;

            std::printf ("  %5.0f Hz %6.2f dB %7.1f dB %7.1f dB         %6.2f dB %7.1f dB %7.1f dB   %s\n",
                         f, c.levelDb, c.differenceDb, c.worstDb, s.levelDb, s.differenceDb, s.worstDb, ok ? "ok" : "WORSE");
        }
    }

    std::printf ("\ndifference = everything that is not a perfect copy, in dB below the tone (lower is better)\n");
    std::printf ("Unconverted copy keeps the level and beats the converter everywhere: %s\n", passed ? "YES" : "NO");
    return passed ? 0 : 1;
}
