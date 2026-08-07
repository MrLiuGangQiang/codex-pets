#include "xiaomi_speaker.h"

#include "json.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <future>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <random>
#include <sstream>
#include <span>
#include <stdexcept>
#include <unordered_map>

namespace codexpets {
namespace {

constexpr char kMinaBase[] = "https://api2.mina.mi.com";
constexpr char kUserAgent[] = "MICO/AndroidApp/@SHIP.TO.2A2FE0D7@/2.4.40";
constexpr char kPassportUserAgent[] = "Dalvik/2.1.0 (Linux; U; Android 10; RMX2111 Build/QP1A.190711.020) APP/xiaomi.mico APPV/2004040 MK/Uk1YMjExMQ== PassportSDK/3.8.3 passport-ui/3.8.3";
constexpr char kAccept[] = "application/json, text/plain, */*";
constexpr char kMiotBase[] = "https://api.io.mi.com/app";
constexpr char kMiotUserAgent[] =
    "iOS-14.4-6.0.103-iPhone12,3--D7744744F7AF32F0544445285880DD63E47D9BE9-8816080-84A3F44E137B71AE-iPhone";
constexpr char kMiotProtocolHeader[] = "x-xiaomi-protocal-flag-cli";
constexpr char kMiotProtocolValue[] = "PROTOCAL-HTTP2";
constexpr char kStoredMiotSsecurity[] = "codexpetsMiotSsecurity";
constexpr char kStoredMiotServiceToken[] = "codexpetsMiotServiceToken";

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string url_encode(std::string_view input) {
    std::ostringstream out;
    out << std::uppercase << std::hex;
    for (const unsigned char ch : input) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            out << static_cast<char>(ch);
        } else {
            out << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(ch);
        }
    }
    return out.str();
}

std::string random_device_id() {
    std::mt19937_64 generator(std::random_device{}());
    std::ostringstream out;
    out << "app_ios_" << std::hex << generator() << generator();
    auto result = out.str();
    if (result.size() > 24) result.resize(24);
    return result;
}

