#include "scada/common/mapped_region.h"

#include "scada/common/result.h"
#include "scada/common/status.h"

#include <sys/mman.h>

#include <cstddef>
#include <span>
#include <utility>

namespace scada {

Result<MappedRegion> MappedRegion::map(int descriptor, std::size_t size,
                                       MapAccess access) noexcept {
    if (descriptor < 0 || size == 0) {
        return Status(StatusCode::INVALID_ARGUMENT,
                      "no file or no size to map");
    }
    const int protection =
        access == MapAccess::READ_WRITE ? PROT_READ | PROT_WRITE : PROT_READ;
    void* const address =
        ::mmap(nullptr, size, protection, MAP_SHARED, descriptor, 0);
    // NOLINTNEXTLINE(scada-banned,cppcoreguidelines-pro-type-cstyle-cast,cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr): MAP_FAILED is a macro of the operating system that casts -1
    if (address == MAP_FAILED) {
        return Status(StatusCode::IO_ERROR, "cannot map the file");
    }
    // The one place where the address the operating system returned becomes
    // a span of bytes; everything else works on the span.
    return MappedRegion(
        std::span<std::byte>(static_cast<std::byte*>(address), size), access);
}

MappedRegion::~MappedRegion() {
    reset();
}

MappedRegion::MappedRegion(MappedRegion&& other) noexcept
    : bytes_(std::exchange(other.bytes_, {})), access_(other.access_) {}

MappedRegion& MappedRegion::operator=(MappedRegion&& other) noexcept {
    if (this != &other) {
        reset();
        bytes_ = std::exchange(other.bytes_, {});
        access_ = other.access_;
    }
    return *this;
}

void MappedRegion::reset() noexcept {
    if (!bytes_.empty()) {
        // An error is ignored: it can only mean the range was not mapped,
        // and there is nothing a caller could do.
        static_cast<void>(::munmap(bytes_.data(), bytes_.size()));
        bytes_ = {};
    }
}

}  // namespace scada
