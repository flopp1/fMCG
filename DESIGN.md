# DESIGN.md -- layered dedup (src/fmcg_layered.cpp)

## History
Sessions 1-2 built `walk_layer` (~2,900 lines in fmcg_engine.cpp): members were found by
scanning RAW bytes for magics, then classified by content heuristics (dump-slice phase guesses,
uniform-grid verifiers, body-stream folds, "outer unwrap" retries, standard-scan fallbacks).
Each corpus file needed new heuristics; tiny cuts were a known limit; MIDI File was silently
miscounted (61,200 vs 61,455 tile copies); happy fell back to an infeasible full scan.
Session 3 (2026-10-10) deleted it and replaced it with the design below. The old file is kept
locally in `.agent/fmcg_engine_session2.cpp.bak` (gitignored) for reference only.

## Definition of correct
Layered result == the STANDARD scan (scan_stream + parse_track_body + sweep_to_frames) of the
fully expanded flat byte stream. `test/dialects/oracle.py` is the executable reference and
mirrors the standard scanner rule by rule (see its docstring). If you change a parsing rule in
the engine, change it in the oracle AND in `Parser` (fmcg_layered.cpp) in the same commit.

## Architecture
1. **Discovery** (`Discovery`): the file must be a chain of xz streams; it is cut BACKWARD
   (footer -> index -> stream start; no decoding). Members are keyed (xxh64, length). Each UNIQUE
   member is decoded exactly once: payload starting with the xz magic => internal node, its
   payload is cut FORWARD into child members at CRC-validated footers whose index walks back
   exactly to the member start (`xz_stream_exact`; an embedded copy of another stream can never
   validate), with a fast path that tries recently seen member lengths first. Children are
   stored run-length. Otherwise => leaf (raw MIDI bytes; compressed bytes kept, decoded again
   only when parsed). Non-xz bytes after members => RAW tail node. A member with no valid
   footer => truncated node (decodable prefix only). Result: a grammar (DAG); its in-order leaf
   expansion IS the flat stream. Expanded size is computed from xz indexes for the log.
2. **Walk** (`Walk`, `Composer`, `Parser`): a resumable byte-level transducer that mirrors the
   standard scan (chunk framing, first-MThd quirks incl. 12-byte MThd, MTrk limits, VLQ
   truncation, running status, meta/sysex skips, tempo, refcounted offs, optional held-note
   closure). The effect of a node is memoized per (node, `Shape`):
   - `Shape` = full parser state minus the affine parts: tick, `rem` (bytes left in the current
     chunk), `ctr` (bytes left in a meta/sysex skip), and the refcount table.
   - An `Effect` records: end shape; counter validity (`*_abs` = counter ended/absolute, else the
     amount consumed => valid for any start value > consumed); tick advance (`init_ended=false`)
     or the absolute tick of the track open at the end; output items (contribution id, frame
     REL|ABS, shift, arithmetic progression, weight); tempo events in stream order; refcount
     behaviour of the initial track as per-key (deficit d, net); refcounts of the open track if
     it started inside; track ends; division.
   - Refcounts: a GENERIC effect is computed assuming every note-off in the initial track fires;
     it is exact whenever incoming refcount[key] >= d[key]. Otherwise the node is re-walked with
     the concrete refcounts (memoized by refcount hash).
   - Composition (`Composer::apply`) shifts REL items by the tick advance, composes deficits
     `d = max(d1, d2 - net1)`, and converts to ABS when a track ends.
   - Runs: after one copy of a child, if the shape maps onto itself and the next lookup returns
     the same effect, the remaining copies are folded analytically (`power`), bounded by the
     chunk/skip counters and refcount deficits. Periods 2..8 (tiles whose length shifts the parse
     phase, e.g. tiny cuts) are detected and folded the same way via a composite effect.
3. **Materialize**: items expand into `TickData::sparse` (sorted cells) and `TickData::runs`
   (arithmetic tick runs, may overlap); the sweep (`sweep_to_frames`) runs in event mode for
   these, so tick spans of 1.4e9+ need no dense arrays.

## Numeric ranges (user decision 2026-10-10)
All non-tick stats <= 93,823,560,428,205 -> 64-bit everywhere. Ticks uint64 (seen up to 1.4e9).
NPS window time is exact integer (tick x us/quarter, in 1/ppqn us) using unsigned __int128.

## Known semantic difference (by design)
Mixed-depth constructions -- a raw-MIDI member next to sibling members whose payload is more xz
-- are defined only by the layered walk/oracle (each tile expands independently). The standard
decoder only unwraps a stream that starts compressed, so it reads the inner xz bytes as data.
No corpus file is built this way; the `tiled_members` / `tempo_diff` dialects are layered-only.

## Not handled yet
- Members that fail to decode count their decodable prefix and warn (none in the corpus; BSOD was not damaged).
- Containers other than xz inside layers (7z/rar/gz members): the walk returns "not applicable"
  only when the FILE is not xz; a non-xz payload inside is treated as a leaf. The flat stream must
  then start with MThd or the scan falls back to the standard path.
