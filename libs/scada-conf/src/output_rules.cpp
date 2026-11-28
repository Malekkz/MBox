// Areas, outputs and their safe states (configuration schemas contract 7.1
// to 7.4 and 10.7; decision D-124, slice 7e-1 of #100).

#include "output_rules.h"

#include "output_values.h"
#include "point_types.h"
#include "scada/conf/diagnostic.h"
#include "scada/conf/expand.h"
#include "scada/conf/node.h"
#include "scada/conf/references.h"
#include "scada/conf/tree.h"
#include "tag_names.h"
#include "tree_reader.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace scada::conf {

namespace {

using detail::Reader;
using detail::Reporter;

constexpr std::uint16_t CW_AREA_NOT_USED = 701;
constexpr std::uint16_t CF_NOT_WRITABLE = 702;
constexpr std::uint16_t CF_TARGET_TWICE = 703;
constexpr std::uint16_t CF_DANGEROUS = 705;
constexpr std::uint16_t CF_ON_ONLY = 707;
constexpr std::uint16_t CF_CANNOT_WRITE = 708;
constexpr std::uint16_t CF_VALUE_TYPE = 713;
constexpr std::uint16_t CF_FEEDBACK_KIND = 715;
constexpr std::uint16_t CF_NVM_CYCLIC = 706;
constexpr std::uint16_t CF_NO_FEEDBACK = 709;
constexpr std::uint16_t CF_NO_SAFE_STATE = 710;
constexpr std::uint16_t CF_NO_INTERLOCKS = 711;
constexpr std::uint16_t CW_NO_INTERLOCK = 712;
constexpr std::uint16_t CF_REFRESH_OR_RELEASE = 714;
constexpr std::uint16_t CF_LOCAL_PANEL = 718;
constexpr std::uint16_t CF_NO_SAFE_ACTION = 719;
constexpr std::uint16_t CF_PULSE_DURATION = 720;
constexpr std::uint16_t CW_HOLDS_ON_LOSS = 721;
constexpr std::uint16_t CF_SAFE_WITH_HOLD = 722;
constexpr std::uint16_t CF_RELEASE_NOT_SETPOINT = 723;

// The fail-safe keys of 7.4 that are a plain action.
constexpr std::array<std::string_view, 2> PLAIN_ACTIONS = {
    "on_shutdown", "on_generation_change"};

// The kinds with no safe action (7.2): every fail-safe key is `hold`.
[[nodiscard]] bool has_no_safe_action(std::string_view kind) {
    return kind == "step" || kind == "pulse" || kind == "trigger";
}

// The kinds that switch (7.2, "remote switching outputs").
[[nodiscard]] bool is_switching(std::string_view kind) {
    return kind == "single" || kind == "double";
}

// The target of an output, as stage 5 reads it from its point.
struct Target {
    bool point = false;  // a point's own tag, of a device
    std::string_view value_type;
    std::string_view access;
    std::string_view protocol;
    bool dangerous = false;
    bool nvm = false;
    // Of a point: its address, the raw type a write writes (empty for one
    // these rules do not count), its scaling and write encoding (A.5).
    NodeId address = 0;
    std::string raw;
    // The raw type a write writes: `write.type` where given, else `raw`
    // (A.5; M4 of the review of #185).
    std::string write_raw;
    Scaling scaling;
    bool mask = false;    // `write.mask`
    bool on_off = false;  // `write.on` or `write.off`
    // Any write encoding: `mask`, `on`/`off`, `values` or `value` (A.5).
    bool encoded = false;
    bool write_on = false;     // `write.on`
    bool write_off = false;    // `write.off`
    bool fixed_value = false;  // `write.value`: one value, whatever is asked
    // The commanded values of `write.values`, when given.
    bool has_values = false;
    std::vector<std::string_view> value_keys;
    // The codes of the point's enumeration (5.5), when it has one.
    bool has_enum = false;
    std::vector<std::string_view> enum_codes;
    bool double_point = false;
    bool at_start_safe = false;  // `io`: `at_start: safe`
    // Why the protocol cannot write it (CF-708), empty if it can.
    std::string unwritable;
};

[[nodiscard]] bool is_number_type(std::string_view value_type) {
    return value_type == "int64" || value_type == "uint64" ||
           value_type == "float64";
}

// The fail-safe actions of an output, each as written or its default (7.2,
// 7.4; ruling Q7 of D-124): what the writer of the target does on an
// orderly stop, on control lost and on a generation change.
struct FailSafe {
    std::string_view on_shutdown;
    bool shutdown_given = false;
    std::string_view on_control_loss;
    bool loss_given = false;
    std::string_view on_generation_change;
    bool generation_given = false;
};

class Checker {
public:
    Checker(const TagNames& tags, const ExpandedTree& tree,
            std::span<const CheckedFile> files, Diagnostics& diagnostics)
        : tags_(&tags),
          files_(files),
          expanded_(tree.document),
          reporter_(tree, diagnostics) {}

    // The readers point into the tree: not copied, not moved.
    Checker(const Checker&) = delete;
    Checker& operator=(const Checker&) = delete;
    Checker(Checker&&) = delete;
    Checker& operator=(Checker&&) = delete;
    ~Checker() = default;

    void run() {
        readers_.reserve(files_.size());
        for (const CheckedFile& file : files_) {
            readers_.emplace_back(*file.document);
        }
        read_enumerations();
        for (std::size_t i = 0; i < files_.size(); ++i) {
            if (files_[i].source->path_class.type == FileType::OUTPUTS) {
                const Reader& reader = readers_[i];
                for (const NodeId output :
                     reader.items(reader.document().root(), "outputs")) {
                    // An output given twice (CF-302 on its tags) is checked
                    // once, by its name, as a device is (CF-303).
                    const std::optional<NodeId> name =
                        reader.scalar(output, "name");
                    if (name &&
                        outputs_.insert(reader.document().text(*name)).second) {
                        check_output(reader, output);
                    }
                }
            }
        }
        check_areas();
        reporter_.flush();
    }

private:
    void error(const Reader& reader, NodeId node, std::uint16_t code,
               std::string message) {
        reporter_.error(reader.document(), node, code, std::move(message));
    }

