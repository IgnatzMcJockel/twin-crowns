// Checks CrownsEngine at every factor: signal order, bypass, trims, block handling, stability, no
// allocation, also when the factor changes. The reference chain below uses the same MoT and
// half-band code, so it pins the wiring, not the DSP. Matching MoT itself is the null test's job.
// Run with `make test`.

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <random>
#include <vector>

#include "CrownsEngine.h"

static long allocations = 0;

void* operator new (std::size_t size)
{
    ++allocations;

    if (void* memory = std::malloc (size))
        return memory;

    throw std::bad_alloc();
}

void operator delete (void* memory) noexcept { std::free (memory); }
void operator delete (void* memory, std::size_t) noexcept { std::free (memory); }

static int failures = 0;
static constexpr double sampleRate = 48000.0;
static int currentLog2 = 2; // the factor checkAll is testing

// Default controls at the factor under test.
static crowns::EngineParams base()
{
    crowns::EngineParams p;
    p.oversampling = (float) currentLog2;
    return p;
}

static void check (const char* name, bool ok, double value)
{
    std::printf ("  %-48s %12.6g  %s\n", name, value, ok ? "ok" : "FAIL");
    failures += ok ? 0 : 1;
}

static std::vector<float> testSignal (int length)
{
    std::minstd_rand random (1);
    std::uniform_real_distribution<float> noise (-0.02f, 0.02f);
    std::vector<float> x ((size_t) length);

    for (int n = 0; n < length; ++n)
    {
        const double t = n / sampleRate;
        const double decay = std::exp (-3.0 * std::fmod (t, 0.25));
        const double note = std::sin (6.283185307 * 110.0 * t) + 0.5 * std::sin (6.283185307 * 1320.0 * t);
        x[(size_t) n] = (float) (0.2 * decay * note) + noise (random);
    }

    return x;
}

static std::vector<float> render (const crowns::EngineParams& params, const std::vector<float>& in, int blockSize)
{
    auto engine = std::make_unique<crowns::CrownsEngine>();
    engine->prepare (sampleRate);
    std::vector<float> out (in.size());

    for (size_t start = 0; start < in.size(); start += (size_t) blockSize)
    {
        const int n = (int) std::min ((size_t) blockSize, in.size() - start);
        engine->process (params, &in[start], &out[start], n);
    }

    return out;
}

// One pedal channel of the chain, written out sample by sample.
template <int Log2Factor>
struct ReferenceChannel
{
    explicit ReferenceChannel (bool hiGain, const crowns::ChannelParams& p) : dsp (hiGain)
    {
        const double osRate = sampleRate * (double) (1 << Log2Factor);
        dsp.prepareLinear (osRate, sampleRate, sampleRate);
        dsp.prepareClip (osRate);
        dsp.setDrive (p.drive);
        dsp.setTone (p.tone);
        dsp.setVolume (p.volume);
        dsp.setPresence (p.presence);
        dsp.setClippingMode ((int) p.mode);
        dsp.setSupplyVoltage (9.0);
    }

    float process (float x)
    {
        const double dry = (double) x;
        double oversampled[1 << Log2Factor];
        os.up (dsp.processStage1 (dry), oversampled);

        for (auto& sample : oversampled)
            sample = dsp.processClip (dsp.processPreOs (sample));

        const double wet = dsp.processPost (os.down (oversampled));
        return (float) (dry + 1.0 * (wet - dry));
    }

    monarch::MonarchChannel dsp;
    crowns::HalfBandOversampler<Log2Factor> os;
};

enum class Order { redThenYellow, yellowThenRed, yellowOnly };

template <int Log2Factor>
static std::vector<float> reference (const crowns::EngineParams& p, const std::vector<float>& in, Order order)
{
    static constexpr float cal = crowns::CrownsEngine::circuitVoltsPerFS;
    auto red = std::make_unique<ReferenceChannel<Log2Factor>> (true, p.red);
    auto yellow = std::make_unique<ReferenceChannel<Log2Factor>> (false, p.yellow);
    std::vector<float> out (in.size());

    for (size_t n = 0; n < in.size(); ++n)
    {
        float x = in[n] * (1.0f * cal);

        if (order == Order::redThenYellow)
            x = yellow->process (red->process (x));
        else if (order == Order::yellowThenRed)
            x = red->process (yellow->process (x));
        else
            x = yellow->process (x);

        out[n] = x * (1.0f / cal);
    }

    return out;
}

static double worstDifference (const std::vector<float>& a, const std::vector<float>& b, size_t first = 0)
{
    double worst = 0.0;

    for (size_t n = first; n < a.size(); ++n)
        worst = std::max (worst, std::abs ((double) a[n] - (double) b[n]));

    return worst;
}

