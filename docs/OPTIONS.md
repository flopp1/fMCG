# fMCG settings in detail

This page explains each setting more fully than the [README](../README.md).

- [Stats](#stats)
- [Timing and MIDI handling](#timing-and-midi-handling)
- [Deduplication](#deduplication)
- [Text and number formatting](#text-and-number-formatting)
- [Counter position](#counter-position)
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

## Counter position

- **Corners** places the block with the alignment dropdown (top or bottom; left, centre or right), 30 px from the edges.
- **Custom x,y** uses two anchors:
  - **Measured from** is the point of the frame that x,y count from: a corner, an edge centre or the centre.
    - From an edge, the values count inward: from the right edge +x moves left, and from the bottom +y moves up.
    - From a centre line, +x is right and +y is down.
    - A position measured from a corner stays in that corner when the resolution changes.
  - **Block anchor** is the point of the text block that sits at that position, one of the same 9 points. The lines of the block align to that side (left, centre or right).
  - The defaults, Top Left and Top Left, put the block's top-left corner at x,y.
- **Position lines in the layout** split the overlay into several blocks.
  - A layout line that is only `[top-left]`, `[top-center]`, `[top-right]`, `[center-left]`, `[center]`, `[center-right]`, `[bottom-left]`, `[bottom-center]` or `[bottom-right]` sends the lines below it, up to the next position line, to that point of the frame. They sit 30 px from the edges, like **Corners**.
  - Lines before the first position line use the placement above (Corners or Custom x,y). `[default]` returns to that placement for the lines below it.
  - Position lines are not shown. Lines that share a position form one block, in layout order, and a position that matches the Corners setting joins the default lines.
  - Names ignore case and spaces. `centre` and `middle` work like `center`, and `[top]`, `[bottom]`, `[left]` and `[right]` are the edge centres. Any other text in brackets is shown as written.
  - Each block's lines align to its side: left, centred or right, unless a line alignment is set (below).
  - **Insert position**, next to the layout box, adds a position line above the line the cursor is on. Its **Lines in the block** choice adds a line alignment to it. **Insert stat** adds a stat token at the cursor.
  - Blocks can overlap if their lines are long enough. Shorten the lines or move a block if they collide.
  - Every block uses the same font and the same fixed-width cells, and one font scale is chosen so that every block fits.
- **Line alignment** sets how the lines of a block align with each other, separately from where the block sits.
  - **Follow position** (the default) aligns them to the block's side, as described above.
  - **Left**, **Center** or **Right** aligns them that way inside the block. The block keeps the width of its widest possible line for the whole video, so a centred label does not drift while the stat under it grows, and the block stays against its corner or edge.
  - A position line can set it for its own block: `[top-left, center]`, `[bottom-right, left]`, `[default, right]`. If two position lines for the same spot give different alignments, the first one counts.
  - For a label centred over a number, also turn on **Fixed-width stats** with **Center** alignment, so the number is centred in its cell.
- **The text never leaves the frame.** In every mode the block, including its outline, is kept fully on screen. If the largest block the video will ever show does not fit, the font is scaled down once for the whole video. The preview shows a note, and the render log says by how much.

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
  - layout text and position;
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
  - their custom positions stay anchored by the corner they used.
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
