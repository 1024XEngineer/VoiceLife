# 日程操作记录模块设计（Schedule Operation Record）

**结论**：日程操作记录（记录操作 / 查询操作）是一个**纯审计日志模块**，只负责把"一次日程领域变更"追加到 `operation_record` 表、并按条件查询历史。它不感知 schedule / rule / exception 的实体结构，也不提供回滚接口——回滚由业务层读取日志中的操作前快照，再调用现有增删改查接口完成。

**适用范围**：`components/voicelife_schedule` 与 `components/voicelife_storage_sqlite`；对外暴露的查询能力通过 `components/voicelife_mcp` 的 MCP Tool 提供。

**状态**：设计定稿，尚未实现。本文是本特性的当前事实源；MCP Tool 定义实现时并入 [`mcp-tool-contract.md`](mcp-tool-contract.md)。本设计取代旧的 `record_schedule_operation` / `query_recent_schedule_operation` / `undo_schedule_operation` 接口及其撤销机制。

---

## 1. 架构决策

1. **纯日志模型**：操作模块只读写自己的 `operation_record` 表，不碰 schedule / rule / exception 三张表。快照由调用方序列化好（JSON 字符串）传入，操作模块不解析。
2. **无回滚接口**：回滚 = 业务层读日志 → 取 `before` 快照 → 调用现有 create/update/cancel 等接口把实体改回去。撤销的正确性在业务层，操作模块只保证"记录了什么、能查到什么"。
3. **记录产生方式**：显式推送，由各变更 service（create/update/cancel 等）在命令执行后调用 `record_operation`，采用"先变更、后记录"的尽力而为语义（方案 A）。
4. **只存 `before` 快照**：不存 `after`；撤销任意操作 = 恢复到 `before`（`before` 为空则删除实体），该逆操作规则由业务层执行，操作模块不实现。
5. **操作类型**：仅 `{ kCreate, kUpdate, kDelete }`，`kUndo` 已移除——撤销动作没有独立接口，回滚动作按其自然类型被记录。
6. **无撤销链**：`superseded_by` 已移除；日志是纯追加，"当前最新操作"由查询侧按实体 + 时间倒序推导。

## 2. 数据模型

### 2.1 枚举

```cpp
/// 操作对象实体类型：决定 before 快照的结构，以及查询时的实体维度。
enum class OperationEntityType { kSchedule = 1, kRule = 2, kException = 3 };

/// 可记录的操作类型。
enum class ScheduleOperationType { kCreate = 1, kUpdate = 2, kDelete = 3 };
```

### 2.2 操作记录

```cpp
/// 日程操作记录，对应 operation_record 数据表。
struct OperationRecord {
    OperationId id = 0;
    OperationEntityType entity_type = OperationEntityType::kSchedule;
    ScheduleOperationType type = ScheduleOperationType::kCreate;
    int64_t entity_id = 0;
    DateTime operated_at;              ///< 仓储盖章，不来自调用方
    std::string label;                 ///< 展示用名称（日程名 / 规则名 / 例外描述）
    std::optional<std::string> before; ///< 操作前快照 JSON；kCreate 必须为空
};
```

字段语义：

| 字段 | 语义 |
| --- | --- |
| `entity_type` + `entity_id` | 定位被操作实体；`entity_id` 为 schedule / rule / exception 统一的 64 位主键 |
| `label` | 冗余展示名，撤销/历史列表直接展示，不解析快照 |
| `before` | 操作前实体的 JSON 序列化；`kCreate` 为空（操作前实体不存在），`kUpdate` / `kDelete` 必须有值 |
| `operated_at` | 记录写入时间，精确到秒，由仓储生成 |

快照的 JSON 结构由各实体（schedule / rule / exception）序列化器决定，操作模块不感知。

### 2.3 表结构（示意）

```sql
CREATE TABLE operation_record (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    entity_type INTEGER NOT NULL CHECK (entity_type IN (1, 2, 3)),
    type INTEGER NOT NULL CHECK (type IN (1, 2, 3)),
    entity_id INTEGER NOT NULL CHECK (entity_id > 0),
    label TEXT NOT NULL CHECK (length(label) BETWEEN 1 AND 100),
    operated_at INTEGER NOT NULL,
    before TEXT,
    CHECK ((type = 1 AND before IS NULL) OR (type IN (2, 3) AND before IS NOT NULL))
);
CREATE INDEX operation_record_recent_idx ON operation_record (operated_at DESC, id DESC);
```

