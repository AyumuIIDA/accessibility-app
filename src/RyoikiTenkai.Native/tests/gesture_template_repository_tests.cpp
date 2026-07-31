#include "HandInput/Recognition/gesture_template_repository.h"
#include "HandInput/Recognition/gesture_template_serialization.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <winsqlite/winsqlite3.h>
namespace recognition = ryoiki::hand_input::recognition;
namespace {
void require(bool c, const char *m) {
  if (!c)
    throw std::runtime_error(m);
}
recognition::UnifiedSequenceTemplate make(float marker) {
  recognition::UnifiedSequenceTemplate v;
  v.topology.valid = true;
  v.topology.palmTravel = marker;
  v.visualization.valid = true;
  for (std::size_t i = 0; i < v.frames.size(); ++i) {
    const auto index = static_cast<float>(i);
    v.frames[i].timeOffsetMs = static_cast<double>(i) * 10.0;
    v.frames[i].centerX = marker + index;
    v.frames[i].values[7] = marker;
    v.visualization.frames[i].timeOffsetMs = static_cast<double>(i) * 10.0;
    v.visualization.frames[i].centerX = marker;
    v.visualization.frames[i].skeleton[20] = {marker, marker + 1, marker + 2};
  }
  return v;
}
void executeSql(const std::filesystem::path &path, const char *sql) {
  sqlite3 *database = nullptr;
  require(sqlite3_open(path.string().c_str(), &database) == SQLITE_OK,
          "test database open failed");
  char *message = nullptr;
  const int result = sqlite3_exec(database, sql, nullptr, nullptr, &message);
  sqlite3_free(message);
  sqlite3_close(database);
  require(result == SQLITE_OK, "test SQL failed");
}
void testVersionOneMigration() {
  const auto path = std::filesystem::temp_directory_path() /
                    "ryoiki-gesture-repository-v1.sqlite";
  std::error_code ec;
  std::filesystem::remove(path, ec);
  executeSql(path,
             "CREATE TABLE definitions(id INTEGER PRIMARY KEY,name TEXT NOT NULL,enabled "
             "INTEGER NOT NULL DEFAULT 1); CREATE TABLE trials(definition_id INTEGER NOT "
             "NULL REFERENCES definitions(id) ON DELETE CASCADE,trial_index INTEGER NOT "
             "NULL,format_version INTEGER NOT NULL,payload BLOB NOT NULL,PRIMARY "
             "KEY(definition_id,trial_index)); PRAGMA user_version=1;");
  recognition::GestureTemplateRepository repository(path.string());
  std::string error;
  require(repository.open(error), error.c_str());
  recognition::GestureTemplateRegistry registry;
  const std::array<recognition::UnifiedSequenceTemplate, 3> takes{
      make(4), make(5), make(6)};
  require(repository.appendThreeTakeSet(registry, 9, "migrated", takes, error),
          error.c_str());
  std::filesystem::remove(path, ec);
}
void test() {
  const auto path = std::filesystem::temp_directory_path() /
                    "ryoiki-gesture-repository-test.sqlite";
  std::error_code ec;
  std::filesystem::remove(path, ec);
  recognition::GestureTemplateRepository repo(path.string());
  std::string error;
  require(repo.open(error), error.c_str());
  recognition::GestureTemplateRegistry registry;
  const std::array<recognition::UnifiedSequenceTemplate, 3> takes{
      make(1), make(2), make(3)};
  require(repo.appendThreeTakeSet(registry, 77, "wave", takes, error),
          error.c_str());
  require(registry.size() == 3, "commit did not update registry");
  std::vector<recognition::GestureDefinitionMetadata> metadata;
  require(repo.list(metadata, error) && metadata.size() == 1, "list failed");
  require(metadata[0].name == "wave" && metadata[0].takeCount == 3,
          "metadata mismatch");
  recognition::GestureTemplateRegistry restored;
  require(repo.load(restored, metadata, error), error.c_str());
  const auto *record = restored.find(77);
  require(record && record->trialCount == 3, "startup restore failed");
  const auto *oneHand = std::get_if<recognition::UnifiedSequenceTemplate>(
      &record->trials[2].payload);
  require(oneHand && oneHand->topology.palmTravel == 3 &&
              oneHand->visualization.frames[0].skeleton[20].z == 5,
          "explicit payload roundtrip failed");
  std::array<recognition::GestureTrial, 3> twoHandTakes{};
  for (std::size_t i = 0; i < twoHandTakes.size(); ++i) {
    recognition::TwoHandTemplate value;
    value.valid = true;
    value.sourceFrameCount = 40 + i;
    value.durationMs = 2100.0 + static_cast<double>(i);
    value.frames[0].values[131] = 10.0F + static_cast<float>(i);
    value.summary.meanRelativeDistance = 0.5F + static_cast<float>(i);
    twoHandTakes[i] = {recognition::GestureTrialKind::TwoHandDynamic, value};
  }
  std::array<recognition::GestureRecordingProvenance, 3> provenance{};
  for (std::size_t i = 0; i < provenance.size(); ++i) {
    provenance[i].sourceId = "source-" + std::to_string(i);
    provenance[i].takeIndex = static_cast<std::uint32_t>(i + 1);
    provenance[i].capturedAtUs = 1000 + i;
    provenance[i].quality.accepted = true;
    provenance[i].quality.sourceFrameCount = 1;
    provenance[i].frames.resize(1);
    provenance[i].frames[0].handCount = 2;
    provenance[i].frames[0].normalizedSkeletons[1][20].z = 7.0F + static_cast<float>(i);
  }
  require(repo.appendThreeTakeSet(registry, 88, "two hand", twoHandTakes, provenance, error),
          error.c_str());
  recognition::GestureTemplateRegistry restoredBoth;
  require(repo.load(restoredBoth, metadata, error), error.c_str());
  const auto *twoHandRecord = restoredBoth.find(88);
  const auto *twoHand = twoHandRecord
      ? std::get_if<recognition::TwoHandTemplate>(&twoHandRecord->trials[2].payload)
      : nullptr;
  require(twoHandRecord && twoHandRecord->trials[2].kind ==
                               recognition::GestureTrialKind::TwoHandDynamic &&
              twoHand && twoHand->sourceFrameCount == 42 &&
              twoHand->frames[0].values[131] == 12.0F &&
              twoHand->summary.meanRelativeDistance == 2.5F,
          "two-hand payload roundtrip failed");
  std::vector<recognition::GestureRecordingProvenance> restoredRecordings;
  require(repo.listRecordings(88, restoredRecordings, error) &&
              restoredRecordings.size() == 3 &&
              restoredRecordings[2].sourceId == "source-2" &&
              restoredRecordings[2].frames[0].normalizedSkeletons[1][20].z == 9.0F,
          "recording provenance roundtrip failed");
  executeSql(path, "UPDATE trials SET kind=99 WHERE definition_id=88 AND trial_index=0;");
  recognition::GestureTemplateRegistry unknownKind;
  require(!repo.load(unknownKind, metadata, error), "unknown kind was accepted");
  executeSql(path, "UPDATE trials SET kind=1 WHERE definition_id=88 AND trial_index=0;");
  require(repo.updateMetadata(77, "wave renamed", false, error),
          "metadata update failed");
  require(repo.upsertBinding({77, true, "keyboard.hotkey", "CTRL+K"}, error),
          error.c_str());
  std::vector<recognition::GestureBindingMetadata> bindings;
  require(repo.listBindings(bindings, error) && bindings.size() == 1
              && bindings[0].definitionId == 77
              && bindings[0].actionType == "keyboard.hotkey"
              && bindings[0].actionParameter == "CTRL+K",
          "binding roundtrip failed");
  recognition::GestureTemplateRegistry disabled;
  require(repo.load(disabled, metadata, error) && disabled.find(77) == nullptr &&
              disabled.find(88) != nullptr,
          "disabled definition restored as active");
  require(repo.remove(77, disabled, error), "delete failed");
  require(repo.listBindings(bindings, error) && bindings.empty(),
          "definition delete did not cascade binding");
  require(repo.list(metadata, error) && metadata.size() == 1 && metadata[0].id == 88,
          "delete did not cascade");
  std::filesystem::remove(path, ec);
}
} // namespace
int main() {
  try {
    testVersionOneMigration();
    test();
    std::cout << "Gesture template repository tests passed.\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
