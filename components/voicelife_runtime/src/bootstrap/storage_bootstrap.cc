#include "bootstrap/storage_bootstrap.h"

#include <memory>
#include <string>
#include <utility>

#if defined(ESP_PLATFORM) && CONFIG_VOICELIFE_STORAGE_FATFS_RUNTIME
#include "voicelife/storage_fatfs/fatfs_volume.h"
#include "voicelife/storage_sqlite/sqlite_database.h"
#include "voicelife/storage_sqlite/sqlite_schedule_reminder_task_repository.h"
#include "voicelife/storage_sqlite/sqlite_schedule_repository.h"
#include "voicelife/storage_sqlite/sqlite_schedule_rule_repository.h"
#include "voicelife/storage_sqlite/sqlite_schema.h"
#include "voicelife/storage_sqlite/voicelife_schema.h"
#else
#include "voicelife/storage_memory/memory_schedule_reminder_task_repository.h"
#include "voicelife/storage_memory/memory_schedule_repository.h"
#endif

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#endif

namespace voicelife::runtime {
namespace {

#if defined(ESP_PLATFORM) && CONFIG_VOICELIFE_STORAGE_FATFS_RUNTIME
constexpr char kStorageTag[] = "VoiceLifeStorage";
constexpr char kDatabaseFileName[] = "voicelife.db";

/**
 * @brief 将状态附加基础设施阶段信息。
 * @param stage 失败的启动阶段。
 * @param status 原始失败状态。
 * @return 带有阶段说明的失败状态。
 */
Status WithStage(const char* stage, const Status& status) {
    if (status.ok()) return status;
    return Status::Error(status.code, std::string(stage) + "：" + status.message);
}

/**
 * @brief 根据 FATFS 挂载路径构造 SQLite URI。
 * @param base_path FATFS VFS 路径前缀。
 * @return 带有 powersafe overwrite 约定的数据库 URI。
 */
std::string DatabaseUri(const std::string& base_path) {
    return "file:" + base_path + "/" + kDatabaseFileName + "?psow=0";
}
#endif

}  // namespace

/** @brief 存储装配器的私有实现，隔离平台类型和资源声明。 */
class StorageBootstrap::Impl final {
   public:
    /** @brief 构造尚未启动的私有资源集合。 */
    Impl()
#if defined(ESP_PLATFORM) && CONFIG_VOICELIFE_STORAGE_FATFS_RUNTIME
        : volume_(MakeVolumeConfig()),
          database_(DatabaseUri(volume_.config().base_path), "unix-none"),
          schedule_repository_(database_),
          schedule_rule_repository_(database_),
          schedule_reminder_task_repository_(database_)
#endif
    {
    }

    /** @brief 析构时释放仍然持有的资源。 */
    ~Impl() { (void)Stop(); }

