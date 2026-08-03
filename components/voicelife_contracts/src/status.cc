#include "voicelife/contracts/status.h"

namespace voicelife {

const char* ErrorCodeName(ErrorCode code) {
    switch (code) {
        case ErrorCode::kNone:
            return "none";
        case ErrorCode::kInvalidArgument:
            return "invalid_argument";
        case ErrorCode::kNotFound:
            return "not_found";
        case ErrorCode::kAlreadyExists:
            return "already_exists";
        case ErrorCode::kConflict:
            return "conflict";
        case ErrorCode::kUnavailable:
            return "unavailable";
        case ErrorCode::kInternal:
            return "internal";
    }
    return "unknown";
}

}  // namespace voicelife
