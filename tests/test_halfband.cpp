// Checks the half-band port. The expected coefficients and latencies come from a separate Python
// copy of JUCE's design function, so they catch transcription slips, not a shared misreading. The
// response checks measure actual filtering and don't depend on that copy. Run with `make test`.

#include <algorithm>
#include <complex>
#include <cstdio>
#include <vector>

#include "HalfBandOversampler.h"

static int failures = 0;

static void check (const char* name, bool ok, double value)
{
    std::printf ("%-44s %12.6g  %s\n", name, value, ok ? "ok" : "FAIL");
    failures += ok ? 0 : 1;
}

static constexpr double twoPi = 2.0 * 3.141592653589793238;

// Single-bin DFT, frequency in cycles per sample.
static std::complex<double> bin (const std::vector<double>& x, size_t first, double frequency)
{
    std::complex<double> sum { 0.0 };

    for (size_t n = first; n < x.size(); ++n)
        sum += x[n] * std::exp (std::complex<double> (0.0, -twoPi * frequency * (double) (n - first)));

    return sum * 2.0 / (double) (x.size() - first);
}

static double levelDb (const std::vector<double>& x, size_t first, double frequency)
{
    return 20.0 * std::log10 (std::abs (bin (x, first, frequency)) + 1.0e-30);
}

static std::vector<double> sine (size_t length, double frequency)
{
    std::vector<double> x (length);

    for (size_t n = 0; n < length; ++n)
        x[n] = std::sin (twoPi * frequency * (double) n);

    return x;
}

template <int Log2Factor>
static std::vector<double> upsample (const std::vector<double>& in)
{
    crowns::HalfBandOversampler<Log2Factor> os;
    const int factor = 1 << Log2Factor;
    std::vector<double> out (in.size() * (size_t) factor);

    for (size_t n = 0; n < in.size(); ++n)
        os.up (in[n], &out[n * (size_t) factor]);

    return out;
}

template <int Log2Factor>
static std::vector<double> downsample (const std::vector<double>& in)
{
    crowns::HalfBandOversampler<Log2Factor> os;
    const int factor = 1 << Log2Factor;
    std::vector<double> out (in.size() / (size_t) factor);

    for (size_t n = 0; n < out.size(); ++n)
        out[n] = os.down (&in[n * (size_t) factor]);

    return out;
}

static void checkCoefficients (const char* name, const crowns::PolyphaseHalfBand& filter,
                               const std::vector<double>& expected, double latency)
{
    const int total = filter.getNumCoefficients();
    double worst = total == (int) expected.size() ? 0.0 : 1.0;

    if (worst == 0.0)
    {
        // Expected values are in design order a[0], a[1], …; the filter holds direct then delayed.
        int stored = 0;

        for (size_t i = 0; i < expected.size(); i += 2)
            worst = std::max (worst, std::abs (filter.getCoefficient (stored++) - expected[i]));

        for (size_t i = 1; i < expected.size(); i += 2)
            worst = std::max (worst, std::abs (filter.getCoefficient (stored++) - expected[i]));
    }

    std::printf ("%s: %d coefficients, %d direct\n", name, total, filter.getNumDirect());
    check ("  worst coefficient error", worst < 1.0e-13, worst);
    check ("  latency, own rate", std::abs (filter.getLatencyInSamples() - latency) < 1.0e-3,
           filter.getLatencyInSamples());
}

template <int Log2Factor>
static void checkRoundTrip (double expectedLatency)
{
    constexpr size_t length = 48000;
    constexpr size_t settle = 4800;
    constexpr double frequency = 100.0 / 48000.0;

    crowns::HalfBandOversampler<Log2Factor> os;
    const auto in = sine (length, frequency);
    std::vector<double> out (length);

    for (size_t n = 0; n < length; ++n)
    {
        double oversampled[1 << Log2Factor];
        os.up (in[n], oversampled);
        out[n] = os.down (oversampled);
    }

    const auto ratio = bin (out, settle, frequency) / bin (in, settle, frequency);
    const double delay = -std::arg (ratio) / (twoPi * frequency);

    check ("  round trip gain at 100 Hz, dB", std::abs (20.0 * std::log10 (std::abs (ratio))) < 1.0e-3,
           20.0 * std::log10 (std::abs (ratio)));
    check ("  round trip delay, base samples", std::abs (delay - expectedLatency) < 2.0e-2, delay);
    check ("  reported latency, base samples", std::abs (os.getLatencyInSamples() - expectedLatency) < 1.0e-3,
           os.getLatencyInSamples());
    std::printf ("  two in series, rounded as in MoT: %ld\n", std::lround (2.0 * os.getLatencyInSamples()));
}

