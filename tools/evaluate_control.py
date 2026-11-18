#!/usr/bin/env python3
# 主机工具与自动验证入口，不连接硬件进行隐式控制。
"""Evaluate resistance-control CSV by boot/run/config; output use is not measured energy."""
import argparse
import csv
import json
import math
from collections import defaultdict
from pathlib import Path


def evaluate(rows, tolerance=.05, hold_seconds=2):
    groups = defaultdict(list)
    for row in rows:
        if row.get('mode') != '1':
            continue
        key = tuple(
            row[k] for k in ('boot_id', 'run_id', 'config_id', 'algorithm'))
        groups[key].append(row)
    output = []
    for key, frames in groups.items():
        times = [int(f['cycle_start_us']) * 1e-6 for f in frames]
        if any(b <= a for a, b in zip(times, times[1:])):
            raise ValueError('timestamps must increase within each run')
        channels = []
        for i in range(4):
            samples = []
            invalid = 0
            missing_intervals = 0
            for f, t in zip(frames, times):
                resistance = f[f'R{i+1}']
                if not resistance or not (int(f['valid_mask']) & (1 << i)):
                    invalid += 1
                    continue
                target = float(f[f'target{i+1}'])
                r = float(resistance)
                duty = float(f[f'duty{i+1}'])
                if not all(math.isfinite(x) for x in [target, r, duty]):
                    raise ValueError('non-finite control record')
                samples.append((t, r - target, duty, f))
            if not samples:
                channels.append({
                    'channel': i + 1,
                    'valid': 0,
                    'invalid': invalid
                })
                continue
            iae = ise = usage = variation = 0
            settled = None
            band_start = None
            previous = None
            for t, e, d, f in samples:
                contiguous = previous is not None and abs(t - previous[0] -
                                                          .02) < 1e-5
                if previous is not None:
                    if contiguous:
                        iae += (abs(previous[1]) + abs(e)) * .01
                        ise += (previous[1]**2 + e**2) * .01
                        usage += (previous[2] + d) * .01
                        variation += abs(d - previous[2])
                    else:
                        missing_intervals += 1
                if abs(e) <= tolerance:
                    if band_start is None or (previous is not None and
                                              not contiguous):
                        band_start = t
                    if settled is None and t - band_start >= hold_seconds:
                        settled = band_start - times[0]
                else:
                    band_start = None
                previous = (t, e, d)
            steady = [e for t, e, d, f in samples if t >= times[-1] - 5]
            channels.append({
                'channel':
                    i + 1,
                'valid':
                    len(samples),
                'invalid':
                    invalid,
                'missing_intervals':
                    missing_intervals,
                'rmse_ohm':
                    math.sqrt(
                        sum(e * e for t, e, d, f in samples) / len(samples)),
                'iae_ohm_s':
                    iae,
                'ise_ohm2_s':
                    ise,
                'steady_signed_error_ohm':
                    sum(steady) / len(steady),
                'overshoot_ohm':
                    max(0, max(e for t, e, d, f in samples)),
                'settling_time_s':
                    settled,
                'pwm_integral_percent_s':
                    usage,
                'pwm_total_variation_percent':
                    variation,
                'limited_fraction':
                    sum(
                        bool(int(f['limited_mask']) & (1 << i))
                        for t, e, d, f in samples) / len(samples),
                'blocked_fraction':
                    sum(
                        bool(int(f['blocked_mask']) & (1 << i))
                        for t, e, d, f in samples) / len(samples)
            })
        output.append(
            dict(zip(('boot_id', 'run_id', 'config_id', 'algorithm'), key),
                 duration_s=times[-1] - times[0],
                 records=len(frames),
                 channels=channels,
                 max_computation_us=max(
                     int(f['computation_us']) for f in frames)))
    return {
        'tolerance_ohm': tolerance,
        'hold_seconds': hold_seconds,
        'experiments': output
    }


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('csv', type=Path)
    p.add_argument('--tolerance', type=float, default=.05)
    p.add_argument('--hold-seconds', type=float, default=2)
    p.add_argument('--output', type=Path)
    a = p.parse_args()
    if not math.isfinite(a.tolerance) or a.tolerance <= 0 or not math.isfinite(
            a.hold_seconds) or a.hold_seconds <= 0:
        p.error('tolerance and hold-seconds must be finite and positive')
    with a.csv.open(newline='') as f:
        result = evaluate(list(csv.DictReader(f)), a.tolerance, a.hold_seconds)
    body = json.dumps(result, indent=2, ensure_ascii=False) + '\n'
    if a.output:
        with a.output.open('x') as f:
            f.write(body)
    else:
        print(body, end='')


if __name__ == '__main__':
    main()
