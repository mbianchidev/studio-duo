# Studio Duo user guide

Studio Duo is under active development. The current application includes the
Phase 1 vertical slice, Phase 2 professional tracking and editing workflows, and
the complete Phase 3 mixer and plugin platform, Phase 4 MIDI composition and
DAWproject exchange, and Phase 5 mastering and release workflows.

## Current capabilities

- CoreAudio, ASIO, and WASAPI device selection
- Tempo and meter maps with jump or ramp changes
- Draggable named marker flags for labels, navigation, loops, and exports
- Persistent song-section placeholders on the timeline ruler
- Routed metronome with accents and subdivisions
- Punch, count-in, pre-roll, post-roll, and loop transport
- Sample-aligned lock-free multitrack audio recording
- Audio import, playback, arm, mute, solo, gain, and pan
- Clip move, split, trim, delete, fades, crossfades, and consolidation
- Deterministic transient detection and pitch-preserving stretch and warp
- Take lanes, playlists, comping, and linked multitrack editing
- Hardware reamp routing, calibration, polarity, and fine alignment
- Plugin tone paths that reference the original DI media
- Sandboxed VST3, Audio Unit, and CLAP discovery and processing
- Explicit ARA 2 compatibility and trusted in-process modes with recovery
  warnings and safe-disabled reopening
- Persistent plugin state, compatibility records, missing-plugin replacement,
  latency metadata, crash/timeout diagnostics, and per-insert reload
- Pre/post-fader sends, per-insert sidechains, parallel paths, auxes, nested
  buses, folders, VCAs, control room, hardware outputs, graph-routed input
  monitoring, and solo-safe routing
- Live, recorded, and retrospective MIDI with persisted notes and per-note
  pitch bend, poly pressure, channel pressure, timbre, and controller
  expression
- Piano-roll and metal drum lower editors with velocity, timing, duration,
  probability, and expression lanes
- Editable/importable drum maps, choke and cymbal metadata, foot control,
  round-robin hints, deterministic metal entry tools, pattern aliases,
  seeded humanization, and channel-filtered multi-output routing templates
- Cycle-safe MIDI routes through in-process and sandboxed instrument and
  MIDI-effect plugins
- Sample-accurate mixer, send, bundled-device, and plugin automation
- Parametric EQ, compressor, true-peak limiter, reverb, gate, gain, polarity,
  delay, tuner, and signal generator devices
- Bundled metal drum composition instrument with velocity, deterministic round
  robin, mapped cymbal/hi-hat behavior, and separate kit-piece outputs
- Bundled guitar and bass amps with real nonlinear/tone DSP, embedded cabinets,
  validated custom cabinet loading, persistent IR state, and automation
- Tone and mixer snapshots, level-matched A/B, stale detection, freeze, print,
  plugin-inclusive rendering, batch reports, and installed plug-in validation
- Versioned `.studioduo` packages, generation saves, and recovery points
- DAWproject 1.0 import/export with embedded media and plug-in state,
  official schema validation, scene preservation, and compatibility reports
- Dedicated album mastering with alternate mixes, sequencing, gaps, overlaps,
  fades, ISRC/release metadata, BS.1770/R128 loudness analysis, true peak,
  correlation, WAV/AIFF/FLAC/Ogg/MP3 exports, deterministic dither, and signed reports
- External licensed DDP encoder integration with sector/MCN validation,
  fileset checks, SHA-256 delivery manifests, and signed reports
- Content-addressed portable copies, relocatable package paths, transfer
  validation, and hash-based missing-media repair
- Configurable mix export with named-marker ranges, lossless and compressed
  formats, mono/stereo, bitrate/quality, normalization, and dither
- In-app update checks, verified background downloads, and user-controlled
  restart installation on macOS and Windows

## Essential controls

| Action | Shortcut |
| --- | --- |
| Play or pause | `Space` |
| Save | `Command/Ctrl+S` |
| Open | `Command/Ctrl+O` |
| Import audio | `Command/Ctrl+I` |
| Create MIDI clip at playhead | `Command/Ctrl+Shift+N` |
| Capture recent MIDI | `Command/Ctrl+Shift+M` |
| Undo | `Command/Ctrl+Z` |
| Redo | `Command/Ctrl+Shift+Z` |
| Copy and duplicate selected clip | `Command/Ctrl+C`, then `Command/Ctrl+V` |
| Split selected clip at playhead | `S` |
| Trim selected clip start to playhead | `[` |
| Trim selected clip end to playhead | `]` |
| Delete selected clip | `Delete` or `Backspace` |
| Piano-roll note create | `Enter` |
| Piano-roll note move | Arrow keys |
| Piano-roll note resize | `Shift+Left/Right` |
| Edit selected MIDI lane | `Alt+Up/Down` |
| Select all notes | `Command/Ctrl+A` while the editor is focused |
| Zoom timeline out or in | `Command/Ctrl+-` or `Command/Ctrl++` |
| Reset timeline zoom | `Command/Ctrl+0` |

Scroll the mouse wheel over the timeline canvas to zoom around the pointer.
Wheel or trackpad scrolling over the track-header column instead moves the
track list vertically. On macOS, pinch gestures continue to zoom the timeline.

