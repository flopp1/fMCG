import csv, sys

# Row-by-row comparison of fMCG's CSV against the independent reference CSV.
a_path, b_path = sys.argv[1], sys.argv[2]
a = list(csv.DictReader(open(a_path)))
b = list(csv.DictReader(open(b_path)))
print(f'{a_path}: {len(a)} rows | {b_path}: {len(b)} rows')
if len(a) != len(b):
    print(f'ROW COUNT MISMATCH: {len(a)} vs {len(b)}')
mism = 0
shown = 0
int_cols = ['cumulative_notes', 'current_nps', 'peak_nps', 'polyphony', 'peak_polyphony']
for i, (ra, rb) in enumerate(zip(a, b)):
    bad = []
    for col in int_cols:
        if int(ra[col]) != int(rb[col]):
            bad.append(f'{col}: {ra[col]} vs {rb[col]}')
    if abs(float(ra['timestamp_sec']) - float(rb['timestamp_sec'])) > 0.002:
        bad.append(f"t: {ra['timestamp_sec']} vs {rb['timestamp_sec']}")
    if abs(float(ra['bpm']) - float(rb['bpm'])) > 0.01:
        bad.append(f"bpm: {ra['bpm']} vs {rb['bpm']}")
    if bad:
        mism += 1
        if shown < 10:
            print(f'row {i}: ' + '; '.join(bad))
            shown += 1
if mism == 0:
    print(f'ALL {len(a)} ROWS MATCH')
else:
    print(f'{mism}/{len(a)} rows MISMATCH')
    sys.exit(1)
