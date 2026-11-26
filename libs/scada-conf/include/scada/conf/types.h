#ifndef SCADA_CONF_TYPES_H
#define SCADA_CONF_TYPES_H

// The types stage 3 gives the scalars of a Document (configuration schemas
// contract 1.2; decision D-111). The Document stays immutable: the types are
// kept beside it, one byte per node, indexed by NodeId.

#include "scada/conf/node.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace scada::conf {

// The type a schema gave a scalar, or NONE if no schema typed it.
enum class ScalarType : std::uint8_t {
    NONE,
    STRING,      // any text: names, free text, enumerated words, addresses
    INTEGER,     // `-?(0|[1-9][0-9]*)` or `0x...`
    DECIMAL,     // a number written with a point or an exponent
    BOOLEAN,     // `true`, `false`
    DURATION,    // `100ms`, `5s` (2.2)
    SIZE,        // `2GiB` (2.2)
    PERCENTAGE,  // `0.5%` (2.2)
    EXPRESSION,  // text of an expression (2.4, 5.6), checked later
    // An expression of a key that takes an integer only (`register`,
    // `bit`, `divide_by`): its value must be an INT (B9 of the review of
    // #146).
    INTEGER_EXPRESSION,
};

// The types of the scalars of one Document, NONE until set.
class ScalarTypes {
public:
    ScalarTypes() = default;

    // NONE for every node of `document`.
    explicit ScalarTypes(const Document& document)
        : types_(document.size(), ScalarType::NONE) {}

    // The type of `node`; NONE for a node it does not know.
    [[nodiscard]] ScalarType of(NodeId node) const noexcept {
        return node < types_.size() ? types_[node] : ScalarType::NONE;
    }

    // Sets the type of `node`; a node it does not know is ignored, as the
    // table always covers its Document.
    void set(NodeId node, ScalarType type) noexcept {
        if (node < types_.size()) {
            types_[node] = type;
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return types_.size(); }

private:
    std::vector<ScalarType> types_;
};

}  // namespace scada::conf

#endif  // SCADA_CONF_TYPES_H
