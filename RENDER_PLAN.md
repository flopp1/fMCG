# RENDER_PLAN.md -- goal 3: in-built renderer (replaces ASS + FFmpeg)

Status: PLAN (2026-10-11). User decisions (2026-10-11):
- A. Shaping: **HarfBuzz now**.
- B. Output: **x264 .mp4 = default**, built-in **qtrle .mov** as the second format.
- C. Transparent background: **yes, as an option**, .mov (qtrle) only.
- D. Outline: **off by default**; a pattern setting with user-configurable colour and width
  (saved in patterns, not global).

## 1. Today
`gui/jobs.cpp run_process` -> `generate_ass` (src/fmcg_format.cpp) writes one ASS Dialogue per
video frame -> `run_render` spawns `ffmpeg -f lavfi color=... -vf subtitles=temp_stats.ass -c:v
libx264 -pix_fmt yuv420p out.mp4` and parses `-progress pipe:1`.
Preview (`gui/preview.cpp`) draws the same text with ImGui's font atlas and *estimates* block size
(0.62 x size per char, 1.5 x size per line) for clamping.

Problems: FFmpeg (+libass) is an external, GPL, user-installed dependency; preview != render
(different rasterizer, different metrics, libass draws a 1 px black outline the preview does not);
clamping uses estimates (goal 5); a multi-hour video means a multi-hundred-MB ASS file; libass has
no control over per-glyph placement (blocks goal 7) or OpenType features (goal 4); yuv420p blurs
coloured text edges.

## 2. Target architecture
```
FrameStats[] + template + settings
        |
   FrameText (per video frame k: lead-in / song / tail -> text lines)        [src/fmcg_format]
        |
   TextLayout (shape lines with the chosen font + features, measure,         [src/fmcg_text]
               place block: corner / centre / custom x,y, exact clamping)
        |
   Raster (RGBA8 canvas: background or transparent, glyph cache,             [src/fmcg_raster]
           coverage blend, optional outline)
        |------------------------------> Preview: upload canvas (dirty rect) as a GL texture
        |
   Encoder (codec) -> Muxer (container)  -> file                              [src/fmcg_video]
```
One code path produces every pixel for both preview and render => WYSIWYG, exact metrics.

### 2.1 FrameText (refactor, no new deps)
Pull the frame-timing logic out of `generate_ass` into a function
`frame_text(k, ...) -> std::vector<std::string>`: video frame k at t = k / fps; t < start_delay ->
lead-in (zero stats, negative time/ticks, starting BPM -- same rules as now); otherwise
`frames[min(floor((t - start_delay) * fps + 1e-9), last)]`. Preview and render call the same function.
Keeps `ProcessTemplateLine` untouched (all token formatting stays as is).

### 2.2 Fonts, shaping, layout
- Font file from the existing discovery (`find_font_file_for_variant`). Add the face index for .ttc
  collections (FontVariant currently has no index -> extend `fmcg_fonts`).
- Rasterization: stb_truetype (already vendored in vendor/imgui, public domain/MIT), glyphs by
  GLYPH INDEX, so a shaper can drive it.
- Shaping **[DECISION A]**: HarfBuzz (MIT; builds as one translation unit `harfbuzz.cc`;
  apt `libharfbuzz-dev` on Linux) gives kerning (GPOS), ligatures, stylistic sets ss01-ss20,
  old-style figures (onum), TABULAR figures (tnum -- equal-width digits, directly useful for goal 7),
  fractions, etc. Alternative: start with stb_truetype's legacy `kern` table only and add a shaper in
  goal 4. Recommendation: HarfBuzz now -- the layout API is designed around shaped glyph runs either
  way, and goal 4 then becomes a settings/UI task.
- Size semantics: keep libass's convention (font size N => ascender+descender = N px) so existing
  patterns keep their size. Line advance = libass-equivalent line height.
- Layout = per line: shaped glyph run -> advance width; block = max width x n * line height.
  Placement exactly like today (alignment 7/8/9/1/2/3, 30 px margins, custom x,y = block top-left),
  but clamping uses the REAL extents of the current frame's text (goal 5 groundwork).
- Goal 7 hook: the layout keeps per-token spans, so a later "fixed-width cell" can reserve a width
  per token and align within it.

### 2.3 Raster
- RGBA8 canvas at video resolution. Background = settings colour, or alpha 0 if transparent output
  is chosen **[DECISION C]**.
- Glyph cache keyed (font, size, glyph id, subpixel x phase): coverage bitmaps from stb_truetype
  (with 1/3-pixel horizontal phase for smooth placement).
- Outline (decision D): pattern settings `outline_enabled` (default off), `outline_width` (px at video
  resolution), `outline_colour`; drawn as a dilated coverage mask under the fill. NOTE: existing videos
  had libass's 1 px black outline; new renders are plain unless enabled.
- Per frame only the text block's bounding box is re-cleared and redrawn (dirty rect); the rest of
  the canvas is constant. This also makes the preview upload and the encoder's change detection cheap.