## Start a session

When Studio Duo opens without a `.studioduo` project path, the startup hub
offers three immediate choices:

- **New Song** creates a blank song with a master track.
- **Create From Template** starts from the bundled Metal Tracking, Songwriting,
  or Mix Session layouts.
- **Open Existing Project** opens the project chooser.

The hub also lists up to 12 recent projects, newest edit first. Opening,
saving, or editing a saved project moves it to the top. Missing or inaccessible
entries are labeled clearly and include a **Remove** action; use **Open Existing
Project** if the package moved. Passing a `.studioduo` path to the application
opens it directly instead of showing the hub.

Click the Studio Duo logo in the top-left corner to leave the current project
and return to the startup hub. Unsaved or never-saved projects prompt for
**Save and Return**, **Return Without Saving**, or **Cancel**. Active recordings
must be stopped and finalized before returning to the hub.

After choosing a project:

1. Open **Settings** (gear icon) > **Audio / MIDI** and enable the required
   hardware inputs and outputs.
2. Add audio, instrument, or MIDI tracks with **Add Track** (plus icon). Import
   WAV, AIFF, FLAC, MP3, or Ogg Vorbis files with **Import Audio** (down-arrow
   icon), or select a MIDI/instrument track
   and use **NEW MIDI CLIP**.
3. Select a track to configure its input, mono or stereo capture, monitoring,
   volume, pan, color, inserts, and output.
4. Save the project as a `.studioduo` directory package before recording so new
   media is written below its `media/` directory.

Studio Duo restores the last working audio device shortly after its window
appears. The audio manager and MIDI discovery are deferred until then, rather
than running during window construction. On Windows, MIDI discovery is checked
in a separate process before the main app creates its audio manager; a failed
check leaves the window open with audio disabled and a diagnostic message.
On macOS, the system may request microphone access at that point. Use
**Settings** (gear icon) > **Audio / MIDI** to enable inputs or change the
active device. On
the first Windows launch, Studio Duo prefers a native ASIO driver over generic
compatibility wrappers, enables every hardware input, and uses the driver's
current sample rate and default buffer size. Before automatically opening an
ASIO driver, including a saved setup, a separate worker opens and closes that
setup with a ten-second deadline. A driver that crashes, hangs, or rejects the
setup is skipped rather than being loaded into the main app. It tries each
native ASIO driver before compatibility wrappers, so an unavailable legacy
driver does not hide a working interface. If no ASIO driver can start, Studio
Duo reports the fallback
in the status bar and opens shared Windows Audio so recording remains
available. Opening **Settings** (gear icon) > **Audio / MIDI** rescans all
backends and
selects one with input devices when the current backend is empty. When no audio
device is open, Settings starts with shared **Windows Audio** instead of
automatically retrying a possibly failing ASIO driver.
Under **Windows Audio**, the Input menu lists active capture endpoints such as
the Focusrite Windows device, the built-in microphone, and virtual
microphones. Under **ASIO**, select the Focusrite driver as the combined audio
device. A track's Input menu shows channels from the active audio device only;
Studio Duo uses one Windows audio device at a time, so Focusrite ASIO and the
Realtek microphone cannot appear as simultaneous track inputs.

After a project has been saved once, each edit refreshes its recovery point.
Opening the package restores newer unsaved recovery state and marks the project
with `*` so it can be saved normally. If the active saved generation is damaged,
Studio Duo opens a valid recovery point instead; stale or corrupt recovery data
is ignored with a warning when the saved project remains usable.

Monitoring is off by default to avoid accidental feedback. Stereo capture uses
the selected hardware input and the adjacent channel. Monitored input enters the
selected track before its inserts, then follows its fader, sends, buses, plugin
delay compensation, hardware routes, master, and control-room path.

## Manage tracks

The session sidebar and track-header context menu can duplicate or delete the
selected non-master track with full undo support. Duplicating a parent copies
its complete track family: clips, take lanes, inserts, input and mix settings,
playlist and comp state, and connected tone-path sends and owned returns. New
track, clip, insert, comp, route, and owned-return IDs prevent later edits from
aliasing the source family.

## Record and manage takes

Each timeline track header has speaker-mute, headphones-solo, and record-circle
controls, a compact horizontal dB fader, and an input dropdown for audio tracks.
The same input dropdown appears on mixer strips, so microphone, guitar, or other
active-device channels can be assigned without opening the inspector.
**Start Recording** (circle icon)
captures every armed audio parent into a separate, sample-aligned WAV. If no
track is armed, the selected audio track becomes the single recording target.

Press **Stop Recording** (square icon) or **Stop** (square icon) to finish every
active recording at the same audio callback boundary. The timeline draws a live
waveform from lock-free peak buckets while recording. Stopping flushes each WAV
before its clip is added; the inspector identifies the saved filename and the
status bar reports completion. The playhead stays at the recording stop
position.

Each completed pass creates grouped `v1`, `v2`, `v3`, and later child tracks
below the recorded parent. The whole multitrack pass is one undoable command.
Version tracks retain ordinary mute, solo, arm, split, trim, move, and delete
behavior. Recording preserves whether each parent's take lanes were open or
collapsed before capture. A collapsed parent plays the active take; an expanded
parent keeps every unmuted take lane visible and audible for layering and
comparison. Parent inserts are inherited by every take and are shown as
inherited in the take inspector and mixer insert list.