    /**
     * @brief 执行存储启动链路。
     * @return 挂载、数据库和 Schema 检查均成功时返回成功状态。
     */
    Status Start() {
#if defined(ESP_PLATFORM) && CONFIG_VOICELIFE_STORAGE_FATFS_RUNTIME
        if (ready_) return Status::Ok();

        ESP_LOGI(kStorageTag, "HEAP_TRACE before_mount free_internal=%u free_total=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)esp_get_free_heap_size());
        Status status = volume_.Mount();
        if (!status.ok()) {
            ESP_LOGE(kStorageTag, "STORAGE_MOUNT_FAILED: %s", status.message.c_str());
            return WithStage("挂载持久化数据卷失败", status);
        }
        ESP_LOGI(kStorageTag, "HEAP_TRACE after_mount free_internal=%u free_total=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)esp_get_free_heap_size());

        status = database_.Open();
        if (!status.ok()) {
            ESP_LOGE(kStorageTag, "STORAGE_SQLITE_OPEN_FAILED: %s", status.message.c_str());
            (void)volume_.Unmount();
            return WithStage("打开持久化 SQLite 失败", status);
        }
        ESP_LOGI(kStorageTag, "HEAP_TRACE after_open free_internal=%u free_total=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)esp_get_free_heap_size());

        status = storage_sqlite::VoiceLifeSchema::Initialize(database_);
        if (!status.ok()) {
            ESP_LOGE(kStorageTag, "STORAGE_SCHEMA_CHECK_FAILED: %s", status.message.c_str());
            database_.Close();
            (void)volume_.Unmount();
            return WithStage("检查持久化 SQLite Schema 失败", status);
        }
        ESP_LOGI(kStorageTag, "HEAP_TRACE after_schema free_internal=%u free_total=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)esp_get_free_heap_size());

        const auto schema_version = storage_sqlite::SqliteSchema::ReadVersion(database_);
        if (!schema_version.ok() || !schema_version.value.has_value()) {
            const Status version_status = schema_version.ok()
                                              ? Status::Error(ErrorCode::kInternal, "Schema 版本结果为空")
                                              : schema_version.status;
            ESP_LOGE(kStorageTag, "STORAGE_SCHEMA_VERSION_FAILED: %s", version_status.message.c_str());
            database_.Close();
            (void)volume_.Unmount();
            return WithStage("读取持久化 SQLite Schema 版本失败", version_status);
        }

        const auto capacity = volume_.Capacity();
        if (!capacity.ok() || !capacity.value.has_value()) {
            const Status capacity_status =
                capacity.ok() ? Status::Error(ErrorCode::kInternal, "容量结果为空") : capacity.status;
            ESP_LOGE(kStorageTag, "STORAGE_CAPACITY_FAILED: %s", capacity_status.message.c_str());
            database_.Close();
            (void)volume_.Unmount();
            return WithStage("查询持久化数据卷容量失败", capacity_status);
        }

        ready_ = true;
        ESP_LOGI(kStorageTag, "STORAGE_READY=1 partition=%s path=%s total_bytes=%llu free_bytes=%llu schema_version=%u",
                 volume_.config().partition_label.c_str(), volume_.config().base_path.c_str(),
                 static_cast<unsigned long long>(capacity.value->total_bytes),
                 static_cast<unsigned long long>(capacity.value->free_bytes),
                 static_cast<unsigned>(*schema_version.value));
        return Status::Ok();
#else
        ready_ = true;
#ifdef ESP_PLATFORM
        ESP_LOGI("VoiceLifeStorage", "STORAGE_MEMORY_READY=1 persistence=volatile");
#endif
        return Status::Ok();
#endif
    }

    /**
     * @brief 逆序释放存储资源。
     * @return FATFS 卸载结果。
     */
    Status Stop() {
#if defined(ESP_PLATFORM) && CONFIG_VOICELIFE_STORAGE_FATFS_RUNTIME
        ready_ = false;
        database_.Close();
        return volume_.Unmount();
#else
        ready_ = false;
        return Status::Ok();
#endif
    }

    /**
     * @brief 查询装配状态。
     * @return 基础设施已就绪时返回 true。
     */
    [[nodiscard]] bool IsReady() const { return ready_; }

#ifdef ESP_PLATFORM
    /**
     * @brief 获取共享当前 SQLite 连接的日程仓储。
     * @return 生命周期与私有实现一致的日程仓储引用。
     */
    [[nodiscard]] schedule::ScheduleRepository& GetScheduleRepository() { return schedule_repository_; }

    /**
     * @brief 获取共享当前 SQLite 连接的日程操作仓储。
     * @return 生命周期与私有实现一致的操作仓储引用。
     */
    [[nodiscard]] schedule::ScheduleOperationRepository& GetScheduleOperationRepository() {
        return schedule_repository_;
    }

    [[nodiscard]] schedule::ScheduleRuleRepository& GetScheduleRuleRepository() { return schedule_rule_repository_; }

    [[nodiscard]] schedule::ScheduleExceptionRepository& GetScheduleExceptionRepository() {
        return schedule_rule_repository_;
    }

    [[nodiscard]] schedule::ScheduleReminderTaskRepository& GetScheduleReminderTaskRepository() {
        return schedule_reminder_task_repository_;
    }
#endif

   private:
#if defined(ESP_PLATFORM) && CONFIG_VOICELIFE_STORAGE_FATFS_RUNTIME
    /**
     * @brief 构造当前产品分区的 FATFS 配置。
     * @return 含有生产分区约束的配置。
     */
    static storage_fatfs::FatFsVolumeConfig MakeVolumeConfig() {
        storage_fatfs::FatFsVolumeConfig config = storage_fatfs::DefaultFatFsVolumeConfig();
        config.expected_partition_address = CONFIG_VOICELIFE_STORAGE_FATFS_EXPECTED_PARTITION_ADDRESS;
        config.expected_partition_size = CONFIG_VOICELIFE_STORAGE_FATFS_EXPECTED_PARTITION_SIZE;
        return config;
    }

    storage_fatfs::FatFsVolume volume_;
    storage_sqlite::SqliteDatabase database_;
    storage_sqlite::SqliteScheduleRepository schedule_repository_;
    storage_sqlite::SqliteScheduleRuleRepository schedule_rule_repository_;
    storage_sqlite::SqliteScheduleReminderTaskRepository schedule_reminder_task_repository_;
#else
    storage_memory::MemoryScheduleRepository schedule_repository_;
    storage_memory::MemoryScheduleRuleRepository schedule_rule_repository_{schedule_repository_};
    storage_memory::MemoryScheduleReminderTaskRepository schedule_reminder_task_repository_;
#endif
    bool ready_ = false;
};

StorageBootstrap::StorageBootstrap() : impl_(std::make_unique<Impl>()) {}

StorageBootstrap::~StorageBootstrap() = default;

Status StorageBootstrap::Start() { return impl_->Start(); }

Status StorageBootstrap::Stop() { return impl_->Stop(); }

bool StorageBootstrap::IsReady() const { return impl_->IsReady(); }

#ifdef ESP_PLATFORM
schedule::ScheduleRepository& StorageBootstrap::GetScheduleRepository() { return impl_->GetScheduleRepository(); }

schedule::ScheduleOperationRepository& StorageBootstrap::GetScheduleOperationRepository() {
    return impl_->GetScheduleOperationRepository();
}

schedule::ScheduleRuleRepository& StorageBootstrap::GetScheduleRuleRepository() {
    return impl_->GetScheduleRuleRepository();
}

schedule::ScheduleExceptionRepository& StorageBootstrap::GetScheduleExceptionRepository() {
    return impl_->GetScheduleExceptionRepository();
}

schedule::ScheduleReminderTaskRepository& StorageBootstrap::GetScheduleReminderTaskRepository() {
    return impl_->GetScheduleReminderTaskRepository();
}
#endif

}  // namespace voicelife::runtime
