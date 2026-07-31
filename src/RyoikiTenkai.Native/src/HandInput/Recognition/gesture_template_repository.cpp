#include "HandInput/Recognition/gesture_template_repository.h"
#include "HandInput/Recognition/gesture_template_serialization.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <utility>
#include <winsqlite/winsqlite3.h>
namespace ryoiki::hand_input::recognition {
namespace {
constexpr int kGestureDatabaseSchemaVersion = 4;

bool sql(sqlite3 *db, const char *text, std::string &error) {
  char *message = nullptr;
  const int rc = sqlite3_exec(db, text, nullptr, nullptr, &message);
  if (rc == SQLITE_OK)
    return true;
  error = message ? message : sqlite3_errmsg(db);
  sqlite3_free(message);
  return false;
}
struct Stmt {
  sqlite3_stmt *value{};
  ~Stmt() {
    if (value)
      sqlite3_finalize(value);
  }
};
bool prep(sqlite3 *db, const char *q, Stmt &s, std::string &e) {
  if (sqlite3_prepare_v2(db, q, -1, &s.value, nullptr) == SQLITE_OK)
    return true;
  e = sqlite3_errmsg(db);
  return false;
}
} // namespace
struct GestureTemplateRepository::Impl {
  explicit Impl(std::string value) : path(std::move(value)) {}
  std::string path;
  sqlite3 *db{};
  ~Impl() {
    if (db)
      sqlite3_close(db);
  }
};
GestureTemplateRepository::GestureTemplateRepository(std::string path)
    : impl_(std::make_unique<Impl>(std::move(path))) {}
GestureTemplateRepository::~GestureTemplateRepository() = default;
bool GestureTemplateRepository::open(std::string &e) noexcept {
  if (impl_->db)
    return true;
  if (sqlite3_open(impl_->path.c_str(), &impl_->db) != SQLITE_OK) {
    e = sqlite3_errmsg(impl_->db);
    return false;
  }
  Stmt s;
  if (!prep(impl_->db, "PRAGMA user_version", s, e) ||
      sqlite3_step(s.value) != SQLITE_ROW)
    return false;
  const int version = sqlite3_column_int(s.value, 0);
  if (version > kGestureDatabaseSchemaVersion) {
    e = "Gesture database schema is newer than this runtime.";
    return false;
  }
  if (version == 0 &&
      !sql(impl_->db,
           "BEGIN IMMEDIATE; CREATE TABLE definitions(id INTEGER PRIMARY "
           "KEY,name TEXT NOT NULL,enabled INTEGER NOT NULL DEFAULT 1); CREATE "
           "TABLE trials(definition_id INTEGER NOT NULL REFERENCES "
           "definitions(id) ON DELETE CASCADE,trial_index INTEGER NOT "
           "NULL,kind INTEGER NOT NULL,format_version INTEGER NOT NULL,payload BLOB NOT NULL,PRIMARY "
           "KEY(definition_id,trial_index)); CREATE TABLE recordings(definition_id INTEGER NOT NULL "
           "REFERENCES definitions(id) ON DELETE CASCADE,take_index INTEGER NOT NULL,source_id TEXT "
           "NOT NULL UNIQUE,format_version INTEGER NOT NULL,payload BLOB NOT NULL,PRIMARY KEY("
           "definition_id,take_index)); CREATE TABLE bindings(definition_id INTEGER PRIMARY KEY REFERENCES definitions(id) ON DELETE CASCADE,enabled INTEGER NOT NULL DEFAULT 1,action_type TEXT NOT NULL,action_parameter TEXT NOT NULL); PRAGMA user_version=4; COMMIT;",
           e)) {
    sql(impl_->db, "ROLLBACK", e);
    return false;
  }
  if (version == 1 &&
      !sql(impl_->db,
           "BEGIN IMMEDIATE; ALTER TABLE trials ADD COLUMN kind INTEGER NOT "
           "NULL DEFAULT 0; PRAGMA user_version=2; COMMIT;",
           e)) {
    sql(impl_->db, "ROLLBACK", e);
    return false;
  }
  if ((version == 1 || version == 2) &&
      !sql(impl_->db,
           "BEGIN IMMEDIATE; CREATE TABLE recordings(definition_id INTEGER NOT NULL REFERENCES "
           "definitions(id) ON DELETE CASCADE,take_index INTEGER NOT NULL,source_id TEXT NOT NULL "
           "UNIQUE,format_version INTEGER NOT NULL,payload BLOB NOT NULL,PRIMARY KEY(definition_id,"
           "take_index)); PRAGMA user_version=3; COMMIT;",
           e)) {
    sql(impl_->db, "ROLLBACK", e);
    return false;
  }
  if (version >= 1 && version <= 3 &&
      !sql(impl_->db,
           "BEGIN IMMEDIATE; CREATE TABLE IF NOT EXISTS bindings(definition_id INTEGER PRIMARY KEY REFERENCES definitions(id) ON DELETE CASCADE,enabled INTEGER NOT NULL DEFAULT 1,action_type TEXT NOT NULL,action_parameter TEXT NOT NULL); PRAGMA user_version=4; COMMIT;",
           e)) {
    sql(impl_->db, "ROLLBACK", e);
    return false;
  }
  return sql(impl_->db, "PRAGMA foreign_keys=ON", e);
}
bool GestureTemplateRepository::list(
    std::vector<GestureDefinitionMetadata> &out, std::string &e) noexcept {
  out.clear();
  Stmt s;
  if (!prep(impl_->db,
            "SELECT d.id,d.name,d.enabled,COUNT(t.trial_index) FROM "
            "definitions d LEFT JOIN trials t ON t.definition_id=d.id GROUP BY "
            "d.id ORDER BY d.id",
            s, e))
    return false;
  while (sqlite3_step(s.value) == SQLITE_ROW) {
    GestureDefinitionMetadata m;
    m.id = static_cast<std::uint32_t>(sqlite3_column_int64(s.value, 0));
    const auto *t = sqlite3_column_text(s.value, 1);
    m.name = t ? reinterpret_cast<const char *>(t) : "";
    m.enabled = sqlite3_column_int(s.value, 2) != 0;
    m.takeCount = static_cast<std::uint32_t>(sqlite3_column_int(s.value, 3));
    out.push_back(std::move(m));
  }
  return true;
}

bool GestureTemplateRepository::listBindings(
    std::vector<GestureBindingMetadata>& out, std::string& e) noexcept {
  out.clear(); Stmt s;
  if (!prep(impl_->db, "SELECT definition_id,enabled,action_type,action_parameter FROM bindings ORDER BY definition_id", s, e)) return false;
  while (sqlite3_step(s.value) == SQLITE_ROW) {
    GestureBindingMetadata value{};
    value.definitionId = static_cast<std::uint32_t>(sqlite3_column_int64(s.value, 0));
    value.enabled = sqlite3_column_int(s.value, 1) != 0;
    const auto* type = sqlite3_column_text(s.value, 2);
    const auto* parameter = sqlite3_column_text(s.value, 3);
    value.actionType = type ? reinterpret_cast<const char*>(type) : "";
    value.actionParameter = parameter ? reinterpret_cast<const char*>(parameter) : "";
    out.push_back(std::move(value));
  }
  return true;
}

bool GestureTemplateRepository::upsertBinding(
    const GestureBindingMetadata& value, std::string& e) noexcept {
  if (value.actionType.empty() || value.actionType.size() > 31
      || value.actionParameter.size() > 255) { e = "Gesture binding fields exceed ABI limits."; return false; }
  Stmt s;
  if (!prep(impl_->db, "INSERT INTO bindings(definition_id,enabled,action_type,action_parameter) VALUES(?,?,?,?) ON CONFLICT(definition_id) DO UPDATE SET enabled=excluded.enabled,action_type=excluded.action_type,action_parameter=excluded.action_parameter", s, e)) return false;
  sqlite3_bind_int64(s.value, 1, value.definitionId);
  sqlite3_bind_int(s.value, 2, value.enabled ? 1 : 0);
  sqlite3_bind_text(s.value, 3, value.actionType.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(s.value, 4, value.actionParameter.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(s.value) == SQLITE_DONE) return true;
  e = sqlite3_errmsg(impl_->db); return false;
}

bool GestureTemplateRepository::removeBinding(
    const std::uint32_t definitionId, std::string& e) noexcept {
  Stmt s; if (!prep(impl_->db, "DELETE FROM bindings WHERE definition_id=?", s, e)) return false;
  sqlite3_bind_int64(s.value, 1, definitionId);
  if (sqlite3_step(s.value) == SQLITE_DONE) return true;
  e = sqlite3_errmsg(impl_->db); return false;
}
bool GestureTemplateRepository::load(
    GestureTemplateRegistry &r, std::vector<GestureDefinitionMetadata> &meta,
    std::string &e) noexcept {
  if (!list(meta, e))
    return false;
  GestureTemplateRegistry fresh;
  for (const auto &m : meta) {
    if (!m.enabled)
      continue;
    Stmt s;
    if (!prep(impl_->db,
              "SELECT kind,format_version,payload FROM trials WHERE definition_id=? ORDER BY "
              "trial_index",
              s, e))
      return false;
    sqlite3_bind_int64(s.value, 1, m.id);
    std::vector<GestureTrial> values;
    while (sqlite3_step(s.value) == SQLITE_ROW) {
      const int kind = sqlite3_column_int(s.value, 0);
      if ((kind != static_cast<int>(GestureTrialKind::OneHandDynamic) &&
           kind != static_cast<int>(GestureTrialKind::TwoHandDynamic)) ||
          sqlite3_column_int(s.value, 1) != static_cast<int>(kGestureTemplateFormatVersion)) {
        e = "Unsupported gesture trial kind or payload format.";
        return false;
      }
      const auto *d =
          static_cast<const std::uint8_t *>(sqlite3_column_blob(s.value, 2));
      const auto n = static_cast<std::size_t>(sqlite3_column_bytes(s.value, 2));
      GestureTrial trial;
      trial.kind = static_cast<GestureTrialKind>(kind);
      if (trial.kind == GestureTrialKind::OneHandDynamic) {
        UnifiedSequenceTemplate value;
        if (!deserializeGestureTemplate(d, n, value)) {
          e = "Invalid versioned gesture payload.";
          return false;
        }
        trial.payload = std::move(value);
      } else {
        TwoHandTemplate value;
        if (!deserializeTwoHandTemplate(d, n, value)) {
          e = "Invalid versioned gesture payload.";
          return false;
        }
        trial.payload = std::move(value);
      }
      values.push_back(std::move(trial));
    }
    if (!values.empty() && !fresh.restoreTrials(m.id, values)) {
      e = "Registry capacity exceeded.";
      return false;
    }
  }
  r.clear();
  for (const auto &x : fresh.records())
    if (x.active) {
      std::vector<GestureTrial> values(
          x.trials.begin(), x.trials.begin() + x.trialCount);
      if (!r.restoreTrials(x.templateId, values))
        return false;
    }
  return true;
}
bool GestureTemplateRepository::appendThreeTakeSet(
    GestureTemplateRegistry &r, std::uint32_t id, const std::string &name,
    const std::array<UnifiedSequenceTemplate, kRequiredGestureTemplateCount>
        &takes,
    std::string &e) noexcept {
  std::array<GestureTrial, kRequiredGestureTemplateCount> trials{};
  for (std::size_t i = 0; i < takes.size(); ++i) {
    trials[i].kind = GestureTrialKind::OneHandDynamic;
    trials[i].payload = takes[i];
  }
  return appendThreeTakeSet(r, id, name, trials, e);
}
bool GestureTemplateRepository::appendThreeTakeSet(
    GestureTemplateRegistry &r, std::uint32_t id, const std::string &name,
    const std::array<GestureTrial, kRequiredGestureTemplateCount> &takes,
    std::string &e) noexcept {
  const std::array<GestureRecordingProvenance, kRequiredGestureTemplateCount> empty{};
  return appendThreeTakeSet(r, id, name, takes, empty, e);
}
bool GestureTemplateRepository::appendThreeTakeSet(
    GestureTemplateRegistry &r, std::uint32_t id, const std::string &name,
    const std::array<GestureTrial, kRequiredGestureTemplateCount> &takes,
    const std::array<GestureRecordingProvenance, kRequiredGestureTemplateCount> &recordings,
    std::string &e) noexcept {
  if (!r.canAppendTemplateSet(id)) {
    e = "Registry capacity exceeded.";
    return false;
  }
  const auto kind = takes.front().kind;
  for (const auto &take : takes) {
    if (take.kind != kind) {
      e = "A three-take set must use one gesture kind.";
      return false;
    }
  }
  for (const auto &recording : recordings) {
    if (recording.frames.size() > kMaxGestureRecordingFrames) {
      e = "Gesture recording exceeds the native frame capacity.";
      return false;
    }
  }
  const auto *old = r.find(id);
  if (old && old->trialCount != 0 && old->trials[0].kind != kind) {
    e = "Cannot mix gesture kinds in one definition.";
    return false;
  }
  const std::size_t start = old ? old->trialCount : 0;
  std::array<UnifiedSequenceTemplate, kRequiredGestureTemplateCount> oneHandValues{};
  std::array<TwoHandTemplate, kRequiredGestureTemplateCount> twoHandValues{};
  if (!sql(impl_->db, "BEGIN IMMEDIATE", e))
    return false;
  Stmt d;
  if (!prep(impl_->db,
            "INSERT INTO definitions(id,name,enabled) VALUES(?,?,1) ON "
            "CONFLICT(id) DO UPDATE SET name=excluded.name",
            d, e))
    goto fail;
  sqlite3_bind_int64(d.value, 1, id);
  sqlite3_bind_text(d.value, 2, name.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(d.value) != SQLITE_DONE)
    goto fail;
  for (std::size_t i = 0; i < takes.size(); ++i) {
    std::vector<std::uint8_t> blob;
    if (takes[i].kind == GestureTrialKind::OneHandDynamic) {
      const auto *value = std::get_if<UnifiedSequenceTemplate>(&takes[i].payload);
      if (!value) { e = "Gesture trial kind does not match its payload."; goto fail; }
      blob = serializeGestureTemplate(*value);
    } else if (takes[i].kind == GestureTrialKind::TwoHandDynamic) {
      const auto *value = std::get_if<TwoHandTemplate>(&takes[i].payload);
      if (!value) { e = "Gesture trial kind does not match its payload."; goto fail; }
      blob = serializeTwoHandTemplate(*value);
    } else { e = "Unsupported gesture trial kind."; goto fail; }
    Stmt s;
    if (!prep(impl_->db, "INSERT INTO trials VALUES(?,?,?,?,?)", s, e))
      goto fail;
    sqlite3_bind_int64(s.value, 1, id);
    sqlite3_bind_int64(s.value, 2, start + i);
    sqlite3_bind_int(s.value, 3, static_cast<int>(takes[i].kind));
    sqlite3_bind_int(s.value, 4, kGestureTemplateFormatVersion);
    sqlite3_bind_blob(s.value, 5, blob.data(), static_cast<int>(blob.size()),
                      SQLITE_TRANSIENT);
    if (sqlite3_step(s.value) != SQLITE_DONE)
      goto fail;
  }
  for (std::size_t i = 0; i < recordings.size(); ++i) {
    if (recordings[i].sourceId.empty()) continue;
    const auto blob = serializeGestureRecording(recordings[i]);
    Stmt s;
    if (!prep(impl_->db, "INSERT INTO recordings VALUES(?,?,?,?,?)", s, e)) goto fail;
    sqlite3_bind_int64(s.value, 1, id);
    sqlite3_bind_int64(s.value, 2, start + i);
    sqlite3_bind_text(s.value, 3, recordings[i].sourceId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(s.value, 4, kGestureTemplateFormatVersion);
    sqlite3_bind_blob(s.value, 5, blob.data(), static_cast<int>(blob.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(s.value) != SQLITE_DONE) goto fail;
  }
  if (!sql(impl_->db, "COMMIT", e))
    goto fail;
  if (kind == GestureTrialKind::OneHandDynamic) {
    for (std::size_t i = 0; i < takes.size(); ++i)
      oneHandValues[i] = std::get<UnifiedSequenceTemplate>(takes[i].payload);
    return r.appendTemplateSet(id, oneHandValues);
  }
  for (std::size_t i = 0; i < takes.size(); ++i)
    twoHandValues[i] = std::get<TwoHandTemplate>(takes[i].payload);
  return r.appendTwoHandTemplateSet(id, twoHandValues);
fail:
  e = sqlite3_errmsg(impl_->db);
  sql(impl_->db, "ROLLBACK", e);
  return false;
}
bool GestureTemplateRepository::listRecordings(
    const std::uint32_t id, std::vector<GestureRecordingProvenance> &out,
    std::string &e) noexcept {
  out.clear();
  Stmt s;
  if (!prep(impl_->db, "SELECT format_version,payload FROM recordings WHERE definition_id=? "
                       "ORDER BY take_index", s, e)) return false;
  sqlite3_bind_int64(s.value, 1, id);
  while (sqlite3_step(s.value) == SQLITE_ROW) {
    if (sqlite3_column_int(s.value, 0) != static_cast<int>(kGestureTemplateFormatVersion)) {
      e = "Unsupported recording provenance format."; return false;
    }
    GestureRecordingProvenance value;
    const auto *data = static_cast<const std::uint8_t *>(sqlite3_column_blob(s.value, 1));
    const auto size = static_cast<std::size_t>(sqlite3_column_bytes(s.value, 1));
    if (!deserializeGestureRecording(data, size, value)) {
      e = "Invalid recording provenance payload."; return false;
    }
    out.push_back(std::move(value));
  }
  return true;
}
bool GestureTemplateRepository::exportRecording(
    const std::uint32_t id, const std::uint32_t takeIndex,
    const std::string &path, std::string &e) noexcept {
  std::vector<GestureRecordingProvenance> recordings;
  if (!listRecordings(id, recordings, e)) return false;
  const auto found = std::find_if(recordings.begin(), recordings.end(),
      [takeIndex](const auto &value) { return value.takeIndex == takeIndex; });
  if (found == recordings.end()) { e = "Recording take was not found."; return false; }
  const auto blob = serializeGestureRecording(*found);
    // The public ABI path is UTF-8. Convert it to a native Windows path before
    // opening so exports work outside the active ANSI code page as well.
    const std::u8string utf8Path{
        reinterpret_cast<const char8_t *>(path.data()),
        reinterpret_cast<const char8_t *>(path.data() + path.size())};
    std::ofstream stream(std::filesystem::path{utf8Path}, std::ios::binary | std::ios::trunc);
  if (!stream) { e = "Could not open recording export path."; return false; }
  stream.write(reinterpret_cast<const char *>(blob.data()), static_cast<std::streamsize>(blob.size()));
  if (!stream) { e = "Could not write recording export."; return false; }
  return true;
}
bool GestureTemplateRepository::updateMetadata(std::uint32_t id,
                                               const std::string &name,
                                               bool enabled,
                                               std::string &e) noexcept {
  Stmt s;
  if (!prep(impl_->db, "UPDATE definitions SET name=?,enabled=? WHERE id=?", s,
            e))
    return false;
  sqlite3_bind_text(s.value, 1, name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(s.value, 2, enabled);
  sqlite3_bind_int64(s.value, 3, id);
  return sqlite3_step(s.value) == SQLITE_DONE && sqlite3_changes(impl_->db) > 0;
}
bool GestureTemplateRepository::remove(std::uint32_t id,
                                       GestureTemplateRegistry &r,
                                       std::string &e) noexcept {
  Stmt s;
  if (!prep(impl_->db, "DELETE FROM definitions WHERE id=?", s, e))
    return false;
  sqlite3_bind_int64(s.value, 1, id);
  if (sqlite3_step(s.value) != SQLITE_DONE) {
    e = sqlite3_errmsg(impl_->db);
    return false;
  }
  r.removeTemplate(id);
  return true;
}
} // namespace ryoiki::hand_input::recognition
