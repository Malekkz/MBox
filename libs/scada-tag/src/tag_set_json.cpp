#include "scada/tag/tag_set_json.h"

#include "scada/common/result.h"
#include "scada/common/static_string.h"
#include "scada/common/status.h"
#include "scada/tag/handle.h"
#include "scada/tag/tag_set.h"
#include "scada/tag/types.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scada::tag {

namespace {

// The longest string the format holds is a tag name.
constexpr std::size_t MAX_STRING_LENGTH = MAX_TAG_NAME_LENGTH;
constexpr char FIRST_PRINTABLE = ' ';
constexpr char LAST_PRINTABLE = '~';
constexpr std::uint64_t DECIMAL_BASE = 10;
// Digits of the largest 32-bit number.
constexpr std::size_t MAX_DIGITS = 10;

// The keys of each object, in the order of the enumerations below.
constexpr std::array<std::string_view, 6> DOCUMENT_KEYS{
    "format", "heartbeat_check_ms", "processes", "owners", "queues", "tags"};
constexpr std::array<std::string_view, 4> PROCESS_KEYS{
    "name", "uid", "heartbeat_timeout_ms", "realtime"};
constexpr std::array<std::string_view, 3> OWNER_KEYS{"name", "process",
                                                     "change_ring_capacity"};
constexpr std::array<std::string_view, 4> QUEUE_KEYS{"name", "producer",
                                                     "consumer", "capacity"};
constexpr std::array<std::string_view, 6> TAG_KEYS{
    "name", "owner", "type", "retain", "writable_by_command", "retain_now"};

enum class DocumentKey : std::uint8_t {
    FORMAT,
    HEARTBEAT_CHECK_MS,
    PROCESSES,
    OWNERS,
    QUEUES,
    TAGS
};
enum class ProcessKey : std::uint8_t {
    NAME,
    UID,
    HEARTBEAT_TIMEOUT_MS,
    REALTIME
};
enum class OwnerKey : std::uint8_t { NAME, PROCESS, CHANGE_RING_CAPACITY };
enum class QueueKey : std::uint8_t { NAME, PRODUCER, CONSUMER, CAPACITY };
enum class TagKey : std::uint8_t {
    NAME,
    OWNER,
    TYPE,
    RETAIN,
    WRITABLE_BY_COMMAND,
    RETAIN_NOW
};

// The bit of `key` in a set of keys.
template <typename Key>
constexpr std::uint32_t bit(Key key) noexcept {
    return std::uint32_t{1} << static_cast<std::uint32_t>(key);
}

// The spelling of each tag type in the file.
struct TypeName {
    std::string_view text;
    TagType type = TagType::BOOL;
};
constexpr std::array TYPE_NAMES{
    TypeName{"bool", TagType::BOOL},     TypeName{"int64", TagType::INT64},
    TypeName{"uint64", TagType::UINT64}, TypeName{"float64", TagType::FLOAT64},
    TypeName{"string", TagType::STRING},
};

constexpr bool is_whitespace(char character) noexcept {
    return character == ' ' || character == '\t' || character == '\n' ||
           character == '\r';
}

constexpr bool is_digit(char character) noexcept {
    return character >= '0' && character <= '9';
}

// Reads one tag-set document. Every read is checked against the end of the
// text. The grammar has a fixed depth (document, list, entry), so the parser
// does not recurse.
class Parser {
public:
    explicit Parser(std::string_view text) noexcept : text_(text) {}

    // Parses the whole document.
    [[nodiscard]] Result<TagSet> parse() {
        TagSet tag_set;
        if (const Status status = parse_document(tag_set); !status.ok()) {
            return status;
        }
        skip_whitespace();
        if (position_ != text_.size()) {
            return fail("unexpected text after the document");
        }
        return tag_set;
    }

    // After a failed `parse`: "line <l>, column <c>: <message>", with the
    // name of the key concerned where there is one.
    [[nodiscard]] std::string describe_error(const Status& error) const {
        const std::string_view before = text_.substr(0, error_position_);
        const auto line = std::ranges::count(before, '\n') + 1;
        const std::size_t line_start = before.rfind('\n');
        const std::size_t column = line_start == std::string_view::npos
                                       ? before.size() + 1
                                       : before.size() - line_start;
        std::string result = "line " + std::to_string(line) + ", column " +
                             std::to_string(column) + ": ";
        result += error.message();
        if (!error_key_.empty()) {
            result += ": ";
            result += error_key_;
        }
        return result;
    }

private:
    // Records where the error is and returns it. `key` is a key of one of
    // the tables above, never text from the document.
    Status fail(StaticString message,
                StatusCode code = StatusCode::DATA_CORRUPT,
                std::string_view key = {}) noexcept {
        error_position_ = position_;
        error_key_ = key;
        return {code, message};
    }

