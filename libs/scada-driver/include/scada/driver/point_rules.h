#ifndef SCADA_DRIVER_POINT_RULES_H
#define SCADA_DRIVER_POINT_RULES_H

// The compiled form of a point's common keys: what the framework applies to
// a raw sample after decode (driver interface contract 5.1 to 5.8; the keys
// are those of configuration schemas contract 4.7, 4.9, 5.3 to 5.5).
//
// Rules are built when a configuration is compiled and do not change while
// it is in use. Every number in them is resolved: a marker is a pattern of
// the raw width, a bit number counts from the least significant bit (the
// protocol applies its `bit_order`), a companion is a slot of the point's
// companion list. `validate_point_rules` checks the fields the processing
// depends on; the compile step refuses rules it rejects, and the processing
// writes BAD for any that reach it anyway, so that no rule value can cause
// undefined behaviour.

#include "scada/common/status.h"
#include "scada/driver/raw_value.h"
#include "scada/driver/tag_value.h"
#include "scada/driver/zone_table.h"
#include "scada/tag/types.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace scada::driver {

// A quality with the flags that go with it.
struct QualityMark {
    tag::Quality quality = tag::Quality::GOOD;
    std::uint16_t flags = 0;
};

// The flags the configuration may give a value: the flags of an invalid
// marker with action `bad` and of a `quality_from` entry (CS 4.7). Every
// configured mark is masked to these. SUBSTITUTED is not among them: the
// configuration gives it only through the `substitute` action (contract
// 5.2, R-6; review R2 of #157).
inline constexpr std::uint16_t CONFIG_FLAGS_MASK =
    tag::to_raw(tag::QualityFlag::OUT_OF_RANGE) |
    tag::to_raw(tag::QualityFlag::OVERFLOW) |
    tag::to_raw(tag::QualityFlag::BLOCKED) |
    tag::to_raw(tag::QualityFlag::TEST);

// The flags a protocol's mark may give a value: its mapping of a NO_VALUE
// or DEVICE_INVALID answer (driver interface contract 5.2, table of
// outcomes). Every protocol mark is masked to these. SUBSTITUTED for a
// value the device reports as forced is room only: no protocol carries a
// value with a mark yet, and through NO_VALUE or DEVICE_INVALID the flag
// would land on a value the device did not give (review R3 of #157). STALE,
// COMM_FAIL, NOT_INIT, LOCAL_TIME and TIME_UNCERTAIN are set by the
// framework itself, OWNER_DOWN is never stored and RESTORED is the
// library's (tag database contract 6, 6.2, 6.3).
inline constexpr std::uint16_t DEVICE_FLAGS_MASK =
    tag::to_raw(tag::QualityFlag::OUT_OF_RANGE) |
    tag::to_raw(tag::QualityFlag::OVERFLOW) |
    tag::to_raw(tag::QualityFlag::BLOCKED) |
    tag::to_raw(tag::QualityFlag::TEST);

// What a matching invalid marker, or `on_invalid`, does (CS 4.7).
enum class InvalidAction : std::uint8_t {
    BAD = 1,         // BAD with the marker's flags
    KEEP_LAST = 2,   // the last valid value, UNCERTAIN and STALE, BAD after
                     // `max_age`
    SUBSTITUTE = 3,  // the configured value, UNCERTAIN with SUBSTITUTED
};

// One entry of `invalid_values` (CS 4.7), with its action resolved.
struct InvalidMarker {
    std::uint64_t pattern = 0;  // in the raw width; ignored when `any_nan`
    bool any_nan = false;       // `nan`: any NaN of a floating raw type
    InvalidAction action = InvalidAction::BAD;
    std::uint16_t flags = 0;  // masked to CONFIG_FLAGS_MASK
    // For SUBSTITUTE, in the tag's type; never a STRING (a marker never
    // matches a string), so it borrows nothing.
    TagValue substitute{};
};

// `exponent_from` (CS 4.7): the raw integer of a companion is a power of 10.
struct ExponentRule {
    std::uint8_t companion = 0;  // slot in PointRules::companions
    bool divide = false;         // `direction: divide`
};

// The lowest and highest exponent CS 4.7 allows.
inline constexpr std::int64_t EXPONENT_MIN = -20;
inline constexpr std::int64_t EXPONENT_MAX = 20;

// `scale` (CS 4.7). The binary64 fields `gain` and `offset` are always
// filled; the integer fields `gain_i` and `offset_i` in addition when
// `integer` is true.
struct ScaleRule {
    enum class Form : std::uint8_t { GAIN = 1, DIVIDE_BY = 2 };
    Form form = Form::GAIN;
    // True when `gain` (or `divide_by`) and `offset` were written as
    // integers (CS 1.2): the integer forms are then exact.
    bool integer = false;
    std::int64_t gain_i = 1;
    std::int64_t offset_i = 0;
    std::uint64_t divide_by = 1;  // positive
    double gain = 1.0;
    double offset = 0.0;
};

// True when `scale` is an integer scaling an INT64 or UINT64 tag may have:
// an integer `gain` and `offset`, or `divide_by: 1` with an integer
// `offset` (CF-517, decision D-133).
[[nodiscard]] bool is_integer_scaling(const ScaleRule& scale) noexcept;

