#include "BridgeCore.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <utility>

namespace sierra_mcp {
namespace {

const std::set<std::string> kActions = {
    "sierra_status.read", "sierra_status.list", "sierra_status.inspect",
    "sierra_chartbooks.read", "sierra_chartbooks.list", "sierra_chartbooks.inspect",
    "sierra_chartbooks.open", "sierra_chartbooks.save", "sierra_chartbooks.save_as",
    "sierra_chartbooks.duplicate",
    "sierra_charts.read", "sierra_charts.list", "sierra_charts.inspect", "sierra_charts.update", "sierra_charts.close",
    "sierra_studies.read", "sierra_studies.list", "sierra_studies.inspect",
    "sierra_studies.add", "sierra_studies.remove", "sierra_studies.update",
    "sierra_drawings.read", "sierra_drawings.list", "sierra_drawings.inspect",
    "sierra_drawings.add", "sierra_drawings.update", "sierra_drawings.remove",
    "sierra_data.read", "sierra_data.list", "sierra_data.inspect", "sierra_data.status",
    "sierra_data.reload", "sierra_data.recalculate",
    "sierra_diagnostics.logs", "sierra_diagnostics.capabilities"
};

const std::set<std::string> kMutationActions = {
    "sierra_chartbooks.open", "sierra_chartbooks.save", "sierra_chartbooks.save_as",
    "sierra_chartbooks.duplicate", "sierra_charts.update", "sierra_charts.close", "sierra_studies.add",
    "sierra_studies.remove", "sierra_studies.update", "sierra_drawings.add",
    "sierra_drawings.update", "sierra_drawings.remove", "sierra_data.reload",
    "sierra_data.recalculate"
};

bool ascii_alnum(char value) {
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
           (value >= '0' && value <= '9');
}

bool has_control(const std::string& value) {
    for (unsigned char character : value) {
        if (character < 0x20u || character == 0x7fu) return true;
    }
    return false;
}

bool valid_utf8(const std::string& value) {
    std::size_t index = 0;
    while (index < value.size()) {
        const unsigned char first = static_cast<unsigned char>(value[index]);
        if (first <= 0x7f) {
            ++index;
            continue;
        }
        int following = 0;
        unsigned int codepoint = 0;
        unsigned int minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            following = 1; codepoint = first & 0x1fu; minimum = 0x80u;
        } else if (first >= 0xe0 && first <= 0xef) {
            following = 2; codepoint = first & 0x0fu; minimum = 0x800u;
        } else if (first >= 0xf0 && first <= 0xf4) {
            following = 3; codepoint = first & 0x07u; minimum = 0x10000u;
        } else {
            return false;
        }
        if (index + static_cast<std::size_t>(following) >= value.size()) return false;
        for (int offset = 1; offset <= following; ++offset) {
            const unsigned char next = static_cast<unsigned char>(value[index + offset]);
            if ((next & 0xc0u) != 0x80u) return false;
            codepoint = (codepoint << 6u) | (next & 0x3fu);
        }
        if (codepoint < minimum || codepoint > 0x10ffffu ||
            (codepoint >= 0xd800u && codepoint <= 0xdfffu)) return false;
        index += static_cast<std::size_t>(following + 1);
    }
    return true;
}

int hex_value(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

[[noreturn]] void fail(const std::string& code, const std::string& message) {
    throw ProtocolError(code, message);
}

void validate_record(const WireRecord& record) {
    if (record.version != 1) fail("unknown_version", "unknown version");
    if (!is_valid_uuid(record.id)) fail("invalid_uuid", "invalid UUID");
    if (record.action.empty() || !valid_utf8(record.action) || record.action.find('/') != std::string::npos ||
        record.action.find('\\') != std::string::npos || has_control(record.action)) {
        fail("unsafe_action", "unsafe action");
    }
    if (record.fields.size() > kMaximumWireFields) fail("too_many_fields", "too many fields");
    for (const auto& field : record.fields) {
        if (!is_safe_field_key(field.first) || field.first == "version" ||
            field.first == "id" || field.first == "action") {
            fail("unsafe_field_key", "unsafe field key: " + field.first);
        }
        if (!valid_utf8(field.second)) fail("malformed_encoding", "invalid UTF-8");
    }
}

std::string require_field(const WireRecord& record, const std::string& key) {
    const auto found = record.fields.find(key);
    if (found == record.fields.end() || found->second.empty()) {
        fail("missing_field", "missing field: " + key);
    }
    return found->second;
}

int parse_integer(const std::string& value, bool allow_zero, const std::string& key) {
    if (value.empty()) fail("invalid_field", "invalid integer: " + key);
    std::size_t position = 0;
    if (value[0] == '-') position = 1;
    if (position == value.size()) fail("invalid_field", "invalid integer: " + key);
    for (; position < value.size(); ++position) {
        if (value[position] < '0' || value[position] > '9')
            fail("invalid_field", "invalid integer: " + key);
    }
    errno = 0;
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (errno == ERANGE || end == nullptr || *end != '\0' ||
        parsed > std::numeric_limits<int>::max() || parsed < std::numeric_limits<int>::min() ||
        (allow_zero ? parsed < 0 : parsed <= 0)) {
        fail("invalid_field", "invalid integer: " + key);
    }
    return static_cast<int>(parsed);
}

double parse_positive_number(const std::string& value, const std::string& key) {
    if (value.empty()) fail("invalid_field", "invalid number: " + key);
    errno = 0;
    char* end = nullptr;
    const double parsed = std::strtod(value.c_str(), &end);
    if (errno == ERANGE || end == nullptr || *end != '\0' || !std::isfinite(parsed) || parsed <= 0.0)
        fail("invalid_field", "invalid number: " + key);
    return parsed;
}

double parse_finite_number(const std::string& value, const std::string& key) {
    if (value.empty()) fail("invalid_field", "invalid number: " + key);
    errno = 0;
    char* end = nullptr;
    const double parsed = std::strtod(value.c_str(), &end);
    if (errno == ERANGE || end == nullptr || *end != '\0' || !std::isfinite(parsed))
        fail("invalid_field", "invalid number: " + key);
    return parsed;
}

int parse_signed_integer(const std::string& value, const std::string& key) {
    if (value.empty()) fail("invalid_field", "invalid integer: " + key);
    std::size_t position = value[0] == '-' ? 1u : 0u;
    if (position == value.size()) fail("invalid_field", "invalid integer: " + key);
    for (; position < value.size(); ++position)
        if (value[position] < '0' || value[position] > '9') fail("invalid_field", "invalid integer: " + key);
    errno = 0;
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (errno == ERANGE || end == nullptr || *end != '\0' ||
        parsed > std::numeric_limits<int>::max() || parsed < std::numeric_limits<int>::min())
        fail("invalid_field", "invalid integer: " + key);
    return static_cast<int>(parsed);
}

void parse_boolean(const std::string& value, const std::string& key) {
    if (value != "true" && value != "false") fail("invalid_field", "invalid boolean: " + key);
}

void validate_clock(const std::string& value, const std::string& key) {
    if (value.size() != 8 || value[2] != ':' || value[5] != ':')
        fail("invalid_field", "invalid clock: " + key);
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 2 || index == 5) continue;
        if (value[index] < '0' || value[index] > '9') fail("invalid_field", "invalid clock: " + key);
    }
    const int hour = (value[0] - '0') * 10 + value[1] - '0';
    const int minute = (value[3] - '0') * 10 + value[4] - '0';
    const int second = (value[6] - '0') * 10 + value[7] - '0';
    if (hour >= 24 || minute >= 60 || second >= 60) fail("invalid_field", "invalid clock: " + key);
}

