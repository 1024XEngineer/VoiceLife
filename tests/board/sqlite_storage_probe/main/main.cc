#include <cerrno>
#include <cstdio>
#include <cstring>
#include <unistd.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "sqlite3.h"
#include "wear_levelling.h"

namespace {

constexpr char kTag[] = "SqliteProbe";
constexpr char kDatabasePath[] = "file:/data/probe.db?psow=0";
constexpr char kPhasePath[] = "/data/probe.phase";
constexpr char kStateMagic[] = "voicelife-sqlite-probe-v1";
constexpr char kDataPartitionLabel[] = CONFIG_SQLITE_PROBE_PARTITION_LABEL;
constexpr size_t kImageIdLength = 64;
wl_handle_t g_wear_levelling = WL_INVALID_HANDLE;

[[noreturn]] void Halt(const char* step, sqlite3* db = nullptr, int rc = SQLITE_ERROR) {
    const char* detail = db == nullptr ? sqlite3_errstr(rc) : sqlite3_errmsg(db);
    ESP_LOGE(kTag, "PROBE_RESULT: FAIL step=%s rc=%d detail=%s", step, rc, detail);
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void CheckSqlite(int rc, sqlite3* db, const char* step) {
    if (rc != SQLITE_OK) {
        Halt(step, db, rc);
    }
}

void Exec(sqlite3* db, const char* sql, const char* step) {
    char* error = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &error);
    if (rc != SQLITE_OK) {
        ESP_LOGE(kTag, "%s: %s", step, error == nullptr ? "unknown sqlite error" : error);
        sqlite3_free(error);
        Halt(step, db, rc);
    }
}

int ScalarInt(sqlite3* db, const char* sql, const char* step) {
    sqlite3_stmt* statement = nullptr;
    CheckSqlite(sqlite3_prepare_v2(db, sql, -1, &statement, nullptr), db, step);
    const int rc = sqlite3_step(statement);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(statement);
        Halt(step, db, rc);
    }
    const int value = sqlite3_column_int(statement, 0);
    CheckSqlite(sqlite3_finalize(statement), db, step);
    return value;
}

void CheckScalarText(sqlite3* db, const char* sql, const char* expected, const char* step) {
    sqlite3_stmt* statement = nullptr;
    CheckSqlite(sqlite3_prepare_v2(db, sql, -1, &statement, nullptr), db, step);
    const int rc = sqlite3_step(statement);
    const unsigned char* result = rc == SQLITE_ROW ? sqlite3_column_text(statement, 0) : nullptr;
    if (rc != SQLITE_ROW || result == nullptr || strcmp(reinterpret_cast<const char*>(result), expected) != 0) {
        ESP_LOGE(kTag, "%s: scalar text mismatch expected=%s actual=%s step_rc=%d", step, expected,
                 result == nullptr ? "<null>" : reinterpret_cast<const char*>(result), rc);
        sqlite3_finalize(statement);
        Halt(step, db, rc);
    }
    CheckSqlite(sqlite3_finalize(statement), db, step);
}

void CheckQuickCheck(sqlite3* db, const char* step) {
    CheckScalarText(db, "PRAGMA quick_check", "ok", step);
}

void CheckCommittedState(sqlite3* db, int expected_rows) {
    const int table_rows =
        ScalarInt(db, "SELECT count(id) FROM schedule NOT INDEXED", "count schedule table rows");
    const int index_rows = ScalarInt(db,
                                     "SELECT count(id) FROM schedule "
                                     "INDEXED BY sqlite_autoindex_schedule_1",
                                     "count schedule index rows");
    ESP_LOGI(kTag, "Commit %d integrity: table_rows=%d index_rows=%d", expected_rows, table_rows, index_rows);
    if (table_rows != expected_rows || index_rows != expected_rows) {
        Halt("committed row count mismatch", db);
    }

    char step[64] = {};
    snprintf(step, sizeof(step), "quick check after commit %d", expected_rows);
    CheckQuickCheck(db, step);
}

