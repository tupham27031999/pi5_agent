#pragma once

#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <iomanip>

namespace pi5_agent {

/**
 * @brief Bộ sinh và phân tích JSON C++17 thuần (Zero-Dependency)
 */
class JsonValue {
public:
    enum class Type { Null, Boolean, Number, String, Array, Object };

    Type type = Type::Null;
    bool bool_val = false;
    double num_val = 0.0;
    std::string str_val;
    std::vector<JsonValue> array_val;
    std::map<std::string, JsonValue> obj_val;

    JsonValue() : type(Type::Null) {}
    JsonValue(bool b) : type(Type::Boolean), bool_val(b) {}
    JsonValue(int n) : type(Type::Number), num_val(static_cast<double>(n)) {}
    JsonValue(int64_t n) : type(Type::Number), num_val(static_cast<double>(n)) {}
    JsonValue(double n) : type(Type::Number), num_val(n) {}
    JsonValue(const char* s) : type(Type::String), str_val(s ? s : "") {}
    JsonValue(const std::string& s) : type(Type::String), str_val(s) {}

    static JsonValue array() {
        JsonValue v;
        v.type = Type::Array;
        return v;
    }

    static JsonValue object() {
        JsonValue v;
        v.type = Type::Object;
        return v;
    }

    void set(const std::string& key, const JsonValue& val) {
        if (type != Type::Object) {
            type = Type::Object;
            obj_val.clear();
        }
        obj_val[key] = val;
    }

    void push_back(const JsonValue& val) {
        if (type != Type::Array) {
            type = Type::Array;
            array_val.clear();
        }
        array_val.push_back(val);
    }

    bool has(const std::string& key) const {
        if (type != Type::Object) return false;
        return obj_val.find(key) != obj_val.end();
    }

    JsonValue get(const std::string& key, const JsonValue& default_val = JsonValue()) const {
        if (type != Type::Object) return default_val;
        auto it = obj_val.find(key);
        if (it != obj_val.end()) return it->second;
        return default_val;
    }

    std::string as_string(const std::string& default_val = "") const {
        if (type == Type::String) return str_val;
        if (type == Type::Number) {
            std::ostringstream ss;
            ss << num_val;
            return ss.str();
        }
        if (type == Type::Boolean) return bool_val ? "true" : "false";
        return default_val;
    }

    double as_double(double default_val = 0.0) const {
        if (type == Type::Number) return num_val;
        if (type == Type::String) {
            try { return std::stod(str_val); } catch (...) {}
        }
        return default_val;
    }

    int as_int(int default_val = 0) const {
        return static_cast<int>(as_double(default_val));
    }

    bool as_bool(bool default_val = false) const {
        if (type == Type::Boolean) return bool_val;
        if (type == Type::String) return str_val == "true" || str_val == "1";
        if (type == Type::Number) return num_val != 0.0;
        return default_val;
    }

    std::string dump(int indent = 0) const {
        std::ostringstream ss;
        serialize_internal(ss, indent, 0);
        return ss.str();
    }

    // Bộ Parser đơn giản
    static JsonValue parse(const std::string& str) {
        size_t index = 0;
        skip_whitespace(str, index);
        return parse_value(str, index);
    }

private:
    static std::string escape_string(const std::string& s) {
        std::ostringstream ss;
        for (char c : s) {
            switch (c) {
                case '"':  ss << "\\\""; break;
                case '\\': ss << "\\\\"; break;
                case '\b': ss << "\\b"; break;
                case '\f': ss << "\\f"; break;
                case '\n': ss << "\\n"; break;
                case '\r': ss << "\\r"; break;
                case '\t': ss << "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        ss << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c);
                    } else {
                        ss << c;
                    }
            }
        }
        return ss.str();
    }

    void serialize_internal(std::ostringstream& ss, int indent, int current_indent) const {
        std::string ind_str(current_indent, ' ');
        std::string next_ind_str(current_indent + indent, ' ');

        switch (type) {
            case Type::Null:
                ss << "null";
                break;
            case Type::Boolean:
                ss << (bool_val ? "true" : "false");
                break;
            case Type::Number:
                if (num_val == static_cast<int64_t>(num_val)) {
                    ss << static_cast<int64_t>(num_val);
                } else {
                    ss << num_val;
                }
                break;
            case Type::String:
                ss << "\"" << escape_string(str_val) << "\"";
                break;
            case Type::Array:
                if (array_val.empty()) {
                    ss << "[]";
                } else {
                    ss << "[";
                    if (indent > 0) ss << "\n";
                    for (size_t i = 0; i < array_val.size(); ++i) {
                        if (indent > 0) ss << next_ind_str;
                        array_val[i].serialize_internal(ss, indent, current_indent + indent);
                        if (i + 1 < array_val.size()) ss << ",";
                        if (indent > 0) ss << "\n";
                    }
                    if (indent > 0) ss << ind_str;
                    ss << "]";
                }
                break;
            case Type::Object:
                if (obj_val.empty()) {
                    ss << "{}";
                } else {
                    ss << "{";
                    if (indent > 0) ss << "\n";
                    size_t count = 0;
                    for (auto it = obj_val.begin(); it != obj_val.end(); ++it, ++count) {
                        if (indent > 0) ss << next_ind_str;
                        ss << "\"" << escape_string(it->first) << "\": ";
                        it->second.serialize_internal(ss, indent, current_indent + indent);
                        if (count + 1 < obj_val.size()) ss << ",";
                        if (indent > 0) ss << "\n";
                    }
                    if (indent > 0) ss << ind_str;
                    ss << "}";
                }
                break;
        }
    }

