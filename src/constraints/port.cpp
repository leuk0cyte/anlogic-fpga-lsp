// =============================================================================
//  src/constraints/port.cpp
// =============================================================================

#include "constraints/port.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <utility>

namespace fpgalsp::constraints {

Port Port::Scalar(std::string name, int line, int character) {
    Port port;
    port.name = std::move(name);
    port.line = line;
    port.character = character;
    return port;
}

Port Port::Bus(std::string name, int msb, int lsb, int line, int character) {
    Port port;
    port.name = std::move(name);
    port.msb = msb;
    port.lsb = lsb;
    port.width = std::abs(msb - lsb) + 1;
    port.line = line;
    port.character = character;
    return port;
}

std::vector<int> PortBits(const Port& port) {
    const auto make_range = [](int low, int width) {
        std::vector<int> bits;
        if (width <= 0) return bits;
        const int clamped = std::min(width, kMaxPortBits);
        bits.reserve(static_cast<size_t>(clamped));
        for (int i = 0; i < clamped; ++i) bits.push_back(low + i);
        return bits;
    };

    if (port.width.has_value() && *port.width > 1) {
        const int low = (port.msb.has_value() && port.lsb.has_value())
                            ? std::min(*port.msb, *port.lsb)
                            : 0;
        return make_range(low, *port.width);
    }
    if (port.width.has_value() && *port.width == 1) return {};
    if (port.msb.has_value() && port.lsb.has_value() && *port.msb != *port.lsb) {
        const int low = std::min(*port.msb, *port.lsb);
        const int high = std::max(*port.msb, *port.lsb);
        return make_range(low, high - low + 1);
    }
    return {};
}

PortIndex BuildPortIndex(const std::vector<Port>& ports) {
    PortIndex index;
    index.reserve(ports.size());
    for (const Port& port : ports) {
        if (port.name.empty()) continue;
        index.emplace(port.name, &port);  // emplace 不覆盖 -> 第一个胜出
    }
    return index;
}

namespace {

std::optional<int> ReadOptionalInt(const Json& json, const char* key) {
    if (!json.is_object()) return std::nullopt;
    const auto it = json.find(key);
    if (it == json.end()) return std::nullopt;
    if (it->is_number_integer()) {
        // 夹到 int 范围，避免 UB 转换
        const long long value = it->get<long long>();
        if (value > std::numeric_limits<int>::max()) return std::numeric_limits<int>::max();
        if (value < std::numeric_limits<int>::min()) return std::numeric_limits<int>::min();
        return static_cast<int>(value);
    }
    if (it->is_number_unsigned()) {
        const unsigned long long value = it->get<unsigned long long>();
        if (value > static_cast<unsigned long long>(std::numeric_limits<int>::max())) {
            return std::numeric_limits<int>::max();
        }
        return static_cast<int>(value);
    }
    return std::nullopt;
}

int ReadInt(const Json& json, const char* key, int fallback) {
    const auto value = ReadOptionalInt(json, key);
    return value.has_value() ? *value : fallback;
}

std::string ReadString(const Json& json, const char* key) {
    if (!json.is_object()) return {};
    const auto it = json.find(key);
    if (it == json.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

}  // namespace

Port PortFromJson(const Json& json) {
    Port port;
    if (!json.is_object()) return port;
    port.name = ReadString(json, "name");
    port.direction = ReadString(json, "direction");
    port.msb = ReadOptionalInt(json, "msb");
    port.lsb = ReadOptionalInt(json, "lsb");
    port.width = ReadOptionalInt(json, "width");
    port.line = ReadInt(json, "line", 0);
    port.character = ReadInt(json, "character", 0);
    port.file = ReadString(json, "file");
    return port;
}

Json PortToJson(const Port& port) {
    Json json = Json::object();
    json["name"] = port.name;
    json["direction"] = port.direction;
    json["msb"] = port.msb.has_value() ? Json(*port.msb) : Json(nullptr);
    json["lsb"] = port.lsb.has_value() ? Json(*port.lsb) : Json(nullptr);
    json["width"] = port.width.has_value() ? Json(*port.width) : Json(nullptr);
    json["line"] = port.line;
    json["character"] = port.character;
    json["file"] = port.file;
    return json;
}

}  // namespace fpgalsp::constraints