    void warning(const Reader& reader, NodeId node, std::uint16_t code,
                 std::string message) {
        reporter_.warning(reader.document(), node, code, std::move(message));
    }

    // --- The target ------------------------------------------------------

    // The target named at `node`, read from its point; nothing when the
    // tags are not whole or name no such tag (CF-310 of the references).
    [[nodiscard]] std::optional<Target> target_of(const Reader& reader,
                                                  NodeId node) const {
        if (!tags_->complete()) {
            return std::nullopt;
        }
        const std::optional<TagInfo> info =
            tags_->find(reader.document().text(node));
        if (!info) {
            return std::nullopt;
        }
        Target target;
        target.point = info->origin == TagOrigin::POINT && info->point != 0;
        target.value_type = info->value_type;
        target.protocol = info->protocol;
        if (target.point) {
            target.access = expanded_.text_of(info->point, "access");
            target.dangerous =
                expanded_.text_of(info->point, "dangerous") == "true";
            target.nvm = expanded_.text_of(info->point, "storage") == "nvm";
            device_ = info->device;
            read_point(info->point, target);
        }
        return target;
    }

    // The address, raw type, scaling and encoding of the target's point, and
    // why its protocol cannot write it (CF-708, ruling Q3 of D-124).
    void read_point(NodeId point, Target& target) const {
        const Document& document = expanded_.document();
        target.double_point =
            expanded_.text_of(point, "kind") == "double_point";
        if (const std::optional<NodeId> scale =
                expanded_.child(point, "scale")) {
            target.scaling =
                Scaling{.gain = expanded_.text_of(*scale, "gain"),
                        .offset = expanded_.text_of(*scale, "offset"),
                        .divide_by = expanded_.text_of(*scale, "divide_by")};
        }
        if (expanded_.child(point, "exponent_from")) {
            target.unwritable = "a point with exponent_from";
        }
        read_enumeration(point, target);
        const std::optional<NodeId> address = expanded_.child(point, "address");
        if (!address || document.kind(*address) != NodeKind::MAP) {
            return;
        }
        target.address = *address;
        const std::optional<NodeId> write = expanded_.child(*address, "write");
        if (write) {
            read_write(*write, target);
        }
        if (target.protocol == "io") {
            target.at_start_safe =
                expanded_.text_of(*address, "at_start") == "safe";
            if (target.value_type == "bool") {
                target.raw = "bool";
                target.write_raw = "bool";
            }
            if (target.unwritable.empty() &&
                expanded_.text_of(*address, "direction") == "input") {
                target.unwritable = "an input line";
            }
            return;
        }
        if (target.protocol != "modbus") {
            return;
        }
        target.raw = modbus_raw_type(document, *address);
        const std::string_view written_type =
            write ? expanded_.text_of(*write, "type") : std::string_view();
        target.write_raw =
            written_type.empty() ? target.raw : std::string(written_type);
        if (!target.unwritable.empty()) {
            return;
        }
        const std::string_view table = expanded_.text_of(*address, "table");
        if (target.raw == "mod10k" || target.raw == "epoch" ||
            target.raw == "datetime") {
            target.unwritable = "raw type " + target.raw;
        } else if ((expanded_.child(*address, "bit") ||
                    expanded_.child(*address, "field")) &&
                   !target.mask) {
            target.unwritable = "a bit or field without write.mask";
        } else if ((table == "discrete" || table == "input") &&
                   !target.on_off) {
            target.unwritable = "table " + std::string(table) +
                                " without write.on or write.off";
        }
    }

    // The write encoding of a point (A.5).
    void read_write(NodeId write, Target& target) const {
        target.mask = expanded_.child(write, "mask").has_value();
        target.write_on = expanded_.child(write, "on").has_value();
        target.write_off = expanded_.child(write, "off").has_value();
        target.on_off = target.write_on || target.write_off;
        target.fixed_value = expanded_.child(write, "value").has_value();
        if (const std::optional<NodeId> values =
                expanded_.child(write, "values");
            values && expanded_.document().kind(*values) == NodeKind::MAP) {
            target.has_values = true;
            for (const Entry& entry : expanded_.document().entries(*values)) {
                target.value_keys.push_back(
                    expanded_.document().text(entry.key));
            }
        }
        target.encoded = target.mask || target.on_off || target.has_values ||
                         target.fixed_value;
    }

    // --- One output ------------------------------------------------------

    void check_output(const Reader& reader, NodeId output) {
        const std::optional<NodeId> name_node = reader.scalar(output, "name");
        if (!name_node) {
            return;  // CF-202 already
        }
        const std::string head =
            "output " + quote_text(reader.document().text(*name_node));
        const std::string_view kind = reader.text_of(output, "kind");
        std::optional<Target> target;
        if (const std::optional<NodeId> target_node =
                reader.scalar(output, "target")) {
            target = target_of(reader, *target_node);
            check_target(reader, *target_node, head, kind, target);
        }
        const bool dangerous = target && target->dangerous;
        if (!reader.child(output, "feedback") &&
            !reader.child(output, "no_feedback")) {
            error(reader, *name_node, CF_NO_FEEDBACK,
                  head + " needs feedback or no_feedback: true");
        }
        // Every output names its safe_state, a dangerous one too (7.2; M3
        // of the review of #185), where one other than `hold` is CF-705.
        if (!reader.child(output, "safe_state")) {
            error(reader, *name_node, CF_NO_SAFE_STATE,
                  head + " needs a safe_state");
        }
        const bool form_fits =
            dangerous || check_safe_state_form(reader, output, head, kind);
        check_interlocks(reader, output, *name_node, head, kind);
        check_refresh(reader, output, head, kind, target);
        check_actions(reader, output, head, kind, dangerous);
        check_loss_after(reader, output, head, kind, dangerous);
        if (const std::optional<NodeId> duration =
                reader.child(output, "duration");
            duration && kind == "pulse") {
            error(reader, *duration, CF_PULSE_DURATION,
                  head + ": the device must time the pulse");
        }
        check_control_loss_hold(reader, output, head, kind, target);
        // A target that is not a writable point of the output's kind is
        // CF-702 alone (S3 of the review of #185).
        if (target && writable(kind, *target)) {
            check_dangerous_flags(reader, output, head, *target);
            check_on_only(reader, output, head, kind, *target);
            check_safe_side(reader, output, head, kind, *target);
            check_values(reader, output, head, kind, *target, form_fits);
            check_native_select(reader, output, head, *target);
            check_defaults(reader, output, head, kind, *target);
        }
        check_feedback(reader, output, head, kind);
    }

