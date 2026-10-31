// Minimal JSON reader for the server's API responses (DOM, recursion-limited).
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lm::net::json {

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool b = false;
    double num = 0;
    std::int64_t i = 0; // exact when the number is an integer
    std::string str;
    std::vector<Value> arr;
    std::map<std::string, Value, std::less<>> obj;

    const Value *get(std::string_view k) const {
        auto it = obj.find(k);
        return it == obj.end() ? nullptr : &it->second;
    }
    std::int64_t i64(std::string_view k, std::int64_t def = 0) const {
        auto *v = get(k);
        return v && v->type == Type::Number ? v->i : def;
    }
    std::string s(std::string_view k) const {
        auto *v = get(k);
        return v && v->type == Type::String ? v->str : std::string();
    }
    bool boolean(std::string_view k) const {
        auto *v = get(k);
        return v && v->type == Type::Bool && v->b;
    }
    const std::vector<Value> &array(std::string_view k) const {
        static const std::vector<Value> none;
        auto *v = get(k);
        return v && v->type == Type::Array ? v->arr : none;
    }
};

// nullopt on malformed input (error text in *err).
std::optional<Value> parse(std::string_view text, std::string *err = nullptr);

} // namespace lm::net::json