void validate_keys(const WireRecord& record, const std::set<std::string>& allowed) {
    for (const auto& field : record.fields) {
        if (allowed.find(field.first) == allowed.end()) fail("unknown_field", "unknown field: " + field.first);
    }
}

std::set<std::string> optional_target() {
    return {"target_chartbook", "target_chart_number"};
}

std::set<std::string> chart_required() {
    return {"target_chartbook", "target_chart_number", "expected_chartbook",
            "expected_chart_number", "idempotency_key"};
}

void insert_keys(std::set<std::string>& destination, std::initializer_list<const char*> keys) {
    for (const char* key : keys) destination.insert(key);
}

void validate_chartbook(const std::string& value) {
    if (!is_safe_chartbook_name(value)) fail("unsafe_chartbook_name", "unsafe chartbook name");
}

bool starts_with(const std::string& value, const char* prefix) {
    return value.compare(0, std::char_traits<char>::length(prefix), prefix) == 0;
}

std::string operation_part(const std::string& action) {
    const std::size_t dot = action.find('.');
    return dot == std::string::npos ? std::string() : action.substr(dot + 1);
}

std::string tool_part(const std::string& action) {
    const std::size_t dot = action.find('.');
    return dot == std::string::npos ? action : action.substr(0, dot);
}

std::string canonical_fingerprint(const WireRecord& record) {
    std::ostringstream output;
    output << record.action << '\n';
    for (const auto& field : record.fields) output << field.first << '=' << percent_encode(field.second) << '\n';
    return output.str();
}