std::string cookie_value(std::string_view cookies, std::string_view wanted_name) {
    std::size_t start = 0;
    while (start <= cookies.size()) {
        const auto end = cookies.find(';', start);
        auto part = cookies.substr(start, end == std::string_view::npos ? end : end - start);
        while (!part.empty() && std::isspace(static_cast<unsigned char>(part.front()))) part.remove_prefix(1);
        while (!part.empty() && std::isspace(static_cast<unsigned char>(part.back()))) part.remove_suffix(1);
        const auto equal = part.find('=');
        if (equal != std::string_view::npos && part.substr(0, equal) == wanted_name) {
            return std::string(part.substr(equal + 1));
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return {};
}

bool has_cookie(std::string_view cookies, std::string_view wanted_name) {
    return !cookie_value(cookies, wanted_name).empty();
}

void merge_cookie(std::string& cookies, std::string_view name, std::string_view value) {
    if (value.empty()) return;
    std::vector<std::pair<std::string, std::string>> values;
    std::size_t start = 0;
    while (start < cookies.size()) {
        const auto end = cookies.find(';', start);
        const auto part = cookies.substr(start, end == std::string::npos ? end : end - start);
        const auto equal = part.find('=');
        if (equal != std::string::npos) values.emplace_back(part.substr(0, equal), part.substr(equal + 1));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    bool replaced = false;
    for (auto& item : values) {
        if (item.first == name) { item.second = std::string(value); replaced = true; }
    }
    if (!replaced) values.emplace_back(std::string(name), std::string(value));
    cookies.clear();
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) cookies += "; ";
        cookies += values[i].first + "=" + values[i].second;
    }
}

void capture_cookies(std::string& cookies, const XiaoAiHttpResponse& response, bool include_service_token = true) {
    for (const auto& [name, value] : response.headers) {
        if (lower(name) != "set-cookie") continue;
        const auto equal = value.find('=');
        if (equal == std::string::npos) continue;
        const auto cookie_name = value.substr(0, equal);
        // account.xiaomi.com also issues a cookie called serviceToken. It is not
        // accepted by api2.mina.mi.com and must never satisfy MiNA validation.
        if (!include_service_token && cookie_name == "serviceToken") continue;
        const auto end = value.find(';', equal + 1);
        merge_cookie(cookies, cookie_name, value.substr(equal + 1, end == std::string::npos ? end : end - equal - 1));
    }
}

std::string json_text(const JsonValue& object, std::string_view key) {
    const auto* value = object.get_ascii_case_insensitive(key);
    return value && value->is_string() ? value->string() : std::string{};
}

const JsonValue* json_property(const JsonValue& object, std::string_view key) {
    return object.get_ascii_case_insensitive(key);
}

bool is_json_value_delimiter(char value) {
    return value == ',' || value == '}' || value == ']' ||
           std::isspace(static_cast<unsigned char>(value));
}

std::string quote_large_integer_values(std::string_view json) {
    std::string result;
    result.reserve(json.size());
    bool inside_string = false;
    bool escaped = false;

    for (std::size_t index = 0; index < json.size();) {
        const char value = json[index];
        if (inside_string) {
            result += value;
            if (escaped) escaped = false;
            else if (value == '\\') escaped = true;
            else if (value == '"') inside_string = false;
            ++index;
            continue;
        }
        if (value == '"') {
            inside_string = true;
            result += value;
            ++index;
            continue;
        }
        if (value != ':') {
            result += value;
            ++index;
            continue;
        }

        result += value;
        ++index;
        while (index < json.size() && std::isspace(static_cast<unsigned char>(json[index]))) {
            result += json[index++];
        }

        const auto number_start = index;
        if (index < json.size() && json[index] == '-') ++index;
        const auto digit_start = index;
        while (index < json.size() && std::isdigit(static_cast<unsigned char>(json[index]))) ++index;
        const auto digit_count = index - digit_start;
        const bool is_integer = digit_count >= 9 &&
            (index == json.size() || is_json_value_delimiter(json[index]));
        if (!is_integer) {
            result.append(json.substr(number_start, index - number_start));
            continue;
        }

        result += '"';
        result.append(json.substr(number_start, index - number_start));
        result += '"';
    }
    return result;
}

JsonValue parse_xiaomi_response(std::string body) {
    body.erase(0, body.find_first_not_of(" \r\n\t"));
    if (body.rfind("&&&START&&&", 0) == 0) body.erase(0, 11);
    const auto start = body.find('{');
    if (start != std::string::npos) body.erase(0, start);
    // Xiaomi returns nonce/userId values as unquoted integers that can exceed
    // JSON's exact double range. Preserve those values as strings.
    return parse_json(quote_large_integer_values(body));
}

bool api_ok(const JsonValue& root, std::string* error) {
    const auto* code = json_property(root, "code");
    if (!code) return true;
    if ((code->is_number() && code->int_or(-1) == 0) ||
        (code->is_string() && code->string() == "0")) return true;
    if (error) {
        const auto description = json_text(root, "desc");
        *error = description.empty() ? "小米接口返回错误" : description;
    }
    return false;
}

XiaoAiHttpResponse request(const XiaoAiHttpTransport& transport, std::string method,
                           std::string url, std::string body, std::string cookies,
                           std::string content_type = {},
                           std::string user_agent = kUserAgent,
                           bool follow_redirects = true) {
    XiaoAiHttpRequest request{std::move(method), std::move(url), std::move(body),
                               {{"User-Agent", std::move(user_agent)},
                                {"Accept", kAccept},
                                {"Cookie", std::move(cookies)}},
                               follow_redirects};
    if (!content_type.empty()) {
        request.headers.emplace_back("Content-Type", std::move(content_type));
    }
    return transport(request);
}

std::string query(std::string base,
                   const std::vector<std::pair<std::string, std::string>>& params) {
    base += base.find('?') == std::string::npos ? '?' : '&';
    for (std::size_t index = 0; index < params.size(); ++index) {
        if (index != 0) base += '&';
        base += url_encode(params[index].first);
        base += '=';
        base += url_encode(params[index].second);
    }
    return base;
}

std::string sha1_base64(std::string_view input) {
    std::vector<std::uint8_t> data(input.begin(), input.end());
    const auto bit_count = static_cast<std::uint64_t>(data.size()) * 8U;
    data.push_back(0x80U);
    while (data.size() % 64U != 56U) data.push_back(0);
    for (int shift = 56; shift >= 0; shift -= 8) {
        data.push_back(static_cast<std::uint8_t>(bit_count >> shift));
    }

    std::uint32_t h0 = 0x67452301U;
    std::uint32_t h1 = 0xEFCDAB89U;
    std::uint32_t h2 = 0x98BADCFEU;
    std::uint32_t h3 = 0x10325476U;
    std::uint32_t h4 = 0xC3D2E1F0U;

    for (std::size_t offset = 0; offset < data.size(); offset += 64) {
        std::array<std::uint32_t, 80> words{};
        for (int index = 0; index < 16; ++index) {
            const auto position = offset + static_cast<std::size_t>(index * 4);
            words[static_cast<std::size_t>(index)] =
                (static_cast<std::uint32_t>(data[position]) << 24) |
                (static_cast<std::uint32_t>(data[position + 1]) << 16) |
                (static_cast<std::uint32_t>(data[position + 2]) << 8) |
                static_cast<std::uint32_t>(data[position + 3]);
        }
        for (int index = 16; index < 80; ++index) {
            const auto value = words[static_cast<std::size_t>(index - 3)] ^
                words[static_cast<std::size_t>(index - 8)] ^
                words[static_cast<std::size_t>(index - 14)] ^
                words[static_cast<std::size_t>(index - 16)];
            words[static_cast<std::size_t>(index)] = (value << 1) | (value >> 31);
        }

        std::uint32_t a = h0;
        std::uint32_t b = h1;
        std::uint32_t c = h2;
        std::uint32_t d = h3;
        std::uint32_t e = h4;
        for (int index = 0; index < 80; ++index) {
            std::uint32_t function{};
            std::uint32_t constant{};
            if (index < 20) {
                function = (b & c) | ((~b) & d);
                constant = 0x5A827999U;
            } else if (index < 40) {
                function = b ^ c ^ d;
                constant = 0x6ED9EBA1U;
            } else if (index < 60) {
                function = (b & c) | (b & d) | (c & d);
                constant = 0x8F1BBCDCU;
            } else {
                function = b ^ c ^ d;
                constant = 0xCA62C1D6U;
            }
            const auto rotated_a = (a << 5) | (a >> 27);
            const auto next = rotated_a + function + e + constant +
                words[static_cast<std::size_t>(index)];
            e = d;
            d = c;
            c = (b << 30) | (b >> 2);
            b = a;
            a = next;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }

    const std::array<std::uint32_t, 5> words{h0, h1, h2, h3, h4};
    std::array<std::uint8_t, 20> digest{};
    for (std::size_t word = 0; word < words.size(); ++word) {
        for (int byte = 0; byte < 4; ++byte) {
            digest[word * 4 + static_cast<std::size_t>(byte)] =
                static_cast<std::uint8_t>(words[word] >> (24 - byte * 8));
        }
    }

    constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(28);
    for (std::size_t index = 0; index < digest.size(); index += 3) {
        const auto value = (static_cast<std::uint32_t>(digest[index]) << 16) |
            (static_cast<std::uint32_t>(index + 1 < digest.size() ? digest[index + 1] : 0) << 8) |
            static_cast<std::uint32_t>(index + 2 < digest.size() ? digest[index + 2] : 0);
        result += alphabet[(value >> 18) & 63U];
        result += alphabet[(value >> 12) & 63U];
        result += index + 1 < digest.size() ? alphabet[(value >> 6) & 63U] : '=';
        result += index + 2 < digest.size() ? alphabet[value & 63U] : '=';
    }
    return result;
}

std::string base64_encode(std::span<const std::uint8_t> input) {
    constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((input.size() + 2) / 3) * 4);
    for (std::size_t index = 0; index < input.size(); index += 3) {
        const auto value = (static_cast<std::uint32_t>(input[index]) << 16) |
            (static_cast<std::uint32_t>(index + 1 < input.size() ? input[index + 1] : 0) << 8) |
            static_cast<std::uint32_t>(index + 2 < input.size() ? input[index + 2] : 0);
        result += alphabet[(value >> 18) & 63U];
        result += alphabet[(value >> 12) & 63U];
        result += index + 1 < input.size() ? alphabet[(value >> 6) & 63U] : '=';
        result += index + 2 < input.size() ? alphabet[value & 63U] : '=';
    }
    return result;
}

std::vector<std::uint8_t> base64_decode(std::string_view input) {
    const auto value_of = [](char value) -> int {
        if (value >= 'A' && value <= 'Z') return value - 'A';
        if (value >= 'a' && value <= 'z') return value - 'a' + 26;
        if (value >= '0' && value <= '9') return value - '0' + 52;
        if (value == '+') return 62;
        if (value == '/') return 63;
        return -1;
    };
    if (input.empty() || input.size() % 4 != 0) {
        throw std::runtime_error("小米 MiOT 授权数据格式无效");
    }
    std::vector<std::uint8_t> result;
    result.reserve(input.size() / 4 * 3);
    for (std::size_t index = 0; index < input.size(); index += 4) {
        const char a = input[index];
        const char b = input[index + 1];
        const char c = input[index + 2];
        const char d = input[index + 3];
        const int va = value_of(a);
        const int vb = value_of(b);
        const int vc = c == '=' ? 0 : value_of(c);
        const int vd = d == '=' ? 0 : value_of(d);
        const bool padded = c == '=' || d == '=';
        if (va < 0 || vb < 0 || vc < 0 || vd < 0 ||
            (c == '=' && d != '=') || (padded && index + 4 != input.size())) {
            throw std::runtime_error("小米 MiOT 授权数据格式无效");
        }
        const auto value = (static_cast<std::uint32_t>(va) << 18) |
            (static_cast<std::uint32_t>(vb) << 12) |
            (static_cast<std::uint32_t>(vc) << 6) |
            static_cast<std::uint32_t>(vd);
        result.push_back(static_cast<std::uint8_t>(value >> 16));
        if (c != '=') result.push_back(static_cast<std::uint8_t>(value >> 8));
        if (d != '=') result.push_back(static_cast<std::uint8_t>(value));
    }
    return result;
}

std::array<std::uint8_t, 32> sha256(std::span<const std::uint8_t> input) {
    std::vector<std::uint8_t> data(input.begin(), input.end());
    const auto bit_count = static_cast<std::uint64_t>(data.size()) * 8U;
    data.push_back(0x80U);
    while (data.size() % 64U != 56U) data.push_back(0);
    for (int shift = 56; shift >= 0; shift -= 8) {
        data.push_back(static_cast<std::uint8_t>(bit_count >> shift));
    }

    std::uint32_t h0 = 0x6A09E667U;
    std::uint32_t h1 = 0xBB67AE85U;
    std::uint32_t h2 = 0x3C6EF372U;
    std::uint32_t h3 = 0xA54FF53AU;
    std::uint32_t h4 = 0x510E527FU;
    std::uint32_t h5 = 0x9B05688CU;
    std::uint32_t h6 = 0x1F83D9ABU;
    std::uint32_t h7 = 0x5BE0CD19U;
    constexpr std::array<std::uint32_t, 64> constants{
        0x428A2F98U, 0x71374491U, 0xB5C0FBCFU, 0xE9B5DBA5U, 0x3956C25BU, 0x59F111F1U,
        0x923F82A4U, 0xAB1C5ED5U, 0xD807AA98U, 0x12835B01U, 0x243185BEU, 0x550C7DC3U,
        0x72BE5D74U, 0x80DEB1FEU, 0x9BDC06A7U, 0xC19BF174U, 0xE49B69C1U, 0xEFBE4786U,
        0x0FC19DC6U, 0x240CA1CCU, 0x2DE92C6FU, 0x4A7484AAU, 0x5CB0A9DCU, 0x76F988DAU,
        0x983E5152U, 0xA831C66DU, 0xB00327C8U, 0xBF597FC7U, 0xC6E00BF3U, 0xD5A79147U,
        0x06CA6351U, 0x14292967U, 0x27B70A85U, 0x2E1B2138U, 0x4D2C6DFCU, 0x53380D13U,
        0x650A7354U, 0x766A0ABBU, 0x81C2C92EU, 0x92722C85U, 0xA2BFE8A1U, 0xA81A664BU,
        0xC24B8B70U, 0xC76C51A3U, 0xD192E819U, 0xD6990624U, 0xF40E3585U, 0x106AA070U,
        0x19A4C116U, 0x1E376C08U, 0x2748774CU, 0x34B0BCB5U, 0x391C0CB3U, 0x4ED8AA4AU,
        0x5B9CCA4FU, 0x682E6FF3U, 0x748F82EEU, 0x78A5636FU, 0x84C87814U, 0x8CC70208U,
        0x90BEFFFAU, 0xA4506CEBU, 0xBEF9A3F7U, 0xC67178F2U};

    const auto rotate_right = [](std::uint32_t value, int bits) {
        return (value >> bits) | (value << (32 - bits));
    };
    for (std::size_t offset = 0; offset < data.size(); offset += 64) {
        std::array<std::uint32_t, 64> words{};
        for (int index = 0; index < 16; ++index) {
            const auto position = offset + static_cast<std::size_t>(index * 4);
            words[static_cast<std::size_t>(index)] =
                (static_cast<std::uint32_t>(data[position]) << 24) |
                (static_cast<std::uint32_t>(data[position + 1]) << 16) |
                (static_cast<std::uint32_t>(data[position + 2]) << 8) |
                static_cast<std::uint32_t>(data[position + 3]);
        }
        for (int index = 16; index < 64; ++index) {
            const auto a = words[static_cast<std::size_t>(index - 15)];
            const auto b = words[static_cast<std::size_t>(index - 2)];
            const auto sigma0 = rotate_right(a, 7) ^ rotate_right(a, 18) ^ (a >> 3);
            const auto sigma1 = rotate_right(b, 17) ^ rotate_right(b, 19) ^ (b >> 10);
            words[static_cast<std::size_t>(index)] = words[static_cast<std::size_t>(index - 16)] +
                sigma0 + words[static_cast<std::size_t>(index - 7)] + sigma1;
        }
        std::uint32_t a = h0; std::uint32_t b = h1; std::uint32_t c = h2; std::uint32_t d = h3;
        std::uint32_t e = h4; std::uint32_t f = h5; std::uint32_t g = h6; std::uint32_t h = h7;
        for (int index = 0; index < 64; ++index) {
            const auto sigma1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
            const auto choose = (e & f) ^ ((~e) & g);
            const auto constant = constants[static_cast<std::size_t>(index)];
            const auto temporary1 = h + sigma1 + choose + constant + words[static_cast<std::size_t>(index)];
            const auto sigma0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto temporary2 = sigma0 + majority;
            h = g; g = f; f = e; e = d + temporary1;
            d = c; c = b; b = a; a = temporary1 + temporary2;
        }
        h0 += a; h1 += b; h2 += c; h3 += d;
        h4 += e; h5 += f; h6 += g; h7 += h;
    }
    const std::array<std::uint32_t, 8> words{h0, h1, h2, h3, h4, h5, h6, h7};
    std::array<std::uint8_t, 32> digest{};
    for (std::size_t word = 0; word < words.size(); ++word) {
        for (int byte = 0; byte < 4; ++byte) {
            digest[word * 4 + static_cast<std::size_t>(byte)] =
                static_cast<std::uint8_t>(words[word] >> (24 - byte * 8));
        }
    }
    return digest;
}

std::array<std::uint8_t, 32> hmac_sha256(std::span<const std::uint8_t> key,
                                         std::span<const std::uint8_t> message) {
    std::array<std::uint8_t, 64> normalized{};
    if (key.size() > normalized.size()) {
        const auto digest = sha256(key);
        std::copy(digest.begin(), digest.end(), normalized.begin());
    } else {
        std::copy(key.begin(), key.end(), normalized.begin());
    }
    std::array<std::uint8_t, 64> outer{};
    std::array<std::uint8_t, 64> inner{};
    for (std::size_t index = 0; index < normalized.size(); ++index) {
        outer[index] = static_cast<std::uint8_t>(normalized[index] ^ 0x5CU);
        inner[index] = static_cast<std::uint8_t>(normalized[index] ^ 0x36U);
    }
    std::vector<std::uint8_t> inner_message(inner.begin(), inner.end());
    inner_message.insert(inner_message.end(), message.begin(), message.end());
    const auto inner_digest = sha256(inner_message);
    std::vector<std::uint8_t> outer_message(outer.begin(), outer.end());
    outer_message.insert(outer_message.end(), inner_digest.begin(), inner_digest.end());
    return sha256(outer_message);
}

std::string miot_signed_nonce(std::string_view ssecurity, std::string_view nonce) {
    auto material = base64_decode(ssecurity);
    const auto nonce_bytes = base64_decode(nonce);
    material.insert(material.end(), nonce_bytes.begin(), nonce_bytes.end());
    const auto digest = sha256(material);
    return base64_encode(digest);
}

std::string miot_nonce() {
    std::array<std::uint8_t, 12> bytes{};
    std::mt19937_64 generator(std::random_device{}());
    for (std::size_t index = 0; index < 8; ++index) {
        bytes[index] = static_cast<std::uint8_t>(generator() & 0xFFU);
    }
    const auto minutes = static_cast<std::uint32_t>(std::time(nullptr) / 60);
    for (int index = 0; index < 4; ++index) {
        bytes[8 + static_cast<std::size_t>(index)] =
            static_cast<std::uint8_t>(minutes >> (24 - index * 8));
    }
    return base64_encode(bytes);
}

struct MiotRequestSignature {
    std::string nonce;
    std::string signature;
};

MiotRequestSignature sign_miot_request(std::string_view uri, std::string_view data,
                                       std::string_view ssecurity) {
    const auto nonce = miot_nonce();
    const auto signed_nonce = miot_signed_nonce(ssecurity, nonce);
    const auto key = base64_decode(signed_nonce);
    const std::string message = std::string(uri) + "&" + signed_nonce + "&" + nonce +
        "&data=" + std::string(data);
    const std::vector<std::uint8_t> message_bytes(message.begin(), message.end());
    const auto digest = hmac_sha256(key, message_bytes);
    return {nonce, base64_encode(digest)};
}

void ensure_service_token(const XiaoAiHttpTransport& transport,
                           std::string& cookies, bool force_refresh = false) {
    if (!force_refresh && has_cookie(cookies, "serviceToken")) return;
    if (!has_cookie(cookies, "userId") && !has_cookie(cookies, "cUserId")) {
        throw std::runtime_error("请先点击“浏览器登录”完成小米授权");
    }
    if (!has_cookie(cookies, "passToken")) {
        throw std::runtime_error("小米授权信息已过期，请重新登录");
    }

    const auto state = request(
        transport, "GET",
        "https://account.xiaomi.com/pass/serviceLogin?sid=micoapi&_json=true&_locale=zh_CN",
        {}, cookies, {}, kPassportUserAgent);
    if (state.status < 200 || state.status >= 300) {
        throw std::runtime_error("小米账号登录状态请求失败（HTTP " +
                                 std::to_string(state.status) + "）");
    }
    capture_cookies(cookies, state, false);

    const auto root = parse_xiaomi_response(state.body);
    std::string api_error;
    if (!api_ok(root, &api_error)) throw std::runtime_error(api_error);
    const auto ssecurity = json_text(root, "ssecurity");
    const auto nonce = json_text(root, "nonce");
    const auto location = json_text(root, "location");
    if (ssecurity.empty() || nonce.empty() || location.empty()) {
        throw std::runtime_error("小米账号登录状态缺少 MiNA 授权信息");
    }

    const auto exchange = request(
        transport, "GET",
        query(location, {{"_userIdNeedEncrypt", "true"},
                         {"clientSign", sha1_base64("nonce=" + nonce + "&" + ssecurity)}}),
        {}, {}, {}, kPassportUserAgent, false);
    if (exchange.status != 200 && exchange.status != 302) {
        throw std::runtime_error("小米 MiNA 授权兑换失败（HTTP " +
                                 std::to_string(exchange.status) + "）");
    }
    capture_cookies(cookies, exchange);
    if (!has_cookie(cookies, "serviceToken")) {
        throw std::runtime_error("小米 MiNA 授权兑换未返回 serviceToken");
    }
}

struct MiotServiceTicket {
    std::string ssecurity;
    std::string service_token;
};

std::optional<MiotServiceTicket> stored_miot_service_ticket(std::string_view cookies) {
    const auto ssecurity = cookie_value(cookies, kStoredMiotSsecurity);
    const auto service_token = cookie_value(cookies, kStoredMiotServiceToken);
    if (ssecurity.empty() || service_token.empty()) return std::nullopt;
    return MiotServiceTicket{ssecurity, service_token};
}

void store_miot_service_ticket(std::string& cookies, const MiotServiceTicket& ticket) {
    merge_cookie(cookies, kStoredMiotSsecurity, ticket.ssecurity);
    merge_cookie(cookies, kStoredMiotServiceToken, ticket.service_token);
}

std::string passport_cookies(std::string_view cookies) {
    std::string result;
    const auto append = [&](std::string_view name) {
        const auto value = cookie_value(cookies, name);
        if (value.empty()) return;
        if (!result.empty()) result += "; ";
        result += std::string(name) + "=" + value;
    };
    append("userId");
    append("cUserId");
    append("passToken");
    append("deviceId");
    return result;
}

MiotServiceTicket issue_miot_service_ticket(const XiaoAiHttpTransport& transport,
                                            std::string& cookies) {
    if (!has_cookie(cookies, "userId") && !has_cookie(cookies, "cUserId")) {
        throw std::runtime_error("请先点击“浏览器登录”完成小米授权");
    }
    if (!has_cookie(cookies, "passToken")) {
        throw std::runtime_error(
            "此音箱需要 MiOT 播报授权。请重新执行“浏览器登录”后再测试播报。");
    }
    const auto state = request(
        transport, "GET",
        "https://account.xiaomi.com/pass/serviceLogin?sid=xiaomiio&_json=true&_locale=zh_CN",
        {}, passport_cookies(cookies), {}, kPassportUserAgent);
    if (state.status < 200 || state.status >= 300) {
        throw std::runtime_error("小米 MiOT 登录状态请求失败（HTTP " +
                                 std::to_string(state.status) + "）");
    }
    capture_cookies(cookies, state, false);

    const auto root = parse_xiaomi_response(state.body);
    std::string api_error;
    if (!api_ok(root, &api_error)) throw std::runtime_error(api_error);
    const auto ssecurity = json_text(root, "ssecurity");
    const auto nonce = json_text(root, "nonce");
    const auto location = json_text(root, "location");
    if (ssecurity.empty() || nonce.empty() || location.empty()) {
        throw std::runtime_error("小米账号登录状态缺少 MiOT 授权信息");
    }

    const auto exchange = request(
        transport, "GET",
        query(location, {{"_userIdNeedEncrypt", "true"},
                         {"clientSign", sha1_base64("nonce=" + nonce + "&" + ssecurity)}}),
        {}, {}, {}, kPassportUserAgent, false);
    if (exchange.status != 200 && exchange.status != 302) {
        throw std::runtime_error("小米 MiOT 授权兑换失败（HTTP " +
                                 std::to_string(exchange.status) + "）");
    }
    std::string exchanged_cookies;
    capture_cookies(exchanged_cookies, exchange);
    const auto service_token = cookie_value(exchanged_cookies, "serviceToken");
    if (service_token.empty()) {
        throw std::runtime_error("小米 MiOT 授权兑换未返回 serviceToken");
    }
    return {ssecurity, service_token};
}

struct Device {
    std::string id;
    std::string miot_did;
    std::string hardware;
    std::string serial;
    std::string mac;
    std::string name;
    std::string alias;
    std::string ip;
    std::string sn_profile;
};

enum class SpeechChannel : unsigned char { MinaUbus, MiotAction };

struct SpeechRoute {
    SpeechChannel channel{SpeechChannel::MinaUbus};
    int service_instance_id{};
    int action_instance_id{};
};

struct MiotHardwareRoute {
    std::string_view hardware;
    int service_instance_id;
    int action_instance_id;
};

// Hardware variants using MiOT actions expose TTS differently from the MiNA UBus
// interface. Keep the compatibility map declarative so adding a device changes data,
// not the notification control flow.
constexpr std::array<MiotHardwareRoute, 16> kMiotSpeechRoutes{{
    {"LX06", 5, 1}, {"L05B", 5, 3}, {"S12", 5, 1}, {"S12A", 5, 1},
    {"LX01", 5, 1}, {"L06A", 5, 1}, {"LX04", 5, 1}, {"L05C", 5, 3},
    {"L17A", 7, 3}, {"X08E", 7, 3}, {"LX05A", 5, 1}, {"LX5A", 5, 1},
    {"L07A", 5, 1}, {"L15A", 7, 3}, {"X6A", 7, 3}, {"X10A", 7, 3},
}};

SpeechRoute speech_route_for(const Device& device) {
    const auto hardware = lower(device.hardware);
    for (const auto& candidate : kMiotSpeechRoutes) {
        if (hardware == lower(std::string(candidate.hardware))) {
            return {SpeechChannel::MiotAction, candidate.service_instance_id,
                    candidate.action_instance_id};
        }
    }
    return {};
}

std::string device_name(const Device& device) {
    return device.alias.empty() ? (device.name.empty() ? device.id : device.name) : device.alias;
}

bool miot_action_ok(const JsonValue& root, std::string* error) {
    if (!api_ok(root, error)) return false;
    const auto* result = json_property(root, "result");
    if (!result || !result->is_object()) {
        if (error) *error = "小米 MiOT 播报响应缺少 result";
        return false;
    }
    const auto* code = json_property(*result, "code");
    if (code && ((code->is_number() && code->int_or(-1) == 0) ||
                 (code->is_string() && code->string() == "0"))) {
        return true;
    }
    if (error) {
        const auto description = json_text(*result, "message");
        *error = description.empty()
            ? (code ? "小米 MiOT 播报动作被设备拒绝" : "小米 MiOT 播报响应缺少动作状态")
            : description;
    }
    return false;
}

std::string send_mina_ubus_tts(const XiaoAiHttpTransport& transport, const Device& device,
                               std::string_view cookies, std::string_view spoken) {
    const std::string message = "{\"text\":\"" + json_escape(spoken) + "\",\"save\":0}";
    const std::string body = "deviceId=" + url_encode(device.id) +
        "&path=mibrain&method=text_to_speech&message=" + url_encode(message);
    std::string device_cookies(cookies);
    // MiNA can return code 0 before it has resolved the speaker context. Supplying
    // the device-bound values matches the Android client's request contract.
    merge_cookie(device_cookies, "sn", device.serial);
    merge_cookie(device_cookies, "hardware", device.hardware);
    merge_cookie(device_cookies, "deviceId", device.id);
    merge_cookie(device_cookies, "deviceSNProfile", device.sn_profile);
    const auto response = request(transport, "POST", std::string(kMinaBase) + "/remote/ubus",
                                  body, std::move(device_cookies),
                                  "application/x-www-form-urlencoded");
    if (response.status < 200 || response.status >= 300) {
        return "HTTP " + std::to_string(response.status);
    }
    std::string api_error;
    const auto root = parse_xiaomi_response(response.body);
    return api_ok(root, &api_error) ? std::string{} : api_error;
}

XiaoAiHttpResponse signed_miot_request(const XiaoAiHttpTransport& transport, std::string_view uri,
                                      std::string_view data, const MiotServiceTicket& ticket,
                                      std::string_view authorization_cookies) {
    const auto signature = sign_miot_request(uri, data, ticket.ssecurity);
    const std::string body = "_nonce=" + url_encode(signature.nonce) +
        "&data=" + url_encode(data) + "&signature=" + url_encode(signature.signature);
    std::string cookies(authorization_cookies);
    merge_cookie(cookies, "serviceToken", ticket.service_token);
    merge_cookie(cookies, "PassportDeviceId", cookie_value(authorization_cookies, "deviceId"));
    return transport({
        "POST", std::string(kMiotBase) + std::string(uri), body,
        {{"User-Agent", kMiotUserAgent}, {"Accept", kAccept}, {"Cookie", std::move(cookies)},
         {kMiotProtocolHeader, kMiotProtocolValue},
         {"Content-Type", "application/x-www-form-urlencoded"}},
    });
}

struct MiotDevice {
    std::string did;
    std::string mac;
    std::string name;
    std::string model;
};

std::string normalized_identifier(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const unsigned char character : value) {
        if (std::isalnum(character)) result += static_cast<char>(std::tolower(character));
    }
    return result;
}

