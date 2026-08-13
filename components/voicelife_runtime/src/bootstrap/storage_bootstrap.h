#pragma once

#include <memory>

#include "voicelife/contracts/status.h"

namespace voicelife::schedule {
class ScheduleRepository;
}

namespace voicelife::runtime {

/**
 * @brief 负责组装并管理运行时的持久化基础设施。
 *
 * 存储启动顺序固定为 FATFS/Wear Levelling 挂载、SQLite 连接、Schema 健康检查。
 * 该类只管理存储资源；组合根可在就绪后借用 Repository 创建独立的应用模块。
 */
class StorageBootstrap final {
   public:
    /** @brief 创建尚未启动的存储装配器。 */
    StorageBootstrap();

    /** @brief 释放存储装配器及其持有的基础设施。 */
    ~StorageBootstrap();

    /** @brief 禁止复制存储装配器。 */
    StorageBootstrap(const StorageBootstrap&) = delete;
    /** @brief 禁止复制赋值存储装配器。 */
    StorageBootstrap& operator=(const StorageBootstrap&) = delete;
    /** @brief 禁止移动存储装配器，以保持资源地址稳定。 */
    StorageBootstrap(StorageBootstrap&&) = delete;
    /** @brief 禁止移动赋值存储装配器。 */
    StorageBootstrap& operator=(StorageBootstrap&&) = delete;

    /**
     * @brief 挂载数据卷并打开、检查 SQLite 数据库。
     * @return 全部基础设施就绪时返回成功状态。
     */
    Status Start();

    /**
     * @brief 先关闭数据库，再卸载文件系统，按启动顺序逆序释放基础设施。
     * @return 卸载结果；数据库关闭本身没有独立失败状态。
     */
    Status Stop();

    /**
     * @brief 查询 SQLite 健康链路是否已经完成。
     * @return 已成功启动且尚未停止时返回 true。
     */
    [[nodiscard]] bool IsReady() const;

    /** @brief 返回生命周期受 StorageBootstrap 管理的日程 Repository。 */
    [[nodiscard]] schedule::ScheduleRepository* schedule_repository();

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace voicelife::runtime
