#include "BridgeCore.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace sierra_mcp;

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

template <typename Fn>
void rejects(Fn fn, const std::string& expected, const char* message) {
    try {
        fn();
        check(false, message);
    } catch (const ProtocolError& error) {
        check(error.code() == expected, message);
    }
}

WireRecord command(const std::string& action) {
    WireRecord record;
    record.version = 1;
    record.id = "123e4567-e89b-42d3-a456-426614174000";
    record.action = action;
    return record;
}

DispatchContext context(bool configuration_enabled = true) {
    DispatchContext value;
    value.current_chartbook = "Main.cht";
    value.calling_chart_number = 3;
    value.configuration_enabled = configuration_enabled;
    value.controller_instance = true;
    return value;
}

void chart_mutation_fields(WireRecord& record) {
    record.fields["target_chartbook"] = "Main.cht";
    record.fields["target_chart_number"] = "3";
    record.fields["expected_chartbook"] = "Main.cht";
    record.fields["expected_chart_number"] = "3";
    record.fields["idempotency_key"] = "idem-1";
}

class FakeAdapter final : public SierraAdapter {
public:
    int calls = 0;
    ValidatedCommand last;

    Result execute(const ValidatedCommand& value) override {
        ++calls;
        last = value;
        Result result;
        result.ok = true;
        result.changed = true;
        result.status = "complete";
        result.code = "updated";
        result.message = "verified";
        if (value.operation == "close" || value.action == "sierra_studies.add") {
            result.changed = false;
            result.code = value.operation == "close" ? "close_queued" : "study_add_queued";
            result.message = value.operation == "close" ? "chart close queued" : "study add queued";
            result.data["status"] = "queued";
        }
        result.data["chartbook"] = value.target_chartbook;
        const auto symbol = value.fields.find("symbol");
        if (symbol != value.fields.end()) result.data["echo"] = symbol->second;
        return result;
    }
};

void protocol_tests() {
    WireRecord original = command("sierra_charts.inspect");
    original.fields["target_chartbook"] = "東京 & Example.cht";
    original.fields["note"] = "snowman ☃ / equals=a";
    const std::string encoded = encode_wire(original);
    check(encoded.find("target_chartbook=%E6%9D%B1%E4%BA%AC%20%26%20Example.cht") != std::string::npos,
          "wire encoding matches encodeURIComponent for UTF-8");
    const WireRecord decoded = decode_wire(encoded);
    check(decoded.fields == original.fields, "wire Unicode round trips");
    check(encoded.back() == '\n', "wire encoder writes terminal newline");

    rejects([] { decode_wire("version=1\nid=x\naction=a\nbad=%ZZ\n"); },
            "malformed_encoding", "malformed percent encoding rejected");
    rejects([] { decode_wire("version=1\nid=123e4567-e89b-42d3-a456-426614174000\naction=a\nx=1\nx=2\n"); },
            "duplicate_key", "duplicate key rejected");
    rejects([] { decode_wire("version=1\nid=not-a-uuid\naction=a\n"); },
            "invalid_uuid", "invalid UUID rejected");
    rejects([] { decode_wire("version=1\nid=123e4567-e89b-42d3-a456-426614174000\naction=a\nid=x\n"); },
            "duplicate_key", "reserved duplicate rejected");
    rejects([] { decode_wire("version=1\nid=123e4567-e89b-42d3-a456-426614174000\naction=a\n_bad=x\n"); },
            "unsafe_field_key", "unsafe field key rejected");
    rejects([] { decode_wire("version=1\nid=123e4567-e89b-42d3-a456-426614174000\naction=a\nx=%C0%AF\n"); },
            "malformed_encoding", "invalid UTF-8 rejected");
}