Loop recording writes one continuous synchronized WAV per armed parent and
creates one version lane per loop pass. Right-click a take clip to choose its
lane as the active playlist or assign its edited range to the parent comp. New
comp selections replace only overlapping regions. **Clear parent comp** returns
playback to the active playlist.

## Configure transport and linked editing

**Tracking Setup** (flag icon) manages:

- Independent named marker flags
- Song sections and section-specific transport settings
- Tempo and meter changes at the playhead
- Jump and ramp tempo transitions
- Punch points, count-in, pre-roll, and post-roll
- Loop bounds
- Straight and triplet metronome subdivisions, click/accent levels, and hardware
  output

All settings are persistent and undoable. The transport shows the current
tempo and meter at the playhead, and the timeline keeps punch and loop ranges
visible as coloured bounds. When punch and loop are both enabled, punch takes
priority for recording while ordinary playback keeps using the loop range.
The bottom transport strip keeps position, Stop, Play/Pause, Record, Loop,
loop-range, metronome, time signature, and tempo controls in compact grouped
modules. **Inspect**, **Mixer**, and **Tracks** switches at the far right own the
right inspector, lower mixer, and left Session pane. The inspector's
**Inspector** and **Plugin Manager** ribbons switch between selected-track controls
and plug-in discovery/validation. File and project tools remain in the top
header, while
undo, redo, scissors, trim, delete, Snap, a visible 1/4-1/32 grid selector, and
zoom tools share the edit toolbar. Snap applies to clip, marker, and section
dragging without introducing separate pointer/eraser tool modes.
Transport buttons are centered independently from the position display;
metronome, meter, and BPM sit immediately to their right. Audio readiness and
device status appear in the top-right info panel. The panel always shows the
latest status or error; click it to open session history. Each message has its
own clear icon, and the header trash icon clears all history. Live progress,
such as the changing recording duration, updates the panel without creating a
new history row; the completed saved-take result is stored once.

Right-click the **MARKERS / SECTIONS** lane above the bar ruler to add either a
marker flag or a song section at that exact timeline position without moving
the playhead. Marker flags can be dragged directly. Drag a section body to move
the complete range, or drag its bright right edge to resize it.

### Set an arbitrary loop

Use **Configure Loop Range** (loop-range icon) beside **Loop**, or **Tracking Setup
> Configure loop range**. The **Loop** button still switches the saved loop
on/off in one click.
The editor accepts timeline seconds, musical positions (`bar:beat:tick`, with
one-based bars/beats and 0-959 ticks), or two named markers. It can also use the
whole project or selected audio/MIDI clip. There is no fixed bar or section
count. Musical positions follow tempo ramps and time-signature changes; beats
cut short by a meter change are rejected instead of silently selecting another
bar.
If musical tick notation would move a sample-precise boundary, the editor keeps
the existing seconds format rather than silently quantizing the loop.

Apply saves the resolved timeline positions, rounded to the playback sample
grid. Choosing musical positions or markers copies their current positions;
it does not create a permanent link. Boundaries remain editable while loop is
off, but must contain at least one sample. Stop recording before changing
boundaries. Loop-range export uses these same saved bounds.

### Section tempo, meter and click

New projects start in **4/4**. Right-click a song section and choose **Tempo,
time signature and click**, or use its section submenu in **Tracking Setup**
(flag icon).
**Create + timing** / **Save + timing** opens the same settings after adding or editing
a section.

Each section can set BPM, an incoming tempo ramp, a time signature, and a click
override. BPM uses quarter notes; meter beats use the selected denominator.
Click settings include on/off, 1-8 subdivisions per meter beat, normal/accent
levels, and a comma-separated list of accented beats. For example, use `1,4`
in 6/8 or `1,4,6` in 7/8; an empty list gives an unaccented click.

Changes begin at the section boundary and continue until the next applicable
change. Unconfigured sections do not reset the clock or click. Clearing an override
inherits previous settings or the project's defaults. The global **CLICK**
button remains the master mute: section settings cannot force it on. Clicks
remain excluded from mix exports.

Section-owned tempo/meter points move with their section; deleting the section
removes those owned changes, not unrelated manual points. Moving into another
tempo/meter point reports a collision instead of overwriting it. These edits,
click patterns and loop bounds support undo/redo and native project persistence.
Stop recording before editing or moving section transport changes.

Arm two or more parent tracks and choose **Link armed parent tracks** to create a
phase-locked edit group. Split, trim, move, delete, comp, warp, and quantize
operations then apply across the active takes at the same timeline position.
The setup menu also selects the timing reference, quantize strength, protected
anchors, suspension, and unlinking.

Fade-handle drags, crossfades, transient analysis, stretch settings, polarity,
reverse, and consolidation also follow an enabled edit group. An operation is
rejected before changing anything if any linked parent lacks an active clip at
the edit position. Moving linked clips against timeline zero clamps the whole
group together so relative timing stays unchanged.

## Edit clips

