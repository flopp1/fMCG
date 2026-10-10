# OPEN_QUESTIONS.md -- decisions + unresolved items (update every session)

## Decided by the user
1. (2026-10-10) Held notes: canonical polyphony = ons - offs (MIDI spec). Closing notes still held at a
   track's final tick is an OPTIONAL GUI setting, default OFF. Engine API:
   `fmcg_stream::set_close_held_notes(bool)` (probe arg 5). GUI checkbox wiring still TODO.
2. (2026-10-10) Decode mismatches: count only what decodes; report mismatch vs declared.
3. (2026-10-10) Tempo is GLOBAL: layered result must equal a flat scan (pushed once per run; `tempo_diff` dialect).
4. (2026-10-10, session 3) Numeric ranges: every non-tick stat <= 93,823,560,428,205 (max notes a MIDI can
   hold: every event in every track a note-on, no offs) -> 64-bit everywhere, never 32-bit. Ticks unbounded:
   99% <= 2^28, max seen 1.4e9, the rest must still work (uint64 ticks, no dense-array assumptions that break
   beyond 2^31).
5. (2026-10-10, session 3) Peak-NPS window: INCLUSIVE of the 1.0 s boundary (an event exactly 1 s before
   still counts). Oracle and engine must both implement this.
6. (2026-10-10, session 3) Scope: layered files only for now. `nut3.7z` and `Deadly apple rev2.rar.xz` are
   left out of testing for the time being.
7. (2026-10-10, session 3) BSOD ("When Systematic & Inferno has BSOD") is deferred to the session after the
   workspace transition (goal 1 + builds + tests first). happy can be tested now (local box: 32 GB RAM, 12 cores).

## Still open / known gaps
- BSOD structure (session 3, verified): root xz (35 MB) -> 1 member (376 MB, ONE xz block, dict 4 GiB, made
  by 7-Zip) whose payload is 2.3 TB of layer-3 members: a 72-byte MThd member, then a 16-member period
  (16 x 256 MB raw slices of a ~4.29 GB MTrk, cut mid-event, running status) repeated ~66,000 times
  (~283 TB expanded). Every member decodes cleanly -- session 2's "damaged members" was the old walker
  misreading the structure. Shrunk BSOD (header + 2.4 periods): layered == standard exactly.
  Cost: the 2.3 TB layer-2 payload must be decoded once to learn the member sequence (single block, so no
  block-level dedup; LZMA2 range-coder state carries across chunks, so no sound skip). libarchive decodes it
  at ~2-3.4 GB/s => measured 16 min total on this box. That is inherent to the file, not the walk.
- Standard (non-dedup) path on mixed-depth constructions differs by design (DESIGN.md).
- .7z inside a compression filter (.7z.xz): NOT supported -- 7z needs random access to its end header, so it
  cannot be streamed (user decision: only support it if streamable). The engine now says so explicitly.

## Corpus status (layered walk, session 3, 2026-10-10; identical on Windows + Linux)
| file | notes | ticks | ppqn | peak poly | peak NPS | time | how verified |
|---|---|---|---|---|---|---|---|
| Ievans Polka | 53,745,997,855,018 | 9,216,000 | 7680 | 53,745,997,824,005 | 322,487,650,862 | 5-8 s | shrunk K=2/3/5 == standard |
| Just 1 note | 93,823,560,428,205 | 0 | 480 | = notes | = notes | 19 s | name; shrunk == standard |
| MIDI File | 7,385,042,433,600 | 178,560 | 480 | 722,710,800 | 210,354,934,050 | 4 s | base.mid (in the author's folder) x61455 exact; shrunk == standard. The '7.8 trillion' title = 65,535 copies, but the 255x-repeated layer-2 block holds 15 (not 16) sub-blocks: 134,937,420 = 15 x 8,995,828 in the xz index. Session 2 was WRONG (7,354,400,010,510) |
| NoiseMidi | 93,823,560,428,205 | 1,431,655,762 | 65535 | = notes | 8,589,737,985 | 41-47 s | shrunk K=1 layered == standard TWO-PASS (notes/ticks/ppqn/frames); grid dialect |
| gymnopedie | 46,836,487,526,682 | 54,144 | 384 | 865,035,607 | 333,038,706,010 | 24-27 s | shrunk == standard |
| happy | 35,180,980,081,758 | 4,608 | 96 | 308,594,756,484 | 20,671,137,314,100 | 16-19 s | shrunk == standard (never completed before) |
| paprikas | 2,661,824,754,708 | 867,869 | 30720 | 43,842,246 | 1,546,303,630,494 | 2-3 s | shrunk == standard |
| BSOD | 93,715,050,000,000 | 2,232,234 | 2048 | = notes | 77,995,697,368,890 | 16 min | 65,535 tracks x exactly 1.43e9 notes; shrunk == standard; time = decoding the 2.3 TB layer-2 payload |
Expected values live in `test/expected/corpus_layered.json` (checked by `run_tests.py --level full`).
