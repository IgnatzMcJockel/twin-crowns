#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <tuple>

#if defined(__x86_64__)
 #include <xmmintrin.h>
#endif

#include "HalfBandOversampler.h"
#include "dsp/MonarchChannel.h"

namespace crowns
{

/** Port of juce::SmoothedValue<float, ValueSmoothingTypes::Linear> at MoT's JUCE commit. */
class LinearRamp
{
public:
    void reset (double sampleRate, double rampLengthInSeconds) noexcept
    {
        stepsToTarget = (int) std::floor (rampLengthInSeconds * sampleRate);
        setCurrentAndTargetValue (target);
    }

    void setCurrentAndTargetValue (float newValue) noexcept
    {
        target = currentValue = newValue;
        countdown = 0;
    }

    void setTargetValue (float newValue) noexcept
    {
        if (approximatelyEqual (newValue, target))
            return;

        if (stepsToTarget <= 0)
        {
            setCurrentAndTargetValue (newValue);
            return;
        }

        target = newValue;
        countdown = stepsToTarget;
        step = (target - currentValue) / (float) countdown;
    }

    float getNextValue() noexcept
    {
        if (! isSmoothing())
            return target;

        --countdown;

        if (isSmoothing())
            currentValue += step;
        else
            currentValue = target;

        return currentValue;
    }

    bool isSmoothing() const noexcept { return countdown > 0; }
    float getTargetValue() const noexcept { return target; }

private:
    static bool approximatelyEqual (float a, float b) noexcept
    {
        if (! (std::isfinite (a) && std::isfinite (b)))
            return a == b;

        const auto diff = std::abs (a - b);

        return diff <= std::numeric_limits<float>::min()
            || diff <= std::numeric_limits<float>::epsilon() * std::max (std::abs (a), std::abs (b));
    }

    float currentValue { 0.0f }, target { 0.0f }, step { 0.0f };
    int countdown { 0 };
    int stepsToTarget { 0 };
};

/** Port of juce::ScopedNoDenormals: flush-to-zero for the scope, then the previous mode again. */
class ScopedNoDenormals
{
public:
    ScopedNoDenormals() noexcept
    {
       #if defined(__x86_64__)
        previous = _mm_getcsr();
        _mm_setcsr (previous | 0x8040u);
       #elif defined(__aarch64__)
        asm volatile ("mrs %0, fpcr" : "=r" (previous));
        const uint64_t flushToZero = previous | (uint64_t { 1 } << 24);
        asm volatile ("msr fpcr, %0" : : "r" (flushToZero));
       #else
        #error "Only x86_64 and aarch64 are supported"
       #endif
    }

    ~ScopedNoDenormals() noexcept
    {
       #if defined(__x86_64__)
        _mm_setcsr (previous);
       #elif defined(__aarch64__)
        asm volatile ("msr fpcr, %0" : : "r" (previous));
       #endif
    }

    ScopedNoDenormals (const ScopedNoDenormals&) = delete;
    ScopedNoDenormals& operator= (const ScopedNoDenormals&) = delete;

private:
   #if defined(__x86_64__)
    unsigned int previous { 0 };
   #else
    uint64_t previous { 0 };
   #endif
};

/** Knob values of one pedal channel, as they arrive from the control ports. */
struct ChannelParams
{
    float on { 1.0f };
    float volume { 0.5f };
    float drive { 0.5f };
    float tone { 0.5f };
    float presence { 0.0f };
    float mode { 1.0f }; // 0 Boost, 1 Overdrive, 2 Distortion
};

struct EngineParams
{
    ChannelParams yellow, red;
    float inputTrim { 0.0f };     // dB
    float outputTrim { 0.0f };    // dB
    float supplyVoltage { 0.0f }; // 0, 1, 2 → 9, 12, 18 V
    float oversampling { 2.0f };  // 0, 1, 2, 3 → 1×, 2×, 4×, 8×, MoT's "Oversampling (Live)"
};

/**
 * The mono signal path of MoT's PluginProcessor without JUCE or LV2: processBlock,
 * processPedalChannel, pushParams, prepareToPlay and updateOversampling, for one strip. Knows nothing
 * about ports, so it can be tested on its own.
 */
class CrownsEngine
{
public:
    // Fitted to the captures; private to MoT's PluginProcessor.h, so it has to be copied.
    static constexpr float circuitVoltsPerFS = 0.87f;

