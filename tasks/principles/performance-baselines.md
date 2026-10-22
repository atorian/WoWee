# Compare performance with saved results

Applies to repeated performance investigations and optimizations in WoWee.

User choice (2026-10-21): save the last run's numbers and compare new results
with them instead of launching both sides of an A/B every time.

Rule: keep a durable summary of the completed reference run, its replay scene,
settings, sample count, resource counters and mean/p95/p99. Run only the changed
version for routine checks, then report its differences from those saved values.
An explicit comparison of two rendering modes can still use a paired run.

Reason: rerunning an already measured reference consumes time without being
necessary for each iteration. Preserved results also retain the history.

Checklist:
- Read `tasks/performance/dalaran-baseline.json` before a Dalaran comparison.
- Use one Replay by default; do not start the old version to read its numbers.
- Retain each new report and compare timings and resources with the reference.
- Report changes in scene, settings, mode, sample count or resident geometry.
- Keep the original reference when writing a new comparison report.

## Pipeline migration gate (user decision, 2026-10-22)

During the current rendering-pipeline refactor, do not run intermediate performance benchmarks. Complete the unified RT path and merged passes first. Builds, behavioral scene/image tests and synchronization validation remain required. Only after completion use a new final measurement against the saved baseline.