std::vector<MiotDevice> list_miot_devices(const XiaoAiHttpTransport& transport,
                                          const MiotServiceTicket& ticket,
                                          std::string_view authorization_cookies) {
    constexpr char data[] = "{\"getVirtualModel\":false,\"getHuamiDevices\":1}";
    const auto response = signed_miot_request(transport, "/home/device_list", data, ticket,
                                              authorization_cookies);
    if (response.status < 200 || response.status >= 300) {
        throw std::runtime_error("小米 MiOT 设备列表请求失败（HTTP " +
                                 std::to_string(response.status) + "）");
    }
    const auto root = parse_xiaomi_response(response.body);
    std::string api_error;
    if (!api_ok(root, &api_error)) throw std::runtime_error(api_error);
    const auto* result = json_property(root, "result");
    const auto* list = result && result->is_object() ? json_property(*result, "list") : nullptr;
    if (!list || !list->is_array()) return {};
    std::vector<MiotDevice> devices;
    devices.reserve(list->array().size());
    for (const auto& item : list->array()) {
        if (!item.is_object()) continue;
        MiotDevice device;
        device.did = json_text(item, "did");
        device.mac = json_text(item, "mac");
        device.name = json_text(item, "name");
        device.model = json_text(item, "model");
        if (!device.did.empty()) devices.push_back(std::move(device));
    }
    return devices;
}