    // A host block longer than this is split, each part acting as one of MoT's blocks.
    static constexpr int maxBlockSize = 512;

    static constexpr int maxLog2Factor = 3;

    // Our processPedalChannel relies on both; fail here if a MoT update changes them.
    static_assert (monarch::MonarchChannel::preAtBaseRate && monarch::MonarchChannel::postAtBaseRate,
                   "the channel split no longer matches MoT");

    /** Designs every oversampler, so construct it in instantiate. */
    CrownsEngine()
    {
        // Two oversamplers in series, even when a channel is bypassed, rounded as MoT does.
        latencyByLog2[1] = (int) std::lround (2.0 * std::get<1> (red.os).getLatencyInSamples());
        latencyByLog2[2] = (int) std::lround (2.0 * std::get<2> (red.os).getLatencyInSamples());
        latencyByLog2[3] = (int) std::lround (2.0 * std::get<3> (red.os).getLatencyInSamples());
    }

    /** Called from activate, like MoT's prepareToPlay. The first process() prepares the channels,
        because only it knows the oversampling factor. */
    void prepare (double newSampleRate)
    {
        constexpr double rampSeconds = 0.005; // MoT's bypassRampSeconds and trimRampSeconds

        sampleRate = newSampleRate;

        for (auto* channel : { &yellow, &red })
            channel->wet.reset (sampleRate, rampSeconds);

        inTrimGain.reset (sampleRate, rampSeconds);
        outTrimGain.reset (sampleRate, rampSeconds);

        activeLog2 = -1;
        firstBlock = true;
    }

    /** What MoT reports for the active factor, 0 at 1×. Valid after the first process(). */
    int getLatencySamples() const noexcept { return activeLog2 < 0 ? 0 : latencyByLog2[(size_t) activeLog2]; }

    /** Real-time safe, also when the factor changes. `in` and `out` may be the same buffer, as LV2
        allows. */
    void process (const EngineParams& params, const float* in, float* out, int numSamples)
    {
        const ScopedNoDenormals noDenormals;

        while (numSamples > 0)
        {
            const int blockSize = std::min (numSamples, maxBlockSize);
            processBlock (params, in, out, blockSize);
            in += blockSize;
            out += blockSize;
            numSamples -= blockSize;
        }
    }

private:
    struct Channel
    {
        explicit Channel (bool hiGain) : dsp (hiGain) {}

        monarch::MonarchChannel dsp;
        // One per factor, all designed up front, so switching allocates nothing. 1× passes through.
        std::tuple<HalfBandOversampler<0>, HalfBandOversampler<1>, HalfBandOversampler<2>, HalfBandOversampler<3>> os;
        LinearRamp wet; // bypass crossfade: 1 = active, 0 = bypassed (dry)
    };

    static float clampKnob (float value) noexcept { return std::min (1.0f, std::max (0.0f, value)); }

    static float trimToGain (float decibels) noexcept
    {
        return std::pow (10.0f, std::min (18.0f, std::max (-18.0f, decibels)) * 0.05f);
    }

    /** MoT's updateOversampling: on a new factor, fresh oversamplers and both channels prepared
        again at the new rate. Leaves the bypass and trim ramps alone, as MoT does. */
    void updateOversampling (int log2)
    {
        if (log2 == activeLog2)
            return;

        activeLog2 = log2;
        const double osRate = sampleRate * (double) (1 << log2);

        for (auto* channel : { &yellow, &red })
        {
            channel->dsp.prepareLinear (osRate, sampleRate, sampleRate);
            channel->dsp.prepareClip (osRate);
            std::apply ([] (auto&... os) { (os.reset(), ...); }, channel->os);
        }
    }

    void pushChannelParams (Channel& channel, const ChannelParams& p, double supplyV)
    {
        channel.dsp.setDrive (clampKnob (p.drive));
        channel.dsp.setTone (clampKnob (p.tone));
        channel.dsp.setVolume (clampKnob (p.volume));
        channel.dsp.setPresence (clampKnob (p.presence));
        channel.dsp.setClippingMode (std::min (2, std::max (0, (int) p.mode)));
        channel.dsp.setSupplyVoltage (supplyV);

        const float wetTarget = p.on > 0.5f ? 1.0f : 0.0f;

        if (firstBlock)
            channel.wet.setCurrentAndTargetValue (wetTarget);
        else
            channel.wet.setTargetValue (wetTarget);
    }

