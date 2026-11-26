# Velmux — Performance

## Engineering targets (warm path)

| Metric | Target |
|---|---|
| Pipeline planning p95 | < ~5 ms |
| Request dispatch p95 | < ~20 ms |
| Full codec enumeration per warm conversion | 0 |
| Test encoding during planning | 0 |
| Unnecessary full-file scans | 0 |
| Cached selector interaction | < 100 ms |
| Warm configuration updates | < 20 ms |

These are measurement targets, not fake guarantees. `core/tests/planner_tests.cpp`
includes a warm-path planning benchmark assertion (generous CI-safe bound) and the
telemetry store accumulates real planning timings.

## Non-negotiables

- Never block a UI thread with media processing (the Kotlin engine client
  dispatches all transport calls off the main thread).
- No expensive codec discovery, FFmpeg init, hardware probing, or test encoding
  synchronously when the user presses Convert.
- UI responsiveness is independent of encoding throughput; the poll loop parks
  when idle and listeners detach cleanly.

## Priority order (when trade-offs collide)

1. valid/correct output
2. responsive UX
3. recoverability
4. actual processing throughput
5. resource efficiency
6. compression efficiency
7. advanced customization

## Measurement program

- Planning latency: micro-benchmarks in core tests + telemetry records per real job.
- Throughput: telemetry records frames/sec and setup time per stage/backend.
- Startup: CI-measured host engine init; device cold-start tracked on-device.
- Recovery overhead: fault-injection tests measure checkpoint cost.

## Concurrency policy

The scheduler bounds active jobs by: CPU cores, GPU/hardware codec availability,
memory pressure, thermal state, power, and measured concurrency behavior. Jobs are
not all run simultaneously.
