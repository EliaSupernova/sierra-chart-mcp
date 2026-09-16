#include "SierraAdapter.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <sstream>
#include <vector>

namespace sierra_mcp {
namespace {

Result success(const std::string& code, const std::string& message, bool changed = false) {
    Result result;
    result.ok = true;
    result.changed = changed;
    result.status = "complete";
    result.code = code;
    result.message = message;
    return result;
}

Result failure(const std::string& code, const std::string& message) {
    Result result;
    result.ok = false;
    result.changed = false;
    result.status = "failed";
    result.code = code;
    result.message = message;
    return result;
}

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

bool ends_with_chartbook(const std::string& name) {
    if (name.size() < 4) return false;
    std::string ending = name.substr(name.size() - 4);
    std::transform(ending.begin(), ending.end(), ending.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ending == ".cht";
}

bool is_regular_non_reparse(HANDLE file) {
    FILE_ATTRIBUTE_TAG_INFO tag = {};
    FILE_STANDARD_INFO standard = {};
    return GetFileInformationByHandleEx(file, FileAttributeTagInfo, &tag, sizeof(tag)) != 0 &&
           GetFileInformationByHandleEx(file, FileStandardInfo, &standard, sizeof(standard)) != 0 &&
           (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 && standard.Directory == FALSE;
}

bool read_small_regular_file(const std::wstring& path, std::string& output) {
    constexpr LONGLONG kMaximumLocalStateBytes = 1024ll * 1024ll;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {};
    const bool safe = is_regular_non_reparse(file) && GetFileSizeEx(file, &size) != 0 &&
                      size.QuadPart >= 0 && size.QuadPart <= kMaximumLocalStateBytes;
    if (!safe) {
        CloseHandle(file);
        return false;
    }
    output.assign(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const BOOL ok = output.empty() || ReadFile(file, &output[0], static_cast<DWORD>(output.size()), &read, nullptr);
    CloseHandle(file);
    if (ok == FALSE || read != output.size()) return false;
    return true;
}

bool write_atomic_replace(const std::wstring& path, const std::string& text) {
    std::wostringstream temporary_name;
    temporary_name << path << L"." << GetCurrentProcessId() << L"." << GetTickCount64() << L".tmp";
    const std::wstring temporary = temporary_name.str();
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL write_ok = text.empty() || WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    const BOOL flush_ok = FlushFileBuffers(file);
    CloseHandle(file);
    if (write_ok == FALSE || flush_ok == FALSE || written != text.size()) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

std::string compact_item(const std::vector<std::string>& values) {
    std::string output;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) output.push_back(',');
        output += percent_encode(values[index]);
    }
    return output;
}

void append_item(std::string& collection, const std::vector<std::string>& values) {
    if (!collection.empty()) collection.push_back(';');
    collection += compact_item(values);
}

std::string integer_text(int value) { return std::to_string(value); }

std::string bool_text(bool value) { return value ? "true" : "false"; }

std::string clock_text(const SCDateTime& time) {
    int seconds = time.GetTimeInSeconds();
    if (seconds < 0) seconds = 0;
    std::ostringstream output;
    output << std::setw(2) << std::setfill('0') << seconds / 3600 << ':'
           << std::setw(2) << (seconds / 60) % 60 << ':' << std::setw(2) << seconds % 60;
    return output.str();
}

SCDateTime parse_clock(const std::string& value) {
    const int hour = std::atoi(value.substr(0, 2).c_str());
    const int minute = std::atoi(value.substr(3, 2).c_str());
    const int second = std::atoi(value.substr(6, 2).c_str());
    return SCDateTime(hour, minute, second, 0);
}

const char* bar_type_name(IntradayBarPeriodTypeEnum type) {
    switch (type) {
        case IBPT_DAYS_MINS_SECS: return "days_mins_secs";
        case IBPT_VOLUME_PER_BAR: return "volume_per_bar";
        case IBPT_NUM_TRADES_PER_BAR: return "number_of_trades_per_bar";
        case IBPT_RANGE_IN_TICKS_STANDARD: return "range_in_ticks";
        case IBPT_REVERSAL_IN_TICKS: return "reversal_in_ticks";
        case IBPT_DELTA_VOLUME_PER_BAR: return "delta_volume_per_bar";
        case IBPT_PRICE_CHANGES_PER_BAR: return "price_changes_per_bar";
        default: return "unsupported_installed_type";
    }
}

bool parse_bar_type(const std::string& value, IntradayBarPeriodTypeEnum& type) {
    if (value == "days_mins_secs") type = IBPT_DAYS_MINS_SECS;
    else if (value == "volume_per_bar") type = IBPT_VOLUME_PER_BAR;
    else if (value == "number_of_trades_per_bar") type = IBPT_NUM_TRADES_PER_BAR;
    else if (value == "range_in_ticks") type = IBPT_RANGE_IN_TICKS_STANDARD;
    else if (value == "reversal_in_ticks") type = IBPT_REVERSAL_IN_TICKS;
    else if (value == "delta_volume_per_bar") type = IBPT_DELTA_VOLUME_PER_BAR;
    else if (value == "price_changes_per_bar") type = IBPT_PRICE_CHANGES_PER_BAR;
    else return false;
    return true;
}

bool parse_period_value(const std::string& value, int& output) {
    char* end = nullptr;
    const double number = std::strtod(value.c_str(), &end);
    if (end == nullptr || *end != '\0' || !std::isfinite(number) || number <= 0.0 ||
        number > std::numeric_limits<int>::max() || std::floor(number) != number) return false;
    output = static_cast<int>(number);
    return true;
}

bool parse_color(const std::string& value, uint32_t& color) {
    std::string digits = value;
    if (!digits.empty() && digits[0] == '#') digits.erase(0, 1);
    if (digits.size() != 6) return false;
    char* end = nullptr;
    const unsigned long rgb = std::strtoul(digits.c_str(), &end, 16);
    if (end == nullptr || *end != '\0' || rgb > 0xfffffful) return false;
    color = RGB_COLOR((rgb >> 16u) & 0xffu, (rgb >> 8u) & 0xffu, rgb & 0xffu);
    return true;
}

bool contains_study(SCStudyInterfaceRef sc, int chart_number, int study_id) {
    const int count = sc.GetStudyCount(chart_number);
    for (int index = 0; index < count; ++index)
        if (sc.GetStudyIDByIndex(chart_number, index) == study_id) return true;
    return false;
}

}  // namespace

ACSILSierraAdapter::ACSILSierraAdapter(SCStudyInterfaceRef sc,
                                       std::wstring state_directory,
                                       std::wstring audit_path,
                                       bool configuration_enabled)
    : sc_(sc),
      state_directory_(std::move(state_directory)),
      audit_path_(std::move(audit_path)),
      configuration_enabled_(configuration_enabled) {
    drawing_registry_path_ = join_path(state_directory_,
        utf8_to_wide(percent_encode(sc_text(sc_.ChartbookName()))) + L".drawings");
    load_drawings();
}

Result ACSILSierraAdapter::execute(const ValidatedCommand& command) {
    if (command.tool == "sierra_status") return status();
    if (command.tool == "sierra_chartbooks") return chartbooks(command);
    if (command.tool == "sierra_charts") return charts(command);
    if (command.tool == "sierra_studies") return studies(command);
    if (command.tool == "sierra_drawings") return drawings(command);
    if (command.tool == "sierra_data") return data(command);
    if (command.tool == "sierra_diagnostics") return diagnostics(command);
    return failure("forbidden_action", "tool is not implemented");
}

bool ACSILSierraAdapter::chart_exists(int chart_number) const {
    return chart_number > 0 && sc_.IsChartNumberExist(chart_number, sc_.ChartbookName()) != 0;
}

Result ACSILSierraAdapter::status() const {
    Result result = success("ok", "bridge status");
    result.data["bridge_version"] = "1.0.0";
    result.data["chartbook"] = sc_text(sc_.ChartbookName());
    result.data["calling_chart_number"] = integer_text(sc_.ChartNumber);
    result.data["configuration_enabled"] = bool_text(configuration_enabled_);
    result.data["data_folder"] = sc_text(sc_.DataFilesFolder());
    result.data["historical_download"] = bool_text(sc_.DownloadingHistoricalData != 0 ||
        (sc_.ChartIsDownloadingHistoricalData != nullptr && sc_.ChartIsDownloadingHistoricalData(sc_.ChartNumber) != 0));
    const bool downloading = result.data["historical_download"] == "true";
    result.data["health"] = "healthy";
    result.data["heartbeat"] = "alive";
    result.data["ready_for_mutation"] = downloading ? "false" : "true";
    result.data["busy"] = downloading ? "true" : "false";
    result.data["status"] = downloading ? "busy" : "ready";
    result.busy = downloading;
    result.status = downloading ? "busy" : "complete";
    return result;
}

Result ACSILSierraAdapter::chartbooks(const ValidatedCommand& command) {
    const std::string current = sc_text(sc_.ChartbookName());
    const std::wstring data_directory = utf8_to_wide(sc_text(sc_.DataFilesFolder()));
    if (command.operation == "read" || command.operation == "list" || command.operation == "inspect") {
        Result result = success("ok", "chartbooks listed");
        std::string entries;
        WIN32_FIND_DATAW found = {};
        HANDLE search = FindFirstFileW(join_path(data_directory, L"*.cht").c_str(), &found);
        if (search != INVALID_HANDLE_VALUE) {
            do {
                if ((found.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) continue;
                const std::string name = wide_to_utf8(found.cFileName);
                if (!is_safe_chartbook_name(name) || !ends_with_chartbook(name)) continue;
                append_item(entries, {name, bool_text(name == current)});
            } while (FindNextFileW(search, &found) != 0);
            FindClose(search);
        }
        result.data["chartbook"] = current;
        result.data["chartbooks"] = entries;
        return result;
    }

    if (!ends_with_chartbook(command.target_chartbook) &&
        (command.operation == "open" || command.operation == "save"))
        return failure("invalid_chartbook", "chartbook must have .cht extension");

    if (command.operation == "open") {
        const std::wstring path = join_path(data_directory, utf8_to_wide(command.target_chartbook));
        HANDLE source = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (source == INVALID_HANDLE_VALUE || !is_regular_non_reparse(source)) {
            if (source != INVALID_HANDLE_VALUE) CloseHandle(source);
            return failure("chartbook_not_found", "target chartbook is not a safe regular file");
        }
        CloseHandle(source);
        sc_.OpenChartbook(SCString(command.target_chartbook.c_str()));
        const bool verified = sc_text(sc_.ChartbookName()) == command.target_chartbook;
        Result result = success(verified ? "opened" : "queued", verified ? "chartbook opened" : "chartbook open requested", verified);
        result.data["chartbook"] = command.target_chartbook;
        if (!verified) result.warnings.push_back("open was not synchronously observable");
        return result;
    }
    if (command.operation == "save") {
        if (command.target_chartbook != current) return failure("wrong_chartbook", "only current chartbook can be saved");
        const bool saved = sc_.SaveChartbook() != 0;
        Result result = saved ? success("saved", "current chartbook saved", true)
                              : failure("save_failed", "Sierra Chart did not confirm save");
        result.data["chartbook"] = current;
        return result;
    }

    const std::string source_name = command.fields.at("source_chartbook");
    const std::string destination_name = command.fields.at("destination_chartbook");
    if (!ends_with_chartbook(source_name) || !ends_with_chartbook(destination_name))
        return failure("invalid_chartbook", "source and destination must have .cht extension");
    if (source_name == current && sc_.SaveChartbook() == 0)
        return failure("save_failed", "current source chartbook could not be saved");
    const std::wstring source_path = join_path(data_directory, utf8_to_wide(source_name));
    const std::wstring destination_path = join_path(data_directory, utf8_to_wide(destination_name));
    HANDLE source = CreateFileW(source_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (source == INVALID_HANDLE_VALUE || !is_regular_non_reparse(source)) {
        if (source != INVALID_HANDLE_VALUE) CloseHandle(source);
        return failure("chartbook_not_found", "source chartbook is not a safe regular file");
    }
    CloseHandle(source);
    if (CopyFileW(source_path.c_str(), destination_path.c_str(), TRUE) == FALSE)
        return failure(GetLastError() == ERROR_FILE_EXISTS ? "destination_exists" : "copy_failed",
                       "chartbook copy failed without overwrite");
    Result result = success(command.operation == "save_as" ? "saved_as" : "duplicated",
                            "chartbook copied without overwrite", true);
    result.data["source_chartbook"] = source_name;
    result.data["destination_chartbook"] = destination_name;
    if (command.operation == "save_as") {
        sc_.OpenChartbook(SCString(destination_name.c_str()));
        const bool opened = sc_text(sc_.ChartbookName()) == destination_name;
        result.data["open_status"] = opened ? "opened" : "queued";
        if (!opened) result.warnings.push_back("destination open was not synchronously observable");
    }
    return result;
}

Result ACSILSierraAdapter::charts(const ValidatedCommand& command) {
    const std::string current = sc_text(sc_.ChartbookName());
    if (!command.target_chartbook.empty() && command.target_chartbook != current)
        return failure("wrong_chartbook", "only charts in the handling chartbook are accessible");

    if (command.operation == "read" || command.operation == "list" || command.operation == "inspect") {
        Result result = success("ok", "charts inspected");
        std::string entries;
        const int highest = sc_.GetHighestChartNumberUsedInChartBook();
        const int only = command.target_chart_number;
        for (int number = 1; number <= highest; ++number) {
            if (only > 0 && number != only) continue;
            if (!chart_exists(number)) continue;
            n_ACSIL::s_ChartSessionTimes sessions;
            n_ACSIL::s_BarPeriod period;
            const bool have_sessions = sc_.GetSessionTimesFromChart(number, sessions) != 0;
            sc_.GetBarPeriodParametersForChart(number, period);
            append_item(entries, {integer_text(number), sc_text(sc_.GetChartName(number)),
                sc_text(sc_.GetChartSymbol(number)),
                bool_text(sc_.ChartIsDownloadingHistoricalData(number) != 0),
                bar_type_name(period.IntradayChartBarPeriodType),
                integer_text(period.IntradayChartBarPeriodParameter1),
                have_sessions ? clock_text(sessions.StartTime) : std::string(),
                have_sessions ? clock_text(sessions.EndTime) : std::string(),
                have_sessions ? bool_text(sessions.UseEveningSessionTimes != 0) : std::string()});
        }
        result.data["chartbook"] = current;
        result.data["charts"] = entries;
        result.data["charts_encoding"] = "number,name,symbol,downloading,bar_type,bar_value,session_start,session_end,use_evening;...";
        return result;
    }

    const int number = command.target_chart_number;
    if (command.operation == "close") {
        if (number == sc_.ChartNumber)
            return failure("calling_chart_close_refused", "the calling chart cannot be closed");
        if (!chart_exists(number)) {
            Result result = success("already_closed", "chart is already closed", false);
            result.data["chartbook"] = current;
            result.data["chart_number"] = integer_text(number);
            return result;
        }
        sc_.CloseChart(number);
        Result result = success("close_queued", "chart close queued", false);
        result.data["status"] = "queued";
        result.data["chartbook"] = current;
        result.data["chart_number"] = integer_text(number);
        return result;
    }
    if (!chart_exists(number)) return failure("chart_not_found", "target chart does not exist");
    bool has_settings = false;
    for (const char* key : {"session_start", "session_end", "evening_session_start",
                            "evening_session_end", "use_evening_session", "bar_period_type",
                            "bar_period_value", "days_to_load"})
        if (command.fields.count(key)) has_settings = true;
    if (command.fields.count("symbol") && has_settings)
        return failure("unsupported_field", "symbol cannot be atomically combined with bar/session changes");

    if (command.fields.count("symbol")) {
        const std::string desired = command.fields.at("symbol");
        const std::string before = sc_text(sc_.GetChartSymbol(number));
        if (before == desired) return success("unchanged", "symbol already matches", false);
        if (sc_.SetChartSymbol(number, SCString(desired.c_str()), SCString("")) == 0)
            return failure("update_failed", "SetChartSymbol failed");
        if (sc_text(sc_.GetChartSymbol(number)) != desired)
            return failure("verification_failed", "symbol readback did not match");
        sc_.RecalculateChart(number);
        Result result = success("updated", "symbol updated and verified", true);
        result.data["chartbook"] = current;
        result.data["chart_number"] = integer_text(number);
        result.data["symbol"] = desired;
        return result;
    }

    if (number != sc_.ChartNumber)
        return failure("unsupported_field", "installed API cannot preserve days-to-load for a non-calling chart");
    n_ACSIL::s_ChartSessionTimes sessions;
    if (sc_.GetSessionTimesFromChart(number, sessions) == 0)
        return failure("unsupported_field", "installed API could not read session settings");
    n_ACSIL::s_BarPeriod period;
    sc_.GetBarPeriodParametersForChart(number, period);
    if (period.IntradayChartBarPeriodType == IBPT_ACSIL_CUSTOM)
        return failure("unsupported_field", "installed API cannot safely preserve custom chart bar configuration");

    s_ACSOpenChartParameters parameters;
    parameters.PriorChartNumber = number;
    parameters.ChartDataType = period.ChartDataType;
    parameters.Symbol = sc_.GetChartSymbol(number);
    parameters.IntradayBarPeriodType = period.IntradayChartBarPeriodType;
    parameters.IntradayBarPeriodLength = period.IntradayChartBarPeriodParameter1;
    parameters.IntradayBarPeriodParm2 = period.IntradayChartBarPeriodParameter2;
    parameters.IntradayBarPeriodParm3 = period.IntradayChartBarPeriodParameter3;
    parameters.IntradayBarPeriodParm4 = period.IntradayChartBarPeriodParameter4;
    parameters.HistoricalChartBarPeriod = period.HistoricalChartBarPeriodType;
    parameters.HistoricalChartBarPeriodLengthInDays = period.HistoricalChartDaysPerBar;
    parameters.SessionStartTime = sessions.StartTime;
    parameters.SessionEndTime = sessions.EndTime;
    parameters.EveningSessionStartTime = sessions.EveningStartTime;
    parameters.EveningSessionEndTime = sessions.EveningEndTime;
    parameters.UseEveningSession = sessions.UseEveningSessionTimes;
    parameters.LoadWeekendData = sessions.LoadWeekendDataSetting;
    parameters.DaysToLoad = sc_.DaysToLoadInChart;
    parameters.UpdatePriorChartNumberParametersToMatch = 1;
    parameters.UseExactMatchForDaysToLoad = command.fields.count("days_to_load") ? 1 : 0;

    if (command.fields.count("bar_period_type") &&
        !parse_bar_type(command.fields.at("bar_period_type"), parameters.IntradayBarPeriodType))
        return failure("unsupported_field", "bar_period_type is not safely mapped by the installed SDK");
    if (command.fields.count("bar_period_value") &&
        !parse_period_value(command.fields.at("bar_period_value"), parameters.IntradayBarPeriodLength))
        return failure("unsupported_field", "bar_period_value must map to a positive integer");
    if (command.fields.count("session_start")) parameters.SessionStartTime = parse_clock(command.fields.at("session_start"));
    if (command.fields.count("session_end")) parameters.SessionEndTime = parse_clock(command.fields.at("session_end"));
    if (command.fields.count("evening_session_start")) parameters.EveningSessionStartTime = parse_clock(command.fields.at("evening_session_start"));
    if (command.fields.count("evening_session_end")) parameters.EveningSessionEndTime = parse_clock(command.fields.at("evening_session_end"));
    if (command.fields.count("use_evening_session")) parameters.UseEveningSession = command.fields.at("use_evening_session") == "true";
    if (command.fields.count("days_to_load")) parameters.DaysToLoad = std::atoi(command.fields.at("days_to_load").c_str());

    const int updated_number = sc_.OpenChartOrGetChartReference(parameters);
    if (updated_number != number) return failure("update_failed", "chart parameter update was not confirmed");
    n_ACSIL::s_ChartSessionTimes verified_sessions;
    n_ACSIL::s_BarPeriod verified_period;
    if (sc_.GetSessionTimesFromChart(number, verified_sessions) == 0)
        return failure("verification_failed", "session readback failed");
    sc_.GetBarPeriodParametersForChart(number, verified_period);
    bool verified = true;
    if (command.fields.count("bar_period_type")) verified = verified && verified_period.IntradayChartBarPeriodType == parameters.IntradayBarPeriodType;
    if (command.fields.count("bar_period_value")) verified = verified && verified_period.IntradayChartBarPeriodParameter1 == parameters.IntradayBarPeriodLength;
    if (command.fields.count("session_start")) verified = verified && clock_text(verified_sessions.StartTime) == command.fields.at("session_start");
    if (command.fields.count("session_end")) verified = verified && clock_text(verified_sessions.EndTime) == command.fields.at("session_end");
    if (command.fields.count("evening_session_start")) verified = verified && clock_text(verified_sessions.EveningStartTime) == command.fields.at("evening_session_start");
    if (command.fields.count("evening_session_end")) verified = verified && clock_text(verified_sessions.EveningEndTime) == command.fields.at("evening_session_end");
    if (command.fields.count("use_evening_session")) verified = verified &&
        (verified_sessions.UseEveningSessionTimes != 0) == (command.fields.at("use_evening_session") == "true");
    if (command.fields.count("days_to_load")) verified = verified && sc_.DaysToLoadInChart == parameters.DaysToLoad;
    if (!verified) {
        Result result = failure("verification_failed", "chart settings readback did not match");
        if (command.fields.count("days_to_load")) {
            result.data["requested_days_to_load"] = integer_text(parameters.DaysToLoad);
            result.data["observed_days_to_load"] = integer_text(sc_.DaysToLoadInChart);
        }
        return result;
    }
    sc_.RecalculateChart(number);
    Result result = success("updated", "chart settings updated and verified", true);
    result.data["chartbook"] = current;
    result.data["chart_number"] = integer_text(number);
    return result;
}

Result ACSILSierraAdapter::studies(const ValidatedCommand& command) {
    if (!command.target_chartbook.empty() && command.target_chartbook != sc_text(sc_.ChartbookName()))
        return failure("wrong_chartbook", "only studies in the handling chartbook are accessible");
    const int number = command.target_chart_number > 0 ? command.target_chart_number : sc_.ChartNumber;
    if (!chart_exists(number)) return failure("chart_not_found", "target chart does not exist");
    if (command.operation == "read" || command.operation == "list" || command.operation == "inspect") {
        Result result = success("ok", "studies inspected");
        std::string entries;
        const int count = sc_.GetStudyCount(number);
        for (int index = 0; index < count; ++index) {
            const int id = sc_.GetStudyIDByIndex(number, index);
            SCString short_name;
            sc_.GetChartStudyShortName(number, id, short_name);
            append_item(entries, {integer_text(id), sc_text(sc_.GetStudyNameFromChart(number, id)), sc_text(short_name)});
        }
        result.data["chartbook"] = sc_text(sc_.ChartbookName());
        result.data["chart_number"] = integer_text(number);
        result.data["studies"] = entries;
        result.data["studies_encoding"] = "study_id,name,short_name;...";
        if (command.operation == "inspect") {
            std::string inputs;
            for (int study_index = 0; study_index < count; ++study_index) {
                const int id = sc_.GetStudyIDByIndex(number, study_index);
                for (int input_index = 0; input_index < 64; ++input_index) {
                    SCString input_name;
                    if (sc_.GetStudyInputName(number, id, input_index, input_name) == 0) break;
                    append_item(inputs, {integer_text(id), integer_text(input_index), sc_text(input_name),
                                         integer_text(sc_.GetChartStudyInputType(number, id, input_index))});
                }
            }
            result.data["study_inputs"] = inputs;
            result.data["study_inputs_encoding"] = "study_id,input_index,name,installed_type;...";
        }
        return result;
    }
    if (command.operation == "add") {
        n_ACSIL::s_AddStudy study;
        study.ChartNumber = number;
        study.CustomStudyFileAndFunctionName = command.fields.at("function_name").c_str();
        study.ShortName = command.fields.at("short_name").c_str();
        if (sc_.AddStudyToChart(study) == 0) return failure("add_failed", "study add request was rejected");
        Result result = success("queued", "study add queued by Sierra Chart", true);
        result.data["chartbook"] = sc_text(sc_.ChartbookName());
        result.data["chart_number"] = integer_text(number);
        result.data["short_name"] = command.fields.at("short_name");
        result.data["status"] = "queued";
        return result;
    }
    const int study_id = std::atoi(command.fields.at("study_id").c_str());
    if (!contains_study(sc_, number, study_id)) return failure("study_not_found", "study ID does not exist");
    if (command.operation == "remove") {
        sc_.RemoveStudyFromChart(number, study_id);
        if (contains_study(sc_, number, study_id)) return failure("verification_failed", "study still exists after removal");
        Result result = success("removed", "study removed and verified", true);
        result.data["study_id"] = integer_text(study_id);
        return result;
    }

    const int input_index = std::atoi(command.fields.at("input_index").c_str());
    const std::string type = command.fields.at("value_type");
    const std::string value = command.fields.at("value");
    const int installed_type = sc_.GetChartStudyInputType(number, study_id, input_index);
    bool setter_ok = false;
    bool readback_ok = false;
    if (type == "int" && installed_type == INT_VALUE) {
        const int desired = std::atoi(value.c_str());
        setter_ok = sc_.SetChartStudyInputInt(number, study_id, input_index, desired) != 0;
        int actual = 0;
        readback_ok = setter_ok && sc_.GetChartStudyInputInt(number, study_id, input_index, actual) != 0 && actual == desired;
    } else if (type == "float" && installed_type == FLOAT_VALUE) {
        const double desired = std::strtod(value.c_str(), nullptr);
        setter_ok = sc_.SetChartStudyInputFloat(number, study_id, input_index, desired) != 0;
        double actual = 0;
        readback_ok = setter_ok && sc_.GetChartStudyInputFloat(number, study_id, input_index, actual) != 0 &&
                      std::fabs(actual - desired) <= std::max(1.0, std::fabs(desired)) * 1e-6;
    } else if (type == "string" && installed_type == STRING_VALUE) {
        setter_ok = sc_.SetChartStudyInputString(number, study_id, input_index, SCString(value.c_str())) != 0;
        SCString actual;
        readback_ok = setter_ok && sc_.GetChartStudyInputString(number, study_id, input_index, actual) != 0 && sc_text(actual) == value;
    } else if (type == "yes_no" && installed_type == YESNO_VALUE) {
        const int desired = value == "true" ? 1 : 0;
        setter_ok = sc_.SetChartStudyInputInt(number, study_id, input_index, desired) != 0;
        int actual = 0;
        readback_ok = setter_ok && sc_.GetChartStudyInputInt(number, study_id, input_index, actual) != 0 && actual == desired;
    } else if (type == "study_id" && installed_type == STUDYID_VALUE) {
        const int desired = std::atoi(value.c_str());
        setter_ok = sc_.SetChartStudyInputInt(number, study_id, input_index, desired) != 0;
        int actual = 0;
        readback_ok = setter_ok && sc_.GetChartStudyInputInt(number, study_id, input_index, actual) != 0 && actual == desired;
    } else {
        return failure("unsupported_field", "requested value_type does not match installed study input type");
    }
    if (!setter_ok) return failure("update_failed", "study input setter failed");
    if (!readback_ok) return failure("verification_failed", "study input readback did not match");
    sc_.RecalculateChart(number);
    Result result = success("updated", "study input updated and verified", true);
    result.data["study_id"] = integer_text(study_id);
    result.data["input_index"] = integer_text(input_index);
    result.data["value"] = value;
    return result;
}

Result ACSILSierraAdapter::drawings(const ValidatedCommand& command) {
    if (!command.target_chartbook.empty() && command.target_chartbook != sc_text(sc_.ChartbookName()))
        return failure("wrong_chartbook", "only drawings in the handling chartbook are accessible");
    const int number = command.target_chart_number > 0 ? command.target_chart_number : sc_.ChartNumber;
    if (!chart_exists(number)) return failure("chart_not_found", "target chart does not exist");
    if (command.operation == "read" || command.operation == "list" || command.operation == "inspect") {
        Result result = success("ok", "owned drawings inspected");
        std::string entries;
        for (const auto& item : drawing_registry_) {
            if (item.second.chart_number != number) continue;
            s_UseTool drawing;
            if (sc_.GetACSDrawingByLineNumber(number, item.second.line_number, drawing) == 0 ||
                drawing.AssociatedStudyID != sc_.StudyGraphInstanceID) continue;
            append_item(entries, {item.first, integer_text(item.second.line_number),
                                  std::to_string(drawing.BeginValue), sc_text(drawing.Text)});
        }
        result.data["chartbook"] = sc_text(sc_.ChartbookName());
        result.data["chart_number"] = integer_text(number);
        result.data["drawings"] = entries;
        result.data["drawings_encoding"] = "uuid,line_number,price,text;...";
        return result;
    }

    const std::string id = command.fields.at("drawing_id");
    if (command.operation == "add") {
        if (drawing_registry_.count(id)) return failure("drawing_exists", "drawing UUID is already registered");
        uint32_t color = RGB_COLOR(255, 215, 0);
        if (command.fields.count("color") && !parse_color(command.fields.at("color"), color))
            return failure("unsupported_field", "color must be #RRGGBB or RRGGBB");
        s_UseTool drawing;
        drawing.ChartNumber = number;
        drawing.DrawingType = DRAWING_HORIZONTALLINE;
        drawing.AddMethod = UTAM_ADD_ALWAYS;
        drawing.BeginValue = static_cast<float>(std::strtod(command.fields.at("price").c_str(), nullptr));
        drawing.Color = color;
        drawing.AssociatedStudyID = sc_.StudyGraphInstanceID;
        drawing.AddAsUserDrawnDrawing = 0;
        drawing.AllowSaveToChartbook = 1;
        if (command.fields.count("text")) drawing.Text = command.fields.at("text").c_str();
        if (sc_.UseTool(drawing) <= 0 || drawing.LineNumber <= 0)
            return failure("add_failed", "UseTool did not assign a line number");
        s_UseTool verified;
        if (sc_.GetACSDrawingByLineNumber(number, drawing.LineNumber, verified) == 0 ||
            verified.AssociatedStudyID != sc_.StudyGraphInstanceID ||
            verified.DrawingType != DRAWING_HORIZONTALLINE) {
            sc_.DeleteACSChartDrawing(number, TOOL_DELETE_CHARTDRAWING, drawing.LineNumber);
            return failure("verification_failed", "owned drawing readback failed");
        }
        drawing_registry_[id] = {number, drawing.LineNumber};
        if (!save_drawings()) {
            drawing_registry_.erase(id);
            sc_.DeleteACSChartDrawing(number, TOOL_DELETE_CHARTDRAWING, drawing.LineNumber);
            return failure("state_write_failed", "drawing registry could not be persisted");
        }
        Result result = success("added", "owned drawing added and verified", true);
        result.data["drawing_id"] = id;
        result.data["line_number"] = integer_text(drawing.LineNumber);
        return result;
    }

    const auto registered = drawing_registry_.find(id);
    if (registered == drawing_registry_.end() || registered->second.chart_number != number)
        return failure("drawing_not_found", "drawing UUID is not registered for target chart");
    s_UseTool drawing;
    if (sc_.GetACSDrawingByLineNumber(number, registered->second.line_number, drawing) == 0 ||
        drawing.AssociatedStudyID != sc_.StudyGraphInstanceID)
        return failure("drawing_not_found", "registered drawing is missing or not owned by this study");
    if (command.operation == "remove") {
        sc_.DeleteACSChartDrawing(number, TOOL_DELETE_CHARTDRAWING, registered->second.line_number);
        s_UseTool after;
        if (sc_.GetACSDrawingByLineNumber(number, registered->second.line_number, after) != 0)
            return failure("verification_failed", "drawing still exists after removal");
        drawing_registry_.erase(registered);
        if (!save_drawings()) return failure("state_write_failed", "drawing removed but registry persistence failed");
        Result result = success("removed", "owned drawing removed and verified", true);
        result.data["drawing_id"] = id;
        return result;
    }

    uint32_t color = drawing.Color;
    if (command.fields.count("color") && !parse_color(command.fields.at("color"), color))
        return failure("unsupported_field", "color must be #RRGGBB or RRGGBB");
    drawing.AddMethod = UTAM_ADD_OR_ADJUST;
    drawing.ChartNumber = number;
    drawing.LineNumber = registered->second.line_number;
    drawing.AssociatedStudyID = sc_.StudyGraphInstanceID;
    drawing.AddAsUserDrawnDrawing = 0;
    drawing.AllowSaveToChartbook = 1;
    if (command.fields.count("price")) drawing.BeginValue = static_cast<float>(std::strtod(command.fields.at("price").c_str(), nullptr));
    if (command.fields.count("text")) drawing.Text = command.fields.at("text").c_str();
    if (command.fields.count("color")) drawing.Color = color;
    if (sc_.UseTool(drawing) <= 0) return failure("update_failed", "UseTool update failed");
    s_UseTool verified;
    if (sc_.GetACSDrawingByLineNumber(number, registered->second.line_number, verified) == 0 ||
        verified.AssociatedStudyID != sc_.StudyGraphInstanceID)
        return failure("verification_failed", "drawing readback failed");
    bool matches = true;
    if (command.fields.count("price")) matches = matches && std::fabs(verified.BeginValue - drawing.BeginValue) < 1e-5f;
    if (command.fields.count("text")) matches = matches && sc_text(verified.Text) == command.fields.at("text");
    if (command.fields.count("color")) matches = matches && verified.Color == color;
    if (!matches) return failure("verification_failed", "drawing readback did not match");
    Result result = success("updated", "owned drawing updated and verified", true);
    result.data["drawing_id"] = id;
    return result;
}

Result ACSILSierraAdapter::data(const ValidatedCommand& command) {
    if (!command.target_chartbook.empty() && command.target_chartbook != sc_text(sc_.ChartbookName()))
        return failure("wrong_chartbook", "only data in the handling chartbook is accessible");
    const int number = command.target_chart_number > 0 ? command.target_chart_number : sc_.ChartNumber;
    if (!chart_exists(number)) return failure("chart_not_found", "target chart does not exist");
    if (command.operation == "read" || command.operation == "list" ||
        command.operation == "inspect" || command.operation == "status") {
        Result result = success("ok", "chart data status");
        result.data["chartbook"] = sc_text(sc_.ChartbookName());
        result.data["chart_number"] = integer_text(number);
        result.data["symbol"] = sc_text(sc_.GetChartSymbol(number));
        result.data["historical_download"] = bool_text(sc_.ChartIsDownloadingHistoricalData(number) != 0);
        result.data["calling_chart_array_size"] = number == sc_.ChartNumber ? integer_text(sc_.ArraySize) : "unavailable";
        return result;
    }
    if (command.operation == "recalculate") {
        if (sc_.RecalculateChart(number) == 0) return failure("recalculate_failed", "recalculate was not confirmed");
        Result result = success("recalculated", "chart recalculation requested", true);
        result.data["chart_number"] = integer_text(number);
        return result;
    }
    if (number != sc_.ChartNumber)
        return failure("unsupported", "reload is supported only for the calling chart");
    sc_.FlagToReloadChartData = 1;
    if (sc_.FlagToReloadChartData != 1) return failure("verification_failed", "reload flag readback failed");
    Result result = success("reload_requested", "calling chart reload flag set", true);
    result.data["chart_number"] = integer_text(number);
    return result;
}

Result ACSILSierraAdapter::diagnostics(const ValidatedCommand& command) const {
    Result result = success("ok", command.operation == "capabilities" ? "bridge capabilities" : "bridge audit status");
    result.data["chartbook"] = sc_text(sc_.ChartbookName());
    if (command.operation == "capabilities") {
        result.data["supported_actions"] =
            "sierra_status.read|sierra_status.list|sierra_status.inspect|sierra_chartbooks.read|sierra_chartbooks.list|sierra_chartbooks.inspect|sierra_chartbooks.open|sierra_chartbooks.save|sierra_chartbooks.save_as|sierra_chartbooks.duplicate|sierra_charts.read|sierra_charts.list|sierra_charts.inspect|sierra_charts.update|sierra_charts.close|sierra_studies.read|sierra_studies.list|sierra_studies.inspect|sierra_studies.add|sierra_studies.remove|sierra_studies.update|sierra_drawings.read|sierra_drawings.list|sierra_drawings.inspect|sierra_drawings.add|sierra_drawings.update|sierra_drawings.remove|sierra_data.read|sierra_data.list|sierra_data.inspect|sierra_data.status|sierra_data.reload|sierra_data.recalculate|sierra_diagnostics.logs|sierra_diagnostics.capabilities";
        result.data["chart_scope"] = "same_chartbook_only";
        result.data["reload_scope"] = "calling_chart_only";
        result.data["chart_update_scope"] = "symbol_any_same_chartbook;bar_session_calling_chart_only";
        result.data["instance_model"] = "one_bridge_per_chartbook_assumed";
        result.data["configuration_enabled"] = bool_text(configuration_enabled_);
        return result;
    }
    std::string log;
    std::size_t count = 0;
    std::string last;
    if (read_small_regular_file(audit_path_, log)) {
        std::istringstream input(log);
        std::string line;
        while (std::getline(input, line)) {
            ++count;
            if (!line.empty()) last = line;
        }
    }
    result.data["audit_path"] = wide_to_utf8(audit_path_);
    result.data["audit_count"] = std::to_string(count);
    result.data["audit_message"] = last;
    return result;
}

void ACSILSierraAdapter::load_drawings() {
    drawing_registry_.clear();
    std::string text;
    if (!read_small_regular_file(drawing_registry_path_, text)) return;
    std::istringstream input(text);
    std::string line;
    if (!std::getline(input, line) || line != "drawing_registry=1") return;
    while (std::getline(input, line)) {
        const std::size_t first = line.find('\t');
        const std::size_t second = first == std::string::npos ? std::string::npos : line.find('\t', first + 1);
        if (first == std::string::npos || second == std::string::npos) continue;
        const std::string id = line.substr(0, first);
        if (!is_valid_uuid(id)) continue;
        const int chart = std::atoi(line.substr(first + 1, second - first - 1).c_str());
        const int drawing = std::atoi(line.substr(second + 1).c_str());
        if (chart > 0 && drawing > 0) drawing_registry_[id] = {chart, drawing};
    }
}

bool ACSILSierraAdapter::save_drawings() const {
    std::ostringstream output;
    output << "drawing_registry=1\n";
    for (const auto& item : drawing_registry_)
        output << item.first << '\t' << item.second.chart_number << '\t' << item.second.line_number << '\n';
    return write_atomic_replace(drawing_registry_path_, output.str());
}

}  // namespace sierra_mcp