Select a clip to expose edge handles. Drag its body horizontally to move it on
the beat grid or vertically to another audio track. Drag the left edge to change
the timeline start and source offset. Drag the right edge to shorten or restore
the available source range.

Hover a clip to reveal its gain and fade controls. Drag the top gain handle
vertically to change clip gain. Drag a fade endpoint horizontally to set a
linear fade length, then drag the point in the middle of the fade vertically to
shape it: upward is logarithmic and downward is a sharper inverse-exponential
curve. Right-click a clip to mute or unmute that individual piece without
muting its take lane.

Trimmed audio remains visible as a dashed waveform ghost and can be restored.
After a split, each half keeps independent source boundaries and cannot expand
through the split point.

The command bar above the timeline exposes trim left, split, trim right, and
delete. Clicking the selected clip or its track moves the playhead without
dropping clip selection, so those commands and their shortcuts act at the
visible cursor.

Right-click a clip for:

- Transient detection
- Drum, monophonic, polyphonic, and full-mix stretch modes
- Playback-rate presets and transient-to-playhead warp markers
- Fade-in and fade-out placement
- Linked crossfade generation and gap closing
- Polarity inversion and reverse playback
- Consolidation to a new immutable WAV

Green lines identify transients, orange triangles identify warp points, and fade
curves remain visible on the clip.

## Mix and route tracks

The inspector and lower mixer expose gain, pan, mute, and solo. Gain defaults to
`0.0 dB`; pan defaults to `Center`. Mixer strips expose dedicated mute, solo,
and record-arm icons plus a boxed dB readout and calibrated fader scale. Drag
faders vertically and the linear left/right pan controls horizontally to edit
them. Mixer and inspector panners share the same center-origin display: only
the active side between center and the knob is colored. Wide pre/post meters
and the current post-fader peak make every track's
level visible at a glance. Click a mixer's boxed dB value to enter a
number such as `-6`, `-6 dB`, or `-6db` directly in the strip; no dialog opens.
Values must be within `-60.0` to
`+12.0 dB` and are normalized to the fader's 0.1 dB step. Displayed positive
values include a leading `+`; zero remains `0.0 dB`.
Double-click a fader lane to return to `0.0 dB` or a pan control to return to
center. Solo is exclusive: selecting a new solo clears the previous solo, and
clicking the active solo again restores normal playback.

Every mixer strip contains its own compact **INSERTS** and **SENDS** sections.
The insert `+` opens a plug-in selection dialog already targeted to that track;
the send `+` opens its route-add menu. Click an insert to open its editor, click
a send or sidechain to open its routing editor, or use the power control on
either row to enable/bypass it in one step. The taller mixer keeps these
processing rows visible with the fader, meters, and pan control.

The left Session pane remains dedicated to project and track actions and no
longer contains the processor catalog. The right inspector groups
**Inspector** and **Plugin Manager** ribbons: Inspector holds the selected-track
controls and routing, while Plugin Manager holds the searchable processor catalog. The
bottom **Inspect**, **Mixer**, and **Tracks** switches keep controlling the right
inspector, lower mixer, and left Session pane independently.

Double-click a track name in the Inspector ribbon, timeline, or mixer to edit its name.
The colored square opens palette choices and an HSV/RGB picker. Appearance changes are
persistent and undoable.
Right-clicking a mixer strip exposes the same mute, solo, arm, name/color,
version, duplicate, and delete actions as the corresponding timeline track
header.

Use **Add Track** (plus icon) for audio, aux, bus, folder, VCA, and
control-room tracks. Newly added tracks start disarmed; arm only the recording
targets you intend to capture.
**Add Bus Track** (routing icon) remains a direct bus shortcut. Select a root
track and choose **OUTPUT** in the inspector. Audio, instrument, aux, and bus
tracks can feed a bus or the master. Buses can feed later buses. Destinations that would
create a cycle are excluded.

The routing panel adds pre-fader and post-fader sends, plugin sidechains, and
direct hardware outputs. Click a route to change tap, level, mute, enablement,
or remove it. Aux and bus tracks sum every incoming path. The graph rejects
cycles across main routes, sends, and sidechains.

**Add Track** also creates instrument and MIDI tracks. Enable a MIDI input in
**Settings** (gear icon) > **Audio / MIDI**, then arm a MIDI or instrument track
with its record-circle control to receive it while the transport is running or
stopped. MIDI and instrument
tracks can add independent MIDI destinations without replacing an instrument
track's audio output. MIDI track inserts process events before they are sent
downstream; standard and CLAP
events cross sandbox workers with their sample offsets intact. MIDI feedback
cycles are rejected before the route is added. A MIDI route can also filter one
of channels 1-16; the metal multi-output template uses those filters.

## Record and edit MIDI

Arm any combination of root MIDI and instrument tracks, then press **Start
Recording** (circle icon).
Studio Duo records the same enabled hardware MIDI input delivered to live
routing while audio tracks can record in the same pass. Stopping creates
ordinary beat-based MIDI clips through one undoable command. Loop passes become
separate clips at the loop position. Notes keep their channel, velocity,
release velocity, duration, probability, timing offset, drum metadata, and
per-note expression points.

