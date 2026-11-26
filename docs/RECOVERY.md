# Velmux — Recovery

Recovery is a first-class requirement. A crash must not mean restarting a
multi-hour job from zero. We never promise impossible exact-frame encoder-state
restoration.

## Per-stage recovery strategies

| Stage | Strategy |
|---|---|
| File copy | byte-offset continuation where safe |
| Stream copy / remux | packet or segment boundary |
| Audio processing | sample/segment checkpoint |
| Video transcode | independently finalized segments |
| Subtitles | cue/timeline checkpoint |
| Temporal filters | restart from safe dependency boundary |
| Final assembly | reconstruct from validated pieces |

## Crash-safe checkpoint protocol

A checkpoint is not valid because an encoder claims a segment finished. Durable
sequence:

1. write temporary segment;
2. finalize segment;
3. validate segment;
4. flush durable data as required;
5. promote/commit artifact safely;
6. record checkpoint in SQLite.

Database and filesystem writes are not one atomic transaction.

## On recovery

1. inspect persisted DB state;
2. inspect artifacts;
3. detect orphaned/partial files;
4. validate the last committed checkpoint;
5. reconcile inconsistencies;
6. resume from the safest valid point.

Committed work remains; partial artifacts are reconciled; no duplicate segments; no
missing timeline regions; no broken A/V sync; correct final duration; valid output.

## Durable sources and segments

- Sources are persistent external resources (Android SAF content URIs with
  persistable grants; iOS security-scoped bookmarks). Recovery retains enough
  access information to reopen the source later.
- Temporary recovery segments live in durable storage, not disposable cache, when
  losing cache would destroy resumability.

## Fault-injection testing

Tests force interruption during demux, decode, video/audio processing, encode, mux,
checkpoint write, DB commit, file promotion, final assembly; then restart and verify
reconciliation invariants (see `docs/TESTING.md` for the verified coverage).

## Real-world handling

Source unavailable, destination unavailable, app killed, device reboot, thermal
throttling, storage exhaustion, malformed media, validation failure — each has an
explicit job state and user-visible outcome. Never silently lose user data.
Never fabricate progress or ETA: if ETA is unreliable, show progress without fake
precision.
