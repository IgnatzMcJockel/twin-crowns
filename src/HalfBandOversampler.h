/*
   Port of parts of the JUCE framework: juce_dsp/processors/juce_Oversampling.cpp and
   juce_dsp/filter_design/juce_FilterDesign.cpp, at JUCE commit
   3cc224c3e84d6acbabf62358ba1ccbf133cd2095.

   JUCE is Copyright (c) Raw Material Software Limited. JUCE is dual-licensed under the AGPLv3
   and the commercial JUCE licence; this port uses it under the AGPLv3, like the rest of
   Twin Crowns.

   Changes from JUCE: reduced to the half-band polyphase IIR path MoT uses, without JUCE's
   classes, buffers or SIMD; filter design and sample loops are kept numerically identical.
*/

#pragma once

#include <array>
#include <cassert>
#include <cmath>
#include <complex>

namespace crowns
{

/**
 * One half-band filter of JUCE's polyphase IIR oversampling stage, as two cascades of allpasses.
 * Port of FilterDesign<double>::designIIRLowpassHalfBandPolyphaseAllpassMethod plus the sample loops
 * of Oversampling2TimesPolyphaseIIR, at the JUCE commit MoT pins (3cc224c).
 *
 * Coefficients are held as JUCE holds them: the direct path's first, then the delayed path's.
 */
class PolyphaseHalfBand
{
public:
    static constexpr int maxCoefficients = 8;

    /** Allocation-free, but runs the full design: call from instantiate, never from run(). */
    void design (double normalisedTransitionWidth, double stopbandAmplitudedB)
    {
        const double wt = twoPi * normalisedTransitionWidth;
        const double ds = std::pow (10.0, stopbandAmplitudedB * 0.05);

        const double k = std::pow (std::tan ((pi - wt) / 4), 2.0);
        const double kp = std::sqrt (1.0 - k * k);
        const double e = (1 - std::sqrt (kp)) / (1 + std::sqrt (kp)) * 0.5;
        const double q = e + 2 * std::pow (e, 5.0) + 15 * std::pow (e, 9.0) + 150 * std::pow (e, 13.0);

        const double k1 = ds * ds / (1 - ds * ds);
        int n = (int) std::lround (std::ceil (std::log (k1 * k1 / 16) / std::log (q)));

        if (n % 2 == 0)
            ++n;

        if (n == 1)
            n = 3;

        const int N = (n - 1) / 2;
        assert (N <= maxCoefficients);
        std::array<double, maxCoefficients> ai {};

        for (int i = 1; i <= N; ++i)
        {
            double num = 0.0;
            double delta = 1.0;
            int m = 0;

            while (std::abs (delta) > 1e-100)
            {
                delta = std::pow (-1, m) * std::pow (q, m * (m + 1)) * std::sin ((2 * m + 1) * pi * i / (double) n);
                num += delta;
                m++;
            }

            num *= 2 * std::pow (q, 0.25);

            double den = 0.0;
            delta = 1.0;
            m = 1;

            while (std::abs (delta) > 1e-100)
            {
                delta = std::pow (-1, m) * std::pow (q, m * m) * std::cos (m * twoPi * i / (double) n);
                den += delta;
                ++m;
            }

            den = 1 + 2 * den;

            const double wi = num / den;
            const double api = std::sqrt ((1 - wi * wi * k) * (1 - wi * wi / k)) / (1 + wi * wi);

            ai[(size_t) i - 1] = (1 - api) / (1 + api);
        }

        numCoefficients = 0;

        for (int i = 0; i < N; i += 2)
            coefficients[(size_t) numCoefficients++] = ai[(size_t) i];

        numDirect = numCoefficients;

        for (int i = 1; i < N; i += 2)
            coefficients[(size_t) numCoefficients++] = ai[(size_t) i];

        reset();
    }

    void reset() noexcept
    {
        state.fill (0.0);
        delay = 0.0;
    }

    /** One input sample → the two samples at twice the rate. No gain factor, as in JUCE. */
    inline void up (double x, double& even, double& odd) noexcept
    {
        even = runPath (x, 0, numDirect);
        odd = runPath (x, numDirect, numCoefficients);
    }

    /** Two samples at twice the rate → one output sample. */
    inline double down (double even, double odd) noexcept
    {
        const double direct = runPath (even, 0, numDirect);
        const double delayed = runPath (odd, numDirect, numCoefficients);
        const double out = (delay + direct) * 0.5;
        delay = delayed;
        return out;
    }