std::string cache_target(const ValidatedCommand& command) {
    std::ostringstream output;
    if (!command.target_chartbook.empty()) output << command.target_chartbook;
    else if (command.fields.count("source_chartbook")) output << command.fields.at("source_chartbook");
    else output << command.expected_chartbook;
    output << '#';
    if (command.target_chart_number > 0) output << command.target_chart_number;
    if (command.fields.count("destination_chartbook")) output << "->" << command.fields.at("destination_chartbook");
    return output.str();
}

std::string serialize_result(const Result& result) {
    WireRecord record;
    record.id = "123e4567-e89b-42d3-a456-426614174000";
    record.action = "result";
    record.fields["ok"] = result.ok ? "true" : "false";
    record.fields["changed"] = result.changed ? "true" : "false";
    record.fields["busy"] = result.busy ? "true" : "false";
    record.fields["status"] = result.status;
    record.fields["code"] = result.code;
    record.fields["message"] = result.message;
    for (std::size_t index = 0; index < result.warnings.size(); ++index) {
        std::ostringstream key;
        key << "warning." << std::setw(3) << std::setfill('0') << index;
        record.fields[key.str()] = result.warnings[index];
    }
    for (const auto& item : result.data) record.fields["data." + item.first] = item.second;
    return encode_wire(record);
}

Result deserialize_result(const std::string& text) {
    const WireRecord record = decode_wire(text);
    if (record.action != "result") fail("malformed_cache", "invalid cached result");
    Result result;
    const auto ok = record.fields.find("ok");
    const auto changed = record.fields.find("changed");
    const auto busy = record.fields.find("busy");
    const auto status = record.fields.find("status");
    if (ok == record.fields.end() || changed == record.fields.end())
        fail("malformed_cache", "missing cached result fields");
    parse_boolean(ok->second, "ok");
    parse_boolean(changed->second, "changed");
    if (busy != record.fields.end()) parse_boolean(busy->second, "busy");
    result.ok = ok->second == "true";
    result.changed = changed->second == "true";
    result.busy = busy != record.fields.end() && busy->second == "true";
    result.status = status != record.fields.end() ? status->second : (result.ok ? "complete" : "failed");
    const auto code = record.fields.find("code");
    const auto message = record.fields.find("message");
    if (code != record.fields.end()) result.code = code->second;
    if (message != record.fields.end()) result.message = message->second;
    for (const auto& item : record.fields) {
        if (starts_with(item.first, "warning.")) result.warnings.push_back(item.second);
        if (starts_with(item.first, "data.")) result.data[item.first.substr(5)] = item.second;
    }
    return result;
}

}  // namespace

ProtocolError::ProtocolError(std::string code, const std::string& message)
    : std::runtime_error(message), code_(std::move(code)) {}

const std::string& ProtocolError::code() const noexcept { return code_; }

