#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "mapping/operation_row_mapper.h"
#include "mapping/schedule_row_mapper.h"
#include "support/test_support.h"
#include "voicelife/schedule/schedule_types.h"
#include "voicelife/storage_sqlite/sqlite_database.h"
#include "voicelife/storage_sqlite/sqlite_schedule_repository.h"

using voicelife::ErrorCode;
using voicelife::schedule::DateTime;
using voicelife::schedule::OperationRecord;
using voicelife::schedule::QueryScheduleCommand;
using voicelife::schedule::Schedule;
using voicelife::schedule::ScheduleId;
using voicelife::schedule::ScheduleOperationType;
using voicelife::schedule::ScheduleStatus;
using voicelife::schedule::ScheduleStatusFilter;
using voicelife::storage_sqlite::SqliteDatabase;
using voicelife::storage_sqlite::SqliteScheduleRepository;
using voicelife::storage_sqlite::SqliteStep;
using voicelife::test::Check;

namespace mapping = voicelife::storage_sqlite::mapping;

namespace {

/** @brief 管理 Repository 单元测试使用的临时数据库文件。 */
struct TemporaryDatabaseFile {
    std::filesystem::path path;

    /** @brief 删除数据库及其日志文件。 @return 无。 */
    ~TemporaryDatabaseFile() {
        std::error_code error;
        std::filesystem::remove(path, error);
        std::filesystem::remove(path.string() + "-journal", error);
        std::filesystem::remove(path.string() + "-wal", error);
        std::filesystem::remove(path.string() + "-shm", error);
    }
};

/** @brief 创建唯一临时数据库路径。 @return 尚不存在的 SQLite 文件路径。 */
TemporaryDatabaseFile MakeTemporaryDatabaseFile() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    return {.path = std::filesystem::temp_directory_path() /
                    ("voicelife-repository-unit-" + std::to_string(suffix) + ".db")};
}

/** @brief 将测试 Unix 秒转换为日程时间。 @param seconds Unix 秒。 @return 日程时间。 */
DateTime At(int64_t seconds) { return DateTime{std::chrono::seconds{seconds}}; }

/**
 * @brief 创建包含全部可选字段的日程。
 * @return 完整测试日程。
 */