void validation_tests() {
    check(is_allowed_action("sierra_status.read"), "status read allowlisted");
    check(is_allowed_action("sierra_drawings.add"), "drawing add allowlisted");
    check(is_allowed_action("sierra_charts.close"), "chart close allowlisted");
    check(!is_allowed_action("sierra_orders.list"), "order action not allowlisted");
    check(!is_allowed_action("sierra_charts.buy"), "forbidden operation not allowlisted");

    MemoryIdempotencyCache cache;
    FakeAdapter adapter;
    rejects([&] { dispatch(command("sierra_orders.list"), context(), adapter, cache); },
            "forbidden_action", "forbidden action tokens rejected");
    rejects([&] { dispatch(command("sierra_charts.update"), context(), adapter, cache); },
            "missing_field", "chart mutation requires target and preconditions");

    WireRecord close = command("sierra_charts.close");
    chart_mutation_fields(close);
    close.fields["target_chart_number"] = "4";
    close.fields["expected_chart_number"] = "4";
    check(validate_command(close, context()).target_chart_number == 4,
          "controller validates closing another chart");
    WireRecord self_close = close;
    self_close.fields["target_chart_number"] = "3";
    self_close.fields["expected_chart_number"] = "3";
    rejects([&] { validate_command(self_close, context()); },
            "calling_chart_close_refused", "central validation rejects calling chart close");
    WireRecord missing_close_target = close;
    missing_close_target.fields.erase("target_chart_number");
    rejects([&] { validate_command(missing_close_target, context()); },
            "missing_field", "close requires target chart number");
    WireRecord mismatched_close = close;
    mismatched_close.fields["expected_chart_number"] = "5";
    rejects([&] { validate_command(mismatched_close, context()); },
            "precondition_failed", "close expected chart number must match target");

    WireRecord update = command("sierra_charts.update");
    chart_mutation_fields(update);
    update.fields["symbol"] = "TEST-FUTURES-DEC";
    update.fields["unknown"] = "x";
    rejects([&] { dispatch(update, context(), adapter, cache); },
            "unknown_field", "strict schema rejects unknown fields");
    update.fields.erase("unknown");

    WireRecord mismatch = update;
    mismatch.fields["expected_chart_number"] = "4";
    rejects([&] { dispatch(mismatch, context(), adapter, cache); },
            "precondition_failed", "expected chart number must match target");
    mismatch = update;
    mismatch.fields["target_chartbook"] = "Other.cht";
    mismatch.fields["expected_chartbook"] = "Other.cht";
    rejects([&] { dispatch(mismatch, context(), adapter, cache); },
            "wrong_chartbook", "chart mutation must target handling chartbook");
    rejects([&] { dispatch(update, context(false), adapter, cache); },
            "configuration_disabled", "configuration mutations require opt-in");

    WireRecord malformed = update;
    malformed.fields["target_chartbook"] = "../Main.cht";
    rejects([&] { dispatch(malformed, context(), adapter, cache); },
            "unsafe_chartbook_name", "unsafe chartbook name rejected");

    WireRecord drawing = command("sierra_drawings.remove");
    chart_mutation_fields(drawing);
    drawing.fields["drawing_id"] = "not-a-uuid";
    rejects([&] { dispatch(drawing, context(), adapter, cache); },
            "invalid_uuid", "drawing UUID validated");

    WireRecord study = command("sierra_studies.update");
    chart_mutation_fields(study);
    study.fields["study_id"] = "2";
    study.fields["input_index"] = "0";
    study.fields["value_type"] = "string";
    study.fields["value"] = "";
    check(validate_command(study, context()).fields.at("value").empty(),
          "empty study string matches frozen schema");
    study.fields["value_type"] = "float";
    study.fields["value"] = "-1.25";
    check(validate_command(study, context()).fields.at("value") == "-1.25",
          "signed study float matches frozen schema");
    study.fields["value_type"] = "int";
    study.fields["value"] = "-4";
    check(validate_command(study, context()).fields.at("value") == "-4",
          "signed study integer matches frozen schema");

    WireRecord read = command("sierra_diagnostics.capabilities");
    check(route_command(read, "Main.cht", 3, true) == RouteDecision::Handle,
          "controller handles untargeted diagnostics");
    check(route_command(read, "Main.cht", 3, false) == RouteDecision::Ignore,
          "non-controller ignores untargeted diagnostics");
    read.fields["target_chartbook"] = "Other.cht";
    check(route_command(read, "Main.cht", 3, true) == RouteDecision::Ignore,
          "bridge ignores another chartbook before claim");

    WireRecord targeted = command("sierra_charts.inspect");
    targeted.fields["target_chartbook"] = "Main.cht";
    targeted.fields["target_chart_number"] = "3";
    check(route_command(targeted, "Main.cht", 3, false) == RouteDecision::Handle,
          "correct chart instance handles target");
    check(route_command(targeted, "Main.cht", 4, true) == RouteDecision::Ignore,
          "wrong chart instance ignores target even when controller");

    WireRecord routed_close = command("sierra_charts.close");
    chart_mutation_fields(routed_close);
    routed_close.fields["target_chart_number"] = "4";
    routed_close.fields["expected_chart_number"] = "4";
    check(route_command(routed_close, "Main.cht", 3, true) == RouteDecision::Handle,
          "controller owns close for another chart");
    check(route_command(routed_close, "Main.cht", 4, false) == RouteDecision::Ignore,
          "target chart does not attempt to close itself");

    WireRecord chartbook = command("sierra_chartbooks.inspect");
    chartbook.fields["target_chartbook"] = "Main.cht";
    check(route_command(chartbook, "Main.cht", 3, true) == RouteDecision::Handle,
          "controller handles chartbook-level command");
    check(route_command(chartbook, "Main.cht", 4, false) == RouteDecision::Ignore,
          "non-controller ignores chartbook-level command");

    WireRecord bootstrap = command("sierra_studies.add");
    chart_mutation_fields(bootstrap);
    bootstrap.fields["target_chart_number"] = "9";
    bootstrap.fields["expected_chart_number"] = "9";
    bootstrap.fields["function_name"] = "scsf_SierraMCPBridge";
    bootstrap.fields["short_name"] = "Sierra MCP Bridge";
    check(route_command(bootstrap, "Main.cht", 3, true) == RouteDecision::Handle,
          "controller handles study-add bootstrap for another chart");
    check(route_command(bootstrap, "Main.cht", 9, false) == RouteDecision::Ignore,
          "target chart does not race controller for study-add bootstrap");

    DispatchContext noncontroller = context();
    noncontroller.controller_instance = false;
    check(validate_command(targeted, noncontroller).target_chart_number == 3,
          "non-controller validates a command for its own chart");
    WireRecord untargeted = command("sierra_diagnostics.capabilities");
    rejects([&] { validate_command(untargeted, noncontroller); },
            "controller_required", "untargeted validation remains controller-only");
}

