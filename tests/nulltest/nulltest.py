#!/usr/bin/env python3
"""Null test: real MoT against our LV2 plugin through lv2apply, expecting bit-identical output.

Needs the reference from setup_reference.sh and a build, so run it with `make nulltest`. Static cases
go through lv2apply with fixed controls. Moving cases change controls during the render, which
lv2apply can't do, so they load the plugin's .so directly and act as the host. Deliberate
mismatches run first, to show the comparison can see a difference.
"""
import array
import ctypes
import math
import os
import random
import re
import struct
import subprocess
import sys

CLONE = os.path.expanduser("~/mot-ref")
PEDAL_RENDER = f"{CLONE}/build/PedalRender_artefacts/Release/PedalRender"
WORK = f"{CLONE}/null"
MOT_SIGNAL = f"{CLONE}/analysis/test_signal_48k.wav"
NAME = "twin-crowns"
URI = f"urn:ignatzmcjockel:{NAME}"
LATENCY = {0: 0, 1: 6, 2: 9, 3: 10}
SAMPLE_RATE = 48000
BOOST, OVERDRIVE, DISTORTION = 0, 1, 2

failures = 0
env_lv2 = dict(os.environ, LV2_PATH=f"{os.getcwd()}/build/lv2:/usr/lib/lv2")


def write_wav(path, samples):
    raw = samples.tobytes()
    header = struct.pack("<HHIIHH", 3, 1, SAMPLE_RATE, SAMPLE_RATE * 4, 4, 32)
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(raw)) + b"WAVEfmt " + struct.pack("<I", 16) + header
                + b"data" + struct.pack("<I", len(raw)) + raw)


def read_float_wav(path):
    with open(path, "rb") as f:
        data = f.read()

    pos, bits = 12, None

    while pos + 8 <= len(data):
        chunk, size = struct.unpack("<4sI", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + size]
        if chunk == b"fmt ":
            bits = struct.unpack("<HHIIHH", body[:16])[5]
        if chunk == b"data":
            assert bits == 32, f"{path} has {bits} bits"
            return array.array("f", body[:len(body) // 4 * 4])
        pos += 8 + size + (size & 1)

    raise SystemExit(f"no data chunk in {path}")


def generate_notes():
    """3 s of decaying notes plus noise."""
    random.seed(1)
    x = array.array("f")

    for n in range(3 * SAMPLE_RATE):
        t = n / SAMPLE_RATE
        note = math.sin(2 * math.pi * 110 * t) + 0.5 * math.sin(2 * math.pi * 1320 * t)
        x.append(0.2 * math.exp(-3 * (t % 0.25)) * note + random.uniform(-0.02, 0.02))

    return x


def run_mot(path, source, log2, yellow, params, events=(), block_size=512):
    env = dict(os.environ, MOT_PARAMS=",".join(params), MOT_EVENTS=",".join(events), MOT_BLOCK=str(block_size))
    subprocess.run([PEDAL_RENDER, source, path, *map(str, yellow), "live", str(log2)],
                   env=env, check=True, capture_output=True)


def run_plugin(path, source, log2, controls):
    args = ["lv2apply", "-i", source, "-o", path]

    for symbol, value in dict(controls, oversampling=log2).items():
        args += ["-c", symbol, str(value)]

    subprocess.run(args + [URI], env=env_lv2, check=True, capture_output=True)


def compare(name, expect_identical, source, mot_log2, plugin_log2, yellow, red=None, supply=0, in_trim=0.0,
            out_trim=0.0, plugin_overrides=None):
    """yellow and red are (drive, tone, volume, presence, mode); red None means Red is bypassed."""
    params = [f"supply_voltage={supply}", f"input_trim={in_trim}", f"output_trim={out_trim}"]
    controls = dict(zip(("yellow_drive", "yellow_tone", "yellow_volume", "yellow_presence", "yellow_mode"), yellow))
    controls.update(red_on=0, supply_voltage=supply, input_trim=in_trim, output_trim=out_trim)

    if red:
        params.append("bypass_red=0")
        params += [f"{knob}_red={value}" for knob, value in
                   zip(("drive", "tone", "volume", "presence", "clipping_mode"), red)]
        controls.update(zip(("red_drive", "red_tone", "red_volume", "red_presence", "red_mode"), red))
        controls["red_on"] = 1

    controls.update(plugin_overrides or {})

    mot_path, plugin_path = f"{WORK}/mot.wav", f"{WORK}/plugin.wav"
    run_mot(mot_path, source, mot_log2, yellow, params)
    run_plugin(plugin_path, source, plugin_log2, controls)

    report(name, expect_identical, read_float_wav(mot_path), read_float_wav(plugin_path))


class Lv2Descriptor(ctypes.Structure):
    pass


Lv2Descriptor._fields_ = [
    ("uri", ctypes.c_char_p),
    ("instantiate", ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.POINTER(Lv2Descriptor), ctypes.c_double,
                                     ctypes.c_char_p, ctypes.c_void_p)),
    ("connect_port", ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p)),
    ("activate", ctypes.CFUNCTYPE(None, ctypes.c_void_p)),
    ("run", ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_uint32)),
    ("deactivate", ctypes.c_void_p),
    ("cleanup", ctypes.CFUNCTYPE(None, ctypes.c_void_p)),
    ("extension_data", ctypes.c_void_p),
]