bool Result::operator==(const Result& other) const {
    return ok == other.ok && changed == other.changed && busy == other.busy && status == other.status && code == other.code &&
           message == other.message && warnings == other.warnings && data == other.data;
}

std::string percent_encode(const std::string& value) {
    if (!valid_utf8(value)) fail("malformed_encoding", "invalid UTF-8");
    static const char* digits = "0123456789ABCDEF";
    std::string output;
    for (unsigned char character : value) {
        if (ascii_alnum(static_cast<char>(character)) || character == '-' || character == '_' ||
            character == '.' || character == '!' || character == '~' || character == '*' ||
            character == '\'' || character == '(' || character == ')') {
            output.push_back(static_cast<char>(character));
        } else {
            output.push_back('%');
            output.push_back(digits[(character >> 4u) & 0x0fu]);
            output.push_back(digits[character & 0x0fu]);
        }
    }
    return output;
}

std::string percent_decode(const std::string& value) {
    std::string output;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] != '%') {
            output.push_back(value[index]);
            continue;
        }
        if (index + 2 >= value.size()) fail("malformed_encoding", "malformed encoding");
        const int high = hex_value(value[index + 1]);
        const int low = hex_value(value[index + 2]);
        if (high < 0 || low < 0) fail("malformed_encoding", "malformed encoding");
        output.push_back(static_cast<char>((high << 4) | low));
        index += 2;
    }
    if (!valid_utf8(output)) fail("malformed_encoding", "malformed encoding");
    return output;
}

bool is_valid_uuid(const std::string& value) {
    if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
        value[18] != '-' || value[23] != '-') return false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) continue;
        if (hex_value(value[index]) < 0) return false;
    }
    const char version = value[14];
    const char variant = static_cast<char>(value[19] >= 'A' && value[19] <= 'Z' ? value[19] - 'A' + 'a' : value[19]);
    return version >= '1' && version <= '5' && (variant == '8' || variant == '9' || variant == 'a' || variant == 'b');
}

bool is_safe_field_key(const std::string& value) {
    if (value.empty() || value.size() > 64 || !((value[0] >= 'A' && value[0] <= 'Z') ||
        (value[0] >= 'a' && value[0] <= 'z'))) return false;
    for (char character : value) {
        if (!ascii_alnum(character) && character != '_' && character != '.' && character != '-') return false;
    }
    return true;
}

bool is_safe_chartbook_name(const std::string& value) {
    return !value.empty() && value.find('/') == std::string::npos &&
           value.find('\\') == std::string::npos && value.find("..") == std::string::npos &&
           !has_control(value) && valid_utf8(value);
}

bool is_allowed_action(const std::string& action) { return kActions.count(action) != 0; }

