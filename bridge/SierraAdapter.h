#pragma once

#include "BridgeCore.h"

#include "sierrachart.h"

#include <map>
#include <string>

namespace sierra_mcp {

class ACSILSierraAdapter final : public SierraAdapter {
public:
    ACSILSierraAdapter(SCStudyInterfaceRef sc,
                       std::wstring state_directory,
                       std::wstring audit_path,
                       bool configuration_enabled);
    Result execute(const ValidatedCommand& command) override;

private:
    struct DrawingRecord {
        int chart_number = 0;
        int line_number = 0;
    };

    Result status() const;
    Result chartbooks(const ValidatedCommand& command);
    Result charts(const ValidatedCommand& command);
    Result studies(const ValidatedCommand& command);
    Result drawings(const ValidatedCommand& command);
    Result data(const ValidatedCommand& command);
    Result diagnostics(const ValidatedCommand& command) const;

    bool chart_exists(int chart_number) const;
    void load_drawings();
    bool save_drawings() const;

    SCStudyInterfaceRef sc_;
    std::wstring state_directory_;
    std::wstring audit_path_;
    std::wstring drawing_registry_path_;
    bool configuration_enabled_;
    std::map<std::string, DrawingRecord> drawing_registry_;
};

}  // namespace sierra_mcp
