#include "scada/common/status.h"

namespace scada {

std::string_view to_string(StatusCode code) noexcept {
    switch (code) {
        case StatusCode::OK:
            return "OK";
        case StatusCode::INVALID_ARGUMENT:
            return "INVALID_ARGUMENT";
        case StatusCode::OUT_OF_RANGE:
            return "OUT_OF_RANGE";
        case StatusCode::NOT_FOUND:
            return "NOT_FOUND";
        case StatusCode::ALREADY_EXISTS:
            return "ALREADY_EXISTS";
        case StatusCode::PERMISSION_DENIED:
            return "PERMISSION_DENIED";
        case StatusCode::FAILED_PRECONDITION:
            return "FAILED_PRECONDITION";
        case StatusCode::RESOURCE_EXHAUSTED:
            return "RESOURCE_EXHAUSTED";
        case StatusCode::UNAVAILABLE:
            return "UNAVAILABLE";
        case StatusCode::TIMEOUT:
            return "TIMEOUT";
        case StatusCode::IO_ERROR:
            return "IO_ERROR";
        case StatusCode::DATA_CORRUPT:
            return "DATA_CORRUPT";
        case StatusCode::UNSUPPORTED:
            return "UNSUPPORTED";
        case StatusCode::INTERNAL:
            return "INTERNAL";
    }
    // Reached only for a value that is not an enumerator, for example one
    // read from outside and not validated.
    return "UNKNOWN";
}

}  // namespace scada
