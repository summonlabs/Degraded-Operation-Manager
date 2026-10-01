# Degraded Operation Manager

Degraded Operation Manager (DOM) is a portable C++20 runtime that answers one
question deterministically:

> Given the current incidents, failure domains, capacity, redundancy, power,
> cooling, maintenance, policy and protected obligations, which degraded
> operating mode is authoritative for the facility right now, what service does
> that mode still permit, what must it restrict, and what evidence is required
> to leave it?

It is infrastructure, not a simulator, a BMS, a load shedder or a policy editor.
It owns *facility-wide degraded-mode authority* and nothing else.

---

## 1. Exact systems boundary

DOM **owns**:

* the canonical degraded-mode catalog: ordered operating classes, per-mode entry
  criteria and exit criteria, latch behaviour, minimum dwell, recovery hold, and
  the bounded restrictions each mode imposes;
* protected obligation classes and the guarantee that an inviolable obligation
  is never denied by any mode;
* the decision: which mode governs now, in which posture, bound to the exact
  incident, failure-domain, capacity, redundancy, power, cooling, maintenance,
  obligation, policy and control-epoch generations it was derived from;
* the indeterminate posture, used when the available evidence cannot justify any
  operating class, which never relaxes the restrictions already in force;
* the authority to apply restrictions: a separate, expiring authorization bound
  to one decision, its own identity, and the control epoch that issued it;
* the durable record of what adjacent owners acknowledged and what was
  independently verified, kept strictly separate from the decision itself;
* durable mode state, bounded transition history, idempotent replay identity,
  latch state, and stale-decision fencing across restarts.

DOM **does not**:

* observe the facility. Incidents, failure domains, capacity, redundancy, power,
  cooling and maintenance arrive as typed evidence published by adjacent owners;
  DOM has no sensor, BMS, DCIM, PDU, UPS, generator or cooling adapter;
* produce capacity accounting, compute load shedding, actuate power or cooling,
  schedule or migrate workloads, reroute traffic, or orchestrate recovery;
* own incident lifecycle, facility policy as a whole, or any adjacent owner's
  execution, scheduling, routing, path authority or device actuation;
* authenticate operators cryptographically. Latch clears and epoch adoption
  require an explicit authority reference and matching evidence, not a
  credential; see section 9.

Integration is through explicit typed values: a policy document, an evidence
snapshot, a mode decision with its reason trace, a restriction set, an
authorization, acknowledgement and verification records. No adjacent runtime
responsibility is absorbed.

---

## 2. Build, install and use

Requirements: CMake 3.20+, a C++20 compiler, and nothing else at runtime. The
library has no third-party dependency; on Windows it links only kernel32.

~~~powershell
# Configure, build and test (Release).
./scripts/build.ps1 -Config Release -RunTests

# Debug.
./scripts/build.ps1 -Config Debug -RunTests

# AddressSanitizer build (the script probes the toolchain first).
./scripts/build.ps1 -Config Release -Sanitizer asan -RunTests

# Install to a clean prefix and validate an independent consumer.
./scripts/closure.ps1 -Stage install
./scripts/closure.ps1 -Stage consumer

# Full closure from a disposable clone of the configured remote.
./scripts/closure.ps1 -Stage fresh
~~~

Equivalent direct commands:

~~~sh
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
ctest --test-dir build/release --output-on-failure
cmake --install build/release --prefix /tmp/dom
~~~

Downstream consumption uses the exported package:

~~~cmake
find_package(DegradedOperationManager 1.0 CONFIG REQUIRED)
target_link_libraries(app PRIVATE DegradedOperationManager::dom_runtime)
~~~

| Target | Contents | Threads |
| --- | --- | --- |
| `DegradedOperationManager::dom_core` | domain model, canonical codecs, SHA-256, evaluation engine | none |
| `DegradedOperationManager::dom_runtime` | durable state store, single-writer lock, coordinator | caller serialised |
| `DegradedOperationManager::dom` | alias of `dom_runtime` | |

A minimal evaluation:

~~~cpp
#include "dom/dom.hpp"

