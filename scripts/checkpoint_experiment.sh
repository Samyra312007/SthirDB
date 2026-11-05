#!/usr/bin/env bash
# Phase 5B, Part B — checkpoint-interval sweep.
#
# Variable: checkpoint interval in log records — none, 100, 500, 1,000, 5,000.
# Fixed:    the same seeded workload, the same data size, and the same crash
#           point (a hard kill after the last commit, so every dirty page is
#           still only in the buffer pool and recovery must redo it).
# Metrics:  normal throughput (txn/s), recovery time, redo records replayed,
#           and bytes of log scanned during recovery.
# Runs 5 repetitions per setting and reports the average.
#
#   ./scripts/checkpoint_experiment.sh
#
# Evidence written (regenerated every run, never edited by hand):
#   docs/evidence/phase5b_checkpoint_sweep.csv   one row per run
#   docs/evidence/phase5b_checkpoint_sweep.md    averaged table
#   docs/evidence/phase5b_plot_interval.svg      throughput + recovery vs interval
#   docs/evidence/phase5b_plot_logsize.svg       recovery vs log scanned

set -u -o pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

BIN=./build/sthirdb
EVID=docs/evidence
DIR=./data/ckpt
REPS=${REPS:-5}
TXNS=${TXNS:-1200}   # ~5 log records per txn -> ~6,000 records, so 5,000 fires
CSV="$EVID/phase5b_checkpoint_sweep.csv"

mkdir -p "$EVID"
make -s all || { echo "build failed"; exit 1; }

echo "interval,rep,committed,elapsed_ms,throughput_txn_per_s,wal_bytes,recovery_time_ms,redo_applied,redo_skipped,log_scanned_bytes" > "$CSV"

field() { sed -n "s/.*$1=\([0-9.]*\).*/\1/p" "$2" | head -1; }

for interval in none 100 500 1000 5000; do
  for rep in $(seq 1 "$REPS"); do
    rm -rf "$DIR"
    "$BIN" trial-init --dir "$DIR" > /dev/null || { echo "trial-init failed"; exit 1; }

    # Same workload, same crash point: a hard kill right after the last commit.
    # The crash fires with no cleanup, so every dirty page is still in memory
    # and only the log is durable.
    run_args="--dir $DIR --seed 1 --txns $TXNS --crash-end"
    if [ "$interval" != "none" ]; then
      run_args="$run_args --checkpoint $interval"
    fi
    STHIRDB_CRASH_POINT=end_of_workload STHIRDB_CRASH_SKIP=0 \
      "$BIN" trial-run $run_args > "$DIR/run.out" 2>&1
    st=$?
    if [ "$st" -ne 99 ]; then
      echo "trial-run did not crash as asked (exit $st) for interval=$interval rep=$rep"
      cat "$DIR/run.out"
      exit 1
    fi

    "$BIN" trial-recover --dir "$DIR" > "$DIR/recover.out" 2>&1 || {
      echo "recovery failed for interval=$interval rep=$rep"; cat "$DIR/recover.out"; exit 1; }

    # A hard exit never flushes stdio, so the run's numbers come from the
    # metrics file trial-run writes just before the crash point.
    committed=$(field committed "$DIR/trial.metrics")
    elapsed=$(field elapsed_ms "$DIR/trial.metrics")
    wal_bytes=$(field wal_bytes "$DIR/trial.metrics")
    rec_ms=$(field recovery_time_ms "$DIR/recover.out")
    redo=$(field redo_applied "$DIR/recover.out")
    redo_skip=$(field redo_skipped "$DIR/recover.out")
    scanned=$(field log_scanned_bytes "$DIR/recover.out")

    thr=$(python3 -c "print('%.2f' % ($committed / ($elapsed/1000.0)))")
    echo "$interval,$rep,$committed,$elapsed,$thr,$wal_bytes,$rec_ms,$redo,$redo_skip,$scanned" >> "$CSV"
    echo "  interval=$interval rep=$rep committed=$committed elapsed=${elapsed}ms recovery=${rec_ms}ms scanned=${scanned}B"
  done
done

# ---------------------------------------------------------------------------
# Averaged table
# ---------------------------------------------------------------------------
python3 - "$CSV" "$EVID/phase5b_checkpoint_sweep.md" <<'PY'
import csv, sys

csv_path, md_path = sys.argv[1], sys.argv[2]
rows = list(csv.DictReader(open(csv_path)))
intervals = ["none", "100", "500", "1000", "5000"]

