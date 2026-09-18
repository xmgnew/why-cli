#!/usr/bin/env python3
"""Linux end-to-end measurements; stdout contains aggregate JSON only.

All children belong to dedicated process groups and are stopped on exit. This
script creates no files; callers may redirect its JSON under a build directory.
"""
import argparse
import json
import math
import os
import re
import signal
import subprocess
import sys
import time
import uuid


def distribution(values):
    values = sorted(values)
    if not values:
        return None
    return {"count": len(values), **{
        f"p{p}": values[math.ceil(len(values) * p / 100) - 1]
        for p in (50, 95, 99)
    }}


def recorder_usage(pid):
    # comm can contain spaces and ')'; fields after its final ')' start at state.
    with open(f"/proc/{pid}/stat", encoding="utf-8") as source:
        fields = source.read().rsplit(") ", 1)[1].split()
    ticks = int(fields[11]) + int(fields[12])
    return ticks / os.sysconf("SC_CLK_TCK"), int(fields[21]) * os.sysconf("SC_PAGE_SIZE")


def stop(process):
    if process is None or process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("why", help="Path to a Release why executable")
    parser.add_argument("--warmup", type=float, default=12)
    parser.add_argument("--queries", type=int, default=40)
    parser.add_argument("--interval", type=float, default=0.5)
    parser.add_argument("--load", choices=("none", "cpu", "churn"), default="none")
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("live measurements require Linux")
    if not (1 <= args.queries <= 1000 and 0 <= args.warmup <= 300
            and 0 <= args.interval <= 10):
        parser.error("queries: 1..1000; warmup: 0..300; interval: 0..10")
    env = dict(os.environ, WHY_SOCKET_NAME="bench-" + uuid.uuid4().hex,
               PYTHONDONTWRITEBYTECODE="1")
    watch = workload = None
    try:
        watch = subprocess.Popen([os.path.abspath(args.why), "watch"], env=env,
                                 stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                                 start_new_session=True, text=True)
        # Startup queries establish readiness but are excluded from latency stats.
        deadline = time.monotonic() + 10
        while True:
            probe = subprocess.run([args.why], env=env, capture_output=True,
                                   text=True, timeout=6)
            if probe.returncode == 0:
                break
            if watch.poll() is not None or time.monotonic() >= deadline:
                raise RuntimeError("recorder failed to become ready")
            time.sleep(0.1)
        if args.load != "none":
            code = ("while True: pass" if args.load == "cpu" else
                    "import subprocess,time\nwhile True:\n subprocess.run(['true'],check=True)\n time.sleep(.05)")
            workload = subprocess.Popen([sys.executable, "-c", code], env=env,
                                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                        start_new_session=True)
        time.sleep(args.warmup)
        cpu_start, rss = recorder_usage(watch.pid)
        start = time.monotonic()
        latencies, ages = [], []
        failures = 0
        for _ in range(args.queries):
            began = time.monotonic()
            reply = subprocess.run([args.why, "cpu", "60s"], env=env,
                                   capture_output=True, text=True, timeout=6)
            elapsed = (time.monotonic() - began) * 1000
            if reply.returncode:
                failures += 1
            else:
                latencies.append(elapsed)
                age = re.search(r"Latest observation age: ([0-9.]+) seconds", reply.stdout)
                if age:
                    ages.append(float(age.group(1)))
            _, current_rss = recorder_usage(watch.pid)
            rss = max(rss, current_rss)
            if workload and workload.poll() is not None:
                raise RuntimeError("controlled workload exited early")
            time.sleep(args.interval)
        wall = time.monotonic() - start
        cpu_end, _ = recorder_usage(watch.pid)
        print(json.dumps({"schema": 1, "mode": "ipc", "load": args.load,
                          "warmup_seconds": args.warmup,
                          "query_interval_seconds": args.interval,
                          "queries_requested": args.queries, "failures": failures,
                          "query_wall_ms": distribution(latencies),
                          "reported_observation_age_seconds": distribution(ages),
                          "recording_wall_seconds": wall,
                          "recorder_cpu_seconds": cpu_end - cpu_start,
                          "recorder_one_core_pct": 100 * (cpu_end - cpu_start) / wall,
                          "sampled_peak_recorder_rss_bytes": rss}, indent=2))
        return int(failures != 0)
    finally:
        stop(workload)
        stop(watch)
        if watch and watch.stderr:
            watch.stderr.close()


if __name__ == "__main__":
    # Turn interruption into normal stack unwinding so owned children are reaped.
    def interrupted(signum, frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, interrupted)
    try:
        sys.exit(main())
    except (KeyboardInterrupt, RuntimeError, OSError, subprocess.TimeoutExpired) as error:
        print(f"Measurement incomplete: {error}", file=sys.stderr)
        sys.exit(1)
