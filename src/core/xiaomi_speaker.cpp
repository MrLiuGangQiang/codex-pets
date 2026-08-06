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
#include <random>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace codexpets {
namespace {

constexpr char kMinaBase[] = "https://api2.mina.mi.com";
constexpr char kUserAgent[] = "MICO/AndroidApp/@SHIP.TO.2A2FE0D7@/2.4.40";
constexpr char kPassportUserAgent[] = "Dalvik/2.1.0 (Linux; U; Android 10; RMX2111 Build/QP1A.190711.020) APP/xiaomi.mico APPV/2004040 MK/Uk1YMjExMQ== PassportSDK/3.8.3 passport-ui/3.8.3";
constexpr char kAccept[] = "application/json, text/plain, */*";

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

struct Device {
    std::string id;
    std::string hardware;
    std::string serial;
    std::string mac;
    std::string name;
    std::string alias;
    std::string ip;
    std::string sn_profile;
};

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
        if (list_devices(transport, cookies).empty()) {
            if (error) *error = "小米账号下没有发现小爱音箱";
            return false;
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
        const auto devices = list_devices(transport, cookies);
        if (devices.empty()) {
            if (error) *error = "小米账号下没有发现小爱音箱";
            return false;
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
    const std::string message = "{\"text\":\"" + json_escape(spoken) + "\",\"save\":0}";
    const auto send_to_device = [&](Device device) -> std::string {
        const std::string body = "deviceId=" + url_encode(device.id) +
            "&path=mibrain&method=text_to_speech&message=" + url_encode(message);
        // MiNA returns code 0 even when an UBus command lacks the selected speaker's
        // context. Supply the same device-bound cookies used by the Android client.
        std::string device_cookies = active.cookies;
        merge_cookie(device_cookies, "sn", device.serial);
        merge_cookie(device_cookies, "hardware", device.hardware);
        merge_cookie(device_cookies, "deviceId", device.id);
        merge_cookie(device_cookies, "deviceSNProfile", device.sn_profile);
        try {
            auto response = request(transport, "POST", std::string(kMinaBase) + "/remote/ubus",
                                    body, std::move(device_cookies), "application/x-www-form-urlencoded");
            if (response.status < 200 || response.status >= 300) {
                return "HTTP " + std::to_string(response.status);
            }
            std::string api_error;
            const auto root = parse_xiaomi_response(response.body);
            return api_ok(root, &api_error) ? std::string{} : api_error;
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
            const auto& device = active.devices[start + index];
            const auto& name = device.alias.empty()
                ? (device.name.empty() ? device.id : device.name) : device.alias;
            failures.push_back(name + "：" + result);
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

std::string compact_xiaoai_authorization(std::string_view cookies) {
    const auto user_id = cookie_value(cookies, "userId");
    const auto service_token = cookie_value(cookies, "serviceToken");
    const auto device_id = cookie_value(cookies, "deviceId");
    if (user_id.empty() || service_token.empty()) return {};
    std::string result = "userId=" + user_id + "; serviceToken=" + service_token;
    if (!device_id.empty()) result += "; deviceId=" + device_id;
    return result;
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
        const auto found = list_devices(transport_, cookies);
        devices->clear();
        devices->reserve(found.size());
        for (const auto& device : found) {
            devices->push_back({device.id, device.name, device.alias, device.hardware});
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
