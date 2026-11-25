// Stage 5 without expansion (decision D-113): names that must exist, names
// given twice, a Modbus channel's protocol rules, and the types of a
// template's configuration expressions. Every check reads the files that
// passed stage 3, so a value of the wrong shape (already reported) is
// skipped, never reported a second time.

#include "scada/conf/references.h"

#include "expression_site.h"
#include "expression_types.h"
#include "scada/conf/diagnostic.h"
#include "scada/conf/expression.h"
#include "scada/conf/names.h"
#include "scada/conf/node.h"
#include "scada/conf/scalar.h"
#include "scada/conf/tree.h"
#include "scada/conf/types.h"

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
#include <utility>
#include <vector>

namespace scada::conf {

namespace {

constexpr std::uint16_t CF_SYSTEM_TAG = 301;
constexpr std::uint16_t CF_TAG_TWICE = 302;
constexpr std::uint16_t CF_NAME_TWICE = 303;
constexpr std::uint16_t CF_NO_SUCH = 310;
constexpr std::uint16_t CF_EXPRESSION_FAILS = 410;
constexpr std::uint16_t CF_TYPE_MISMATCH = 523;
constexpr std::uint16_t CF_PROTOCOL_RULE = 622;

// The most parents of a template (4.3); a longer chain is CF-401 of the
// expander, and here only bounds the walk.
constexpr std::size_t MAX_CHAIN = 5;

// Defaults when no file gives the list (6, 8).
constexpr std::array<std::string_view, 2> DEFAULT_TIERS = {"1m", "1h"};
constexpr std::array<std::string_view, 4> DEFAULT_PRIORITIES = {
    "critical", "high", "medium", "low"};
constexpr std::array<std::string_view, 2> DEFAULT_CLASSES = {"process",
                                                             "system"};
// 9.1: "Without `control/tasks.yaml`, the tasks `fast` (10 ms), `normal`
// (100 ms) and `slow` (1 s) exist. A file replaces them all."
constexpr std::array<std::string_view, 3> DEFAULT_TASKS = {"fast", "normal",
                                                           "slow"};

// Names of one kind, each with the place it was first given.
class Definitions {
public:
    // Adds `name`; the place of its first definition if it was there.
    std::optional<Location> add(std::string_view name, const Location& where) {
        const auto [entry, added] =
            names_.try_emplace(std::string(name), where);
        if (added) {
            return std::nullopt;
        }
        return entry->second;
    }

    [[nodiscard]] bool has(std::string_view name) const {
        return names_.find(name) != names_.end();
    }

    [[nodiscard]] bool empty() const noexcept { return names_.empty(); }

private:
    std::map<std::string, Location, std::less<>> names_;
};

std::string first_at(const Location& where) {
    return "; first at " + where.file + ":" + std::to_string(where.line);
}

// The parts of an IPv4 literal; nothing for anything else.
// Why an IPv4 address cannot be a device's (Annex A.1), or nothing.
std::optional<std::string> refused_ipv4(
    const std::array<std::uint8_t, IPV4_BYTES>& bytes) {
    constexpr std::uint8_t LOOPBACK = 127;
    constexpr std::uint8_t FIRST_MULTICAST = 224;
    constexpr std::uint8_t LAST_MULTICAST = 239;
    constexpr std::uint8_t ALL = 255;
    if (bytes[0] == LOOPBACK) {
        return "a loopback address";
    }
    if (bytes[0] >= FIRST_MULTICAST && bytes[0] <= LAST_MULTICAST) {
        return "a multicast address";
    }
    if (bytes == std::array<std::uint8_t, IPV4_BYTES>{ALL, ALL, ALL, ALL}) {
        return "the broadcast address";
    }
    return std::nullopt;
}

// Why `host` cannot be a device's address (Annex A.1), or nothing. An
// address is classified by its bytes, however it is written; an
// IPv4-mapped IPv6 address (`::ffff:a.b.c.d`) by its IPv4 address.
std::optional<std::string> refused_host(std::string_view host) {
    if (const std::optional<std::array<std::uint8_t, IPV4_BYTES>> bytes =
            ipv4_bytes(host)) {
        return refused_ipv4(*bytes);
    }
    const std::optional<std::array<std::uint8_t, IPV6_BYTES>> bytes =
        ipv6_bytes(host);
    if (!bytes) {
        return std::nullopt;
    }
    constexpr std::uint8_t MULTICAST = 0xFF;
    constexpr std::size_t MAPPED_AT = 10;
    if (bytes->front() == MULTICAST) {
        return "a multicast address";
    }
    // `::ffff:a.b.c.d`: ten zero bytes, two 0xFF, then the IPv4 address.
    const bool mapped =
        std::all_of(bytes->begin(), bytes->begin() + MAPPED_AT,
                    [](std::uint8_t byte) { return byte == 0; }) &&
        bytes->at(MAPPED_AT) == MULTICAST &&
        bytes->at(MAPPED_AT + 1) == MULTICAST;
    if (mapped) {
        std::array<std::uint8_t, IPV4_BYTES> ipv4{};
        std::copy(bytes->end() - IPV4_BYTES, bytes->end(), ipv4.begin());
        return refused_ipv4(ipv4);
    }
    std::array<std::uint8_t, IPV6_BYTES> loopback{};
    loopback.back() = 1;
    if (*bytes == loopback) {
        return "a loopback address";
    }
    return std::nullopt;
}

class Checker {
public:
    Checker(std::span<const CheckedFile> files, Diagnostics& diagnostics)
        : files_(files), diagnostics_(&diagnostics) {}

