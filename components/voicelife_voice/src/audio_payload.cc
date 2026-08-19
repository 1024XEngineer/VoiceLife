#include "voicelife/voice/audio_payload.h"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

namespace voicelife::voice {

AudioPayload::AudioPayload(std::shared_ptr<AudioPayloadPool> pool, std::size_t slot, uint8_t* data,
                           std::size_t capacity) noexcept
    : pool_(std::move(pool)), slot_(slot), pooled_data_(data), pooled_capacity_(capacity), pooled_size_(capacity) {}

AudioPayload::~AudioPayload() { Reset(); }

AudioPayload::AudioPayload(AudioPayload&& other) noexcept { *this = std::move(other); }

AudioPayload& AudioPayload::operator=(AudioPayload&& other) noexcept {
    if (this == &other) return *this;
    Reset();
    pool_ = std::move(other.pool_);
    slot_ = other.slot_;
    pooled_data_ = other.pooled_data_;
    pooled_capacity_ = other.pooled_capacity_;
    pooled_size_ = other.pooled_size_;
    heap_bytes_ = std::move(other.heap_bytes_);
    other.slot_ = 0;
    other.pooled_data_ = nullptr;
    other.pooled_capacity_ = 0;
    other.pooled_size_ = 0;
    return *this;
}

AudioPayload& AudioPayload::operator=(std::vector<uint8_t> bytes) {
    Reset();
    heap_bytes_ = std::move(bytes);
    return *this;
}

AudioPayload& AudioPayload::operator=(std::initializer_list<uint8_t> bytes) {
    Reset();
    heap_bytes_ = bytes;
    return *this;
}

const uint8_t* AudioPayload::data() const { return pool_ != nullptr ? pooled_data_ : heap_bytes_.data(); }

uint8_t* AudioPayload::data() { return pool_ != nullptr ? pooled_data_ : heap_bytes_.data(); }

std::size_t AudioPayload::size() const { return pool_ != nullptr ? pooled_size_ : heap_bytes_.size(); }

bool AudioPayload::empty() const { return size() == 0; }

void AudioPayload::resize(std::size_t size) {
    if (pool_ != nullptr) {
        pooled_size_ = std::min(size, pooled_capacity_);
        return;
    }
    heap_bytes_.resize(size);
}

void AudioPayload::assign(std::size_t count, uint8_t value) {
    Reset();
    heap_bytes_.assign(count, value);
}

void AudioPayload::Reset() noexcept {
    heap_bytes_.clear();
    if (pool_ != nullptr) pool_->Release(slot_);
    pool_.reset();
    slot_ = 0;
    pooled_data_ = nullptr;
    pooled_capacity_ = 0;
    pooled_size_ = 0;
}

AudioPayloadPool::AudioPayloadPool(std::size_t slot_count, std::size_t slot_bytes) noexcept
    : slot_count_(slot_count), slot_bytes_(slot_bytes) {}

AudioPayloadPool::~AudioPayloadPool() {
#ifdef ESP_PLATFORM
    heap_caps_free(bytes_);
#else
    delete[] bytes_;
#endif
}

std::shared_ptr<AudioPayloadPool> AudioPayloadPool::Create(std::size_t slot_count, std::size_t slot_bytes) {
    if (slot_count == 0 || slot_bytes == 0 || slot_count > std::numeric_limits<std::size_t>::max() / slot_bytes) {
        return {};
    }
    std::shared_ptr<AudioPayloadPool> pool(new (std::nothrow) AudioPayloadPool(slot_count, slot_bytes));
    return pool != nullptr && pool->Initialize() ? pool : std::shared_ptr<AudioPayloadPool>{};
}

bool AudioPayloadPool::Initialize() noexcept {
    used_.reset(new (std::nothrow) bool[slot_count_]{});
    if (used_ == nullptr) return false;
    const std::size_t total_bytes = slot_count_ * slot_bytes_;
#ifdef ESP_PLATFORM
    bytes_ = static_cast<uint8_t*>(heap_caps_malloc(total_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#else
    bytes_ = new (std::nothrow) uint8_t[total_bytes];
#endif
    return bytes_ != nullptr;
}

AudioPayload AudioPayloadPool::TryAcquire() {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        acquisition_failures_.fetch_add(1, std::memory_order_relaxed);
        return {};
    }
    for (std::size_t slot = 0; slot < slot_count_; ++slot) {
        if (used_[slot]) continue;
        used_[slot] = true;
        ++in_use_;
        high_watermark_ = std::max(high_watermark_, in_use_);
        return AudioPayload(shared_from_this(), slot, bytes_ + slot * slot_bytes_, slot_bytes_);
    }
    acquisition_failures_.fetch_add(1, std::memory_order_relaxed);
    return {};
}

void AudioPayloadPool::Release(std::size_t slot) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot >= slot_count_ || !used_[slot]) return;
    used_[slot] = false;
    --in_use_;
}

std::size_t AudioPayloadPool::acquisition_failures() const {
    return acquisition_failures_.load(std::memory_order_relaxed);
}

std::size_t AudioPayloadPool::high_watermark() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return high_watermark_;
}

}  // namespace voicelife::voice
