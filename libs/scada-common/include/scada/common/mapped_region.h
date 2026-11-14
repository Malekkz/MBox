#ifndef SCADA_COMMON_MAPPED_REGION_H
#define SCADA_COMMON_MAPPED_REGION_H

#include "scada/common/result.h"

#include <cstddef>
#include <span>

namespace scada {

// How a file is mapped.
enum class MapAccess : bool { READ_ONLY, READ_WRITE };

// Owns one shared mapping of a file and unmaps it on destruction (coding
// standard, section 3). Move-only. A MappedRegion that holds no mapping is
// "invalid" and `bytes()` is empty.
class MappedRegion {
public:
    // Holds no mapping.
    MappedRegion() noexcept = default;

    // Maps the first `size` bytes of the file open as `descriptor`, shared
    // with every other process that maps the file. The descriptor is
    // borrowed and may be closed afterwards; the mapping stays.
    //
    // Fails with INVALID_ARGUMENT if `descriptor` is negative or `size` is 0,
    // and with IO_ERROR if the operating system refuses the mapping. The
    // caller makes sure the file is at least `size` bytes long: touching a
    // page beyond the end of the file ends the process with a signal.
    [[nodiscard]] static Result<MappedRegion> map(int descriptor,
                                                  std::size_t size,
                                                  MapAccess access) noexcept;

    ~MappedRegion();

    MappedRegion(MappedRegion&& other) noexcept;
    MappedRegion& operator=(MappedRegion&& other) noexcept;
    MappedRegion(const MappedRegion&) = delete;
    MappedRegion& operator=(const MappedRegion&) = delete;

    // True if a mapping is held.
    [[nodiscard]] bool valid() const noexcept { return !bytes_.empty(); }

    // True if the mapping may be written.
    [[nodiscard]] bool writable() const noexcept {
        return access_ == MapAccess::READ_WRITE;
    }

    // The mapped bytes; empty if invalid. The span is valid until this
    // object is destroyed, reset or moved from. Its start is aligned to a
    // page. Storing through it is allowed only if `writable()`: a store into
    // a read-only mapping ends the process with a signal. The bytes are
    // shared with other processes and can change at any time.
    [[nodiscard]] std::span<std::byte> bytes() const noexcept { return bytes_; }

    // Removes the mapping, if any.
    void reset() noexcept;

private:
    MappedRegion(std::span<std::byte> bytes, MapAccess access) noexcept
        : bytes_(bytes), access_(access) {}

    // The mapping itself, which this object owns.
    std::span<std::byte> bytes_;
    MapAccess access_ = MapAccess::READ_ONLY;
};

}  // namespace scada

#endif  // SCADA_COMMON_MAPPED_REGION_H