def avg(interval, key):
    vals = [float(r[key]) for r in rows if r["interval"] == interval]
    return sum(vals) / len(vals) if vals else 0.0

lines = []
lines.append("# Phase 5B Part B — checkpoint-interval sweep")
lines.append("")
lines.append("Fixed workload: %d transactions per run (seed 1), hard crash after the last"
             % int(avg("none", "committed")))
lines.append("commit. Each row averages %d repetitions. Times are milliseconds."
             % len([r for r in rows if r["interval"] == "none"]))
lines.append("")
lines.append("| checkpoint interval (log records) | throughput (txn/s) | recovery time (ms) | WAL bytes | log scanned (bytes) | redo records applied |")
lines.append("|---|---:|---:|---:|---:|---:|")
for it in intervals:
    label = "none" if it == "none" else it.replace("5000", "5,000").replace("1000", "1,000")
    lines.append("| %s | %.1f | %.2f | %d | %d | %.0f |" % (
        label, avg(it, "throughput_txn_per_s"), avg(it, "recovery_time_ms"),
        int(avg(it, "wal_bytes")), int(avg(it, "log_scanned_bytes")),
        avg(it, "redo_applied")))
lines.append("")
open(md_path, "w").write("\n".join(lines) + "\n")
print("\n".join(lines))
PY

# ---------------------------------------------------------------------------
# Plots (pure-Python SVG: matplotlib is not a project dependency)
# ---------------------------------------------------------------------------
python3 - "$CSV" <<'PY'
import csv, sys

csv_path = sys.argv[1]
rows = list(csv.DictReader(open(csv_path)))
intervals = ["none", "100", "500", "1000", "5000"]
labels = ["none", "100", "500", "1,000", "5,000"]

def avg(interval, key):
    vals = [float(r[key]) for r in rows if r["interval"] == interval]
    return sum(vals) / len(vals) if vals else 0.0

W, H = 760, 460
L, R, T, B = 90, 95, 60, 80
plot_w, plot_h = W - L - R, H - T - B

def sx(i, n):
    return L + (plot_w / 2 if n == 1 else plot_w * i / (n - 1))

def sy(v, lo, hi):
    if hi <= lo:
        return T + plot_h / 2
    return T + plot_h - (v - lo) * plot_h / (hi - lo)