void resolve_miot_device_ids(std::vector<Device>& mina_devices,
                             const std::vector<MiotDevice>& miot_devices) {
    for (auto& mina_device : mina_devices) {
        if (!mina_device.miot_did.empty() ||
            speech_route_for(mina_device).channel != SpeechChannel::MiotAction) {
            continue;
        }
        const auto mina_mac = normalized_identifier(mina_device.mac);
        const auto hardware = lower(mina_device.hardware);
        const auto mina_name = lower(mina_device.alias.empty() ? mina_device.name : mina_device.alias);
        std::vector<const MiotDevice*> matches;
        const auto collect = [&](const auto& predicate) {
            matches.clear();
            for (const auto& candidate : miot_devices) {
                if (predicate(candidate)) matches.push_back(&candidate);
            }
        };
        if (!mina_mac.empty()) {
            collect([&](const MiotDevice& candidate) {
                return normalized_identifier(candidate.mac) == mina_mac;
            });
        }
        if (matches.empty() && !hardware.empty()) {
            collect([&](const MiotDevice& candidate) {
                return lower(candidate.model).find(hardware) != std::string::npos &&
                    (mina_name.empty() || lower(candidate.name) == mina_name);
            });
        }
        if (matches.size() == 1) mina_device.miot_did = matches.front()->did;
    }
}

