#!/usr/bin/env python3
"""Summarize actual client frame timelines, and compare identical replay samples.

No FPS estimate substitutes for a stage time. All CPU times are wall times;
overlap includes waits and is not CPU utilization or input-to-display latency.
"""
import argparse
import csv
import json
import math
from collections import defaultdict
from pathlib import Path


def distribution(values):
    values = sorted(values)
    if not values:
        return None

    def percentile(fraction):
        p = fraction * (len(values) - 1)
        lo, hi = math.floor(p), math.ceil(p)
        return values[lo] + (values[hi] - values[lo]) * (p - lo)

    return dict(count=len(values), mean=sum(values) / len(values),
                p50=percentile(.50), p95=percentile(.95), p99=percentile(.99), max=values[-1])


def union(intervals):
    result = []
    for start, end in sorted(intervals):
        if result and start <= result[-1][1]:
            result[-1][1] = max(end, result[-1][1])
        else:
            result.append([start, end])
    return result


def intersection_ms(left, right):
    left, right = union(left), union(right)
    a = b = 0
    total = 0
    while a < len(left) and b < len(right):
        total += max(0, min(left[a][1], right[b][1]) - max(left[a][0], right[b][0]))
        if left[a][1] < right[b][1]:
            a += 1
        else:
            b += 1
    return total


