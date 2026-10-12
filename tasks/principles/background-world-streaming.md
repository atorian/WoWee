# Background world streaming

Applies when movement, visibility changes, or server updates require world assets.

User choice (2026-10-11): prepare data in the background without blocking the render thread; show the result when it is ready.

Rule: workers perform file reads, model parsing, texture decoding and CPU compositing. The render thread polls readiness without waiting, retains the current visual until replacement data is ready, and submits GPU uploads asynchronously. A per-frame budget does not make a synchronous operation nonblocking.

Reason: one expensive operation can exceed an entire frame before a budget can be checked.

Checklist:
- No synchronous disk/decode/composite fallback in a prepared-result consumer.
- Bound worker concurrency; retain completed data until an upload slot is available.
- Reject stale results after despawn, appearance changes or map transitions.
- Keep worker lifetimes safe when changing the asset dataset.
- Test cold loading and queue completion with real assets and GPU validation.