std::string send_miot_action_tts(const XiaoAiHttpTransport& transport, const Device& device,
                                 const SpeechRoute& route, const MiotServiceTicket& ticket,
                                 std::string_view authorization_cookies, std::string_view spoken) {
    if (device.miot_did.empty()) {
        return "未能关联此音箱的 MiOT DID；请重新登录并重新扫描音箱";
    }
    const std::string data = "{\"params\":{\"did\":\"" + json_escape(device.miot_did) +
        "\",\"siid\":" + std::to_string(route.service_instance_id) + ",\"aiid\":" +
        std::to_string(route.action_instance_id) + ",\"in\":[\"" + json_escape(spoken) + "\"]}}";
    const auto response = signed_miot_request(transport, "/miotspec/action", data, ticket,
                                              authorization_cookies);
    if (response.status < 200 || response.status >= 300) {
        return "HTTP " + std::to_string(response.status);
    }
    std::string api_error;
    const auto root = parse_xiaomi_response(response.body);
    return miot_action_ok(root, &api_error) ? std::string{} : api_error;
}

std::vector<Device> list_devices(const XiaoAiHttpTransport& transport,
                                 std::string& cookies) {
    const auto response = request(
        transport, "GET",
        query(std::string(kMinaBase) + "/admin/v2/device_list",
              {{"master", "0"},
               {"requestId", random_device_id()},
               {"timestamp", std::to_string(std::time(nullptr))}}),
        {}, cookies, "application/x-www-form-urlencoded");
    capture_cookies(cookies, response);
    if (response.status < 200 || response.status >= 300) {
        throw std::runtime_error("小米设备列表请求失败（HTTP " +
                                 std::to_string(response.status) + "）");
    }

    const auto root = parse_xiaomi_response(response.body);
    std::string api_error;
    if (!api_ok(root, &api_error)) throw std::runtime_error(api_error);

    const JsonValue* data = json_property(root, "data");
    if (data && data->is_object()) data = json_property(*data, "list");
    if (!data || !data->is_array()) data = json_property(root, "list");
    if (!data || !data->is_array()) return {};

    std::vector<Device> devices;
    devices.reserve(data->array().size());
    for (const auto& item : data->array()) {
        if (!item.is_object()) continue;
        Device device;
        device.id = json_text(item, "deviceID");
        if (device.id.empty()) device.id = json_text(item, "deviceId");
        device.miot_did = json_text(item, "miotDID");
        if (device.miot_did.empty()) device.miot_did = json_text(item, "miotDid");
        if (device.miot_did.empty()) device.miot_did = json_text(item, "miot_did");
        device.hardware = json_text(item, "hardware");
        device.serial = json_text(item, "serialNumber");
        if (device.serial.empty()) device.serial = json_text(item, "sn");
        device.mac = json_text(item, "mac");
        device.name = json_text(item, "name");
        device.alias = json_text(item, "alias");
        device.ip = json_text(item, "ip");
        if (device.ip.empty()) device.ip = json_text(item, "localIp");
        device.sn_profile = json_text(item, "deviceSNProfile");
        devices.push_back(std::move(device));
    }
    return devices;
}

