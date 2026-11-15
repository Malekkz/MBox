#ifndef SCADA_CTL_DATABASE_H
#define SCADA_CTL_DATABASE_H

// scadactl's way into the tag database: an observer context of
// libscada-tag (tag database contract, section 11.4), which reads and
// never writes.

#include "scada/ctl/arguments.h"
#include "scada/ctl/pattern.h"
#include "scada/ctl/view.h"
#include "scada/tag/scada_tag.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace scada::ctl {

// A tag found in the tag directory and resolved in the context.
struct TagRef {
    std::string name;
    std::string owner;
    std::uint16_t owner_index = 0;
    std::uint32_t handle = SCADA_INVALID_HANDLE;
    std::uint8_t type = 0;
};

// An observer context of the database the arguments name. Detached when
// destroyed.
class Database {
public:
    // Observes the database of `args` (--base-dir, --tagd-uid). Nothing if
    // that fails; `detail` then says why, in one sentence for the user.
    [[nodiscard]] static std::unique_ptr<Database> open(const Arguments& args,
                                                        std::string& detail);

    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&&) = delete;
    Database& operator=(Database&&) = delete;

    // Detaches and observes again, after a generation change, up to a few
    // times while the generation is changing. False, with `detail`, if no
    // generation could be observed; the object then holds no context and
    // must not be used again.
    [[nodiscard]] bool reopen(std::string& detail);

    // scada_tag_status: looks again for missing regions and reports a
    // generation change as SCADA_E_RETIRING. Call it about once a second.
    [[nodiscard]] int status(scada_status& out);

    // Every tag whose name matches `pattern`, resolved, sorted by name.
    [[nodiscard]] std::vector<TagRef> find(const TagPattern& pattern);

    // The tag `name`, resolved, or nothing if there is none.
    [[nodiscard]] std::optional<TagRef> find_one(const std::string& name);

    // Reads `tag` now.
    [[nodiscard]] TagRow read(const TagRef& tag);

    // What `scadactl status` shows: the generation, every process (each
    // resolved, so that its region is mapped) and every owner.
    [[nodiscard]] StatusView status_view();

    [[nodiscard]] scada_ctx* ctx() const noexcept { return ctx_; }

private:
    // Only `open` can name it, so only `open` can construct.
    struct Key {
        explicit Key() = default;
    };

public:
    // For `open` alone: takes over `ctx`.
    Database(Key /*key*/, std::string base_dir, std::int64_t tagd_uid,
             scada_ctx* ctx)
        : base_dir_(std::move(base_dir)), tagd_uid_(tagd_uid), ctx_(ctx) {}

private:
    // Observes once; the return code of scada_tag_observe_opts.
    [[nodiscard]] static int observe(const std::string& base_dir,
                                     std::int64_t tagd_uid, scada_ctx*& ctx);

    std::string base_dir_;  // empty: the default of the library
    std::int64_t tagd_uid_ = -1;
    scada_ctx* ctx_ = nullptr;
};

// The sentence for the user that says why observing failed with `code`
// (and `os_error`, the errno of SCADA_E_IO), for the database in
// `base_dir` (empty: the default).
[[nodiscard]] std::string describe_observe_error(int code, int os_error,
                                                 const std::string& base_dir);

}  // namespace scada::ctl

#endif  // SCADA_CTL_DATABASE_H
