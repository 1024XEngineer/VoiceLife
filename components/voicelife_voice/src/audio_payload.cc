#include "voicelife/voice/audio_payload.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <new>
#include <utility>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

namespace voicelife::voice {
namespace {

uint32_t AvailableMask(std::size_t slot_count) {
    return slot_count == AudioPayloadPool::kSlotsPerBitmapWord ? std::numeric_limits<uint32_t>::max()
                                                               : (uint32_t{1} << slot_count) - uint32_t{1};
}

}  // namespace

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
    if (slot_count == 0 || slot_count > kMaximumSlots || slot_bytes == 0 ||
        slot_count > std::numeric_limits<std::size_t>::max() / slot_bytes) {
        return {};
    }
    std::shared_ptr<AudioPayloadPool> pool(new (std::nothrow) AudioPayloadPool(slot_count, slot_bytes));
    return pool != nullptr && pool->Initialize() ? pool : std::shared_ptr<AudioPayloadPool>{};
}

bool AudioPayloadPool::Initialize() noexcept {
    const std::size_t total_bytes = slot_count_ * slot_bytes_;
#ifdef ESP_PLATFORM
    bytes_ = static_cast<uint8_t*>(heap_caps_malloc(total_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#else
    bytes_ = new (std::nothrow) uint8_t[total_bytes];
#endif
    if (bytes_ == nullptr) return false;
    for (std::size_t word = 0; word < kBitmapWordCount; ++word) {
        const std::size_t word_start = word * kSlotsPerBitmapWord;
        const std::size_t slots_in_word =
            word_start < slot_count_ ? std::min(kSlotsPerBitmapWord, slot_count_ - word_start) : 0;
        available_slots_[word].store(slots_in_word == 0 ? 0 : AvailableMask(slots_in_word), std::memory_order_release);
    }
    return true;
}

AudioPayload AudioPayloadPool::TryAcquire() {
    for (std::size_t word = 0; word < kBitmapWordCount; ++word) {
        uint32_t available = available_slots_[word].load(std::memory_order_acquire);
        while (available != 0) {
            const uint32_t selected = available & (uint32_t{0} - available);
            const uint32_t next = available & ~selected;
            if (available_slots_[word].compare_exchange_weak(available, next, std::memory_order_acq_rel,
                                                             std::memory_order_acquire)) {
                const std::size_t in_use = in_use_.fetch_add(1, std::memory_order_relaxed) + 1;
                std::size_t high_watermark = high_watermark_.load(std::memory_order_relaxed);
                while (in_use > high_watermark &&
                       !high_watermark_.compare_exchange_weak(high_watermark, in_use, std::memory_order_relaxed,
                                                              std::memory_order_relaxed)) {
                }
                const std::size_t slot =
                    word * kSlotsPerBitmapWord + static_cast<std::size_t>(std::countr_zero(selected));
                return AudioPayload(shared_from_this(), slot, bytes_ + slot * slot_bytes_, slot_bytes_);
            }
        }
    }
    acquisition_failures_.fetch_add(1, std::memory_order_relaxed);
    return {};
}

void AudioPayloadPool::Release(std::size_t slot) noexcept {
    if (slot >= slot_count_) return;
    const std::size_t word = slot / kSlotsPerBitmapWord;
    const uint32_t slot_bit = uint32_t{1} << (slot % kSlotsPerBitmapWord);
    const uint32_t previous = available_slots_[word].fetch_or(slot_bit, std::memory_order_release);
    if ((previous & slot_bit) == 0) {
        in_use_.fetch_sub(1, std::memory_order_relaxed);
    }
}

std::size_t AudioPayloadPool::acquisition_failures() const {
    return acquisition_failures_.load(std::memory_order_relaxed);
}

std::size_t AudioPayloadPool::in_use() const { return in_use_.load(std::memory_order_relaxed); }

std::size_t AudioPayloadPool::high_watermark() const { return high_watermark_.load(std::memory_order_relaxed); }

}  // namespace voicelife::voice
