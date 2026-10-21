# Timed scene capture and offline performance replay

Purpose: repeat approximately the same real-world rendering workload without
logging into a realm for each change. Exact images and resident counts are useful
diagnostics, not prerequisites for running the test.

## Record once

Build the dev client, then use its ordinary launcher and complete active profile:

```powershell
tools/scene_replay.ps1 Capture `
  -Recording D:\games\WoWee\build-dev\scratch\dalaran.json `
  -Profile D:\path\to\the\active-profile -Data D:\games\WoWee\Data
```

Enter the intended place once. Automatic recording starts three seconds after
the world is in view and records 30 seconds by default
(`-Seconds`). **Ctrl+F9** starts immediately or stops an active capture. Without
the script, Ctrl+F9 saves to a new `build-dev/bin/logs/scene-*.json`.
Existing capture files are never overwritten. The log says when recording starts
and when both the scene and its metrics have finished writing in the background.
Capture adds sampling overhead; use the replay to measure changes.

The recording contains the character/map, server archive references and file
metadata, update-field layout, complete initial entity fields (including cull
candidates), field changes, spawn/despawn, evaluated movement/locomotion, resolved
remote-player equipment/enchants, casts, spell launches, emotes and requested visual events, camera,
weather/clock, and graphics settings/CVars. It contains no account password or
authentication packet stream. It uses local game assets rather than copying
their bytes or storing runtime GPU/model handles.

## Replay and compare

```powershell
tools/scene_replay.ps1
```

The default is one Replay using the scene in
`tasks/performance/dalaran-baseline.json`, followed by a comparison with those
saved metrics. The reference build is not launched. The last completed live
Dalaran run (760 samples) is already saved there, with a readable table in
`tasks/performance/dalaran-baseline.md`. Differences in mean, p95, p99 and percent
are written to the new run's comparison report; the saved baseline is preserved.
Use `-Baseline <summary.json>` to choose another saved reference.

This makes isolated profiles on D:, loads the actual map and source entities
without a server, warms the asynchronous loading queues within a bounded time, then advances through
the recorded source samples and their game deltas. The normal client still runs
presentation, scene extraction, culling, RT preparation and recording each frame.
One replay sample corresponds to one measured frame in each mode, regardless of
how fast the test machine executes it. Vsync and the frame cap are disabled for
the replay; every other graphics setting comes from the capture.

JSON timelines, CSV rows,
distribution summaries and a baseline comparison report land under
`build-dev/scratch/scene-replay/run-*/`. The game closes when playback finishes.
`Replay -RenderThread 0|1` runs one mode. Explicit `AB` runs modes 0 then 1 only
when that comparison is needed. `Benchmark` records the existing
offline Dalaran route as a geometry baseline; it has no live crowd.

Low-level switches: `WOWEE_SCENE_CAPTURE`, `WOWEE_SCENE_CAPTURE_SECONDS`,
`WOWEE_SCENE_REPLAY`, `WOWEE_SCENE_REPLAY_SETTLE`, `WOWEE_PERF_TRACE`.
`python tools/perf_trace.py new.perf.json --baseline tasks/performance/dalaran-baseline.json`
compares a saved new trace without running either client. Two trace paths still
regenerate an explicit paired comparison.

## Read the measurements

- `producerWallMs`, `producerCpuMs`: preparation up to submission to the render
  worker. CPU time comes from the OS thread clock (quantized on Windows).
- `producerStages/extract/*`: consecutive extraction parts: grass population,
  globals/collision/lights, terrain, water, WMO, characters, M2, minimap, sky,
  effects/composites, RT source synchronization and RT region changes.
- `queueWaitMs`: producer waiting for one of the two frame reservations.
  `outstanding` includes the producer's reserved frame, not just queued jobs.
- `queueResidenceMs`: submission to the start of recording.
- `recordWallMs`, `recordCpuMs`, `recordStages/*`: recording and its GPU-slot,
  swapchain acquire, submit and present waits.
- `producerRecordingWallOverlapMs`: intersection of the wall intervals. Waiting
  threads can overlap; this alone does not establish CPU utilization.
- `sceneToRecordEndMs`: producer start to recording completion, **not** actual
  input-to-display latency. `resultDeliveryMs` measures mailbox delivery delay.
- GPU pass timestamps are deduplicated and retain `gpuTimingFrame`; that GPU
  frame can predate the source sample carrying the answer.
- Resident/entity/draw/cull/RT counters and cheap driver VRAM usage/budget help
  explain changes in workload. Differences with the capture and between A/B
  runs are reported and do not block execution. No expensive memory maps or
  model-owner walks are enabled by the launcher.

CPU scopes are nested and parallel; do not add them as serial frame costs. The
reader reports mean, p50, p95, p99 and maximum. Text frame-budget reports are
suppressed during machine tracing so their formatting does not pollute tails.

## Limits

This is a sampled rendering workload replay, not a complete gameplay demo.
Initial transient spell particles, animation phase, addon/UI work and network
processing are not restored. Movement and events have frame-level timing.
Static map residency can differ because the original client travelled before
capture; the normal streamer reconstructs the surrounding world. These limits
do not prevent identifying costly extraction stages, recording stalls, GPU
passes or memory growth. Report them when interpreting a comparison.

The optional `scene_replay_probe` target seeds eight real character models,
mounts, weapons, movement and a despawn through game APIs for native end-to-end
testing. It is available only with `WOWEE_BUILD_RENDER_CLIENT_PROBE=ON`.

## Validation (2026-10-21)

- Dev client and optional native probe build successfully. `recorded_entities`,
  `frame_worker` and `perf_trace` pass; `sweep_guard` passed during implementation.
  The frame-input and recording-scene dependency checks remain at zero.
- Native Dalaran capture with eight players, two mounts and eight real weapon
  models records movement, two emote events and a despawn. Capturing does not
  strip equipment. The final fixture contains 728 samples over 8.00189 seconds.
- Full-client offline replay completes every sample with zero resident-counter
  differences (entities, characters, M2, WMO and terrain tiles). Initial scene:
  52,379 M2 placements, 365 WMO instances and 68 tiles. This is a fixture crowd;
  the user's live crowd was subsequently captured below.
- Earlier inline/threaded A/B smoke runs completed 150 source samples each and
  wrote JSON, CSV and a comparison report. Native Vulkan validation reported no
  VUID/threading errors; shutdown left zero VMA allocations.
- Final native evidence: `build-dev/scratch/scene-replay/fixture-20261021-081900/`
  and `build-dev/scratch/scene-replay/run-20261021-082016-599/`.

## Live Dalaran capture verified (2026-10-21)

`build-dev/scratch/scene-replay/dalaran-live-20261021-082422.json` holds 30.013
seconds / 760 source samples, 279 presentation events and 24 local realm archive
references. Initial entity count 814, final 970. Offline replay completed all
760 samples without contacting the realm. The entity timeline matched; normal
world streaming warmed more static geometry than the original capture, and the
character/residency differences are explicitly reported rather than blocking it.

Reports: `build-dev/scratch/scene-replay/run-20261021-082718-620/`.
In this approximate reproduction, extraction averaged 17.87 ms (p95 20.44): M2
7.05 ms, RT source synchronization 4.81 ms, RT region updates 1.65 ms, terrain
3.03 ms. These identify work to investigate, not an optimization gain. The normal
client was restored with the complete active profile after the offline run.