    void run() {
        for (const CheckedFile& file : files_) {
            enter(file);
            collect();
        }
        add_defaults();
        for (const CheckedFile& file : files_) {
            enter(file);
            check();
        }
    }

private:
    // --- Reading the current file ----------------------------------------

    void enter(const CheckedFile& file) {
        file_ = &file;
        document_ = file.document;
    }

    [[nodiscard]] FileType type() const noexcept {
        return file_->source->path_class.type;
    }

    [[nodiscard]] NodeId root() const noexcept { return document_->root(); }

    [[nodiscard]] bool is_map(NodeId node) const noexcept {
        return document_->kind(node) == NodeKind::MAP;
    }

    [[nodiscard]] std::optional<NodeId> find(NodeId map,
                                             std::string_view key) const {
        if (!is_map(map)) {
            return std::nullopt;
        }
        return document_->find(map, key);
    }

    [[nodiscard]] std::optional<NodeId> find_scalar(
        NodeId map, std::string_view key) const {
        const std::optional<NodeId> node = find(map, key);
        if (!node || document_->kind(*node) != NodeKind::SCALAR) {
            return std::nullopt;
        }
        return node;
    }

    [[nodiscard]] std::optional<NodeId> find_map(NodeId map,
                                                 std::string_view key) const {
        const std::optional<NodeId> node = find(map, key);
        if (!node || !is_map(*node)) {
            return std::nullopt;
        }
        return node;
    }

    // The items of a list under `key`; empty for anything else.
    [[nodiscard]] std::span<const NodeId> items(NodeId map,
                                                std::string_view key) const {
        const std::optional<NodeId> node = find(map, key);
        if (!node) {
            return {};
        }
        return document_->items(*node);
    }

    [[nodiscard]] std::string_view text(NodeId node) const {
        return document_->text(node);
    }

    void error(std::uint16_t code, NodeId node, std::string message) {
        diagnostics_->error(code, document_->location(node),
                            std::move(message));
    }

    // CF-310 when the scalar `node` names nothing in `names`.
    void need(const Definitions& names, std::string_view kind, NodeId node) {
        if (document_->kind(node) != NodeKind::SCALAR ||
            names.has(text(node))) {
            return;
        }
        error(CF_NO_SUCH, node,
              std::string(kind) + " " + quote_text(text(node)) +
                  " does not exist");
    }

    void need_key(const Definitions& names, std::string_view kind, NodeId map,
                  std::string_view key) {
        if (const std::optional<NodeId> node = find_scalar(map, key)) {
            need(names, kind, *node);
        }
    }

    // --- Definitions -----------------------------------------------------

    void collect() {
        switch (type()) {
            case FileType::TEMPLATE:
                templates_.emplace(file_->source->path_class.stem, file_);
                template_names_.add(file_->source->path_class.stem,
                                    document_->location(root()));
                break;
            case FileType::CHANNEL:
                collect_channel();
                break;
            case FileType::AREAS:
                collect_keys(areas_, "areas");
                break;
            case FileType::ENUMS:
                collect_keys(enums_, "enums");
                break;
            case FileType::LOGGING:
                collect_logging();
                break;
            case FileType::TASKS:
                tasks_given_ = true;
                collect_keys(tasks_, "tasks");
                break;
            case FileType::PROGRAM:
                programs_.add(file_->source->path_class.stem,
                              document_->location(root()));
                break;
            case FileType::ALARMS:
                collect_alarm_lists();
                break;
            case FileType::OUTPUTS:
                collect_outputs();
                break;
            case FileType::TAGS:
                for (const NodeId tag : items(root(), "tags")) {
                    define_tag(tag, "name");
                }
                break;
            default:
                break;
        }
    }