static crowns::EngineParams bothOff()
{
    auto p = base();
    p.red.on = 0.0f;
    p.yellow.on = 0.0f;
    return p;
}

static void checkRamp()
{
    crowns::LinearRamp ramp;
    ramp.reset (sampleRate, 0.005);
    ramp.setCurrentAndTargetValue (1.0f);
    ramp.setTargetValue (0.0f);

    int steps = 0;
    float value = 1.0f;

    while (ramp.isSmoothing())
    {
        value = ramp.getNextValue();
        ++steps;
    }

    check ("ramp steps at 48 kHz", steps == 240, steps);
    check ("ramp ends exactly on target", value == 0.0f, value);
}

template <int Log2Factor>
static void checkChain (int expectedLatency)
{
    using Engine = crowns::CrownsEngine;
    const auto in = testSignal (48000);

    auto latencyProbe = std::make_unique<Engine>();
    latencyProbe->prepare (sampleRate);
    std::vector<float> probeOut (in.size());
    latencyProbe->process (base(), in.data(), probeOut.data(), 64);
    check ("reported latency, samples", latencyProbe->getLatencySamples() == expectedLatency,
           latencyProbe->getLatencySamples());

    auto p = base();
    const auto both = render (p, in, 128);
    const auto redThenYellow = reference<Log2Factor> (p, in, Order::redThenYellow);
    const auto yellowThenRed = reference<Log2Factor> (p, in, Order::yellowThenRed);

    check ("equals written-out Red -> Yellow", worstDifference (both, redThenYellow) == 0.0,
           worstDifference (both, redThenYellow));
    check ("differs from Yellow -> Red", worstDifference (both, yellowThenRed) > 1.0e-3,
           worstDifference (both, yellowThenRed));

    p.red.on = 0.0f;
    const auto yellowOnly = render (p, in, 128);
    const auto yellowReference = reference<Log2Factor> (p, in, Order::yellowOnly);

    check ("Red off: equals Yellow alone", worstDifference (yellowOnly, yellowReference) == 0.0,
           worstDifference (yellowOnly, yellowReference));

    auto busy = base();
    busy.yellow.mode = 2.0f;
    busy.red.mode = 0.0f;
    busy.yellow.presence = 0.7f;
    const auto blocks128 = render (busy, in, 128);
    double worstBlock = 0.0;

    for (int blockSize : { 1, 7, 64, 511, 512, 513, 2000 })
        worstBlock = std::max (worstBlock, worstDifference (render (busy, in, blockSize), blocks128));

    check ("block size changes nothing", worstBlock == 0.0, worstBlock);

    std::vector<float> inPlace = in;
    auto engine = std::make_unique<Engine>();
    engine->prepare (sampleRate);

    for (size_t start = 0; start < inPlace.size(); start += 128)
    {
        const int n = (int) std::min ((size_t) 128, inPlace.size() - start);
        engine->process (busy, &inPlace[start], &inPlace[start], n);
    }

    check ("in == out buffer changes nothing", worstDifference (inPlace, blocks128) == 0.0,
           worstDifference (inPlace, blocks128));
}

template <int Log2Factor>
static void checkBypass()
{
    const auto in = testSignal (24000);

    const auto dry = render (bothOff(), in, 128);
    check ("both off: passes the input", worstDifference (dry, in) < 1.0e-6, worstDifference (dry, in));

    auto p = base();
    p.red.on = 0.0f;
    const auto on = render (p, in, 128);

    // Switch Yellow off on a block boundary and compare with the crossfade written out.
    constexpr size_t toggle = 94 * 128;
    auto engine = std::make_unique<crowns::CrownsEngine>();
    engine->prepare (sampleRate);
    std::vector<float> out (in.size());

    for (size_t start = 0; start < in.size(); start += 128)
    {
        auto params = p;
        params.yellow.on = start < toggle ? 1.0f : 0.0f;
        engine->process (params, &in[start], &out[start], (int) std::min ((size_t) 128, in.size() - start));
    }

    double worstFade = 0.0;
    crowns::LinearRamp wet;
    wet.reset (sampleRate, 0.005);
    wet.setCurrentAndTargetValue (1.0f);
    wet.setTargetValue (0.0f);

    for (size_t n = toggle; n < toggle + 240; ++n)
    {
        const double g = (double) wet.getNextValue();
        worstFade = std::max (worstFade, std::abs ((double) out[n] - ((double) dry[n] + g * ((double) on[n] - (double) dry[n]))));
    }

    check ("off: fade follows the 240-step ramp", worstFade < 1.0e-5, worstFade);
    check ("off: dry from the end of the ramp on", worstDifference (out, dry, toggle + 240) < 1.0e-6,
           worstDifference (out, dry, toggle + 240));

    double biggestJump = 0.0;

    for (size_t n = toggle - 2000; n < toggle + 2000; ++n)
        biggestJump = std::max (biggestJump, std::abs ((double) out[n + 1] - (double) out[n]));

    // The output is `on` before the fade and `dry` after it, and dry is the brighter of the two, so
    // a click has to stand out against the larger of their own sample-to-sample jumps.
    double normalJump = 0.0;

    for (size_t n = toggle - 2000; n < toggle + 2000; ++n)
    {
        normalJump = std::max (normalJump, std::abs ((double) on[n + 1] - (double) on[n]));
        normalJump = std::max (normalJump, std::abs ((double) dry[n + 1] - (double) dry[n]));
    }

    check ("off: no click, jump vs normal", biggestJump < 1.25 * normalJump, biggestJump / normalJump);
}