void CurrentImageId(char (&image_id)[kImageIdLength + 1]) {
    constexpr char kHex[] = "0123456789abcdef";
    const esp_app_desc_t* description = esp_app_get_description();
    for (size_t index = 0; index < sizeof(description->app_elf_sha256); ++index) {
        const unsigned char byte = description->app_elf_sha256[index];
        image_id[index * 2] = kHex[byte >> 4];
        image_id[index * 2 + 1] = kHex[byte & 0x0f];
    }
    image_id[kImageIdLength] = '\0';
}

void RemoveProbeFile(const char* path) {
    if (unlink(path) != 0 && errno != ENOENT) {
        Halt("remove stale probe file", nullptr, errno);
    }
}

void StartFreshProbe() {
    RemoveProbeFile("/data/probe.db-journal");
    RemoveProbeFile("/data/probe.db-wal");
    RemoveProbeFile("/data/probe.db-shm");
    RemoveProbeFile("/data/probe.db");
    RemoveProbeFile(kPhasePath);
}

void WritePhase(int phase) {
    char image_id[kImageIdLength + 1] = {};
    CurrentImageId(image_id);
    FILE* phase_file = fopen(kPhasePath, "w");
    if (phase_file == nullptr) {
        Halt("persist probe phase");
    }
    if (fprintf(phase_file, "%s\n%s\n%d\n", kStateMagic, image_id, phase) < 0 ||
        fflush(phase_file) != 0 ||
        fsync(fileno(phase_file)) != 0 || fclose(phase_file) != 0) {
        Halt("persist probe phase");
    }
}

int PrepareProbeRun() {
    FILE* phase_file = fopen(kPhasePath, "r");
    if (phase_file == nullptr) {
        if (errno != ENOENT) {
            Halt("open probe phase", nullptr, errno);
        }
        StartFreshProbe();
        return 0;
    }

    char magic[sizeof(kStateMagic)] = {};
    if (fscanf(phase_file, "%25s", magic) != 1 || strcmp(magic, kStateMagic) != 0) {
        fclose(phase_file);
        ESP_LOGW(kTag, "Discarding probe state from an older protocol");
        StartFreshProbe();
        return 0;
    }

    char stored_image_id[kImageIdLength + 1] = {};
    int phase = -1;
    const bool parsed = fscanf(phase_file, "%64s%d", stored_image_id, &phase) == 2;
    const bool closed = fclose(phase_file) == 0;
    if (!parsed || !closed) {
        Halt("read probe phase");
    }

    char current_image_id[kImageIdLength + 1] = {};
    CurrentImageId(current_image_id);
    if (strcmp(stored_image_id, current_image_id) != 0) {
        ESP_LOGW(kTag, "Starting a fresh probe for the new firmware image");
        StartFreshProbe();
        return 0;
    }
    if (phase < 0 || phase > 2) {
        Halt("invalid probe phase", nullptr, phase);
    }
    return phase;
}

sqlite3* OpenDatabase() {
    sqlite3_vfs* vfs = sqlite3_vfs_find("unix-none");
    if (vfs == nullptr) {
        for (sqlite3_vfs* current = sqlite3_vfs_find(nullptr); current != nullptr; current = current->pNext) {
            ESP_LOGI(kTag, "available VFS: %s", current->zName);
        }
        Halt("unix-none VFS unavailable");
    }

    sqlite3* db = nullptr;
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI | SQLITE_OPEN_NOMUTEX;
    CheckSqlite(sqlite3_open_v2(kDatabasePath, &db, flags, "unix-none"), db, "open database");
    sqlite3_extended_result_codes(db, 1);
    sqlite3_busy_timeout(db, 0);

    int powersafe_overwrite = -1;
    CheckSqlite(sqlite3_file_control(db, "main", SQLITE_FCNTL_POWERSAFE_OVERWRITE, &powersafe_overwrite), db,
                "query powersafe overwrite");
    ESP_LOGI(kTag, "Opened database vfs=unix-none powersafe_overwrite=%d", powersafe_overwrite);
    return db;
}

