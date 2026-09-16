#include "BridgeCore.h"
#include "SierraAdapter.h"

#include "sierrachart.h"

#include <windows.h>

#include <cstdint>
#include <sstream>
#include <string>

namespace {

constexpr int kAllowConfigurationChangesInput = 0;
constexpr int kControllerInstanceInput = 1;
constexpr int kRuntimePointerKey = 716001;
constexpr LONGLONG kMaximumStateBytes = 24ll * 1024ll * 1024ll;
constexpr ULONGLONG kHeartbeatIntervalMs = 5000ull;

struct RuntimeState {
    sierra_mcp::MemoryIdempotencyCache cache;
    std::string chartbook;
    int chart_number = 0;
    std::wstring journal_path;
    ULONGLONG last_heartbeat_tick = 0;
};

std::string sc_text(const SCString& value) {
    const char* characters = value.GetChars();
    return characters == nullptr ? std::string() : std::string(characters);
}

std::wstring utf8_to_wide(const std::string& value) {
    if (value.empty()) return std::wstring();
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0);
    if (needed <= 0) return std::wstring();
    std::wstring output(static_cast<std::size_t>(needed), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), &output[0], needed) != needed) return std::wstring();
    return output;
}

std::string wide_to_utf8(const std::wstring& value) {
    if (value.empty()) return std::string();
    const int needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return std::string();
    std::string output(static_cast<std::size_t>(needed), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), &output[0], needed, nullptr, nullptr) != needed) return std::string();
    return output;
}

std::wstring join_path(const std::wstring& directory, const std::wstring& name) {
    if (directory.empty()) return name;
    if (directory.back() == L'\\') return directory + name;
    return directory + L"\\" + name;
}

bool safe_directory(const std::wstring& path, bool create) {
    DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES && create) {
        if (CreateDirectoryW(path.c_str(), nullptr) == FALSE && GetLastError() != ERROR_ALREADY_EXISTS) return false;
        attributes = GetFileAttributesW(path.c_str());
    }
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