std::vector<Device> choose_devices(const std::vector<Device>& devices,
                                   const XiaoAiSettings& settings) {
    std::vector<std::string> targets = settings.device_ids;
    if (targets.empty() && !settings.device_id.empty()) targets.push_back(settings.device_id);
    if (targets.empty()) {
        if (devices.size() == 1) return {devices.front()};
        std::string available;
        for (const auto& device : devices) {
            const auto& name = device.alias.empty() ? device.name : device.alias;
            if (name.empty()) continue;
            if (!available.empty()) available += "、";
            available += name;
        }
        const auto suffix = available.empty() ? std::string{} : "。可选名称：" + available;
        throw std::runtime_error(
            "检测到多个在线小爱音箱，请在设置中选择要播报的音箱" + suffix);
    }

    std::vector<Device> selected;
    selected.reserve(targets.size());
    for (const auto& target_text : targets) {
        const auto target = lower(target_text);
        std::vector<const Device*> matches;
        for (const auto& device : devices) {
            if (lower(device.id) == target || lower(device.name) == target ||
                lower(device.alias) == target) matches.push_back(&device);
        }
        if (matches.empty()) {
            throw std::runtime_error("未找到已选择的小爱音箱：" + target_text);
        }
        if (matches.size() > 1) {
            throw std::runtime_error("选择的小爱音箱名称不唯一，请改用设备ID：" + target_text);
        }
        selected.push_back(*matches.front());
    }
    return selected;
}