int main()
{
    std::printf ("--- 2x\n");
    crowns::HalfBandOversampler<1> os2;

    checkCoefficients ("stage 0 up", os2.getUpFilter (0),
                       { 0.045728147624082692, 0.16808754441358362, 0.33250110957929974, 0.50448574772579069,
                         0.66320202004105133, 0.80378086620290357, 0.93385580308838401 },
                       3.3005);
    checkCoefficients ("stage 0 down", os2.getDownFilter (0),
                       { 0.054217526478844993, 0.19679797212217914, 0.3830873308979566, 0.57313641503021207,
                         0.74872094746739482, 0.91429371091299194 },
                       2.9741);
    checkRoundTrip<1> (3.1373);

    {
        const size_t length = 96000, settle = 9600;
        const auto up = upsample<1> (sine (length / 2, 20000.0 / 48000.0));
        const auto down = downsample<1> (sine (length, 30000.0 / 96000.0));

        check ("  20 kHz passes upsampler, dB", std::abs (levelDb (up, settle, 20000.0 / 96000.0)) < 0.1,
               levelDb (up, settle, 20000.0 / 96000.0));
        // The image of 20 kHz at this rate is 48 − 20 = 28 kHz.
        check ("  image at 28 kHz, dB", levelDb (up, settle, 28000.0 / 96000.0) < -90.0,
               levelDb (up, settle, 28000.0 / 96000.0));
        check ("  30 kHz alias at 18 kHz, dB", levelDb (down, settle / 2, 18000.0 / 48000.0) < -75.0,
               levelDb (down, settle / 2, 18000.0 / 48000.0));
    }

    std::printf ("--- 4x\n");
    crowns::HalfBandOversampler<2> os4;

    checkCoefficients ("stage 1 up", os4.getUpFilter (1),
                       { 0.054230780337565128, 0.19969957772571781, 0.3987969709870654, 0.62109684265303322,
                         0.8629178114945566 },
                       2.8013);
    checkCoefficients ("stage 1 down", os4.getDownFilter (1),
                       { 0.070765950045145978, 0.25785308161769949, 0.51316757868020335, 0.81731735655648086 },
                       2.3801);
    checkRoundTrip<2> (4.4327);

    {
        const size_t length = 192000, settle = 19200;
        const auto up = upsample<2> (sine (length / 4, 20000.0 / 48000.0));
        const auto down = downsample<2> (sine (length, 30000.0 / 192000.0));
        const auto downStage1 = downsample<2> (sine (length, 100000.0 / 192000.0));

        check ("  20 kHz passes upsampler, dB", std::abs (levelDb (up, settle, 20000.0 / 192000.0)) < 0.1,
               levelDb (up, settle, 20000.0 / 192000.0));
        // Stage 0 sets the image at 48 − 20 = 28 kHz, stage 1 the one at 96 − 20 = 76 kHz.
        check ("  image at 28 kHz, dB", levelDb (up, settle, 28000.0 / 192000.0) < -90.0,
               levelDb (up, settle, 28000.0 / 192000.0));
        check ("  image at 76 kHz, dB", levelDb (up, settle, 76000.0 / 192000.0) < -80.0,
               levelDb (up, settle, 76000.0 / 192000.0));
        check ("  30 kHz alias at 18 kHz, dB", levelDb (down, settle / 4, 18000.0 / 48000.0) < -75.0,
               levelDb (down, settle / 4, 18000.0 / 48000.0));
        // 100 kHz folds to 96 − 100 → 4 kHz in stage 1, which then passes stage 0 untouched.
        check ("  100 kHz alias at 4 kHz, dB", levelDb (downStage1, settle / 4, 4000.0 / 48000.0) < -65.0,
               levelDb (downStage1, settle / 4, 4000.0 / 48000.0));
    }

    // JUCE's 8× just adds stage 2 by the same rule. Its coefficients aren't pinned here; the null
    // test checks 8× against MoT.
    std::printf ("--- 8x\n");
    checkRoundTrip<3> (4.9514);

    {
        const size_t length = 384000, settle = 38400;
        const auto up = upsample<3> (sine (length / 8, 20000.0 / 48000.0));

        check ("  20 kHz passes upsampler, dB", std::abs (levelDb (up, settle, 20000.0 / 384000.0)) < 0.1,
               levelDb (up, settle, 20000.0 / 384000.0));
        check ("  image at 28 kHz, dB", levelDb (up, settle, 28000.0 / 384000.0) < -90.0,
               levelDb (up, settle, 28000.0 / 384000.0));
        check ("  image at 76 kHz, dB", levelDb (up, settle, 76000.0 / 384000.0) < -80.0,
               levelDb (up, settle, 76000.0 / 384000.0));
        // Stage 2 sets the image at 192 − 20 = 172 kHz.
        check ("  image at 172 kHz, dB", levelDb (up, settle, 172000.0 / 384000.0) < -70.0,
               levelDb (up, settle, 172000.0 / 384000.0));
    }

    std::printf ("--- 1x\n");
    {
        crowns::HalfBandOversampler<0> os1;
        double sample[1];
        os1.up (0.25, sample);
        check ("  passes through, no latency", sample[0] == 0.25 && os1.down (sample) == 0.25
                                                   && os1.getLatencyInSamples() == 0.0, sample[0]);
    }

    std::printf ("%s\n", failures == 0 ? "ALL PASSED" : "FAILED");
    return failures == 0 ? 0 : 1;
}