def frame(title, xlabel, ylab_left, ylab_right, lo_l, hi_l, lo_r, hi_r, ticks_l, ticks_r):
    out = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
           'font-family="Helvetica,Arial,sans-serif" font-size="13">' % (W, H),
           '<rect width="%d" height="%d" fill="#ffffff"/>' % (W, H),
           '<text x="%d" y="28" font-size="16" font-weight="bold">%s</text>' % (L, title),
           '<rect x="%d" y="%d" width="%d" height="%d" fill="#fbfbfb" stroke="#999"/>'
           % (L, T, plot_w, plot_h)]
    for v in ticks_l:
        y = sy(v, lo_l, hi_l)
        out.append('<line x1="%d" y1="%.1f" x2="%d" y2="%.1f" stroke="#e4e4e4"/>'
                   % (L, y, L + plot_w, y))
        out.append('<text x="%d" y="%.1f" text-anchor="end" fill="#1f4e79">%g</text>'
                   % (L - 8, y + 4, v))
    for v in ticks_r:
        y = sy(v, lo_r, hi_r)
        out.append('<text x="%d" y="%.1f" fill="#a33">%g</text>' % (L + plot_w + 8, y + 4, v))
    out.append('<text x="%d" y="%d" text-anchor="middle" fill="#1f4e79" font-weight="bold">%s</text>'
               % (L - 52, T + plot_h // 2, ylab_left))
    out.append('<text x="%d" y="%d" text-anchor="middle" fill="#a33" font-weight="bold" '
               'transform="rotate(90 %d %d)">%s</text>'
               % (W - 18, T + plot_h // 2, W - 18, T + plot_h // 2, ylab_right))
    out.append('<text x="%d" y="%d" text-anchor="middle">%s</text>'
               % (L + plot_w // 2, H - 30, xlabel))
    return out

def axis_labels(out, names, xs):
    for name, x in zip(names, xs):
        out.append('<text x="%.1f" y="%d" text-anchor="middle">%s</text>' % (x, T + plot_h + 22, name))

def series(out, xs, ys, color, lo, hi):
    pts = " ".join("%.1f,%.1f" % (x, sy(y, lo, hi)) for x, y in zip(xs, ys))
    out.append('<polyline points="%s" fill="none" stroke="%s" stroke-width="2.5"/>' % (pts, color))
    for x, y in zip(xs, ys):
        out.append('<circle cx="%.1f" cy="%.1f" r="4" fill="%s"/>' % (x, sy(y, lo, hi), color))

def legend(out, items):
    x = L + 12
    for name, color in items:
        out.append('<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="%s" stroke-width="2.5"/>'
                   % (x, T + 18, x + 26, T + 18, color))
        out.append('<text x="%d" y="%d" font-size="12">%s</text>' % (x + 32, T + 22, name))
        x += 40 + 7 * len(name)

def nice_ticks(lo, hi, n=5):
    if hi <= lo:
        hi = lo + 1
    step = (hi - lo) / n
    return [lo + step * i for i in range(n + 1)]

# ---- plot 1: throughput and recovery time vs checkpoint interval ----
thr = [avg(i, "throughput_txn_per_s") for i in intervals]
rec = [avg(i, "recovery_time_ms") for i in intervals]
xs = [sx(k, len(intervals)) for k in range(len(intervals))]
lo_l, hi_l = 0.0, max(thr) * 1.15 if max(thr) > 0 else 1.0
lo_r, hi_r = 0.0, max(rec) * 1.15 if max(rec) > 0 else 1.0
out = frame("Throughput vs recovery time by checkpoint interval",
            "checkpoint interval (log records)",
            "throughput (txn/s)", "recovery time (ms)",
            lo_l, hi_l, lo_r, hi_r,
            [round(v, 1) for v in nice_ticks(lo_l, hi_l)],
            [round(v, 1) for v in nice_ticks(lo_r, hi_r)])
series(out, xs, thr, "#1f77b4", lo_l, hi_l)
series(out, xs, rec, "#d62728", lo_r, hi_r)
axis_labels(out, labels, xs)
legend(out, [("throughput (txn/s)", "#1f77b4"), ("recovery time (ms)", "#d62728")])
out.append("</svg>")
open("docs/evidence/phase5b_plot_interval.svg", "w").write("\n".join(out) + "\n")

# ---- plot 2: recovery time vs log bytes scanned ----
pts = sorted(((avg(i, "log_scanned_bytes"), avg(i, "recovery_time_ms"), lab)
              for i, lab in zip(intervals, labels)))
xvals = [p[0] / 1024.0 for p in pts]   # KiB
yvals = [p[1] for p in pts]
xmax = max(xvals) * 1.15 if xvals and max(xvals) > 0 else 1.0
ymax = max(yvals) * 1.15 if yvals and max(yvals) > 0 else 1.0
out = frame("Recovery time vs bytes of log scanned",
            "log bytes scanned during recovery (KiB)",
            "recovery time (ms)", "", 0.0, ymax, 0.0, 1.0,
            [round(v, 1) for v in nice_ticks(0.0, ymax)], [])
out = [s for s in out if 'fill="#a33"' not in s]
px = [L + (plot_w * (x / xmax)) for x in xvals]
py = [sy(y, 0.0, ymax) for y in yvals]
out.append('<polyline points="%s" fill="none" stroke="#2a9d8f" stroke-width="2.5"/>'
           % " ".join("%.1f,%.1f" % (a, b) for a, b in zip(px, py)))
for (x, y, lab) in zip(px, py, [p[2] for p in pts]):
    out.append('<circle cx="%.1f" cy="%.1f" r="4" fill="#2a9d8f"/>' % (x, y))
    out.append('<text x="%.1f" y="%.1f" font-size="11" anchor="start">%s</text>' % (x + 6, y - 8, lab))
for v in nice_ticks(0.0, xmax):
    out.append('<text x="%.1f" y="%d" text-anchor="middle">%.0f</text>'
               % (L + plot_w * (v / xmax), T + plot_h + 22, v))
out.append("</svg>")
open("docs/evidence/phase5b_plot_logsize.svg", "w").write("\n".join(out) + "\n")
print("plots written")
PY

echo
echo "OK: sweep complete. Evidence: $CSV, docs/evidence/phase5b_checkpoint_sweep.md,"
echo "    docs/evidence/phase5b_plot_interval.svg, docs/evidence/phase5b_plot_logsize.svg"