    void processBlock (const EngineParams& params, const float* in, float* out, int numSamples)
    {
        static constexpr double kSupplyVolts[] = { 9.0, 12.0, 18.0 };
        const double supplyV = kSupplyVolts[std::min (2, std::max (0, (int) params.supplyVoltage))];
        const int log2 = std::min (maxLog2Factor, std::max (0, (int) params.oversampling));

        // MoT prepares in prepareToPlay, before the first pushParams, and in processBlock after each.
        // The order matters: preparing resets the drive-keyed shelves until the next setDrive.
        if (firstBlock)
            updateOversampling (log2);

        pushChannelParams (yellow, params.yellow, supplyV);
        pushChannelParams (red, params.red, supplyV);
        updateOversampling (log2);

        const float inTarget = trimToGain (params.inputTrim) * circuitVoltsPerFS;
        const float outTarget = trimToGain (params.outputTrim) / circuitVoltsPerFS;

        if (firstBlock)
        {
            inTrimGain.setCurrentAndTargetValue (inTarget);
            outTrimGain.setCurrentAndTargetValue (outTarget);
            firstBlock = false;
        }
        else
        {
            inTrimGain.setTargetValue (inTarget);
            outTrimGain.setTargetValue (outTarget);
        }

        for (int n = 0; n < numSamples; ++n)
            buffer[(size_t) n] = in[n] * inTrimGain.getNextValue();

        // Series: Red → Yellow, the real pedal's signal flow.
        processPedalChannel (red, numSamples);
        processPedalChannel (yellow, numSamples);

        for (int n = 0; n < numSamples; ++n)
            out[n] = buffer[(size_t) n] * outTrimGain.getNextValue();
    }

    void processPedalChannel (Channel& channel, int numSamples)
    {
        // Fully bypassed → skip the DSP and the oversampler entirely.
        if (! (channel.wet.isSmoothing() || channel.wet.getTargetValue() > 0.0f))
            return;

        switch (activeLog2)
        {
            case 0:  processWet<0> (channel, numSamples); break;
            case 1:  processWet<1> (channel, numSamples); break;
            case 2:  processWet<2> (channel, numSamples); break;
            default: processWet<3> (channel, numSamples); break;
        }

        for (int n = 0; n < numSamples; ++n)
        {
            const auto i = (size_t) n;
            const double g = (double) channel.wet.getNextValue();
            buffer[i] = (float) (scratchDry[i] + g * (scratchWet[i] - scratchDry[i]));
        }
    }

    template <int Log2Factor>
    void processWet (Channel& channel, int numSamples)
    {
        auto& dsp = channel.dsp;
        auto& os = std::get<Log2Factor> (channel.os);

        for (int n = 0; n < numSamples; ++n)
        {
            const auto i = (size_t) n;
            scratchDry[i] = (double) buffer[i];

            if constexpr (Log2Factor == 0)
            {
                // MoT's 1× branch: the whole channel at the base rate, without an oversampler.
                (void) os;
                scratchWet[i] = dsp.processPost (dsp.processClip (dsp.processPre (scratchDry[i])));
            }
            else
            {
                double oversampled[(size_t) 1 << Log2Factor];
                os.up (dsp.processStage1 (scratchDry[i]), oversampled);

                for (auto& sample : oversampled)
                    sample = dsp.processClip (dsp.processPreOs (sample));

                scratchWet[i] = dsp.processPost (os.down (oversampled));
            }
        }
    }

    Channel yellow { false };
    Channel red { true };
    LinearRamp inTrimGain, outTrimGain;

    std::array<float, maxBlockSize> buffer {};
    std::array<double, maxBlockSize> scratchDry {}, scratchWet {};

    std::array<int, maxLog2Factor + 1> latencyByLog2 {};
    double sampleRate { 48000.0 };
    int activeLog2 { -1 };
    bool firstBlock { true };
};

} // namespace crowns
