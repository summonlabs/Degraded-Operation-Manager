# Contributing

Thank you for contributing to Degraded Operation Manager. This is a systems runtime that makes an authoritative facility-wide operating decision, so correctness, boundedness, determinism and explainability matter more than convenience.

## Ground rules

- Keep the boundary narrow. This runtime decides *which degraded operating mode governs the facility right now*, what service that mode permits and restricts, and what evidence is required to leave it. It does not own incident lifecycle, facility policy as a whole, raw capacity accounting, load-shedding execution, power or cooling actuation, workload scheduling, fabric rerouting, recovery orchestration, or any device.
- A mode decision is never a boolean. Operating classes are explicit and ordered, and `indeterminate` is a first-class posture for evidence that cannot justify a stronger answer.
- Missing, stale, conflicted or future-dated evidence never restores a less restrictive mode. Escalation is conservative; de-escalation requires current evidence plus minimum dwell and recovery hold.
- Policy eligibility, the mode decision, the authorization to apply restrictions, an adjacent owner's acknowledgement and an independently verified effect are separate objects with separate identities. Never collapse them.
- Every durable structure is bounded, every counter, generation and duration is overflow checked, and every canonical encoding has one deterministic byte sequence.
- Decoders are strict: refuse truncated, oversized, out-of-order, unknown-code and invalid-UTF-8 input instead of guessing.

## Building

Requires C++20 and CMake 3.20 or newer. On Windows use an MSVC toolchain; the helper script finds it through vswhere.

```powershell
./scripts/build.ps1 -Config Release -RunTests
./scripts/build.ps1 -Config Debug -RunTests
./scripts/build.ps1 -Config Release -Sanitizer asan -RunTests
./scripts/closure.ps1 -Stage install
./scripts/closure.ps1 -Stage consumer
```

## Testing

Add cases to the suite that matches the claim, using the small harness in `tests/testkit`. Randomized and property tests must print their seed and check invariants after every mutation. Never use a timeout, watchdog or forced termination to make a failing or hanging test pass: a hang is a defect to diagnose. Crash and multiprocess claims must be proven with real operating system processes, never with serialization-only or exception-only tests. The suite must pass in Release and Debug with zero first-party warnings.

## Style

- Compiled with MSVC `/W4 /WX /permissive-` (and the equivalent strict flags elsewhere). Fix warnings at the source; do not add broad suppressions.
- Use the strong identity and counter types rather than raw integers for identities, generations and epochs.
- Use `Status` and `Result<T>` for fallible operations; do not use exceptions for control flow.
- Keep the public headers in `include/dom` free of internal types; anything under `src/` is implementation detail and is not installed.
- Determinism first: canonical ordering for every collection, no dependence on hash or map iteration order, and no hidden clock.

## Commit

- Keep commits focused and atomic.
- Do not add `Co-authored-by` trailers or any other attribution trailer.
- Never commit build output, test stores, logs or generated intermediates.