struct Session {
    std::string key;
    std::string cookies;
    std::vector<Device> devices;
    std::optional<MiotServiceTicket> miot_ticket;
};

std::string session_key(const XiaoAiSettings& settings) {
    std::string result = settings.auth_cookies;
    result += "\x1f";
    if (settings.device_ids.empty()) {
        result += settings.device_id;
        result.push_back('\x1e');
    } else {
        for (const auto& target : settings.device_ids) {
            result += target;
            result.push_back('\x1e');
        }
    }
    return result;
}

bool validate_authorization(const XiaoAiHttpTransport& transport, XiaoAiSettings& settings,
                            std::string* error) {
    try {
        std::string cookies = settings.auth_cookies;
        ensure_service_token(transport, cookies);
        auto devices = list_devices(transport, cookies);
        if (devices.empty()) {
            if (error) *error = "小米账号下没有发现小爱音箱";
            return false;
        }
        if (std::any_of(devices.begin(), devices.end(), [](const Device& device) {
                return speech_route_for(device).channel == SpeechChannel::MiotAction;
            })) {
            const auto ticket = issue_miot_service_ticket(transport, cookies);
            resolve_miot_device_ids(devices, list_miot_devices(transport, ticket, cookies));
            store_miot_service_ticket(cookies, ticket);
        }
        settings.auth_cookies = std::move(cookies);
        return true;
    } catch (const std::exception& exception) {
        if (error) *error = exception.what();
        return false;
    } catch (...) {
        if (error) *error = "小米账号授权验证失败";
        return false;
    }
}

bool authenticate(const XiaoAiHttpTransport& transport, const XiaoAiSettings& settings,
                  Session& session, std::string* error) {
    try {
        std::string cookies = settings.auth_cookies;
        ensure_service_token(transport, cookies);
        auto devices = list_devices(transport, cookies);
        if (devices.empty()) {
            if (error) *error = "小米账号下没有发现小爱音箱";
            return false;
        }
        if (std::any_of(devices.begin(), devices.end(), [](const Device& device) {
                return speech_route_for(device).channel == SpeechChannel::MiotAction;
            })) {
            auto ticket = stored_miot_service_ticket(cookies);
            if (!ticket && has_cookie(cookies, "passToken")) {
                ticket = issue_miot_service_ticket(transport, cookies);
            }
            if (ticket) {
                resolve_miot_device_ids(devices, list_miot_devices(transport, *ticket, cookies));
                session.miot_ticket = std::move(ticket);
            }
        }
        session.devices = choose_devices(devices, settings);
        session.key = session_key(settings);
        session.cookies = std::move(cookies);
        return true;
    } catch (const std::exception& exception) {
        if (error) *error = exception.what();
        return false;
    } catch (...) {
        if (error) *error = "小米账号授权验证失败";
        return false;
    }
}
bool send_once(const XiaoAiHttpTransport& transport, const XiaoAiSettings& settings,
               XiaoAiEvent event, std::string_view context_label, std::string* error,
               Session* session = nullptr) {
    Session temporary;
    Session& active = session ? *session : temporary;
    if (active.key != session_key(settings) && !authenticate(transport, settings, active, error)) return false;

    const std::string text = event == XiaoAiEvent::Started ? "开始工作" :
        event == XiaoAiEvent::Completed ? "任务完成" :
        event == XiaoAiEvent::Error ? "执行出错" : "任务被中断";
    const auto spoken = context_label.empty() ? text : std::string(context_label) + "，" + text;

    std::string miot_ticket_error;
    const bool needs_miot_ticket = std::any_of(active.devices.begin(), active.devices.end(),
        [](const Device& device) { return speech_route_for(device).channel == SpeechChannel::MiotAction; });
    if (needs_miot_ticket && !active.miot_ticket) {
        active.miot_ticket = stored_miot_service_ticket(active.cookies);
        if (!active.miot_ticket) {
            try {
                active.miot_ticket = issue_miot_service_ticket(transport, active.cookies);
            } catch (const std::exception& exception) {
                miot_ticket_error = exception.what();
            } catch (...) {
                miot_ticket_error = "无法获取小米 MiOT 播报授权";
            }
        }
    }

    const auto send_to_device = [&](Device device) -> std::string {
        try {
            const auto route = speech_route_for(device);
            if (route.channel == SpeechChannel::MinaUbus) {
                return send_mina_ubus_tts(transport, device, active.cookies, spoken);
            }
            if (!miot_ticket_error.empty()) return miot_ticket_error;
            if (!active.miot_ticket) return "无法获取小米 MiOT 播报授权";
            return send_miot_action_tts(transport, device, route, *active.miot_ticket,
                                        active.cookies, spoken);
        } catch (const std::exception& exception) {
            return exception.what();
        } catch (...) {
            return "无法解析小米播报响应";
        }
    };

    const auto parallelism = static_cast<std::size_t>(std::clamp(
        settings.max_parallel_requests, 1, 8));
    std::vector<std::string> failures;
    for (std::size_t start = 0; start < active.devices.size(); start += parallelism) {
        const auto end = std::min(active.devices.size(), start + parallelism);
        std::vector<std::future<std::string>> requests;
        requests.reserve(end - start);
        for (std::size_t index = start; index < end; ++index) {
            requests.push_back(std::async(std::launch::async, send_to_device,
                                          active.devices[index]));
        }
        for (std::size_t index = 0; index < requests.size(); ++index) {
            const auto result = requests[index].get();
            if (result.empty()) continue;
            failures.push_back(device_name(active.devices[start + index]) + "：" + result);
        }
    }
    if (failures.empty()) return true;
    if (error) {
        *error = "小米播报失败（" + std::to_string(failures.size()) + "/" +
            std::to_string(active.devices.size()) + "）：";
        for (std::size_t index = 0; index < failures.size(); ++index) {
            if (index != 0) *error += "；";
            *error += failures[index];
        }
    }
    return false;
}

} // namespace

XiaoAiAuthorizationParts split_xiaoai_authorization(std::string_view cookies) {
    auto user_id = cookie_value(cookies, "userId");
    const auto c_user_id = cookie_value(cookies, "cUserId");
    const auto service_token = cookie_value(cookies, "serviceToken");
    const auto device_id = cookie_value(cookies, "deviceId");
    const auto miot_ticket = stored_miot_service_ticket(cookies);
    const bool uses_c_user_id = user_id.empty();
    if (uses_c_user_id) user_id = c_user_id;
    if (user_id.empty() || service_token.empty()) return {};

    XiaoAiAuthorizationParts result;
    result.mina = std::string(uses_c_user_id ? "cUserId=" : "userId=") + user_id +
        "; serviceToken=" + service_token;
    if (!device_id.empty()) result.mina += "; deviceId=" + device_id;
    if (miot_ticket) {
        result.miot_ssecurity = miot_ticket->ssecurity;
        result.miot_service_token = miot_ticket->service_token;
    }
    return result;
}

