// Fuzz target for the retain file parser (contract, section 12; decision
// D-45, D-59). A retain file is outside input to every process that loads
// it. A file that parses must encode back to the same bytes in the version
// it was read in, so that what the parser accepts is exactly what the
// format allows; and a file of version 1 must encode as version 2 to the
// same values.

#include "retain_file.h"

#include "scada/common/panic.h"
#include "scada/common/result.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

// NOLINTNEXTLINE(readability-identifier-naming): the name is set by libFuzzer
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    const std::span<const std::uint8_t> input(data, size);
    std::vector<std::byte> bytes(input.size());
    std::ranges::transform(input, bytes.begin(),
                           [](std::uint8_t byte) { return std::byte{byte}; });
    std::string detail;
    const scada::Result<scada::tag::RetainFile> file =
        scada::tag::decode_retain_file(bytes, detail);
    if (!file.ok()) {
        return 0;
    }
    const bool old_format =
        file.value().version == scada::tag::RETAIN_FORMAT_VERSION_1;
    const scada::Result<std::vector<std::byte>> again =
        old_format ? scada::tag::encode_retain_file_v1(file.value(), detail)
                   : scada::tag::encode_retain_file(file.value(), detail);
    if (!again.ok() || again.value() != bytes) {
        scada::panic("a retain file that parses does not encode to its bytes");
    }
    if (old_format) {
        // What the writer would make of it, read back.
        const scada::Result<std::vector<std::byte>> written =
            scada::tag::encode_retain_file(file.value(), detail);
        const scada::Result<scada::tag::RetainFile> read =
            written.ok()
                ? scada::tag::decode_retain_file(written.value(), detail)
                : scada::Result<scada::tag::RetainFile>(written.status());
        if (!read.ok() || read.value().values != file.value().values) {
            scada::panic("a retain file of version 1 does not encode as 2");
        }
    }
    return 0;
}
