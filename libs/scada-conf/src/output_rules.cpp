// Areas, outputs and their safe states (configuration schemas contract 7.1
// to 7.4 and 10.7; decision D-124, slice 7e-1 of #100).

#include "output_rules.h"

#include "scada/conf/diagnostic.h"
#include "scada/conf/expand.h"
#include "scada/conf/node.h"
#include "scada/conf/references.h"
#include "scada/conf/tree.h"
#include "tag_names.h"
#include "tree_reader.h"

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
        Target target{
            .point = info->origin == TagOrigin::POINT && info->point != 0,
            .value_type = info->value_type,
            .access = {},
            .protocol = info->protocol,
            .dangerous = false,
            .nvm = false};
        if (target.point) {
            target.access = expanded_.text_of(info->point, "access");
            target.dangerous =
                expanded_.text_of(info->point, "dangerous") == "true";
            target.nvm = expanded_.text_of(info->point, "storage") == "nvm";
        }
        return target;
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
            check_target(reader, *target_node, head, target);
        }
        const bool dangerous = target && target->dangerous;
        if (!reader.child(output, "feedback") &&
            !reader.child(output, "no_feedback")) {
            error(reader, *name_node, CF_NO_FEEDBACK,
                  head + " needs feedback or no_feedback: true");
        }
        // A dangerous output holds whatever it is given (7.2): no
        // `safe_state` is needed, and one other than `hold` is CF-705.
        if (!reader.child(output, "safe_state") && !dangerous) {
            error(reader, *name_node, CF_NO_SAFE_STATE,
                  head + " needs a safe_state");
        }
        check_interlocks(reader, output, *name_node, head, kind);
        check_refresh(reader, output, head, kind, target);
        check_actions(reader, output, head, kind, dangerous);
        if (const std::optional<NodeId> duration =
                reader.child(output, "duration");
            duration && kind == "pulse") {
            error(reader, *duration, CF_PULSE_DURATION,
                  head + ": the device must time the pulse");
        }
        check_control_loss_hold(reader, output, head, kind, target);
    }

    // CF-702 and CF-703.
    void check_target(const Reader& reader, NodeId node,
                      const std::string& head,
                      const std::optional<Target>& target) {
        const std::string_view tag = reader.document().text(node);
        if (target && (!target->point ||
                       (target->access != "rw" && target->access != "wo") ||
                       target->value_type == "string")) {
            error(reader, node, CF_NOT_WRITABLE,
                  head + ": target " + quote_text(tag) +
                      " is not a writable point");
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
    // The outputs checked, by name: views into the tree's Documents.
    std::set<std::string_view> outputs_;
    // Each target and the first output that names it, quoted.
    std::map<std::string, std::string, std::less<>> targets_;
};

}  // namespace

void check_output_rules(const TagNames& tags, const ExpandedTree& tree,
                        std::span<const CheckedFile> files,
                        Diagnostics& diagnostics) {
    Checker(tags, tree, files, diagnostics).run();
}

}  // namespace scada::conf