void Configure(sqlite3* db) {
    Exec(db, "PRAGMA locking_mode=EXCLUSIVE", "locking mode");
    Exec(db, "PRAGMA page_size=4096", "page size");
    Exec(db, "PRAGMA journal_mode=DELETE", "journal mode");
    Exec(db, "PRAGMA synchronous=EXTRA", "synchronous mode");
    Exec(db, "PRAGMA foreign_keys=ON", "foreign keys");
    Exec(db, "PRAGMA temp_store=MEMORY", "temp store");
    Exec(db, "PRAGMA cache_size=-128", "cache size");
    Exec(db, "PRAGMA mmap_size=0", "mmap size");
    Exec(db, "PRAGMA journal_size_limit=262144", "journal size limit");

    CheckScalarText(db, "PRAGMA locking_mode", "exclusive", "verify locking mode");
    CheckScalarText(db, "PRAGMA journal_mode", "delete", "verify journal mode");
    if (ScalarInt(db, "PRAGMA page_size", "verify page size") != 4096 ||
        ScalarInt(db, "PRAGMA synchronous", "verify synchronous") != 3 ||
        ScalarInt(db, "PRAGMA foreign_keys", "verify foreign keys") != 1 ||
        ScalarInt(db, "PRAGMA temp_store", "verify temp store") != 2 ||
        ScalarInt(db, "PRAGMA mmap_size", "verify mmap size") != 0 ||
        ScalarInt(db, "PRAGMA cache_size", "verify cache size") != -128 ||
        ScalarInt(db, "PRAGMA journal_size_limit", "verify journal size limit") != 262144) {
        Halt("verify sqlite configuration", db);
    }
}

void CreateSchema(sqlite3* db) {
    Exec(db,
         "CREATE TABLE IF NOT EXISTS schedule("
         "id TEXT PRIMARY KEY, title TEXT NOT NULL);"
         "CREATE TABLE IF NOT EXISTS timing_task("
         "id TEXT PRIMARY KEY, schedule_id TEXT NOT NULL REFERENCES schedule(id));"
         "CREATE TABLE IF NOT EXISTS outbox("
         "event_id TEXT PRIMARY KEY, schedule_id TEXT NOT NULL REFERENCES schedule(id));"
         "CREATE TABLE IF NOT EXISTS request_dedup("
         "request_id TEXT PRIMARY KEY, schedule_id TEXT NOT NULL REFERENCES schedule(id));"
         "CREATE TABLE IF NOT EXISTS crash_row("
         "id INTEGER PRIMARY KEY, payload BLOB NOT NULL);"
         "CREATE TABLE IF NOT EXISTS probe_metric("
         "name TEXT PRIMARY KEY, value INTEGER NOT NULL);",
         "create schema");
}

void TestExplicitRollback(sqlite3* db) {
    Exec(db, "BEGIN IMMEDIATE", "rollback begin");
    Exec(db, "INSERT INTO schedule VALUES('rollback-only','must disappear')", "rollback insert");
    Exec(db, "ROLLBACK", "rollback");
    const int table_rows = ScalarInt(db,
                                     "SELECT count(title) FROM schedule NOT INDEXED "
                                     "WHERE title='must disappear'",
                                     "rollback table count");
    const int index_rows = ScalarInt(db,
                                     "SELECT count(id) FROM schedule "
                                     "INDEXED BY sqlite_autoindex_schedule_1 WHERE id='rollback-only'",
                                     "rollback index count");
    ESP_LOGI(kTag, "Explicit rollback integrity: table_rows=%d index_rows=%d", table_rows, index_rows);
    if (table_rows != 0 || index_rows != 0) {
        Halt("explicit rollback leaked row", db);
    }
}