WireRecord decode_wire(const std::string& text) {
    if (text.empty() || text.size() > kMaximumWireBytes) fail("invalid_size", "invalid wire size");
    for (unsigned char character : text) {
        if ((character <= 0x08u) || character == 0x0bu || character == 0x0cu ||
            (character >= 0x0eu && character <= 0x1fu) || character == 0x7fu) {
            fail("control_character", "control character");
        }
    }
    std::string body = text;
    if (!body.empty() && body.back() == '\n') body.pop_back();
    std::map<std::string, std::string> values;
    std::size_t start = 0;
    while (start <= body.size()) {
        const std::size_t end = body.find('\n', start);
        const std::string line = body.substr(start, end == std::string::npos ? std::string::npos : end - start);
        const std::size_t equal = line.find('=');
        if (equal == std::string::npos || equal == 0 || equal != line.rfind('=')) fail("malformed_line", "malformed line");
        const std::string key = line.substr(0, equal);
        if (values.count(key)) fail("duplicate_key", "duplicate key");
        if (key != "version" && key != "id" && key != "action" && !is_safe_field_key(key))
            fail("unsafe_field_key", "unsafe field key");
        values[key] = percent_decode(line.substr(equal + 1));
        if (values.size() > kMaximumWireFields + 3u) fail("too_many_fields", "too many fields");
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (values["version"] != "1" || !values.count("id") || !values.count("action"))
        fail("malformed_record", "malformed record");
    WireRecord record;
    record.id = values["id"];
    record.action = values["action"];
    for (const auto& item : values) {
        if (item.first != "version" && item.first != "id" && item.first != "action") record.fields[item.first] = item.second;
    }
    validate_record(record);
    return record;
}

std::string encode_wire(const WireRecord& record) {
    validate_record(record);
    std::ostringstream output;
    output << "version=1\nid=" << percent_encode(record.id) << "\naction=" << percent_encode(record.action) << '\n';
    for (const auto& field : record.fields) output << field.first << '=' << percent_encode(field.second) << '\n';
    const std::string encoded = output.str();
    if (encoded.size() > kMaximumWireBytes) fail("invalid_size", "encoded record too large");
    return encoded;
}

RouteDecision route_command(const WireRecord& record,
                            const std::string& current_chartbook,
                            int calling_chart_number,
                            bool controller_instance) {
    const auto expected = record.fields.find("expected_chartbook");
    const auto target = record.fields.find("target_chartbook");
    const auto source = record.fields.find("source_chartbook");
    const std::string* route = nullptr;
    if (expected != record.fields.end()) route = &expected->second;
    else if (target != record.fields.end()) route = &target->second;
    else if (source != record.fields.end()) route = &source->second;
    if (route != nullptr) {
        if (!is_safe_chartbook_name(*route))
            return controller_instance ? RouteDecision::Handle : RouteDecision::Ignore;
        if (*route != current_chartbook) return RouteDecision::Ignore;
    }

    // Adding a study is the bootstrap path for charts that do not yet have a
    // bridge instance, so the single controller owns it regardless of target.
    if (record.action == "sierra_studies.add")
        return controller_instance ? RouteDecision::Handle : RouteDecision::Ignore;

    // A chart cannot close itself. The controller owns close commands and
    // delegates the target chart number to Sierra Chart.
    if (record.action == "sierra_charts.close")
        return controller_instance ? RouteDecision::Handle : RouteDecision::Ignore;

    // Chartbook operations are global to Sierra/chartbook state and must never
    // race across per-chart bridge instances.
    if (starts_with(record.action, "sierra_chartbooks."))
        return controller_instance ? RouteDecision::Handle : RouteDecision::Ignore;

    const auto target_number = record.fields.find("target_chart_number");
    const auto expected_number = record.fields.find("expected_chart_number");
    const std::string* routed_number = target_number != record.fields.end() ? &target_number->second :
        (expected_number != record.fields.end() ? &expected_number->second : nullptr);
    if (routed_number != nullptr) {
        try {
            const int chart_number = parse_integer(*routed_number, false, "target_chart_number");
            return chart_number == calling_chart_number ? RouteDecision::Handle : RouteDecision::Ignore;
        } catch (const ProtocolError&) {
            return controller_instance ? RouteDecision::Handle : RouteDecision::Ignore;
        }
    }

    // Global operations and reads without a chart target belong only to the
    // controller instance.
    return controller_instance ? RouteDecision::Handle : RouteDecision::Ignore;
}

ValidatedCommand validate_command(const WireRecord& record, const DispatchContext& context) {
    validate_record(record);
    if (!is_allowed_action(record.action)) fail("forbidden_action", "action is not allowed");

    ValidatedCommand command;
    command.id = record.id;
    command.action = record.action;
    command.tool = tool_part(record.action);
    command.operation = operation_part(record.action);
    command.fields = record.fields;
    command.mutation = kMutationActions.count(record.action) != 0;

    std::set<std::string> allowed;
    if (command.operation == "read" || command.operation == "list" || command.operation == "inspect" ||
        command.operation == "status" || command.tool == "sierra_diagnostics") {
        allowed = optional_target();
    } else if (record.action == "sierra_chartbooks.open") {
        allowed = {"target_chartbook", "expected_chartbook", "idempotency_key"};
    } else if (record.action == "sierra_chartbooks.save") {
        allowed = {"target_chartbook", "expected_chartbook", "idempotency_key"};
    } else if (record.action == "sierra_chartbooks.save_as" || record.action == "sierra_chartbooks.duplicate") {
        allowed = {"source_chartbook", "destination_chartbook", "expected_chartbook", "idempotency_key"};
    } else {
        allowed = chart_required();
        if (record.action == "sierra_charts.update") {
            insert_keys(allowed, {"symbol", "session_start", "session_end", "evening_session_start",
                                  "evening_session_end", "use_evening_session", "bar_period_type",
                                  "bar_period_value", "days_to_load"});
        } else if (record.action == "sierra_studies.add") {
            insert_keys(allowed, {"function_name", "short_name"});
        } else if (record.action == "sierra_studies.remove") {
            insert_keys(allowed, {"study_id"});
        } else if (record.action == "sierra_studies.update") {
            insert_keys(allowed, {"study_id", "input_index", "value_type", "value"});
        } else if (record.action == "sierra_drawings.add") {
            insert_keys(allowed, {"drawing_id", "kind", "price", "text", "color"});
        } else if (record.action == "sierra_drawings.update") {
            insert_keys(allowed, {"drawing_id", "price", "text", "color"});
        } else if (record.action == "sierra_drawings.remove") {
            insert_keys(allowed, {"drawing_id"});
        }
    }
    validate_keys(record, allowed);

    auto target_book = record.fields.find("target_chartbook");
    if (target_book != record.fields.end()) {
        validate_chartbook(target_book->second);
        command.target_chartbook = target_book->second;
    }
    auto target_number = record.fields.find("target_chart_number");
    if (target_number != record.fields.end()) command.target_chart_number = parse_integer(target_number->second, false, "target_chart_number");

    if (command.mutation) {
        if (!context.configuration_enabled) fail("configuration_disabled", "configuration changes are disabled");
        command.idempotency_key = require_field(record, "idempotency_key");
        command.expected_chartbook = require_field(record, "expected_chartbook");
        validate_chartbook(command.expected_chartbook);
    }

    const bool chart_mutation = command.mutation && command.tool != "sierra_chartbooks";
    if (chart_mutation) {
        command.target_chartbook = require_field(record, "target_chartbook");
        validate_chartbook(command.target_chartbook);
        command.target_chart_number = parse_integer(require_field(record, "target_chart_number"), false, "target_chart_number");
        command.expected_chart_number = parse_integer(require_field(record, "expected_chart_number"), false, "expected_chart_number");
        if (command.target_chartbook != context.current_chartbook) fail("wrong_chartbook", "target chartbook is handled by another bridge");
        if (command.expected_chartbook != context.current_chartbook ||
            command.expected_chartbook != command.target_chartbook ||
            command.expected_chart_number != command.target_chart_number)
            fail("precondition_failed", "target and expected chart do not match");
        if (command.operation == "close" && command.target_chart_number == context.calling_chart_number)
            fail("calling_chart_close_refused", "the calling chart cannot be closed");
    }

    if (command.mutation && !chart_mutation && command.expected_chartbook != context.current_chartbook)
        fail("precondition_failed", "expected chartbook does not match current chartbook");

    if (record.action == "sierra_chartbooks.open" || record.action == "sierra_chartbooks.save") {
        command.target_chartbook = require_field(record, "target_chartbook");
        validate_chartbook(command.target_chartbook);
        if (record.action == "sierra_chartbooks.save" && command.target_chartbook != context.current_chartbook)
            fail("wrong_chartbook", "save target is not current chartbook");
    }
    if (record.action == "sierra_chartbooks.save_as" || record.action == "sierra_chartbooks.duplicate") {
        validate_chartbook(require_field(record, "source_chartbook"));
        validate_chartbook(require_field(record, "destination_chartbook"));
        if (record.fields.at("source_chartbook") == record.fields.at("destination_chartbook"))
            fail("invalid_field", "source and destination must differ");
    }

    const bool controller_owned_chart_action = record.action == "sierra_studies.add" ||
        record.action == "sierra_charts.close";
    if (controller_owned_chart_action) {
        if (!context.controller_instance)
            fail("controller_required", "chart action must be handled by the controller instance");
    } else if (command.target_chart_number > 0) {
        if (command.target_chart_number != context.calling_chart_number)
            fail("wrong_chart_instance", "target chart is handled by another bridge instance");
    } else if (!context.controller_instance) {
        fail("controller_required", "global or untargeted command requires the controller instance");
    }

    if (record.action == "sierra_charts.update") {
        bool change = false;
        for (const char* key : {"symbol", "session_start", "session_end", "evening_session_start",
                                "evening_session_end", "use_evening_session", "bar_period_type",
                                "bar_period_value", "days_to_load"}) {
            if (record.fields.count(key)) change = true;
        }
        if (!change) fail("missing_field", "update needs a change");
        if (record.fields.count("symbol") && record.fields.at("symbol").empty()) fail("invalid_field", "symbol is empty");
        for (const char* key : {"session_start", "session_end", "evening_session_start", "evening_session_end"})
            if (record.fields.count(key)) validate_clock(record.fields.at(key), key);
        if (record.fields.count("use_evening_session")) parse_boolean(record.fields.at("use_evening_session"), "use_evening_session");
        if (record.fields.count("bar_period_type") && record.fields.at("bar_period_type").empty()) fail("invalid_field", "bar period type is empty");
        if (record.fields.count("bar_period_value")) parse_positive_number(record.fields.at("bar_period_value"), "bar_period_value");
        if (record.fields.count("days_to_load")) parse_integer(record.fields.at("days_to_load"), false, "days_to_load");
    } else if (record.action == "sierra_studies.add") {
        require_field(record, "function_name"); require_field(record, "short_name");
    } else if (record.action == "sierra_studies.remove") {
        parse_integer(require_field(record, "study_id"), false, "study_id");
    } else if (record.action == "sierra_studies.update") {
        parse_integer(require_field(record, "study_id"), false, "study_id");
        parse_integer(require_field(record, "input_index"), true, "input_index");
        const std::string type = require_field(record, "value_type");
        const auto value_field = record.fields.find("value");
        if (value_field == record.fields.end()) fail("missing_field", "missing field: value");
        const std::string& value = value_field->second;
        if (type == "int") parse_signed_integer(value, "value");
        else if (type == "study_id") parse_integer(value, false, "value");
        else if (type == "float") parse_finite_number(value, "value");
        else if (type == "yes_no") parse_boolean(value, "value");
        else if (type != "string") fail("invalid_field", "invalid value_type");
    } else if (starts_with(record.action, "sierra_drawings.") && command.mutation) {
        const std::string drawing_id = require_field(record, "drawing_id");
        if (!is_valid_uuid(drawing_id)) fail("invalid_uuid", "invalid drawing UUID");
        if (record.action == "sierra_drawings.add") {
            if (require_field(record, "kind") != "horizontal_line") fail("invalid_field", "unsupported drawing kind");
            parse_positive_number(require_field(record, "price"), "price");
        } else if (record.action == "sierra_drawings.update") {
            if (!record.fields.count("price") && !record.fields.count("text") && !record.fields.count("color"))
                fail("missing_field", "update needs a change");
            if (record.fields.count("price")) parse_positive_number(record.fields.at("price"), "price");
        }
    }

    command.fingerprint = canonical_fingerprint(record);
    if (command.mutation) command.cache_key = command.idempotency_key + "\n" + command.action + "\n" + cache_target(command);
    return command;
}

Result dispatch(const WireRecord& record,
                const DispatchContext& context,
                SierraAdapter& adapter,
                IdempotencyCache& cache) {
    const ValidatedCommand command = validate_command(record, context);
    if (command.mutation) {
        Result cached;
        const CacheLookup lookup = cache.lookup(command.cache_key, command.fingerprint, cached);
        if (lookup == CacheLookup::Replay) return cached;
        if (lookup == CacheLookup::Conflict) fail("idempotency_conflict", "idempotency key reused with different command");
    }
    Result result = adapter.execute(command);
    if (result.code == "queued" || (result.code.size() > 7 &&
        result.code.compare(result.code.size() - 7, 7, "_queued") == 0)) {
        result.status = "queued";
        result.busy = false;
    } else if (result.busy || result.status == "busy") {
        result.status = "busy";
        result.busy = true;
    } else if (result.ok) {
        result.status = "complete";
        result.busy = false;
    } else {
        result.status = "failed";
        result.busy = false;
    }
    if (command.mutation && result.ok)
        cache.store_success(command.cache_key, command.fingerprint, result);
    return result;
}

WireRecord result_record(const std::string& request_id, const Result& result) {
    WireRecord record;
    record.id = request_id;
    record.action = "result";
    record.fields["ok"] = result.ok ? "true" : "false";
    record.fields["changed"] = result.changed ? "true" : "false";
    record.fields["busy"] = result.busy ? "true" : "false";
    record.fields["status"] = result.status;
    record.fields["code"] = result.code;
    record.fields["message"] = result.message;
    std::string warnings;
    for (std::size_t index = 0; index < result.warnings.size(); ++index) {
        if (index != 0) warnings.push_back('|');
        warnings += result.warnings[index];
    }
    record.fields["warnings"] = warnings;
    for (const auto& item : result.data) {
        if (!is_safe_field_key(item.first)) continue;
        if (item.first == "ok" || item.first == "changed" || item.first == "busy" ||
            item.first == "status" || item.first == "code" ||
            item.first == "message" || item.first == "warnings") continue;
        record.fields[item.first] = item.second;
    }
    return record;
}

MemoryIdempotencyCache::MemoryIdempotencyCache(std::size_t capacity)
    : capacity_(capacity == 0 ? 1u : capacity) {}

CacheLookup MemoryIdempotencyCache::lookup(const std::string& key,
                                           const std::string& fingerprint,
                                           Result& result) const {
    for (auto iterator = entries_.rbegin(); iterator != entries_.rend(); ++iterator) {
        if (iterator->key != key) continue;
        if (iterator->fingerprint != fingerprint) return CacheLookup::Conflict;
        result = iterator->result;
        return CacheLookup::Replay;
    }
    return CacheLookup::Miss;
}

void MemoryIdempotencyCache::store_success(const std::string& key,
                                           const std::string& fingerprint,
                                           const Result& result) {
    if (!result.ok) return;
    for (auto iterator = entries_.begin(); iterator != entries_.end(); ++iterator) {
        if (iterator->key == key) {
            entries_.erase(iterator);
            break;
        }
    }
    entries_.push_back({key, fingerprint, result});
    while (entries_.size() > capacity_) entries_.pop_front();
}

std::string MemoryIdempotencyCache::serialize() const {
    std::ostringstream output;
    output << "cache_version=2\n";
    for (const Entry& entry : entries_) {
        output << percent_encode(entry.key) << '\t' << percent_encode(entry.fingerprint) << '\t'
               << percent_encode(serialize_result(entry.result)) << '\n';
    }
    return output.str();
}

void MemoryIdempotencyCache::deserialize(const std::string& text) {
    std::deque<Entry> restored;
    std::istringstream input(text);
    std::string line;
    if (!std::getline(input, line) || (line != "cache_version=1" && line != "cache_version=2"))
        fail("malformed_cache", "unknown cache version");
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        const std::size_t first = line.find('\t');
        const std::size_t second = first == std::string::npos ? std::string::npos : line.find('\t', first + 1);
        if (first == std::string::npos || second == std::string::npos || line.find('\t', second + 1) != std::string::npos)
            fail("malformed_cache", "malformed cache entry");
        Entry entry;
        entry.key = percent_decode(line.substr(0, first));
        entry.fingerprint = percent_decode(line.substr(first + 1, second - first - 1));
        entry.result = deserialize_result(percent_decode(line.substr(second + 1)));
        restored.push_back(std::move(entry));
        while (restored.size() > capacity_) restored.pop_front();
    }
    entries_.swap(restored);
}

std::size_t MemoryIdempotencyCache::size() const noexcept { return entries_.size(); }

}  // namespace sierra_mcp