    // The output's kind fits its target's type (interim rule (g) of D-124):
    // a switching kind needs a BOOL target, a double point for `double`, or
    // a write encoding; a `step` or `setpoint` needs a number.
    [[nodiscard]] static bool kind_fits(std::string_view kind,
                                        const Target& target) {
        const bool always_on = kind == "pulse" || kind == "trigger";
        // `write.value` writes one value whatever is asked: only for a
        // trigger or a pulse (A.5; ruling B1 of the delta check of #185).
        if (target.fixed_value) {
            return always_on;
        }
        // A `values` map writes only the commands it has keys for (N1 of
        // the delta check of #185): a map of true and false writes no code
        // of a double and no number, and a pulse's or trigger's map needs
        // `true`, the one command they send.
        if (target.has_values) {
            if (bool_values(target) &&
                (kind == "double" || kind == "setpoint" || kind == "step")) {
                return false;
            }
            if (always_on && !is_values_key(target, "true")) {
                return false;
            }
        }
        if (kind == "single" || always_on) {
            return target.value_type == "bool" || target.mask ||
                   target.on_off || bool_values(target);
        }
        if (kind == "double") {
            return target.double_point || target.on_off;
        }
        if (kind == "step" || kind == "setpoint") {
            return is_number_type(target.value_type);
        }
        return true;  // `latch`: reserved (CF-210)
    }

    // True if `target` is a writable point that an output of `kind` can
    // drive (CF-702).
    [[nodiscard]] static bool writable(std::string_view kind,
                                       const Target& target) {
        return target.point &&
               (target.access == "rw" || target.access == "wo") &&
               target.value_type != "string" && kind_fits(kind, target);
    }

    // CF-713 for a `safe_state` whose form its kind does not take (7.2; M1
    // of the review of #185): `off`, `on` or `hold` for `single` and
    // `double`, `{ value }`, `{ raw }` or `hold` for `setpoint`. A `step`,
    // `pulse` or `trigger` holds only: CF-719 of `check_actions`. True if
    // the form fits, or there is none.
    bool check_safe_state_form(const Reader& reader, NodeId output,
                               const std::string& head, std::string_view kind) {
        const std::optional<NodeId> safe_state =
            reader.child(output, "safe_state");
        if (!safe_state) {
            return true;
        }
        if (has_no_safe_action(kind)) {
            return true;  // CF-719 of check_actions; no value is checked
        }
        const Document& document = reader.document();
        const bool scalar = document.kind(*safe_state) == NodeKind::SCALAR;
        const std::string_view text = scalar ? document.text(*safe_state) : "";
        bool fits = true;
        if (is_switching(kind)) {
            fits = scalar && (text == "off" || text == "on" || text == "hold");
        } else if (kind == "setpoint") {
            fits = !scalar || text == "hold";
        }
        if (!fits) {
            refuse_value(reader, *safe_state, head, "safe_state");
        }
        return fits;
    }

    // True if `write.values` maps BOOL commands, as A.5's polarity
    // example `{ true: 0, false: 1 }` does: a switching command is written
    // through it whatever the register's type.
    [[nodiscard]] static bool bool_values(const Target& target) {
        return target.has_values &&
               std::ranges::all_of(target.value_keys, [](std::string_view key) {
                   return key == "true" || key == "false";
               });
    }

    // True if `value`, a commanded value, is a key of `write.values`.
    [[nodiscard]] static bool is_values_key(const Target& target,
                                            std::string_view value) {
        return std::ranges::any_of(
            target.value_keys, [value](std::string_view key) {
                return key == value ||
                       compare_numbers(value, key).value_or(1) == 0;
            });
    }

    // The side a switching output's safe_state commands (B1 of the delta
    // check of #185): a lone `write.on` or `write.off` writes one side only
    // (CF-707), and `write.values` must give the side a pattern (CF-713).
    void check_safe_side(const Reader& reader, NodeId output,
                         const std::string& head, std::string_view kind,
                         const Target& target) {
        const std::optional<NodeId> safe_state =
            reader.scalar(output, "safe_state");
        if (!is_switching(kind) || !safe_state) {
            return;
        }
        const std::string_view state = reader.document().text(*safe_state);
        if (state != "off" && state != "on") {
            return;
        }
        const bool wants_on = state == "on";
        if (target.on_off && !(wants_on ? target.write_on : target.write_off)) {
            error(reader, *safe_state, CF_ON_ONLY,
                  head + (wants_on ? " is off-only" : " is on-only"));
        } else if (target.has_values) {
            if (!is_values_key(target, command_of(kind, wants_on))) {
                refuse_value(reader, *safe_state, head, "safe_state");
            }
        }
    }

    // The command a switching output writes for on or off: a single's
    // true or false, a double's code 2 (closed) or 1 (open) (7.2).
    [[nodiscard]] static std::string_view command_of(std::string_view kind,
                                                     bool wants_on) {
        if (kind == "single") {
            return wants_on ? "true" : "false";
        }
        return wants_on ? "2" : "1";
    }