dom::CoordinatorOptions options;
options.directory = "/var/lib/dom";
options.policy = policy;                          // validated document
options.incarnation = dom::Incarnation::FromValue(17);
options.max_authorization_ttl_ms = dom::Duration::FromValue(600000);

auto coordinator = dom::Coordinator::Open(options);
auto status = coordinator.value()->GetStatus();    // epoch, sequence, posture

dom::EvaluationRequest request;
request.envelope.idempotency_key = dom::Sha256::Of("evidence-cycle-42");
request.envelope.expected_state_sequence = status.value().state_sequence;
request.envelope.expected_epoch = status.value().epoch;
request.context = {status.value().epoch, status.value().incarnation,
                   dom::Instant::FromValue(now_ms), dom::Tick::FromValue(tick)};
request.evidence = evidence;                       // published by adjacent owners

auto outcome = coordinator.value()->Evaluate(request);
// outcome.value().decision.posture, .trace, .generations, .digest()
// outcome.value().restrictions.permitted_services / .restricted_services
~~~

---

## 3. Architecture and data model

### 3.1 Operating classes and postures

An ordered operating class expresses how much service the facility may provide:

| Class | Rank | Meaning |
| --- | --- | --- |
| `nominal` | 0 | full service, full redundancy |
| `watch` | 1 | elevated awareness, no service reduction |
| `conserve` | 2 | optional consumption reduced |
| `restricted` | 3 | only essential service classes remain |
| `critical` | 4 | only protected obligations and minimum service |
| `emergency` | 5 | life-safety posture, minimal service |

The *posture* is the class currently in force, or `indeterminate`. Indeterminate
is a first-class state: it means the evidence cannot justify any operating
class. It retains the governing mode and unions the indeterminate restriction
set with the committed mode's restrictions, so it is never weaker than the mode
it replaced. A degraded mode is never a boolean.

### 3.2 Modes and criteria

A policy defines modes: a stable `ModeId`, a key, an operating class, a
revision, a latch mode, a minimum dwell, a recovery hold, entry criteria, exit
criteria, restrictions and protected obligations. Criteria are decidable
predicates over published evidence:

* `EvidenceClass`: incident, failure-domain, capacity, redundancy, power,
  cooling, maintenance, obligation;
* selector: exact subject and metric, or `*` for every key of the class;
* aggregation: `any`, `all`, `count`, `maximum`, `minimum`, `sum`;
* comparator: `at-least`, `at-most`, `equals`, `not-equals`;
* typed threshold (unit plus value), and whether only fresh evidence may satisfy
  it.

A predicate is *resolvable* or it is not satisfied: an aggregate over no
records, a mixed-unit comparison or a summed overflow never counts as
satisfied. Values are never coerced between units.

### 3.3 Evidence and freshness

Evidence arrives as records of (class, subject, metric) with a typed value, the
instant it was observed, an optional explicit expiry, a generation, the digest
of the producer's own snapshot, and the producer identity. A snapshot is built
once per evaluation and is canonical:

* records are ordered by (class, subject, metric, producer, generation,
  observation, value, digest);
* exact duplicates collapse;
* a key that is published twice with disagreeing content is *conflicted*, and a
  conflicted key is never usable.

Freshness is computed against the evaluation instant and the declared
requirements: `fresh`, `stale`, `expired`, `future-dated`, `missing` or
`conflicted`. Policy validation requires that every fresh-gated criterion is
covered by a declared freshness requirement, so a policy cannot quietly depend
on evidence whose staleness nobody declared.

### 3.4 Evaluation

Evaluation is a pure function of (policy, evidence snapshot, committed state,
explicit context). It reads no clock and holds no mutable state, so the same
inputs always produce the same digests.

1. **Preconditions.** The policy is validated, the context epoch and
   incarnation must own the committed state, the committed mode must exist, and
   the evaluation instant must not precede the committed mode start. Otherwise
   the evaluation is refused and nothing changes.
2. **Posture.** Every declared requirement is classified. Requirements that
   cover recovery must be fresh before any relaxation is considered.