def read_ports(bundle):
    """Port symbols and defaults from the built TTL, so a wrong index there fails the comparison."""
    with open(f"{bundle}/{NAME}.ttl") as f:
        ttl = f.read()

    pattern = r'lv2:index (\d+) ;\s+lv2:symbol "(\w+)" ;\s+lv2:name "[^"]+"(?: ;\s+lv2:default ([-\d.]+))?'
    return {symbol: (int(index), float(default or 0)) for index, symbol, default in re.findall(pattern, ttl)}


def run_plugin_moving(source, log2, block_size, events, initial):
    """Plays the host: `events` is [(block, symbol, value)], sorted by block."""
    bundle = f"{os.getcwd()}/build/lv2/{NAME}.lv2"
    library = ctypes.CDLL(f"{bundle}/{NAME}.so")
    library.lv2_descriptor.restype = ctypes.POINTER(Lv2Descriptor)
    library.lv2_descriptor.argtypes = [ctypes.c_uint32]
    descriptor_pointer = library.lv2_descriptor(0)
    descriptor = descriptor_pointer.contents

    ports = read_ports(bundle)
    controls = (ctypes.c_float * len(ports))()
    features = (ctypes.c_void_p * 1)(None)
    handle = descriptor.instantiate(descriptor_pointer, SAMPLE_RATE, bundle.encode() + b"/",
                                    ctypes.cast(features, ctypes.c_void_p))
    assert handle, "instantiate failed"

    for symbol, (index, default) in ports.items():
        controls[index] = default
        descriptor.connect_port(handle, index, ctypes.addressof(controls) + index * 4)

    for symbol, value in dict(initial, oversampling=log2).items():
        controls[ports[symbol][0]] = value

    descriptor.activate(handle)

    length = len(source)
    audio_in = (ctypes.c_float * length).from_buffer_copy(source.tobytes())
    audio_out = (ctypes.c_float * length)()
    pending = list(events)

    for block, start in enumerate(range(0, length, block_size)):
        while pending and pending[0][0] <= block:
            controls[ports[pending[0][1]][0]] = pending[0][2]
            pending.pop(0)

        descriptor.connect_port(handle, ports["in"][0], ctypes.addressof(audio_in) + start * 4)
        descriptor.connect_port(handle, ports["out"][0], ctypes.addressof(audio_out) + start * 4)
        descriptor.run(handle, min(block_size, length - start))

    assert controls[ports["latency"][0]] == LATENCY[int(controls[ports["oversampling"][0]])]
    descriptor.cleanup(handle)
    return array.array("f", bytes(audio_out))


