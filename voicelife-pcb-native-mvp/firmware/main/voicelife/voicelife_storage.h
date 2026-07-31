#pragma once

#include "voicelife_domain.h"

#include <mutex>
#include <string>

namespace voicelife {

class Storage {
public:
    virtual ~Storage() = default;
    virtual bool Initialize() = 0;
    virtual bool Load(State* state) = 0;
    virtual bool Save(const State& state) = 0;
};

// A small JSON journal on the dedicated SPIFFS partition. The service keeps a
// bounded in-memory state and rewrites the journal after each transaction;
// this is intentionally replaceable by SQLite without changing service APIs.
class FlashStorage final : public Storage {
public:
    FlashStorage() = default;
    ~FlashStorage() override;

    bool Initialize() override;
    bool Load(State* state) override;
    bool Save(const State& state) override;

private:
    bool initialized_ = false;
    std::mutex mutex_;
};

}  // namespace voicelife
