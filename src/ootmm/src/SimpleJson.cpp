#include "SimpleJson.hpp"

#include <charconv>
#include <cctype>
#include <sstream>

namespace ootmm::json {

Value::Value() : data_(nullptr) {}
Value::Value(std::nullptr_t) : data_(nullptr) {}
Value::Value(bool value) : data_(value) {}
Value::Value(double value) : data_(value) {}
Value::Value(std::string value) : data_(std::move(value)) {}
Value::Value(Array value) : data_(std::move(value)) {}
Value::Value(Object value) : data_(std::move(value)) {}

bool Value::IsNull() const { return std::holds_alternative<std::nullptr_t>(data_); }
bool Value::IsBool() const { return std::holds_alternative<bool>(data_); }
bool Value::IsNumber() const { return std::holds_alternative<double>(data_); }
bool Value::IsString() const { return std::holds_alternative<std::string>(data_); }
bool Value::IsArray() const { return std::holds_alternative<Array>(data_); }
bool Value::IsObject() const { return std::holds_alternative<Object>(data_); }

bool Value::AsBool() const { return std::get<bool>(data_); }
double Value::AsNumber() const { return std::get<double>(data_); }
const std::string& Value::AsString() const { return std::get<std::string>(data_); }
const Value::Array& Value::AsArray() const { return std::get<Array>(data_); }
const Value::Object& Value::AsObject() const { return std::get<Object>(data_); }

const Value* Value::Find(const std::string& key) const {
    if (!IsObject()) {
        return nullptr;
    }
    const auto& object = AsObject();
    const auto it = object.find(key);
    return it == object.end() ? nullptr : &it->second;
}

const Value& Value::At(const std::string& key) const {
    const Value* value = Find(key);
    if (value == nullptr) {
        throw std::out_of_range("Missing JSON key: " + key);
    }
    return *value;
}

ParseError::ParseError(const std::string& message) : std::runtime_error(message) {}

class Parser {
  public:
    explicit Parser(const std::string& text) : text_(text) {}

    Value ParseValue() {
        SkipWhitespace();
        if (Done()) {
            throw ParseError("Unexpected end of JSON");
        }
        switch (Peek()) {
            case 'n':
                ConsumeLiteral("null");
                return Value(nullptr);
            case 't':
                ConsumeLiteral("true");
                return Value(true);
            case 'f':
                ConsumeLiteral("false");
                return Value(false);
            case '"':
                return Value(ParseString());
            case '[':
                return Value(ParseArray());
            case '{':
                return Value(ParseObject());
            default:
                if (Peek() == '-' || std::isdigit(static_cast<unsigned char>(Peek()))) {
                    return Value(ParseNumber());
                }
                throw ParseError("Unexpected JSON token at offset " + std::to_string(pos_));
        }
    }

    void ExpectEnd() {
        SkipWhitespace();
        if (!Done()) {
            throw ParseError("Unexpected trailing JSON at offset " + std::to_string(pos_));
        }
    }

  private:
    const std::string& text_;
    std::size_t pos_ = 0;

    [[nodiscard]] bool Done() const { return pos_ >= text_.size(); }
    [[nodiscard]] char Peek() const { return text_[pos_]; }

    char Take() {
        if (Done()) {
            throw ParseError("Unexpected end of JSON");
        }
        return text_[pos_++];
    }

    void SkipWhitespace() {
        while (!Done() && std::isspace(static_cast<unsigned char>(Peek()))) {
            ++pos_;
        }
    }

    void ConsumeLiteral(const char* literal) {
        while (*literal != '\0') {
            if (Done() || Take() != *literal++) {
                throw ParseError("Invalid JSON literal at offset " + std::to_string(pos_));
            }
        }
    }

    std::string ParseString() {
        if (Take() != '"') {
            throw ParseError("Expected string at offset " + std::to_string(pos_));
        }

        std::string out;
        while (!Done()) {
            const char c = Take();
            if (c == '"') {
                return out;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (Done()) {
                throw ParseError("Unterminated JSON escape");
            }
            switch (Take()) {
                case '"':
                    out.push_back('"');
                    break;
                case '\\':
                    out.push_back('\\');
                    break;
                case '/':
                    out.push_back('/');
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                default:
                    throw ParseError("Unsupported JSON escape at offset " + std::to_string(pos_));
            }
        }
        throw ParseError("Unterminated JSON string");
    }

    double ParseNumber() {
        const std::size_t start = pos_;
        if (Peek() == '-') {
            ++pos_;
        }
        while (!Done() && std::isdigit(static_cast<unsigned char>(Peek()))) {
            ++pos_;
        }
        if (!Done() && Peek() == '.') {
            ++pos_;
            while (!Done() && std::isdigit(static_cast<unsigned char>(Peek()))) {
                ++pos_;
            }
        }
        if (!Done() && (Peek() == 'e' || Peek() == 'E')) {
            ++pos_;
            if (!Done() && (Peek() == '+' || Peek() == '-')) {
                ++pos_;
            }
            while (!Done() && std::isdigit(static_cast<unsigned char>(Peek()))) {
                ++pos_;
            }
        }

        double value = 0;
        const auto view = std::string_view(text_).substr(start, pos_ - start);
        const auto result = std::from_chars(view.data(), view.data() + view.size(), value);
        if (result.ec != std::errc()) {
            throw ParseError("Invalid JSON number at offset " + std::to_string(start));
        }
        return value;
    }

    Value::Array ParseArray() {
        (void)Take();
        Value::Array array;
        SkipWhitespace();
        if (!Done() && Peek() == ']') {
            (void)Take();
            return array;
        }

        while (true) {
            array.push_back(ParseValue());
            SkipWhitespace();
            const char next = Take();
            if (next == ']') {
                return array;
            }
            if (next != ',') {
                throw ParseError("Expected ',' or ']' at offset " + std::to_string(pos_));
            }
        }
    }

    Value::Object ParseObject() {
        (void)Take();
        Value::Object object;
        SkipWhitespace();
        if (!Done() && Peek() == '}') {
            (void)Take();
            return object;
        }

        while (true) {
            SkipWhitespace();
            std::string key = ParseString();
            SkipWhitespace();
            if (Take() != ':') {
                throw ParseError("Expected ':' at offset " + std::to_string(pos_));
            }
            object.emplace(std::move(key), ParseValue());
            SkipWhitespace();
            const char next = Take();
            if (next == '}') {
                return object;
            }
            if (next != ',') {
                throw ParseError("Expected ',' or '}' at offset " + std::to_string(pos_));
            }
        }
    }
};

Value Parse(const std::string& text) {
    Parser parser(text);
    Value value = parser.ParseValue();
    parser.ExpectEnd();
    return value;
}

std::string EscapeString(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (const char c : value) {
        switch (c) {
            case '"':
                out << "\\\"";
                break;
            case '\\':
                out << "\\\\";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            default:
                out << c;
                break;
        }
    }
    out << '"';
    return out.str();
}

} // namespace ootmm::json
