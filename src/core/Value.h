#pragma once

#include "core/Bytes.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace cardbridge {

// A dynamically-typed tree: the common shape of a MessagePack document (the
// card's sections, the list tables) and of a Unity object read through its
// type tree. Maps keep file order and allow any key type; struct-like lookups
// go through at("name").
class Value {
public:
    struct Nil {};
    using Array = std::vector<Value>;
    using Map = std::vector<std::pair<Value, Value>>;

    Value() : v_(Nil{}) {}
    explicit Value(bool b) : v_(b) {}
    explicit Value(int64_t i) : v_(i) {}
    explicit Value(double d) : v_(d) {}
    explicit Value(std::string s) : v_(std::move(s)) {}
    explicit Value(Bytes b) : v_(std::move(b)) {}
    explicit Value(Array a) : v_(std::move(a)) {}
    explicit Value(Map m) : v_(std::move(m)) {}

    bool isNil() const { return std::holds_alternative<Nil>(v_); }
    bool isBool() const { return std::holds_alternative<bool>(v_); }
    bool isInt() const { return std::holds_alternative<int64_t>(v_); }
    bool isFloat() const { return std::holds_alternative<double>(v_); }
    bool isNumber() const { return isInt() || isFloat(); }
    bool isStr() const { return std::holds_alternative<std::string>(v_); }
    bool isBin() const { return std::holds_alternative<Bytes>(v_); }
    bool isArray() const { return std::holds_alternative<Array>(v_); }
    bool isMap() const { return std::holds_alternative<Map>(v_); }

    bool asBool(bool def = false) const { return isBool() ? std::get<bool>(v_) : (isInt() ? asInt() != 0 : def); }
    int64_t asInt(int64_t def = 0) const {
        if (isInt()) return std::get<int64_t>(v_);
        if (isFloat()) return static_cast<int64_t>(std::get<double>(v_));
        if (isBool()) return std::get<bool>(v_) ? 1 : 0;
        return def;
    }
    double asFloat(double def = 0) const {
        if (isFloat()) return std::get<double>(v_);
        if (isInt()) return static_cast<double>(std::get<int64_t>(v_));
        return def;
    }
    const std::string& asStr() const { return isStr() ? std::get<std::string>(v_) : empty_str(); }
    const Bytes& asBin() const { return isBin() ? std::get<Bytes>(v_) : empty_bin(); }
    const Array& asArray() const { return isArray() ? std::get<Array>(v_) : empty_array(); }
    const Map& asMap() const { return isMap() ? std::get<Map>(v_) : empty_map(); }
    Array& array() { return std::get<Array>(v_); }
    Map& map() { return std::get<Map>(v_); }

    // Map lookup by string key; a shared Nil value if there is no such key or
    // this is not a map - callers read fields without a chain of checks.
    const Value& at(const std::string& key) const {
        if (isMap())
            for (const auto& kv : std::get<Map>(v_))
                if (kv.first.isStr() && kv.first.asStr() == key) return kv.second;
        return nil();
    }
    bool has(const std::string& key) const {
        if (isMap())
            for (const auto& kv : std::get<Map>(v_))
                if (kv.first.isStr() && kv.first.asStr() == key) return true;
        return false;
    }
    // Map lookup by integer key (list tables).
    const Value& atInt(int64_t key) const {
        if (isMap())
            for (const auto& kv : std::get<Map>(v_))
                if (kv.first.isInt() && kv.first.asInt() == key) return kv.second;
        return nil();
    }
    const Value& at(size_t i) const {
        if (isArray() && i < std::get<Array>(v_).size()) return std::get<Array>(v_)[i];
        return nil();
    }
    size_t size() const {
        if (isArray()) return std::get<Array>(v_).size();
        if (isMap()) return std::get<Map>(v_).size();
        if (isBin()) return std::get<Bytes>(v_).size();
        return 0;
    }

    static const Value& nil() { static const Value n; return n; }

private:
    static const std::string& empty_str() { static const std::string s; return s; }
    static const Bytes& empty_bin() { static const Bytes b; return b; }
    static const Array& empty_array() { static const Array a; return a; }
    static const Map& empty_map() { static const Map m; return m; }

    std::variant<Nil, bool, int64_t, double, std::string, Bytes, Array, Map> v_;
};

} // namespace cardbridge