// `raw_range` (CS 4.7), on the raw number.
struct RawRange {
    double min = 0.0;
    double max = 0.0;
};

// `kind: counter` (CS 5.3).
struct CounterRule {
    enum class OnDecrease : std::uint8_t { ACCEPT = 1, HOLD = 2, RESET = 3 };
    std::uint64_t limit = 0;  // the largest raw value
    bool saturate = false;    // `at_limit: saturate`; otherwise `wrap`
    OnDecrease on_decrease = OnDecrease::ACCEPT;
};

// Double-point codes (contract 5.4).
enum class DoublePoint : std::uint8_t {
    INTERMEDIATE = 0,
    OPEN = 1,
    CLOSED = 2,
    BAD_STATE = 3,
};

// `kind: double_point` with its `states` map (CS 5.3, annex A.2). The
// compile step fills the default map of the bit shapes.
struct DoublePointRule {
    struct Entry {
        std::int64_t raw = 0;
        DoublePoint code = DoublePoint::BAD_STATE;
    };
    std::vector<Entry> states;  // sorted by `raw`, no duplicates
};

// The highest bit number of a raw value (CS 5.4: bit numbers 0 to 63).
inline constexpr std::uint8_t MAX_BIT_NUMBER = 63;

// One entry of `quality_from` (CS 4.7).
struct QualityRule {
    struct Entry {
        std::int64_t key = 0;
        QualityMark mark;  // flags masked to CONFIG_FLAGS_MASK
    };
    std::uint8_t companion = 0;  // slot; ignored when `self`
    bool self = false;           // the point's own raw word
    bool has_field = false;      // `bit` (lo == hi) or `field`
    std::uint8_t lo = 0;         // lo <= hi <= MAX_BIT_NUMBER
    std::uint8_t hi = 0;
    std::vector<Entry> entries;  // sorted by `key`, no duplicates
    // `default`; a map without one gives BAD for a value it does not list
    // (decision D-131).
    QualityMark otherwise{.quality = tag::Quality::BAD};
};

// One `health` rule that applies to this point (CS 4.9).
struct HealthRule {
    std::uint8_t companion = 0;  // slot of the health point
    bool has_field = false;      // `{ field, equals }`
    std::uint8_t lo = 0;
    std::uint8_t hi = 0;
    std::vector<std::int64_t> good;  // `good_when`, sorted
    bool comm_fail = false;
};

// The most bits one point may name (CS 5.4: bit numbers 0 to 63).
inline constexpr std::size_t MAX_POINT_BITS = 64;

// One named bit of `bits` (CS 5.4), written to its own BOOL tag.
struct BitRule {
    std::uint8_t bit = 0;  // from the least significant bit, at most 63
    bool invert = false;
};

// `device_time` (CS 4.6) of the values a point reads as times.
struct DeviceTimeRule {
    bool local = false;  // `zone: local`
    // The zone's table, borrowed: one per zone, owned by the compiled
    // configuration that holds these rules and destroyed with it (decision
    // D-130). Null with `local` is a rule the compile step refuses; the
    // processing then marks every time uncertain.
    const ZoneTable* zone = nullptr;
    bool synced = false;  // `trust: synced`
};

// Everything the framework applies to one point's samples.
struct PointRules {
    tag::TagType tag_type = tag::TagType::INT64;
    // The raw type's kind (CS 4.7 `type`), for the checks of
    // `validate_point_rules`; the processing reads the kind of each sample.
    RawKind raw_kind = RawKind::UNSIGNED;
    bool has_tag = true;  // `tag: false` writes only the bits
    bool invert = false;  // BOOL points: the device's 1 is `false`

    // The points this one depends on, by their index in the device (CS 4.7,
    // "Companions"). The rules below name them by slot.
    std::vector<std::uint32_t> companions;

    std::vector<InvalidMarker> markers;
    std::optional<ExponentRule> exponent;
    std::optional<ScaleRule> scale;
    std::optional<RawRange> raw_range;
    std::optional<CounterRule> counter;
    std::optional<DoublePointRule> double_point;
    bool has_enum = false;                 // `enum`: an INT64 tag
    std::vector<std::int64_t> enum_codes;  // sorted, for `has_enum`
    std::vector<QualityRule> quality_rules;
    std::vector<HealthRule> health;
    std::vector<BitRule> bits;
    DeviceTimeRule device_time;                  // this point's own TIME values
    std::optional<std::uint8_t> time_companion;  // `source_time_from`
    DeviceTimeRule companion_time;  // the companion's `device_time`

    // `max_age` in nanoseconds; 0 when the point has none.
    std::int64_t max_age_ns = 0;
};

// Checks what the processing relies on: companion slots within
// `companions`, bit and field numbers within 0 to 63 with lo <= hi, at most
// MAX_POINT_BITS bits, sorted and unique tables, a positive `divide_by`, an
// enumeration only on an INT64 tag and without a scale, an integer tag only
// with an integer scaling and no exponent (CF-517), no floating raw type on
// an INT64 or UINT64 value other than an enumeration or a double point
// (CF-504, decision D-134), a zone table for every
// local device time. Returns INVALID_ARGUMENT naming the first failure.
[[nodiscard]] Status validate_point_rules(const PointRules& rules) noexcept;

}  // namespace scada::driver

#endif  // SCADA_DRIVER_POINT_RULES_H