    /** Phase delay near DC in samples of this stage's rate, measured at the frequency JUCE uses. */
    double getLatencyInSamples() const
    {
        constexpr double frequency = 0.0001;
        const auto z1 = std::exp (std::complex<double> (0.0, -twoPi * frequency));
        const auto z2 = z1 * z1;

        auto allpass = [&z2] (double a) { return (a + z2) / (1.0 + a * z2); };

        std::complex<double> direct { 1.0 };
        std::complex<double> delayed { z1 };

        for (int n = 0; n < numDirect; ++n)
            direct *= allpass (coefficients[(size_t) n]);

        for (int n = numDirect; n < numCoefficients; ++n)
            delayed *= allpass (coefficients[(size_t) n]);

        return -std::arg (direct + delayed) / (frequency * twoPi);
    }

    int getNumCoefficients() const noexcept { return numCoefficients; }
    int getNumDirect() const noexcept { return numDirect; }
    double getCoefficient (int n) const noexcept { return coefficients[(size_t) n]; }

private:
    static constexpr double pi = 3.141592653589793238;
    static constexpr double twoPi = 2.0 * pi;

    inline double runPath (double x, int first, int last) noexcept
    {
        for (int n = first; n < last; ++n)
        {
            const double alpha = coefficients[(size_t) n];
            const double y = alpha * x + state[(size_t) n];
            state[(size_t) n] = x - alpha * y;
            x = y;
        }

        return x;
    }

    std::array<double, maxCoefficients> coefficients {};
    std::array<double, maxCoefficients> state {};
    double delay { 0.0 };
    int numCoefficients { 0 };
    int numDirect { 0 };
};

/**
 * Replaces juce::dsp::Oversampling<double> (1, log2, filterHalfBandPolyphaseIIR, true) for one mono
 * channel. Works per sample: JUCE's state doesn't depend on block boundaries, apart from
 * JUCE_SNAP_TO_ZERO, which is Intel-only and not ported. Log2Factor 0 passes samples through, as MoT
 * runs 1× without an oversampler.
 */
template <int Log2Factor>
class HalfBandOversampler
{
public:
    static_assert (Log2Factor >= 0 && Log2Factor <= 3, "MoT offers 1×, 2×, 4× and 8×");

    static constexpr int factor = 1 << Log2Factor;

    /** Designs the filters, so construct it in instantiate. */
    HalfBandOversampler()
    {
        for (int n = 0; n < Log2Factor; ++n)
        {
            // float on purpose, as in JUCE's ctor: the widths are 0.05000000074…, not 0.05.
            const float twUp = 0.10f * (n == 0 ? 0.5f : 1.0f);
            const float twDown = 0.12f * (n == 0 ? 0.5f : 1.0f);

            stages[(size_t) n].up.design (twUp, -90.0f + 10.0f * (float) n);
            stages[(size_t) n].down.design (twDown, -75.0f + 10.0f * (float) n);
        }
    }

    void reset() noexcept
    {
        for (auto& stage : stages)
        {
            stage.up.reset();
            stage.down.reset();
        }
    }

    /** In base-rate samples, as Oversampling::getLatencyInSamples with integer latency off. */
    double getLatencyInSamples() const
    {
        double latency = 0.0;
        int order = 1;

        for (const auto& stage : stages)
        {
            order *= 2;
            latency += (stage.up.getLatencyInSamples() + stage.down.getLatencyInSamples()) / (double) order;
        }

        return latency;
    }

    /** One base-rate sample → `factor` oversampled samples. */
    inline void up (double x, double* out) noexcept { upFrom<0> (x, out); }

    /** `factor` oversampled samples → one base-rate sample. */
    inline double down (const double* in) noexcept { return downFrom<0> (in); }

    const PolyphaseHalfBand& getUpFilter (int stage) const noexcept { return stages[(size_t) stage].up; }
    const PolyphaseHalfBand& getDownFilter (int stage) const noexcept { return stages[(size_t) stage].down; }

private:
    struct Stage
    {
        PolyphaseHalfBand up, down;
    };

    // JUCE runs each stage over the whole block before the next. Each stage only sees the previous
    // one's output in order, so going depth first per sample gives the same numbers.
    template <int N>
    inline void upFrom (double x, double* out) noexcept
    {
        if constexpr (N == Log2Factor)
        {
            out[0] = x;
        }
        else
        {
            double even, odd;
            stages[(size_t) N].up.up (x, even, odd);
            upFrom<N + 1> (even, out);
            upFrom<N + 1> (odd, out + (factor >> (N + 1)));
        }
    }

    template <int N>
    inline double downFrom (const double* in) noexcept
    {
        if constexpr (N == Log2Factor)
        {
            return in[0];
        }
        else
        {
            const double even = downFrom<N + 1> (in);
            const double odd = downFrom<N + 1> (in + (factor >> (N + 1)));
            return stages[(size_t) N].down.down (even, odd);
        }
    }

    std::array<Stage, (size_t) Log2Factor> stages;
};

} // namespace crowns