    // As `fail`, for an error that starts at `position`.
    Status fail_at(std::size_t position, StaticString message,
                   StatusCode code = StatusCode::DATA_CORRUPT) noexcept {
        position_ = position;
        return fail(message, code);
    }

    [[nodiscard]] bool at_end() const noexcept {
        return position_ >= text_.size();
    }

    // The next character. Precondition: not at the end.
    [[nodiscard]] char peek() const { return text_.at(position_); }

    void skip_whitespace() noexcept {
        while (!at_end() && is_whitespace(text_[position_])) {
            ++position_;
        }
    }

    // Skips whitespace, then takes `expected` if it is next.
    [[nodiscard]] bool consume(char expected) noexcept {
        skip_whitespace();
        if (at_end() || text_[position_] != expected) {
            return false;
        }
        ++position_;
        return true;
    }

    // Takes `word` if the text continues with it.
    [[nodiscard]] bool consume_word(std::string_view word) noexcept {
        if (!text_.substr(position_).starts_with(word)) {
            return false;
        }
        position_ += word.size();
        return true;
    }

    // A string of printable ASCII without escape sequences, at most
    // MAX_STRING_LENGTH bytes. The view refers to the document.
    [[nodiscard]] Result<std::string_view> parse_string() {
        if (!consume('"')) {
            return fail("expected a string");
        }
        const std::size_t start = position_;
        while (!at_end() && peek() != '"') {
            const char character = peek();
            if (character == '\\') {
                return fail("escape sequences are not supported");
            }
            if (character < FIRST_PRINTABLE || character > LAST_PRINTABLE) {
                return fail("a string holds printable ASCII only");
            }
            if (position_ - start == MAX_STRING_LENGTH) {
                return fail_at(start, "string too long",
                               StatusCode::OUT_OF_RANGE);
            }
            ++position_;
        }
        if (at_end()) {
            return fail_at(start, "string without closing quote");
        }
        const std::string_view value = text_.substr(start, position_ - start);
        ++position_;
        return value;
    }

    // A whole number from 0 to 4,294,967,295 in decimal.
    [[nodiscard]] Result<std::uint32_t> parse_u32() {
        skip_whitespace();
        const std::size_t start = position_;
        std::uint64_t value = 0;
        while (!at_end() && is_digit(peek())) {
            if (position_ - start == MAX_DIGITS) {
                return fail_at(start, "number too large",
                               StatusCode::OUT_OF_RANGE);
            }
            value = (value * DECIMAL_BASE) +
                    static_cast<std::uint64_t>(peek() - '0');
            ++position_;
        }
        const std::size_t digits = position_ - start;
        if (digits == 0) {
            return fail("expected a whole number that is not negative");
        }
        if (digits > 1 && text_.at(start) == '0') {
            return fail_at(start, "number with a leading zero");
        }
        if (!at_end() && (peek() == '.' || peek() == 'e' || peek() == 'E')) {
            return fail_at(start, "expected a whole number");
        }
        if (value > std::numeric_limits<std::uint32_t>::max()) {
            return fail_at(start, "number too large", StatusCode::OUT_OF_RANGE);
        }
        return static_cast<std::uint32_t>(value);
    }

    [[nodiscard]] Result<bool> parse_bool() {
        skip_whitespace();
        if (consume_word("true")) {
            return true;
        }
        if (consume_word("false")) {
            return false;
        }
        return fail("expected true or false");
    }

    // Each of the three stores a parsed value in `target`.
    [[nodiscard]] Status read(std::string& target) {
        const Result<std::string_view> value = parse_string();
        if (value.ok()) {
            target = value.value();
        }
        return value.status();
    }
    [[nodiscard]] Status read(std::uint32_t& target) {
        const Result<std::uint32_t> value = parse_u32();
        if (value.ok()) {
            target = value.value();
        }
        return value.status();
    }
    [[nodiscard]] Status read(bool& target) {
        const Result<bool> value = parse_bool();
        if (value.ok()) {
            target = value.value();
        }
        return value.status();
    }

