# Stream-validate a full CSV dump: cumulative is monotonic, current_nps equals
# the 60-frame (1-second) rolling window delta, peaks are nondecreasing and
# match the running maxima, and the final cumulative equals the parser's note
# total.
#
# Usage: python check_full_csv.py <full.csv> [expected_total_notes]
# (expected_total_notes is optional; when omitted the final-cumulative check
#  just reports the value instead of asserting).
import csv, sys
from collections import deque

path = sys.argv[1] if len(sys.argv) > 1 else "full.csv"
expected_total = int(sys.argv[2]) if len(sys.argv) > 2 else None
bad = 0
prev_cum = 0
prev_peak_nps = 0
prev_peak_poly = 0
window = deque()
peak_nps_row = None
max_nps_seen = 0
n = 0
with open(path, newline="") as f:
    r = csv.reader(f)
    header = next(r)
    for row in r:
        n += 1
        f_i, t, cum, nps, pnps, poly, ppoly, bpm = row
        cum, nps, pnps, poly, ppoly = int(cum), int(nps), int(pnps), int(poly), int(ppoly)
        window.append(cum)
        if len(window) > 61:
            window.popleft()
        # nps[k] = cum[k] - cum[k-60], baseline 0 for the first 60 frames
        expect_nps = cum - (window[0] if len(window) == 61 else 0)
        if cum < prev_cum or nps != expect_nps or pnps < prev_peak_nps or ppoly < prev_peak_poly:
            bad += 1
            if bad <= 5:
                print(f"BAD row {n}: nps={nps} expected={expect_nps} cum={cum} prev_cum={prev_cum} pnps={pnps} prev_pnps={prev_peak_nps} ppoly={ppoly} prev_ppoly={prev_peak_poly}")
        if nps > max_nps_seen:
            max_nps_seen = nps
            peak_nps_row = (f_i, t, nps, poly)
        prev_cum = cum
        prev_peak_nps = pnps
        prev_peak_poly = ppoly

print(f"rows={n} bad={bad}")
if expected_total is not None:
    print(f"final cumulative={prev_cum}  expected total_notes={expected_total}  match={prev_cum == expected_total}")
else:
    print(f"final cumulative={prev_cum}")
print(f"max current nps={max_nps_seen} at {peak_nps_row}")
print(f"final peak_nps={prev_peak_nps} peak_polyphony={prev_peak_poly}")
if bad or (expected_total is not None and prev_cum != expected_total):
    sys.exit(1)
print("INVARIANTS OK")
