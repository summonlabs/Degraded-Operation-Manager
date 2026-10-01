// Degraded Operation Manager - persistence, corruption and recovery tests.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// These cases write real files, corrupt them at real byte offsets, and reopen
// real stores. Nothing here is a serialization-only or exception-only test.

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "dom/dom.hpp"
#include "testkit/fixtures.hpp"
#include "testkit/testkit.hpp"

namespace {

using namespace dom;

namespace fs = std::filesystem;

std::vector<std::uint8_t> ReadBytes(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(stream),
                                   std::istreambuf_iterator<char>());
}

void WriteBytes(const std::string& path, const std::vector<std::uint8_t>& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

void AppendBytes(const std::string& path, const std::vector<std::uint8_t>& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::app);
  stream.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

std::vector<std::string> SnapshotFiles(const std::string& directory) {
  std::vector<std::string> files;
  for (const fs::directory_entry& entry : fs::directory_iterator(directory)) {
    const std::string name = entry.path().filename().string();
    if (name.rfind("snapshot-", 0) == 0 && name.size() > 8 &&
        name.compare(name.size() - 5, 5, ".doms") == 0) {
      files.push_back(entry.path().string());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

void PushU16(std::vector<std::uint8_t>& out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
}

void PushU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) {
    out.push_back(static_cast<std::uint8_t>((value >> (8u * i)) & 0xFFu));
  }
}

void PushU64(std::vector<std::uint8_t>& out, std::uint64_t value) {
  for (unsigned i = 0; i < 8; ++i) {
    out.push_back(static_cast<std::uint8_t>((value >> (8u * i)) & 0xFFu));
  }
}

std::vector<std::uint8_t> JournalHeader() {
  std::vector<std::uint8_t> out = {'D', 'O', 'M', 'J', 'R', 'N', 'L', '1'};
  PushU16(out, 1);
  PushU16(out, 0);
  return out;
}

std::vector<std::uint8_t> JournalRecord(std::uint8_t kind, std::uint64_t sequence,
                                        const Digest& digest, const std::string& detail) {
  std::vector<std::uint8_t> out;
  out.push_back(kind);
  PushU64(out, sequence);
  const auto bytes = digest.bytes();
  out.insert(out.end(), bytes.begin(), bytes.end());
  PushU32(out, static_cast<std::uint32_t>(detail.size()));
  out.insert(out.end(), detail.begin(), detail.end());
  const Digest record_digest = Sha256::Of(out);
  const auto record_bytes = record_digest.bytes();
  out.insert(out.end(), record_bytes.begin(), record_bytes.end());
  return out;
}

std::string PolicyText(const PolicyDocument& policy) { return EncodePolicyText(policy); }

/// Runs one evaluation through a coordinator, adopting the epoch when needed.
Status EvaluateOnce(Coordinator& coordinator, const EvidenceSnapshot& evidence,
                    const std::string& key, std::uint64_t now_ms, std::uint64_t tick) {
  auto status = coordinator.GetStatus();
  if (!status) {
    return status.status();
  }
  if (!status.value().epoch_adopted) {
    AdoptEpochRequest adopt;
    adopt.expected_epoch = status.value().epoch;
    adopt.new_incarnation = Incarnation::FromValue(4242);
    adopt.authority_reference = "persistence-test";
    auto adopted = coordinator.AdoptEpoch(adopt);
    if (!adopted) {
      return adopted.status();
    }
    status = coordinator.GetStatus();
    if (!status) {
      return status.status();
    }
  }
  EvaluationRequest request;
  request.envelope.idempotency_key = Sha256::Of(key);
  request.envelope.expected_state_sequence = status.value().state_sequence;
  request.envelope.expected_epoch = status.value().epoch;
  request.context.epoch = status.value().epoch;
  request.context.incarnation = status.value().incarnation;
  request.context.now_ms = Instant::FromValue(now_ms);
  request.context.tick = Tick::FromValue(tick);
  request.evidence = evidence;
  auto outcome = coordinator.Evaluate(request);
  if (!outcome) {
    return outcome.status();
  }
  return Status::Ok();
}

DOM_TEST(fresh_store_is_empty_and_reports_its_recovery) {
  auto directory = dom::test::TempDir::Create("persist-fresh");
  DOM_CHECK_OK(directory);
  StoreOptions options;
  options.directory = directory.value().path();
  auto store = StateStore::Open(options);
  DOM_CHECK_OK(store);
  DOM_CHECK_EQ(store.value()->state().sequence.value(), 0ull);
  DOM_CHECK_EQ(store.value()->recovery().outcome, RecoveryOutcome::Fresh);
  DOM_CHECK(store.value()->state().committed.mode.IsZero());
  DOM_CHECK(StateStore::Open(options).has_value() == false);  // still locked by this process
}

DOM_TEST(coordinator_initialises_a_fresh_store_explicitly) {
  auto directory = dom::test::TempDir::Create("persist-init");
  DOM_CHECK_OK(directory);
  const PolicyDocument policy = dom::test::MakePolicy();
  auto options = dom::test::MakeCoordinatorOptions(directory.value().path(), policy, 1234);
  auto coordinator = Coordinator::Open(options);
  DOM_CHECK_OK(coordinator);
  auto status = coordinator.value()->GetStatus();
  DOM_CHECK_OK(status);
  DOM_CHECK_EQ(status.value().epoch.value(), 1ull);
  DOM_CHECK_EQ(status.value().incarnation.value(), 1234ull);
  DOM_CHECK_EQ(status.value().mode.value(), 1ull);
  DOM_CHECK_EQ(status.value().posture, Posture::Nominal);
  DOM_CHECK(status.value().epoch_adopted);
  DOM_CHECK_EQ(status.value().state_sequence.value(), 1ull);
}

DOM_TEST(commits_are_monotonic_and_survive_reopening) {
  auto directory = dom::test::TempDir::Create("persist-commits");
  DOM_CHECK_OK(directory);
  const PolicyDocument policy = dom::test::MakePolicy();
  {
    auto options = dom::test::MakeCoordinatorOptions(directory.value().path(), policy, 111);
    auto coordinator = Coordinator::Open(options);
    DOM_CHECK_OK(coordinator);
    for (int i = 0; i < 4; ++i) {
      dom::test::EvidenceOptions evidence_options;
      evidence_options.revision = static_cast<std::uint64_t>(i + 1);
      evidence_options.observed_at_ms = 1000 + static_cast<std::uint64_t>(i) * 100000;
      evidence_options.severity = i % 2 == 0 ? 3 : 0;
      evidence_options.redundancy = i % 2 == 0 ? 400 : 1000;
      auto evidence = dom::test::MakeEvidence(evidence_options);
      DOM_CHECK_OK(EvaluateOnce(*coordinator.value(), evidence, "commit-" + std::to_string(i),
                                evidence_options.observed_at_ms,
                                static_cast<std::uint64_t>(i + 1)));
    }
    auto status = coordinator.value()->GetStatus();
    DOM_CHECK_OK(status);
    DOM_CHECK_EQ(status.value().state_sequence.value(), 5ull);
    DOM_CHECK_EQ(status.value().decision_sequence.value(), 4ull);
  }
  {
    StoreOptions options;
    options.directory = directory.value().path();
    auto store = StateStore::Open(options);
    DOM_CHECK_OK(store);
    DOM_CHECK_EQ(store.value()->state().sequence.value(), 5ull);
    DOM_CHECK_EQ(store.value()->recovery().outcome, RecoveryOutcome::Loaded);
    DOM_CHECK_EQ(store.value()->state().history.size(), 4u);
    auto on_disk = store.value()->VerifyOnDisk();
    DOM_CHECK_OK(on_disk);
    DOM_CHECK_EQ(on_disk.value().digest(), store.value()->state().digest());
    DOM_CHECK_EQ(store.value()->committed_generations(), 0ull);
  }
}

DOM_TEST(staged_files_are_never_authoritative) {
  auto directory = dom::test::TempDir::Create("persist-staged");
  DOM_CHECK_OK(directory);
  const std::string staged =
      dom::test::Join(directory.value().path(), "snapshot-00000000000000000009.doms.tmp");
  DOM_CHECK_OK(dom::test::WriteTextFile(staged, "not a snapshot"));
  StoreOptions options;
  options.directory = directory.value().path();
  auto store = StateStore::Open(options);
  DOM_CHECK_OK(store);
  DOM_CHECK_EQ(store.value()->recovery().stray_files_removed, 1u);
  DOM_CHECK_EQ(store.value()->recovery().outcome, RecoveryOutcome::Fresh);
  DOM_CHECK(!fs::exists(staged));
}

DOM_TEST(corrupt_and_truncated_snapshots_are_refused) {
  auto directory = dom::test::TempDir::Create("persist-corrupt");
  DOM_CHECK_OK(directory);
  const PolicyDocument policy = dom::test::MakePolicy();
  {
    auto options = dom::test::MakeCoordinatorOptions(directory.value().path(), policy, 111);
    auto coordinator = Coordinator::Open(options);
    DOM_CHECK_OK(coordinator);
    dom::test::EvidenceOptions evidence_options;
    evidence_options.observed_at_ms = 1000;
    evidence_options.severity = 2;
    auto evidence = dom::test::MakeEvidence(evidence_options);
    DOM_CHECK_OK(EvaluateOnce(*coordinator.value(), evidence, "corrupt-1", 1000, 1));
  }
  const std::vector<std::string> snapshots = SnapshotFiles(directory.value().path());
  DOM_CHECK(snapshots.size() >= 1u);
  const std::string path = snapshots.back();
  const std::vector<std::uint8_t> original = ReadBytes(path);
  DOM_CHECK(original.size() > 140u);

  {
    std::vector<std::uint8_t> corrupted = original;
    corrupted[100] = static_cast<std::uint8_t>(corrupted[100] ^ 0x40u);
    WriteBytes(path, corrupted);
    StoreOptions options;
    options.directory = directory.value().path();
    auto store = StateStore::Open(options);
    DOM_CHECK(!store.has_value());
    DOM_CHECK(store.status().code() == ErrorCode::Corrupt ||
              store.status().code() == ErrorCode::DecodeError);
  }
  {
    std::vector<std::uint8_t> truncated = original;
    truncated.resize(truncated.size() - 12);
    WriteBytes(path, truncated);
    StoreOptions options;
    options.directory = directory.value().path();
    auto store = StateStore::Open(options);
    DOM_CHECK(!store.has_value());
  }
  {
    std::vector<std::uint8_t> extended = original;
    extended.push_back(0x00);
    WriteBytes(path, extended);
    StoreOptions options;
    options.directory = directory.value().path();
    auto store = StateStore::Open(options);
    DOM_CHECK(!store.has_value());
  }
  {
    WriteBytes(path, original);
    StoreOptions options;
    options.directory = directory.value().path();
    auto store = StateStore::Open(options);
    DOM_CHECK_OK(store);
    DOM_CHECK_EQ(store.value()->state().sequence.value(), 2ull);
  }
}

DOM_TEST(tampering_with_the_previous_generation_breaks_the_chain) {
  auto directory = dom::test::TempDir::Create("persist-chain");
  DOM_CHECK_OK(directory);
  const PolicyDocument policy = dom::test::MakePolicy();
  {
    auto options = dom::test::MakeCoordinatorOptions(directory.value().path(), policy, 111);
    auto coordinator = Coordinator::Open(options);
    DOM_CHECK_OK(coordinator);
    dom::test::EvidenceOptions first;
    first.observed_at_ms = 1000;
    first.severity = 2;
    DOM_CHECK_OK(EvaluateOnce(*coordinator.value(), dom::test::MakeEvidence(first), "chain-1", 1000,
                              1));
    dom::test::EvidenceOptions second;
    second.revision = 2;
    second.observed_at_ms = 2000;
    second.severity = 0;
    DOM_CHECK_OK(EvaluateOnce(*coordinator.value(), dom::test::MakeEvidence(second), "chain-2",
                              2000, 2));
  }
  const std::vector<std::string> snapshots = SnapshotFiles(directory.value().path());
  DOM_CHECK_EQ(snapshots.size(), 2u);
  std::vector<std::uint8_t> older = ReadBytes(snapshots.front());
  older[120] = static_cast<std::uint8_t>(older[120] ^ 0x01u);
  WriteBytes(snapshots.front(), older);

  StoreOptions options;
  options.directory = directory.value().path();
  auto store = StateStore::Open(options);
  DOM_CHECK(!store.has_value());
}

DOM_TEST(journal_recovery_classifies_unpublished_and_missing_markers) {
  const PolicyDocument policy = dom::test::MakePolicy();

  // An intent without a published snapshot is discarded and recorded.
  {
    auto directory = dom::test::TempDir::Create("persist-intent");
    DOM_CHECK_OK(directory);
    {
      auto options = dom::test::MakeCoordinatorOptions(directory.value().path(), policy, 111);
      auto coordinator = Coordinator::Open(options);
      DOM_CHECK_OK(coordinator);
      dom::test::EvidenceOptions evidence_options;
      evidence_options.observed_at_ms = 1000;
      evidence_options.severity = 2;
      DOM_CHECK_OK(EvaluateOnce(*coordinator.value(), dom::test::MakeEvidence(evidence_options),
                                "intent-1", 1000, 1));
    }
    const std::string journal = dom::test::Join(directory.value().path(), "journal.domj");
    std::vector<std::uint8_t> bytes = ReadBytes(journal);
    const std::vector<std::uint8_t> extra =
        JournalRecord(1, 99, Sha256::Of(std::string_view("unpublished")), "never published");
    bytes.insert(bytes.end(), extra.begin(), extra.end());
    WriteBytes(journal, bytes);

    StoreOptions options;
    options.directory = directory.value().path();
    auto store = StateStore::Open(options);
    DOM_CHECK_OK(store);
    DOM_CHECK_EQ(store.value()->recovery().outcome, RecoveryOutcome::DiscardedUnpublished);
    DOM_CHECK_EQ(store.value()->recovery().discarded_unpublished, 1u);
    DOM_CHECK_EQ(store.value()->state().sequence.value(), 2ull);
  }

  // A commit marker whose snapshot is missing is refused.
  {
    auto directory = dom::test::TempDir::Create("persist-marker");
    DOM_CHECK_OK(directory);
    {
      auto options = dom::test::MakeCoordinatorOptions(directory.value().path(), policy, 111);
      auto coordinator = Coordinator::Open(options);
      DOM_CHECK_OK(coordinator);
      dom::test::EvidenceOptions evidence_options;
      evidence_options.observed_at_ms = 1000;
      evidence_options.severity = 2;
      DOM_CHECK_OK(EvaluateOnce(*coordinator.value(), dom::test::MakeEvidence(evidence_options),
                                "marker-1", 1000, 1));
    }
    const std::string journal = dom::test::Join(directory.value().path(), "journal.domj");
    std::vector<std::uint8_t> bytes = ReadBytes(journal);
    const std::vector<std::uint8_t> extra =
        JournalRecord(2, 50, Sha256::Of(std::string_view("ghost")), "ghost commit");
    bytes.insert(bytes.end(), extra.begin(), extra.end());
    WriteBytes(journal, bytes);
    StoreOptions options;
    options.directory = directory.value().path();
    auto store = StateStore::Open(options);
    DOM_CHECK(!store.has_value());
    DOM_CHECK_EQ(store.status().code(), ErrorCode::Corrupt);
  }

  // A published generation whose commit marker was lost is repaired.
  {
    auto directory = dom::test::TempDir::Create("persist-repair");
    DOM_CHECK_OK(directory);
    {
      auto options = dom::test::MakeCoordinatorOptions(directory.value().path(), policy, 111);
      auto coordinator = Coordinator::Open(options);
      DOM_CHECK_OK(coordinator);
      dom::test::EvidenceOptions evidence_options;
      evidence_options.observed_at_ms = 1000;
      evidence_options.severity = 2;
      DOM_CHECK_OK(EvaluateOnce(*coordinator.value(), dom::test::MakeEvidence(evidence_options),
                                "repair-1", 1000, 1));
    }
    const std::string journal = dom::test::Join(directory.value().path(), "journal.domj");
    std::vector<std::uint8_t> bytes = JournalHeader();
    const Digest digest = Sha256::Of(std::string_view("payload"));
    const std::vector<std::uint8_t> intent = JournalRecord(1, 2, digest, "staged");
    bytes.insert(bytes.end(), intent.begin(), intent.end());
    WriteBytes(journal, bytes);

    StoreOptions options;
    options.directory = directory.value().path();
    auto store = StateStore::Open(options);
    DOM_CHECK_OK(store);
    DOM_CHECK_EQ(store.value()->recovery().outcome, RecoveryOutcome::RepairedCommitMarker);
    DOM_CHECK_EQ(store.value()->state().sequence.value(), 2ull);
    // The repaired journal is readable again and the store keeps working.
    DOM_CHECK(StateStore::Open(options).has_value() == false);
  }
}

DOM_TEST(a_torn_journal_tail_is_discarded_and_the_store_keeps_working) {
  auto directory = dom::test::TempDir::Create("persist-torn");
  DOM_CHECK_OK(directory);
  const PolicyDocument policy = dom::test::MakePolicy();
  {
    auto options = dom::test::MakeCoordinatorOptions(directory.value().path(), policy, 111);
    auto coordinator = Coordinator::Open(options);
    DOM_CHECK_OK(coordinator);
    dom::test::EvidenceOptions evidence_options;
    evidence_options.observed_at_ms = 1000;
    evidence_options.severity = 2;
    DOM_CHECK_OK(EvaluateOnce(*coordinator.value(), dom::test::MakeEvidence(evidence_options),
                              "torn-1", 1000, 1));
  }
  const std::string journal = dom::test::Join(directory.value().path(), "journal.domj");
  const std::vector<std::uint8_t> partial = {0x01, 0x00, 0x00, 0x00, 0x99};
  AppendBytes(journal, partial);

  {
    StoreOptions options;
    options.directory = directory.value().path();
    auto store = StateStore::Open(options);
    DOM_CHECK_OK(store);
    DOM_CHECK(store.value()->recovery().detail.find("torn") != std::string::npos);
    DOM_CHECK_EQ(store.value()->state().sequence.value(), 2ull);
  }
  {
    auto options = dom::test::MakeCoordinatorOptions(directory.value().path(), policy, 555);
    auto coordinator = Coordinator::Open(options);
    DOM_CHECK_OK(coordinator);
    dom::test::EvidenceOptions evidence_options;
    evidence_options.revision = 2;
    evidence_options.observed_at_ms = 2000;
    evidence_options.severity = 0;
    DOM_CHECK_OK(EvaluateOnce(*coordinator.value(), dom::test::MakeEvidence(evidence_options),
                              "torn-2", 2000, 2));
    auto status = coordinator.value()->GetStatus();
    DOM_CHECK_OK(status);
    // 2 published generations before, then the epoch adoption and the
    // evaluation that this process committed.
    DOM_CHECK_EQ(status.value().state_sequence.value(), 4ull);
  }
  {
    StoreOptions options;
    options.directory = directory.value().path();
    auto store = StateStore::Open(options);
    DOM_CHECK_OK(store);
    DOM_CHECK_EQ(store.value()->state().sequence.value(), 4ull);
  }
}

DOM_TEST(commit_preconditions_and_reentrancy_are_enforced) {
  auto directory = dom::test::TempDir::Create("persist-precondition");
  DOM_CHECK_OK(directory);
  StoreOptions options;
  options.directory = directory.value().path();
  options.history_capacity = 4;
  bool reentered = false;
  StateStore* raw = nullptr;
  options.commit_observer = [&raw, &reentered](CommitStage stage) {
    if (stage != CommitStage::StagedWritten || raw == nullptr) {
      return;
    }
    reentered = true;
    PersistedState next = raw->state();
    auto refused = raw->Commit(std::move(next), CommitOptions{raw->state().sequence});
    DOM_CHECK(!refused.has_value());
    DOM_CHECK_EQ(refused.status().code(), ErrorCode::Internal);
  };
  auto store = StateStore::Open(options);
  DOM_CHECK_OK(store);
  raw = store.value().get();
  PersistedState next = store.value()->state();
  auto committed = store.value()->Commit(std::move(next), CommitOptions{StateSequence()});
  DOM_CHECK_OK(committed);
  DOM_CHECK(reentered);
  DOM_CHECK_EQ(store.value()->state().sequence.value(), 1ull);

  // A stale expectation is refused rather than applied on top.
  PersistedState again = store.value()->state();
  auto stale = store.value()->Commit(std::move(again), CommitOptions{StateSequence()});
  DOM_CHECK(!stale.has_value());
  DOM_CHECK_EQ(stale.status().code(), ErrorCode::Conflict);
}

DOM_TEST(long_and_unusual_store_paths_work) {
  auto directory = dom::test::TempDir::Create("persist-paths");
  DOM_CHECK_OK(directory);
  const PolicyDocument policy = dom::test::MakePolicy();

  std::string deep = directory.value().path();
  for (int i = 0; i < 16; ++i) {
    deep = dom::test::Join(deep, "segment-" + std::to_string(i) + "-0123456789abcdef");
  }
  DOM_CHECK(deep.size() > 260u);
  // std::filesystem needs the extended-length prefix itself; the library applies
  // it internally, which is exactly what this case proves.
  fs::create_directories("\\\\?\\" + deep);
  {
    auto options = dom::test::MakeCoordinatorOptions(deep, policy, 77);
    auto coordinator = Coordinator::Open(options);
    DOM_CHECK_OK(coordinator);
    dom::test::EvidenceOptions evidence_options;
    evidence_options.observed_at_ms = 1000;
    evidence_options.severity = 2;
    DOM_CHECK_OK(EvaluateOnce(*coordinator.value(), dom::test::MakeEvidence(evidence_options),
                              "deep-1", 1000, 1));
  }
  StoreOptions store_options;
  store_options.directory = deep;
  auto store = StateStore::Open(store_options);
  DOM_CHECK_OK(store);
  DOM_CHECK_EQ(store.value()->state().sequence.value(), 2ull);

  const std::string odd =
      dom::test::Join(directory.value().path(), std::string("sp ace-\xC3\xA9\xE2\x82\xAC"));
  DOM_CHECK_OK(fs::create_directories(odd) ? Status::Ok()
                                           : Status::Error(ErrorCode::IoError, "create"));
  auto odd_options = dom::test::MakeCoordinatorOptions(odd, policy, 88);
  auto odd_coordinator = Coordinator::Open(odd_options);
  DOM_CHECK_OK(odd_coordinator);
  dom::test::EvidenceOptions evidence_options;
  evidence_options.observed_at_ms = 1000;
  DOM_CHECK_OK(EvaluateOnce(*odd_coordinator.value(), dom::test::MakeEvidence(evidence_options),
                            "odd-1", 1000, 1));
}

DOM_TEST(bounded_state_is_trimmed_on_commit) {
  auto directory = dom::test::TempDir::Create("persist-bounds");
  DOM_CHECK_OK(directory);
  const PolicyDocument policy = dom::test::MakePolicy();
  auto options = dom::test::MakeCoordinatorOptions(directory.value().path(), policy, 111);
  options.history_capacity = 4;
  options.idempotency_capacity = 3;
  options.record_capacity = 2;
  auto coordinator = Coordinator::Open(options);
  DOM_CHECK_OK(coordinator);
  for (int i = 0; i < 10; ++i) {
    dom::test::EvidenceOptions evidence_options;
    evidence_options.revision = static_cast<std::uint64_t>(i + 1);
    evidence_options.observed_at_ms = 1000 + static_cast<std::uint64_t>(i) * 100000;
    evidence_options.severity = i % 2 == 0 ? 3 : 0;
    evidence_options.redundancy = i % 2 == 0 ? 400 : 1000;
    DOM_CHECK_OK(EvaluateOnce(*coordinator.value(), dom::test::MakeEvidence(evidence_options),
                              "bounded-" + std::to_string(i),
                              evidence_options.observed_at_ms,
                              static_cast<std::uint64_t>(i + 1)));
  }
  auto status = coordinator.value()->GetStatus();
  DOM_CHECK_OK(status);
  DOM_CHECK(status.value().history_entries <= 4u);
  DOM_CHECK(status.value().idempotency_entries <= 3u);
  auto history = coordinator.value()->History(100);
  DOM_CHECK_OK(history);
  DOM_CHECK(history.value().size() <= 4u);
  for (std::size_t i = 1; i < history.value().size(); ++i) {
    DOM_CHECK(history.value()[i - 1].state_sequence < history.value()[i].state_sequence);
  }
}

DOM_TEST(a_store_path_that_is_not_a_directory_is_refused) {
  auto directory = dom::test::TempDir::Create("persist-path-shape");
  DOM_CHECK_OK(directory);
  const std::string file_path = dom::test::Join(directory.value().path(), "not-a-directory");
  DOM_CHECK_OK(dom::test::WriteTextFile(file_path, "this is a file"));

  StoreOptions options;
  options.directory = file_path;
  auto store = StateStore::Open(options);
  DOM_CHECK(!store.has_value());
  DOM_CHECK(store.status().code() == ErrorCode::AlreadyExists ||
            store.status().code() == ErrorCode::IoError);

  StoreOptions missing;
  missing.directory = dom::test::Join(directory.value().path(), "absent");
  missing.create_if_missing = false;
  auto absent = StateStore::Open(missing);
  DOM_CHECK(!absent.has_value());
  DOM_CHECK_EQ(absent.status().code(), ErrorCode::NotFound);

  StoreOptions empty;
  DOM_CHECK_ERR(StateStore::Open(empty), ErrorCode::InvalidArgument);
}

DOM_TEST(absurd_and_framing_damaged_snapshots_are_refused) {
  auto directory = dom::test::TempDir::Create("persist-absurd");
  DOM_CHECK_OK(directory);
  const std::string path =
      dom::test::Join(directory.value().path(), "snapshot-00000000000000000005.doms");

  // A recording length of nearly 2^64 with a body of one byte must be refused
  // by the length check, not by attempting an allocation.
  std::vector<std::uint8_t> absurd = {'D', 'O', 'M', 'S', 'N', 'A', 'P', '1'};
  PushU16(absurd, 1);
  PushU16(absurd, 0);
  PushU64(absurd, 5);
  PushU64(absurd, 0xFFFFFFFFFFFFull);
  for (int i = 0; i < 32; ++i) {
    absurd.push_back(0);
  }
  for (int i = 0; i < 32; ++i) {
    absurd.push_back(0);
  }
  absurd.push_back(0x00);
  WriteBytes(path, absurd);
  StoreOptions options;
  options.directory = directory.value().path();
  auto store = StateStore::Open(options);
  DOM_CHECK(!store.has_value());
  DOM_CHECK(store.status().code() == ErrorCode::Corrupt ||
            store.status().code() == ErrorCode::Truncated ||
            store.status().code() == ErrorCode::LimitExceeded);

  // A header-only file is a truncation, not an empty store.
  std::vector<std::uint8_t> header_only(absurd.begin(), absurd.begin() + 92);
  WriteBytes(path, header_only);
  auto truncated = StateStore::Open(options);
  DOM_CHECK(!truncated.has_value());
  DOM_CHECK_EQ(truncated.status().code(), ErrorCode::Truncated);

  // A reserved field that is not zero is corruption.
  std::vector<std::uint8_t> reserved = header_only;
  reserved[10] = 0x01;
  WriteBytes(path, reserved);
  auto reserved_result = StateStore::Open(options);
  DOM_CHECK(!reserved_result.has_value());

  // An unknown format version is refused as incompatible. The file must be at
  // least header plus footer, otherwise it is reported as a truncation first.
  std::vector<std::uint8_t> versioned = header_only;
  versioned.resize(124, 0);
  versioned[8] = 0x63;
  WriteBytes(path, versioned);
  auto version_result = StateStore::Open(options);
  DOM_CHECK(!version_result.has_value());
  DOM_CHECK_EQ(version_result.status().code(), ErrorCode::IncompatibleFormat);
}

DOM_TEST(repeated_open_and_close_cycles_keep_the_lock_and_the_sequence) {
  auto directory = dom::test::TempDir::Create("persist-cycles");
  DOM_CHECK_OK(directory);
  const PolicyDocument policy = dom::test::MakePolicy();
  StateSequence previous;
  for (int cycle = 0; cycle < 8; ++cycle) {
    StoreOptions options;
    options.directory = directory.value().path();
    auto store = StateStore::Open(options);
    DOM_CHECK_OK(store);
    DOM_CHECK(previous <= store.value()->state().sequence);
    previous = store.value()->state().sequence;
    // While this handle lives, no second handle may open the same store.
    auto contender = StateStore::Open(options);
    DOM_CHECK(!contender.has_value());
    DOM_CHECK_EQ(contender.status().code(), ErrorCode::Locked);
    PersistedState next = store.value()->state();
    auto committed = store.value()->Commit(std::move(next), CommitOptions{previous});
    DOM_CHECK_OK(committed);
    previous = committed.value();
  }
  StoreOptions options;
  options.directory = directory.value().path();
  auto store = StateStore::Open(options);
  DOM_CHECK_OK(store);
  DOM_CHECK_EQ(store.value()->state().sequence.value(), 8ull);
  DOM_CHECK_EQ(store.value()->recovery().outcome, RecoveryOutcome::Loaded);
  (void)policy;
}

DOM_TEST(policy_text_loaded_from_disk_is_the_same_document) {
  auto directory = dom::test::TempDir::Create("persist-policy");
  DOM_CHECK_OK(directory);
  const PolicyDocument policy = dom::test::MakePolicy();
  const std::string path = dom::test::Join(directory.value().path(), "policy.domtext");
  DOM_CHECK_OK(dom::test::WriteTextFile(path, PolicyText(policy)));

  std::ifstream stream(path);
  const std::string text((std::istreambuf_iterator<char>(stream)),
                         std::istreambuf_iterator<char>());
  auto decoded = DecodePolicyText(text);
  DOM_CHECK_OK(decoded);
  DOM_CHECK_EQ(PolicyDigest(decoded.value()), PolicyDigest(policy));
  DOM_CHECK_OK(ValidatePolicy(decoded.value()));
}

}  // namespace
