"""Behaviour of the trace reader on overlapping frames and paired scene data."""
import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('perf_trace', ROOT / 'tools/perf_trace.py')
trace = importlib.util.module_from_spec(spec)
spec.loader.exec_module(trace)


def frame(sequence, start):
    return dict(sequence=sequence, sourceSample=sequence - 1, measured=True,
                producerStart=start, extracted=start + 4, submitted=start + 5,
                recordStart=start + 6, recordEnd=start + 11, resultReceived=start + 13,
                queueWaitMs=.5, outstanding=2, producerCpuMs=3, recordCpuMs=2,
                resident={'characters': 8}, expectedResident={'characters': 8},
                statistics={'m2Draws': 20}, gpuSubmission=sequence,
                gpuTimingFrame=1, gpuTimingSerial=1, gpu=[['whole', 7]],
                producerStages=[{'name': 'extract/M2', 'ms': 2}])


class TraceTest(unittest.TestCase):
    def setUp(self):
        scratch = ROOT / 'build-dev/scratch'
        scratch.mkdir(parents=True, exist_ok=True)
        self.directory = tempfile.TemporaryDirectory(dir=scratch)
        self.addCleanup(self.directory.cleanup)

    def write(self, name, frames):
        path = Path(self.directory.name) / name
        path.write_text(json.dumps(dict(version=1, scene='crowd.json', frames=frames)))
        return path

    def test_reports_overlap_stages_and_deduplicates_delayed_gpu_answers(self):
        path = self.write('a.json', [frame(1, 100), frame(2, 108)])
        report, _ = trace.summarize(path)
        self.assertEqual(report['producerRecordingWallOverlapMs'], 3)
        self.assertEqual(report['metrics']['queueResidenceMs']['mean'], 1)
        self.assertEqual(report['metrics']['producerStages/extract/M2']['p95'], 2)
        self.assertEqual(report['metrics']['gpu/whole']['count'], 1)
        self.assertTrue(path.with_suffix('.csv').exists())

    def test_scene_difference_is_reported_and_does_not_block_measurements(self):
        a = self.write('a.json', [frame(1, 100)])
        changed = frame(1, 100)
        changed['resident']['characters'] = 7
        b = self.write('b.json', [changed])
        left, right = trace.summarize(a), trace.summarize(b)
        self.assertFalse(right[0]['countersMatchCapture'])
        result = trace.compare(left, right)
        self.assertFalse(result['sameCounters'])
        self.assertEqual(result['counterDifferences'], 1)
        self.assertIn('recordWallMs', result['metrics'])

    def test_incomplete_run_and_different_source_samples_cannot_masquerade_as_ab(self):
        bad = self.write('empty.json', [])
        with self.assertRaisesRegex(ValueError, 'no completed measured'):
            trace.summarize(bad)
        a = trace.summarize(self.write('a.json', [frame(1, 100)]))
        b = trace.summarize(self.write('b.json', [frame(2, 100)]))
        with self.assertRaisesRegex(ValueError, 'source samples differ'):
            trace.compare(a, b)

    def test_one_new_trace_compares_with_saved_numbers_without_old_trace(self):
        old = self.write('old.json', [frame(1, 100)])
        saved, _ = trace.summarize(old)
        baseline = Path(self.directory.name) / 'baseline.json'
        baseline.write_text(json.dumps(saved))
        before = baseline.read_bytes()
        old.unlink()
        changed = frame(1, 100)
        changed['recordEnd'] = 110
        current = self.write('new.json', [changed])
        process = subprocess.run([sys.executable, str(ROOT / 'tools/perf_trace.py'), str(current),
                                  '--baseline', str(baseline)], capture_output=True, text=True)
        self.assertEqual(process.returncode, 0, process.stderr)
        comparison = json.loads(current.with_suffix('.comparison.json').read_text())
        result = comparison['metrics']['recordWallMs']['mean']
        self.assertEqual(result, dict(a=5, b=4, delta=-1, deltaPercent=-20))
        self.assertEqual(baseline.read_bytes(), before)

    def test_saved_numbers_from_different_scene_are_not_presented_as_a_comparison(self):
        saved, _ = trace.summarize(self.write('old.json', [frame(1, 100)]))
        current = dict(saved, scene='another-city.json')
        with self.assertRaisesRegex(ValueError, 'same captured scene'):
            trace.compare_saved(saved, current)


if __name__ == '__main__':
    unittest.main()