3. **Criteria.** Every mode's entry and exit criteria are evaluated once.
4. **Escalation.** If any mode more restrictive than the committed rank has all
   entry criteria satisfied, the *most restrictive* such mode is entered
   (ties broken by the smallest mode id). Escalation is conservative: it never
   needs the recovery requirements, because serving a stricter posture with
   incomplete information is the fail-safe direction.
5. **Latch.** A latched mode holds until an explicit clear with matching cause
   digest, matching control epoch and fresh evidence, or, for
   `until-recovery-permitted`, until recovery preconditions hold.
6. **Recovery.** A relaxation requires all four of: fresh recovery evidence, the
   committed mode's exit criteria satisfied, the minimum dwell elapsed, and a
   completed recovery hold (continuous clearance for the mode's recovery hold
   duration, tracked in durable state so a restart never shortens it). The
   target is the least restrictive mode whose entry criteria hold, provided
   every mode strictly between it and the committed mode also satisfies its exit
   criteria, and provided the descent stays within the policy's
   `max_recovery_step`.
7. **Indeterminate.** If the declared recovery evidence is not fresh, the posture
   becomes indeterminate instead of relaxing, and the restrictions already in
   force are retained.
8. **Explanation.** Every decision carries a canonical reason trace: ordered,
   de-duplicated reason codes naming the evidence, criteria, latch, dwell, hold
   or fencing condition that produced it.

### 3.5 Authority separation

Four objects are deliberately distinct:

| Object | Identity | Meaning |
| --- | --- | --- |
| mode decision | `DecisionSequence` plus digest | which mode governs, bound to generations |
| restriction authorization | `AuthorizationId` | permission to apply that decision's restrictions until it expires |
| acknowledgement | authorization plus owner | what an adjacent owner reported; not evidence of effect |
| effect verification | authorization plus observer digest | an independently observed effect, flagged stale when its bound decision is superseded |

DOM never treats an acknowledgement, an earlier success, a cached value or its
own recovered state as authority to act.

### 3.6 Generations, idempotency and fencing

Every decision carries a `GenerationVector`: control epoch, incarnation,
policy generation, evidence revision, and one generation per evidence class,
plus the policy, evidence, input, previous-decision and restriction digests.

Every mutation carries an idempotency key, the epoch it was built against and
the state sequence it expects. The same key with the same request resolves as an
idempotent replay *before* any precondition is checked, so a lost response can
never cause a second consequential mutation; the same key with a different
request is a conflict. A request whose epoch or sequence is stale is refused.

---

## 4. Persistence and recovery

### 4.1 Files

| File | Contents |
| --- | --- |
| `store.lock` | kernel-owned single-writer lock |
| `snapshot-<20 digits>.doms` | one published generation |
| `snapshot-<20 digits>.doms.tmp` | staged generation, never authoritative |
| `journal.domj` | publication intents, commits and abandoned intents |

A snapshot is a 92-byte header (magic, format version, reserved, state sequence,
payload length, previous-generation digest, payload digest), the canonical state
payload, and a footer digest over header and payload. The payload itself is a
canonical encoding with a version, the committed view, the last decision,
bounded history, authorizations, acknowledgements, verifications, the
idempotency window and counters.

### 4.2 Publication protocol

1. Write the staged snapshot and flush it to stable storage.
2. Read it back and verify length, framing and both digests.
3. Append the journal intent and flush.
4. Atomically replace the published snapshot with the staged file — **this
   rename is the commit point**.
5. Append the journal commit marker and flush (a repair aid; if it cannot be
   written the published generation is still authoritative).

The store keeps the two newest generations so a rollback can be detected.

### 4.3 Recovery

Recovery loads the newest snapshot that verifies completely. A snapshot file
that exists but does not verify is refused — the store never silently falls back
to an older generation and never applies part of one. Unpublished intents are
recorded as abandoned, a missing commit marker is repaired, staged files are
removed, and a torn journal tail is truncated and reported. When the previous
generation is retained, its digest must match the chain digest recorded in the
newest header, so a swapped or edited older file is detected.

### 4.4 Single writer and control epochs

