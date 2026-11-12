# Reads run.ps1's results.txt and prints, per page, the median over the passes of each figure for the two SDK builds
# and B relative to A (below 1.00 = B is cheaper).   python compare.py <results.txt>
import collections
import statistics
import sys

FIGURES = [("paint_ms", "load to first paint, ms"), ("cpu_total", "CPU over the run, ms"), ("cpu_med", "CPU per drawn frame, median ms"),
           ("cpu_p95", "CPU per drawn frame, p95 ms"), ("cpu_worst", "CPU worst frame, ms"), ("render_med", "Render, median ms"),
           ("gpu_med", "GPU per drawn frame, median ms"), ("gpu_p95", "GPU per drawn frame, p95 ms"), ("drawn", "frames drawn"),
           ("private_mb", "private memory, MB"), ("target_mb", "render targets, MB")]


def main():
    runs = collections.defaultdict(lambda: collections.defaultdict(list))   # name -> sdk -> [fields]
    sdks, names, failed = [], [], []
    for line in open(sys.argv[1], encoding="ascii", errors="replace"):
        parts = line.split()
        if not parts:
            continue
        fields = dict(p.split("=", 1) for p in parts[1:] if "=" in p)
        if parts[0] != "RESULT":
            failed.append(line.strip())
            continue
        if fields["sdk"] not in sdks:
            sdks.append(fields["sdk"])
        if fields["name"] not in names:
            names.append(fields["name"])
        runs[fields["name"]][fields["sdk"]].append(fields)
    if len(sdks) != 2:
        sys.exit("need results for exactly two SDK builds, found %s" % sdks)
    a, b = sdks
    print("A = %s, B = %s; each figure is the median of the passes; B/A below 1.00 means B is cheaper\n" % (a, b))
    for name in names:
        ra, rb = runs[name][a], runs[name][b]
        if not ra or not rb:
            print("%s: missing a side" % name)
            continue
        f0 = ra[0]
        print("%s  (%sx%s, scale %s, threads %s, %d and %d passes)" % (name, f0["w"], f0["h"], f0["scale"], f0["threads"], len(ra), len(rb)))
        for key, label in FIGURES:
            va = statistics.median(float(r[key]) for r in ra)
            vb = statistics.median(float(r[key]) for r in rb)
            spread = max(max(float(r[key]) for r in rs) - min(float(r[key]) for r in rs) for rs in (ra, rb))
            ratio = "%.2f" % (vb / va) if va > 0 else "-"
            print("  %-34s %10.3f %10.3f   B/A %5s   widest spread between passes %.3f" % (label, va, vb, ratio, spread))
        errs = max(int(r["page_errors"]) for r in ra + rb)
        if errs:
            print("  page errors: up to %d a run" % errs)
        print()
    for line in failed:
        print(line)


main()
