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
using voicelife::schedule::OperationEntityType;
using voicelife::schedule::OperationRecord;
using voicelife::schedule::QueryOperationCommand;
using voicelife::schedule::QueryScheduleCommand;
using voicelife::schedule::Schedule;
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
    Check(repository.Find(QueryScheduleCommand{}).status.code == ErrorCode::kUnavailable,
          "未打开数据库不能条件查询日程");
    Check(repository.Count(QueryScheduleCommand{}).status.code == ErrorCode::kUnavailable, "未打开数据库不能统计日程");
    Check(repository.FindOverlapping(At(2'100'000'000), At(2'100'003'600), std::nullopt).status.code ==
              ErrorCode::kUnavailable,
          "未打开数据库不能查询重叠日程");
    Check(repository.FindOperations(QueryOperationCommand{}).status.code == ErrorCode::kUnavailable,
          "未打开数据库不能查询操作记录");
    Check(repository.CountOperations(QueryOperationCommand{}).status.code == ErrorCode::kUnavailable,
          "未打开数据库不能统计操作记录");
    Check(repository.InsertOperation(OperationRecord{}).status.code == ErrorCode::kUnavailable,
          "未打开数据库不能写入操作记录");
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
    sample.entity_type = OperationEntityType::kSchedule;
    sample.type = ScheduleOperationType::kCreate;
    sample.entity_id = 100;
    sample.label = "创建";
    const auto entity_bind = mapping::BindOperation(*no_parameters.value, sample);
    Check(entity_bind.code == ErrorCode::kInternal && entity_bind.message.find("entity_type") != std::string::npos,
          "操作 Mapper 应为 entity_type 绑定错误补充字段名");

    auto invalid_entity = database.Prepare("SELECT 1, 99, 1, 100, '创建', 2000000000, NULL");
    Check(invalid_entity.ok() && invalid_entity.value->Step().ok(), "应构造非法实体类型结果行");
    Check(mapping::ReadOperation(*invalid_entity.value).status.code == ErrorCode::kInternal,
          "操作 Mapper 应拒绝非法实体类型");

    auto invalid_type = database.Prepare("SELECT 1, 1, 99, 100, '创建', 2000000000, NULL");
    Check(invalid_type.ok() && invalid_type.value->Step().ok(), "应构造非法操作类型结果行");
    Check(mapping::ReadOperation(*invalid_type.value).status.code == ErrorCode::kInternal,
          "操作 Mapper 应拒绝非法操作类型");

    auto null_id = database.Prepare("SELECT 1, 1, 1, NULL, '创建', 2000000000, NULL");
    Check(null_id.ok() && null_id.value->Step().ok(), "应构造空实体标识结果行");
    Check(mapping::ReadOperation(*null_id.value).status.code == ErrorCode::kInternal,
          "操作 Mapper 应拒绝空实体标识");

    auto null_label = database.Prepare("SELECT 1, 1, 1, 100, NULL, 2000000000, NULL");
    Check(null_label.ok() && null_label.value->Step().ok(), "应构造空名称结果行");
    Check(mapping::ReadOperation(*null_label.value).status.code == ErrorCode::kInternal,
          "操作 Mapper 应拒绝空名称");

    auto create_with_before = database.Prepare("SELECT 1, 1, 1, 100, '创建', 2000000000, '{\"a\":1}'");
    Check(create_with_before.ok() && create_with_before.value->Step().ok(), "应构造创建带快照结果行");
    Check(mapping::ReadOperation(*create_with_before.value).status.code == ErrorCode::kInternal,
          "操作 Mapper 应拒绝创建操作携带 before");

    auto update_without_before = database.Prepare("SELECT 1, 1, 2, 100, '修改', 2000000000, NULL");
    Check(update_without_before.ok() && update_without_before.value->Step().ok(), "应构造修改缺快照结果行");
    Check(mapping::ReadOperation(*update_without_before.value).status.code == ErrorCode::kInternal,
          "操作 Mapper 应拒绝修改操作缺少 before");

    auto full_create = database.Prepare("SELECT 7, 1, 1, 100, '创建', 2000000000, NULL");
    Check(full_create.ok() && full_create.value->Step().ok(), "应构造完整创建结果行");
    const auto create_operation = mapping::ReadOperation(*full_create.value);
    Check(create_operation.ok() && create_operation.value->id == 7 &&
              create_operation.value->entity_type == OperationEntityType::kSchedule &&
              create_operation.value->type == ScheduleOperationType::kCreate &&
              create_operation.value->entity_id == 100 && create_operation.value->label == "创建" &&
              !create_operation.value->before.has_value(),
          "操作 Mapper 应还原完整创建记录");

    auto full_update = database.Prepare("SELECT 8, 2, 2, 101, '修改', 2000000001, '{\"id\":101}'");
    Check(full_update.ok() && full_update.value->Step().ok(), "应构造完整修改结果行");
    const auto update_operation = mapping::ReadOperation(*full_update.value);
    Check(update_operation.ok() && update_operation.value->entity_type == OperationEntityType::kRule &&
              update_operation.value->type == ScheduleOperationType::kUpdate &&
              update_operation.value->entity_id == 101 &&
              update_operation.value->before.has_value() && *update_operation.value->before == "{\"id\":101}",
          "操作 Mapper 应还原 before 快照");
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

    const int64_t schedule_id = 1001;
    const std::string snapshot = R"({"id":1001,"event":"操作目标日程"})";

    // InsertOperation 校验分支。
    OperationRecord empty_label;
    empty_label.entity_type = OperationEntityType::kSchedule;
    empty_label.type = ScheduleOperationType::kCreate;
    empty_label.entity_id = schedule_id;
    empty_label.label = "";
    Check(repository.InsertOperation(empty_label).status.code == ErrorCode::kInvalidArgument, "空操作名称应被拒绝");

    OperationRecord bad_entity;
    bad_entity.entity_type = static_cast<OperationEntityType>(99);
    bad_entity.type = ScheduleOperationType::kCreate;
    bad_entity.entity_id = schedule_id;
    bad_entity.label = "非法实体类型";
    Check(repository.InsertOperation(bad_entity).status.code == ErrorCode::kInvalidArgument, "非法实体类型应被拒绝");

    OperationRecord bad_type;
    bad_type.entity_type = OperationEntityType::kSchedule;
    bad_type.type = static_cast<ScheduleOperationType>(99);
    bad_type.entity_id = schedule_id;
    bad_type.label = "非法类型";
    Check(repository.InsertOperation(bad_type).status.code == ErrorCode::kInvalidArgument, "非法操作类型应被拒绝");

    OperationRecord bad_id;
    bad_id.entity_type = OperationEntityType::kSchedule;
    bad_id.type = ScheduleOperationType::kCreate;
    bad_id.entity_id = 0;
    bad_id.label = "非法 ID";
    Check(repository.InsertOperation(bad_id).status.code == ErrorCode::kInvalidArgument, "非正数实体 ID 应被拒绝");

    OperationRecord create_with_before;
    create_with_before.entity_type = OperationEntityType::kSchedule;
    create_with_before.type = ScheduleOperationType::kCreate;
    create_with_before.entity_id = schedule_id;
    create_with_before.label = "创建带快照";
    create_with_before.before = snapshot;
    Check(repository.InsertOperation(create_with_before).status.code == ErrorCode::kInvalidArgument,
          "创建操作带快照应被拒绝");

    OperationRecord update_without_before;
    update_without_before.entity_type = OperationEntityType::kSchedule;
    update_without_before.type = ScheduleOperationType::kUpdate;
    update_without_before.entity_id = schedule_id;
    update_without_before.label = "修改无快照";
    Check(repository.InsertOperation(update_without_before).status.code == ErrorCode::kInvalidArgument,
          "修改操作缺快照应被拒绝");

    OperationRecord delete_without_before;
    delete_without_before.entity_type = OperationEntityType::kSchedule;
    delete_without_before.type = ScheduleOperationType::kDelete;
    delete_without_before.entity_id = schedule_id;
    delete_without_before.label = "删除无快照";
    Check(repository.InsertOperation(delete_without_before).status.code == ErrorCode::kInvalidArgument,
          "删除操作缺快照应被拒绝");

    // 创建 / 修改 / 删除操作的正常写入（覆盖 BindOperation 两种快照分支）。
    OperationRecord create_op;
    create_op.entity_type = OperationEntityType::kSchedule;
    create_op.type = ScheduleOperationType::kCreate;
    create_op.entity_id = schedule_id;
    create_op.label = "创建操作";
    const auto saved_create = repository.InsertOperation(create_op);
    Check(saved_create.ok() && saved_create.value->id > 0 && saved_create.value->operated_at != DateTime{},
          "应保存创建操作");

    OperationRecord update_op;
    update_op.entity_type = OperationEntityType::kSchedule;
    update_op.type = ScheduleOperationType::kUpdate;
    update_op.entity_id = schedule_id;
    update_op.label = "修改操作";
    update_op.before = snapshot;
    const auto saved_update = repository.InsertOperation(update_op);
    Check(saved_update.ok() && saved_update.value->before == snapshot, "应保存修改操作");

    OperationRecord delete_op;
    delete_op.entity_type = OperationEntityType::kException;
    delete_op.type = ScheduleOperationType::kDelete;
    delete_op.entity_id = 42;
    delete_op.label = "例外删除";
    delete_op.before = snapshot;
    const auto saved_delete = repository.InsertOperation(delete_op);
    Check(saved_delete.ok(), "应保存删除操作");

    // 默认查询应返回全部操作，同秒时按标识倒序。
    const auto all = repository.FindOperations(QueryOperationCommand{});
    Check(all.ok() && all.value->size() == 3, "应查询到全部操作");
    Check(all.value->front().id == saved_delete.value->id && all.value->back().id == saved_create.value->id,
          "查询应按标识倒序返回");

    // 按实体类型和操作类型筛选。
    QueryOperationCommand schedule_query;
    schedule_query.entity_type = OperationEntityType::kSchedule;
    const auto schedules = repository.FindOperations(schedule_query);
    Check(schedules.ok() && schedules.value->size() == 2, "实体类型筛选应命中两条操作");

    QueryOperationCommand create_query;
    create_query.type = ScheduleOperationType::kCreate;
    const auto creates = repository.FindOperations(create_query);
    Check(creates.ok() && creates.value->size() == 1 && creates.value->front().label == "创建操作",
          "操作类型筛选应命中创建操作");

    QueryOperationCommand by_entity;
    by_entity.entity_type = OperationEntityType::kSchedule;
    by_entity.entity_id = schedule_id;
    const auto by_entity_result = repository.FindOperations(by_entity);
    Check(by_entity_result.ok() && by_entity_result.value->size() == 2, "实体标识筛选应命中该日程操作");

    QueryOperationCommand by_id;
    by_id.operation_id = saved_update.value->id;
    const auto by_id_result = repository.FindOperations(by_id);
    Check(by_id_result.ok() && by_id_result.value->size() == 1 &&
              by_id_result.value->front().id == saved_update.value->id,
          "按操作 ID 应精确命中");

    // 时间窗口闭区间：全部操作同秒应命中，未来窗口无命中。
    QueryOperationCommand window;
    window.operated_from = saved_create.value->operated_at;
    window.operated_to = saved_create.value->operated_at;
    const auto ranged = repository.FindOperations(window);
    Check(ranged.ok() && ranged.value->size() == 3, "时间窗口应命中同秒操作");

    QueryOperationCommand empty_window;
    empty_window.operated_from = saved_create.value->operated_at + std::chrono::seconds{1};
    empty_window.operated_to = saved_create.value->operated_at + std::chrono::seconds{10};
    const auto empty_ranged = repository.FindOperations(empty_window);
    Check(empty_ranged.ok() && empty_ranged.value->empty(), "未来时间窗口应无命中");

    // 名称模糊匹配。
    QueryOperationCommand keyword;
    keyword.keyword = std::string{"操作"};
    const auto by_keyword = repository.FindOperations(keyword);
    Check(by_keyword.ok() && by_keyword.value->size() == 2, "名称模糊查询应命中两条操作");

    // 分页裁剪结果但总数不受影响。
    QueryOperationCommand paged;
    paged.limit = 2;
    paged.offset = 1;
    const auto page = repository.FindOperations(paged);
    Check(page.ok() && page.value->size() == 2, "分页查询应限制条数");

    const auto total = repository.CountOperations(QueryOperationCommand{});
    Check(total.ok() && total.value == 3, "总数统计应为三条");
    const auto filtered_total = repository.CountOperations(schedule_query);
    Check(filtered_total.ok() && filtered_total.value == 2, "筛选总数应为两条");
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
        // 删除操作表后：操作查询与统计应透传 SQL 编译错误。
        const TemporaryDatabaseFile file = MakeTemporaryDatabaseFile();
        SqliteDatabase database(file.path.string());
        Check(database.Open().ok(), "操作表失败分支应打开数据库");
        SqliteScheduleRepository repository(database);
        Check(repository.Initialize().ok(), "操作表失败分支应初始化表结构");
        Check(database.Execute("DROP TABLE operation_record").ok(), "应删除操作表制造 SQL 错误");
        Check(repository.FindOperations(QueryOperationCommand{}).status.code == ErrorCode::kInternal,
              "FindOperations 应透传操作表缺失错误");
        Check(repository.CountOperations(QueryOperationCommand{}).status.code == ErrorCode::kInternal,
              "CountOperations 应透传操作表缺失错误");
        const OperationRecord op{
            .entity_type = OperationEntityType::kSchedule,
            .type = ScheduleOperationType::kCreate,
            .entity_id = 1,
            .label = "失败写入",
        };
        Check(repository.InsertOperation(op).status.code == ErrorCode::kInternal,
              "InsertOperation 应透传操作表缺失错误");
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
