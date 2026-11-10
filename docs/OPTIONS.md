# fMCG settings in detail

This page explains each setting more fully than the [README](../README.md).

- [Stats](#stats)
- [Timing and MIDI handling](#timing-and-midi-handling)
- [Deduplication](#deduplication)
- [Text and number formatting](#text-and-number-formatting)
- [Boxes](#boxes)
- [Advanced typography](#advanced-typography)
- [Output formats and encoder settings](#output-formats-and-encoder-settings)
- [Patterns and saved settings](#patterns-and-saved-settings)
- [CJCMCG preprocessed files](#cjcmcg-preprocessed-files)

## Stats

- **Notes per second** counts the note-ons in the last second. The window includes its boundary, so a note exactly 1.0 s earlier still counts. `{nps-max}` is the highest value so far.
- **CC per second** (`{cps}`, `{cps-max}`) counts control-change events the same way: the last second, boundary included, and the highest value so far. The "CC" comma option groups them.
- **Polyphony** is note-ons minus note-offs. A note-off only counts when that key is held on that channel. `{plph-max}` is the highest value so far.
- **Ticks** keep advancing during the end delay at the last tempo. During the start delay they count up from a negative value to zero.
- **Beats and measures** follow the file's time signatures, with 4/4 when there are none.
  - Numbering starts at 1. During the start delay, beats and measures show 0.
  - A beat is one note of the signature's denominator, so 6/8 has six eighth-note beats per measure.
  - A time signature that repeats the one already in force changes nothing.
  - A real change starts a new measure. A measure or beat cut short by the change still counts as one.
  - `{beat-total}` and `{measure-total}` cover the whole song, and the last frame shows those totals.
  - `{measure-beat}` is the beat within the current measure, from 1 to the numerator.
  - `{timesig}` shows the time signature in force, such as `3/4`.

## Timing and MIDI handling

- **Start delay** adds a lead-in before the song.
  - All stats sit at zero, and the time fields count up from a negative value to zero.
  - The background is shown, and BPM and time signature show their starting values.
- **End delay** adds a tail after the song.
  - Note and CC stats freeze at their final values, and NPS and polyphony fall to their resting values.
  - Time keeps counting. `{time-max}` and `{sec-max}` still give the song's own length.
- **Vel-0 as Note-Off** treats a note-on with velocity 0 as a note-off, as the MIDI standard says. Turn it off only for unusual files that count those events as notes.
- **Close held notes** releases every note still held at the end of its track. Without it, a note that is never released stays in the polyphony count, which is what the MIDI file literally says. It only changes polyphony.
- **Parsing threads** split a plain MIDI file's tracks across threads. The default is all cores, and the result is identical to a single-threaded scan. Compressed files are always read in one stream.
- **Very long files:** a file whose tick count goes past the MIDI spec's 28-bit limit asks whether to continue in one pass, which can use a lot of RAM, or in a low-memory **two-pass** mode.

## Deduplication

Two separate options, both off by default. Only one can be on at a time; ticking one greys the other out.

### Track dedup

- Before parsing each MIDI track, fMCG hashes its raw bytes and compares them with the tracks it has already seen.
- A byte-identical repeat replays the cached result instead of being parsed again.
- It works on every input: plain files, compressed files and nested archives. Archives are still fully decompressed; only the parsing is saved.
- It helps most on files where one track is duplicated many times. On fully unique content it only costs the hashing.
- Tracks that end with notes still held are never replayed, because they cannot be replayed exactly. The cache size is capped for very large unique files.

### Layered dedup

- This option is for nested archives built by compressing a MIDI, concatenating copies of the compressed stream, compressing again, and so on (xz inside xz).
- Only the unique compressed members are decoded and parsed. Repeats are counted by multiplicity, and uniform note grids are folded arithmetically.
- Files that expand to terabytes load in seconds or minutes, and the counts are exact.
- Inputs that are not layered xz archives get the normal scan.

### Risky fast dedup

- This option requires **Layered dedup**. It is off at every start and never saved.
- It applies in each layer once the first tile repeats. A tile is either one member repeated back to back, or a run of up to 64 members repeated as a whole, for example a track cut into 16 slices that cycle.
- If the rest of the layer is an exact multiple of the tile's size, it is **assumed** to be that tile repeated, and is not decoded.
- It is exact on files built as "header + one tile repeated". It turns the 16-minute "When Systematic & Inferno has BSOD" into about 10 seconds.
- It is **silently wrong** on files where the rest of a layer is made of different tiles whose sizes happen to add up to a multiple. Use it only where that cannot happen.
- Every assumption is written to the log (layer, header bytes, tile size, multiplicity and bytes skipped), followed by a final reminder that the counts depend on it.

## Text and number formatting

- **Comma separators** are set per stat (Notes, Poly, NPS, Ticks, CC, Beats) to show `1,234,567` instead of `1234567`. Beats covers both the beat and the measure stats.
- **Leading zeros** pads every number to the digit count of its own maximum, so no width needs to be set:
  - notes to the total notes;
  - polyphony to peak polyphony;
  - seconds to total seconds;
  - beats to total beats.
- **Fixed-width stats** gives every stat a cell as wide as its widest value anywhere in the video, measured in the chosen font. The value is aligned left, centre or right inside the cell, so text after a stat and the width of each line stay put while digits change.
- **BPM decimals** sets how many decimal places `{bpm}` shows, from 0 (`120`) to 6. The default is 2.
- **Text / Background colour:** click a swatch for a picker with R/G/B sliders and a hex value.
- **Outline** draws a border of the chosen width and colour around the text. It is on (1 px black) for new patterns.

## Boxes

The overlay is split into boxes. Each box has its own text, and says where in the box that text sits. Boxes never overlap and always cover the whole frame; a box without text shows nothing.

- **The box editor** above the text editor shows the frame as it will be drawn, with the boxes on top. It shows the processed song's last frame, or every stat at zero before Process.
  - Click a box to select it. Its number is in its top-left corner, and the text editor below shows its text.
  - **Split into columns** and **Split into rows** divide the selected box into the number of equal **parts** chosen beside them (2 to 8). Splitting a column into more columns adds them to the same row of columns.
  - Drag the line between two boxes to resize the boxes on both sides of it. The line snaps to 1/2, 1/3, 1/4, 1/5, 1/6, 1/8, 1/10 and 1/12 of the frame and to the edges of other boxes; the tooltip shows where it is. Hold Alt to drag without snapping.
  - **Equal sizes** gives the selected box and the boxes it was split with the same size.
  - **Delete box** removes the selected box. Its space goes to the box before it (or after it, for the first), and its text is added to the end of that box's text.
- **Text sits at** chooses one of 9 points of the selected box: a corner, an edge centre or the centre. The block of text is placed there, inset by the **Margin**.
- **Lines** sets how the box's lines align with each other.
  - **Follow position** (the default) aligns them to the side the text sits at: left, centred or right.
  - **Left**, **Center** or **Right** aligns them that way inside the text block. The block then keeps the width of its widest possible line for the whole video, so for example a centred label does not drift while the stat under it grows.
- **Margin** is the space kept between each box's edges and its text, 30 px by default, the same for every box.
- Box sizes are fractions of the frame, so a layout keeps its proportions at any resolution.
- For a label centred over a number, also turn on **Fixed-width stats** with **Center** alignment, so the number is centred in its cell.
- **The text never leaves its box or the frame.** Every block, including its outline, is kept inside its box when it fits there, and always inside the frame. If the largest block the video will ever show does not fit its box, the font is scaled down once for the whole video, by the same amount for every box. The preview shows a note, and the render log says by how much.

## Advanced typography

These OpenType options are saved with the pattern and applied when the text is shaped.

- **Kerning** and **Ligatures** are on by default.
- **Slashed zero** uses the font's `zero` feature. A font whose zero is already slashed may show no change, and many fonts keep other zero styles in stylistic sets or character variants (for example Victor Mono `ss02`..`ss05`, Iosevka `cv10=1`..`30`).
- **Numerals**:
  - *Lining* or *Old-style* figures, or the font's default.
  - *Proportional* or *Tabular* widths, or the font's default. Tabular digits all have the same width, so counters stop shifting sideways.
- **Stylistic sets** lists the sets the selected font offers (`ss01`..`ss20`), with the font's own names when it has them.
- **More features** takes any other OpenType features, separated by spaces or commas (`cv01 salt -calt smcp`, or `tag=2` for an alternate).
  - These are applied last, so they override the options above.
  - Invalid entries are shown in red and ignored.
- **Letter spacing** is in px and may be negative. **Line spacing** is a multiple of the font size.

The section lists the features the selected font has, and warns when a chosen option is not in the font, because it then has no effect.

## Output formats and encoder settings

- **MP4 (H.264)** is the default and plays everywhere. It is encoded with x264 in BT.709.
- **MOV (QuickTime Animation)** is lossless RGB, very fast to encode, and small for this kind of content.
  - It is meant for video editors (Premiere, After Effects, Vegas, DaVinci Resolve) and does not play in browsers.
  - With **Transparent background**, the frame keeps an alpha channel (32-bit ARGB), so it can be overlaid without keying.

**Encoder settings** sit in the Global column. They are collapsible and saved globally:

| Setting | Default | Meaning |
|---|---|---|
| Quality (CRF) | 18 | MP4 only. From 0 to 51; lower means better quality and bigger files. 18 is visually lossless for text. 0 is mathematically lossless but uses the High 4:4:4 profile, which many players and browsers cannot play. |
| Speed preset | veryfast | MP4 only. The x264 preset, from `ultrafast` to `placebo`. Slower presets give smaller files at the same quality but render more slowly. |
| Tune | None | MP4 only. The x264 tune: Animation, Still image, Film, Grain or Fast decode. |
| Keyframe every | 2 s | Both formats. The maximum keyframe distance, from 0.1 to 60 s. Shorter means faster seeking in editors and slightly bigger files. |

**Reset encoder defaults** restores these values.

## Patterns and saved settings

- A **pattern** stores the overlay's look:
  - the boxes, with each box's text, position and line alignment, and the margin;
  - font family, style and size, and advanced typography;
  - text, background and outline colours;
  - comma options, padding, fixed-width cells and BPM decimals.
- Working with patterns:
  - pick one from the dropdown;
  - **Save** updates it;
  - **Save As...** creates a new one, and asks before overwriting an existing name;
  - **Delete** removes it.
  - Switching patterns with unsaved edits asks before discarding them.
- Pattern files from older versions keep their look:
  - missing settings take their defaults;
  - patterns saved before the built-in renderer (the FFmpeg/ASS era) keep that era's 1 px black outline;
  - a corner position becomes one box covering the frame, with the text at that corner;
  - a custom x,y position becomes a box with an edge at that point (margin 0), converted at the resolution set when the pattern is loaded. The text is drawn exactly where it was, except when it did not fit between the point and the frame edge: it used to be pushed back from the edge and is now scaled down to fit its box.
- Everything else is a **global setting**: resolution, FPS, delays, MIDI handling, threads, dedup, output format and encoder settings.
  - Global settings are saved automatically when changed, at most once per second, and restored at the next launch.
  - Risky fast dedup is the exception and is never saved.

Settings live in the per-user config folder, created on first run:

| Platform | Location |
|---|---|
| Windows | `%APPDATA%\fMCG\` |
| Linux | `$XDG_CONFIG_HOME/fMCG/` or `~/.config/fMCG/` |

- Inside are `settings.ini` (global settings) and `patterns/<name>.ini` (one file per pattern).
- Both are plain `key = value` files that you can edit by hand, and updates never touch them.
- Deleting the folder resets everything; the `Default` pattern is recreated at the next launch.

## CJCMCG preprocessed files

fMCG reads the preprocessed files saved by **CJCMCG**, another note counter. They can be plain or inside any supported archive (`song.cjcmcg`, `song.cjcmcg.xz`, a `.7z` holding a `.cjcmcg`, ...). Pick one like a MIDI and press **Process**.

| Available from a `.cjcmcg` | Not in the file |
|---|---|
| Notes, NPS and peak NPS (fMCG's exact 1 s window), polyphony and peak, tempo / `{bpm}`, time, ticks, `{ppqn}` | CC (`{cc*}` stay 0), time signatures (beats and measures count in 4/4), lyrics (ignored) |

A `.cjcmcg` keeps only per-tick note-on and note-off **counts**, with no pitches, velocities, channels or tracks. So:

- Velocity-0 note-ons count as notes, because CJCMCG counted every note-on event. **Vel-0 as Note-Off** and **Close held notes** cannot be applied.
- Polyphony is note-ons minus note-offs, never below zero. A note-off with nothing held is ignored, as fMCG does for a key that is not held.
- The song ends at the last note, tempo or lyric tick, because the file has no End-of-Track events.

For a MIDI without velocity-0 note-ons, a `.cjcmcg` made from it gives the same stats as the MIDI itself, apart from beats and measures.

CJCMCG's writer stores a placeholder tempo record at tick 0. It is CJCMCG's internal value for 120 BPM and would otherwise read as about 6 BPM, so fMCG recognises it and uses the standard 120 BPM default.