    // CF-702, CF-708 and CF-703.
    void check_target(const Reader& reader, NodeId node,
                      const std::string& head, std::string_view kind,
                      const std::optional<Target>& target) {
        const std::string_view tag = reader.document().text(node);
        if (target && !writable(kind, *target)) {
            error(reader, node, CF_NOT_WRITABLE,
                  head + ": target " + quote_text(tag) +
                      " is not a writable point");
        } else if (target && !target->unwritable.empty()) {
            error(reader, node, CF_CANNOT_WRITE,
                  head + ": the protocol cannot write " + target->unwritable);
        }
        const auto [first, added] = targets_.try_emplace(
            std::string(tag), std::string(head.substr(head.find(' ') + 1)));
        if (!added) {
            error(reader, node, CF_TARGET_TWICE,
                  "tag " + quote_text(tag) + " is the target of outputs " +
                      first->second + " and " +
                      head.substr(head.find(' ') + 1));
        }
    }

    // CF-711 and CW-712: every kind but `setpoint` lists its interlocks
    // (7.2, ruling O-21), an empty list when none applies.
    void check_interlocks(const Reader& reader, NodeId output, NodeId name,
                          const std::string& head, std::string_view kind) {
        if (kind == "setpoint") {
            return;
        }
        const std::optional<NodeId> interlocks =
            reader.child(output, "interlocks");
        if (!interlocks) {
            error(reader, name, CF_NO_INTERLOCKS,
                  head + " needs interlocks (an empty list if none applies)");
            return;
        }
        const Document& document = reader.document();
        const bool empty = document.kind(*interlocks) == NodeKind::SEQUENCE
                               ? document.items(*interlocks).empty()
                               : reader.items(*interlocks, "on").empty() &&
                                     reader.items(*interlocks, "off").empty();
        if (empty) {
            warning(reader, *interlocks, CW_NO_INTERLOCK,
                    head + " has no interlock");
        }
    }

    // CF-705, CF-706 and CF-714 for `refresh`.
    void check_refresh(const Reader& reader, NodeId output,
                       const std::string& head, std::string_view kind,
                       const std::optional<Target>& target) {
        const std::optional<NodeId> refresh = reader.child(output, "refresh");
        if (!refresh) {
            return;
        }
        if (kind != "setpoint") {
            error(reader, *refresh, CF_REFRESH_OR_RELEASE,
                  head + ": refresh is only for setpoint outputs");
        } else if (reader.text_of(output, "sbo") == "true") {
            error(reader, *refresh, CF_REFRESH_OR_RELEASE,
                  head + ": refresh cannot be used with sbo");
        }
        if (target && target->dangerous) {
            error(reader, *refresh, CF_DANGEROUS,
                  head + " is dangerous: it cannot be refreshed");
        } else if (target && target->nvm) {
            error(reader, *refresh, CF_NVM_CYCLIC,
                  head +
                      " writes non-volatile memory and cannot be written "
                      "cyclically");
        }
    }

    // The fail-safe keys written (7.4): each key's action and its node.
    [[nodiscard]] static std::vector<std::pair<std::string_view, NodeId>>
    actions_of(const Reader& reader, NodeId output) {
        std::vector<std::pair<std::string_view, NodeId>> actions;
        for (const std::string_view key : PLAIN_ACTIONS) {
            if (const std::optional<NodeId> action =
                    reader.scalar(output, key)) {
                actions.emplace_back(key, *action);
            }
        }
        if (const std::optional<NodeId> loss =
                reader.child(output, "on_control_loss")) {
            if (const std::optional<NodeId> action =
                    reader.scalar(*loss, "action")) {
                actions.emplace_back("on_control_loss", *action);
            }
        }
        return actions;
    }

    // CF-705, CF-714, CF-719, CF-722 and CF-723 for `safe_state` and the
    // fail-safe keys written.
    void check_actions(const Reader& reader, NodeId output,
                       const std::string& head, std::string_view kind,
                       bool dangerous) {
        const Document& document = reader.document();
        const std::optional<NodeId> safe_state =
            reader.child(output, "safe_state");
        const bool holds = safe_state &&
                           document.kind(*safe_state) == NodeKind::SCALAR &&
                           document.text(*safe_state) == "hold";
        if (safe_state && !holds) {
            if (dangerous) {
                error(reader, *safe_state, CF_DANGEROUS,
                      head + " is dangerous: its safe_state must be hold");
            } else if (has_no_safe_action(kind)) {
                no_safe_action(reader, *safe_state, head, kind);
            }
        }
        const std::optional<NodeId> limits = reader.child(output, "limits");
        const bool has_release = limits && reader.child(*limits, "release");
        for (const auto& [key, node] : actions_of(reader, output)) {
            const std::string_view action = document.text(node);
            if (action == "hold") {
                continue;
            }
            if (dangerous) {
                error(reader, node, CF_DANGEROUS,
                      head + " is dangerous: its " + std::string(key) +
                          " must be hold");
                continue;
            }
            if (has_no_safe_action(kind)) {
                no_safe_action(reader, node, head, kind);
                continue;
            }
            if (action == "release") {
                if (kind != "setpoint") {
                    error(reader, node, CF_RELEASE_NOT_SETPOINT,
                          head + ": release is only for setpoints");
                } else if (!has_release) {
                    error(reader, node, CF_REFRESH_OR_RELEASE,
                          head + ": release in " + std::string(key) +
                              " needs limits.release");
                }
            } else if (action == "safe" && holds) {
                error(
                    reader, node, CF_SAFE_WITH_HOLD,
                    head + ": action safe needs a safe_state other than hold");
            }
        }
    }

    // Interim rule (j) of D-124 (S1 of the review of #185 and of its delta
    // check): a written control-loss action other than hold says when. A
    // dangerous output and the kinds with no safe action hold (CF-705,
    // CF-719).
    void check_loss_after(const Reader& reader, NodeId output,
                          const std::string& head, std::string_view kind,
                          bool dangerous) {
        const std::optional<NodeId> loss =
            reader.child(output, "on_control_loss");
        const std::optional<NodeId> action =
            loss ? reader.scalar(*loss, "action") : std::nullopt;
        if (!loss || !action || dangerous || has_no_safe_action(kind) ||
            reader.document().text(*action) == "hold" ||
            reader.child(*loss, "after")) {
            return;
        }
        error(reader, *action, CF_REFRESH_OR_RELEASE,
              head + ": on_control_loss with action " +
                  std::string(reader.document().text(*action)) +
                  " needs after");
    }

