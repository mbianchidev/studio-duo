# Bundled devices

Studio Duo lists thirteen built-in processors in the normal processor catalog.
They use the same insert, state, automation, latency compensation, editor, live
processing, and offline render paths as hosted plugins.

## Utility processors

Add these devices from **Plugin Manager** or a track's insert `+`, then open
their editor from the insert row. The device editor exposes the controls below;
the generic parameter panel and automation lanes use normalized values from
0 to 1.

| Device | Purpose and controls |
| --- | --- |
| Parametric EQ (`studio.device.eq`) | A single bell band, not a multiband EQ. Frequency 20 Hz to 20 kHz, gain -18 to +18 dB, and Q 0.2 to 10; the usable frequency is bounded by the sample rate. |
| Compressor (`studio.device.compressor`) | Threshold, ratio, attack, release, makeup gain and dry/wet mix. An enabled external sidechain can drive its detector. |
| True-Peak Limiter (`studio.device.limiter`) | Ceiling -12 to 0 dBFS, release and a true-peak switch, enabled by default. Its reported processing latency participates in delay compensation. |
| Algorithmic Reverb (`studio.device.reverb`) | Room size, damping, stereo width and dry/wet mix. It reports a four-second effects tail for offline exports. |
| Noise Gate (`studio.device.gate`) | Threshold, attack, release and attenuation range; supports an external sidechain detector. |
| Gain (`studio.device.gain`) | Insert gain from -24 to +24 dB, separate from the track fader. |
| Polarity (`studio.device.polarity`) | Invert both audio channels without changing their timing. |
| Delay (`studio.device.delay`) | Sample-offset alignment delay, with a 0 to 96,000-sample control bounded by a two-second buffer. It is not a tempo-synced feedback echo. |
| Tuner (`studio.device.tuner`) | Pass-through monophonic pitch analysis. Detected frequency is available through the processor metering API; there are no parameter controls and no dedicated pitch display in the current editor. |
| Signal Generator (`studio.device.generator`) | Replaces input with a sine wave, white noise or pink noise. Waveform values are 0, 1 and 2 respectively; sine frequency and output level are adjustable. |

Start with a low monitor level before using the Signal Generator; its default
output level is -18 dBFS. Compressor and gate sidechains use the ordinary
[routing controls](user-guide.md#mix-and-route-tracks). The same devices can be
placed in [serial or parallel racks](user-guide.md#plugin-racks-and-parallel-effects)
or used for [clip-range FX](user-guide.md#clip-fx-bounce-and-freeze).

The standalone VST3, Audio Unit and CLAP targets below cover the drum instrument
and two amps, not all ten utility processors.

## Metal Drum Composer

`studio.device.drum-composer` is a deterministic MIDI instrument intended for
the default Studio Duo Metal drum map. It synthesizes a usable basic kit, so no
sample library or external file is required.

**Add Track > Drum performance track** creates this instrument, an armed track,
and a mapped MIDI clip with twelve mouse/keyboard pads. Pad sound assignments
select notes/articulations from the drum map; they do not replace the
instrument's synthesis or require sample files. The [live drum-pad
workflow](user-guide.md#live-drum-pads) records ordinary MIDI through the same
graph as hardware input and the piano roll.

The instrument responds to normal MIDI:

- note velocity controls hit level through the automatable velocity curve;
- the default kick, snare, and tom base/variant notes drive deterministic
  round-robin timbre and pan changes;
- saved round-robin hints are converted to the map's ordinary variant notes by
  the scheduler;
- closed, pedal, and open hi-hat notes use the MIDI CC4 foot-controller
  convention: 0 is fully open, 127 is fully closed, and pedal state uses an
  intermediate value;
- crash, china, ride, and hi-hat groups respond to mapped choke notes and
  later hits in the same group.

Automatable parameters are Main, Kick, Snare, Toms, Cymbals, Room, Kit tuning,
and Velocity curve. Resetting the processor resets its voice noise and
round-robin counters, so the same MIDI/state produces the same render.

The five stereo buses are Main, Kick, Snare, Toms, and Cymbals. Main contains
the complete kit. To create stems inside Studio Duo:

1. Put Metal Drum Composer last on an instrument track.
2. Create aux or bus tracks.
3. Choose **ROUTING** > **ADD** > **Processor output** and route each auxiliary
   bus.
4. Lower Main when only the stems should reach the mix.

Processor-output routes persist the source insert and bus index and use the
normal send, automation, PDC, and render graph.

## Guitar Amp and Bass Amp

`studio.device.guitar-amp` and `studio.device.bass-amp` contain nonlinear
preamp stages, three-band tone shaping, presence, cabinet mix, and output gain.
The guitar device provides a saturation control; the bass device provides a
clean/distorted drive blend and preserves low-frequency weight separately.

Both amps start with an embedded cabinet and therefore work on a clean
installation:

- Guitar Amp: **Embedded Modern 4x12**
- Bass Amp: **Embedded Tight 8x10**

The editor's **Load cabinet IR...** action accepts mono or stereo WAV, AIFF, or
FLAC audio. A candidate must decode successfully, contain finite non-silent
samples, use one or two channels, and remain at or below 8,192 samples when
converted to 384 kHz (about 21.3 ms). Every rejection returns a visible error
and leaves the active kernel unchanged. **Use embedded cabinet** is the only
automatic/default cabinet path.

IR decoding, validation, resampling, normalization, partition FFT creation, and
kernel publication happen outside `processBlock`. Processing uses fixed
128-sample partitions, preallocated history, and a lock-free triple-buffered
kernel publication scheme. The amps report 128 samples of latency and the
active IR tail.

Custom IR samples are embedded in the opaque insert state along with all
normalized parameters. Reopening does not require the original file. Invalid
state fails explicitly through the standard failed/missing processor path.
Exports capture the live processor state before rebuilding the offline graph,
so a newly loaded cabinet is rendered even before the next project save.

## Plugin targets

The in-app processors and plugin builds compile the same DSP sources:

- individual VST3 builds for Metal Drum Composer, Guitar Amp, and Bass Amp;
- individual Audio Unit builds on macOS;
- `StudioDuoBundledDevices.clap`, which exports all three CLAP plugins.

The plugin targets keep the same parameters, buses, state, latency, tail, MIDI,
and embedded cabinet behavior. Studio Duo's host implements
[DAWproject interchange](dawproject.md), including insert descriptors, supported
state payloads and device automation. Review its compatibility report:
native rack topology and vendor preset-container portability have explicit
limits; exchange does not guarantee that another DAW can load every device.