Exactly one process may hold a store: on Windows the lock file is opened with a
zero share mode and locked with `LockFileEx`; elsewhere `flock` is used.
The kernel owns the lock, so an abrupt death releases it. Opening a store does
**not** grant mutation authority: a successor must adopt a new control epoch and
incarnation explicitly, after which every token, authorization and decision from
the previous epoch is fenced and rejected.

---

## 5. Concurrency model and lock audit

* The coordinator owns one mutex. Every public method takes it once and calls
  only private helpers that assume it is held; there is no re-entrant locking,
  no recursive mutex, and no helper that reacquires it.
* The state store is deliberately *not* internally synchronised: the coordinator
  serialises every call, so there is no second lock and no lock ordering
  question between them. The store's only kernel-level lock is the process
  lifetime file lock taken at open.
* No observer or callback is invoked while a store-internal critical section is
  held. The single callback is the commit-stage observer used for instrumentation
  and crash injection; it runs on the committing thread, it is documented as
  non-reentrant, re-entering the store from it is refused with an internal error
  rather than deadlocking, and an exception escaping it cannot break
  publication.
* Shutdown joins no workers: there are no worker threads inside the library.
  Every value returned by reference (committed view, restriction set, decision)
  is either a copy or owned by the caller; no view outlives the state it
  describes.
* The audit of these paths, including callback-under-lock, join-under-lock,
  cancellation ordering and view lifetime, is recorded in the header comment of
  `src/runtime/coordinator.cpp` and exercised by the concurrency suite, which
  races four threads of evaluations, one identical-request replay group and a
  concurrent reader.

---

## 6. Failure semantics

Operations return `Status` or `Result<T>`; exceptions are not used for
control flow. Selected codes:

| Code | Meaning |
| --- | --- |
| `stale-authority` | the request does not own the current control epoch |
| `precondition-failed` | the expected state sequence is not current |
| `conflict` | an idempotency key was reused for a different request |
| `fenced` | the referenced decision or authorization is superseded |
| `locked` | another process holds the single-writer store lock |
| `policy-rejected` | the policy document is structurally invalid |
| `evidence-unusable` | evidence cannot be used as published |
| `not-permitted` | an authority or evidence precondition is missing |
| `corrupt` / `truncated` / `incompatible-format` | durable state refused rather than guessed at |
| `overflow` / `underflow` | checked arithmetic refused to wrap |
| `limit-exceeded` | a declared bound was exceeded |

A refused evaluation publishes nothing. A refusal is always accompanied by the
reason trace that explains it.

---

## 7. Validation actually performed

Host: Windows, MSVC 19.44 (Visual Studio 2022 Build Tools), CMake 4.3, Ninja,
x64, local NTFS. Configuration: `/W4 /WX /permissive-` on every first-party
target.

### 7.1 Suites