    void collect_keys(Definitions& names, std::string_view key) {
        if (const std::optional<NodeId> map = find_map(root(), key)) {
            for (const Entry& entry : document_->entries(*map)) {
                names.add(text(entry.key), document_->location(entry.key));
            }
        }
    }

    // A name of `kind` given twice is CF-303 at the second.
    void define(Definitions& names, std::string_view kind, NodeId name) {
        if (std::optional<Location> first =
                names.add(text(name), document_->location(name))) {
            error(CF_NAME_TWICE, name,
                  std::string(kind) + " " + quote_text(text(name)) +
                      " is defined twice" + first_at(*first));
        }
    }

    void collect_channel() {
        if (const std::optional<NodeId> channel = find_map(root(), "channel")) {
            if (const std::optional<NodeId> name =
                    find_scalar(*channel, "name")) {
                define(channels_, "channel", *name);
            }
        }
        for (const NodeId device : items(root(), "devices")) {
            if (const std::optional<NodeId> name =
                    find_scalar(device, "name")) {
                define(devices_, "device", *name);
            }
        }
    }

    void collect_logging() {
        collect_keys(policies_, "policies");
        const std::optional<NodeId> tiers = find(root(), "aggregate_tiers");
        if (!tiers) {
            return;
        }
        tiers_given_ = true;
        for (const NodeId tier : document_->items(*tiers)) {
            if (const std::optional<NodeId> name = find_scalar(tier, "name")) {
                tiers_.add(text(*name), document_->location(*name));
            }
        }
    }

    // `priorities` and `classes` are given in one file at most (CF-811,
    // checked with the other alarm rules); the first file that gives a list
    // defines it.
    void collect_alarm_lists() {
        const auto take = [this](std::string_view key, Definitions& names,
                                 bool& given) {
            const std::optional<NodeId> list = find(root(), key);
            if (!list || given) {
                return;
            }
            given = true;
            for (const NodeId item : document_->items(*list)) {
                if (document_->kind(item) == NodeKind::SCALAR) {
                    names.add(text(item), document_->location(item));
                }
            }
        };
        take("priorities", priorities_, priorities_given_);
        take("classes", classes_, classes_given_);
    }

    void collect_outputs() {
        for (const NodeId interlock : items(root(), "interlocks")) {
            if (const std::optional<NodeId> name =
                    find_scalar(interlock, "name")) {
                define(interlocks_, "interlock", *name);
                define_tag(interlock, "name");
            }
        }
        for (const NodeId output : items(root(), "outputs")) {
            if (const std::optional<NodeId> name =
                    find_scalar(output, "name")) {
                outputs_.add(text(*name), document_->location(*name));
            }
        }
    }

    // A tag that a tag file or an interlock defines: CF-301 in `sys`, CF-302
    // when given twice. Point tags come with the expander (#103).
    void define_tag(NodeId owner, std::string_view key) {
        const std::optional<NodeId> name = find_scalar(owner, key);
        if (!name) {
            return;
        }
        const std::string_view tag = text(*name);
        // 2.1: a user tag name "must not start with the segment `sys`".
        if (tag == "sys" || tag.starts_with("sys.")) {
            error(CF_SYSTEM_TAG, *name,
                  "tag names starting with sys. are reserved");
            return;
        }
        if (std::optional<Location> first =
                tags_.add(tag, document_->location(*name))) {
            error(CF_TAG_TWICE, *name,
                  "tag " + quote_text(tag) + " is defined twice" +
                      first_at(*first));
        }
    }

    void add_defaults() {
        const Location none{};
        if (!tasks_given_) {
            for (const std::string_view task : DEFAULT_TASKS) {
                tasks_.add(task, none);
            }
        }
        if (!tiers_given_) {
            for (const std::string_view tier : DEFAULT_TIERS) {
                tiers_.add(tier, none);
            }
        }
        if (!priorities_given_) {
            for (const std::string_view priority : DEFAULT_PRIORITIES) {
                priorities_.add(priority, none);
            }
        }
        if (!classes_given_) {
            for (const std::string_view name : DEFAULT_CLASSES) {
                classes_.add(name, none);
            }
        }
    }