    static void skip_whitespace(const std::string& s, size_t& i) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) {
            i++;
        }
    }

    static JsonValue parse_value(const std::string& s, size_t& i) {
        skip_whitespace(s, i);
        if (i >= s.size()) return JsonValue();

        char c = s[i];
        if (c == '{') return parse_object(s, i);
        if (c == '[') return parse_array(s, i);
        if (c == '"') return parse_string(s, i);
        if (c == 't' || c == 'f') return parse_bool(s, i);
        if (c == 'n') return parse_null(s, i);
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number(s, i);

        return JsonValue();
    }

    static JsonValue parse_object(const std::string& s, size_t& i) {
        JsonValue val = JsonValue::object();
        i++; // skip '{'
        skip_whitespace(s, i);

        if (i < s.size() && s[i] == '}') {
            i++;
            return val;
        }

        while (i < s.size()) {
            skip_whitespace(s, i);
            if (i >= s.size() || s[i] != '"') break;

            JsonValue key_val = parse_string(s, i);
            std::string key = key_val.str_val;

            skip_whitespace(s, i);
            if (i >= s.size() || s[i] != ':') break;
            i++; // skip ':'

            JsonValue member_val = parse_value(s, i);
            val.set(key, member_val);

            skip_whitespace(s, i);
            if (i < s.size() && s[i] == ',') {
                i++;
            } else if (i < s.size() && s[i] == '}') {
                i++;
                return val;
            } else {
                break;
            }
        }
        return val;
    }

    static JsonValue parse_array(const std::string& s, size_t& i) {
        JsonValue val = JsonValue::array();
        i++; // skip '['
        skip_whitespace(s, i);

        if (i < s.size() && s[i] == ']') {
            i++;
            return val;
        }

        while (i < s.size()) {
            JsonValue elem = parse_value(s, i);
            val.push_back(elem);

            skip_whitespace(s, i);
            if (i < s.size() && s[i] == ',') {
                i++;
            } else if (i < s.size() && s[i] == ']') {
                i++;
                return val;
            } else {
                break;
            }
        }
        return val;
    }

    static JsonValue parse_string(const std::string& s, size_t& i) {
        i++; // skip opening '"'
        std::string res;
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') {
                return JsonValue(res);
            }
            if (c == '\\' && i < s.size()) {
                char esc = s[i++];
                switch (esc) {
                    case '"': res += '"'; break;
                    case '\\': res += '\\'; break;
                    case '/': res += '/'; break;
                    case 'b': res += '\b'; break;
                    case 'f': res += '\f'; break;
                    case 'n': res += '\n'; break;
                    case 'r': res += '\r'; break;
                    case 't': res += '\t'; break;
                    default: res += esc; break;
                }
            } else {
                res += c;
            }
        }
        return JsonValue(res);
    }

    static JsonValue parse_number(const std::string& s, size_t& i) {
        size_t start = i;
        if (s[i] == '-') i++;
        while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == 'e' || s[i] == 'E' || s[i] == '+' || s[i] == '-')) {
            i++;
        }
        std::string num_str = s.substr(start, i - start);
        try {
            double d = std::stod(num_str);
            return JsonValue(d);
        } catch (...) {
            return JsonValue(0.0);
        }
    }

    static JsonValue parse_bool(const std::string& s, size_t& i) {
        if (s.substr(i, 4) == "true") {
            i += 4;
            return JsonValue(true);
        }
        if (s.substr(i, 5) == "false") {
            i += 5;
            return JsonValue(false);
        }
        return JsonValue(false);
    }

    static JsonValue parse_null(const std::string& s, size_t& i) {
        if (s.substr(i, 4) == "null") {
            i += 4;
        }
        return JsonValue();
    }
};

} // namespace pi5_agent