The engine also keeps a bounded lock-free history of short MIDI messages.
Choose **CAPTURE** in the MIDI editor or press `Command/Ctrl+Shift+M` to recover
the recent performance on the selected MIDI/instrument track. The result is
trimmed to the captured performance and remains fully editable. Channel voice
messages, poly pressure, channel pressure, pitch bend, and controllers are
converted into notes and expression. Long system-exclusive messages continue
through live routing but are not stored as note data; overflow or ignored data
is reported in the status bar.

Create an empty one-bar clip with **NEW MIDI CLIP**,
`Command/Ctrl+Shift+N`, or by double-clicking empty arrangement space on a MIDI
or instrument track. Selecting a MIDI clip opens the lower editor in place of
the mixer:

- Click empty grid space or press `Enter` to create a note.
- Click a note to select it; `Command/Ctrl`-click toggles selection.
- Drag notes to move them and drag the right edge to resize.
- Use arrow keys to move, `Shift+Left/Right` to resize, and
  `Delete`/`Backspace` to remove selected notes.
- Choose **Velocity**, **Timing**, **Duration**, **Probability**, or
  **Expression** in the lower lane. Drag lane values, or use `Alt+Up/Down` for
  a keyboard-only adjustment. Expression supports poly pressure, channel
  pressure, timbre, pitch bend, and a saved per-note controller.
- Choose a grid from quarter notes through 32nd notes or 16th-note triplets.

Use **PIANO/DRUMS** to switch the same ordinary MIDI clip between editors. The
drum view reads named kit pieces and articulations from the selected drum map
and shows choke groups, cymbal edge/bow/bell/open/closed/pedal/choke states,
foot-control CCs, and round-robin hints. **IMPORT MAP** accepts the documented
JSON drum-map object. **EDIT MAP** changes the selected row without replacing
its stable ID.

The **FLAM**, **ROLL**, **GRAVITY**, **BLAST**, and **DOUBLE KICK** tools insert
fixed grid-derived notes, never opaque generated regions. Saved pattern aliases
expand with **EXPAND** into new ordinary notes. **HUMANIZE** accepts an explicit
seed plus timing-tick and velocity ranges. Studio Duo uses its own integer
generator rather than a standard-library distribution, stores both the seed
and the resulting values, and reproduces the exact project JSON after reopen on
supported platforms.

The default metal routing template separates kick, snare, tom, and cymbal note
groups onto MIDI channels 1-4, creates named destination tracks, and adds
channel-filtered routes. Applying it is one undoable command. Add **Metal Drum
Composer** to an instrument track for the bundled basic kit. Its Main output is
a complete stereo mix; its Kick, Snare, Toms, and Cymbals buses can be sent to
aux or bus tracks from **ROUTING** > **ADD** > **Processor output**. The
multi-output insert must remain last on its source track. Lower its automatable
Main level when using stems alone to avoid summing the full mix twice.

**TRACK** in the routing panel changes mono/stereo layout, polarity, solo-safe
state, folder placement, and VCA assignment. Folder mute and solo scope their
children without hiding summing; use a bus for audio summing. VCAs control the
assigned track faders without changing signal routing.

A control-room track receives the master monitor path without entering exports.
Its menu selects monitor hardware, dim, mono, mute, and inserts. Metronome
hardware routing remains separate in **Tracking Setup** (flag icon) and is never included in
the final render.
Mixer strips show separate pre-fader and post-fader meters. Plugin and bridge
latencies are aligned at every summing point and at the exact insert targeted by
each sidechain.

## Automate controls and plugin parameters

Choose **Automation** (curve icon) to open the lane editor for the selected track.

- Select read, touch, latch, write, trim, or preview mode.
- Arm writing with **WRITE ARM**.
- Add lanes for volume, pan, mute, polarity, sends, bundled devices, or
  automatable plugin parameters.
- Choose seconds or beat time, and linear or step interpolation. Changing an
  existing lane's timebase preserves its timeline positions.
- Add or remove points at the playhead with a normalized value.

Beat lanes follow tempo ramps and abrupt changes. Playback schedules mixer
changes and plugin events at exact sample offsets. Mixer fader, pan, mute,
polarity, send, bundled-device, and plugin-parameter gestures write the armed
track according to its selected mode. Touch returns to the existing lane after
release; latch holds until the next existing point; trim applies a relative
offset. Preview remains non-destructive until **COMMIT PREVIEW** writes the
captured gesture. Master and control-room volume, pan, mute, polarity, dim, and
processor automation use the same sample-accurate path.

## Use plugins

Choose **SCAN** in the processor catalog to probe installed VST3 plugins, Audio
Units, and CLAP bundles outside the main process. Bundled utility devices are
always listed together with Metal Drum Composer, Guitar Amp, and Bass Amp.
Select an entry and choose **ADD** to attach it to the selected track. The drum
instrument requires an instrument track; the amps require an audio-capable
track.

Choose **PATHS** to review the active VST3 locations, add a custom folder with
the native directory chooser, or remove a custom folder. Custom folders are
stored in application settings and apply to every project.

Studio Duo scans these VST3 defaults recursively:

- Windows: `C:\Program Files\Common Files\VST3`,
  `%LOCALAPPDATA%\Programs\Common\VST3`, and folders in `VST3_PATH`