Schedule CompleteSchedule() {
    return {
        .id = 55,
        .event = "完整字段日程",
        .start_time = At(2'100'000'000),
        .end_time = At(2'100'003'600),
        .location = "会议室 C",
        .notes = "完整字段往返",
        .rule_id = 88,
        .status = ScheduleStatus::kCancelled,
        .created_at = At(2'000'000'000),
        .updated_at = At(2'000'000'100),
    };
}

/**
 * @brief 验证数据库未打开时 Repository 返回不可用。
 * @param path 临时数据库路径。
 * @return 无。
 */
void CheckUnavailableRepository(const std::filesystem::path& path) {
    SqliteDatabase database(path.string());
    SqliteScheduleRepository repository(database);
    Check(repository.Initialize().code == ErrorCode::kUnavailable, "未打开数据库不能初始化 Repository");
    Check(repository.Insert(CompleteSchedule()).status.code == ErrorCode::kUnavailable, "未打开数据库不能写入日程");
    Check(repository.FindAll().status.code == ErrorCode::kUnavailable, "未打开数据库不能查询日程");
    Check(repository.Find(QueryScheduleCommand{}).status.code == ErrorCode::kUnavailable, "未打开数据库不能条件查询日程");
    Check(repository.Count(QueryScheduleCommand{}).status.code == ErrorCode::kUnavailable, "未打开数据库不能统计日程");
    Check(repository.FindOverlapping(At(2'100'000'000), At(2'100'003'600), std::nullopt).status.code ==
              ErrorCode::kUnavailable,
          "未打开数据库不能查询重叠日程");
    Check(repository.FindRecentOperations(At(2'100'000'000)).status.code == ErrorCode::kUnavailable,
          "未打开数据库不能查询近期操作");
    Check(repository.InsertOperation(OperationRecord{}).status.code == ErrorCode::kUnavailable,
          "未打开数据库不能写入操作记录");
    Check(repository.UndoOperation(1, At(2'100'000'000)).status.code == ErrorCode::kUnavailable,
          "未打开数据库不能撤销操作");
}

/**
 * @brief 验证空标题、默认时间和所有字段往返。
 * @param path 临时数据库路径。
 * @return 无。
 */
void CheckInsertAndRoundTrip(const std::filesystem::path& path) {
    SqliteDatabase database(path.string());
    Check(database.Open().ok(), "Repository 测试应打开数据库");
    SqliteScheduleRepository repository(database);
    Check(repository.Initialize().ok() && repository.Initialize().ok(), "Repository 初始化应保持幂等");
    Check(repository.Insert(Schedule{}).status.code == ErrorCode::kInvalidArgument, "空标题日程应被拒绝");

    const auto minimal = repository.Insert(Schedule{
        .id = 999,
        .event = "最小日程",
        .start_time = std::nullopt,
        .end_time = std::nullopt,
        .location = std::nullopt,
        .notes = std::nullopt,
        .rule_id = std::nullopt,
        .status = ScheduleStatus::kActive,
        .created_at = {},
        .updated_at = {},
    });
    Check(minimal.ok() && minimal.value->id > 0 && minimal.value->id != 999, "Repository 应生成标识并忽略调用方标识");
    Check(minimal.value->created_at != DateTime{} && minimal.value->updated_at == minimal.value->created_at,
          "Repository 应为最小日程补齐时间戳");

    const Schedule complete_input = CompleteSchedule();
    const auto complete = repository.Insert(complete_input);
    Check(complete.ok() && complete.value->created_at == complete_input.created_at &&
              complete.value->updated_at == complete_input.updated_at,
          "Repository 应保留调用方提供的时间戳");

    const auto stored = repository.FindAll();
    Check(stored.ok() && stored.value->size() == 2, "Repository 应读取两条日程");
    const Schedule& complete_row = stored.value->front();
    Check(complete_row.event == complete_input.event && complete_row.start_time == complete_input.start_time &&
              complete_row.end_time == complete_input.end_time && complete_row.location == complete_input.location &&
              complete_row.notes == complete_input.notes && complete_row.rule_id == complete_input.rule_id &&
              complete_row.status == complete_input.status && complete_row.created_at == complete_input.created_at &&
              complete_row.updated_at == complete_input.updated_at,
          "完整日程的所有字段都应往返一致");
    const Schedule& minimal_row = stored.value->back();
    Check(!minimal_row.start_time.has_value() && !minimal_row.end_time.has_value() &&
              !minimal_row.location.has_value() && !minimal_row.notes.has_value() && !minimal_row.rule_id.has_value(),
          "最小日程的可空字段应保持为空");
}

/**
 * @brief 验证 Mapper 拒绝非法状态和空标题结果行。
 * @param path 临时数据库路径。
 * @return 无。
 */
void CheckMapperValidation(const std::filesystem::path& path) {
    SqliteDatabase database(path.string());
    Check(database.Open().ok(), "Mapper 测试应打开数据库");

    auto invalid_status = database.Prepare("SELECT 1, '日程', NULL, NULL, NULL, NULL, NULL, 99, 100, 100");
    Check(invalid_status.ok() && invalid_status.value->Step().ok(), "应构造非法状态结果行");
    Check(mapping::ReadSchedule(*invalid_status.value).status.code == ErrorCode::kInternal,
          "Mapper 应拒绝非法日程状态");

    auto null_event = database.Prepare("SELECT 1, NULL, NULL, NULL, NULL, NULL, NULL, 1, 100, 100");
    Check(null_event.ok() && null_event.value->Step().ok(), "应构造空标题结果行");
    Check(mapping::ReadSchedule(*null_event.value).status.code == ErrorCode::kInternal, "Mapper 应拒绝空标题结果行");

    auto no_parameters = database.Prepare("SELECT 1");
    Check(no_parameters.ok(), "应创建无参数语句");
    const auto event_error = mapping::BindSchedule(*no_parameters.value, CompleteSchedule());
    Check(event_error.code == ErrorCode::kInternal && event_error.message.find("event") != std::string::npos,
          "Mapper 应为标题绑定错误补充字段名");

    auto one_parameter = database.Prepare("SELECT ?");
    Check(one_parameter.ok(), "应创建单参数语句");
    const auto start_error = mapping::BindSchedule(*one_parameter.value, CompleteSchedule());
    Check(start_error.code == ErrorCode::kInternal && start_error.message.find("start_time") != std::string::npos,
          "Mapper 应为开始时间绑定错误补充字段名");
}

/**
 * @brief 验证操作记录 Mapper 的绑定错误和非法结果行拒绝分支。
 * @param path 临时数据库路径。
 * @return 无。
 */
void CheckOperationMapperValidation(const std::filesystem::path& path) {
    SqliteDatabase database(path.string());
    Check(database.Open().ok(), "操作 Mapper 测试应打开数据库");

    auto no_parameters = database.Prepare("SELECT 1");
    Check(no_parameters.ok(), "操作 Mapper 应创建无参数语句");
    OperationRecord sample;
    sample.type = ScheduleOperationType::kCreate;
    sample.schedule_id = 100;
    sample.schedule_event = "创建";
    const auto type_bind = mapping::BindOperation(*no_parameters.value, sample);
    Check(type_bind.code == ErrorCode::kInternal && type_bind.message.find("type") != std::string::npos,
          "操作 Mapper 应为 type 绑定错误补充字段名");

    auto invalid_type = database.Prepare(
        "SELECT 1, 99, 100, '创建', 2000000000, 1, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL");
    Check(invalid_type.ok() && invalid_type.value->Step().ok(), "应构造非法操作类型结果行");
    Check(mapping::ReadOperation(*invalid_type.value).status.code == ErrorCode::kInternal,
          "操作 Mapper 应拒绝非法操作类型");

    auto null_field = database.Prepare(
        "SELECT 1, 1, 100, NULL, 2000000000, 1, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL");
    Check(null_field.ok() && null_field.value->Step().ok(), "应构造空字段结果行");
    Check(mapping::ReadOperation(*null_field.value).status.code == ErrorCode::kInternal, "操作 Mapper 应拒绝空字段");

    auto invalid_active = database.Prepare(
        "SELECT 1, 1, 100, '创建', 2000000000, 2, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL");
    Check(invalid_active.ok() && invalid_active.value->Step().ok(), "应构造非法 active 结果行");
    Check(mapping::ReadOperation(*invalid_active.value).status.code == ErrorCode::kInternal,
          "操作 Mapper 应拒绝非法 active");

    auto inconsistent = database.Prepare(
        "SELECT 1, 1, 100, '修改', 2000000000, 1, NULL, 'x', NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL");
    Check(inconsistent.ok() && inconsistent.value->Step().ok(), "应构造快照列不一致结果行");
    Check(mapping::ReadOperation(*inconsistent.value).status.code == ErrorCode::kInternal,
          "操作 Mapper 应拒绝不一致的快照列");

    auto incomplete = database.Prepare(
        "SELECT 1, 1, 100, '修改', 2000000000, 1, 100, NULL, 2100000000, 2100003600, NULL, NULL, NULL, 1, "
        "2000000000, 2000000100");
    Check(incomplete.ok() && incomplete.value->Step().ok(), "应构造快照字段不完整结果行");
    Check(mapping::ReadOperation(*incomplete.value).status.code == ErrorCode::kInternal,
          "操作 Mapper 应拒绝不完整的快照");

    auto bad_status = database.Prepare(
        "SELECT 1, 1, 100, '修改', 2000000000, 1, 100, 'x', 2100000000, 2100003600, NULL, NULL, NULL, 99, "
        "2000000000, 2000000100");
    Check(bad_status.ok() && bad_status.value->Step().ok(), "应构造非法快照状态结果行");
    Check(mapping::ReadOperation(*bad_status.value).status.code == ErrorCode::kInternal,
          "操作 Mapper 应拒绝非法快照状态");

    auto id_mismatch = database.Prepare(
        "SELECT 1, 1, 100, '修改', 2000000000, 1, 999, 'x', 2100000000, 2100003600, NULL, NULL, NULL, 1, "
        "2000000000, 2000000100");
    Check(id_mismatch.ok() && id_mismatch.value->Step().ok(), "应构造快照 ID 不一致结果行");
    Check(mapping::ReadOperation(*id_mismatch.value).status.code == ErrorCode::kInternal,
          "操作 Mapper 应拒绝快照 ID 不一致");

    auto bad_range = database.Prepare(
        "SELECT 1, 1, 100, '修改', 2000000000, 1, 100, 'x', NULL, 2100003600, NULL, NULL, NULL, 1, 2000000000, "
        "2000000100");
    Check(bad_range.ok() && bad_range.value->Step().ok(), "应构造快照时间范围无效结果行");
    Check(mapping::ReadOperation(*bad_range.value).status.code == ErrorCode::kInternal,
          "操作 Mapper 应拒绝无效快照时间范围");

    auto full_snapshot = database.Prepare(
        "SELECT 1, 1, 100, '修改', 2000000000, 1, 100, 'x', 2100000000, 2100003600, '会议室', '复盘', NULL, 1, "
        "2000000000, 2000000100");
    Check(full_snapshot.ok() && full_snapshot.value->Step().ok(), "应构造完整快照结果行");
    const auto full_operation = mapping::ReadOperation(*full_snapshot.value);
    Check(full_operation.ok() && full_operation.value->previous.has_value() &&
              full_operation.value->previous->location == "会议室" && full_operation.value->previous->notes == "复盘",
          "操作 Mapper 应还原含地点和备注的完整快照");
}

/**
 * @brief 验证 Repository 会传播 SQL 编译、执行和行映射错误。
 * @param path 临时数据库路径。
 * @return 无。
 */
void CheckRepositoryErrorPropagation(const std::filesystem::path& path) {
    SqliteDatabase database(path.string());
    Check(database.Open().ok(), "错误传播测试应打开数据库");
    SqliteScheduleRepository repository(database);
    Check(repository.Initialize().ok(), "错误传播测试应初始化表");

    Check(database
              .Execute("CREATE TRIGGER reject_schedule BEFORE INSERT ON schedule "
                       "BEGIN SELECT RAISE(ABORT, 'blocked'); END")
              .ok(),
          "应成功创建拒绝写入触发器");
    Check(repository.Insert(CompleteSchedule()).status.code == ErrorCode::kAlreadyExists,
          "Repository 应传播 Statement 执行错误");
    Check(database.Execute("DROP TRIGGER reject_schedule").ok(), "应删除拒绝写入触发器");

    Check(database.Execute("DROP TABLE schedule").ok(), "应删除日程表以制造 SQL 编译错误");
    Check(repository.Insert(CompleteSchedule()).status.code == ErrorCode::kInternal,
          "Repository 应传播写入 SQL 编译错误");
    Check(repository.FindAll().status.code == ErrorCode::kInternal, "Repository 应传播查询 SQL 编译错误");
}

/** @brief 返回当前秒级系统时间。 @return 当前日程时间。 */
DateTime CurrentTime() {
    return std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now());
}

/** @brief 构造仅含事件名的日程。 @param event 日程名称。 @return 最小日程。 */
Schedule MinimalSchedule(const std::string& event) {
    Schedule schedule;
    schedule.event = event;
    return schedule;
}

/**
 * @brief 验证操作记录写入、查询与原子撤销的完整链路。
 * @param path 临时数据库路径。
 * @return 无。
 */
void CheckOperationRepository(const std::filesystem::path& path) {
    SqliteDatabase database(path.string());
    Check(database.Open().ok(), "操作仓储测试应打开数据库");
    SqliteScheduleRepository repository(database);
    Check(repository.Initialize().ok(), "操作仓储测试应初始化表结构");

    Schedule base = MinimalSchedule("操作目标日程");
    base.start_time = At(2'100'000'000);
    base.end_time = At(2'100'003'600);
    base.created_at = At(2'000'000'000);
    base.updated_at = At(2'000'000'100);
    const auto target = repository.Insert(base);
    Check(target.ok() && target.value->id > 0, "应创建操作目标日程");
    const ScheduleId sid = target.value->id;

    // InsertOperation 校验分支。
    OperationRecord empty_event;
    empty_event.type = ScheduleOperationType::kCreate;
    empty_event.schedule_id = sid;
    empty_event.schedule_event = "";
    Check(repository.InsertOperation(empty_event).status.code == ErrorCode::kInvalidArgument, "空操作事件名应被拒绝");

    OperationRecord bad_type;
    bad_type.type = static_cast<ScheduleOperationType>(99);
    bad_type.schedule_id = sid;
    bad_type.schedule_event = "非法类型";
    Check(repository.InsertOperation(bad_type).status.code == ErrorCode::kInvalidArgument, "非法操作类型应被拒绝");

    OperationRecord create_with_previous;
    create_with_previous.type = ScheduleOperationType::kCreate;
    create_with_previous.schedule_id = sid;
    create_with_previous.schedule_event = "创建带快照";
    create_with_previous.previous = *target.value;
    Check(repository.InsertOperation(create_with_previous).status.code == ErrorCode::kInvalidArgument,
          "创建操作带快照应被拒绝");

    OperationRecord update_without_previous;
    update_without_previous.type = ScheduleOperationType::kUpdate;
    update_without_previous.schedule_id = sid;
    update_without_previous.schedule_event = "修改无快照";
    Check(repository.InsertOperation(update_without_previous).status.code == ErrorCode::kInvalidArgument,
          "修改操作缺快照应被拒绝");

    OperationRecord mismatch_previous;
    mismatch_previous.type = ScheduleOperationType::kUpdate;
    mismatch_previous.schedule_id = sid;
    mismatch_previous.schedule_event = "快照不一致";
    mismatch_previous.previous = *target.value;
    mismatch_previous.previous->id = sid + 999;
    Check(repository.InsertOperation(mismatch_previous).status.code == ErrorCode::kInvalidArgument,
          "快照 ID 不一致应被拒绝");

    // 创建 / 修改 / 删除操作的正常写入（覆盖 BindOperation 两种快照分支）。
    OperationRecord create_op;
    create_op.type = ScheduleOperationType::kCreate;
    create_op.schedule_id = sid;
    create_op.schedule_event = "创建操作";
    const auto saved_create = repository.InsertOperation(create_op);
    Check(saved_create.ok() && saved_create.value->id > 0, "应保存创建操作");

    OperationRecord update_op;
    update_op.type = ScheduleOperationType::kUpdate;
    update_op.schedule_id = sid;
    update_op.schedule_event = "修改操作";
    update_op.previous = *target.value;
    const auto saved_update = repository.InsertOperation(update_op);
    Check(saved_update.ok(), "应保存修改操作");

    OperationRecord delete_op;
    delete_op.type = ScheduleOperationType::kDelete;
    delete_op.schedule_id = sid;
    delete_op.schedule_event = "删除操作";
    delete_op.previous = *target.value;
    const auto saved_delete = repository.InsertOperation(delete_op);
    Check(saved_delete.ok(), "应保存删除操作");

    // FindRecentOperations 应返回窗口内全部有效操作。
    const auto recent = repository.FindRecentOperations(CurrentTime());
    Check(recent.ok() && recent.value->size() >= 3, "应查询到窗口内操作");

    // UndoOperation 校验分支。
    Check(repository.UndoOperation(0, CurrentTime()).status.code == ErrorCode::kInvalidArgument, "撤销非法标识应被拒绝");
    Check(repository.UndoOperation(999999, CurrentTime()).status.code == ErrorCode::kNotFound,
          "撤销不存在操作应被拒绝");

    // 撤销修改操作：恢复 previous 快照（RestoreScheduleLocked 更新路径）。
    const auto undo_update = repository.UndoOperation(saved_update.value->id, CurrentTime());
    Check(undo_update.ok() && undo_update.value->schedule.has_value() &&
              undo_update.value->schedule->event == target.value->event,
          "撤销修改应恢复快照");

    // 撤销删除操作：恢复 previous 快照（删除逆操作分支）。
    Check(repository.Delete(sid).ok(), "应先软删除目标日程");
    const auto undo_delete = repository.UndoOperation(saved_delete.value->id, CurrentTime());
    Check(undo_delete.ok() && undo_delete.value->schedule.has_value(), "撤销删除应恢复快照");

    // 撤销创建操作：物理删除日程。
    const auto undo_create = repository.UndoOperation(saved_create.value->id, CurrentTime());
    Check(undo_create.ok() && !undo_create.value->schedule.has_value(), "撤销创建应物理删除日程");
    Check(repository.FindById(sid).status.code == ErrorCode::kNotFound, "撤销创建后日程应不存在");

    // 撤销已被撤销的操作：active=false 分支。
    Check(repository.UndoOperation(saved_create.value->id, CurrentTime()).status.code == ErrorCode::kNotFound,
          "重复撤销应返回未找到");

    // 撤销 undo 操作：恢复（覆盖 BindScheduleWithId 插入路径）。
    const auto after_undo_create = repository.FindRecentOperations(CurrentTime());
    Check(after_undo_create.ok() && !after_undo_create.value->empty() &&
              after_undo_create.value->front().type == ScheduleOperationType::kUndo,
          "撤销创建后应写入 undo 记录");
    const auto undo_of_undo = repository.UndoOperation(after_undo_create.value->front().id, CurrentTime());
    Check(undo_of_undo.ok() && undo_of_undo.value->schedule.has_value() &&
              undo_of_undo.value->schedule->event == target.value->event,
          "撤销 undo 应恢复被删除的日程");

    // 操作时间晚于撤销时间 → 冲突。
    OperationRecord future_op;
    future_op.type = ScheduleOperationType::kCreate;
    future_op.schedule_id = sid;
    future_op.schedule_event = "未来操作";
    const auto saved_future = repository.InsertOperation(future_op);
    const DateTime past = CurrentTime() - std::chrono::seconds{2};
    Check(repository.UndoOperation(saved_future.value->id, past).status.code == ErrorCode::kConflict,
          "操作时间晚于撤销时间应冲突");

    // 操作超出十五分钟撤销窗口 → 冲突。
    OperationRecord window_op;
    window_op.type = ScheduleOperationType::kCreate;
    window_op.schedule_id = sid;
    window_op.schedule_event = "窗口外操作";
    const auto saved_window = repository.InsertOperation(window_op);
    const DateTime too_late = CurrentTime() + std::chrono::minutes{16};
    Check(repository.UndoOperation(saved_window.value->id, too_late).status.code == ErrorCode::kConflict,
          "超过十五分钟撤销期限应冲突");
}

/**
 * @brief 验证重叠查询、计数、非法标识与软删除冲突等查询分支。
 * @param path 临时数据库路径。
 * @return 无。
 */
void CheckQueryBranches(const std::filesystem::path& path) {
    SqliteDatabase database(path.string());
    Check(database.Open().ok(), "查询分支测试应打开数据库");
    SqliteScheduleRepository repository(database);
    Check(repository.Initialize().ok(), "查询分支测试应初始化表结构");

    Schedule a = MinimalSchedule("早间日程");
    a.start_time = At(2'100'000'000);
    a.end_time = At(2'100'003'600);
    Schedule b = MinimalSchedule("重叠日程");
    b.start_time = At(2'100'001'800);
    b.end_time = At(2'100'007'200);
    Schedule c = MinimalSchedule("晚间日程");
    c.start_time = At(2'100'010'800);
    c.end_time = At(2'100'014'400);
    const auto inserted_a = repository.Insert(a);
    const auto inserted_b = repository.Insert(b);
    const auto inserted_c = repository.Insert(c);
    Check(inserted_a.ok() && inserted_b.ok() && inserted_c.ok(), "应创建查询分支日程");

    // FindOverlapping 命中与排除。
    const auto overlap = repository.FindOverlapping(At(2'100'000'000), At(2'100'005'400), std::nullopt);
    Check(overlap.ok() && overlap.value->size() == 2, "重叠查询应命中两条日程");
    const auto overlap_excluded =
        repository.FindOverlapping(At(2'100'000'000), At(2'100'005'400), inserted_a.value->id);
    Check(overlap_excluded.ok() && overlap_excluded.value->size() == 1, "排除标识后应命中一条日程");

    // Count 活跃日程。
    QueryScheduleCommand active_query;
    active_query.status = ScheduleStatusFilter::kActive;
    const auto count = repository.Count(active_query);
    Check(count.ok() && count.value == 3, "活跃日程计数应为三条");

    // Find 关键词 / 规则标识 / 时间范围 / 分页。
    QueryScheduleCommand keyword;
    keyword.keyword = std::string{"早间"};
    const auto by_keyword = repository.Find(keyword);
    Check(by_keyword.ok() && by_keyword.value->size() == 1 && by_keyword.value->front().event == "早间日程",
          "关键词查询应命中");

    QueryScheduleCommand by_rule;
    by_rule.rule_id = int64_t{42};
    Check(repository.Find(by_rule).ok(), "规则标识查询应执行成功");

    QueryScheduleCommand ranged;
    ranged.start_from = At(2'100'000'000);
    ranged.start_to = At(2'100'005'400);
    const auto by_range = repository.Find(ranged);
    Check(by_range.ok() && by_range.value->size() == 2, "时间范围查询应命中两条日程");

    QueryScheduleCommand paged;
    paged.limit = 2;
    paged.offset = 0;
    const auto by_page = repository.Find(paged);
    Check(by_page.ok() && by_page.value->size() == 2, "分页查询应限制条数");

    // 非法标识与软删除冲突。
    Check(repository.FindById(0).status.code == ErrorCode::kInvalidArgument, "非法日程标识应被拒绝");

    Schedule bad_update = MinimalSchedule("非法更新");
    bad_update.id = 0;
    Check(repository.Update(bad_update).code == ErrorCode::kInvalidArgument, "更新无标识应被拒绝");
    bad_update.id = 999999;
    Check(repository.Update(bad_update).code == ErrorCode::kNotFound, "更新不存在应返回未找到");

    Check(repository.Delete(0).code == ErrorCode::kInvalidArgument, "删除非法标识应被拒绝");
    Check(repository.Delete(999999).code == ErrorCode::kNotFound, "删除不存在应返回未找到");
    Check(repository.Delete(inserted_a.value->id).ok(), "首次删除应成功");
    Check(repository.Delete(inserted_a.value->id).code == ErrorCode::kConflict, "重复删除应冲突");
}

/**
 * @brief 验证操作查询与撤销在表缺失或目标失效时透传 SQL 错误并回滚。
 * @return 无。
 */
void CheckOperationFailureBranches() {
    {
        // 删除操作表后：近期操作查询与撤销应透传 SQL 编译错误。
        const TemporaryDatabaseFile file = MakeTemporaryDatabaseFile();
        SqliteDatabase database(file.path.string());
        Check(database.Open().ok(), "操作表失败分支应打开数据库");
        SqliteScheduleRepository repository(database);
        Check(repository.Initialize().ok(), "操作表失败分支应初始化表结构");
        Check(database.Execute("DROP TABLE operation_record").ok(), "应删除操作表制造 SQL 错误");
        Check(repository.FindRecentOperations(CurrentTime()).status.code == ErrorCode::kInternal,
              "FindRecentOperations 应透传操作表缺失错误");
        Check(repository.UndoOperation(1, CurrentTime()).status.code == ErrorCode::kInternal,
              "UndoOperation 应透传操作表缺失错误");
    }
    {
        // 删除日程表后：撤销操作在读取目标日程时失败应回滚。
        const TemporaryDatabaseFile file = MakeTemporaryDatabaseFile();
        SqliteDatabase database(file.path.string());
        Check(database.Open().ok(), "日程表失败分支应打开数据库");
        SqliteScheduleRepository repository(database);
        Check(repository.Initialize().ok(), "日程表失败分支应初始化表结构");
        Schedule base = MinimalSchedule("撤销目标日程");
        base.start_time = At(2'100'000'000);
        base.end_time = At(2'100'003'600);
        const auto target = repository.Insert(base);
        Check(target.ok(), "应创建撤销目标日程");
        OperationRecord op;
        op.type = ScheduleOperationType::kCreate;
        op.schedule_id = target.value->id;
        op.schedule_event = "撤销创建";
        const auto saved = repository.InsertOperation(op);
        Check(saved.ok(), "应保存创建操作");
        Check(database.Execute("DROP TABLE schedule").ok(), "应删除日程表制造 SQL 错误");
        Check(repository.UndoOperation(saved.value->id, CurrentTime()).status.code == ErrorCode::kInternal,
              "撤销操作读取目标日程失败应回滚");
    }
}

}  // namespace

/** @brief 执行 SQLite 日程 Repository 和 Mapper 单元测试。 @return 全部断言通过时返回 0。 */
int main() {
    const TemporaryDatabaseFile unavailable = MakeTemporaryDatabaseFile();
    CheckUnavailableRepository(unavailable.path);
    const TemporaryDatabaseFile round_trip = MakeTemporaryDatabaseFile();
    CheckInsertAndRoundTrip(round_trip.path);
    const TemporaryDatabaseFile mapper = MakeTemporaryDatabaseFile();
    CheckMapperValidation(mapper.path);
    const TemporaryDatabaseFile operation_mapper = MakeTemporaryDatabaseFile();
    CheckOperationMapperValidation(operation_mapper.path);
    const TemporaryDatabaseFile errors = MakeTemporaryDatabaseFile();
    CheckRepositoryErrorPropagation(errors.path);
    const TemporaryDatabaseFile operations = MakeTemporaryDatabaseFile();
    CheckOperationRepository(operations.path);
    const TemporaryDatabaseFile queries = MakeTemporaryDatabaseFile();
    CheckQueryBranches(queries.path);
    CheckOperationFailureBranches();
    return 0;
}