    // --- References ------------------------------------------------------

    void check() {
        switch (type()) {
            case FileType::TEMPLATE:
                check_template();
                break;
            case FileType::CHANNEL:
                check_channel();
                break;
            case FileType::TAGS:
                for (const NodeId tag : items(root(), "tags")) {
                    check_tag(tag);
                }
                break;
            case FileType::OUTPUTS:
                check_outputs();
                break;
            case FileType::ALARMS:
                check_alarm_file();
                break;
            case FileType::LOGGING:
                check_logging();
                break;
            case FileType::TASKS:
                check_tasks();
                break;
            case FileType::PROGRAM:
                check_program();
                break;
            default:
                break;
        }
    }

    void check_template() {
        need_key(template_names_, "template", root(), "extends");
        if (const std::optional<NodeId> protocol =
                find_scalar(root(), "protocol");
            protocol && !is_known_protocol(text(*protocol))) {
            error(
                CF_NO_SUCH, *protocol,
                "protocol " + quote_text(text(*protocol)) + " does not exist");
        }
        const Scope& scope = scope_of(file_);
        for (const NodeId point : items(root(), "points")) {
            check_point(point, scope, {});
        }
        for (const NodeId block : items(root(), "repeat")) {
            const Variables variables = variables_of(block);
            check_repeat(block, scope);
            for (const NodeId point : items(block, "points")) {
                check_point(point, scope, variables);
            }
        }
        if (const std::optional<NodeId> defaults =
                find_map(root(), "defaults")) {
            check_point(*defaults, scope, scope.variables);
        }
    }

    using Variables = std::set<std::string, std::less<>>;

    // The variables of a `repeat` block's `for`.
    [[nodiscard]] Variables variables_of(NodeId block) const {
        Variables variables;
        if (const std::optional<NodeId> range = find_map(block, "for")) {
            for (const Entry& entry : document_->entries(*range)) {
                variables.emplace(text(entry.key));
            }
        }
        return variables;
    }

    // What the names of a template's points and expressions resolve to:
    // the enumerations and parameters of its chain, and the variables of
    // every `repeat` block of the chain (a template's `defaults` reach the
    // points of every level).
    struct Scope {
        std::set<std::string, std::less<>> enums;
        std::map<std::string, ValueType, std::less<>> parameters;
        std::set<std::string, std::less<>> variables;
        // False when the chain names a template the tree does not have:
        // its enumerations and parameters are not known, and the missing
        // template is the error.
        bool known = true;
        // False where the expander types the expressions (an instance of a
        // template).
        bool typed = true;
        // True for a device without template: a name in an expression is
        // an error here, as nothing can define it.
        bool bare = false;
    };

    // The scope of `file`, a template, walking `extends` up to MAX_CHAIN
    // files, built once per template. A chain that leaves the tree, has a
    // cycle or is longer is not known: its names are not checked, as the
    // missing template or CF-401 (#103) is its error.
    const Scope& scope_of(const CheckedFile* file) {
        const auto cached = scopes_.find(file);
        if (cached != scopes_.end()) {
            return cached->second;
        }
        Scope& scope = scopes_[file];
        std::set<const CheckedFile*> seen;
        for (std::size_t depth = 0; file != nullptr; ++depth) {
            if (depth == MAX_CHAIN || !seen.insert(file).second) {
                scope.known = false;
                break;
            }
            const Document& document = *file->document;
            add_level(document, scope);
            const std::optional<NodeId> parent =
                document.kind(document.root()) == NodeKind::MAP
                    ? document.find(document.root(), "extends")
                    : std::nullopt;
            if (!parent) {
                break;
            }
            const auto next = templates_.find(document.text(*parent));
            if (next == templates_.end()) {
                scope.known = false;
                break;
            }
            file = next->second;
        }
        return scope;
    }