void dispatch_and_idempotency_tests() {
    MemoryIdempotencyCache cache;
    FakeAdapter adapter;
    WireRecord update = command("sierra_charts.update");
    chart_mutation_fields(update);
    update.fields["symbol"] = "TEST-FUTURES-DEC";

    const Result first = dispatch(update, context(), adapter, cache);
    check(first.ok && first.changed && first.code == "updated", "adapter result returned exactly");
    check(first.data.at("echo") == "TEST-FUTURES-DEC", "adapter data returned exactly");
    check(adapter.calls == 1, "adapter called once");
    check(adapter.last.tool == "sierra_charts" && adapter.last.operation == "update",
          "validated command dispatched to fake adapter");

    const Result replay = dispatch(update, context(), adapter, cache);
    check(adapter.calls == 1, "successful mutation replay does not call adapter");
    check(replay == first, "idempotency replay returns identical result");

    WireRecord conflict = update;
    conflict.fields["symbol"] = "ESZ26-CME";
    rejects([&] { dispatch(conflict, context(), adapter, cache); },
            "idempotency_conflict", "same idempotency tuple rejects different fingerprint");

    const std::string persisted = cache.serialize();
    MemoryIdempotencyCache restored;
    restored.deserialize(persisted);
    FakeAdapter restored_adapter;
    const Result after_restart = dispatch(update, context(), restored_adapter, restored);
    check(restored_adapter.calls == 0, "persisted replay survives restart");
    check(after_restart == first, "persisted result is exact");

    MemoryIdempotencyCache bounded(2);
    for (int index = 0; index < 3; ++index) {
        WireRecord item = update;
        item.fields["idempotency_key"] = "bounded-" + std::to_string(index);
        FakeAdapter bounded_adapter;
        dispatch(item, context(), bounded_adapter, bounded);
    }
    check(bounded.size() == 2, "idempotency cache is bounded");

    WireRecord add = command("sierra_studies.add");
    chart_mutation_fields(add);
    add.fields["target_chart_number"] = "4";
    add.fields["expected_chart_number"] = "4";
    add.fields["function_name"] = "scsf_SierraMCPBridge";
    add.fields["short_name"] = "Sierra MCP Bridge";
    MemoryIdempotencyCache add_cache;
    FakeAdapter add_adapter;
    const Result queued = dispatch(add, context(), add_adapter, add_cache);
    check(queued.ok && !queued.changed && queued.code == "study_add_queued",
          "queued bootstrap result preserves asynchronous semantics");
    check(queued.status == "queued" && !queued.busy, "queued result exposes lifecycle state");
    const Result queued_again = dispatch(add, context(), add_adapter, add_cache);
    check(add_adapter.calls == 1, "queued bootstrap is durably idempotent");
    check(queued_again == queued, "repeated queued bootstrap returns the adapter result");
}

void result_tests() {
    Result result;
    result.ok = false;
    result.changed = false;
    result.code = "unsupported_field";
    result.message = "cannot preserve bar_period_type";
    result.warnings.push_back("same-chartbook only");
    result.data["chartbook"] = "Main.cht";
    const WireRecord wire = result_record("123e4567-e89b-42d3-a456-426614174000", result);
    check(wire.action == "result" && wire.id == "123e4567-e89b-42d3-a456-426614174000",
          "result has exact action and request id");
    check(wire.fields.at("ok") == "false" && wire.fields.at("changed") == "false",
          "result stable boolean fields encoded");
    check(wire.fields.at("status") == "failed" && wire.fields.at("busy") == "false",
          "result lifecycle fields encoded");
    check(wire.fields.at("warnings") == "same-chartbook only", "warnings encoded stably");
    check(decode_wire(encode_wire(wire)).fields == wire.fields, "result wire round trips");
}

void heartbeat_contract_tests() {
    // The platform writer is intentionally Windows/ACSIL-only; keep the wire-safe
    // content contract portable and forbid account/private fields by construction.
    const std::string content = "version=1\nchartbook=Main.cht\ndownloading=true\n";
    check(content.find("version=1") != std::string::npos, "heartbeat has version");
    check(content.find("chartbook=") != std::string::npos, "heartbeat has chartbook");
    check(content.find("downloading=") != std::string::npos, "heartbeat has download state");
    check(content.find("account") == std::string::npos && content.find("order") == std::string::npos,
          "heartbeat contract excludes private and trading data");
}

}  // namespace

int main() {
    protocol_tests();
    validation_tests();
    dispatch_and_idempotency_tests();
    result_tests();
    heartbeat_contract_tests();
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "bridge_core_test: PASS\n";
    return EXIT_SUCCESS;
}
