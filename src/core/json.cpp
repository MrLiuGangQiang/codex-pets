#include "json.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <stdexcept>

namespace codexpets {
namespace {

bool ascii_iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        auto left = static_cast<unsigned char>(a[i]);
        auto right = static_cast<unsigned char>(b[i]);
        if (left >= 'A' && left <= 'Z') left = static_cast<unsigned char>(left + 32);
        if (right >= 'A' && right <= 'Z') right = static_cast<unsigned char>(right + 32);
        if (left != right) return false;
    }
    return true;
}

void append_utf8(std::string& output, std::uint32_t codepoint) {
    if (codepoint <= 0x7f) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ff) {
        output.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (codepoint <= 0xffff) {
        output.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (codepoint <= 0x10ffff) {
        output.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else {
        append_utf8(output, 0xfffd);
    }
}

int hex_value(char ch) noexcept {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

class Parser {
public:
    explicit Parser(std::string_view input) : input_(input) {
        if (input_.size() >= 3 && static_cast<unsigned char>(input_[0]) == 0xef &&
            static_cast<unsigned char>(input_[1]) == 0xbb &&
            static_cast<unsigned char>(input_[2]) == 0xbf) {
            position_ = 3;
        }
    }

    JsonValue parse() {
        skip_space();
        auto result = parse_value(0);
        skip_space();
        if (position_ != input_.size()) fail("JSON contains trailing characters");
        return result;
    }

private:
    JsonValue parse_value(int depth) {
        if (depth > 64) fail("JSON nesting is too deep");
        skip_space();
        if (position_ >= input_.size()) fail("Unexpected end of JSON");
        switch (input_[position_]) {
            case '{': return parse_object(depth + 1);
            case '[': return parse_array(depth + 1);
            case '"': return JsonValue(parse_string());
            case 't': consume_literal("true"); return JsonValue(true);
            case 'f': consume_literal("false"); return JsonValue(false);
            case 'n': consume_literal("null"); return JsonValue();
            default:
                if (input_[position_] == '-' || (input_[position_] >= '0' && input_[position_] <= '9')) {
                    return JsonValue(parse_number());
                }
                fail("Unexpected JSON token");
        }
    }

    JsonValue parse_object(int depth) {
        ++position_;
        JsonValue::Object object;
        skip_space();
        if (consume('}')) return JsonValue(std::move(object));
        while (true) {
            skip_space();
            if (position_ >= input_.size() || input_[position_] != '"') {
                fail("JSON object key must be a string");
            }
            auto key = parse_string();
            skip_space();
            if (!consume(':')) fail("Missing ':' after JSON object key");
            object.emplace_back(std::move(key), parse_value(depth));
            skip_space();
            if (consume('}')) break;
            if (!consume(',')) fail("Missing ',' in JSON object");
        }
        return JsonValue(std::move(object));
    }

    JsonValue parse_array(int depth) {
        ++position_;
        JsonValue::Array array;
        skip_space();
        if (consume(']')) return JsonValue(std::move(array));
        while (true) {
            array.push_back(parse_value(depth));
            skip_space();
            if (consume(']')) break;
            if (!consume(',')) fail("Missing ',' in JSON array");
        }
        return JsonValue(std::move(array));
    }

    std::string parse_string() {
        if (!consume('"')) fail("Missing opening quote");
        std::string output;
        output.reserve(32);
        while (position_ < input_.size()) {
            const auto ch = static_cast<unsigned char>(input_[position_++]);
            if (ch == '"') return output;
            if (ch < 0x20) fail("Control character in JSON string");
            if (ch != '\\') {
                output.push_back(static_cast<char>(ch));
                continue;
            }
            if (position_ >= input_.size()) fail("Incomplete JSON escape");
            const auto escaped = input_[position_++];
            switch (escaped) {
                case '"': output.push_back('"'); break;
                case '\\': output.push_back('\\'); break;
                case '/': output.push_back('/'); break;
                case 'b': output.push_back('\b'); break;
                case 'f': output.push_back('\f'); break;
                case 'n': output.push_back('\n'); break;
                case 'r': output.push_back('\r'); break;
                case 't': output.push_back('\t'); break;
                case 'u': {
                    auto codepoint = parse_hex4();
                    if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                        if (position_ + 2 > input_.size() || input_[position_] != '\\' ||
                            input_[position_ + 1] != 'u') {
                            fail("Missing low surrogate");
                        }
                        position_ += 2;
                        const auto low = parse_hex4();
                        if (low < 0xdc00 || low > 0xdfff) fail("Invalid low surrogate");
                        codepoint = 0x10000u + ((codepoint - 0xd800u) << 10u) + (low - 0xdc00u);
                    } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
                        fail("Unexpected low surrogate");
                    }
                    append_utf8(output, codepoint);
                    break;
                }
                default: fail("Unsupported JSON escape");
            }
        }
        fail("Unterminated JSON string");
    }

    std::uint32_t parse_hex4() {
        if (position_ + 4 > input_.size()) fail("Incomplete Unicode escape");
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const auto digit = hex_value(input_[position_++]);
            if (digit < 0) fail("Invalid Unicode escape");
            value = (value << 4u) | static_cast<std::uint32_t>(digit);
        }
        return value;
    }

    double parse_number() {
        const auto start = position_;
        if (consume('-') && position_ >= input_.size()) fail("Incomplete JSON number");
        if (consume('0')) {
            if (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
                fail("Leading zero in JSON number");
            }
        } else {
            if (position_ >= input_.size() || input_[position_] < '1' || input_[position_] > '9') {
                fail("Invalid JSON number");
            }
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
                ++position_;
            }
        }
        if (consume('.')) {
            if (position_ >= input_.size() || input_[position_] < '0' || input_[position_] > '9') {
                fail("Invalid fractional JSON number");
            }
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
                ++position_;
            }
        }
        if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-')) {
                ++position_;
            }
            if (position_ >= input_.size() || input_[position_] < '0' || input_[position_] > '9') {
                fail("Invalid exponent in JSON number");
            }
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
                ++position_;
            }
        }
        const std::string value(input_.substr(start, position_ - start));
        char* end = nullptr;
        const auto parsed = std::strtod(value.c_str(), &end);
        if (end != value.c_str() + value.size() || !std::isfinite(parsed)) {
            fail("JSON number is out of range");
        }
        return parsed;
    }

    void consume_literal(std::string_view value) {
        if (input_.substr(position_, value.size()) != value) fail("Invalid JSON literal");
        position_ += value.size();
    }

    bool consume(char expected) noexcept {
        if (position_ < input_.size() && input_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    void skip_space() noexcept {
        while (position_ < input_.size()) {
            const auto ch = input_[position_];
            if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') break;
            ++position_;
        }
    }

    [[noreturn]] void fail(const char* message) const {
        throw std::runtime_error(std::string(message) + " at byte " + std::to_string(position_));
    }

    std::string_view input_;
    std::size_t position_{};
};

} // namespace