bool safe_regular_handle(HANDLE file) {
    FILE_ATTRIBUTE_TAG_INFO tag = {};
    FILE_STANDARD_INFO standard = {};
    return GetFileInformationByHandleEx(file, FileAttributeTagInfo, &tag, sizeof(tag)) != 0 &&
           GetFileInformationByHandleEx(file, FileStandardInfo, &standard, sizeof(standard)) != 0 &&
           (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 && standard.Directory == FALSE;
}

bool read_regular_file(const std::wstring& path, LONGLONG maximum, std::string& output) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {};
    const bool safe = safe_regular_handle(file) && GetFileSizeEx(file, &size) != 0 &&
                      size.QuadPart >= 0 && size.QuadPart <= maximum;
    if (!safe) {
        CloseHandle(file);
        return false;
    }
    output.assign(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const BOOL ok = output.empty() || ReadFile(file, &output[0], static_cast<DWORD>(output.size()), &read, nullptr);
    CloseHandle(file);
    return ok != FALSE && read == output.size();
}

bool write_file_and_flush(HANDLE file, const std::string& text) {
    DWORD written = 0;
    const BOOL write_ok = text.empty() || WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    return write_ok != FALSE && written == text.size() && FlushFileBuffers(file) != FALSE;
}

bool write_atomic_replace(const std::wstring& path, const std::string& text) {
    std::wostringstream temporary_name;
    temporary_name << path << L"." << GetCurrentProcessId() << L"." << GetTickCount64() << L".tmp";
    const std::wstring temporary = temporary_name.str();
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const bool wrote = write_file_and_flush(file, text);
    CloseHandle(file);
    if (!wrote) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

void refresh_heartbeat(SCStudyInterfaceRef sc, RuntimeState& runtime) {
    const ULONGLONG now = GetTickCount64();
    if (runtime.last_heartbeat_tick != 0 && now - runtime.last_heartbeat_tick < kHeartbeatIntervalMs) return;

    const std::wstring data_directory = utf8_to_wide(sc_text(sc.DataFilesFolder()));
    if (data_directory.empty() || !safe_directory(data_directory, false)) return;
    const std::wstring root = join_path(join_path(data_directory, L"SierraMCP"), L"state");
    if (!safe_directory(join_path(data_directory, L"SierraMCP"), true) || !safe_directory(root, true)) return;

    const bool downloading = sc.DownloadingHistoricalData != 0 ||
        (sc.ChartIsDownloadingHistoricalData != nullptr && sc.ChartIsDownloadingHistoricalData(sc.ChartNumber) != 0);
    std::ostringstream content;
    content << "version=1\nchartbook=" << sierra_mcp::percent_encode(sc_text(sc.ChartbookName()))
            << "\ndownloading=" << (downloading ? "true" : "false") << "\n";
    if (write_atomic_replace(join_path(root, L"bridge-heartbeat"), content.str()))
        runtime.last_heartbeat_tick = now;
}

void append_audit(const std::wstring& path,
                  const std::string& id,
                  const std::string& action,
                  const sierra_mcp::Result& result,
                  const std::string& event) {
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE || !safe_regular_handle(file)) {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        return;
    }
    std::ostringstream line;
    line << GetTickCount64() << '\t' << event << '\t' << id << '\t' << action << '\t'
         << result.code << '\t' << (result.ok ? "ok" : "error") << '\t'
         << (result.changed ? "changed" : "unchanged") << "\r\n";
    DWORD written = 0;
    const std::string text = line.str();
    WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    FlushFileBuffers(file);
    CloseHandle(file);
}

std::string filename_request_id(const std::wstring& name) {
    static const std::wstring suffix = L".command";
    if (name.size() != 36u + suffix.size() || name.substr(36) != suffix) return std::string();
    const std::string id = wide_to_utf8(name.substr(0, 36));
    return sierra_mcp::is_valid_uuid(id) ? id : std::string();
}

RuntimeState* runtime_state(SCStudyInterfaceRef sc,
                            const std::wstring& state_directory,
                            const std::wstring& audit_path) {
    void*& stored = sc.GetPersistentPointer(kRuntimePointerKey);
    RuntimeState* runtime = static_cast<RuntimeState*>(stored);
    const std::string chartbook = sc_text(sc.ChartbookName());
    if (runtime != nullptr && runtime->chartbook == chartbook && runtime->chart_number == sc.ChartNumber) return runtime;
    delete runtime;
    runtime = new RuntimeState;
    runtime->chartbook = chartbook;
    runtime->chart_number = sc.ChartNumber;
    runtime->journal_path = join_path(state_directory,
        utf8_to_wide(sierra_mcp::percent_encode(chartbook)) + L"." +
        std::to_wstring(sc.ChartNumber) + L".idempotency");
    std::string persisted;
    if (read_regular_file(runtime->journal_path, kMaximumStateBytes, persisted)) {
        try {
            runtime->cache.deserialize(persisted);
        } catch (const std::exception&) {
            sierra_mcp::Result warning;
            warning.code = "state_load_failed";
            append_audit(audit_path, "-", "state.load", warning, "warning");
        }
    }
    stored = runtime;
    return runtime;
}

bool publish_result(const std::wstring& outbox,
                    const std::string& id,
                    const sierra_mcp::Result& result) {
    std::string encoded;
    try {
        encoded = sierra_mcp::encode_wire(sierra_mcp::result_record(id, result));
    } catch (const std::exception&) {
        sierra_mcp::Result compact;
        compact.ok = false;
        compact.changed = false;
        compact.code = "response_too_large";
        compact.message = "bridge response exceeded 64 KiB";
        encoded = sierra_mcp::encode_wire(sierra_mcp::result_record(id, compact));
    }
    std::wostringstream temporary_name;
    temporary_name << L"." << utf8_to_wide(id) << L"." << GetCurrentProcessId() << L"."
                   << GetTickCount64() << L".tmp";
    const std::wstring temporary = join_path(outbox, temporary_name.str());
    const std::wstring destination = join_path(outbox, utf8_to_wide(id) + L".result");
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const bool wrote = write_file_and_flush(file, encoded);
    CloseHandle(file);
    if (!wrote) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    if (MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH) == FALSE) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

void archive_claim(const std::wstring& processing,
                   const std::wstring& archive,
                   const std::string& id) {
    std::wostringstream name;
    name << utf8_to_wide(id) << L"." << GetCurrentProcessId() << L"." << GetTickCount64() << L".command";
    MoveFileExW(processing.c_str(), join_path(archive, name.str()).c_str(), MOVEFILE_WRITE_THROUGH);
}

bool process_one(SCStudyInterfaceRef sc) {
    const std::wstring data_directory = utf8_to_wide(sc_text(sc.DataFilesFolder()));
    if (data_directory.empty() || !safe_directory(data_directory, false)) return false;
    const std::wstring root = join_path(data_directory, L"SierraMCP");
    const std::wstring inbox = join_path(root, L"inbox");
    const std::wstring outbox = join_path(root, L"outbox");
    const std::wstring processing_directory = join_path(root, L"processing");
    const std::wstring archive = join_path(root, L"archive");
    const std::wstring state_directory = join_path(root, L"state");
    if (!safe_directory(root, true) || !safe_directory(inbox, true) ||
        !safe_directory(outbox, true) || !safe_directory(processing_directory, true) ||
        !safe_directory(archive, true) || !safe_directory(state_directory, true)) return false;
    const std::wstring audit_path = join_path(state_directory, L"bridge-audit.log");

    WIN32_FIND_DATAW found = {};
    HANDLE search = FindFirstFileW(join_path(inbox, L"*.command").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return false;
    bool processed = false;
    do {
        if ((found.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) continue;
        const std::wstring filename = found.cFileName;
        const std::string file_id = filename_request_id(filename);
        if (file_id.empty()) continue;
        const std::wstring source = join_path(inbox, filename);
        std::string preview_text;
        if (!read_regular_file(source, static_cast<LONGLONG>(sierra_mcp::kMaximumWireBytes), preview_text)) continue;

        bool should_handle = sc.Input[kControllerInstanceInput].GetYesNo() != 0;
        try {
            const sierra_mcp::WireRecord preview = sierra_mcp::decode_wire(preview_text);
            if (preview.id != file_id) should_handle = sc.Input[kControllerInstanceInput].GetYesNo() != 0;
            else should_handle = sierra_mcp::route_command(preview, sc_text(sc.ChartbookName()), sc.ChartNumber,
                sc.Input[kControllerInstanceInput].GetYesNo() != 0) == sierra_mcp::RouteDecision::Handle;
        } catch (const std::exception&) {
            should_handle = sc.Input[kControllerInstanceInput].GetYesNo() != 0;
        }
        if (!should_handle) continue;

        std::wostringstream processing_name;
        processing_name << utf8_to_wide(file_id) << L"." << GetCurrentProcessId() << L"."
                        << sc.ChartNumber << L"." << GetTickCount64() << L".processing";
        const std::wstring claimed = join_path(processing_directory, processing_name.str());
        if (MoveFileExW(source.c_str(), claimed.c_str(), MOVEFILE_WRITE_THROUGH) == FALSE) continue;

        sierra_mcp::Result result;
        std::string action = "invalid";
        RuntimeState* runtime = runtime_state(sc, state_directory, audit_path);
        try {
            std::string claimed_text;
            if (!read_regular_file(claimed, static_cast<LONGLONG>(sierra_mcp::kMaximumWireBytes), claimed_text))
                throw sierra_mcp::ProtocolError("unsafe_command_file", "claimed command is not a safe regular file");
            const sierra_mcp::WireRecord record = sierra_mcp::decode_wire(claimed_text);
            if (record.id != file_id) throw sierra_mcp::ProtocolError("request_id_mismatch", "filename and record ID differ");
            action = record.action;
            sierra_mcp::DispatchContext context;
            context.current_chartbook = sc_text(sc.ChartbookName());
            context.calling_chart_number = sc.ChartNumber;
            context.configuration_enabled = sc.Input[kAllowConfigurationChangesInput].GetYesNo() != 0;
            context.controller_instance = sc.Input[kControllerInstanceInput].GetYesNo() != 0;
            sierra_mcp::ACSILSierraAdapter adapter(sc, state_directory, audit_path, context.configuration_enabled);
            result = sierra_mcp::dispatch(record, context, adapter, runtime->cache);
            if (result.ok && !write_atomic_replace(runtime->journal_path, runtime->cache.serialize())) {
                result.warnings.push_back("idempotency journal persistence failed");
            }
        } catch (const sierra_mcp::ProtocolError& error) {
            result.ok = false;
            result.changed = false;
            result.code = error.code();
            result.message = error.what();
        } catch (const std::exception& error) {
            result.ok = false;
            result.changed = false;
            result.code = "internal_error";
            result.message = error.what();
        } catch (...) {
            result.ok = false;
            result.changed = false;
            result.code = "internal_error";
            result.message = "unknown bridge error";
        }

        const bool published = publish_result(outbox, file_id, result);
        append_audit(audit_path, file_id, action, result, published ? "result" : "publish_failed");
        if (published) archive_claim(claimed, archive, file_id);
        processed = true;
        break;
    } while (FindNextFileW(search, &found) != 0);
    FindClose(search);
    return processed;
}

void cleanup_runtime(SCStudyInterfaceRef sc) {
    void*& stored = sc.GetPersistentPointer(kRuntimePointerKey);
    delete static_cast<RuntimeState*>(stored);
    stored = nullptr;
}

}  // namespace

SCSFExport scsf_SierraMCPBridge(SCStudyInterfaceRef sc) {
    SCInputRef allow_configuration = sc.Input[kAllowConfigurationChangesInput];
    SCInputRef controller_instance = sc.Input[kControllerInstanceInput];

    if (sc.SetDefaults) {
        sc.GraphName = "Sierra MCP Bridge";
        sc.StudyDescription = "Local file-queue bridge for non-order Sierra Chart inspection and configuration.";
        sc.AutoLoop = 0;
        sc.UpdateAlways = 1;
        sc.FreeDLL = 0;

        allow_configuration.Name = "Allow Configuration Changes";
        allow_configuration.SetYesNo(1);
        controller_instance.Name = "Controller Instance";
        controller_instance.SetYesNo(0);
        return;
    }

    if (sc.LastCallToFunction) {
        cleanup_runtime(sc);
        return;
    }

    const std::wstring data_directory = utf8_to_wide(sc_text(sc.DataFilesFolder()));
    const std::wstring state_directory = join_path(join_path(data_directory, L"SierraMCP"), L"state");
    const std::wstring audit_path = join_path(state_directory, L"bridge-audit.log");
    RuntimeState* runtime = runtime_state(sc, state_directory, audit_path);
    if (controller_instance.GetYesNo() != 0) refresh_heartbeat(sc, *runtime);
    process_one(sc);
}
