#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <vector>

namespace voicelife::voice {

/** @brief 为实时 PCM 负载提供固定槽位租约的资源池。 */
class AudioPayloadPool;

/** @brief 可移动的音频字节负载，可在销毁时归还固定池槽位。 */
class AudioPayload final {
   public:
    /** @brief 构造空负载。 */
    AudioPayload() = default;
    /** @brief 销毁负载并归还已租用的池槽位。 */
    ~AudioPayload();
    /** @brief 禁止复制负载，避免重复归还池槽位。 */
    AudioPayload(const AudioPayload&) = delete;
    /** @brief 禁止复制赋值，避免重复归还池槽位。 */
    AudioPayload& operator=(const AudioPayload&) = delete;
    /** @brief 移动构造负载并转移池槽位所有权。
     * @param other 被转移的负载。
     */
    AudioPayload(AudioPayload&& other) noexcept;
    /** @brief 移动赋值负载并转移池槽位所有权。
     * @param other 被转移的负载。
     * @return 当前负载引用。
     */
    AudioPayload& operator=(AudioPayload&& other) noexcept;

    /** @brief 以堆字节替换当前负载。
     * @param bytes 要接管的字节向量。
     * @return 当前负载引用。
     */
    AudioPayload& operator=(std::vector<uint8_t> bytes);
    /** @brief 以字节列表替换当前负载。
     * @param bytes 要复制的字节列表。
     * @return 当前负载引用。
     */
    AudioPayload& operator=(std::initializer_list<uint8_t> bytes);

    /** @brief 返回只读负载起始地址。
     * @return 负载为空时可为 null 的只读地址。
     */
    [[nodiscard]] const uint8_t* data() const;
    /** @brief 返回可写负载起始地址。
     * @return 负载为空时可为 null 的可写地址。
     */
    [[nodiscard]] uint8_t* data();
    /** @brief 返回当前负载字节数。
     * @return 字节数。
     */
    [[nodiscard]] std::size_t size() const;
    /** @brief 判断负载是否为空。
     * @return 无字节时返回 true。
     */
    [[nodiscard]] bool empty() const;
    /** @brief 判断负载是否持有池化槽位。
     * @return 持有池槽位时返回 true。
     */
    [[nodiscard]] bool pooled() const { return pool_ != nullptr; }
    [[nodiscard]] const uint8_t& operator[](std::size_t index) const { return data()[index]; }
    [[nodiscard]] uint8_t& operator[](std::size_t index) { return data()[index]; }

    /** @brief 调整负载字节数。
     * @param size 调整后的字节数。
     */
    void resize(std::size_t size);
    /** @brief 以重复字节填充负载。
     * @param count 要写入的字节数。
     * @param value 填充字节。
     */
    void assign(std::size_t count, uint8_t value);

    /** @brief 从迭代器范围复制字节。
     * @param first 范围起始迭代器。
     * @param last 范围结束迭代器。
     */
    template <typename Iterator>
    void assign(Iterator first, Iterator last) {
        (*this).Reset();
        heap_bytes_.assign(first, last);
    }

   private:
    friend class AudioPayloadPool;
    AudioPayload(std::shared_ptr<AudioPayloadPool> pool, std::size_t slot, uint8_t* data,
                 std::size_t capacity) noexcept;
    void Reset() noexcept;

    std::shared_ptr<AudioPayloadPool> pool_;
    std::size_t slot_ = 0;
    uint8_t* pooled_data_ = nullptr;
    std::size_t pooled_capacity_ = 0;
    std::size_t pooled_size_ = 0;
    std::vector<uint8_t> heap_bytes_;
};

/** @brief 为 PCM 生产者提供固定大小、非阻塞获取的负载租约池。 */
class AudioPayloadPool final : public std::enable_shared_from_this<AudioPayloadPool> {
   public:
    static constexpr std::size_t kSlotsPerBitmapWord = 32;
    static constexpr std::size_t kBitmapWordCount = 4;
    static constexpr std::size_t kMaximumSlots = kSlotsPerBitmapWord * kBitmapWordCount;

    /** @brief 创建并初始化固定槽位池。
     * @param slot_count 可同时租用的槽位数。
     * @param slot_bytes 每个槽位的字节容量。
     * @return 创建成功返回池对象，内存不足时返回空指针。
     */
    static std::shared_ptr<AudioPayloadPool> Create(std::size_t slot_count, std::size_t slot_bytes);
    /** @brief 销毁池及尚未归还的底层存储。 */
    ~AudioPayloadPool();
    /** @brief 禁止复制资源池。 */
    AudioPayloadPool(const AudioPayloadPool&) = delete;
    /** @brief 禁止复制赋值资源池。 */
    AudioPayloadPool& operator=(const AudioPayloadPool&) = delete;

    /** @brief 非阻塞地尝试获取一个负载槽位。
     * @return 有可用槽位时返回池化负载，否则返回空负载。
     */
    [[nodiscard]] AudioPayload TryAcquire();
    /** @brief 返回池中槽位总数。
     * @return 槽位总数。
     */
    [[nodiscard]] std::size_t slot_count() const { return slot_count_; }
    /** @brief 返回每个槽位的字节容量。
     * @return 槽位字节容量。
     */
    [[nodiscard]] std::size_t slot_bytes() const { return slot_bytes_; }
    /** @brief 返回非阻塞获取失败的累计次数。
     * @return 获取失败次数。
     */
    [[nodiscard]] std::size_t acquisition_failures() const;
    /** @brief 返回当前已租用的槽位数。
     * @return 当前已租用槽位数。
     */
    [[nodiscard]] std::size_t in_use() const;
    /** @brief 返回同时被租用的槽位历史峰值。
     * @return 峰值已租用槽位数。
     */
    [[nodiscard]] std::size_t high_watermark() const;

   private:
    friend class AudioPayload;
    AudioPayloadPool(std::size_t slot_count, std::size_t slot_bytes) noexcept;
    bool Initialize() noexcept;
    void Release(std::size_t slot) noexcept;

    std::size_t slot_count_ = 0;
    std::size_t slot_bytes_ = 0;
    uint8_t* bytes_ = nullptr;
    // PCM producers and consumers run on different real-time tasks. Slot
    // ownership therefore remains a non-blocking bitmap rather than a mutex.
    std::array<std::atomic<uint32_t>, kBitmapWordCount> available_slots_{};
    std::atomic_size_t in_use_{0};
    std::atomic_size_t high_watermark_{0};
    std::atomic_size_t acquisition_failures_{0};
};

}  // namespace voicelife::voice
