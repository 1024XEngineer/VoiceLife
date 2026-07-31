#pragma once

#include <cstdint>
#include <string>

namespace voicelife {

class VoiceLifeService;

struct ImHttpResponse {
    bool transport_ok = false;
    int status_code = 0;
    std::string body;
    std::string error;
};

class ImTransport {
public:
    virtual ~ImTransport() = default;
    virtual ImHttpResponse Request(const std::string& method, const std::string& path,
                                   const std::string& body) = 0;
};

// Synchronizes PCB-owned reminders with the PR #85 Gateway. The class is
// deliberately transport-agnostic so the host tests can exercise retries and
// idempotency without an ESP network stack.
class VoiceLifeImSync final {
public:
    struct Config {
        std::string device_id;
        int poll_seconds = 5;
    };

    VoiceLifeImSync(VoiceLifeService* service, ImTransport* transport, Config config);

    // One bounded synchronization pass. It never throws and never calls into
    // the audio path. A false return means the pass encountered a transport or
    // protocol error; a later pass may retry it.
    bool PollOnce();

    const std::string& LastError() const { return last_error_; }
    uint32_t ConsecutiveFailures() const { return consecutive_failures_; }
    void ResetFailures() { consecutive_failures_ = 0; }

private:
    VoiceLifeService* service_ = nullptr;
    ImTransport* transport_ = nullptr;
    Config config_;
    std::string last_error_;
    uint32_t consecutive_failures_ = 0;

    bool ReportDue(const std::string& payload);
    bool PullAndApplyAction();
    bool AckAction(const std::string& action_id, const std::string& result_json,
                   bool ok, const std::string& error);
    bool ParseOkResponse(const ImHttpResponse& response, bool* ok, std::string* message) const;
    void Fail(const std::string& message);
};

}  // namespace voicelife
