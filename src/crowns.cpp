// LV2 entry points. All signal processing lives in CrownsEngine; this file only maps ports onto it.
// The Makefile sets CROWNS_URI.

#include <lv2/core/lv2.h>

#include <array>
#include <cstdint>
#include <new>

#include "CrownsEngine.h"

#ifndef CROWNS_URI
 #error "CROWNS_URI must be defined"
#endif

namespace
{

// Must match lv2:index in ttl/twin-crowns.ttl.in. Red comes first, as in the signal flow.
enum Port : uint32_t
{
    audioIn,
    audioOut,
    redOn,
    redVolume,
    redDrive,
    redTone,
    redPresence,
    redMode,
    yellowOn,
    yellowVolume,
    yellowDrive,
    yellowTone,
    yellowPresence,
    yellowMode,
    inputTrim,
    outputTrim,
    supplyVoltage,
    latency,
    oversampling,
    numPorts
};

struct Plugin
{
    explicit Plugin (double rate) : sampleRate (rate) {}

    float control (Port port) const noexcept { return *ports[port]; }

    crowns::ChannelParams channelParams (Port first) const noexcept
    {
        crowns::ChannelParams p;
        p.on = control (first);
        p.volume = control ((Port) (first + 1));
        p.drive = control ((Port) (first + 2));
        p.tone = control ((Port) (first + 3));
        p.presence = control ((Port) (first + 4));
        p.mode = control ((Port) (first + 5));
        return p;
    }

    crowns::CrownsEngine engine;
    std::array<float*, numPorts> ports {};
    double sampleRate;
};

LV2_Handle instantiate (const LV2_Descriptor*, double sampleRate, const char*, const LV2_Feature* const*)
{
    return new (std::nothrow) Plugin (sampleRate);
}

void connectPort (LV2_Handle instance, uint32_t port, void* data)
{
    if (port < numPorts)
        static_cast<Plugin*> (instance)->ports[port] = static_cast<float*> (data);
}

void activate (LV2_Handle instance)
{
    auto* plugin = static_cast<Plugin*> (instance);
    plugin->engine.prepare (plugin->sampleRate);
}

void run (LV2_Handle instance, uint32_t numSamples)
{
    auto* plugin = static_cast<Plugin*> (instance);

    crowns::EngineParams params;
    params.yellow = plugin->channelParams (yellowOn);
    params.red = plugin->channelParams (redOn);
    params.inputTrim = plugin->control (inputTrim);
    params.outputTrim = plugin->control (outputTrim);
    params.supplyVoltage = plugin->control (supplyVoltage);
    params.oversampling = plugin->control (oversampling);

    plugin->engine.process (params, plugin->ports[audioIn], plugin->ports[audioOut], (int) numSamples);

    *plugin->ports[latency] = (float) plugin->engine.getLatencySamples();
}

void cleanup (LV2_Handle instance)
{
    delete static_cast<Plugin*> (instance);
}

const LV2_Descriptor descriptor = {
    CROWNS_URI, instantiate, connectPort, activate, run, nullptr, cleanup, nullptr
};

} // namespace

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor (uint32_t index)
{
    return index == 0 ? &descriptor : nullptr;
}