template <int Log2Factor>
static void checkTrims()
{
    const auto in = testSignal (24000);
    const auto unity = render (bothOff(), in, 128);

    auto p = bothOff();
    p.outputTrim = 6.0f;
    const auto louder = render (p, in, 128);
    const float expected = std::pow (10.0f, 6.0f * 0.05f);
    double worst = 0.0;

    for (size_t n = 0; n < in.size(); ++n)
        worst = std::max (worst, std::abs ((double) louder[n] - (double) unity[n] * (double) expected));

    check ("output trim +6 dB from the first sample", worst < 1.0e-5, worst);

    p = bothOff();
    p.inputTrim = -6.0f;
    p.outputTrim = 6.0f;
    const auto cancelled = render (p, in, 128);
    check ("input -6, output +6 cancel", worstDifference (cancelled, in) < 1.0e-5, worstDifference (cancelled, in));

    auto engine = std::make_unique<crowns::CrownsEngine>();
    engine->prepare (sampleRate);
    const std::vector<float> ones (256, 1.0f);
    std::vector<float> out (256);
    auto trim = bothOff();
    engine->process (trim, ones.data(), out.data(), 128);
    trim.outputTrim = 6.0f;
    engine->process (trim, ones.data() + 128, out.data() + 128, 128);

    check ("trim change ramps, first step small", out[128] < 1.02f, out[128]);
}

template <int Log2Factor>
static void checkStability()
{
    using Engine = crowns::CrownsEngine;

    auto engine = std::make_unique<Engine>();
    engine->prepare (sampleRate);
    auto in = testSignal (4800);

    for (auto& sample : in)
        sample *= 5.0f; // past full scale

    std::vector<float> out (in.size());
    double peak = 0.0;
    bool finite = true;

    const long before = allocations;

    for (int mode : { 0, 1, 2 })
        for (float supply : { 0.0f, 1.0f, 2.0f })
          for (float factor : { (float) currentLog2, 0.0f, 3.0f, 1.0f, -1.0f, 9.0f })
            for (float knob : { 0.0f, 0.5f, 1.0f, -3.0f, 3.0f })
            {
                auto p = base();
                p.supplyVoltage = supply;
                p.oversampling = factor; // switches mid-stream, out of range included
                p.inputTrim = 18.0f;
                p.yellow.mode = (float) mode;
                p.red.mode = (float) mode;
                p.yellow.drive = p.red.drive = knob;
                p.yellow.tone = p.red.presence = knob;
                p.yellow.volume = p.red.volume = 1.0f;
                p.red.on = knob > 0.0f ? 1.0f : 0.0f;
                engine->process (p, in.data(), out.data(), (int) in.size());

                for (float sample : out)
                {
                    finite = finite && std::isfinite (sample);
                    peak = std::max (peak, (double) std::abs (sample));
                }
            }

    const long used = allocations - before;

    check ("hot input, every mode and knob: finite", finite, peak);
    check ("hot input: output bounded", peak < 16.0, peak);
    check ("process() allocates nothing, also switching factor", used == 0, (double) used);
}

template <int Log2Factor>
static void checkAll (int expectedLatency)
{
    std::printf ("--- %dx\n", 1 << Log2Factor);
    currentLog2 = Log2Factor;
    checkChain<Log2Factor> (expectedLatency);
    checkBypass<Log2Factor>();
    checkTrims<Log2Factor>();
    checkStability<Log2Factor>();
}

int main()
{
    checkRamp();
    checkAll<0> (0);
    checkAll<1> (6);
    checkAll<2> (9);
    checkAll<3> (10);

    std::printf ("%s\n", failures == 0 ? "ALL PASSED" : "FAILED");
    return failures == 0 ? 0 : 1;
}