void BenchmarkAtomicWrites(sqlite3* db) {
    sqlite3_stmt* schedule = nullptr;
    sqlite3_stmt* timing = nullptr;
    sqlite3_stmt* outbox = nullptr;
    sqlite3_stmt* dedup = nullptr;
    CheckSqlite(sqlite3_prepare_v2(db, "INSERT INTO schedule VALUES(?1,?2)", -1, &schedule, nullptr), db,
                "prepare schedule");
    CheckSqlite(sqlite3_prepare_v2(db, "INSERT INTO timing_task VALUES(?1,?2)", -1, &timing, nullptr), db,
                "prepare timing");
    CheckSqlite(sqlite3_prepare_v2(db, "INSERT INTO outbox VALUES(?1,?2)", -1, &outbox, nullptr), db,
                "prepare outbox");
    CheckSqlite(sqlite3_prepare_v2(db, "INSERT INTO request_dedup VALUES(?1,?2)", -1, &dedup, nullptr), db,
                "prepare dedup");

    int64_t total_us = 0;
    int64_t maximum_us = 0;
    for (int index = 0; index < 40; ++index) {
        char schedule_id[24] = {};
        char timing_id[24] = {};
        char event_id[24] = {};
        char request_id[24] = {};
        snprintf(schedule_id, sizeof(schedule_id), "schedule-%02d", index);
        snprintf(timing_id, sizeof(timing_id), "timing-%02d", index);
        snprintf(event_id, sizeof(event_id), "event-%02d", index);
        snprintf(request_id, sizeof(request_id), "request-%02d", index);

        const int64_t started = esp_timer_get_time();
        Exec(db, "BEGIN IMMEDIATE", "atomic write begin");
        sqlite3_bind_text(schedule, 1, schedule_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(schedule, 2, "board probe", -1, SQLITE_STATIC);
        sqlite3_bind_text(timing, 1, timing_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(timing, 2, schedule_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(outbox, 1, event_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(outbox, 2, schedule_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(dedup, 1, request_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(dedup, 2, schedule_id, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(schedule) != SQLITE_DONE || sqlite3_step(timing) != SQLITE_DONE ||
            sqlite3_step(outbox) != SQLITE_DONE || sqlite3_step(dedup) != SQLITE_DONE) {
            Halt("atomic write statement", db);
        }
        CheckSqlite(sqlite3_reset(schedule), db, "reset schedule statement");
        CheckSqlite(sqlite3_reset(timing), db, "reset timing statement");
        CheckSqlite(sqlite3_reset(outbox), db, "reset outbox statement");
        CheckSqlite(sqlite3_reset(dedup), db, "reset dedup statement");
        Exec(db, "COMMIT", "atomic write commit");
        const int64_t elapsed = esp_timer_get_time() - started;
        total_us += elapsed;
        if (elapsed > maximum_us) {
            maximum_us = elapsed;
        }
        CheckCommittedState(db, index + 1);
        vTaskDelay(1);
    }

    CheckSqlite(sqlite3_finalize(schedule), db, "finalize schedule statement");
    CheckSqlite(sqlite3_finalize(timing), db, "finalize timing statement");
    CheckSqlite(sqlite3_finalize(outbox), db, "finalize outbox statement");
    CheckSqlite(sqlite3_finalize(dedup), db, "finalize dedup statement");

    char metric_sql[256] = {};
    snprintf(metric_sql, sizeof(metric_sql),
             "INSERT OR REPLACE INTO probe_metric VALUES('commit_avg_us',%lld);"
             "INSERT OR REPLACE INTO probe_metric VALUES('commit_max_us',%lld);",
             static_cast<long long>(total_us / 40), static_cast<long long>(maximum_us));
    Exec(db, metric_sql, "store metrics");

    if (ScalarInt(db, "SELECT count(*) FROM schedule", "schedule count") != 40 ||
        ScalarInt(db, "SELECT count(*) FROM timing_task", "timing count") != 40 ||
        ScalarInt(db, "SELECT count(*) FROM outbox", "outbox count") != 40) {
        Halt("atomic table counts", db);
    }

    const int duplicate_rc = sqlite3_exec(
        db, "INSERT INTO request_dedup VALUES('request-00','schedule-00')", nullptr, nullptr, nullptr);
    if (duplicate_rc != SQLITE_CONSTRAINT_PRIMARYKEY) {
        Halt("idempotency constraint", db, duplicate_rc);
    }
}

void StartCrashRecoveryScenario(sqlite3* db) {
    WritePhase(1);
    Exec(db, "PRAGMA cache_size=8", "small crash cache");
    Exec(db, "BEGIN IMMEDIATE", "crash begin");

    sqlite3_stmt* statement = nullptr;
    CheckSqlite(sqlite3_prepare_v2(db, "INSERT INTO crash_row(payload) VALUES(randomblob(2048))", -1,
                                  &statement, nullptr),
                db, "prepare crash row");
    for (int index = 0; index < 24; ++index) {
        if (sqlite3_step(statement) != SQLITE_DONE) {
            Halt("insert crash row", db);
        }
        CheckSqlite(sqlite3_reset(statement), db, "reset crash statement");
        if ((index + 1) % 4 == 0) {
            vTaskDelay(1);
        }
    }
    CheckSqlite(sqlite3_finalize(statement), db, "finalize crash statement");
    CheckSqlite(sqlite3_db_cacheflush(db), db, "flush dirty pages before reset");
    ESP_LOGW(kTag, "HOST_RESET_POINT: OPEN_TRANSACTION");
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void VerifyCrashRecovery(sqlite3* db) {
    if (ScalarInt(db, "SELECT count(*) FROM crash_row", "crash recovery count") != 0) {
        Halt("uncommitted rows survived restart", db);
    }
    CheckQuickCheck(db, "crash recovery quick check");
    WritePhase(2);
    Exec(db, "BEGIN IMMEDIATE; INSERT INTO crash_row VALUES(1,randomblob(128)); COMMIT;",
         "committed durability marker");
    ESP_LOGW(kTag, "HOST_RESET_POINT: AFTER_COMMIT");
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void VerifyCommittedDurability(sqlite3* db) {
    if (ScalarInt(db, "SELECT count(*) FROM crash_row WHERE id=1", "durable marker") != 1) {
        Halt("committed marker missing", db);
    }
    CheckQuickCheck(db, "final quick check");

    const int average = ScalarInt(db, "SELECT value FROM probe_metric WHERE name='commit_avg_us'", "avg metric");
    const int maximum = ScalarInt(db, "SELECT value FROM probe_metric WHERE name='commit_max_us'", "max metric");
    uint64_t total = 0;
    uint64_t free = 0;
    ESP_ERROR_CHECK(esp_vfs_fat_info("/data", &total, &free));
    ESP_LOGI(kTag,
             "PROBE_RESULT: PASS sqlite=%s vfs=unix-none fs=fatfs-wl journal=DELETE psow=0 sync=EXTRA "
             "reset=host-en "
             "avg_commit_us=%d max_commit_us=%d fs_used=%llu fs_total=%llu free_heap=%u",
             sqlite3_libversion(), average, maximum, static_cast<unsigned long long>(total - free),
             static_cast<unsigned long long>(total),
             static_cast<unsigned>(esp_get_free_heap_size()));
}

}  // namespace

extern "C" void app_main() {
    if (strlen(kDataPartitionLabel) == 0 || strlen(kDataPartitionLabel) > 15) {
        Halt("invalid data partition label");
    }
    const esp_vfs_fat_mount_config_t mount = {
        .format_if_mount_failed = CONFIG_SQLITE_PROBE_ALLOW_FORMAT,
        .max_files = 6,
        .allocation_unit_size = 4096,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };
    ESP_ERROR_CHECK(
        esp_vfs_fat_spiflash_mount_rw_wl("/data", kDataPartitionLabel, &mount, &g_wear_levelling));

    CheckSqlite(sqlite3_initialize(), nullptr, "sqlite initialize");
    if (sqlite3_threadsafe() != 0 || sqlite3_vfs_find("unix-none") == nullptr) {
        Halt("verify sqlite build");
    }
    const int phase = PrepareProbeRun();
    sqlite3* db = OpenDatabase();
    Configure(db);
    CreateSchema(db);

    char image_id[kImageIdLength + 1] = {};
    CurrentImageId(image_id);
    ESP_LOGI(kTag, "PROBE_PHASE: phase=%d image=%.16s", phase, image_id);
    ESP_LOGI(kTag, "Starting phase %d with SQLite %s", phase, sqlite3_libversion());
    if (phase == 0) {
        TestExplicitRollback(db);
        BenchmarkAtomicWrites(db);
        CheckQuickCheck(db, "pre-crash quick check");
        CheckSqlite(sqlite3_close(db), nullptr, "close before crash reopen");
        db = OpenDatabase();
        Configure(db);
        CheckQuickCheck(db, "reopened pre-crash quick check");
        StartCrashRecoveryScenario(db);
    } else if (phase == 1) {
        VerifyCrashRecovery(db);
    } else if (phase == 2) {
        VerifyCommittedDurability(db);
    } else {
        Halt("invalid phase", db, phase);
    }

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
