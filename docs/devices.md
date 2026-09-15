# Bundled devices

Studio Duo lists thirteen built-in processors in the normal processor catalog.
They use the same insert, state, automation, latency compensation, editor, live
processing, and offline render paths as hosted plugins.

## Metal Drum Composer

`studio.device.drum-composer` is a deterministic MIDI instrument intended for
the default Studio Duo Metal drum map. It synthesizes a usable basic kit, so no
sample library or external file is required.

The instrument responds to normal MIDI:

- note velocity controls hit level through the automatable velocity curve;
- the default kick, snare, and tom base/variant notes drive deterministic
  round-robin timbre and pan changes;
- saved round-robin hints are converted to the map's ordinary variant notes by
  the scheduler;
- closed, pedal, and open hi-hat notes use CC4 foot-control state;
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
and embedded cabinet behavior. DAWproject interchange is not implemented yet.