表结构为示意图，具体约束与迁移脚本以实现为准。

## 3. 仓储接口

```cpp
/// 定义日程操作日志的持久化能力。
class ScheduleOperationRepository {
   public:
    virtual ~ScheduleOperationRepository() = default;

    /// 插入一条操作记录，生成 id 和 operated_at。
    virtual Result<OperationRecord> InsertOperation(const OperationRecord& operation) = 0;

    /// 按筛选条件查询操作记录，按 operated_at DESC, id DESC 排序。
    virtual Result<std::vector<OperationRecord>> FindOperations(const QueryOperationCommand& query) const = 0;

    /// 统计满足筛选条件的总条数（不受分页影响），配合查询结果 total。
    virtual Result<int64_t> CountOperations(const QueryOperationCommand& query) const = 0;
};
```

## 4. Service 接口

### 4.1 记录操作

```cpp
/// 写入一条日程操作记录所需的数据。
struct RecordOperationCommand {
    OperationEntityType entity_type = OperationEntityType::kSchedule;
    ScheduleOperationType type = ScheduleOperationType::kCreate;
    int64_t entity_id = 0;
    std::string label;
    std::optional<std::string> before; ///< JSON 快照；kCreate 必须为空
};

/// 记录操作的返回数据。
struct RecordOperationResult {
    CommandResult<std::optional<OperationRecord>> result;
};
```

记录操作是**内部链路**：由 create / update / cancel 等变更 service 在命令执行成功后调用，不暴露为 MCP Tool。调用方负责：

1. 在执行变更前加载实体当前状态并序列化为 `before`；
2. 执行变更；
3. 调用 `record_operation` 推送记录。

写入失败不回滚已完成的变更（方案 A：尽力而为），最坏情况是某次变更失去日志记录。

### 4.2 查询操作

```cpp
/// 查询操作记录所需的筛选和分页条件；与 QueryScheduleCommand 对齐。
struct QueryOperationCommand {
    std::optional<OperationId> operation_id;      ///< 精确查单条，读 before 快照用
    std::optional<OperationEntityType> entity_type;
    std::optional<int64_t> entity_id;             ///< 需配合 entity_type
    std::optional<ScheduleOperationType> type;
    std::optional<DateTime> operated_from;        ///< 时间范围下界
    std::optional<DateTime> operated_to;          ///< 时间范围上界
    std::optional<std::string> keyword;           ///< label 模糊匹配
    int64_t limit = 20;
    int64_t offset = 0;
};

/// 查询操作的返回数据，total 不受分页影响。
struct QueryOperationResult {
    CommandResult<std::vector<OperationRecord>> result;
    int64_t total = 0;
};
```

### 4.3 Service

```cpp
class ScheduleOperationService {
   public:
    explicit ScheduleOperationService(ScheduleOperationRepository& operation_repository);

    RecordOperationResult record_operation(const RecordOperationCommand& command);

    QueryOperationResult query_operations(const QueryOperationCommand& command) const;
};
```

### 4.4 校验规则

**记录操作**：

- `entity_type` ∈ {kSchedule, kRule, kException}，`type` ∈ {kCreate, kUpdate, kDelete}；
- `entity_id > 0`；
- `label` 非空且 ≤ 100 字符；
- `before` 与 type 一致：`kCreate` 必须为空，`kUpdate` / `kDelete` 必须有值；
- `before` 长度 ≤ 2048（防呆保护）。

**查询操作**：

- `entity_id` 必须配合 `entity_type` 使用；
- `operated_from ≤ operated_to`（同时提供时）；
- `limit > 0` 且 ≤ 100，`offset ≥ 0`。

## 5. MCP Tool

对外只暴露**查询**能力，工具名暂定 `schedule.operation_query`。记录操作不暴露为 Tool。

### 5.1 入参

| 工具名称 | 参数 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- | --- |
| `schedule.operation_query` | `entity_type` | string | 否 | 操作对象类型：`schedule`、`rule`、`exception` |
| `schedule.operation_query` | `type` | string | 否 | 操作类型：`create`、`update`、`delete` |
| `schedule.operation_query` | `keyword` | string | 否 | 按操作对象名称模糊搜索 |

```cpp
PropertyList OperationQueryProperties() {
    return PropertyList({
        Property::Optional("entity_type", PropertyType::kString)
            .with_description("操作对象类型，取值为 schedule、rule、exception"),
        Property::Optional("type", PropertyType::kString)
            .with_description("操作类型，取值为 create、update、delete"),
        Property::Optional("keyword", PropertyType::kString)
            .with_description("按操作对象名称模糊搜索"),
    });
}
```