    // The parameters a level declares with their type.
    static void add_parameters(const Document& document, NodeId root,
                               Scope& scope) {
        const std::optional<NodeId> params = document.find(root, "params");
        if (!params || document.kind(*params) != NodeKind::MAP) {
            return;
        }
        for (const Entry& entry : document.entries(*params)) {
            // A child may change a default without repeating the type, and
            // never changes it (CF-415): the type is that of the nearest
            // declaration that gives one.
            if (document.kind(entry.value) != NodeKind::MAP ||
                !document.find(entry.value, "type")) {
                continue;
            }
            scope.parameters.try_emplace(std::string(document.text(entry.key)),
                                         parameter_type(document, entry.value));
        }
    }

    static void add_level(const Document& document, Scope& scope) {
        const NodeId root = document.root();
        if (document.kind(root) != NodeKind::MAP) {
            return;
        }
        if (const std::optional<NodeId> enums = document.find(root, "enums");
            enums && document.kind(*enums) == NodeKind::MAP) {
            for (const Entry& entry : document.entries(*enums)) {
                scope.enums.emplace(document.text(entry.key));
            }
        }
        add_parameters(document, root, scope);
        if (const std::optional<NodeId> repeat =
                document.find(root, "repeat")) {
            for (const NodeId block : document.items(*repeat)) {
                const std::optional<NodeId> range =
                    document.kind(block) == NodeKind::MAP
                        ? document.find(block, "for")
                        : std::nullopt;
                if (!range || document.kind(*range) != NodeKind::MAP) {
                    continue;
                }
                for (const Entry& entry : document.entries(*range)) {
                    scope.variables.emplace(document.text(entry.key));
                }
            }
        }
    }

    // The type of a parameter's value in an expression (5.6): `number` is
    // REAL, `string` and `enum` STRING, `set` SET.
    static ValueType parameter_type(const Document& document, NodeId param) {
        const std::optional<NodeId> type = document.kind(param) == NodeKind::MAP
                                               ? document.find(param, "type")
                                               : std::nullopt;
        const std::string_view name =
            type ? document.text(*type) : std::string_view();
        if (name == "int") {
            return ValueType::INT;
        }
        if (name == "number") {
            return ValueType::REAL;
        }
        if (name == "bool") {
            return ValueType::BOOL;
        }
        if (name == "set") {
            return ValueType::SET;
        }
        return ValueType::STRING;
    }

    void check_channel() {
        if (file_->source->path_class.protocol == "modbus") {
            check_modbus_channel();
        }
        for (const NodeId device : items(root(), "devices")) {
            check_device(device);
        }
    }

    // Annex A.1: 7 data bits belong to Modbus ASCII, which is reserved; a
    // device is never at a loopback, multicast or broadcast address.
    void check_modbus_channel() {
        const std::optional<NodeId> channel = find_map(root(), "channel");
        if (!channel) {
            return;
        }
        const std::optional<NodeId> kind = find_scalar(*channel, "type");
        constexpr std::uint64_t ASCII_DATA_BITS = 7;
        // By its value, as stage 3 typed it: `7`, `0x7` and `0x07` alike.
        const std::optional<NodeId> bits = find_scalar(*channel, "data_bits");
        const std::optional<Integer> value =
            bits && file_->types->of(*bits) == ScalarType::INTEGER
                ? parse_integer(text(*bits))
                : std::nullopt;
        if (bits && kind && text(*kind) == "rtu" && value && !value->negative &&
            value->magnitude == ASCII_DATA_BITS) {
            error(CF_PROTOCOL_RULE, *bits,
                  "channel.data_bits: 7 data bits are Modbus ASCII, which is "
                  "not available in this version");
        }
        if (const std::optional<NodeId> host = find_scalar(*channel, "host")) {
            if (const std::optional<std::string> reason =
                    refused_host(text(*host))) {
                error(CF_PROTOCOL_RULE, *host,
                      "channel.host: " + quote_text(text(*host)) + " is " +
                          *reason);
            }
        }
    }

    void check_device(NodeId device) {
        Scope scope;
        if (const std::optional<NodeId> name =
                find_scalar(device, "template")) {
            need(template_names_, "template", *name);
            if (const auto found = templates_.find(text(*name));
                found != templates_.end()) {
                scope = scope_of(found->second);
            } else {
                scope.known = false;
            }
            // An instance's points, defaults and overrides may use the
            // variables of the template's `repeat` blocks (4.5, step 6);
            // their expressions are typed by the expander, which binds
            // them.
            scope.typed = false;
        } else {
            // Without a template there is no parameter or variable: every
            // name in an expression is unknown (B5 of the review of #144).
            scope.bare = true;
        }
        for (const NodeId point : items(device, "points")) {
            check_point(point, scope, {});
        }
        if (const std::optional<NodeId> defaults =
                find_map(device, "defaults")) {
            check_point(*defaults, scope, {});
        }
        if (const std::optional<NodeId> overrides =
                find_map(device, "overrides")) {
            for (const Entry& entry : document_->entries(*overrides)) {
                check_point(entry.value, scope, {});
            }
        }
    }

