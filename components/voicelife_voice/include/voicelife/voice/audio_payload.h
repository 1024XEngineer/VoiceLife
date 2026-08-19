#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <vector>

namespace voicelife::voice {

class AudioPayloadPool;

/** Move-only audio bytes that optionally return a fixed slot to its source pool. */
class AudioPayload final {
   public:
    AudioPayload() = default;
    ~AudioPayload();
    AudioPayload(const AudioPayload&) = delete;
    AudioPayload& operator=(const AudioPayload&) = delete;
    AudioPayload(AudioPayload&& other) noexcept;
    AudioPayload& operator=(AudioPayload&& other) noexcept;

    AudioPayload& operator=(std::vector<uint8_t> bytes);
    AudioPayload& operator=(std::initializer_list<uint8_t> bytes);

    [[nodiscard]] const uint8_t* data() const;
    [[nodiscard]] uint8_t* data();
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] bool empty() const;
    [[nodiscard]] bool pooled() const { return pool_ != nullptr; }
    [[nodiscard]] const uint8_t& operator[](std::size_t index) const { return data()[index]; }
    [[nodiscard]] uint8_t& operator[](std::size_t index) { return data()[index]; }

    void resize(std::size_t size);
    void assign(std::size_t count, uint8_t value);

    template <typename Iterator>
    void assign(Iterator first, Iterator last) {
        Reset();
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

/** Fixed-size, nonblocking-acquire pool for a PCM producer's payload leases. */
class AudioPayloadPool final : public std::enable_shared_from_this<AudioPayloadPool> {
   public:
    static std::shared_ptr<AudioPayloadPool> Create(std::size_t slot_count, std::size_t slot_bytes);
    ~AudioPayloadPool();
    AudioPayloadPool(const AudioPayloadPool&) = delete;
    AudioPayloadPool& operator=(const AudioPayloadPool&) = delete;

    [[nodiscard]] AudioPayload TryAcquire();
    [[nodiscard]] std::size_t slot_count() const { return slot_count_; }
    [[nodiscard]] std::size_t slot_bytes() const { return slot_bytes_; }
    [[nodiscard]] std::size_t acquisition_failures() const;
    [[nodiscard]] std::size_t high_watermark() const;

   private:
    friend class AudioPayload;
    AudioPayloadPool(std::size_t slot_count, std::size_t slot_bytes) noexcept;
    bool Initialize() noexcept;
    void Release(std::size_t slot) noexcept;

    std::size_t slot_count_ = 0;
    std::size_t slot_bytes_ = 0;
    uint8_t* bytes_ = nullptr;
    std::unique_ptr<bool[]> used_;
    mutable std::mutex mutex_;
    std::size_t in_use_ = 0;
    std::size_t high_watermark_ = 0;
    std::atomic_size_t acquisition_failures_{0};
};

}  // namespace voicelife::voice
