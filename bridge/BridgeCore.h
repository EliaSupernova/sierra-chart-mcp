#pragma once

#include <cstddef>
#include <deque>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace sierra_mcp {

constexpr std::size_t kMaximumWireBytes = 64u * 1024u;
constexpr std::size_t kMaximumWireFields = 64u;

class ProtocolError final : public std::runtime_error {
public:
    ProtocolError(std::string code, const std::string& message);
    const std::string& code() const noexcept;

private:
    std::string code_;
};

struct WireRecord {
    int version = 1;
    std::string id;
    std::string action;
    std::map<std::string, std::string> fields;
};

struct Result {
    bool ok = false;
    bool changed = false;
    bool busy = false;
    std::string status = "failed";
    std::string code;
    std::string message;
    std::vector<std::string> warnings;
    std::map<std::string, std::string> data;

    bool operator==(const Result& other) const;
};

struct DispatchContext {
    std::string current_chartbook;
    int calling_chart_number = 0;
    bool configuration_enabled = false;
    bool controller_instance = false;
};

struct ValidatedCommand {
    std::string id;
    std::string action;
    std::string tool;
    std::string operation;
    std::map<std::string, std::string> fields;
    std::string target_chartbook;
    int target_chart_number = 0;
    std::string expected_chartbook;
    int expected_chart_number = 0;
    std::string idempotency_key;
    std::string fingerprint;
    std::string cache_key;
    bool mutation = false;
};

class SierraAdapter {
public:
    virtual ~SierraAdapter() = default;
    virtual Result execute(const ValidatedCommand& command) = 0;
};

enum class CacheLookup { Miss, Replay, Conflict };

class IdempotencyCache {
public:
    virtual ~IdempotencyCache() = default;
    virtual CacheLookup lookup(const std::string& key,
                               const std::string& fingerprint,
                               Result& result) const = 0;
    virtual void store_success(const std::string& key,
                               const std::string& fingerprint,
                               const Result& result) = 0;
};

class MemoryIdempotencyCache final : public IdempotencyCache {
public:
    explicit MemoryIdempotencyCache(std::size_t capacity = 256u);

    CacheLookup lookup(const std::string& key,
                       const std::string& fingerprint,
                       Result& result) const override;
    void store_success(const std::string& key,
                       const std::string& fingerprint,
                       const Result& result) override;
    std::string serialize() const;
    void deserialize(const std::string& text);
    std::size_t size() const noexcept;

private:
    struct Entry {
        std::string key;
        std::string fingerprint;
        Result result;
    };

    std::size_t capacity_;
    std::deque<Entry> entries_;
};

enum class RouteDecision { Handle, Ignore };

std::string percent_encode(const std::string& value);
std::string percent_decode(const std::string& value);
bool is_valid_uuid(const std::string& value);
bool is_safe_field_key(const std::string& value);
bool is_safe_chartbook_name(const std::string& value);
bool is_allowed_action(const std::string& action);
WireRecord decode_wire(const std::string& text);
std::string encode_wire(const WireRecord& record);
RouteDecision route_command(const WireRecord& record,
                            const std::string& current_chartbook,
                            int calling_chart_number,
                            bool controller_instance);
ValidatedCommand validate_command(const WireRecord& record,
                                  const DispatchContext& context);
Result dispatch(const WireRecord& record,
                const DispatchContext& context,
                SierraAdapter& adapter,
                IdempotencyCache& cache);
WireRecord result_record(const std::string& request_id, const Result& result);

}  // namespace sierra_mcp