| Suite | What it proves |
| --- | --- |
| `dom_test_unit` | SHA-256 vectors, checked arithmetic and counter exhaustion, unit safety, canonical evidence construction and conflicts, freshness classification, decidable predicates, policy validation (accepted and rejected cases), obligation protection, canonical reason traces, generation vectors |
| `dom_test_codec` | canonical binary and text round trips, exact text round trip, declaration-order independence, truncation at every prefix length, hostile length fields, a deterministic 2000-mutation fuzz (seed 1592594996) that requires every accepted mutation to re-encode byte for byte, strict text parsing (unknown keywords, arity, quoting, escapes, numbers, units, UTF-8), total enum helpers |
| `dom_test_engine` | healthy hold, conservative deterministic escalation, most-restrictive selection, dwell and recovery hold, stale evidence becoming indeterminate without restoring service, missing and conflicted evidence, latch hold and clear, step limit and ladder blocking, structural refusals, unit mismatch never coercing, future-dated evidence |
| `dom_test_property` | a 400-step seeded randomized state machine (seed 1372628965) checked after every step against an independently written reference model: no relaxation without genuine health, no relaxation on unusable evidence, escalation always equal to the reference answer, indeterminate never weaker than the mode it retains, decisions bound to the evidence and epoch they were made from, per-step determinism; seed reproducibility; six restart cycles against a durable store |
| `dom_test_persistence` | fresh store state, explicit initialisation, monotonic commits surviving reopen, staged-file removal, corrupt/truncated/extended snapshots refused, generation chain verification, unpublished intents discarded, ghost commits refused, missing commit markers repaired, torn journal tails discarded and the store continuing to work, commit preconditions, observer re-entrancy refused, long (>260 character) and non-ASCII paths, bounded trimming, policy text loading |
| `dom_test_concurrency` | four threads of evaluations producing one contiguous deterministic sequence with no skipped or duplicated generation, six threads sending the identical request publishing exactly one generation and replaying the rest, a reader thread observing monotonically non-decreasing sequences while writers commit |
| `dom_test_multiprocess` | a second real process refused while the holder lives, abrupt holder death releasing the kernel lock, the successor required to adopt a new epoch before any mutation, a real process killed at each of the five publication boundaries recovering to a whole generation (stages 0–2 recover the previous generation, stages 3–4 recover the new one; stage 2 reports a discarded unpublished intent, stage 3 a repaired commit marker), and authority from the previous epoch fenced after recovery |
| `dom_test_integration` | the full lifecycle (decision → authorization → acknowledgement → verified effect → superseded binding), idempotent replay and conflicting key reuse, stale envelope refusals, authorization bounds, latch clear requiring cause digest, authority and fresh evidence, bounded ordered history, reopen keeping the mode and requiring adoption |
| `dom_test_scale` | the measured workloads in section 7.4 |

### 7.2 Configurations

| Configuration | Result |
| --- | --- |
| Release | 9 of 9 suites pass, zero first-party warnings |
| Debug | 9 of 9 suites pass, zero first-party warnings |
| AddressSanitizer (Release with debug information) | 9 of 9 suites pass, no sanitizer report |

### 7.3 Static analysis

MSVC static analysis (`/analyze`) is run over every first-party target —
library, tools, test harness and suites — with the warning gate still on. The
first run produced four genuine findings, all fixed at the source rather than
suppressed:

* `C28020` in the SHA-256 update path: the staging-buffer bound was only
  implicit. The write is now explicitly guarded so the invariant is stated
  where it is relied on.
* `C6262` three times: a 64 KB and a 32 KB buffer allocated on the stack in
  the file reader, the executable-path helper and the child-process reader. All
  three staging buffers now live on the heap, so threads with small stacks are
  no longer a limitation.

The final analysis run reports zero findings. On hosts without clang-tidy the
`.clang-tidy` configuration is provided for the same purpose.

### 7.4 Benchmarks

Both workloads are synthetic facility models; the durability is real.

| Workload | Scale | Measured |
| --- | --- | --- |
| engine evaluation (no I/O) | 32 modes, 513 records, 2000 evaluations | 500 evaluations/second |
| durable publication | 32 modes, 513 records, 80 evaluations | 81 generations, 769471 bytes (about 9.5 KB each), 73 committed generations/second |

Durable publication counts completed generations, including the real staged
write, flush, read-back verification, atomic rename and journal flushes.

### 7.5 Packaging, downstream consumption and fresh clone

1. A clean install prefix is produced by `scripts/closure.ps1 -Stage install`
   (`cmake --install` into a prefix that is deleted first).
2. `examples/consumer` is an independent project that sees only the installed
   package: `find_package(DegradedOperationManager 1.0 CONFIG REQUIRED)` and
   the exported targets. It is configured and built in a scratch directory
   outside the repository, then run against a scratch store. It prints, and
   exits zero only if the authority behaves as documented:

   ~~~
   product=Degraded Operation Manager version=1.0.0
   policy-digest=dbeaab6af42326305dbdbf73c2e0b3f05a95fb9ae6c3ac94055f37e3dc25cab5
   step=0 state-sequence=2 posture=emergency verdict=escalate restricted-services=batch
   step=1 state-sequence=3 posture=emergency verdict=hold restricted-services=batch
   final state-sequence=3 decision-sequence=2 recovery=fresh
   ~~~

3. `scripts/closure.ps1 -Stage fresh` clones the configured remote URL into a
   disposable directory outside the repository, configures, builds and tests
   both Release and Debug, installs each into a fresh prefix, runs the
   downstream consumer against each installed prefix, and then deletes the
   clone.