    void no_safe_action(const Reader& reader, NodeId node,
                        const std::string& head, std::string_view kind) {
        error(reader, node, CF_NO_SAFE_ACTION,
              head + ": a " + std::string(kind) +
                  " output has no safe action; use hold");
    }

    // CW-721 (7.2, ruling O-12): a remote switching output whose written
    // `on_control_loss` holds never uses the safe state it names when
    // control is lost. The defaults never give it.
    void check_control_loss_hold(const Reader& reader, NodeId output,
                                 const std::string& head, std::string_view kind,
                                 const std::optional<Target>& target) {
        if (!target || target->protocol.empty() || target->protocol == "io" ||
            target->dangerous || !is_switching(kind)) {
            return;
        }
        const std::optional<NodeId> safe_state =
            reader.scalar(output, "safe_state");
        const std::optional<NodeId> loss =
            reader.child(output, "on_control_loss");
        if (!safe_state || !loss) {
            return;
        }
        const std::string_view state = reader.document().text(*safe_state);
        if ((state == "off" || state == "on") &&
            reader.text_of(*loss, "action") == "hold") {
            warning(reader, *loss, CW_HOLDS_ON_LOSS,
                    head +
                        " holds on control loss although its safe_state is " +
                        std::string(state));
        }
    }

    // --- The rulings of D-124
    // -------------------------------------------------

    // CF-705 (ruling Q2): a dangerous output always asks for a reason and
    // a re-authentication; writing `false` for either is refused.
    void check_dangerous_flags(const Reader& reader, NodeId output,
                               const std::string& head, const Target& target) {
        if (!target.dangerous) {
            return;
        }
        for (const std::string_view key : {"reason_required", "reauth"}) {
            if (const std::optional<NodeId> flag = reader.scalar(output, key);
                flag && reader.document().text(*flag) == "false") {
                error(reader, *flag, CF_DANGEROUS,
                      head + " is dangerous: its " + std::string(key) +
                          " must be true");
            }
        }
    }

    // CF-707 (ruling Q4): an on-only output, by `on_only: true` or a mask
    // target (A.5), has no off value; `on_only: false` cannot be given to a
    // `trigger`, a `pulse` or a mask target. A trigger's or pulse's
    // `safe_state` is CF-719's.
    void check_on_only(const Reader& reader, NodeId output,
                       const std::string& head, std::string_view kind,
                       const Target& target) {
        const std::optional<NodeId> on_only = reader.scalar(output, "on_only");
        const bool written_off =
            on_only && reader.document().text(*on_only) == "false";
        const bool always_on = kind == "trigger" || kind == "pulse";
        if (written_off && (always_on || target.mask)) {
            error(reader, *on_only, CF_ON_ONLY, head + " is on-only");
            return;
        }
        const bool on_only_output =
            !always_on &&
            ((on_only && reader.document().text(*on_only) == "true") ||
             target.mask);
        if (const std::optional<NodeId> safe_state =
                reader.scalar(output, "safe_state");
            on_only_output && safe_state &&
            reader.document().text(*safe_state) == "off") {
            error(reader, *safe_state, CF_ON_ONLY, head + " is on-only");
        }
    }

    // CF-715 (ruling Q5, interim rules (b) and (i)): the feedback a kind
    // takes.
    void check_feedback(const Reader& reader, NodeId output,
                        const std::string& head, std::string_view kind) {
        const std::optional<NodeId> feedback = reader.child(output, "feedback");
        if (!feedback || !tags_->complete()) {
            return;
        }
        if (const std::optional<std::string_view> needed =
                feedback_fault(reader, *feedback, kind)) {
            error(reader, *feedback, CF_FEEDBACK_KIND,
                  head + ": feedback must be " + std::string(*needed));
        }
    }

    // The tag a scalar at `node` names, if any.
    [[nodiscard]] std::optional<TagInfo> tag_at(
        const Reader& reader, std::optional<NodeId> node) const {
        return node ? tags_->find(reader.document().text(*node))
                    : std::optional<TagInfo>();
    }

    // What `feedback` of an output of `kind` must be, when it is not.
    [[nodiscard]] std::optional<std::string_view> feedback_fault(
        const Reader& reader, NodeId feedback, std::string_view kind) const {
        const NodeKind shape = reader.document().kind(feedback);
        const bool on_off =
            shape == NodeKind::MAP && reader.child(feedback, "on").has_value();
        if (on_off) {
            for (const std::string_view side : {"on", "off"}) {
                const std::optional<TagInfo> info =
                    tag_at(reader, reader.scalar(feedback, side));
                if (info && info->value_type != "bool") {
                    return "{ on, off } of two BOOL tags";
                }
            }
        }
        if (kind == "step") {
            return "none: a step output has no feedback yet";
        }
        const std::optional<TagInfo> plain =
            shape == NodeKind::SCALAR ? tag_at(reader, feedback) : std::nullopt;
        if (kind == "single" || kind == "pulse" || kind == "trigger") {
            const bool fits = shape == NodeKind::SCALAR
                                  ? !plain || plain->value_type == "bool"
                                  : on_off;
            return fits ? std::nullopt
                        : std::optional<std::string_view>(
                              "a BOOL tag or { on, off }");
        }
        if (kind == "double") {
            const bool fits = shape == NodeKind::SCALAR
                                  ? !plain || is_double_point(*plain)
                                  : on_off;
            return fits ? std::nullopt
                        : std::optional<std::string_view>(
                              "a double point or { on, off }");
        }
        if (kind == "setpoint") {
            return setpoint_feedback_fault(reader, feedback);
        }
        return std::nullopt;
    }

    [[nodiscard]] bool is_double_point(const TagInfo& info) const {
        return info.origin == TagOrigin::POINT && info.point != 0 &&
               expanded_.text_of(info.point, "kind") == "double_point";
    }