- macOS: `/Library/Audio/Plug-Ins/VST3`,
  `~/Library/Audio/Plug-Ins/VST3`, and folders in `VST3_PATH`

Default and custom paths are normalized and deduplicated before each scan.
Missing, unreadable, or invalid custom folders are skipped and identified in
the processor-catalog status while the remaining locations continue scanning.

Ready external inserts process playback in sandbox workers. Double-click an
insert to open its plugin editor in the worker process; plugins without a native
editor receive an isolated generic editor. Right-click an insert to open the
generic parameter editor or choose sandboxed, trusted in-process, or advertised
ARA 2 mode.

Moving the playhead resets existing plugin processors and queued bridge audio
in place. Sandboxed workers remain running and ready instead of being rebuilt.

ARA activation requires a saved project, writes a recovery point, and warns
about reduced crash isolation. Studio Duo registers the track's immutable audio
sources, clip playback regions, tempo map, and meter map with the ARA document.
Processor and ARA document state are archived together, including unsaved ARA
edits preserved across clip, tempo, and meter graph rebuilds.

The inspector reports loading, ready, missing, bypassed, recovery-disabled,
crashed, and late-block states. Click a failed insert to reload that runtime.
Click a missing insert, then choose a catalog processor to replace it while
preserving the insert ID, routing, automation, and prior state reference.

## Use bundled amps and cabinets

**Guitar Amp** provides preamp gain, bass, mid, treble, presence, saturation,
cabinet mix, and output parameters. **Bass Amp** uses the same cabinet core
with a bass-specific low-frequency path and clean/distorted drive blend. Both
are real processors, report 128 samples of cabinet-convolution latency, render
offline, and expose every sound control to the normal parameter and automation
panels.

Open an amp editor and choose **Load cabinet IR...** for a mono or stereo WAV,
AIFF, or FLAC file. Studio Duo rejects missing, unsupported, silent,
non-finite, multichannel, or oversized files with a visible error and keeps the
current cabinet. The bounded configuration accepts at most 8,192 samples after
conversion to 384 kHz (about 21.3 ms), so processing remains deterministic and
real-time safe at supported sample rates. Decoding and FFT preparation happen
outside the callback.

Each amp starts with a useful embedded cabinet: Modern 4x12 for guitar and
Tight 8x10 for bass. **Use embedded cabinet** restores it explicitly. A custom
IR's normalized samples are stored inside the insert's content-addressed
opaque state, so the project restores after the source file is moved. Corrupt
or truncated cabinet state reports a failed insert; Studio Duo never silently
substitutes the default.

**TEST** runs black-box public-standard compatibility checks for the selected
plug-in in a separate process. **Tracking Setup > Check installed VST3
plug-ins** only searches readable platform-default and configured VST3 folders;
it does not launch another Studio Duo process, load plug-ins, or initialize
audio devices.

## Create reamp paths

Select a DI parent in **Tracking Setup** (flag icon) and create a hardware or plugin tone
path.

A hardware path sends the processed DI to an interface output and records the
configured return input. **Calibrate round-trip latency** emits an impulse and
stores the measured delay. Recorded returns are shifted by the measured latency
plus the saved fine-alignment offset and can invert polarity.

A plugin path creates a non-destructive audio track that references the active
DI playlist. Add inserts to that track to build the tone without replacing the
source DI.

For a selected tone path, **Tracking Setup** (flag icon) can:

- Capture named snapshots of routing, processor state, level, and automation
- Recall a snapshot with undo and its stored level-match trim
- Show stale state after the DI, playlist, routing, automation, or chain changes
- Freeze a plugin tone to an immutable WAV while preserving and muting the live
  return
- Unfreeze by restoring the live return and removing the frozen track
- Print a tone to a separate editable audio track
- Batch render every snapshot with one JSON report per item

Rendered snapshots store content hashes. Batch comparison uses deterministic
gated RMS trims, constrained to the return fader range, for level-matched A/B;
it does not claim mastering-loudness compliance. Plugin tone paths read the
current DI playlist during playback and rendering instead of keeping stale clip
copies.

## Navigate the timeline

Use the **-**, **100%**, and **+** controls, keyboard shortcuts, or
`Command/Ctrl` plus the mouse wheel to zoom while keeping the playhead centered.
The arrangement follows the playhead during playback and recording while track
headers remain pinned.

Right-click the timeline to place the playhead and open the context menu.
Pressing Play at the project end rewinds before starting.

## Mastering and release

Choose **Export > Open mastering and release workspace**. Add finished mixes with **+ SONG**,
attach alternate mixes to the selected song, choose the active source, reorder
the sequence, and edit gaps, overlaps, fades, gain, ISRC, album, artist,
songwriter, label, catalog, MCN/EAN, release date, and genre fields.

Reference files are stored separately and are excluded from album gain and
release rendering. **ANALYZE ALBUM** reports integrated LUFS, loudness range,
true peak, sample peak, and stereo correlation. Distribution presets only
report targets and warnings; they never normalize the album automatically.

**EXPORT MASTER** creates WAV, FLAC, compressed Ogg references, or a
44.1 kHz / 16-bit CD WAV with deterministic TPDF dither. The completed file is
decoded and measured again. An adjacent signed JSON report records source,
settings, and output hashes plus final measurements and warnings.

