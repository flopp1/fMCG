# fMCG

**Fast MIDI Counter Generator** turns a MIDI file into a video of a live statistics overlay: notes played, notes per second, polyphony, BPM, time, beats and more. It is built for black MIDIs with billions or even trillions of notes.

- **Fast.** Renders a note counter for a 2.3-billion-note MIDI in 3 minutes on a Ryzen 5 5500.
- **Constant RAM.** The file is streamed once and never loaded whole, compressed and nested archives included.
- **Self-contained.** fMCG encodes MP4 or MOV itself, so no FFmpeg is needed, and the live preview shows exactly the frames that get rendered.

Thanks to Nitsua for the idea of zero-copy mmap and other scanner micro-optimisations.

## Download

Windows and Linux builds are on the [Releases page](https://github.com/flopp1/fMCG/releases).

- **Windows:** a single `fMCG.exe` with nothing to install.
- **Linux:** a binary that needs a few runtime libraries; see [Running the prebuilt Linux binary](docs/BUILDING.md#running-the-prebuilt-linux-binary).

To build from source, see [docs/BUILDING.md](docs/BUILDING.md).

## Quick start

1. **Browse** to a MIDI file. Plain `.mid` works, and so do `.7z`, `.xz`, `.rar`, `.gz`, `.bz2`, `.zst` and `.lz4` archives in any nesting, such as `.rar.xz`.
2. Press **Process** to scan the file.
3. Press **Preview** to watch it live, or **Render Video** to encode it. By default the video is saved as `<midi name>_fMCG.mp4` next to the MIDI.

You can design an overlay before loading anything: with no file processed, Preview shows the current pattern with every stat at zero.

## Features

- **Any size of MIDI.** Plain files are memory-mapped and their tracks parsed in parallel. Archives are decoded on the fly and never written to disk.
- **Layered dedup.** "Concatenate, compress, repeat" archives that expand to terabytes load in seconds, with exact counts.
- **Your own layout.** Write any text around stat tokens. Each line of the layout is one row of the overlay.
- **Typography.** Any installed font, with kerning, ligatures, tabular numerals, stylistic sets and other OpenType features. Text colour, background colour and an outline are configurable too.
- **Stable layout.** Fixed-width stat cells, leading zeros and comma separators keep the overlay from jumping as digits change.
- **Never overflows.** Text is always kept fully on screen, and the font shrinks for the whole video if the block would not fit.
- **MP4 or MOV output.** MP4 is H.264 and plays everywhere. MOV is lossless QuickTime Animation, optionally with a transparent background for video editors.
- **Patterns.** Save overlay looks as patterns and switch between them.
- **CJCMCG files.** `.cjcmcg` preprocessed files from CJCMCG are read directly.

## Layout

Each line of a box's text is one row of the overlay, and a row can mix any text with tokens, for example `Notes: {nc}/{nc-total}  NPS: {nps}`. **Insert stat** adds a token at the cursor, **Stats list** in the app shows every token, and **Load from file...** reads a box's text from a `.txt` file.

| Stat | Tokens (current / total / remaining) |
|---|---|
| Notes | `{nc}` `{nc-total}` `{nc-rem}` |
| Notes per second (1 s window) | `{nps}` `{nps-max}` |
| Polyphony | `{plph}` `{plph-max}` |
| BPM | `{bpm}` |
| Time `mm:ss` | `{time}` `{time-max}` `{time-rem}` |
| Time `mm:ss.mmm` | `{time-milli}` `{time-milli-max}` `{time-milli-rem}` |
| Seconds | `{sec}` `{sec-max}` `{sec-rem}` |
| Ticks | `{tick}` `{tick-total}` `{tick-rem}` |
| Beats | `{beat}` `{beat-total}` `{beat-rem}` |
| Measures | `{measure}` `{measure-total}` `{measure-rem}` |
| Beat within the measure | `{measure-beat}` |
| Time signature | `{timesig}` (e.g. `6/8`) |
| Control changes | `{cc}` `{cc-total}` `{cc-rem}` |
| Control changes per second (1 s window) | `{cps}` `{cps-max}` |
| PPQN | `{ppqn}` |

- Times switch to `h:mm:ss` from one hour on.
- During the start delay, the time fields count up from a negative value to zero.
- Beats and measures follow the file's time signatures; files without any count in 4/4.

### Boxes

The overlay is split into **boxes**, shown over a small picture of the frame above the text editor. A new pattern has one box covering the whole frame.

- Click a box to select it. The text editor below shows that box's text.
- **Split into columns** or **Split into rows** divides the selected box into equal parts. To place three counters side by side, split the one box into 3 columns.
- Drag the line between two boxes to resize them. It snaps to halves, thirds, quarters and so on of the frame, and to other box edges. Hold Alt to drag freely.
- **Text sits at** picks the point of the box where its text goes: a corner, an edge centre or the centre. The text keeps the pattern's **Margin** from the box edges.
- **Lines** aligns the box's lines with each other: following that point (the default), or left, centred or right.

For a label centred over its stat in each third of the frame, split the frame into 3 columns and set each box's text to sit at the top centre:

```text
NOTES
{nc}
```

## Settings

The window has two columns. **Global settings** on the left apply to every video. The **Pattern** on the right is the overlay's look and is saved with the pattern.

**Global settings**

| Setting | What it does |
|---|---|
| Start / End delay | Time before the song, with stats at zero, and after it, with stats frozen. Up to 1 hour each. |
| Vel-0 as Note-Off | Treats a note-on with velocity 0 as a note-off, which is the MIDI standard. |
| Close held notes | Releases notes that are never turned off at their track's end. This only affects polyphony. |
| Resolution / FPS | Video size, from 16 to 16384 px (MP4 needs even sizes), and frame rate, up to 1000. |
| Parsing / Encoder threads | Threads for reading plain MIDI files and for encoding. |
| Track dedup / Layered dedup | Speed-ups for files built from repeated copies; only one can be on at a time. Off by default. |
| Risky fast dedup | Optional and much faster on some layered files, but can give **wrong counts**. Read [the details](docs/OPTIONS.md#risky-fast-dedup) before using it. |
| Format | MP4 (H.264) or MOV (lossless), with an optional transparent background for MOV. |
| Encoder settings | Quality, speed preset, tune and keyframe interval. |
| Output Path | Where the video is saved. |

**Pattern**

| Setting | What it does |
|---|---|
| Font, size, colours, outline | The look of the text. New patterns have a 1 px black outline. |
| Boxes and layout | The frame split into boxes, each with its own text, position and line alignment (see [Boxes](#boxes)). |
| Comma separators | Per stat: `1,234,567` or `1234567`. |
| Leading zeros | Pads each stat to the digit count of its own maximum. |
| Fixed-width stats | Gives each stat a cell as wide as its widest value, aligned left, centre or right. |
| BPM decimals | 0 to 6 decimal places for `{bpm}`. |
| Margin | The space between each box's edges and its text, 30 px by default. |
| Advanced typography | Kerning, ligatures, numeral styles, stylistic sets, extra OpenType features, letter and line spacing. |

Global settings save automatically. Patterns are saved with **Save** and **Save As...**, and patterns from older fMCG versions keep their look.

[docs/OPTIONS.md](docs/OPTIONS.md) explains every setting in detail, including dedup, boxes, typography, encoder settings, CJCMCG files and where settings are stored.

## License

GPL-3.0; see [LICENSE](LICENSE). Licences of the bundled libraries are in [THIRD_PARTY_LICENSES.txt](THIRD_PARTY_LICENSES.txt).