### 2.4 Encoder + muxer  **[DECISION B]**
Candidates (all in-process, no FFmpeg):
| option | licence / deps | pros | cons |
|---|---|---|---|
| QuickTime Animation (qtrle) in .mov, written by us (~300 lines) | none | lossless RGB(+alpha); encodes only changed rows vs the previous frame -> extremely fast and tiny files for this content (static bg + a few changing digits); accepted by Premiere/AE/Vegas/DaVinci/YouTube | not playable in browsers / most phone players |
| H.264 via x264 in .mp4 | GPL-2+ (fMCG is GPL-3: compatible); Linux `libx264-dev`; Windows needs building x264 (nasm for asm, else ~3-5x slower) | plays everywhere; small files | lossy, yuv420p colour edges; slow for multi-hour videos (NoiseMidi = 655,371 frames); build complexity on Windows |
| H.264 via OpenH264 in .mp4 | BSD-2; Cisco's prebuilt DLL (royalty-covered) or source | plays everywhere | weaker quality/speed than x264 |
Decision B: x264 .mp4 is the DEFAULT (plays everywhere; Linux via the system lib, Windows via a
bootstrap step), built-in qtrle .mov is the second format (lossless, fast, transparent option).
Muxer: one small ISO-BMFF writer of our own (moov/mdat, stts/stsz/stco/co64, mvhd/tkhd/mdhd/hdlr,
avcC for H.264 / stsd 'rle ' for qtrle), streaming-friendly: mdat written as frames arrive, moov at
the end, 64-bit offsets (co64) for files > 4 GB.

### 2.5 Threads, progress, cancel
Render thread pool: workers rasterize frames in order-numbered chunks; the encoder consumes in order
(qtrle needs the previous frame -> each chunk's worker also produces its first frame's predecessor).
Progress = frames written / total; cancel flag checked per frame; partial file deleted on cancel/error.
"FFmpeg threads" setting becomes "Encoder threads" (same meaning, same saved key migrated).

### 2.6 What goes away
`generate_ass` + the .ass temp file, `src/fmcg_render.*` (ffmpeg args), `spawn_ffmpeg_cancellable`,
the FFmpeg requirement in README, `test/cpp/test_ffspawn.cpp`, `test/cpp/test_double_render.cpp`.
(Keep `generate_ass` behind a debug export until the new path is proven, then delete.)

## 3. Testing
- Golden-image tests: render selected frames of fixture files with a bundled open font
  (e.g. DejaVu Sans, Bitstream Vera licence) and compare against stored PNG/raw hashes; pixel-exact
  on the same build, tolerance-based across compilers.
- Encoder tests: encode N frames, decode them back (qtrle decoder is tiny; for x264 check with
  ffprobe if available, else structure checks of the MP4 boxes) and compare pixels (lossless path).
- Layout tests: extents never exceed the frame for every alignment / custom position at sample
  resolutions (goal 5 properties), ASS-free.
- Muxer structure tests: box tree, sample counts == frames, duration == frames / fps.

## 4. Implementation order
1. DONE: FrameText refactor (`FrameTextSource` in fmcg_format; ASS output verified byte-identical on 4 configs).
2. DONE: text layout + raster (`src/fmcg_text`: HarfBuzz + stb_truetype, libass-compatible size, OS/2 win
   metrics, 4 subpixel phases, outline by disc dilation, straight-alpha blending, dirty-rect FrameRenderer).
   Tests (`test/cpp/test_text.cpp`, system font, skips without one): incremental == fresh render, custom
   position clamped on screen, corner alignment, outline growth, OpenType features applied, transparent bg.
   No bundled font, so invariant tests instead of stored golden images. Deps: HarfBuzz 14.5.0 + x264
   (b35605ac) + NASM 3.02 via bootstrap.bat (SHA-256 pinned); Linux via pkg-config.
   Behaviour fix vs ASS: custom x,y really is the block's TOP-LEFT (libass anchored \pos at the
   selected corner alignment).
3. DONE (needs a visual check by the user): preview shows the renderer's own frame (`draw_canvas` in
   gui/preview.cpp: FrameRenderer at video resolution, dirty-rect GL texture upload, scaled to fit with a
   neutral letterbox so the frame edges are visible); processed preview takes its text from
   FrameTextSource (identical to the encoder). (Render still used ASS + FFmpeg until step 4.)
4. DONE: `src/fmcg_video` -- ISO-BMFF writer (MP4/MOV, co64, stss), qtrle encoder (24-bit / 32-bit ARGB,
   delta lines inside the dirty rect, keyframe every 2 s, FFmpeg-compatible packets), x264 encoder (no
   B-frames, CRF 18, veryfast, BT.709, dirty-rect RGBA->I420), `render_video` (progress, cancel deletes the
   partial file). GUI: Format (MP4 default / MOV) + Transparent background (MOV) in Global; Outline on /
   width / colour in Pattern. Tests: test_video (bit-exact qtrle via an independent decoder + FFmpeg,
   H.264 structure + FFmpeg PSNR), test_render (MIDI -> file end to end, bit-exact MOV frames, duration,
   cancel). Speed (1080p60, FINAL.mid, 14,245 frames): MP4 503 fps (8.4x realtime, 7.3 MB), MOV 2,100 fps.
5. DONE with step 4 (x264 integration, bootstrap/release/README).
6. DONE: FFmpeg path removed (fmcg_render.*, spawn_ffmpeg_cancellable, ASS temp files, test_ffspawn /
   test_double_render); README / AGENTS / licences updated. `generate_ass` kept as a tested library fn.