### 5.2 Handler 映射

Tool 入参 → service 命令，"最近 15 分钟"窗口由 handler 作为调用方约定填充：

```cpp
schedule::QueryOperationCommand command;
command.entity_type   = ParseEntityType(properties.value<std::string>("entity_type"));
command.type          = ParseOperationType(properties.value<std::string>("type"));
command.keyword       = properties.value<std::string>("keyword");
command.operated_from = Now() - std::chrono::minutes{15};
command.operated_to   = Now();
command.limit = 50;
command.offset = 0;
const auto result = service.query_operations(command);
```

### 5.3 出参

统一遵循 MCP 返回协议（`status` / `message`）：

| 工具名称 | 返回字段 | 类型 | 必返 | 说明 |
| --- | --- | --- | --- | --- |
| `schedule.operation_query` | `status` | string | 是 | `success` 或 `failure` |
| `schedule.operation_query` | `message` | string | 是 | 结果描述 |
| `schedule.operation_query` | `total` | integer | 是 | 满足筛选条件的总条数 |
| `schedule.operation_query` | `operations` | array | 是 | 操作记录列表，元素为 `operation` |

`operation` 数据结构：

| 返回字段 | 类型 | 必返 | 说明 |
| --- | --- | --- | --- |
| `id` | integer | 是 | 操作记录 ID |
| `entity_type` | string | 是 | `schedule`、`rule`、`exception` |
| `type` | string | 是 | `create`、`update`、`delete` |
| `entity_id` | integer | 是 | 被操作实体 ID |
| `label` | string | 是 | 操作对象名称 |
| `operated_at` | string | 是 | 操作时间，`YYYY-MM-DD HH:mm:ss` |
| `before` | object \| null | 是 | 操作前快照；`create` 为 `null`，其字段结构随 `entity_type` 而定 |

## 6. 分层与集成

```
MCP Tool (schedule.operation_query)
   │  解析入参，填窗口/分页
   ▼
ScheduleOperationService::query_operations   ←── 变更 service（create/update/cancel）──→  record_operation
   ▼
ScheduleOperationRepository (SQLite)
   ▼
operation_record 表
```

- **tool 层**：入参面向 AI / 语音，字符串、自然语言友好；只做解析、路由、结果组装，不使用 Repository。
- **service 层**：完整领域命令（含精确查询、时间范围、分页），校验 + 结果。
- **仓储层**：窗口 SQL、COUNT、排序，操作模块唯一的持久化点。
- **记录写入**：不经过 tool，由变更 service 在命令成功后显式调用 `record_operation`；`before` 快照由变更 service 序列化。

## 7. 已知语义与取舍

1. **盲恢复**：只存 `before` 意味着回滚时不做"当前状态是否等于预期"的校验，业务层直接用快照覆盖当前实体；是否校验由业务层决定。
2. **撤销顺序**：日志不限定"只能撤最近一条"。业务层可撤销任意记录，若先撤销早期记录而未撤销其后的记录，可能产生非直觉结果。
3. **窗口是约定**：15 分钟窗口不是查询的固有行为，由调用方（tool handler / 业务层）通过 `operated_from/operated_to` 表达。
4. **记录可靠性**：先变更后记录（方案 A），写入失败不回滚变更；最坏情况是某次变更无日志，实体仍存在。
5. **快照版本**：`before` 是写入时的实体序列化；设备 OTA 升级后旧快照可能无法被当前 schema 解析，序列化格式需保持稳定（后续可单独议）。

## 8. 待办 / 未决

- 记录写入的接入点：create / update / cancel（schedule）以及 rule / exception 变更路径逐一接线，序列化器实现。
- 旧接口清理：移除 `record_schedule_operation` / `query_recent_schedule_operation` / `undo_schedule_operation` 及仓储 `UndoOperation`、`active` 软删除、`previous_*` 摊平列。
- 日志保留策略：`operation_record` 是否会无限增长，是否需要清理/归档（当前查询窗口只覆盖最近 15 分钟，历史行不影响查询但占存储）。
- 实现时把 `schedule.operation_query` 并入 [`mcp-tool-contract.md`](mcp-tool-contract.md)，并更新其对 `ScheduleOperationService` 的引用（现行为旧接口名 `record_schedule_operation`）。