    // A setpoint's feedback: `{ tag, within, settle }` on a number tag, and
    // a `within` above 0 on a FLOAT64 one (interim rule (i)).
    [[nodiscard]] std::optional<std::string_view> setpoint_feedback_fault(
        const Reader& reader, NodeId feedback) const {
        constexpr std::string_view NUMBER_TAG =
            "{ tag, within, settle } on a number tag";
        const std::optional<NodeId> tag =
            reader.document().kind(feedback) == NodeKind::MAP
                ? reader.scalar(feedback, "tag")
                : std::nullopt;
        if (!tag) {
            return NUMBER_TAG;
        }
        const std::optional<TagInfo> info = tag_at(reader, tag);
        if (!info) {
            return std::nullopt;  // CF-310 of the references
        }
        if (!is_number_type(info->value_type)) {
            return NUMBER_TAG;
        }
        const std::optional<NodeId> within = reader.scalar(feedback, "within");
        if (info->value_type == "float64" && within &&
            compare_numbers(reader.document().text(*within), "0") == 0) {
            return "{ tag, within, settle } with a within above 0 on a FLOAT64 "
                   "tag";
        }
        return std::nullopt;
    }

    // An output's `limits`, as interim rule (d) reads them: texts of the
    // Document.
    struct Limits {
        std::string_view min;
        std::string_view max;
        std::vector<std::pair<std::string_view, std::string_view>> ranges;
    };

    void refuse_value(const Reader& reader, NodeId node,
                      const std::string& head, const std::string& key) {
        error(reader, node, CF_VALUE_TYPE,
              head + ": " + key + " does not fit the target's type");
    }

    // CF-713 (ruling Q8, interim rules (d), (e) and (f)): the values an
    // output writes fit its target, by mathematical value.
    void check_values(const Reader& reader, NodeId output,
                      const std::string& head, std::string_view kind,
                      const Target& target, bool form_fits) {
        const std::optional<NodeId> limits = reader.child(output, "limits");
        const Limits bounds =
            limits ? read_limits(reader, *limits, head, target) : Limits{};
        if (const std::optional<NodeId> safe_state =
                reader.child(output, "safe_state");
            safe_state && form_fits && !has_no_safe_action(kind)) {
            check_written(reader, *safe_state, "safe_state", head, target,
                          &bounds);
        }
        if (const std::optional<NodeId> release =
                limits ? reader.child(*limits, "release") : std::nullopt) {
            check_written(reader, *release, "limits.release", head, target,
                          nullptr);
        }
    }

    // The bounds of `limits`, each checked against the target's type.
    [[nodiscard]] Limits read_limits(const Reader& reader, NodeId limits,
                                     const std::string& head,
                                     const Target& target) {
        const Document& document = reader.document();
        Limits bounds;
        for (const std::string_view key : {"min", "max"}) {
            if (const std::optional<NodeId> bound =
                    reader.scalar(limits, key)) {
                if (!fits_value_type(document.text(*bound),
                                     target.value_type)) {
                    refuse_value(reader, *bound, head,
                                 "limits." + std::string(key));
                }
                (key == "min" ? bounds.min : bounds.max) =
                    document.text(*bound);
            }
        }
        std::size_t index = 0;
        for (const NodeId range : reader.items(limits, "ranges")) {
            const std::span<const NodeId> pair =
                document.kind(range) == NodeKind::SEQUENCE
                    ? document.items(range)
                    : std::span<const NodeId>();
            for (const NodeId bound : pair) {
                if (!fits_value_type(document.text(bound), target.value_type)) {
                    refuse_value(
                        reader, bound, head,
                        "limits.ranges[" + std::to_string(index) + "]");
                }
            }
            if (pair.size() == 2) {
                bounds.ranges.emplace_back(document.text(pair[0]),
                                           document.text(pair[1]));
            }
            ++index;
        }
        return bounds;
    }

    // True if `value` is within `limits`: `min`, `max` and the union of
    // `ranges`.
    [[nodiscard]] static bool within(const Limits& limits,
                                     std::string_view value) {
        if ((!limits.min.empty() &&
             compare_numbers(value, limits.min).value_or(0) < 0) ||
            (!limits.max.empty() &&
             compare_numbers(value, limits.max).value_or(0) > 0)) {
            return false;
        }
        return limits.ranges.empty() ||
               std::ranges::any_of(limits.ranges, [value](const auto& range) {
                   return compare_numbers(value, range.first).value_or(-1) >=
                              0 &&
                          compare_numbers(value, range.second).value_or(1) <= 0;
               });
    }

    // A `{ value }` or `{ raw }` that the output writes, at `map`; `limits`
    // bound a safe value (interim rule (d)), none for a release value.
    void check_written(const Reader& reader, NodeId map, const std::string& key,
                       const std::string& head, const Target& target,
                       const Limits* limits) {
        const Document& document = reader.document();
        if (document.kind(map) != NodeKind::MAP) {
            return;
        }
        // One text for every cause (S4 of the review of #185): D-124 lists
        // them.
        if (const std::optional<NodeId> value = reader.scalar(map, "value")) {
            const std::string_view text = document.text(*value);
            if (!fits_value_type(text, target.value_type) ||
                (target.has_enum && !is_enum_code(target, text)) ||
                (target.has_values && !is_values_key(target, text)) ||
                (!target.write_raw.empty() &&
                 !survives_scaling(text, target.value_type, target.write_raw,
                                   target.scaling)) ||
                (limits != nullptr && !within(*limits, text))) {
                refuse_value(reader, *value, head, key + ".value");
            }
        }
        if (const std::optional<NodeId> raw = reader.scalar(map, "raw")) {
            if (target.encoded ||
                !fits_raw_width(document.text(*raw), target.write_raw)) {
                refuse_value(reader, *raw, head, key + ".raw");
            }
        }
    }

    // Interim rule (a) of D-124: `native_select` needs a select on the
    // device, which no protocol of phase 2 has (CF-714).
    void check_native_select(const Reader& reader, NodeId output,
                             const std::string& head, const Target& target) {
        if (const std::optional<NodeId> select =
                reader.child(output, "native_select");
            select &&
            (target.protocol == "modbus" || target.protocol == "io")) {
            error(reader, *select, CF_REFRESH_OR_RELEASE,
                  head +
                      ": native_select needs a select on the device, "
                      "which protocol " +
                      std::string(target.protocol) + " does not have");
        }
    }