    // Parses one `"key": value` of an object whose keys are `keys`, calling
    // `member(index of the key)` to parse the value, and adds the key to
    // `seen`. A key that is not in `keys` or is already in `seen` is an
    // error.
    template <typename Member>
    [[nodiscard]] Status parse_member(std::span<const std::string_view> keys,
                                      std::uint32_t& seen, Member& member) {
        skip_whitespace();
        const std::size_t key_position = position_;
        const Result<std::string_view> key = parse_string();
        if (!key.ok()) {
            return key.status();
        }
        const auto found = std::ranges::find(keys, key.value());
        if (found == keys.end()) {
            return fail_at(key_position, "unknown key");
        }
        const auto index = static_cast<std::uint32_t>(found - keys.begin());
        const std::uint32_t key_bit = std::uint32_t{1} << index;
        if ((seen & key_bit) != 0) {
            return fail_at(key_position, "key appears twice");
        }
        seen |= key_bit;
        if (!consume(':')) {
            return fail("expected ':' after a key");
        }
        return member(index);
    }

    // Parses an object whose keys are `keys`. For each member it calls
    // `member(index of the key)`, which must parse the value. A key that is
    // not in `keys` or appears twice is an error, and so is the absence of
    // a key whose bit is set in `required`. Returns the keys seen in `seen`.
    template <typename Member>
    [[nodiscard]] Status parse_object(std::span<const std::string_view> keys,
                                      std::uint32_t required,
                                      std::uint32_t& seen, Member member) {
        seen = 0;
        if (!consume('{')) {
            return fail("expected '{'");
        }
        // An empty object has no first member.
        const bool has_members = !consume('}');
        bool more = has_members;
        while (more) {
            if (const Status status = parse_member(keys, seen, member);
                !status.ok()) {
                return status;
            }
            more = consume(',');
        }
        if (has_members && !consume('}')) {
            return fail("expected ',' or '}'");
        }
        for (std::size_t i = 0; i < keys.size(); ++i) {
            const std::uint32_t key_bit = std::uint32_t{1} << i;
            if ((required & key_bit) != 0 && (seen & key_bit) == 0) {
                return fail("a required key is missing",
                            StatusCode::DATA_CORRUPT, keys[i]);
            }
        }
        return {};
    }

    // Parses a list of at most `max_count` elements, calling `element()` to
    // parse each.
    template <typename Element>
    [[nodiscard]] Status parse_list(std::size_t max_count, Element element) {
        if (!consume('[')) {
            return fail("expected '['");
        }
        if (consume(']')) {
            return {};
        }
        std::size_t count = 0;
        bool more = true;
        while (more) {
            if (count == max_count) {
                skip_whitespace();
                return fail("too many entries in a list",
                            StatusCode::OUT_OF_RANGE);
            }
            ++count;
            if (const Status status = element(); !status.ok()) {
                return status;
            }
            more = consume(',');
        }
        if (!consume(']')) {
            return fail("expected ',' or ']'");
        }
        return {};
    }

    [[nodiscard]] Status parse_process(ProcessDefinition& process) {
        std::uint32_t seen = 0;
        const Status status = parse_object(
            PROCESS_KEYS, bit(ProcessKey::NAME) | bit(ProcessKey::UID), seen,
            [this, &process](std::uint32_t key) {
                switch (static_cast<ProcessKey>(key)) {
                    case ProcessKey::NAME:
                        return read(process.name);
                    case ProcessKey::UID:
                        return read(process.uid);
                    case ProcessKey::HEARTBEAT_TIMEOUT_MS:
                        return read(process.heartbeat_timeout_ms);
                    case ProcessKey::REALTIME:
                        return read(process.realtime);
                }
                return Status{};
            });
        if (status.ok() && process.realtime &&
            (seen & bit(ProcessKey::HEARTBEAT_TIMEOUT_MS)) == 0) {
            process.heartbeat_timeout_ms =
                DEFAULT_REALTIME_HEARTBEAT_TIMEOUT_MS;
        }
        return status;
    }

    [[nodiscard]] Status parse_owner(OwnerDefinition& owner) {
        std::uint32_t seen = 0;
        return parse_object(OWNER_KEYS,
                            bit(OwnerKey::NAME) | bit(OwnerKey::PROCESS), seen,
                            [this, &owner](std::uint32_t key) {
                                switch (static_cast<OwnerKey>(key)) {
                                    case OwnerKey::NAME:
                                        return read(owner.name);
                                    case OwnerKey::PROCESS:
                                        return read(owner.process);
                                    case OwnerKey::CHANGE_RING_CAPACITY:
                                        return read(owner.change_ring_capacity);
                                }
                                return Status{};
                            });
    }

    [[nodiscard]] Status parse_queue(QueueDefinition& queue) {
        std::uint32_t seen = 0;
        return parse_object(QUEUE_KEYS,
                            bit(QueueKey::NAME) | bit(QueueKey::PRODUCER) |
                                bit(QueueKey::CONSUMER),
                            seen, [this, &queue](std::uint32_t key) {
                                switch (static_cast<QueueKey>(key)) {
                                    case QueueKey::NAME:
                                        return read(queue.name);
                                    case QueueKey::PRODUCER:
                                        return read(queue.producer);
                                    case QueueKey::CONSUMER:
                                        return read(queue.consumer);
                                    case QueueKey::CAPACITY:
                                        return read(queue.capacity);
                                }
                                return Status{};
                            });
    }