    // The names a point uses: enumeration, logging, alarms, its bits'; and
    // the types of its configuration expressions, when its scope is known.
    void check_point(NodeId point, const Scope& scope,
                     const Variables& variables) {
        if (!is_map(point)) {
            return;
        }
        // In a template, a child of the chain may define the enumeration
        // (4.5, step 3.1): it is checked at the instance (#103).
        if (const std::optional<NodeId> name = find_scalar(point, "enum");
            name && scope.known && !in_template() &&
            !scope.enums.contains(text(*name))) {
            need(enums_, "enumeration", *name);
        }
        check_logging_of(point);
        check_alarms_of(point);
        if (const std::optional<NodeId> bits = find_map(point, "bits")) {
            for (const Entry& entry : document_->entries(*bits)) {
                if (is_map(entry.value)) {
                    check_logging_of(entry.value);
                    check_alarms_of(entry.value);
                }
            }
        }
        if (scope.known && scope.typed) {
            check_expressions(point, scope, variables);
        }
    }

    void check_logging_of(NodeId owner) {
        const std::optional<NodeId> logging = find(owner, "logging");
        if (!logging) {
            return;
        }
        if (document_->kind(*logging) == NodeKind::SCALAR) {
            if (text(*logging) != "none") {
                need_policy(*logging);
            }
            return;
        }
        check_settings(*logging);
    }

    void need_policy(NodeId name) { need(policies_, "logging policy", name); }

    // `policy` and the tiers of logging settings.
    void check_settings(NodeId settings) {
        if (const std::optional<NodeId> policy =
                find_scalar(settings, "policy")) {
            need_policy(*policy);
        }
        if (const std::optional<NodeId> tiers = find_map(settings, "tiers")) {
            for (const Entry& entry : document_->entries(*tiers)) {
                if (text(entry.key) != "replace") {
                    need(tiers_, "aggregate tier", entry.key);
                }
            }
        }
    }

    void check_alarms_of(NodeId owner) {
        for (const NodeId alarm : items(owner, "alarms")) {
            check_alarm(alarm);
        }
    }

    void check_alarm(NodeId alarm) {
        need_key(priorities_, "alarm priority", alarm, "priority");
        need_key(classes_, "alarm class", alarm, "class");
        need_key(areas_, "area", alarm, "area");
    }

    void check_tag(NodeId tag) {
        need_key(areas_, "area", tag, "area");
        need_key(tasks_, "task", tag, "task");
        need_key(enums_, "enumeration", tag, "enum");
        check_logging_of(tag);
        check_alarms_of(tag);
    }

    void check_outputs() {
        for (const NodeId interlock : items(root(), "interlocks")) {
            need_key(tasks_, "task", interlock, "task");
        }
        for (const NodeId output : items(root(), "outputs")) {
            need_key(areas_, "area", output, "area");
            need_key(tasks_, "task", output, "task");
            const std::optional<NodeId> interlocks = find(output, "interlocks");
            if (!interlocks) {
                continue;
            }
            for (const NodeId name : document_->items(*interlocks)) {
                need(interlocks_, "interlock", name);
            }
            for (const std::string_view side : {"on", "off"}) {
                for (const NodeId name : items(*interlocks, side)) {
                    need(interlocks_, "interlock", name);
                }
            }
        }
    }

    void check_alarm_file() {
        if (const std::optional<NodeId> shelve =
                find_map(root(), "max_shelve")) {
            for (const Entry& entry : document_->entries(*shelve)) {
                need(priorities_, "alarm priority", entry.key);
            }
        }
        for (const NodeId alarm : items(root(), "alarms")) {
            check_alarm(alarm);
        }
    }

    void check_logging() {
        if (const std::optional<NodeId> name =
                find_scalar(root(), "default_policy")) {
            need_policy(*name);
        }
        if (const std::optional<NodeId> policies =
                find_map(root(), "policies")) {
            for (const Entry& entry : document_->entries(*policies)) {
                check_settings(entry.value);
            }
        }
    }