    // The fail-safe actions of an output, written or by default (7.2, 7.4;
    // ruling Q7 of D-124): a dangerous output and the kinds with no safe
    // action hold on every protocol; an `io` output goes safe on a stop and
    // at once on control lost; a remote switching output goes safe 30 s
    // after control lost and holds on a stop; the others hold.
    [[nodiscard]] static FailSafe fail_safe_of(const Reader& reader,
                                               NodeId output,
                                               std::string_view kind,
                                               const Target& target) {
        const FailSafeDefaults defaults =
            fail_safe_defaults(kind, target.protocol, target.dangerous,
                               reader.text_of(output, "safe_state"));
        FailSafe fail_safe{
            .on_shutdown = defaults.on_shutdown,
            .shutdown_given = false,
            .on_control_loss = defaults.on_control_loss,
            .loss_given = false,
            .on_generation_change = defaults.on_generation_change,
            .generation_given = false};
        if (const std::optional<NodeId> shutdown =
                reader.scalar(output, "on_shutdown")) {
            fail_safe.on_shutdown = reader.document().text(*shutdown);
            fail_safe.shutdown_given = true;
        }
        if (const std::optional<NodeId> loss =
                reader.child(output, "on_control_loss")) {
            if (const std::optional<NodeId> action =
                    reader.scalar(*loss, "action")) {
                fail_safe.on_control_loss = reader.document().text(*action);
                fail_safe.loss_given = true;
            }
        }
        if (const std::optional<NodeId> generation =
                reader.scalar(output, "on_generation_change")) {
            fail_safe.on_generation_change =
                reader.document().text(*generation);
            fail_safe.generation_given = true;
        }
        return fail_safe;
    }

    // CF-722 on the defaults (ruling Q7), and on an `io` line's `at_start:
    // safe` (interim rule (c)), where `safe_state` is `hold`; and the io
    // form of CW-721 (interim rule (h)).
    void check_defaults(const Reader& reader, NodeId output,
                        const std::string& head, std::string_view kind,
                        const Target& target) {
        const std::optional<NodeId> safe_state =
            reader.scalar(output, "safe_state");
        if (!safe_state) {
            return;
        }
        const std::string_view state = reader.document().text(*safe_state);
        const FailSafe fail_safe = fail_safe_of(reader, output, kind, target);
        if (state == "hold" && !target.dangerous && !has_no_safe_action(kind)) {
            for (const auto& [key, action, given] :
                 {std::tuple{"on_shutdown", fail_safe.on_shutdown,
                             fail_safe.shutdown_given},
                  std::tuple{"on_control_loss", fail_safe.on_control_loss,
                             fail_safe.loss_given}}) {
                if (!given && action == "safe") {
                    error(reader, *safe_state, CF_SAFE_WITH_HOLD,
                          head +
                              ": action safe needs a safe_state other than "
                              "hold (safe is the default of " +
                              std::string(key) + " for an io target)");
                }
            }
        }
        // Interim rule (c), on every output (M2 of the review of #185): an
        // `at_start: safe` line is requested at the safe state, which `hold`
        // does not name.
        if (state == "hold" && target.at_start_safe) {
            error(reader, *safe_state, CF_SAFE_WITH_HOLD,
                  head +
                      ": action safe needs a safe_state other than hold "
                      "(the target's at_start is safe)");
        }
        if (target.protocol != "io" || target.dangerous ||
            !is_switching(kind) || (state != "off" && state != "on")) {
            return;
        }
        if (const std::optional<NodeId> loss =
                reader.child(output, "on_control_loss");
            loss && fail_safe.loss_given &&
            fail_safe.on_control_loss == "hold") {
            warning(reader, *loss, CW_HOLDS_ON_LOSS,
                    head +
                        " holds on control loss although its safe_state is " +
                        std::string(state));
        }
        if (const std::optional<NodeId> shutdown =
                reader.scalar(output, "on_shutdown");
            shutdown && fail_safe.on_shutdown == "hold") {
            warning(reader, *shutdown, CW_HOLDS_ON_LOSS,
                    head + " holds on shutdown although its safe_state is " +
                        std::string(state));
        }
    }

    // --- Enumerations (5.5; M6 of the review of #185) -----------------------

    // The devices of the expanded tree by name, and the tables of
    // enums.yaml, for the enumerations that points name.
    void read_enumerations() {
        const Document& document = expanded_.document();
        for (const NodeId channel :
             expanded_.items(document.root(), "channels")) {
            for (const NodeId device : expanded_.items(channel, "devices")) {
                devices_.try_emplace(expanded_.text_of(device, "name"), device);
            }
        }
        for (std::size_t i = 0; i < files_.size(); ++i) {
            if (files_[i].source->path_class.type != FileType::ENUMS) {
                continue;
            }
            const Reader& reader = readers_[i];
            for (const Entry& table :
                 reader.entries(reader.document().root(), "enums")) {
                global_enums_.try_emplace(reader.document().text(table.key),
                                          Table{&reader, table.value});
            }
        }
    }

    // The codes of the enumeration of `point`: inline, else named in the
    // device's `enums` (its templates' merged), else in enums.yaml.
    void read_enumeration(NodeId point, Target& target) const {
        const std::optional<NodeId> named = expanded_.child(point, "enum");
        if (!named) {
            return;
        }
        Table table{&expanded_, *named};
        if (expanded_.document().kind(*named) == NodeKind::SCALAR) {
            const std::string_view name = expanded_.document().text(*named);
            const auto device = devices_.find(device_);
            const std::optional<NodeId> own =
                device == devices_.end()
                    ? std::nullopt
                    : expanded_.child(device->second, "enums");
            const std::optional<NodeId> local =
                own ? expanded_.child(*own, name) : std::nullopt;
            if (local) {
                table.node = *local;
            } else if (const auto global = global_enums_.find(name);
                       global != global_enums_.end()) {
                table = global->second;
            } else {
                return;  // CF-310 of the references
            }
        }
        if (table.reader->document().kind(table.node) != NodeKind::MAP) {
            return;
        }
        target.has_enum = true;
        for (const Entry& code : table.reader->document().entries(table.node)) {
            target.enum_codes.push_back(
                table.reader->document().text(code.key));
        }
    }

