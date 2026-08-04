#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace codexpets {

class JsonValue {
public:
    enum class Type : std::uint8_t { Null, Boolean, Number, String, Array, Object };
    using Array = std::vector<JsonValue>;
    using Object = std::vector<std::pair<std::string, JsonValue>>;

    JsonValue() = default;
    explicit JsonValue(bool value) : type_(Type::Boolean), boolean_(value) {}
    explicit JsonValue(double value) : type_(Type::Number), number_(value) {}
    explicit JsonValue(std::string value) : type_(Type::String), string_(std::move(value)) {}
    explicit JsonValue(Array value) : type_(Type::Array), array_(std::move(value)) {}
    explicit JsonValue(Object value) : type_(Type::Object), object_(std::move(value)) {}

    [[nodiscard]] Type type() const noexcept { return type_; }
    [[nodiscard]] bool is_null() const noexcept { return type_ == Type::Null; }
    [[nodiscard]] bool is_boolean() const noexcept { return type_ == Type::Boolean; }
    [[nodiscard]] bool is_number() const noexcept { return type_ == Type::Number; }
    [[nodiscard]] bool is_string() const noexcept { return type_ == Type::String; }
    [[nodiscard]] bool is_array() const noexcept { return type_ == Type::Array; }
    [[nodiscard]] bool is_object() const noexcept { return type_ == Type::Object; }

    [[nodiscard]] bool boolean(bool fallback = false) const noexcept {
        return is_boolean() ? boolean_ : fallback;
    }
    [[nodiscard]] double number(double fallback = 0.0) const noexcept {
        return is_number() ? number_ : fallback;
    }
    [[nodiscard]] const std::string& string() const noexcept { return string_; }
    [[nodiscard]] const Array& array() const noexcept { return array_; }
    [[nodiscard]] const Object& object() const noexcept { return object_; }
    [[nodiscard]] Array& array() noexcept { return array_; }
    [[nodiscard]] Object& object() noexcept { return object_; }

    [[nodiscard]] const JsonValue* get(std::string_view key) const noexcept;
    [[nodiscard]] const JsonValue* get_ascii_case_insensitive(std::string_view key) const noexcept;
    [[nodiscard]] std::string string_or(std::string fallback = {}) const;
    [[nodiscard]] int int_or(int fallback = 0) const noexcept;

private:
    Type type_{Type::Null};
    bool boolean_{};
    double number_{};
    std::string string_;
    Array array_;
    Object object_;
};

JsonValue parse_json(std::string_view input);
std::string json_escape(std::string_view input);

} // namespace codexpets
