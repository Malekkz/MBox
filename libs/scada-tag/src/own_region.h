#ifndef SCADA_TAG_OWN_REGION_H
#define SCADA_TAG_OWN_REGION_H

// A region the calling process writes: found and reused, or created
// (docs/contracts/tag-database.md, sections 3.2 and 11.2, step 3). Private
// to libscada-tag.

#include "region_file.h"

#include "scada/common/fd.h"
#include "scada/common/mapped_region.h"
#include "scada/common/result.h"

#include <cstddef>
#include <functional>
#include <span>
#include <string>

namespace scada::tag {

// A region of the calling process, mapped read-write.
struct OwnRegion {
    Fd file;  // holds the lock, if one was asked for
    MappedRegion memory;
    // True if the region existed and was reused; false if it was created.
    bool reused = false;
};

// What `attach_own_region` is to find or create.
struct OwnRegionSpec {
    std::string name;  // a region name built from validated names
    RegionIdentity identity;
    // True for the `proc` region: the file is locked exclusively without
    // waiting (section 10), a new one before it gets its name.
    bool lock = false;
    // Called once with the whole file of a new region, after its
    // RegionHeader was written and before it is published. The rest of the
    // file is zero. Empty: nothing more to write.
    std::function<void(std::span<std::byte>)> initialise;
    // Called with the whole file of an existing region that passed every
    // check of section 3.1. False if what follows the RegionHeader is not
    // what this generation needs; the region is then removed and created
    // again. Empty: always usable. It must not write.
    std::function<bool(std::span<std::byte>)> usable;
};

// Attaches to the region `spec` describes in the generation directory open
// as `dir_fd`:
// - a region that exists with the right owner, size and header, is ready
//   and is `usable`, is reused as it is;
// - an entry of that name owned by `spec.identity.uid` that is not such a
//   region is removed and the region is created;
// - an absent region is created (section 3.2), after the caller's stale
//   temporary files were removed.
//
// ALREADY_EXISTS if `spec.lock` and another process holds the lock;
// PERMISSION_DENIED if the entry belongs to another user, and the entry is
// left for scada-tagd to remove; RESOURCE_EXHAUSTED if the file system has
// no room; IO_ERROR otherwise. On an error `detail` says what was found.
[[nodiscard]] Result<OwnRegion> attach_own_region(int dir_fd,
                                                  const OwnRegionSpec& spec,
                                                  std::string& detail);

}  // namespace scada::tag

#endif  // SCADA_TAG_OWN_REGION_H