def mot_event(block, block_size, symbol, value):
    """One of our control changes as PedalRender's `sample:id=value`."""
    channel, _, knob = symbol.partition("_")

    if symbol == "oversampling":
        return f"{block * block_size}:oversampling_realtime={value}"

    if channel not in ("yellow", "red"):
        return f"{block * block_size}:{symbol}={value}"

    if knob == "on":
        return f"{block * block_size}:bypass_{channel}={1 - value}"

    return f"{block * block_size}:{'clipping_mode' if knob == 'mode' else knob}_{channel}={value}"


def compare_moving(name, expect_identical, source_path, log2, block_size, events, plugin_events=None):
    """Both start from the defaults with both channels on, then apply the same control changes."""
    mot_path = f"{WORK}/mot.wav"
    run_mot(mot_path, source_path, log2, (.5, .5, .5, 0, OVERDRIVE), ["bypass_red=0"],
            [mot_event(block, block_size, symbol, value) for block, symbol, value in events], block_size)
    plugin = run_plugin_moving(read_float_wav(source_path), log2, block_size,
                               events if plugin_events is None else plugin_events, {})
    report(f"{name}, {len(events)} changes, block {block_size}", expect_identical, read_float_wav(mot_path), plugin)


def scripted_events():
    """Footswitch use, in blocks of 128: off, back on after settling, and back on mid-ramp."""
    return [(100, "yellow_on", 0), (300, "yellow_on", 1),
            (500, "red_on", 0), (700, "red_on", 1),
            (900, "yellow_on", 0), (901, "yellow_on", 1),
            (1000, "red_on", 0), (1001, "red_on", 1),
            (1100, "yellow_on", 0), (1101, "red_on", 0), (1300, "red_on", 1), (1500, "yellow_on", 1),
            (1700, "yellow_mode", DISTORTION), (1800, "red_mode", BOOST), (1900, "yellow_drive", 0.9),
            (2000, "red_tone", 0.1), (2100, "yellow_volume", 0.8), (2200, "red_presence", 0.6),
            (2300, "supply_voltage", 2), (2400, "input_trim", 6), (2500, "output_trim", -3)]


def factor_switches():
    """Every factor, from and to each other, with footswitches and a knob in between."""
    return [(200, "oversampling", 3), (400, "oversampling", 0), (600, "oversampling", 1),
            (650, "yellow_on", 0), (700, "oversampling", 2), (701, "yellow_on", 1),
            (900, "oversampling", 0), (901, "red_drive", 0.9), (1100, "oversampling", 3),
            (1300, "oversampling", 1), (1500, "red_on", 0), (1600, "oversampling", 3), (1601, "red_on", 1),
            (1800, "oversampling", 2)]


def random_events(seed, total_blocks):
    """Every control at random, often only a block apart, so changes land during running ramps."""
    rng = random.Random(seed)
    on = {"yellow": 1, "red": 1}
    events = []
    block = 8

    while block < total_blocks:
        channel = rng.choice(("yellow", "red"))
        kind = rng.random()

        if kind < 0.35:
            on[channel] = 1 - on[channel]
            events.append((block, f"{channel}_on", on[channel]))
        elif kind < 0.7:
            knob = rng.choice(("drive", "tone", "volume", "presence"))
            events.append((block, f"{channel}_{knob}", rng.randrange(101) / 100))
        elif kind < 0.85:
            events.append((block, f"{channel}_mode", rng.randrange(3)))
        elif kind < 0.92:
            events.append((block, "supply_voltage", rng.randrange(3)))
        elif kind < 0.95:
            events.append((block, "oversampling", rng.randrange(4)))
        else:
            events.append((block, rng.choice(("input_trim", "output_trim")), rng.choice((-12, -6, -3, 0, 3, 6, 12))))

        block += rng.choice((1, 1, 2, 3, 5, 20, 80))

    return events


def report(name, expect_identical, mot, plugin):
    global failures

    assert len(mot) == len(plugin), (len(mot), len(plugin))

    differing = sum(a != b for a, b in zip(mot, plugin))
    peak = max(abs(a - b) for a, b in zip(mot, plugin))
    passed = (differing == 0) == expect_identical
    failures += 0 if passed else 1
    print(f"{'ok  ' if passed else 'FAIL'} {name:<52} peak diff {peak:.3e}, {differing} of {len(mot)} samples differ",
          flush=True)


