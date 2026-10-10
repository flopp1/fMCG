# DIALECTS.md -- construction dialects + test generators

Generators: `test/dialects/gen.py` (GENS). Oracle: `test/dialects/oracle.py`.
Run: `python -I test/run_tests.py -k dialect [--seeds N]` (or the table view:
`python -I test/dialects/run.py <abs probe path> --seeds N --dedup 1|0|both [--close-held 1]`).

| kind | what | layered | standard |
|---|---|---|---|
| plain, xz1 | ordinary / single xz | pass | pass |
| tiled_members | header member + tiled track members, nested layers | pass | n/a (mixed depth) |
| tiled_whole | whole file repeated (multi MThd) | pass | pass |
| dump_midevent | headerless running-status dump under a fiction MTrk stub, cuts >= 8 B | pass | pass |
| dump_tinycuts | cuts of 1-7 B (parse phase shifts each tile) | pass (was a known limit before session 3) | pass |
| dump_tiled_slices | fixed-size slices of an endless triplet tile (phase-shifted uniques) | pass | pass |
| mthd12 | 12-byte MThd, no division | pass | pass |
| tempo_diff | tiles with differing tempo patterns at shared ticks, interleaved/nested | pass | n/a (mixed depth) |
| mixed_checks, empty_fragments | xz check types / empty members | pass | pass |
| grid | uniform grid (NoiseMidi-like) | pass | pass |
| tempo_in_tiles, held_notes | tempo meta in tiles / notes held at track end | pass | pass |

All verified at 200 seeds (layered + standard) and 100 seeds with close-held on, Windows and Linux.

Add a dialect: write a generator returning (xz bytes, description), register it in GENS, run.
"Mixed depth" = a raw member next to members whose payload is more xz (see DESIGN.md).