    [[nodiscard]] static bool is_enum_code(const Target& target,
                                           std::string_view value) {
        return std::ranges::any_of(
            target.enum_codes, [value](std::string_view code) {
                return compare_numbers(value, code).value_or(1) == 0;
            });
    }

    // --- Areas (7.1) -------------------------------------------------------

    void check_areas() {
        std::set<std::string_view> used;
        collect_area_uses(expanded_, used);
        for (std::size_t i = 0; i < files_.size(); ++i) {
            const FileType type = files_[i].source->path_class.type;
            if (type == FileType::TAGS || type == FileType::ALARMS ||
                type == FileType::OUTPUTS) {
                collect_area_uses(readers_[i], used);
            }
        }
        for (std::size_t i = 0; i < files_.size(); ++i) {
            if (files_[i].source->path_class.type != FileType::AREAS) {
                continue;
            }
            const Reader& reader = readers_[i];
            for (const Entry& area :
                 reader.entries(reader.document().root(), "areas")) {
                const std::string_view name = reader.document().text(area.key);
                check_authority(reader, area.value, name);
                if (!used.contains(name)) {
                    warning(reader, area.key, CW_AREA_NOT_USED,
                            "area " + quote_text(name) + " is not used");
                }
            }
        }
    }

    // Every scalar under an `area` key of `reader`'s Document: an output's,
    // a tag's, a point's, an alarm's (D-124). A walk with its own stack: the
    // depth is the document's, which its parser bounds.
    static void collect_area_uses(const Reader& reader,
                                  std::set<std::string_view>& used) {
        const Document& document = reader.document();
        std::vector<NodeId> stack{document.root()};
        while (!stack.empty()) {
            const NodeId node = stack.back();
            stack.pop_back();
            if (document.kind(node) == NodeKind::MAP) {
                for (const Entry& entry : document.entries(node)) {
                    if (document.text(entry.key) == "area" &&
                        document.kind(entry.value) == NodeKind::SCALAR) {
                        used.insert(document.text(entry.value));
                    } else {
                        stack.push_back(entry.value);
                    }
                }
            } else if (document.kind(node) == NodeKind::SEQUENCE) {
                for (const NodeId item : document.items(node)) {
                    stack.push_back(item);
                }
            }
        }
    }

    // CF-718 (7.1): `local_blocks_auto` needs a panel, and the panel's
    // input is a BOOL tag that a driver reads.
    void check_authority(const Reader& reader, NodeId area,
                         std::string_view name) {
        const std::optional<NodeId> authority = reader.child(area, "authority");
        if (!authority) {
            return;
        }
        const std::string head = "area " + quote_text(name) + ": ";
        const std::optional<NodeId> panel =
            reader.child(*authority, "local_panel");
        if (const std::optional<NodeId> blocks =
                reader.scalar(*authority, "local_blocks_auto");
            blocks && reader.document().text(*blocks) == "true" && !panel) {
            error(reader, *blocks, CF_LOCAL_PANEL,
                  head + "local_blocks_auto needs local_panel");
        }
        const std::optional<NodeId> input =
            panel ? reader.scalar(*panel, "input") : std::nullopt;
        if (!input || !tags_->complete()) {
            return;
        }
        const std::optional<TagInfo> info =
            tags_->find(reader.document().text(*input));
        if (!info) {
            return;  // CF-310 of the references
        }
        const bool driver_reads =
            (info->origin == TagOrigin::POINT ||
             info->origin == TagOrigin::BIT) &&
            info->point != 0 &&
            expanded_.text_of(info->point, "access") != "wo";
        if (!driver_reads || info->value_type != "bool") {
            error(reader, *input, CF_LOCAL_PANEL,
                  head + "local_panel.input " +
                      quote_text(reader.document().text(*input)) +
                      " is not a BOOL tag read by a driver");
        }
    }

    const TagNames* tags_;
    std::span<const CheckedFile> files_;
    Reader expanded_;
    Reporter reporter_;
    std::vector<Reader> readers_;
    // An enumeration table: a map of codes in a Document.
    struct Table {
        const Reader* reader = nullptr;
        NodeId node = 0;
    };
    std::map<std::string_view, NodeId, std::less<>> devices_;
    std::map<std::string_view, Table, std::less<>> global_enums_;
    // The device of the target being read.
    mutable std::string_view device_;
    // The outputs checked, by name: views into the tree's Documents.
    std::set<std::string_view> outputs_;
    // Each target and the first output that names it, quoted.
    std::map<std::string, std::string, std::less<>> targets_;
};

}  // namespace

FailSafeDefaults fail_safe_defaults(std::string_view kind,
                                    std::string_view protocol, bool dangerous,
                                    std::string_view safe_state) {
    if (dangerous || has_no_safe_action(kind)) {
        return {.on_shutdown = "hold",
                .on_control_loss = "hold",
                .control_loss_after = {},
                .on_generation_change = "hold"};
    }
    if (protocol == "io") {
        return {.on_shutdown = "safe",
                .on_control_loss = "safe",
                .control_loss_after = "0s",
                .on_generation_change = "hold"};
    }
    if (is_switching(kind) && (safe_state == "off" || safe_state == "on")) {
        return {.on_shutdown = "hold",
                .on_control_loss = "safe",
                .control_loss_after = "30s",
                .on_generation_change = "hold"};
    }
    return {.on_shutdown = "hold",
            .on_control_loss = "hold",
            .control_loss_after = {},
            .on_generation_change = "hold"};
}

void check_output_rules(const TagNames& tags, const ExpandedTree& tree,
                        std::span<const CheckedFile> files,
                        Diagnostics& diagnostics) {
    Checker(tags, tree, files, diagnostics).run();
}

}  // namespace scada::conf