const JsonValue* JsonValue::get(std::string_view key) const noexcept {
    if (!is_object()) return nullptr;
    for (const auto& [name, value] : object_) {
        if (name == key) return &value;
    }
    return nullptr;
}

const JsonValue* JsonValue::get_ascii_case_insensitive(std::string_view key) const noexcept {
    if (!is_object()) return nullptr;
    for (const auto& [name, value] : object_) {
        if (ascii_iequals(name, key)) return &value;
    }
    return nullptr;
}

std::string JsonValue::string_or(std::string fallback) const {
    if (is_string()) return string_;
    if (is_boolean()) return boolean_ ? "true" : "false";
    if (is_number()) return std::to_string(number_);
    return fallback;
}

int JsonValue::int_or(int fallback) const noexcept {
    if (!is_number() || number_ < static_cast<double>(std::numeric_limits<int>::min()) ||
        number_ > static_cast<double>(std::numeric_limits<int>::max())) return fallback;
    return static_cast<int>(number_);
}

JsonValue parse_json(std::string_view input) {
    return Parser(input).parse();
}

std::string json_escape(std::string_view input) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string output;
    output.reserve(input.size() + 8);
    for (const auto raw : input) {
        const auto ch = static_cast<unsigned char>(raw);
        switch (ch) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (ch < 0x20) {
                    output += "\\u00";
                    output.push_back(hex[(ch >> 4) & 0xf]);
                    output.push_back(hex[ch & 0xf]);
                } else {
                    output.push_back(static_cast<char>(ch));
                }
        }
    }
    return output;
}

} // namespace codexpets