def main():
    if not os.path.exists(PEDAL_RENDER):
        sys.exit("no reference build: run tests/nulltest/setup_reference.sh first")

    os.makedirs(WORK, exist_ok=True)
    notes = f"{WORK}/notes.wav"
    write_wav(notes, generate_notes())

    # 20 s of MoT's own signal, which is far harder than the notes. Starts 5 s in, as a slice of float.
    slice_path = f"{WORK}/mot_slice.wav"
    write_wav(slice_path, read_float_wav(MOT_SIGNAL)[5 * SAMPLE_RATE:25 * SAMPLE_RATE])

    yellow_od = (.5, .5, .5, 0, OVERDRIVE)

    print("--- deliberate mismatches, which must differ")
    compare("MoT 4x vs plugin 2x", False, notes, 2, 1, yellow_od)
    compare("MoT 2x vs plugin 4x", False, notes, 1, 2, yellow_od)
    compare("MoT Yellow only vs plugin with Red on", False, notes, 1, 1, yellow_od, plugin_overrides={"red_on": 1})
    compare("drive .5 vs .500001", False, notes, 1, 1, yellow_od, plugin_overrides={"yellow_drive": 0.500001})
    compare("9 V vs 18 V, Boost, hot", False, notes, 1, 1, (1.0, .5, 1.0, 0, BOOST), in_trim=12.0,
            plugin_overrides={"supply_voltage": 2})

    scripted = scripted_events()
    late = [(block + 1, symbol, value) if (block, symbol) == (300, "yellow_on") else (block, symbol, value)
            for block, symbol, value in scripted]
    compare_moving("re-engage one block late", False, slice_path, 1, 128, scripted, late)
    compare_moving("plugin ignores the changes", False, slice_path, 1, 128, scripted, [])
    compare_moving("factor switch one block late", False, slice_path, 2, 128, factor_switches(),
                   [(block + (symbol == "oversampling"), symbol, value) for block, symbol, value in factor_switches()])

    for log2 in (0, 1, 2, 3):
        print(f"--- {1 << log2}x, which must be identical")

        def identical(name, *args, source=notes, **kwargs):
            compare(name, True, source, log2, log2, *args, **kwargs)

        identical("Yellow OD, defaults", yellow_od)
        identical("Yellow Boost", (.5, .5, .5, 0, BOOST))
        identical("Yellow Dist, drive .8", (.8, .5, .5, 0, DISTORTION))
        identical("Yellow OD, drive .2, tone .8, vol .7, presence .5", (.2, .8, .7, .5, OVERDRIVE))
        identical("Red OD -> Yellow OD", yellow_od, yellow_od)
        identical("Red Boost -> Yellow Dist", (.6, .4, .6, .2, DISTORTION), (.3, .7, .5, 0, BOOST))
        identical("Red Dist -> Yellow Boost, 18 V", (.7, .5, .5, 0, BOOST), (.9, .5, .5, 0, DISTORTION), supply=2)
        identical("both OD, 12 V, in +6 dB, out -3 dB", yellow_od, yellow_od, supply=1, in_trim=6.0, out_trim=-3.0)
        identical("Yellow Boost, hot, in +18 dB", (1.0, .5, 1.0, 0, BOOST), in_trim=18.0)
        identical("MoT signal, Yellow OD", yellow_od, source=slice_path)
        identical("MoT signal, Red Dist -> Yellow Boost, 18 V", (.7, .5, .5, 0, BOOST), (.9, .5, .5, 0, DISTORTION),
                  supply=2, source=slice_path)

        compare_moving("moving: footswitches and knobs", True, slice_path, log2, 128, scripted)
        compare_moving("moving: switching the factor", True, slice_path, log2, 128, factor_switches())

        for seed, block_size in ((1, 128), (2, 128), (3, 64), (4, 512)):
            events = random_events(seed + 10 * log2, 20 * SAMPLE_RATE // block_size)
            compare_moving(f"moving: random seed {seed}", True, slice_path, log2, block_size, events)

    print("ALL PASSED" if failures == 0 else f"{failures} FAILED")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