    [[nodiscard]] Status parse_type(TagType& type) {
        skip_whitespace();
        const std::size_t start = position_;
        const Result<std::string_view> text = parse_string();
        if (!text.ok()) {
            return text.status();
        }
        const auto* const found =
            std::ranges::find(TYPE_NAMES, text.value(), &TypeName::text);
        if (found == TYPE_NAMES.end()) {
            return fail_at(start, "unknown tag type");
        }
        type = found->type;
        return {};
    }

    [[nodiscard]] Status parse_tag(TagDefinition& tag) {
        std::uint32_t seen = 0;
        return parse_object(
            TAG_KEYS,
            bit(TagKey::NAME) | bit(TagKey::OWNER) | bit(TagKey::TYPE), seen,
            [this, &tag](std::uint32_t key) {
                switch (static_cast<TagKey>(key)) {
                    case TagKey::NAME:
                        return read(tag.name);
                    case TagKey::OWNER:
                        return read(tag.owner);
                    case TagKey::TYPE:
                        return parse_type(tag.type);
                    case TagKey::RETAIN:
                        return read(tag.retain);
                    case TagKey::WRITABLE_BY_COMMAND:
                        return read(tag.writable_by_command);
                    case TagKey::RETAIN_NOW:
                        return read(tag.retain_now);
                }
                return Status{};
            });
    }

    [[nodiscard]] Status parse_format() {
        skip_whitespace();
        const std::size_t start = position_;
        const Result<std::uint32_t> format = parse_u32();
        if (!format.ok()) {
            return format.status();
        }
        if (format.value() != TAG_SET_JSON_FORMAT) {
            return fail_at(start, "unsupported format version",
                           StatusCode::UNSUPPORTED);
        }
        return {};
    }

    // Parses a list of entries into `entries`, each with `parse_entry`.
    template <typename Definition>
    [[nodiscard]] Status parse_entries(
        std::vector<Definition>& entries, std::size_t max_count,
        Status (Parser::*parse_entry)(Definition&)) {
        return parse_list(max_count, [this, &entries, parse_entry] {
            Definition entry;
            const Status status = (this->*parse_entry)(entry);
            if (status.ok()) {
                entries.push_back(std::move(entry));
            }
            return status;
        });
    }

    [[nodiscard]] Status parse_document(TagSet& tag_set) {
        std::uint32_t seen = 0;
        return parse_object(
            DOCUMENT_KEYS, bit(DocumentKey::FORMAT), seen,
            [this, &tag_set](std::uint32_t key) {
                switch (static_cast<DocumentKey>(key)) {
                    case DocumentKey::FORMAT:
                        return parse_format();
                    case DocumentKey::HEARTBEAT_CHECK_MS:
                        return read(tag_set.heartbeat_check_ms);
                    case DocumentKey::PROCESSES:
                        return parse_entries(tag_set.processes, MAX_PROCESSES,
                                             &Parser::parse_process);
                    case DocumentKey::OWNERS:
                        return parse_entries(tag_set.owners, MAX_OWNERS,
                                             &Parser::parse_owner);
                    case DocumentKey::QUEUES:
                        return parse_entries(tag_set.queues, MAX_QUEUES,
                                             &Parser::parse_queue);
                    case DocumentKey::TAGS:
                        return parse_entries(tag_set.tags, MAX_TAGS,
                                             &Parser::parse_tag);
                }
                return Status{};
            });
    }

    // The document. The caller of `parse_tag_set_json` keeps it alive for
    // the life of the parser, which does not outlive that call.
    std::string_view text_;
    std::size_t position_ = 0;
    std::size_t error_position_ = 0;
    // Refers to one of the constant key tables.
    std::string_view error_key_;
};

}  // namespace

Result<TagSet> parse_tag_set_json(std::string_view text, std::string& detail) {
    if (text.size() > MAX_TAG_SET_JSON_BYTES) {
        detail = "the document is larger than " +
                 std::to_string(MAX_TAG_SET_JSON_BYTES) + " bytes";
        return Status{StatusCode::OUT_OF_RANGE, "tag-set document too large"};
    }
    Parser parser(text);
    Result<TagSet> tag_set = parser.parse();
    if (!tag_set.ok()) {
        detail = parser.describe_error(tag_set.status());
    }
    return tag_set;
}

}  // namespace scada::tag
