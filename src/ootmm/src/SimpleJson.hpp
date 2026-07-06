#pragma once

#include <map>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace ootmm::json {

class Value {
  public:
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value>;

    Value();
    explicit Value(std::nullptr_t);
    explicit Value(bool value);
    explicit Value(double value);
    explicit Value(std::string value);
    explicit Value(Array value);
    explicit Value(Object value);

    [[nodiscard]] bool IsNull() const;
    [[nodiscard]] bool IsBool() const;
    [[nodiscard]] bool IsNumber() const;
    [[nodiscard]] bool IsString() const;
    [[nodiscard]] bool IsArray() const;
    [[nodiscard]] bool IsObject() const;

    [[nodiscard]] bool AsBool() const;
    [[nodiscard]] double AsNumber() const;
    [[nodiscard]] const std::string& AsString() const;
    [[nodiscard]] const Array& AsArray() const;
    [[nodiscard]] const Object& AsObject() const;

    [[nodiscard]] const Value* Find(const std::string& key) const;
    [[nodiscard]] const Value& At(const std::string& key) const;

  private:
    std::variant<std::nullptr_t, bool, double, std::string, Array, Object> data_;
};

class ParseError final : public std::runtime_error {
  public:
    explicit ParseError(const std::string& message);
};

[[nodiscard]] Value Parse(const std::string& text);
[[nodiscard]] std::string EscapeString(const std::string& value);

} // namespace ootmm::json