    void check_tasks() {
        if (const std::optional<NodeId> tasks = find_map(root(), "tasks")) {
            for (const Entry& entry : document_->entries(*tasks)) {
                for (const NodeId program : items(entry.value, "programs")) {
                    need(programs_, "program", program);
                }
            }
        }
    }

    void check_program() {
        need_key(tasks_, "task", root(), "task");
        for (const NodeId block : items(root(), "blocks")) {
            need_key(outputs_, "output", block, "output");
        }
    }

    [[nodiscard]] bool in_template() const noexcept {
        return type() == FileType::TEMPLATE;
    }

    // --- Configuration expressions of a template ---------------------------

    void check_repeat(NodeId block, const Scope& scope) {
        if (!scope.known) {
            return;
        }
        // A range is evaluated before its variables exist, so it may use
        // parameters only; the block's `when` may use its variables (4.4).
        if (const std::optional<NodeId> range = find_map(block, "for")) {
            for (const Entry& entry : document_->entries(*range)) {
                check_for_value(entry.value, scope);
            }
        }
        if (const std::optional<NodeId> when = find_scalar(block, "when")) {
            check_typed(*when, scope, variables_of(block),
                        ExpressionKind::CONFIGURATION, Wanted::BOOL);
        }
    }

    // A `for` value: a range, a list of integers and ranges, or one INT.
    // NOLINTNEXTLINE(misc-no-recursion): bounded by the nesting limit of 32
    void check_for_value(NodeId value, const Scope& scope) {
        if (document_->kind(value) == NodeKind::SEQUENCE) {
            for (const NodeId item : document_->items(value)) {
                check_for_value(item, scope);
            }
            return;
        }
        if (file_->types->of(value) == ScalarType::EXPRESSION) {
            check_typed(value, scope, {}, ExpressionKind::FOR_VALUE,
                        Wanted::RANGE_OR_INT, false);
        }
    }

    enum class Wanted : std::uint8_t { BOOL, NUMBER, RANGE_OR_INT };

    // The one place that tells run-time expressions from configuration
    // expressions in a point (8): an alarm's `expr`, `condition` and
    // `suppress_when` read tags, wherever the alarm stands (a point, a bit
    // of a point, a tag). Stage 3 types both kinds as EXPRESSION.
    static bool holds_run_time_expressions(std::string_view key) noexcept {
        return key == "alarms";
    }

    // Every expression of a point: its `when`, and each scalar stage 3 typed
    // as an expression (addresses, scaling).
    void check_expressions(NodeId point, const Scope& scope,
                           const Variables& variables) {
        for (const Entry& entry : document_->entries(point)) {
            const std::string_view key = text(entry.key);
            if (holds_run_time_expressions(key) || key == "logging" ||
                key == "name") {
                continue;
            }
            if (key == "when") {
                if (document_->kind(entry.value) == NodeKind::SCALAR) {
                    check_typed(entry.value, scope, variables,
                                ExpressionKind::CONFIGURATION, Wanted::BOOL);
                }
                continue;
            }
            check_numbers_below(entry.value, scope, variables);
        }
    }

    // NOLINTNEXTLINE(misc-no-recursion): bounded by the nesting limit of 32
    void check_numbers_below(NodeId node, const Scope& scope,
                             const Variables& variables) {
        switch (document_->kind(node)) {
            case NodeKind::SCALAR:
                if (file_->types->of(node) == ScalarType::EXPRESSION) {
                    check_typed(node, scope, variables,
                                ExpressionKind::CONFIGURATION, Wanted::NUMBER);
                }
                return;
            case NodeKind::MAP:
                for (const Entry& entry : document_->entries(node)) {
                    if (!holds_run_time_expressions(text(entry.key))) {
                        check_numbers_below(entry.value, scope, variables);
                    }
                }
                return;
            case NodeKind::SEQUENCE:
                for (const NodeId item : document_->items(node)) {
                    check_numbers_below(item, scope, variables);
                }
                return;
        }
    }

