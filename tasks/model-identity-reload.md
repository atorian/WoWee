# Model identities across lookup rebuilds

Trigger: realm patches are downloaded while the player is in a map, then mount
between maps. EntitySpawner::rebuildLookups resets AssetCaches. The renderers
intentionally retain their models across map lifetimes.

The allocators were inside AssetCaches, so the next WMO/M2/character/attachment
could receive the ID of a different resident GPU model. The loader's existing
model check then reused that geometry. This explains zeppelins drawn as ships
and wrong decorations drawn with another object's placement and scale.

The five allocation counters now belong to EntitySpawner outside the resettable
lookup caches. Synchronous and asynchronous loaders share the same counters.
Lookup tables still rebuild after realm/expansion changes.

Regression: the optional `WOWEE_BUILD_ENTITY_ASSET_RELOAD_TEST` target loads real
game objects through the public asynchronous spawn queue, despawns them while
keeping GPU models resident, rebuilds lookups, and requests them in reversed
order. It checks the geometry exposed by the production renderers, and checks
attachment allocation across another rebuild.

Before the fix, the test reproduced both substitutions:

| Requested display | Original triangles | After rebuild, before fix |
| --- | ---: | ---: |
| Zeppelin (3031) | 3596 | 5854 (ship) |
| Ship (3015) | 5854 | 3596 (zeppelin) |
| Sign (22) | 16 | 98 (pumpkin) |
| Pumpkin (60) | 98 | 16 (sign) |

After the fix all four retain their original geometry; attachment IDs also
remain distinct. Native dev build and five selected CTest cases pass: model
reload, client transitions, transport paths/components, and realm patch sync.
Artifacts: `build-dev/scratch/model-reload/`.