std::string combine_xiaoai_authorization(const XiaoAiAuthorizationParts& parts) {
    if (parts.mina.empty()) return {};
    std::string result = parts.mina;
    if (!parts.miot_ssecurity.empty()) {
        result += "; " + std::string(kStoredMiotSsecurity) + "=" + parts.miot_ssecurity;
    }
    if (!parts.miot_service_token.empty()) {
        result += "; " + std::string(kStoredMiotServiceToken) + "=" + parts.miot_service_token;
    }
    return result;
}

std::string compact_xiaoai_authorization(std::string_view cookies) {
    return combine_xiaoai_authorization(split_xiaoai_authorization(cookies));
}

XiaoAiNotifier::XiaoAiNotifier(XiaoAiHttpTransport transport)
    : transport_(std::move(transport)), worker_([this] { worker_loop(); }) {}

XiaoAiNotifier::~XiaoAiNotifier() {
    stop();
}

void XiaoAiNotifier::configure(const XiaoAiSettings& settings) {
    std::lock_guard lock(mutex_);
    settings_ = settings;
    std::queue<Job> empty;
    jobs_.swap(empty);
    std::queue<std::function<void()>> empty_controls;
    control_tasks_.swap(empty_controls);
}

void XiaoAiNotifier::notify(XiaoAiEvent event, std::string_view context_label) {
    std::lock_guard lock(mutex_);
    if (stopping_ || !settings_.enabled) return;

    const bool selected = [&] {
        switch (event) {
            case XiaoAiEvent::Started: return settings_.notify_started;
            case XiaoAiEvent::Completed: return settings_.notify_completed;
            case XiaoAiEvent::Error: return settings_.notify_error;
            case XiaoAiEvent::Interrupted: return settings_.notify_interrupted;
        }
        return false;
    }();
    if (!selected) return;

    constexpr std::size_t maximum_pending_jobs = 16;
    while (jobs_.size() >= maximum_pending_jobs) jobs_.pop();
    jobs_.push(Job{settings_, event, std::string(context_label)});
    condition_.notify_one();
}

bool XiaoAiNotifier::validate(XiaoAiSettings& settings, std::string* error) {
    return validate_authorization(transport_, settings, error);
}

bool XiaoAiNotifier::discover_devices(const XiaoAiSettings& settings,
                                      std::vector<XiaoAiDeviceInfo>* devices,
                                      std::string* error) {
    if (!devices) {
        if (error) *error = "无法接收扫描到的小爱音箱";
        return false;
    }
    try {
        std::string cookies = settings.auth_cookies;
        ensure_service_token(transport_, cookies);
        auto found = list_devices(transport_, cookies);
        if (std::any_of(found.begin(), found.end(), [](const Device& device) {
                return speech_route_for(device).channel == SpeechChannel::MiotAction;
            })) {
            try {
                auto ticket = stored_miot_service_ticket(cookies);
                if (!ticket && has_cookie(cookies, "passToken")) {
                    ticket = issue_miot_service_ticket(transport_, cookies);
                }
                if (ticket) {
                    resolve_miot_device_ids(found, list_miot_devices(transport_, *ticket, cookies));
                }
            } catch (...) {
                // The selector still remains usable for MiNA devices; the selected MiOT
                // device will surface a specific authorization/DID error during testing.
            }
        }
        devices->clear();
        devices->reserve(found.size());
        for (const auto& device : found) {
            devices->push_back({device.id, device.name, device.alias, device.hardware, device.miot_did});
        }
        return true;
    } catch (const std::exception& exception) {
        if (error) *error = exception.what();
        return false;
    } catch (...) {
        if (error) *error = "扫描小爱音箱失败";
        return false;
    }
}

bool XiaoAiNotifier::test(const XiaoAiSettings& settings, std::string* error) {
    try {
        return send_once(transport_, settings, XiaoAiEvent::Completed, {}, error);
    } catch (const std::exception& exception) {
        if (error) *error = exception.what();
        return false;
    } catch (...) {
        if (error) *error = "小米播报请求失败";
        return false;
    }
}

void XiaoAiNotifier::validate_async(XiaoAiSettings settings, ValidateCallback callback) {
    std::lock_guard lock(mutex_);
    if (stopping_) return;
    control_tasks_.push([this, settings = std::move(settings), callback = std::move(callback)]() mutable {
        std::string error;
        (void)validate(settings, &error);
        if (callback) callback(std::move(settings), std::move(error));
    });
    condition_.notify_one();
}

void XiaoAiNotifier::discover_devices_async(XiaoAiSettings settings, DiscoverCallback callback) {
    std::lock_guard lock(mutex_);
    if (stopping_) return;
    control_tasks_.push([this, settings = std::move(settings), callback = std::move(callback)]() mutable {
        std::vector<XiaoAiDeviceInfo> devices;
        std::string error;
        (void)discover_devices(settings, &devices, &error);
        if (callback) callback(std::move(devices), std::move(error));
    });
    condition_.notify_one();
}

void XiaoAiNotifier::test_async(XiaoAiSettings settings, TestCallback callback) {
    std::lock_guard lock(mutex_);
    if (stopping_) return;
    control_tasks_.push([this, settings = std::move(settings), callback = std::move(callback)]() mutable {
        std::string error;
        (void)test(settings, &error);
        if (callback) callback(std::move(error));
    });
    condition_.notify_one();
}

void XiaoAiNotifier::stop() noexcept {
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        stopping_ = true;
        std::queue<Job> empty;
        jobs_.swap(empty);
        std::queue<std::function<void()>> empty_controls;
        control_tasks_.swap(empty_controls);
    }
    condition_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void XiaoAiNotifier::worker_loop() {
    Session session;
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [&] { return stopping_ || !control_tasks_.empty() || !jobs_.empty(); });
            if (stopping_) return;
            if (!control_tasks_.empty()) {
                auto task = std::move(control_tasks_.front());
                control_tasks_.pop();
                lock.unlock();
                try { task(); } catch (...) {}
                continue;
            }
            job = std::move(jobs_.front());
            jobs_.pop();
        }

        std::string ignored;
        try {
            send_once(transport_, job.settings, job.event, job.context_label, &ignored, &session);
        } catch (...) {
            // Notification failures must not terminate the desktop application worker thread.
        }
    }
}

} // namespace codexpets