### 7.6 Defects found and fixed during hardening

| Where | Defect | Fix |
| --- | --- | --- |
| evaluation engine | an explicit latch clear was evaluated after the indeterminate branch, so a clear refused for unusable evidence reported the indeterminate transition instead of `latch-clear-refused` | the clear is evaluated first and never changes the mode |
| state store | a mutation that did not come from an evaluation could publish a payload whose committed sequence disagreed with the file sequence; the store detected it and refused the commit | the store now owns the invariant and stamps the published sequence into the committed view |
| canonical decoder | a decoded string could contain an embedded NUL that the encoder then refused, so a payload could decode but not re-encode | embedded NUL is rejected at decode |
| canonical text codec | a quoted value containing spaces after `name=` was split into several tokens, so an evidence record with a spaced producer name did not round trip | the tokenizer accepts a quoted section anywhere inside a token |
| text decoder | non-UTF-8 bytes in a text document were carried into the domain model | the whole document is validated as UTF-8 before parsing |
| policy validation | a mode could deny a service that an *inviolable* obligation depends on as long as it did not list that obligation as protected | an inviolable obligation is enforced facility wide across every mode |
| test harness (found by AddressSanitizer) | `DOM_CHECK_EQ` bound references into a temporary `Result`, which dangled before the comparison | the macro copies its operands |
| static analysis | see section 7.3 | |

---

## 8. REAL / SYNTHETIC / UNSUPPORTED

**REAL**: the state store and its file formats, kernel-enforced single-writer
locking, atomic publication, crash recovery from real terminated processes,
independent processes, the concurrency model, checked arithmetic, canonical
encoding and digests, the install/export package, and the downstream consumer.

**SYNTHETIC**: the facility, its incidents, failure domains, capacity,
redundancy, power, cooling and maintenance measurements, the mode catalog used
in examples and tests, and all benchmark workloads. Nothing in this repository
talks to real facility hardware.

**UNSUPPORTED**: BMS/DCIM integration, PDU, UPS or generator actuation, cooling
hardware control, accelerator or RDMA/NVLink hardware, multi-host or
distributed operation, cross-site replication, cryptographic operator
authentication, and any vendor SDK. DOM records what adjacent owners report; it
never performs their work.

---

## 9. Genuine limitations

* Evidence must be published by adjacent owners; DOM cannot validate that a
  measurement is physically true, only that it is well formed, unambiguous,
  fresh enough for the decision it is used for, and attributed to a producer.
* Operator authority is an explicit reference string with matching evidence and
  cause digest, not a cryptographic credential. A deployment that needs
  authenticated operators must put DOM behind an authenticated control path.
* Recovery is staged by class. A policy that limits
  `max_recovery_step` to fewer classes than the ladder it expects to recover
  through will refuse recovery with an explicit
  `recovery-step-limit` reason rather than guess; the limit or the ladder must
  be changed deliberately.
* The idempotency window, history, authorization and verification records are
  bounded; the oldest entries are evicted. An evicted authorization cannot be
  re-bound, because authorizations are fenced by decision and epoch, but its
  acknowledgement history is gone.
* Recovery is single-node. There is no consensus, no leader election and no
  cross-host replication; the single-writer lock is what makes one process
  authoritative on one host.
* Durable throughput is dominated by real flush and rename costs (about 73
  committed generations per second at the measured state size on this host); a
  higher rate requires a different durability contract, not a faster encoder.

---

## 10. Command line tools

| Tool | Purpose |
| --- | --- |
| `dom_cli` | `version`, `validate-policy`, `status`, `history`, `evaluate`, `authorize`, `acknowledge`, `verify`, `clear-latch`, `adopt-epoch` |
| `dom_authority` | long-lived authority holder: adopts an epoch once and serves `status`, `evaluate` and `authorize` commands on standard input |

Every mutating `dom_cli` invocation is a new process, so it explicitly adopts a
new control epoch before it mutates anything; nothing is inherited by possession
of the store directory.

---

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