**EXPORT DDP** asks for a separately installed licensed encoder adapter and an
empty destination. Studio Duo generates sector-aligned CUE/audio input,
validates MCN/EAN and index positions, checks the returned DDP fileset, writes
SHA-256 transfer checksums, and signs the delivery report. A licensed
independent validator or replication plant must still accept the result.

**PORTABLE COPY** gathers clips, mastering sources, references, and plugin
state into a relocatable package with content hashes. **REPAIR FILES** scans a
selected folder and restores missing resources by SHA-256, falling back to an
exact filename only for legacy resources without a saved hash. See
[mastering.md](mastering.md).

## Projects and export

Studio Duo projects are versioned `.studioduo` directory packages. A save writes
a new session generation before atomically replacing `manifest.json`; the latest
complete state is also copied to `recovery/latest.json`.

Project format version 11 stores the typed routing graph, separate automation
generations, content-addressed plugin state, compatibility policy, tone and
mixer snapshots, render reports, ordinary MIDI clips and expressions, drum
maps, pattern aliases, humanization state, MIDI routing templates, project
metadata, scenes, persisted interchange reports, and channel-scoped MIDI
pressure automation. It also stores the mastering album, source hashes,
references, sequencing, release metadata, final measurements, independent
marker flags, section-owned tempo/meter points and per-section click patterns.
Versions 1-10 migrate on load.
See [project-format.md](project-format.md).

### Audio export

**Export > Export audio** opens audio settings before the destination chooser. WAV at 48 kHz,
24-bit stereo remains the default. Choose WAV, AIFF, FLAC, Ogg Vorbis, or MP3;
MP3 encoding is built in and requires no separate encoder installation.
The same encoding controls are available in the mastering workspace under
**EXPORT MASTER**.

Select a supported sample rate and bit depth, stereo or mono, and the
format-specific controls: MP3 constant bitrate or variable-bitrate quality,
Ogg quality, or FLAC compression level. WAV also supports 32-bit floating point
for preserving processing headroom. Higher FLAC compression changes file size
and encoding speed, not audio quality. Mono averages the left and right channels.
MP3 offers 64-320 kbps CBR or VBR quality 0-9 at 32, 44.1, or 48 kHz.
Ogg offers quality 0-10; FLAC offers compression levels 1-8 (JUCE's level 0
does not actually select its advertised compression level). MP3 files carry
encoder-delay/padding metadata; players that ignore gapless tags may expose
additional codec padding.

Optional peak normalization sets the sample peak to the chosen dBFS target.
It is not LUFS normalization or a true-peak limiter; lossy encoding can introduce
additional peaks. TPDF dither is available for integer PCM output only, not MP3,
Ogg, or floating-point WAV.

For mix exports, choose the whole project, the loop range, two named markers,
or custom start/end times in seconds. Playback looping is ignored during export.
The start is included and the end excluded, rounded to the output sample grid.
Choose no effects tail for an exact range, automatic for the processors'
reported tails, or a custom tail duration. Optional fade-in/out lengths apply
to the exported file, including its tail, without changing timeline clips.

Create named **Start** and **End** markers using **TRACKING SETUP > Add marker
at playhead**, or double-click empty space in the timeline marker lane. Enter a
name and position in seconds. Markers are independent flags rather than song
sections; they survive saving, reopening, dragging, and DAWproject exchange.
Use the marker's context menu, double-click its label, drag its flag, or use the
tracking menu to rename/move/delete it; all changes support undo and redo. In
export settings, choose **Between markers**
and select the start and end names. Names can repeat; each choice also shows its
position and uses a stable marker ID. Missing markers or an end at/before the
start are rejected rather than falling back to the entire project.

Projects without processors use the fast deterministic graph. Bundled and
trusted processors render offline; sandboxed third-party processors use the
same one-block pipeline in a real-time fallback so processing is never silently
omitted. Range exports retain the preceding processor/automation history and
compensate processing latency. Tails stop new source audio/MIDI at the selected
end instead of including the next section. Completed files replace their
destination only after successful encoding; a failed export preserves the
previous file.

### DAWproject interchange

Use **Export > DAWproject 1.0** to:

- Import a `.dawproject` archive into a newly created `.studioduo` project
- Export the open project as a deterministic `.dawproject` archive
- View the latest structured compatibility report
- Save the report as JSON

Import validates `project.xml` and `metadata.xml` against the embedded official
DAWproject 1.0 schemas before creating a staging project. The open project is
not replaced until the archive, media, plug-in state, translated model, native
save, and reopen verification all succeed. The source archive and external
media are read-only. Choose a new destination path; an existing `.studioduo`
package is never moved or replaced. Export also stages and verifies the complete ZIP before
atomically publishing it, so a failed export does not leave a partial success
file.

Studio Duo embeds referenced audio and captured plug-in state. Track/channel
hierarchy, mixer routing, audio and MIDI clips, notes, note expressions,
automation, devices, scenes, warps, markers, metadata, tempo, and time
signatures are translated through the dedicated interchange layer. Unsupported
source or destination details remain listed by object path in the compatibility
report instead of disappearing silently. See
[dawproject.md](dawproject.md) for the exact mapping and current compatibility
boundary.