def summarize(path):
    document = json.loads(path.read_text(encoding='utf-8'))
    if document['version'] != 1:
        raise ValueError(f'{path}: unsupported trace version')
    if document.get('error'):
        raise ValueError(f'{path}: run failed: {document["error"]}')
    frames = [f for f in document['frames'] if f.get('measured') and
              f.get('producerStart', 0) > 0 and 'recordEnd' in f and 'statistics' in f]
    if not frames:
        raise ValueError(f'{path}: no completed measured frames')
    series = defaultdict(list)
    seen_gpu = set()
    measured_gpu_frames = {f.get('gpuSubmission') for f in frames}
    rows = []
    mismatches = []
    for frame in frames:
        row = dict(sequence=frame['sequence'], sample=frame.get('sourceSample'))
        for name, start, end in [
            ('producerWallMs', 'producerStart', 'submitted'),
            ('queueResidenceMs', 'submitted', 'recordStart'),
            ('recordWallMs', 'recordStart', 'recordEnd'),
            ('sceneToRecordEndMs', 'producerStart', 'recordEnd'),
            ('resultDeliveryMs', 'recordEnd', 'resultReceived'),
        ]:
            if start in frame and end in frame:
                row[name] = frame[end] - frame[start]
        row['queueWaitMs'] = frame.get('queueWaitMs', 0)
        row['outstanding'] = frame.get('outstanding', 0)
        for group in ['producerStages', 'recordStages']:
            # A repeated scope's samples belong to the same source frame.
            for scope in frame.get(group, []):
                key = group + '/' + scope['name']
                row[key] = row.get(key, 0) + scope['ms']
        for key, value in frame.get('resident', {}).items():
            row['resident/' + key] = value
        for key, value in frame['statistics'].items():
            row['draw/' + key] = value
        for key in ['vramUsage', 'vramBudget', 'producerCpuMs', 'recordCpuMs']:
            if key in frame:
                row[key] = frame[key]
        identity = (frame.get('gpuTimingSerial'), frame.get('gpuTimingFrame'))
        if identity[0] and identity[1] in measured_gpu_frames and identity not in seen_gpu:
            seen_gpu.add(identity)
            row['gpuTimingFrame'] = identity[1]
            for label, ms in frame.get('gpu', []):
                row['gpu/' + label] = ms
        for key, expected in frame.get('expectedResident', {}).items():
            actual = frame.get('resident', {}).get(key)
            if actual != expected:
                mismatches.append(dict(sample=frame.get('sourceSample'), counter=key,
                                       expected=expected, actual=actual))
        for key, value in row.items():
            if key not in ['sample', 'sequence', 'gpuTimingFrame']:
                series[key].append(value)
        rows.append(row)
    producer = [(f['producerStart'], f['submitted']) for f in frames if 'submitted' in f]
    series['frameIntervalMs'] = [y['producerStart']-x['producerStart'] for x, y in zip(frames, frames[1:])]
    recording = [(f['recordStart'], f['recordEnd']) for f in frames]
    summary = dict(trace=str(path), scene=document.get('scene'), renderThread=document.get('renderThread'), frames=len(frames),
                   countersMatchCapture=not mismatches, mismatchCount=len(mismatches),
                   mismatchExamples=mismatches[:20],
                   producerRecordingWallOverlapMs=intersection_ms(producer, recording),
                   metrics={key: distribution(values) for key, values in sorted(series.items())},
                   caveat='Wall durations include waits; nested scopes must not be summed. '
                          'Thread CPU times use the OS clock (quantized on Windows). '
                          'Scene-to-record-end is not input-to-display latency. '
                          'GPU measurements belong to gpuTimingFrame, not the row source sample.')
    keys = sorted({key for row in rows for key in row})
    with path.with_suffix('.csv').open('w', encoding='utf-8', newline='') as output:
        writer = csv.DictWriter(output, fieldnames=keys)
        writer.writeheader()
        writer.writerows(rows)
    path.with_suffix('.summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
    return summary, frames


def metric_deltas(a, b):
    result = {}
    for key in sorted(set(a['metrics']) & set(b['metrics'])):
        x, y = a['metrics'][key], b['metrics'][key]
        if x is None or y is None:
            continue
        result[key] = {stat: dict(a=x[stat], b=y[stat], delta=y[stat]-x[stat],
                                 deltaPercent=(y[stat]-x[stat])*100/x[stat] if x[stat] else None)
                       for stat in ['mean', 'p95', 'p99']}
    return result


def compare_saved(baseline, current):
    # Only saved distributions are needed; never reopen or rerun the old trace.
    if not baseline.get('scene') or baseline['scene'] != current.get('scene'):
        raise ValueError('saved baseline requires the same captured scene')
    a_mode, b_mode = baseline.get('renderThread'), current.get('renderThread')
    return dict(mode='saved-baseline', a=baseline['trace'], b=current['trace'],
                sameScene=True, sameFrameCount=baseline['frames'] == current['frames'],
                sameRenderThread=str(a_mode) == str(b_mode) if a_mode is not None and b_mode is not None else None,
                baselineFrames=baseline['frames'], currentFrames=current['frames'],
                metrics=metric_deltas(baseline, current))


def compare(left, right):
    a, af = left
    b, bf = right
    if not a.get('scene') or a['scene'] != b.get('scene'):
        raise ValueError('A/B requires the same captured scene path')
    indexed = [{f['sourceSample']: f for f in frames} for frames in [af, bf]]
    if set(indexed[0]) != set(indexed[1]) or len(indexed[0]) != len(af) or len(indexed[1]) != len(bf):
        raise ValueError('A/B source samples differ, are repeated, or are missing')
    differences = []
    for sample in sorted(indexed[0]):
        x, y = indexed[0][sample], indexed[1][sample]
        for name in ['resident', 'statistics']:
            for key in sorted(set(x.get(name, {})) | set(y.get(name, {}))):
                if key.endswith('Bytes'):
                    continue  # Residency size is a result, not scene identity.
                xv, yv = x.get(name, {}).get(key), y.get(name, {}).get(key)
                if xv != yv:
                    differences.append(dict(sample=sample, counter=name + '/' + key, a=xv, b=yv))
    result = dict(a=a['trace'], b=b['trace'], sameSourceSamples=True,
                  sameCounters=not differences, counterDifferences= len(differences),
                  differenceExamples=differences[:20], metrics=metric_deltas(a, b))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('traces', nargs='+', type=Path)
    parser.add_argument('--compare-output', type=Path)
    parser.add_argument('--baseline', type=Path, help='compare one new trace with saved summary metrics; no old run')
    args = parser.parse_args()
    if len(args.traces) not in [1, 2]:
        parser.error('provide one trace, or two traces for a paired A/B')
    if args.baseline and len(args.traces) != 1:
        parser.error('--baseline takes one new trace')
    summaries = [summarize(path) for path in args.traces]
    for summary, _ in summaries:
        print(f'{summary["trace"]}: {summary["frames"]} completed frames; '
              f'{summary["mismatchCount"]} resident counter mismatches with capture')
        for key in ['producerWallMs', 'queueWaitMs', 'queueResidenceMs', 'recordWallMs',
                    'sceneToRecordEndMs', 'producerStages/Present/extract scene']:
            value = summary['metrics'].get(key)
            if value:
                print(f'  {key}: mean {value["mean"]:.3f}, p95 {value["p95"]:.3f}, p99 {value["p99"]:.3f}')
    if args.baseline:
        baseline = json.loads(args.baseline.read_text(encoding='utf-8'))
        result = compare_saved(baseline, summaries[0][0])
        result['baselineFile'] = str(args.baseline)
        output = args.compare_output or args.traces[0].with_suffix('.comparison.json')
        output.write_text(json.dumps(result, indent=2), encoding='utf-8')
        print(f'Saved baseline: {args.baseline}; {output}')
        print(f'  same frame count={result["sameFrameCount"]}, render mode={result["sameRenderThread"]}')
        for key in ['frameIntervalMs', 'producerStages/Present/extract scene', 'recordWallMs',
                    'producerStages/extract/M2', 'producerStages/extract/RT source sync',
                    'producerStages/extract/terrain', 'gpu/wmo', 'gpu/rt_lighting']:
            if key in result['metrics']:
                value = result['metrics'][key]['mean']
                percent = f'{value["deltaPercent"]:+.1f}%' if value['deltaPercent'] is not None else 'n/a'
                print(f'  {key}: {value["a"]:.3f} -> {value["b"]:.3f}, {value["delta"]:+.3f} ms ({percent})')
    elif len(summaries) == 2:
        result = compare(*summaries)
        output = args.compare_output or args.traces[1].with_suffix('.comparison.json')
        output.write_text(json.dumps(result, indent=2), encoding='utf-8')
        print(f'A/B: counters match={result["sameCounters"]}; {output}')


if __name__ == '__main__':
    main()
