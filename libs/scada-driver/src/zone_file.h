#ifndef SCADA_DRIVER_SRC_ZONE_FILE_H
#define SCADA_DRIVER_SRC_ZONE_FILE_H

// Reading a zone file beneath the zone directory (decision D-130; coding
// standard 10: a path is built from a validated name under a fixed
// directory, and the resolved path is checked to stay inside it).

#include "scada/common/result.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scada::driver::detail {

// How the file is opened beneath the directory.
enum class BeneathMethod : std::uint8_t {
    // openat2 with RESOLVE_BENEATH (Linux 5.6; the minimum kernel is 5.16),
    // falling back to RESOLVED where the call is not available (ENOSYS).
    AUTOMATIC = 1,
    // The path resolved in user space, checked to lie inside the directory,
    // then opened without following a link (for tests of the fallback).
    RESOLVED,
};

// Reads the file `zone_name` (a name `is_zone_name` accepts) beneath
// `directory`, an absolute path. A link inside the directory is followed
// while its target stays beneath it; one that leads out is refused with
// PERMISSION_DENIED. Fails with NOT_FOUND for no such file, DATA_CORRUPT for
// a file that is not a regular file or is larger than TZIF_MAX_BYTES,
// IO_ERROR otherwise. Opens with O_NONBLOCK, so that a FIFO never blocks.
[[nodiscard]] Result<std::vector<std::byte>> read_zone_file_beneath(
    const std::string& directory, std::string_view zone_name,
    BeneathMethod method);

}  // namespace scada::driver::detail

#endif  // SCADA_DRIVER_SRC_ZONE_FILE_H
