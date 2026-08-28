// Minimal hand written JSON support.
//
// Deliberately NOT a general purpose library: it only has to cope with the
// flat, line delimited schema of docs/protocol-ipc-bridge.md (strings,
// numbers, bools, null, flat arrays and one level of nested objects inside
// arrays). No external dependency is pulled in on purpose so that the build
// stays offline friendly.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace tfrs {
namespace json {

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Value> items;                          // Array
    std::vector<std::pair<std::string, Value>> members;  // Object

    bool isNull() const { return type == Type::Null; }
    bool isObject() const { return type == Type::Object; }
    bool isArray() const { return type == Type::Array; }

    // Returns nullptr when the key does not exist or this is not an object.
    const Value* find(const std::string& key) const;

    std::string asString(const std::string& fallback = std::string()) const;
    bool asBool(bool fallback = false) const;
    double asNumber(double fallback = 0.0) const;

    // Convenience accessors that already do the find() + conversion.
    std::string getString(const std::string& key,
                          const std::string& fallback = std::string()) const;
    bool getBool(const std::string& key, bool fallback = false) const;
    double getNumber(const std::string& key, double fallback = 0.0) const;
};

// Parses one JSON document. Returns false on any syntax error; `out` is then
// left in an unspecified (but valid) state.
bool parse(const std::string& text, Value& out);

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

// Escapes a string and wraps it in quotes.
std::string quote(const std::string& raw);

// Formats a float with a fixed, compact precision and never emits
// "inf"/"nan" (those would produce invalid JSON).
std::string number(double value, int decimals = 4);

}  // namespace json
}  // namespace tfrs
