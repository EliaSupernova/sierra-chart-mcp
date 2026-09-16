#include "sierrachart.h"

#include <windows.h>

#include <cstring>
#include <iostream>
#include <new>

namespace {

using VersionFunction = int(__cdecl*)();
using NameFunction = const char*(__cdecl*)();
using StudyFunction = void(__cdecl*)(SCStudyInterfaceRef);

template <typename Function>
Function load(HMODULE module, const char* name) {
    const FARPROC address = GetProcAddress(module, name);
    Function function = nullptr;
    static_assert(sizeof(function) == sizeof(address), "Windows function pointer size mismatch");
    std::memcpy(&function, &address, sizeof(function));
    return function;
}

}  // namespace

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "SierraMCPBridge_64.dll";
    HMODULE module = LoadLibraryA(path);
    if (module == nullptr) {
        std::cerr << "LoadLibrary failed: " << GetLastError() << '\n';
        return 1;
    }

    const VersionFunction version = load<VersionFunction>(module, "scdll_DLLVersion");
    const NameFunction name = load<NameFunction>(module, "scdll_DLLName");
    const StudyFunction defaults = load<StudyFunction>(module, "scsf_SierraMCPBridge");
    if (version == nullptr || name == nullptr || defaults == nullptr) {
        std::cerr << "required export missing\n";
        FreeLibrary(module);
        return 2;
    }
    if (version() != SC_DLL_VERSION || std::strcmp(name(), "Sierra MCP Bridge") != 0) {
        std::cerr << "DLL identity mismatch\n";
        FreeLibrary(module);
        return 3;
    }

    void* raw = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(s_sc));
    if (raw == nullptr) {
        FreeLibrary(module);
        return 4;
    }
    s_sc* sc = static_cast<s_sc*>(raw);
    new (&sc->GraphName) SCString();
    new (&sc->StudyDescription) SCString();
    new (&sc->Input) SCInput145Array();
    s_SCInput_145 inputs[2];
    sc->Input.InternalSetArray(inputs, 2);
    sc->SetDefaults = 1;

    defaults(*sc);
    const bool valid = sc->GraphName == "Sierra MCP Bridge" &&
                       sc->AutoLoop == 0 && sc->UpdateAlways == 1 &&
                       inputs[0].GetYesNo() == 1 && inputs[1].GetYesNo() == 0;
    sc->GraphName.~SCString();
    sc->StudyDescription.~SCString();
    HeapFree(GetProcessHeap(), 0, raw);
    FreeLibrary(module);

    if (!valid) {
        std::cerr << "defaults mismatch\n";
        return 5;
    }
    std::cout << "bridge_dll_smoke: PASS\n";
    return 0;
}
