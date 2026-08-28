#include "Json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace tfrs {
namespace json {

namespace {

constexpr int kMaxDepth = 8;

class Parser {
public:
    explicit Parser(const std::string& text) : m_text(text) {}

    bool parseValue(Value& out, int depth) {
        if (depth > kMaxDepth) return false;
        skipWhitespace();
        if (m_pos >= m_text.size()) return false;

        const char c = m_text[m_pos];
        switch (c) {
            case '{': return parseObject(out, depth);
            case '[': return parseArray(out, depth);
            case '"': {
                out.type = Value::Type::String;
                return parseString(out.string);
            }
            case 't':
                if (!literal("true")) return false;
                out.type = Value::Type::Bool;
                out.boolean = true;
                return true;
            case 'f':
                if (!literal("false")) return false;
                out.type = Value::Type::Bool;
                out.boolean = false;
                return true;
            case 'n':
                if (!literal("null")) return false;
                out.type = Value::Type::Null;
                return true;
            default: return parseNumber(out);
        }
    }

    void skipWhitespace() {
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++m_pos;
            } else {
                break;
            }
        }
    }

    bool atEndAfterWhitespace() {
        skipWhitespace();
        return m_pos >= m_text.size();
    }

private:
    bool literal(const char* lit) {
        const size_t len = std::char_traits<char>::length(lit);
        if (m_text.compare(m_pos, len, lit) != 0) return false;
        m_pos += len;
        return true;
    }

    bool parseObject(Value& out, int depth) {
        out.type = Value::Type::Object;
        ++m_pos;  // '{'
        skipWhitespace();
        if (m_pos < m_text.size() && m_text[m_pos] == '}') {
            ++m_pos;
            return true;
        }
        while (true) {
            skipWhitespace();
            if (m_pos >= m_text.size() || m_text[m_pos] != '"') return false;
            std::string key;
            if (!parseString(key)) return false;
            skipWhitespace();
            if (m_pos >= m_text.size() || m_text[m_pos] != ':') return false;
            ++m_pos;
            Value child;
            if (!parseValue(child, depth + 1)) return false;
            out.members.emplace_back(std::move(key), std::move(child));
            skipWhitespace();
            if (m_pos >= m_text.size()) return false;
            if (m_text[m_pos] == ',') {
                ++m_pos;
                continue;
            }
            if (m_text[m_pos] == '}') {
                ++m_pos;
                return true;
            }
            return false;
        }
    }

    bool parseArray(Value& out, int depth) {
        out.type = Value::Type::Array;
        ++m_pos;  // '['
        skipWhitespace();
        if (m_pos < m_text.size() && m_text[m_pos] == ']') {
            ++m_pos;
            return true;
        }
        while (true) {
            Value child;
            if (!parseValue(child, depth + 1)) return false;
            out.items.push_back(std::move(child));
            skipWhitespace();
            if (m_pos >= m_text.size()) return false;
            if (m_text[m_pos] == ',') {
                ++m_pos;
                continue;
            }
            if (m_text[m_pos] == ']') {
                ++m_pos;
                return true;
            }
            return false;
        }
    }

    bool parseString(std::string& out) {
        out.clear();
        if (m_pos >= m_text.size() || m_text[m_pos] != '"') return false;
        ++m_pos;
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos++];
            if (c == '"') return true;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (m_pos >= m_text.size()) return false;
            const char esc = m_text[m_pos++];
            switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (m_pos + 4 > m_text.size()) return false;
                    unsigned int code = 0;
                    for (int i = 0; i < 4; ++i) {
                        const char h = m_text[m_pos + static_cast<size_t>(i)];
                        code <<= 4;
                        if (h >= '0' && h <= '9') {
                            code |= static_cast<unsigned int>(h - '0');
                        } else if (h >= 'a' && h <= 'f') {
                            code |= static_cast<unsigned int>(h - 'a' + 10);
                        } else if (h >= 'A' && h <= 'F') {
                            code |= static_cast<unsigned int>(h - 'A' + 10);
                        } else {
                            return false;
                        }
                    }
                    m_pos += 4;
                    appendUtf8(out, code);
                    break;
                }
                default: return false;
            }
        }
        return false;  // unterminated
    }

    static void appendUtf8(std::string& out, unsigned int code) {
        // Surrogate halves are emitted as U+FFFD; the bridge never uses them.
        if (code >= 0xD800 && code <= 0xDFFF) code = 0xFFFD;
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    bool parseNumber(Value& out) {
        const size_t start = m_pos;
        if (m_pos < m_text.size() && (m_text[m_pos] == '-' || m_text[m_pos] == '+')) ++m_pos;
        bool anyDigit = false;
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos];
            if ((c >= '0' && c <= '9')) {
                anyDigit = true;
                ++m_pos;
            } else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                ++m_pos;
            } else {
                break;
            }
        }
        if (!anyDigit) return false;
        const std::string token = m_text.substr(start, m_pos - start);
        out.type = Value::Type::Number;
        out.number = std::strtod(token.c_str(), nullptr);
        return true;
    }

    const std::string& m_text;
    size_t m_pos = 0;
};

}  // namespace

const Value* Value::find(const std::string& key) const {
    if (type != Type::Object) return nullptr;
    for (size_t i = 0; i < members.size(); ++i) {
        if (members[i].first == key) return &members[i].second;
    }
    return nullptr;
}

std::string Value::asString(const std::string& fallback) const {
    if (type == Type::String) return string;
    return fallback;
}

bool Value::asBool(bool fallback) const {
    if (type == Type::Bool) return boolean;
    if (type == Type::Number) return number != 0.0;
    return fallback;
}

double Value::asNumber(double fallback) const {
    if (type == Type::Number) return number;
    if (type == Type::Bool) return boolean ? 1.0 : 0.0;
    return fallback;
}

std::string Value::getString(const std::string& key, const std::string& fallback) const {
    const Value* v = find(key);
    return v ? v->asString(fallback) : fallback;
}

bool Value::getBool(const std::string& key, bool fallback) const {
    const Value* v = find(key);
    return v ? v->asBool(fallback) : fallback;
}

double Value::getNumber(const std::string& key, double fallback) const {
    const Value* v = find(key);
    return v ? v->asNumber(fallback) : fallback;
}

bool parse(const std::string& text, Value& out) {
    out = Value();
    Parser parser(text);
    if (!parser.parseValue(out, 0)) return false;
    return parser.atEndAfterWhitespace();
}

std::string quote(const std::string& raw) {
    std::string out;
    out.reserve(raw.size() + 2);
    out.push_back('"');
    for (size_t i = 0; i < raw.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(raw[i]);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned int>(c));
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    out.push_back('"');
    return out;
}

std::string number(double value, int decimals) {
    if (!(value == value)) value = 0.0;                    // NaN
    if (value > 1e12) value = 1e12;                        // +inf / absurd
    if (value < -1e12) value = -1e12;                      // -inf / absurd
    if (decimals < 0) decimals = 0;
    if (decimals > 9) decimals = 9;

    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, value);

    std::string out(buf);
    // Trim redundant trailing zeros to keep the messages small.
    if (out.find('.') != std::string::npos) {
        while (!out.empty() && out.back() == '0') out.pop_back();
        if (!out.empty() && out.back() == '.') out.pop_back();
    }
    if (out.empty() || out == "-") out = "0";
    return out;
}

}  // namespace json
}  // namespace tfrs