## Update Studio Duo

Studio Duo checks the official release feed once shortly after launch. When a
new version is available, the app prompts without interrupting the current
project. Open **Settings** (gear icon) > **Updates** to check again, download manually, or
change **Download updates automatically**. Automatic downloads are enabled by
default.

The **General** Settings tab includes **Autosave project recovery after edits**.
It is enabled by default and persists across launches. Autosave updates the
recovery copy inside an already-saved `.studioduo` package; manual Save still
publishes the durable project generation.

The **Appearance** tab provides Studio Gray, Slate Blue, Forest, and Aubergine
themes. Studio Gray is the default and uses layered dark-gray surfaces rather
than near-black backgrounds. Theme selection applies immediately and persists
across launches. The base, panel, raised, text, and accent colors can be
customized; Studio Duo rejects combinations that do not preserve WCAG AA text
contrast. **Reset Default Theme** restores Studio Gray.

The **VST Plug-ins** Settings tab lists active default and custom VST3 search
folders. Defaults are scanned at startup unless **Scan plug-in folders at
startup** is disabled. Add custom folders, remove either custom or default
locations, restore all platform defaults, or run **Rescan now** manually.
For deeper verification, choose one scanned external plug-in and run
**Advanced Validate**. That explicit action launches the isolated compatibility
validator for only the selected plug-in; ordinary folder scans remain
filesystem-only.

Every package is downloaded inside the Studio Duo application-data directory
and must match the release manifest's filename, byte size, and SHA-256 checksum.
The current app keeps running after the download. Choose **Restart and Update**
only when convenient; Studio Duo then quits, installs the staged version, and
reopens.

Open **Help > User Guide** in the top application bar to read the documentation
inside Studio Duo. A persistent contents index separates the guide into focused
pages, with previous/next navigation and search within the current page.
Headings, emphasis, code, and tables are rendered with appropriate fonts and
alignment. The rendered text remains selectable, so commands, paths, and other
examples can be copied. The guide is embedded when the application is built, so
it always matches the installed Studio Duo release and remains available
offline.

On macOS, the updater accepts the project's normal ad-hoc-signed application
bundle and does not require notarization or an Apple update-signing key. The
installed `.app` and its parent folder must be writable by the current user. On
Windows, the updater runs the release installer silently and uses its existing
upgrade identity; it adds no updater-specific certificate, key, service, or
background process. The portable Windows ZIP remains a manually replaced
standalone copy; install Studio Duo with Setup to enable in-app updates.

## Logs and diagnostics

Studio Duo writes asynchronous, process-specific daily logs below
`~/Library/Application Support/Studio Duo/Logs` on macOS and
`%APPDATA%\Studio Duo\Logs` on Windows. Logs from the last 24 hours remain
plain text, older logs are gzip-compressed, and files older than seven days are
deleted. Each batch is flushed while the app is running; startup checkpoints
are also flushed before window creation and native audio initialization.

On Windows, unhandled native exceptions produce a matching
`studio-duo-crash-*.log` and `studio-duo-crash-*.dmp`. The text report records
the exception code, fault address, module filename, thread ID, and active
startup/runtime phase. The native dump records thread stacks and loaded-module
details for a debugger. The main app displays both locations in its native error
dialog. Probe and plugin workers do not display blocking crash dialogs.
Abrupt process termination can bypass an exception handler; an ASIO probe that
exits without a completed response is still treated as a failure.

If the app closes during startup, run the installed executable from PowerShell:

```powershell
& "$env:ProgramFiles\Studio Duo\Studio Duo.exe" --safe-audio
```

Use the actual executable path for a portable or custom installation.
`--safe-audio` skips automatic audio and MIDI initialization for this launch
without deleting the saved setup. Open **Settings** (gear icon) > **Audio / MIDI** and
choose **Windows Audio** or another working driver. Share the newest ordinary
log plus the matching crash report and dump when reporting the problem. A dump
can contain fragments of process memory, including project or plug-in data, so
share it privately with a trusted maintainer rather than posting it publicly.
Each release publishes a matching Windows symbols archive; dumps must be opened
with symbols from the exact Studio Duo build that crashed.
MIDI discovery failures also leave **Updates** available without loading audio
devices; fix the reported driver or system error before retrying audio Settings.

The ASIO probe checks startup only: the selected driver still runs in the main
process during recording and playback. Explicit device changes in Settings
also run in the main process. This is not runtime driver isolation.

Create `logging.json` beside the `Logs` directory to change retention or enable
verbose debug logging, then restart Studio Duo:

```json
{
  "schemaVersion": 1,
  "retentionDays": 7,
  "debugLogging": false
}
```

`retentionDays` accepts 1 through 365. Debug logging is disabled by default.
Logs redact the user-home and current workspace paths and never include audio
content or plugin state. User-visible failures and caught application errors
are written at `ERROR`; routine lifecycle information is intentionally small.

## Brand assets

Editable logo, icon, PNG, ICNS, and ICO sources are stored in
[`../assets/branding`](../assets/branding). The application embeds the SVG mark
in its header and uses the platform icon sources during builds.
