# Import from Studio One

Studio Duo accepts native Studio One `.song` files and Studio One DAWproject
exports. Use **Import From Studio One** on the startup hub or
**Export > Import from Studio One...** in an open session. **Open**
(`Command/Ctrl+O`), dropping one project onto the window, and passing a project
path on the command line use the same source recognition and import service.

Choose a **new** `.studioduo` destination. Imports collect referenced audio
into the new portable package. The original song, archive, and media stay
read-only. Unsaved current work prompts for **Save and Continue**,
**Continue Without Saving**, or **Cancel**. An unsuccessful or cancelled
import does not replace the current project.

## Recommended: DAWproject

For the broader transfer of editable MIDI, automation, tempo and meter maps,
audio processing, and portable plug-in data:

1. Open the song in **Studio One 6.5 Professional or later**. DAWproject was
   introduced in 6.5 Professional and Studio One+; older/Artist editions do not
   offer this export.
2. Choose **File > Convert To > DAWproject File...** in Studio One.
3. Choose the resulting `.dawproject` in Studio Duo, or open/drop it directly.
4. Choose the new `.studioduo` package and review the compatibility report.

DAWproject preserves the features represented by both applications. It does
not make proprietary instruments, effects, or preset containers universally
loadable. Render tracks whose original sound depends on unavailable devices.
See [DAWproject interchange](dawproject.md) for the exact supported mapping,
plug-in state boundary, archive size limit, and compatibility reporting.

The export command and edition requirements are documented by
[PreSonus](https://support.presonus.com/hc/en-us/articles/19743606863629-Introducing-DAW-Project).

## Direct native `.song` import

Select the `.song` without an export step. Keep its **Media** directory beside
the song, especially when moving between Windows and macOS. Studio Duo resolves
relative media, available local file URLs, and relocated references from the
current `Media/` tree, including percent-encoded names and nested directories.
It does not guess matches by filename or download network media.

Native `.song` is a proprietary format, not the open DAWproject schema.
Direct import deliberately handles a verified ZIP/XML subset:

| Native content | Direct import |
| --- | --- |
| Song title, artist, album, comment | Preserved |
| Constant tempo and base meter | Preserved |
| Track names, order, colours, folders | Preserved |
| Ordinary audio events | Media, seconds/beat positions, lengths, source offsets, speed, and mute |
| Mixer channels | Mono/stereo, volume, pan, mute/disable, solo, and solo-safe |
| Bus/FX routing and sends | Reconstructed when channel destinations are available |
| Timeline markers | Positions and names; native stop-at-marker behavior is reported separately |
| Native processors | Named missing descriptors; private processor state is not restored |
| Native MIDI parts and automation | Reported, not converted or fabricated |
| Unknown tracks, events, audio processing, or channel types | Explicit compatibility notices |
| Changing/curved tempo or changing meter | Import stops; use DAWproject rather than risk incorrect timing |

When supported content can be imported but other native content cannot,
Studio Duo first displays a compatibility report and offers **Use DAWproject**,
**Import Supported Content**, or **Cancel**. Partial import is never automatic.
The MIDI track may remain as an empty instrument track, but the importer does
not invent notes or imply that private native data was restored.

Missing media, invalid audio bounds or routing, unsafe/duplicate ZIP paths,
corrupt entry CRCs, malformed XML, and excessive XML depth/size stop the import.
Existing destination packages are rejected. Native input does not initialize
audio hardware or load the source song's processors.

Successful imports persist their report in the native package. Open
**Export > DAWproject 1.0 > View latest compatibility report** or
**Save latest compatibility report...** to inspect/export the latest report,
including reports from native Studio One import.

Studio One `.project` files are mastering albums, not `.song` sessions. They
are not accepted as songs.

## Development

`src/studio_one_io/StudioOneProjectIO` translates the native subset.
`ProjectImportService` dispatches `.song` and `.dawproject` sources, while
`.studioduo` packages continue to open through `ProjectFile`.
`ProjectArchiveReader` shares bounded ZIP validation and CRC-checked reads
with the existing DAWproject importer. Native import uses the portable-copy
service to stage, hash-check, validate, and publish collected audio.

`studioOneProjectTests` creates only synthetic XML, ZIPs, audio, MIDI,
processor descriptors, and preferences. It covers native mapping, relocated
Windows paths, source immutability, private-content confirmation, invalid
sources, shared DAWproject dispatch, and the keyboard-accessible startup action.