    // `chain_variables`: whether the variables of every `repeat` block of
    // the chain may be used, as in a point, which merging may join to a
    // point a `repeat` made.
    void check_typed(NodeId scalar, const Scope& scope,
                     const Variables& variables, ExpressionKind kind,
                     Wanted wanted, bool chain_variables = true) {
        ExpressionError syntax;
        const std::optional<Expression> tree =
            parse_expression(text(scalar), kind, syntax);
        if (!tree) {
            // A syntax error is stage 3's, and the tree passed it: a plain
            // literal of a `when` (true) is not an expression either.
            return;
        }
        const NameTypes names =
            [&scope, &variables, chain_variables](
                std::string_view name) -> std::optional<ValueType> {
            if (variables.contains(name)) {
                return ValueType::INT;
            }
            if (const auto found = scope.parameters.find(name);
                found != scope.parameters.end()) {
                return found->second;
            }
            if (chain_variables && scope.variables.contains(name)) {
                return ValueType::INT;
            }
            return std::nullopt;
        };
        // A name the template's chain upward does not have may come from a
        // child of the chain (4.5): it is checked at the instance (#103).
        // Without a template, no name can be defined, so each is CF-410.
        for (const ExprNode& node : tree->nodes) {
            if (node.op != ExprOp::REFERENCE || names(node.text)) {
                continue;
            }
            if (scope.bare) {
                report(scalar, node.column,
                       quote_text(node.text) +
                           " is not a parameter or a variable");
            }
            return;
        }
        TypeError problem;
        const std::optional<ValueType> result =
            configuration_type(*tree, names, problem);
        if (!result) {
            report(scalar, problem.column, reason_of(problem.message));
            return;
        }
        if (fits(*result, wanted)) {
            return;
        }
        const std::uint32_t column = tree->nodes.at(tree->root).column;
        const std::string reason = "the value must be " + wanted_name(wanted) +
                                   ", not " +
                                   std::string(value_type_name(*result));
        // 5.6 names CF-523 for the type of a `for` value; any other failure
        // of a configuration expression is CF-410 (2.4).
        if (wanted == Wanted::RANGE_OR_INT) {
            ExpressionSite site = expression_site(*document_, scalar, column);
            diagnostics_->error(CF_TYPE_MISMATCH, std::move(site.location),
                                "expression: " + reason + site.where);
            return;
        }
        report(scalar, column, reason);
    }

    // The reason of a type checker's message, without its "expression: ".
    static std::string reason_of(const std::string& message) {
        constexpr std::string_view PREFIX = "expression: ";
        return message.starts_with(PREFIX) ? message.substr(PREFIX.size())
                                           : message;
    }

    // CF-410 (2.4, 10.4): `expression {expr}: {reason}`, at the offending
    // character (D-109).
    void report(NodeId scalar, std::uint32_t column,
                const std::string& reason) {
        ExpressionSite site = expression_site(*document_, scalar, column);
        diagnostics_->error(
            CF_EXPRESSION_FAILS, std::move(site.location),
            "expression " + quote_text(text(scalar)) + ": " + reason);
    }

    static bool fits(ValueType type, Wanted wanted) noexcept {
        switch (wanted) {
            case Wanted::BOOL:
                return type == ValueType::BOOL;
            case Wanted::NUMBER:
                return type == ValueType::INT || type == ValueType::REAL;
            case Wanted::RANGE_OR_INT:
                return type == ValueType::RANGE || type == ValueType::INT;
        }
        return false;
    }

    static std::string wanted_name(Wanted wanted) {
        switch (wanted) {
            case Wanted::BOOL:
                return "BOOL";
            case Wanted::NUMBER:
                return "a number";
            case Wanted::RANGE_OR_INT:
                return "a range or an INT";
        }
        return "?";
    }

    std::span<const CheckedFile> files_;
    Diagnostics* diagnostics_;
    const CheckedFile* file_ = nullptr;
    const Document* document_ = nullptr;

    std::map<std::string, const CheckedFile*, std::less<>> templates_;
    // The scope of each template, built once (S4 of the review of #144).
    std::map<const CheckedFile*, Scope> scopes_;
    Definitions template_names_;
    Definitions channels_;
    Definitions devices_;
    Definitions areas_;
    Definitions enums_;
    Definitions policies_;
    Definitions tiers_;
    Definitions tasks_;
    Definitions programs_;
    Definitions interlocks_;
    Definitions outputs_;
    Definitions priorities_;
    Definitions classes_;
    Definitions tags_;
    bool tiers_given_ = false;
    bool tasks_given_ = false;
    bool priorities_given_ = false;
    bool classes_given_ = false;
};

}  // namespace

void check_references(std::span<const CheckedFile> files,
                      Diagnostics& diagnostics) {
    Checker(files, diagnostics).run();
}

}  // namespace scada::conf
