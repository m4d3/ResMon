#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <powrprof.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kWindowClass[] = L"ResMonWindow";
constexpr wchar_t kOpacityWindowClass[] = L"ResMonOpacityWindow";
constexpr wchar_t kAppName[] = L"ResMon";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"ResMon";
constexpr int kDefaultOpacityPercent = 100;
constexpr int kMinOpacityPercent = 55;
constexpr int kMaxOpacityPercent = 100;
constexpr int kWidthDip = 292;
constexpr int kHeaderHeightDip = 54;
constexpr int kHeaderLogoDip = 27;
constexpr int kHeaderDragHeightDip = 36; // Top strip that drags the window.
constexpr int kRowHeightDip = 28;
constexpr UINT_PTR kSampleTimer = 1;
constexpr UINT_PTR kAnimationTimer = 2;

constexpr UINT IDM_TOGGLE_TOPMOST = 1002;
constexpr UINT IDM_ROW_CPU = 1010;
constexpr UINT IDM_ROW_GPU = 1011;
constexpr UINT IDM_ROW_RAM = 1012;
constexpr UINT IDM_ROW_TEMP = 1013;
constexpr UINT IDM_ROW_NET = 1014;
constexpr UINT IDM_ROW_DISK = 1015;
constexpr UINT IDM_INTERVAL_1000 = 1101;
constexpr UINT IDM_INTERVAL_2000 = 1102;
constexpr UINT IDM_INTERVAL_5000 = 1103;
constexpr UINT IDM_THEME_DARK = 1151;
constexpr UINT IDM_THEME_LIGHT = 1152;
constexpr UINT IDM_OPACITY = 1160;
constexpr UINT IDM_SENSOR_INSTALL = 1170;
constexpr UINT IDM_SENSOR_REFRESH = 1171;
constexpr UINT IDC_OPACITY_CLOSE = 2160;
constexpr UINT IDM_AUTOSTART = 1201;
constexpr UINT IDM_RESET_POSITION = 1202;
constexpr UINT IDM_ABOUT = 1203;
constexpr UINT IDM_EXIT = 1299;

constexpr int IDR_PAWNIO_INTEL_MSR = 301;
constexpr int IDR_PAWNIO_AMD_F17 = 302;
constexpr int IDR_PAWNIO_SETUP = 303;
constexpr int IDR_HEADER_LOGO_PNG = 401;

uint64_t FileTimeToU64(const FILETIME& ft) {
    ULARGE_INTEGER v{};
    v.LowPart = ft.dwLowDateTime;
    v.HighPart = ft.dwHighDateTime;
    return v.QuadPart;
}

std::wstring FormatPercent(double value) {
    wchar_t buf[32]{};
    swprintf_s(buf, L"%.0f%%", std::clamp(value, 0.0, 100.0));
    return buf;
}

std::wstring FormatRate(double bytesPerSecond) {
    const wchar_t* units[] = {L"B/s", L"KB/s", L"MB/s", L"GB/s"};
    double value = std::max(0.0, bytesPerSecond);
    int unit = 0;
    while (value >= 1024.0 && unit < 3) {
        value /= 1024.0;
        ++unit;
    }

    wchar_t buf[64]{};
    if (unit == 0) {
        swprintf_s(buf, L"%.0f %s", value, units[unit]);
    } else if (value >= 100.0) {
        swprintf_s(buf, L"%.0f %s", value, units[unit]);
    } else {
        swprintf_s(buf, L"%.1f %s", value, units[unit]);
    }
    return buf;
}

std::wstring FormatGiB(uint64_t bytes) {
    wchar_t buf[64]{};
    const double gib = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    swprintf_s(buf, L"%.1f GB", gib);
    return buf;
}

std::wstring GetExePath() {
    std::vector<wchar_t> buffer(32768);
    const DWORD len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (len == 0 || len >= buffer.size()) return {};
    return std::wstring(buffer.data(), len);
}

std::wstring GetConfigPath() {
    wchar_t localAppData[MAX_PATH]{};
    DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    std::filesystem::path base;
    if (len > 0 && len < MAX_PATH) {
        base = localAppData;
    } else {
        base = std::filesystem::temp_directory_path();
    }
    base /= L"ResMon";
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    base /= L"settings.ini";
    return base.wstring();
}


enum class ThemeMode {
    Dark = 0,
    Light = 1,
};

inline double LerpDouble(double a, double b, double t) {
    return a + (b - a) * t;
}

inline uint64_t LerpU64(uint64_t a, uint64_t b, double t) {
    return static_cast<uint64_t>(std::llround(LerpDouble(static_cast<double>(a), static_cast<double>(b), t)));
}
struct Settings {
    int intervalMs = 2000;
    bool showCpu = true;
    bool showGpu = true;
    bool showRam = true;
    bool showTemp = true;
    bool showNet = true;
    bool showDisk = true;
    bool topMost = true;
    ThemeMode theme = ThemeMode::Dark;
    int opacityPercent = kDefaultOpacityPercent;
    bool hasPosition = false;
    int x = 60;
    int y = 60;
};

Settings LoadSettings() {
    Settings s;
    const auto path = GetConfigPath();
    s.intervalMs = GetPrivateProfileIntW(L"monitor", L"interval_ms", 2000, path.c_str());
    if (s.intervalMs != 1000 && s.intervalMs != 2000 && s.intervalMs != 5000) s.intervalMs = 2000;
    s.showCpu = GetPrivateProfileIntW(L"rows", L"cpu", 1, path.c_str()) != 0;
    s.showGpu = GetPrivateProfileIntW(L"rows", L"gpu", 1, path.c_str()) != 0;
    s.showRam = GetPrivateProfileIntW(L"rows", L"ram", 1, path.c_str()) != 0;
    s.showTemp = GetPrivateProfileIntW(L"rows", L"temp", 1, path.c_str()) != 0;
    s.showNet = GetPrivateProfileIntW(L"rows", L"net", 1, path.c_str()) != 0;
    s.showDisk = GetPrivateProfileIntW(L"rows", L"disk", GetPrivateProfileIntW(L"monitor", L"show_disk", 1, path.c_str()), path.c_str()) != 0;
    s.topMost = GetPrivateProfileIntW(L"window", L"topmost", 1, path.c_str()) != 0;
    s.theme = GetPrivateProfileIntW(L"window", L"theme", 0, path.c_str()) == 1 ? ThemeMode::Light : ThemeMode::Dark;
    s.opacityPercent = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"window", L"opacity", kDefaultOpacityPercent, path.c_str())), kMinOpacityPercent, kMaxOpacityPercent);
    s.hasPosition = GetPrivateProfileIntW(L"window", L"has_position", 0, path.c_str()) != 0;
    s.x = GetPrivateProfileIntW(L"window", L"x", 60, path.c_str());
    s.y = GetPrivateProfileIntW(L"window", L"y", 60, path.c_str());
    return s;
}

// Taken by value: the live window position is folded in locally so that a
// failed GetWindowRect simply rewrites the position that was loaded, rather
// than dropping it.
void SaveSettings(Settings s, HWND hwnd) {
    const auto path = GetConfigPath();

    RECT r{};
    if (hwnd && GetWindowRect(hwnd, &r)) {
        s.hasPosition = true;
        s.x = r.left;
        s.y = r.top;
    }

    auto entry = [](const wchar_t* key, int value) {
        wchar_t buf[64]{};
        swprintf_s(buf, L"%s=%d", key, value);
        return std::wstring(buf);
    };

    // One write per section rather than one per key. Every
    // WritePrivateProfileStringW call re-reads and rewrites the entire file, so
    // the old per-key form rewrote settings.ini fourteen times per menu click.
    auto writeSection = [&path](const wchar_t* section, std::initializer_list<std::wstring> entries) {
        std::wstring block;
        for (const auto& e : entries) {
            block.append(e);
            block.push_back(L'\0');
        }
        block.push_back(L'\0'); // Section buffers are double-null terminated.
        WritePrivateProfileSectionW(section, block.c_str(), path.c_str());
    };

    writeSection(L"monitor", {entry(L"interval_ms", s.intervalMs)});
    writeSection(L"rows", {
        entry(L"cpu", s.showCpu ? 1 : 0),
        entry(L"gpu", s.showGpu ? 1 : 0),
        entry(L"ram", s.showRam ? 1 : 0),
        entry(L"temp", s.showTemp ? 1 : 0),
        entry(L"net", s.showNet ? 1 : 0),
        entry(L"disk", s.showDisk ? 1 : 0),
    });
    writeSection(L"window", {
        entry(L"topmost", s.topMost ? 1 : 0),
        entry(L"theme", s.theme == ThemeMode::Light ? 1 : 0),
        entry(L"opacity", s.opacityPercent),
        entry(L"has_position", s.hasPosition ? 1 : 0),
        entry(L"x", s.x),
        entry(L"y", s.y),
    });
}

bool IsAutostartEnabled() {
    HKEY key{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
    // Only the existence and type matter, so no value buffer is requested. The
    // previous 64 KB stack buffer also reported "disabled" for a long value.
    DWORD type = 0;
    const LONG result = RegQueryValueExW(key, kRunValue, nullptr, &type, nullptr, nullptr);
    RegCloseKey(key);
    return result == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ);
}

void SetAutostart(bool enabled) {
    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    if (enabled) {
        const std::wstring exe = L"\"" + GetExePath() + L"\"";
        RegSetValueExW(key, kRunValue, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(exe.c_str()),
                       static_cast<DWORD>((exe.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kRunValue);
    }
    RegCloseKey(key);
}


enum class TemperatureSource {
    None = 0,
    Acpi = 1,
    PawnIoIntel = 2,
    PawnIoAmd = 3,
};

bool LoadEmbeddedResource(int resourceId, std::vector<BYTE>& data) {
    data.clear();
    HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (!resource) return false;
    HGLOBAL loaded = LoadResource(nullptr, resource);
    if (!loaded) return false;
    const DWORD size = SizeofResource(nullptr, resource);
    const void* bytes = LockResource(loaded);
    if (!bytes || size == 0) return false;
    data.resize(size);
    std::memcpy(data.data(), bytes, size);
    return true;
}

bool WriteEmbeddedResourceToFile(int resourceId, const std::wstring& path) {
    std::vector<BYTE> data;
    if (!LoadEmbeddedResource(resourceId, data)) return false;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
    CloseHandle(file);
    return ok && written == static_cast<DWORD>(data.size());
}

bool IsProcessElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

class PawnIoModule {
public:
    ~PawnIoModule() { Close(); }
    PawnIoModule(const PawnIoModule&) = delete;
    PawnIoModule& operator=(const PawnIoModule&) = delete;
    PawnIoModule() = default;

    void Close() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

    bool OpenFromResource(int resourceId, DWORD* errorOut = nullptr) {
        Close();
        handle_ = CreateFileW(L"\\\\?\\GLOBALROOT\\Device\\PawnIO",
                              GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            if (errorOut) *errorOut = GetLastError();
            return false;
        }

        std::vector<BYTE> module;
        if (!LoadEmbeddedResource(resourceId, module)) {
            if (errorOut) *errorOut = ERROR_RESOURCE_DATA_NOT_FOUND;
            Close();
            return false;
        }

        DWORD returned = 0;
        const BOOL ok = DeviceIoControl(handle_, kIoctlLoadBinary,
                                        module.data(), static_cast<DWORD>(module.size()),
                                        nullptr, 0, &returned, nullptr);
        if (!ok) {
            if (errorOut) *errorOut = GetLastError();
            Close();
            return false;
        }
        if (errorOut) *errorOut = ERROR_SUCCESS;
        return true;
    }

    bool Execute(std::string_view functionName, const int64_t* input, size_t inputCount,
                 int64_t* output, size_t outputCount) const {
        if (handle_ == INVALID_HANDLE_VALUE || functionName.empty() || functionName.size() >= kFunctionNameLength) return false;
        std::vector<BYTE> in(kFunctionNameLength + inputCount * sizeof(int64_t), 0);
        std::memcpy(in.data(), functionName.data(), functionName.size());
        if (inputCount > 0 && input) {
            std::memcpy(in.data() + kFunctionNameLength, input, inputCount * sizeof(int64_t));
        }
        DWORD returned = 0;
        const DWORD outBytes = static_cast<DWORD>(outputCount * sizeof(int64_t));
        const BOOL ok = DeviceIoControl(handle_, kIoctlExecute,
                                        in.data(), static_cast<DWORD>(in.size()),
                                        output, outBytes, &returned, nullptr);
        return ok && returned >= outBytes;
    }

    bool IsOpen() const { return handle_ != INVALID_HANDLE_VALUE; }

private:
    static constexpr DWORD kDeviceType = 41394u << 16;
    static constexpr DWORD kIoctlLoadBinary = kDeviceType | (0x821u << 2);
    static constexpr DWORD kIoctlExecute = kDeviceType | (0x841u << 2);
    static constexpr size_t kFunctionNameLength = 32;
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

class EnhancedTemperatureReader {
public:
    EnhancedTemperatureReader() { DetectCpu(); }

    bool Initialize() {
        module_.Close();
        active_ = false;
        accessDenied_ = false;
        lastError_ = ERROR_SUCCESS;
        int resourceId = 0;
        if (vendor_ == Vendor::Intel) resourceId = IDR_PAWNIO_INTEL_MSR;
        if (vendor_ == Vendor::Amd) resourceId = IDR_PAWNIO_AMD_F17;
        if (resourceId == 0) return false;
        DWORD error = ERROR_SUCCESS;
        if (!module_.OpenFromResource(resourceId, &error)) {
            lastError_ = error;
            accessDenied_ = error == ERROR_ACCESS_DENIED;
            return false;
        }
        active_ = true;
        return true;
    }

    bool Read(double& temperatureC, TemperatureSource& source) {
        temperatureC = 0.0;
        source = TemperatureSource::None;
        if (!active_) return false;
        if (vendor_ == Vendor::Intel) return ReadIntel(temperatureC, source);
        if (vendor_ == Vendor::Amd) return ReadAmd(temperatureC, source);
        return false;
    }

    bool Active() const { return active_; }
    bool AccessDenied() const { return accessDenied_; }
    DWORD LastError() const { return lastError_; }
    bool SupportedCpu() const { return vendor_ == Vendor::Intel || vendor_ == Vendor::Amd; }

private:
    enum class Vendor { Other, Intel, Amd };
    Vendor vendor_ = Vendor::Other;
    std::string brand_;
    PawnIoModule module_;
    bool active_ = false;
    bool accessDenied_ = false;
    DWORD lastError_ = ERROR_SUCCESS;

    void DetectCpu() {
        int regs[4]{};
        __cpuid(regs, 0);
        char vendor[13]{};
        std::memcpy(vendor + 0, &regs[1], 4);
        std::memcpy(vendor + 4, &regs[3], 4);
        std::memcpy(vendor + 8, &regs[2], 4);
        if (std::strcmp(vendor, "GenuineIntel") == 0) vendor_ = Vendor::Intel;
        else if (std::strcmp(vendor, "AuthenticAMD") == 0) vendor_ = Vendor::Amd;

        __cpuid(regs, static_cast<int>(0x80000000u));
        const unsigned maxExtended = static_cast<unsigned>(regs[0]);
        if (maxExtended >= 0x80000004u) {
            std::array<int, 12> brandRegs{};
            __cpuid(brandRegs.data() + 0, static_cast<int>(0x80000002u));
            __cpuid(brandRegs.data() + 4, static_cast<int>(0x80000003u));
            __cpuid(brandRegs.data() + 8, static_cast<int>(0x80000004u));
            brand_.assign(reinterpret_cast<const char*>(brandRegs.data()), 48);
        }
    }

    bool ExecuteOne(const char* functionName, uint32_t inputValue, uint64_t& outputValue) {
        const int64_t in = static_cast<int64_t>(inputValue);
        int64_t out = 0;
        if (!module_.Execute(functionName, &in, 1, &out, 1)) return false;
        outputValue = static_cast<uint64_t>(out);
        return true;
    }

    bool ReadIntel(double& c, TemperatureSource& source) {
        constexpr uint32_t kIa32TemperatureTarget = 0x01A2;
        constexpr uint32_t kIa32PackageThermStatus = 0x01B1;
        uint64_t target = 0;
        uint64_t status = 0;
        int tjMax = 100;
        if (ExecuteOne("ioctl_read_msr", kIa32TemperatureTarget, target)) {
            const int detected = static_cast<int>((target >> 16) & 0xFFu);
            if (detected >= 70 && detected <= 125) tjMax = detected;
        }
        if (!ExecuteOne("ioctl_read_msr", kIa32PackageThermStatus, status)) return false;
        if ((status & 0x80000000ull) == 0) return false;
        const int delta = static_cast<int>((status & 0x007F0000ull) >> 16);
        const double temperature = static_cast<double>(tjMax - delta);
        if (!std::isfinite(temperature) || temperature < -20.0 || temperature > 130.0) return false;
        c = temperature;
        source = TemperatureSource::PawnIoIntel;
        return true;
    }

    bool ReadAmd(double& c, TemperatureSource& source) {
        constexpr uint32_t kThermalControlCurrentTemp = 0x00059800;
        constexpr uint32_t kRangeSelectMask = 0x00080000;
        constexpr uint32_t kTjSelectMask = 0x00030000;

        HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Global\\Access_PCI");
        if (!mutex && GetLastError() == ERROR_ACCESS_DENIED) {
            mutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, L"Global\\Access_PCI");
        }
        if (!mutex) return false;
        const DWORD wait = WaitForSingleObject(mutex, 50);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
            CloseHandle(mutex);
            return false;
        }

        uint64_t raw64 = 0;
        const bool ok = ExecuteOne("ioctl_read_smn", kThermalControlCurrentTemp, raw64);
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        if (!ok) return false;

        const uint32_t raw = static_cast<uint32_t>(raw64);
        const bool rangeOffset = (raw & kRangeSelectMask) != 0 || (raw & kTjSelectMask) == kTjSelectMask;
        double temperature = static_cast<double>((raw >> 21) * 125u) / 1000.0;
        if (rangeOffset) temperature -= 49.0;
        if (brand_.find("1600X") != std::string::npos || brand_.find("1700X") != std::string::npos || brand_.find("1800X") != std::string::npos) {
            temperature -= 20.0;
        } else if (brand_.find("Threadripper 19") != std::string::npos || brand_.find("Threadripper 29") != std::string::npos) {
            temperature -= 27.0;
        } else if (brand_.find("2700X") != std::string::npos) {
            temperature -= 10.0;
        }
        if (!std::isfinite(temperature) || temperature < -20.0 || temperature > 130.0) return false;
        c = temperature;
        source = TemperatureSource::PawnIoAmd;
        return true;
    }
};

class NvidiaTemperatureReader {
public:
    NvidiaTemperatureReader() = default;
    ~NvidiaTemperatureReader() { Shutdown(); }
    NvidiaTemperatureReader(const NvidiaTemperatureReader&) = delete;
    NvidiaTemperatureReader& operator=(const NvidiaTemperatureReader&) = delete;

    bool Read(double& hottestC) {
        hottestC = 0.0;
        if (!initialized_ && !Initialize()) return false;
        if (!getCount_ || !getHandleByIndex_ || (!getTemperatureV_ && !getTemperature_)) return false;

        unsigned int count = 0;
        if (getCount_(&count) != kNvmlSuccess || count == 0) return false;

        bool found = false;
        unsigned int hottest = 0;
        for (unsigned int i = 0; i < count; ++i) {
            NvmlDevice device = nullptr;
            if (getHandleByIndex_(i, &device) != kNvmlSuccess || !device) continue;
            unsigned int temp = 0;
            NvmlReturn tempResult = -1;
            if (getTemperatureV_) {
                NvmlTemperatureV1 value{};
                value.version = static_cast<unsigned int>(sizeof(NvmlTemperatureV1)) | (1u << 24u);
                value.sensorType = static_cast<unsigned int>(kNvmlTemperatureGpu);
                tempResult = getTemperatureV_(device, &value);
                temp = value.temperature;
            } else if (getTemperature_) {
                tempResult = getTemperature_(device, kNvmlTemperatureGpu, &temp);
            }
            if (tempResult != kNvmlSuccess) continue;
            if (temp > 0 && temp < 150) {
                hottest = std::max(hottest, temp);
                found = true;
            }
        }
        if (found) hottestC = static_cast<double>(hottest);
        return found;
    }

    bool Available() const { return initialized_; }
    void Reset() {
        Shutdown();
        attempted_ = false;
    }

private:
    using NvmlReturn = int;
    using NvmlDevice = void*;
    using NvmlInitFn = NvmlReturn (__cdecl*)();
    using NvmlShutdownFn = NvmlReturn (__cdecl*)();
    using NvmlGetCountFn = NvmlReturn (__cdecl*)(unsigned int*);
    struct NvmlTemperatureV1 {
        unsigned int version;
        unsigned int sensorType;
        unsigned int temperature;
    };
    using NvmlGetHandleByIndexFn = NvmlReturn (__cdecl*)(unsigned int, NvmlDevice*);
    using NvmlGetTemperatureFn = NvmlReturn (__cdecl*)(NvmlDevice, int, unsigned int*);
    using NvmlGetTemperatureVFn = NvmlReturn (__cdecl*)(NvmlDevice, NvmlTemperatureV1*);

    static constexpr NvmlReturn kNvmlSuccess = 0;
    static constexpr int kNvmlTemperatureGpu = 0;

    HMODULE module_ = nullptr;
    NvmlShutdownFn shutdown_ = nullptr;
    NvmlGetCountFn getCount_ = nullptr;
    NvmlGetHandleByIndexFn getHandleByIndex_ = nullptr;
    NvmlGetTemperatureFn getTemperature_ = nullptr;
    NvmlGetTemperatureVFn getTemperatureV_ = nullptr;
    bool attempted_ = false;
    bool initialized_ = false;

    HMODULE TryLoadLibrary() {
        if (HMODULE m = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) return m;

        wchar_t systemDir[MAX_PATH]{};
        if (GetSystemDirectoryW(systemDir, MAX_PATH) > 0) {
            std::filesystem::path path = systemDir;
            path /= L"nvml.dll";
            if (HMODULE m = LoadLibraryW(path.c_str())) return m;
        }

        wchar_t programFiles[MAX_PATH]{};
        DWORD len = GetEnvironmentVariableW(L"ProgramW6432", programFiles, MAX_PATH);
        if (len == 0 || len >= MAX_PATH) {
            len = GetEnvironmentVariableW(L"ProgramFiles", programFiles, MAX_PATH);
        }
        if (len > 0 && len < MAX_PATH) {
            std::filesystem::path path = programFiles;
            path /= L"NVIDIA Corporation";
            path /= L"NVSMI";
            path /= L"nvml.dll";
            if (HMODULE m = LoadLibraryW(path.c_str())) return m;
        }
        return nullptr;
    }

    template <typename T>
    static T Proc(HMODULE module, const char* primary, const char* fallback = nullptr) {
        if (!module) return nullptr;
        FARPROC p = GetProcAddress(module, primary);
        if (!p && fallback) p = GetProcAddress(module, fallback);
        return reinterpret_cast<T>(p);
    }

    bool Initialize() {
        if (attempted_) return initialized_;
        attempted_ = true;
        module_ = TryLoadLibrary();
        if (!module_) return false;

        auto init = Proc<NvmlInitFn>(module_, "nvmlInit_v2", "nvmlInit");
        shutdown_ = Proc<NvmlShutdownFn>(module_, "nvmlShutdown");
        getCount_ = Proc<NvmlGetCountFn>(module_, "nvmlDeviceGetCount_v2", "nvmlDeviceGetCount");
        getHandleByIndex_ = Proc<NvmlGetHandleByIndexFn>(module_, "nvmlDeviceGetHandleByIndex_v2", "nvmlDeviceGetHandleByIndex");
        getTemperatureV_ = Proc<NvmlGetTemperatureVFn>(module_, "nvmlDeviceGetTemperatureV");
        getTemperature_ = Proc<NvmlGetTemperatureFn>(module_, "nvmlDeviceGetTemperature");

        if (!init || !shutdown_ || !getCount_ || !getHandleByIndex_ || (!getTemperatureV_ && !getTemperature_)) {
            FreeLibrary(module_);
            module_ = nullptr;
            return false;
        }
        if (init() != kNvmlSuccess) {
            FreeLibrary(module_);
            module_ = nullptr;
            return false;
        }
        initialized_ = true;
        return true;
    }

    void Shutdown() {
        if (initialized_ && shutdown_) shutdown_();
        initialized_ = false;
        if (module_) FreeLibrary(module_);
        module_ = nullptr;
        // Clear the entry points too; they point into the module that was just
        // unloaded, and Reset() allows a later Initialize() to run again.
        shutdown_ = nullptr;
        getCount_ = nullptr;
        getHandleByIndex_ = nullptr;
        getTemperature_ = nullptr;
        getTemperatureV_ = nullptr;
    }
};

// Creates a freshly named directory under %TEMP%. Extracting the installer to a
// predictable path and then launching it elevated would let another process
// running as the same user swap the file in between and have it run as admin.
std::filesystem::path CreateUniqueTempDirectory() {
    wchar_t tempDir[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, tempDir)) return {};
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) return {};
    wchar_t name[64]{};
    swprintf_s(name, L"ResMon_%08lX%04hX%04hX", guid.Data1, guid.Data2, guid.Data3);
    const std::filesystem::path dir = std::filesystem::path(tempDir) / name;
    std::error_code ec;
    // create_directory reports false when the directory already existed, so a
    // successful call means this process created it.
    if (!std::filesystem::create_directory(dir, ec) || ec) return {};
    return dir;
}

// Waits for the installer while keeping the widget painting. Input to the owner
// window is disabled for the duration so that pumping messages cannot re-enter
// the menu and start a second installation.
void WaitForProcessKeepingUiAlive(HANDLE process, HWND owner) {
    const BOOL wasEnabled = owner ? IsWindowEnabled(owner) : FALSE;
    if (owner && wasEnabled) EnableWindow(owner, FALSE);

    bool quitSeen = false;
    int quitCode = 0;
    for (;;) {
        const DWORD wait = MsgWaitForMultipleObjects(1, &process, FALSE, INFINITE, QS_ALLINPUT);
        if (wait != WAIT_OBJECT_0 + 1) break; // Process exited, or the wait failed.
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                quitSeen = true;
                quitCode = static_cast<int>(msg.wParam);
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quitSeen) {
            // Let the installer finish, then put WM_QUIT back for the main loop.
            WaitForSingleObject(process, INFINITE);
            break;
        }
    }

    if (owner && wasEnabled && IsWindow(owner)) EnableWindow(owner, TRUE);
    if (quitSeen) PostQuitMessage(quitCode);
}

bool InstallPawnIoFromEmbeddedSetup(HWND owner, DWORD& exitCode, std::wstring& detail) {
    exitCode = ERROR_GEN_FAILURE;
    detail.clear();
    const std::filesystem::path workDir = CreateUniqueTempDirectory();
    if (workDir.empty()) {
        detail = L"Could not create a temporary folder for the PawnIO installer.";
        return false;
    }
    const std::filesystem::path installer = workDir / L"PawnIO_setup.exe";
    auto cleanup = [&workDir]() {
        std::error_code ec;
        std::filesystem::remove_all(workDir, ec);
    };
    if (!WriteEmbeddedResourceToFile(IDR_PAWNIO_SETUP, installer.wstring())) {
        cleanup();
        detail = L"The embedded PawnIO installer could not be extracted.";
        return false;
    }
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.hwnd = owner;
    sei.lpVerb = L"runas";
    sei.lpFile = installer.c_str();
    sei.lpParameters = L"-install -silent";
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        const DWORD error = GetLastError();
        cleanup();
        exitCode = error;
        detail = error == ERROR_CANCELLED ? L"Installation was cancelled." : L"Windows could not start the PawnIO installer.";
        return false;
    }
    WaitForProcessKeepingUiAlive(sei.hProcess, owner);
    GetExitCodeProcess(sei.hProcess, &exitCode);
    CloseHandle(sei.hProcess);
    cleanup();
    if (exitCode == ERROR_SUCCESS) return true;
    if (exitCode == ERROR_SUCCESS_REBOOT_REQUIRED) {
        detail = L"PawnIO installed successfully, but Windows requested a restart.";
        return true;
    }
    wchar_t message[160]{};
    swprintf_s(message, L"PawnIO setup returned error code %lu.", exitCode);
    detail = message;
    return false;
}

struct Snapshot {
    double cpu = 0.0;
    double gpu = 0.0;
    bool gpuAvailable = false;
    double ram = 0.0;
    uint64_t memUsed = 0;
    uint64_t memTotal = 0;
    double cpuMhz = 0.0;
    double netDown = 0.0;
    double netUp = 0.0;
    double diskRead = 0.0;
    double diskWrite = 0.0;
    double thermalC = 0.0;
    bool thermalAvailable = false;
    TemperatureSource thermalSource = TemperatureSource::None;
    double gpuThermalC = 0.0;
    bool gpuThermalAvailable = false;
};

struct SampleOptions {
    bool cpu = true;
    bool gpu = true;
    bool ram = true;
    bool temp = true;
    bool net = true;
    bool disk = true;
};

class MetricsSampler {
public:
    MetricsSampler() {
        PrimeCpu();
        InitPdh();
        enhancedThermal_.Initialize();
    }

    // Owns raw PDH query handles that are closed in the destructor, so copying
    // one would close them twice.
    MetricsSampler(const MetricsSampler&) = delete;
    MetricsSampler& operator=(const MetricsSampler&) = delete;

    ~MetricsSampler() {
        if (gpuQuery_) PdhCloseQuery(gpuQuery_);
        if (netQuery_) PdhCloseQuery(netQuery_);
        if (diskQuery_) PdhCloseQuery(diskQuery_);
        if (thermalQuery_) PdhCloseQuery(thermalQuery_);
    }

    // PDH derives its own rates from the interval between collections, so the
    // sampler does not need to be told how much time has passed.
    Snapshot Sample(const SampleOptions& options) {
        Snapshot out{};
        const ULONGLONG now = GetTickCount64();
        const double cpuNow = SampleCpu(); // Keep the baseline current even when the CPU row is hidden.
        if (options.cpu) {
            out.cpu = cpuNow;
            if (cachedCpuMhz_ <= 0.0 || now - lastFrequencyTick_ >= 10000) {
                cachedCpuMhz_ = SampleCpuFrequency();
                lastFrequencyTick_ = now;
            }
            out.cpuMhz = cachedCpuMhz_;
        }
        if (options.ram) SampleMemory(out);
        SamplePdh(out, options.gpu, options.net, options.disk);
        if (options.temp && (options.cpu || options.gpu)) {
            SampleTemperatures(out, now, options.cpu, options.gpu);
        }
        return out;
    }

    bool RefreshEnhancedThermal() {
        lastCpuThermalTick_ = 0;
        lastGpuThermalTick_ = 0;
        cachedThermalAvailable_ = false;
        cachedThermalSource_ = TemperatureSource::None;
        cachedGpuThermalAvailable_ = false;
        nvidiaThermal_.Reset();
        return enhancedThermal_.Initialize();
    }
    bool EnhancedThermalActive() const { return enhancedThermal_.Active(); }
    bool NvidiaThermalActive() const { return nvidiaThermal_.Available(); }
    bool EnhancedThermalAccessDenied() const { return enhancedThermal_.AccessDenied(); }
    bool EnhancedThermalSupportedCpu() const { return enhancedThermal_.SupportedCpu(); }
    DWORD EnhancedThermalLastError() const { return enhancedThermal_.LastError(); }

private:
    uint64_t lastIdle_ = 0;
    uint64_t lastKernel_ = 0;
    uint64_t lastUser_ = 0;
    double cachedCpuMhz_ = 0.0;
    ULONGLONG lastFrequencyTick_ = 0;
    PDH_HQUERY gpuQuery_ = nullptr;
    PDH_HQUERY netQuery_ = nullptr;
    PDH_HQUERY diskQuery_ = nullptr;
    PDH_HCOUNTER gpuCounter_ = nullptr;
    PDH_HCOUNTER netDownCounter_ = nullptr;
    PDH_HCOUNTER netUpCounter_ = nullptr;
    PDH_HCOUNTER diskReadCounter_ = nullptr;
    PDH_HCOUNTER diskWriteCounter_ = nullptr;
    PDH_HQUERY thermalQuery_ = nullptr;
    PDH_HCOUNTER thermalCounter_ = nullptr;
    double cachedThermalC_ = 0.0;
    bool cachedThermalAvailable_ = false;
    ULONGLONG lastCpuThermalTick_ = 0;
    ULONGLONG lastGpuThermalTick_ = 0;
    TemperatureSource cachedThermalSource_ = TemperatureSource::None;
    double cachedGpuThermalC_ = 0.0;
    bool cachedGpuThermalAvailable_ = false;
    EnhancedTemperatureReader enhancedThermal_{};
    NvidiaTemperatureReader nvidiaThermal_{};

    void PrimeCpu() {
        FILETIME idle{}, kernel{}, user{};
        if (GetSystemTimes(&idle, &kernel, &user)) {
            lastIdle_ = FileTimeToU64(idle);
            lastKernel_ = FileTimeToU64(kernel);
            lastUser_ = FileTimeToU64(user);
        }
    }

    double SampleCpu() {
        FILETIME idle{}, kernel{}, user{};
        if (!GetSystemTimes(&idle, &kernel, &user)) return 0.0;
        const uint64_t i = FileTimeToU64(idle);
        const uint64_t k = FileTimeToU64(kernel);
        const uint64_t u = FileTimeToU64(user);
        const uint64_t idleDelta = i - lastIdle_;
        const uint64_t kernelDelta = k - lastKernel_;
        const uint64_t userDelta = u - lastUser_;
        lastIdle_ = i;
        lastKernel_ = k;
        lastUser_ = u;
        const uint64_t total = kernelDelta + userDelta;
        if (total == 0) return 0.0;
        const uint64_t busy = total > idleDelta ? total - idleDelta : 0;
        return std::clamp(100.0 * static_cast<double>(busy) / static_cast<double>(total), 0.0, 100.0);
    }

    void SampleMemory(Snapshot& out) {
        MEMORYSTATUSEX m{};
        m.dwLength = sizeof(m);
        if (GlobalMemoryStatusEx(&m)) {
            out.memTotal = m.ullTotalPhys;
            out.memUsed = m.ullTotalPhys - m.ullAvailPhys;
            out.ram = m.dwMemoryLoad;
        }
    }

    double SampleCpuFrequency() {
        const DWORD count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        if (count == 0) return 0.0;
        struct ProcessorPowerInformationEntry {
            ULONG Number;
            ULONG MaxMhz;
            ULONG CurrentMhz;
            ULONG MhzLimit;
            ULONG MaxIdleState;
            ULONG CurrentIdleState;
        };
        std::vector<ProcessorPowerInformationEntry> info(count);
        const ULONG status = CallNtPowerInformation(
            ProcessorInformation,
            nullptr,
            0,
            info.data(),
            static_cast<ULONG>(info.size() * sizeof(ProcessorPowerInformationEntry)));
        if (status != 0) return 0.0;
        // The buffer is sized for every processor so it can never be too small,
        // but CallNtPowerInformation only fills entries for the calling thread's
        // processor group. Averaging over the whole buffer therefore divided by
        // too many entries on machines with more than 64 logical processors and
        // reported a fraction of the real clock. Only entries the call actually
        // populated are counted; a real processor always reports a MaxMhz.
        double sum = 0.0;
        size_t populated = 0;
        for (const auto& p : info) {
            if (p.MaxMhz == 0) continue;
            sum += p.CurrentMhz;
            ++populated;
        }
        if (populated == 0) return 0.0;
        return sum / static_cast<double>(populated);
    }

    void InitPdh() {
        if (PdhOpenQueryW(nullptr, 0, &gpuQuery_) == ERROR_SUCCESS) {
            if (PdhAddEnglishCounterW(gpuQuery_, L"\\GPU Engine(*)\\Utilization Percentage", 0, &gpuCounter_) != ERROR_SUCCESS) {
                gpuCounter_ = nullptr;
            }
            PdhCollectQueryData(gpuQuery_);
        } else {
            gpuQuery_ = nullptr;
        }

        if (PdhOpenQueryW(nullptr, 0, &netQuery_) == ERROR_SUCCESS) {
            if (PdhAddEnglishCounterW(netQuery_, L"\\Network Interface(*)\\Bytes Received/sec", 0, &netDownCounter_) != ERROR_SUCCESS) {
                netDownCounter_ = nullptr;
            }
            if (PdhAddEnglishCounterW(netQuery_, L"\\Network Interface(*)\\Bytes Sent/sec", 0, &netUpCounter_) != ERROR_SUCCESS) {
                netUpCounter_ = nullptr;
            }
            PdhCollectQueryData(netQuery_);
        } else {
            netQuery_ = nullptr;
        }

        if (PdhOpenQueryW(nullptr, 0, &diskQuery_) == ERROR_SUCCESS) {
            if (PdhAddEnglishCounterW(diskQuery_, L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec", 0, &diskReadCounter_) != ERROR_SUCCESS) {
                diskReadCounter_ = nullptr;
            }
            if (PdhAddEnglishCounterW(diskQuery_, L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec", 0, &diskWriteCounter_) != ERROR_SUCCESS) {
                diskWriteCounter_ = nullptr;
            }
            PdhCollectQueryData(diskQuery_);
        } else {
            diskQuery_ = nullptr;
        }

        // Best-effort temperature source exposed by Windows' thermal manager.
        // Not every desktop firmware publishes ACPI thermal zones.
        if (PdhOpenQueryW(nullptr, 0, &thermalQuery_) == ERROR_SUCCESS) {
            if (PdhAddEnglishCounterW(thermalQuery_, L"\\Thermal Zone Information(*)\\Temperature", 0, &thermalCounter_) != ERROR_SUCCESS) {
                thermalCounter_ = nullptr;
            }
        } else {
            thermalQuery_ = nullptr;
        }
    }

    static double ReadCounter(PDH_HCOUNTER counter) {
        if (!counter) return 0.0;
        DWORD type = 0;
        PDH_FMT_COUNTERVALUE value{};
        if (PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE, &type, &value) != ERROR_SUCCESS) return 0.0;
        if (value.CStatus != ERROR_SUCCESS) return 0.0;
        return std::max(0.0, value.doubleValue);
    }

    // Fetches a counter's instance array into `buffer` and returns the item
    // pointer plus the item count, or nullptr when the counter is unavailable.
    static PDH_FMT_COUNTERVALUE_ITEM_W* ReadCounterArray(PDH_HCOUNTER counter,
                                                        std::vector<BYTE>& buffer,
                                                        DWORD& count) {
        count = 0;
        if (!counter) return nullptr;
        DWORD bytes = 0;
        PDH_STATUS status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &count, nullptr);
        if (status != PDH_MORE_DATA || bytes == 0) return nullptr;
        buffer.assign(bytes, 0);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
        status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &count, items);
        if (status != ERROR_SUCCESS) {
            count = 0;
            return nullptr;
        }
        return items;
    }

    static double ReadCounterArraySum(PDH_HCOUNTER counter) {
        std::vector<BYTE> buffer;
        DWORD count = 0;
        const auto* items = ReadCounterArray(counter, buffer, count);
        if (!items) return 0.0;

        double total = 0.0;
        for (DWORD i = 0; i < count; ++i) {
            if (items[i].FmtValue.CStatus != ERROR_SUCCESS) continue;
            total += std::max(0.0, items[i].FmtValue.doubleValue);
        }
        return total;
    }

    static bool ContainsInsensitive(const wchar_t* text, const wchar_t* needle) {
        if (!text || !needle) return false;
        // Searches in place. The previous implementation copied and lower-cased
        // both strings on every call, and ReadGpu calls this twice for every
        // counter instance - several hundred of them on a typical machine.
        return StrStrIW(text, needle) != nullptr;
    }

    double ReadGpu(bool& available) {
        available = false;
        std::vector<BYTE> buffer;
        DWORD count = 0;
        const auto* items = ReadCounterArray(gpuCounter_, buffer, count);
        if (!items) return 0.0;

        double preferred = 0.0;
        double fallback = 0.0;
        bool sawPreferred = false;
        bool sawAny = false;
        for (DWORD i = 0; i < count; ++i) {
            if (items[i].FmtValue.CStatus != ERROR_SUCCESS) continue;
            const double v = std::max(0.0, items[i].FmtValue.doubleValue);
            fallback += v;
            sawAny = true;
            if (ContainsInsensitive(items[i].szName, L"engtype_3d") ||
                ContainsInsensitive(items[i].szName, L"engtype_compute")) {
                preferred += v;
                sawPreferred = true;
            }
        }
        available = sawAny;
        return std::clamp(sawPreferred ? preferred : fallback, 0.0, 100.0);
    }

    bool ReadThermalCelsius(double& hottestC) {
        hottestC = 0.0;
        std::vector<BYTE> buffer;
        DWORD count = 0;
        const auto* items = ReadCounterArray(thermalCounter_, buffer, count);
        if (!items) return false;

        bool found = false;
        double maxC = -273.15;
        for (DWORD i = 0; i < count; ++i) {
            if (items[i].FmtValue.CStatus != ERROR_SUCCESS) continue;
            const double kelvin = items[i].FmtValue.doubleValue;
            const double celsius = kelvin - 273.15;
            // Reject clearly bogus firmware values while allowing cold systems.
            if (!std::isfinite(celsius) || celsius < -20.0 || celsius > 150.0) continue;
            maxC = std::max(maxC, celsius);
            found = true;
        }
        if (found) hottestC = maxC;
        return found;
    }

    void SampleTemperatures(Snapshot& out, ULONGLONG now, bool sampleCpuTemperature, bool sampleGpuTemperature) {
        if (sampleCpuTemperature) {
            const ULONGLONG cpuInterval = enhancedThermal_.Active() ? 5000ull : 10000ull;
            if (lastCpuThermalTick_ == 0 || now - lastCpuThermalTick_ >= cpuInterval) {
                lastCpuThermalTick_ = now;
                double c = 0.0;
                TemperatureSource source = TemperatureSource::None;
                bool found = false;
                if (enhancedThermal_.Active()) found = enhancedThermal_.Read(c, source);
                if (!found && thermalQuery_ && thermalCounter_ && PdhCollectQueryData(thermalQuery_) == ERROR_SUCCESS) {
                    found = ReadThermalCelsius(c);
                    if (found) source = TemperatureSource::Acpi;
                }
                cachedThermalAvailable_ = found;
                if (found) cachedThermalC_ = c;
                cachedThermalSource_ = found ? source : TemperatureSource::None;
            }
            out.thermalAvailable = cachedThermalAvailable_;
            out.thermalC = cachedThermalC_;
            out.thermalSource = cachedThermalSource_;
        }

        if (sampleGpuTemperature) {
            constexpr ULONGLONG gpuInterval = 5000ull;
            if (lastGpuThermalTick_ == 0 || now - lastGpuThermalTick_ >= gpuInterval) {
                lastGpuThermalTick_ = now;
                double gpuC = 0.0;
                cachedGpuThermalAvailable_ = nvidiaThermal_.Read(gpuC);
                if (cachedGpuThermalAvailable_) cachedGpuThermalC_ = gpuC;
            }
            out.gpuThermalAvailable = cachedGpuThermalAvailable_;
            out.gpuThermalC = cachedGpuThermalC_;
        }
    }

    void SamplePdh(Snapshot& out, bool sampleGpu, bool sampleNet, bool sampleDisk) {
        if (sampleGpu && gpuQuery_ && PdhCollectQueryData(gpuQuery_) == ERROR_SUCCESS) {
            out.gpu = ReadGpu(out.gpuAvailable);
        }
        if (sampleNet && netQuery_ && PdhCollectQueryData(netQuery_) == ERROR_SUCCESS) {
            out.netDown = ReadCounterArraySum(netDownCounter_);
            out.netUp = ReadCounterArraySum(netUpCounter_);
        }
        if (sampleDisk && diskQuery_ && PdhCollectQueryData(diskQuery_) == ERROR_SUCCESS) {
            out.diskRead = ReadCounter(diskReadCounter_);
            out.diskWrite = ReadCounter(diskWriteCounter_);
        }
    }
};

class App {
public:
    explicit App(HINSTANCE instance) : instance_(instance), settings_(LoadSettings()) {}

    int Run() {
        // Report startup failures instead of exiting silently, which looked
        // like the executable simply did nothing.
        auto fail = [](const wchar_t* what) {
            MessageBoxW(nullptr, what, kAppName, MB_OK | MB_ICONERROR);
            return 1;
        };
        if (!InitFactories()) return fail(L"ResMon could not initialise Direct2D, DirectWrite or WIC.");
        if (!RegisterWindowClass()) return fail(L"ResMon could not register its window class.");
        if (!CreateMainWindow()) return fail(L"ResMon could not create its window.");

        SetTimer(hwnd_, kSampleTimer, settings_.intervalMs, nullptr);
        SampleNow();
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        UpdateWindow(hwnd_);

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (opacityPopup_ && IsWindow(opacityPopup_) && IsDialogMessageW(opacityPopup_, &msg)) {
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return static_cast<int>(msg.wParam);
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        App* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<App*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        }
        if (self) return self->HandleMessage(msg, wParam, lParam);
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

private:
    HINSTANCE instance_{};
    HWND hwnd_{};
    HWND opacityPopup_{};
    HWND opacitySlider_{};
    HWND opacityLabel_{};
    HWND opacityCloseButton_{};
    UINT dpi_ = 96;
    Settings settings_{};
    MetricsSampler sampler_{};
    Snapshot targetSnapshot_{};
    Snapshot displaySnapshot_{};
    bool haveDisplaySnapshot_ = false;
    bool animationActive_ = false;
    ULONGLONG lastAnimationTick_ = 0;

    ComPtr<ID2D1Factory> d2dFactory_;
    ComPtr<IDWriteFactory> dwriteFactory_;
    ComPtr<IWICImagingFactory> wicFactory_;
    ComPtr<ID2D1HwndRenderTarget> renderTarget_;
    ComPtr<ID2D1Bitmap> headerLogoBitmap_;
    ComPtr<ID2D1SolidColorBrush> textBrush_;
    ComPtr<ID2D1SolidColorBrush> mutedBrush_;
    ComPtr<ID2D1SolidColorBrush> accentBrush_;
    ComPtr<ID2D1SolidColorBrush> trackBrush_;
    ComPtr<ID2D1SolidColorBrush> borderBrush_;
    ComPtr<ID2D1SolidColorBrush> backgroundBrush_;
    ComPtr<IDWriteTextFormat> titleFormat_;
    ComPtr<IDWriteTextFormat> labelFormat_;
    ComPtr<IDWriteTextFormat> metricFormat_;
    ComPtr<IDWriteTextFormat> valueFormat_;
    ComPtr<IDWriteTextFormat> tinyFormat_;

    bool SnapshotsCloseEnough() const {
        auto close = [](double a, double b, double eps) { return std::fabs(a - b) <= eps; };
        return close(displaySnapshot_.cpu, targetSnapshot_.cpu, 0.25) &&
               close(displaySnapshot_.gpu, targetSnapshot_.gpu, 0.25) &&
               displaySnapshot_.gpuAvailable == targetSnapshot_.gpuAvailable &&
               close(displaySnapshot_.ram, targetSnapshot_.ram, 0.25) &&
               close(displaySnapshot_.cpuMhz, targetSnapshot_.cpuMhz, 25.0) &&
               close(displaySnapshot_.netDown, targetSnapshot_.netDown, 1024.0) &&
               close(displaySnapshot_.netUp, targetSnapshot_.netUp, 1024.0) &&
               close(displaySnapshot_.diskRead, targetSnapshot_.diskRead, 1024.0) &&
               close(displaySnapshot_.diskWrite, targetSnapshot_.diskWrite, 1024.0) &&
               close(displaySnapshot_.thermalC, targetSnapshot_.thermalC, 0.15) &&
               displaySnapshot_.thermalAvailable == targetSnapshot_.thermalAvailable &&
               displaySnapshot_.thermalSource == targetSnapshot_.thermalSource &&
               close(displaySnapshot_.gpuThermalC, targetSnapshot_.gpuThermalC, 0.15) &&
               displaySnapshot_.gpuThermalAvailable == targetSnapshot_.gpuThermalAvailable &&
               displaySnapshot_.memTotal == targetSnapshot_.memTotal &&
               std::fabs(static_cast<double>(displaySnapshot_.memUsed) - static_cast<double>(targetSnapshot_.memUsed)) <= (8.0 * 1024.0 * 1024.0);
    }

    void AnimateStep(double blend) {
        displaySnapshot_.cpu = LerpDouble(displaySnapshot_.cpu, targetSnapshot_.cpu, blend);
        if (targetSnapshot_.gpuAvailable) {
            displaySnapshot_.gpu = LerpDouble(displaySnapshot_.gpu, targetSnapshot_.gpu, blend);
        } else {
            displaySnapshot_.gpu = targetSnapshot_.gpu;
        }
        displaySnapshot_.gpuAvailable = targetSnapshot_.gpuAvailable;
        displaySnapshot_.ram = LerpDouble(displaySnapshot_.ram, targetSnapshot_.ram, blend);
        displaySnapshot_.memUsed = LerpU64(displaySnapshot_.memUsed, targetSnapshot_.memUsed, blend);
        displaySnapshot_.memTotal = targetSnapshot_.memTotal;
        displaySnapshot_.cpuMhz = LerpDouble(displaySnapshot_.cpuMhz, targetSnapshot_.cpuMhz, blend);
        displaySnapshot_.netDown = LerpDouble(displaySnapshot_.netDown, targetSnapshot_.netDown, blend);
        displaySnapshot_.netUp = LerpDouble(displaySnapshot_.netUp, targetSnapshot_.netUp, blend);
        displaySnapshot_.diskRead = LerpDouble(displaySnapshot_.diskRead, targetSnapshot_.diskRead, blend);
        displaySnapshot_.diskWrite = LerpDouble(displaySnapshot_.diskWrite, targetSnapshot_.diskWrite, blend);
        if (targetSnapshot_.thermalAvailable) {
            displaySnapshot_.thermalC = LerpDouble(displaySnapshot_.thermalC, targetSnapshot_.thermalC, blend);
        } else {
            displaySnapshot_.thermalC = targetSnapshot_.thermalC;
        }
        displaySnapshot_.thermalAvailable = targetSnapshot_.thermalAvailable;
        displaySnapshot_.thermalSource = targetSnapshot_.thermalSource;
        if (targetSnapshot_.gpuThermalAvailable) {
            displaySnapshot_.gpuThermalC = LerpDouble(displaySnapshot_.gpuThermalC, targetSnapshot_.gpuThermalC, blend);
        } else {
            displaySnapshot_.gpuThermalC = targetSnapshot_.gpuThermalC;
        }
        displaySnapshot_.gpuThermalAvailable = targetSnapshot_.gpuThermalAvailable;
    }

    void StartAnimation() {
        animationActive_ = true;
        lastAnimationTick_ = GetTickCount64();
        SetTimer(hwnd_, kAnimationTimer, 50, nullptr);
    }

    void StopAnimation(bool snapToTarget) {
        KillTimer(hwnd_, kAnimationTimer);
        animationActive_ = false;
        if (snapToTarget) displaySnapshot_ = targetSnapshot_;
    }

    void ApplyOpacity() {
        settings_.opacityPercent = std::clamp(settings_.opacityPercent, kMinOpacityPercent, kMaxOpacityPercent);
        const BYTE alpha = static_cast<BYTE>(MulDiv(settings_.opacityPercent, 255, 100));
        SetLayeredWindowAttributes(hwnd_, 0, alpha, LWA_ALPHA);
    }

    void UpdateOpacityLabel() {
        if (!opacityLabel_) return;
        wchar_t text[64]{};
        swprintf_s(text, L"Opacity: %d%%", settings_.opacityPercent);
        SetWindowTextW(opacityLabel_, text);
    }

    static LRESULT CALLBACK OpacityWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        App* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<App*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);

        switch (msg) {
            case WM_HSCROLL:
                if (reinterpret_cast<HWND>(lParam) == self->opacitySlider_) {
                    const int pos = static_cast<int>(SendMessageW(self->opacitySlider_, TBM_GETPOS, 0, 0));
                    if (pos != self->settings_.opacityPercent) {
                        self->settings_.opacityPercent = pos;
                        self->ApplyOpacity();
                        self->UpdateOpacityLabel();
                    }

                    // Apply continuously while dragging, but avoid writing the INI file
                    // dozens of times per second. Persist when tracking ends.
                    const int scrollCode = LOWORD(wParam);
                    if (scrollCode == TB_ENDTRACK || scrollCode == TB_THUMBPOSITION) {
                        SaveSettings(self->settings_, self->hwnd_);
                    }
                }
                return 0;

            case WM_COMMAND:
                if (LOWORD(wParam) == IDC_OPACITY_CLOSE && HIWORD(wParam) == BN_CLICKED) {
                    SaveSettings(self->settings_, self->hwnd_);
                    DestroyWindow(hwnd);
                    return 0;
                }
                break;

            case WM_CLOSE:
                SaveSettings(self->settings_, self->hwnd_);
                DestroyWindow(hwnd);
                return 0;

            case WM_DESTROY:
                self->opacityPopup_ = nullptr;
                self->opacitySlider_ = nullptr;
                self->opacityLabel_ = nullptr;
                self->opacityCloseButton_ = nullptr;
                return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    void ShowOpacityPopup(POINT screenPt) {
        // Reuse the existing native settings window if it is already open.
        if (opacityPopup_ && IsWindow(opacityPopup_)) {
            ShowWindow(opacityPopup_, SW_SHOWNORMAL);
            SetForegroundWindow(opacityPopup_);
            if (opacitySlider_) SetFocus(opacitySlider_);
            return;
        }

        const int scaleDpi = static_cast<int>(dpi_);
        const int clientW = MulDiv(286, scaleDpi, 96);
        const int clientH = MulDiv(116, scaleDpi, 96);

        RECT wr{0, 0, clientW, clientH};
        const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
        const DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_CONTROLPARENT | (settings_.topMost ? WS_EX_TOPMOST : 0);
        AdjustWindowRectExForDpi(&wr, style, FALSE, exStyle, dpi_);
        const int windowW = wr.right - wr.left;
        const int windowH = wr.bottom - wr.top;

        HMONITOR mon = MonitorFromPoint(screenPt, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(mon, &mi);
        int x = std::min(static_cast<int>(screenPt.x), static_cast<int>(mi.rcWork.right) - windowW);
        int y = std::min(static_cast<int>(screenPt.y), static_cast<int>(mi.rcWork.bottom) - windowH);
        x = std::max(x, static_cast<int>(mi.rcWork.left));
        y = std::max(y, static_cast<int>(mi.rcWork.top));

        opacityPopup_ = CreateWindowExW(
            exStyle,
            kOpacityWindowClass,
            L"ResMon - Opacity",
            style,
            x, y, windowW, windowH,
            hwnd_, nullptr, instance_, this);
        if (!opacityPopup_) return;

        const HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        const int margin = MulDiv(12, scaleDpi, 96);

        opacityLabel_ = CreateWindowExW(
            0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            margin, MulDiv(10, scaleDpi, 96),
            MulDiv(170, scaleDpi, 96), MulDiv(20, scaleDpi, 96),
            opacityPopup_, nullptr, instance_, nullptr);
        SendMessageW(opacityLabel_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

        opacitySlider_ = CreateWindowExW(
            0, TRACKBAR_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_AUTOTICKS,
            margin, MulDiv(34, scaleDpi, 96),
            clientW - margin * 2, MulDiv(36, scaleDpi, 96),
            opacityPopup_, nullptr, instance_, nullptr);
        SendMessageW(opacitySlider_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(opacitySlider_, TBM_SETRANGEMIN, FALSE, kMinOpacityPercent);
        SendMessageW(opacitySlider_, TBM_SETRANGEMAX, TRUE, kMaxOpacityPercent);
        SendMessageW(opacitySlider_, TBM_SETLINESIZE, 0, 1);
        SendMessageW(opacitySlider_, TBM_SETPAGESIZE, 0, 5);
        SendMessageW(opacitySlider_, TBM_SETTICFREQ, 5, 0);
        SendMessageW(opacitySlider_, TBM_SETPOS, TRUE, settings_.opacityPercent);

        opacityCloseButton_ = CreateWindowExW(
            0, L"BUTTON", L"Close",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            clientW - margin - MulDiv(74, scaleDpi, 96), MulDiv(78, scaleDpi, 96),
            MulDiv(74, scaleDpi, 96), MulDiv(26, scaleDpi, 96),
            opacityPopup_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_OPACITY_CLOSE)), instance_, nullptr);
        SendMessageW(opacityCloseButton_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

        UpdateOpacityLabel();

        // Keep only native Windows title-bar styling; the controls themselves are
        // standard Win32 controls and are not custom painted by ResMon.
        const BOOL dark = settings_.theme == ThemeMode::Dark ? TRUE : FALSE;
        const DWORD darkModeAttr = 20; // DWMWA_USE_IMMERSIVE_DARK_MODE
        DwmSetWindowAttribute(opacityPopup_, darkModeAttr, &dark, sizeof(dark));
        const DWORD cornerAttr = 33; // DWMWA_WINDOW_CORNER_PREFERENCE
        const int cornerPreference = 2; // DWMWCP_ROUND
        DwmSetWindowAttribute(opacityPopup_, cornerAttr, &cornerPreference, sizeof(cornerPreference));

        ShowWindow(opacityPopup_, SW_SHOWNORMAL);
        UpdateWindow(opacityPopup_);
        SetForegroundWindow(opacityPopup_);
        SetFocus(opacitySlider_);
    }

    bool InitFactories() {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory_.GetAddressOf()))) return false;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(dwriteFactory_.GetAddressOf())))) return false;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(wicFactory_.GetAddressOf())))) return false;
        if (FAILED(CreateTextFormats())) return false;
        return true;
    }

    HRESULT CreateHeaderLogoBitmap() {
        if (headerLogoBitmap_) return S_OK;
        if (!wicFactory_ || !renderTarget_) return E_FAIL;

        HRSRC resource = FindResourceW(instance_, MAKEINTRESOURCEW(IDR_HEADER_LOGO_PNG), RT_RCDATA);
        if (!resource) return HRESULT_FROM_WIN32(GetLastError());
        HGLOBAL loaded = LoadResource(instance_, resource);
        if (!loaded) return HRESULT_FROM_WIN32(GetLastError());
        const DWORD size = SizeofResource(instance_, resource);
        const void* data = LockResource(loaded);
        if (!data || size == 0) return E_FAIL;

        ComPtr<IWICStream> stream;
        HRESULT hr = wicFactory_->CreateStream(stream.GetAddressOf());
        if (FAILED(hr)) return hr;
        hr = stream->InitializeFromMemory(const_cast<BYTE*>(reinterpret_cast<const BYTE*>(data)), size);
        if (FAILED(hr)) return hr;

        ComPtr<IWICBitmapDecoder> decoder;
        hr = wicFactory_->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf());
        if (FAILED(hr)) return hr;

        ComPtr<IWICBitmapFrameDecode> frame;
        hr = decoder->GetFrame(0, frame.GetAddressOf());
        if (FAILED(hr)) return hr;

        ComPtr<IWICFormatConverter> converter;
        hr = wicFactory_->CreateFormatConverter(converter.GetAddressOf());
        if (FAILED(hr)) return hr;
        hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
        if (FAILED(hr)) return hr;

        return renderTarget_->CreateBitmapFromWicBitmap(converter.Get(), nullptr, headerLogoBitmap_.GetAddressOf());
    }

    bool RegisterWindowClass() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.hInstance = instance_;
        wc.lpfnWndProc = &App::WndProc;
        wc.lpszClassName = kWindowClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(101));
        wc.hIconSm = static_cast<HICON>(LoadImageW(
            instance_, MAKEINTRESOURCEW(101), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
        if (!wc.hIconSm) wc.hIconSm = wc.hIcon;
        const bool mainRegistered = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        if (!mainRegistered) return false;

        WNDCLASSEXW popup{};
        popup.cbSize = sizeof(popup);
        popup.hInstance = instance_;
        popup.lpfnWndProc = &App::OpacityWndProc;
        popup.lpszClassName = kOpacityWindowClass;
        popup.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        popup.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        return RegisterClassExW(&popup) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    int DipToPx(int dip) const {
        return MulDiv(dip, static_cast<int>(dpi_), 96);
    }

    int VisibleRowCount() const {
        return (settings_.showCpu ? 1 : 0) +
               (settings_.showGpu ? 1 : 0) +
               (settings_.showRam ? 1 : 0) +
               (settings_.showNet ? 1 : 0) +
               (settings_.showDisk ? 1 : 0);
    }

    int DesiredHeightDip() const {
        return kHeaderHeightDip + VisibleRowCount() * kRowHeightDip;
    }

    bool CreateMainWindow() {
        dpi_ = GetDpiForSystem();
        int x = settings_.hasPosition ? settings_.x : 60;
        int y = settings_.hasPosition ? settings_.y : 60;
        const DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_LAYERED | (settings_.topMost ? WS_EX_TOPMOST : 0);
        hwnd_ = CreateWindowExW(
            exStyle,
            kWindowClass,
            kAppName,
            WS_POPUP,
            x,
            y,
            DipToPx(kWidthDip),
            DipToPx(DesiredHeightDip()),
            nullptr,
            nullptr,
            instance_,
            this);
        if (!hwnd_) return false;

        dpi_ = GetDpiForWindow(hwnd_);
        ApplyWindowStyle();
        ApplyOpacity();
        return true;
    }

    void ApplyWindowStyle() {
        const BOOL dark = settings_.theme == ThemeMode::Dark ? TRUE : FALSE;
        const DWORD darkModeAttr = 20; // DWMWA_USE_IMMERSIVE_DARK_MODE
        DwmSetWindowAttribute(hwnd_, darkModeAttr, &dark, sizeof(dark));

        const DWORD cornerAttr = 33; // DWMWA_WINDOW_CORNER_PREFERENCE
        const int cornerPreference = 2; // DWMWCP_ROUND
        DwmSetWindowAttribute(hwnd_, cornerAttr, &cornerPreference, sizeof(cornerPreference));

        // Explicitly disable Mica/Acrylic/system backdrops. ResMon uses a simple
        // flat surface; the user-controlled layered-window opacity is the only
        // transparency applied to the main widget.
        const DWORD backdropAttr = 38; // DWMWA_SYSTEMBACKDROP_TYPE
        const int backdropType = 1; // DWMSBT_NONE
        DwmSetWindowAttribute(hwnd_, backdropAttr, &backdropType, sizeof(backdropType));

        DWM_BLURBEHIND blur{};
        blur.dwFlags = DWM_BB_ENABLE;
        blur.fEnable = FALSE;
        DwmEnableBlurBehindWindow(hwnd_, &blur);
    }

    // IDWriteTextFormat is device independent, so these are built once and kept
    // across theme changes and render-target loss. Recreating them in
    // CreateDeviceResources() also leaked the previous formats, because
    // ComPtr::GetAddressOf() overwrites the stored pointer without releasing it.
    HRESULT CreateTextFormats() {
        struct FormatSpec {
            const wchar_t* family;
            DWRITE_FONT_WEIGHT weight;
            float size;
            DWRITE_TEXT_ALIGNMENT alignment;
            ComPtr<IDWriteTextFormat>* target;
        };
        const FormatSpec specs[] = {
            {L"Segoe UI Variable Display", DWRITE_FONT_WEIGHT_SEMI_BOLD, 14.5f, DWRITE_TEXT_ALIGNMENT_LEADING,  &titleFormat_},
            {L"Segoe UI Variable Text",    DWRITE_FONT_WEIGHT_MEDIUM,    10.0f, DWRITE_TEXT_ALIGNMENT_LEADING,  &labelFormat_},
            {L"Segoe UI Variable Text",    DWRITE_FONT_WEIGHT_SEMI_BOLD, 10.0f, DWRITE_TEXT_ALIGNMENT_TRAILING, &metricFormat_},
            {L"Segoe UI Variable Text",    DWRITE_FONT_WEIGHT_NORMAL,    10.0f, DWRITE_TEXT_ALIGNMENT_LEADING,  &valueFormat_},
            {L"Segoe UI Variable Text",    DWRITE_FONT_WEIGHT_NORMAL,     8.0f, DWRITE_TEXT_ALIGNMENT_TRAILING, &tinyFormat_},
        };
        for (const auto& spec : specs) {
            const HRESULT hr = dwriteFactory_->CreateTextFormat(
                spec.family, nullptr, spec.weight, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, spec.size, L"",
                spec.target->ReleaseAndGetAddressOf());
            if (FAILED(hr)) return hr;
            (*spec.target)->SetTextAlignment(spec.alignment);
        }
        return S_OK;
    }

    HRESULT CreateDeviceResources() {
        if (renderTarget_) return S_OK;
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        const D2D1_SIZE_U size = D2D1::SizeU(std::max(1L, rc.right - rc.left), std::max(1L, rc.bottom - rc.top));
        HRESULT hr = d2dFactory_->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(hwnd_, size),
            renderTarget_.GetAddressOf());
        if (FAILED(hr)) return hr;
        renderTarget_->SetDpi(static_cast<FLOAT>(dpi_), static_cast<FLOAT>(dpi_));

        const bool isLight = settings_.theme == ThemeMode::Light;
        const D2D1_COLOR_F textColor = isLight ? D2D1::ColorF(0x1F2329) : D2D1::ColorF(0xF1F3F5);
        const D2D1_COLOR_F mutedColor = isLight ? D2D1::ColorF(0x68717D) : D2D1::ColorF(0x8D96A3);
        const D2D1_COLOR_F accentColor = isLight ? D2D1::ColorF(0x5277D6) : D2D1::ColorF(0x7596E8);
        const D2D1_COLOR_F trackColor = isLight ? D2D1::ColorF(0xDCE1E8) : D2D1::ColorF(0x2A2F37);
        const D2D1_COLOR_F borderColor = isLight ? D2D1::ColorF(0xC7CCD3) : D2D1::ColorF(0x3B414B);
        const D2D1_COLOR_F backgroundColor = isLight ? D2D1::ColorF(0xF7F7F8) : D2D1::ColorF(0x15181D);

        hr = renderTarget_->CreateSolidColorBrush(backgroundColor, backgroundBrush_.GetAddressOf());
        if (FAILED(hr)) return hr;
        hr = renderTarget_->CreateSolidColorBrush(textColor, textBrush_.GetAddressOf());
        if (FAILED(hr)) return hr;
        hr = renderTarget_->CreateSolidColorBrush(mutedColor, mutedBrush_.GetAddressOf());
        if (FAILED(hr)) return hr;
        hr = renderTarget_->CreateSolidColorBrush(accentColor, accentBrush_.GetAddressOf());
        if (FAILED(hr)) return hr;
        hr = renderTarget_->CreateSolidColorBrush(trackColor, trackBrush_.GetAddressOf());
        if (FAILED(hr)) return hr;
        hr = renderTarget_->CreateSolidColorBrush(borderColor, borderBrush_.GetAddressOf());
        if (FAILED(hr)) return hr;

        hr = CreateHeaderLogoBitmap();
        if (FAILED(hr)) return hr;
        return S_OK;
    }

    void DiscardDeviceResources() {
        headerLogoBitmap_.Reset();
        renderTarget_.Reset();
        textBrush_.Reset();
        mutedBrush_.Reset();
        accentBrush_.Reset();
        trackBrush_.Reset();
        borderBrush_.Reset();
        backgroundBrush_.Reset();
    }

    void DrawTextSimple(const std::wstring& text, IDWriteTextFormat* format, const D2D1_RECT_F& rect,
                        ID2D1Brush* brush) {
        renderTarget_->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format, rect, brush,
                                 D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
    }

    void DrawBar(float left, float top, float right, float percent) {
        const float h = 2.0f;
        renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(left, top, right, top + h), 1.0f, 1.0f), trackBrush_.Get());
        const float fill = left + (right - left) * static_cast<float>(std::clamp(percent, 0.0f, 100.0f) / 100.0f);
        renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(left, top, std::max(left, fill), top + h), 1.0f, 1.0f), accentBrush_.Get());
    }

    // Fixed scan columns: label | percentage/value | temperature | detail.
    // Keeping the column geometry stable makes CPU/GPU/RAM easy to compare
    // without the text shifting as sensors appear or disappear.
    static constexpr float kLabelLeft = 16.0f;
    static constexpr float kLabelRight = 58.0f;
    static constexpr float kPrimaryLeft = 60.0f;
    static constexpr float kPrimaryRight = 108.0f;
    static constexpr float kTempLeft = 112.0f;
    static constexpr float kTempRight = 160.0f;
    static constexpr float kDetailLeft = 172.0f;
    static constexpr float kRowRight = kWidthDip - 16.0f;

    void DrawLoadRow(float y, const wchar_t* label, const std::wstring& primary,
                     const std::wstring& temperature, const std::wstring& detail,
                     double percent, bool bar) {
        DrawTextSimple(label, labelFormat_.Get(), D2D1::RectF(kLabelLeft, y, kLabelRight, y + 18.0f), mutedBrush_.Get());
        DrawTextSimple(primary, metricFormat_.Get(), D2D1::RectF(kPrimaryLeft, y, kPrimaryRight, y + 18.0f), textBrush_.Get());
        DrawTextSimple(temperature, metricFormat_.Get(), D2D1::RectF(kTempLeft, y, kTempRight, y + 18.0f), textBrush_.Get());
        DrawTextSimple(detail, valueFormat_.Get(), D2D1::RectF(kDetailLeft, y, kRowRight, y + 18.0f), textBrush_.Get());
        if (bar) DrawBar(kPrimaryLeft, y + 19.5f, kRowRight, static_cast<float>(percent));
    }

    // NET and DISK have no percentage and no temperature reading, so their text
    // starts at the percentage column instead of leaving those two columns
    // blank. That reclaims the widest run of dead space in the widget.
    void DrawInfoRow(float y, const wchar_t* label, const std::wstring& detail) {
        DrawTextSimple(label, labelFormat_.Get(), D2D1::RectF(kLabelLeft, y, kLabelRight, y + 18.0f), mutedBrush_.Get());
        DrawTextSimple(detail, valueFormat_.Get(), D2D1::RectF(kPrimaryLeft, y, kRowRight, y + 18.0f), textBrush_.Get());
    }

    void Render(const RECT& paintRectPx) {
        if (FAILED(CreateDeviceResources())) return;
        const float pxToDip = 96.0f / static_cast<float>(dpi_);
        const D2D1_RECT_F clip = D2D1::RectF(
            paintRectPx.left * pxToDip, paintRectPx.top * pxToDip,
            paintRectPx.right * pxToDip, paintRectPx.bottom * pxToDip);

        renderTarget_->BeginDraw();
        renderTarget_->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
        renderTarget_->FillRectangle(D2D1::RectF(0.0f, 0.0f, static_cast<float>(kWidthDip), static_cast<float>(DesiredHeightDip())), backgroundBrush_.Get());

        const auto borderRect = D2D1::RoundedRect(
            D2D1::RectF(0.5f, 0.5f, kWidthDip - 0.5f, DesiredHeightDip() - 0.5f),
            7.0f, 7.0f);
        renderTarget_->DrawRoundedRectangle(borderRect, borderBrush_.Get(), 1.0f);

        if (headerLogoBitmap_) {
            const D2D1_RECT_F logoRect = D2D1::RectF(16.0f, 6.0f, 16.0f + kHeaderLogoDip, 6.0f + kHeaderLogoDip);
            renderTarget_->DrawBitmap(headerLogoBitmap_.Get(), logoRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
        DrawTextSimple(L"ResMon", titleFormat_.Get(), D2D1::RectF(51.0f, 11.0f, 170.0f, 34.0f), textBrush_.Get());
        wchar_t cadence[32]{};
        swprintf_s(cadence, L"%.0fs", settings_.intervalMs / 1000.0);
        DrawTextSimple(cadence, tinyFormat_.Get(), D2D1::RectF(160.0f, 14.0f, kWidthDip - 16.0f, 30.0f), mutedBrush_.Get());

        float y = 44.0f;
        auto nextRow = [&y]() { y += static_cast<float>(kRowHeightDip); };

        if (settings_.showCpu) {
            std::wstring cpuTemp;
            if (settings_.showTemp) {
                if (displaySnapshot_.thermalAvailable) {
                    wchar_t temp[32]{};
                    swprintf_s(temp, L"%.0f C", displaySnapshot_.thermalC);
                    cpuTemp = temp;
                } else {
                    cpuTemp = L"--";
                }
            }

            std::wstring cpuDetail = L"--";
            if (displaySnapshot_.cpuMhz > 0.0) {
                wchar_t freq[48]{};
                swprintf_s(freq, L"%.2f GHz", displaySnapshot_.cpuMhz / 1000.0);
                cpuDetail = freq;
            }
            DrawLoadRow(y, L"CPU", FormatPercent(displaySnapshot_.cpu), cpuTemp, cpuDetail, displaySnapshot_.cpu, true);
            nextRow();
        }

        if (settings_.showGpu) {
            const std::wstring gpuPrimary = displaySnapshot_.gpuAvailable ? FormatPercent(displaySnapshot_.gpu) : L"--";
            std::wstring gpuTemp;
            if (settings_.showTemp) {
                if (displaySnapshot_.gpuThermalAvailable) {
                    wchar_t temp[32]{};
                    swprintf_s(temp, L"%.0f C", displaySnapshot_.gpuThermalC);
                    gpuTemp = temp;
                } else {
                    gpuTemp = L"--";
                }
            }
            const std::wstring gpuDetail = displaySnapshot_.gpuThermalAvailable ? L"NVIDIA" : L"";
            DrawLoadRow(y, L"GPU", gpuPrimary, gpuTemp, gpuDetail, displaySnapshot_.gpu, displaySnapshot_.gpuAvailable);
            nextRow();
        }

        if (settings_.showRam) {
            std::wstring ramDetail = L"--";
            if (displaySnapshot_.memTotal > 0) {
                ramDetail = FormatGiB(displaySnapshot_.memUsed) + L" / " + FormatGiB(displaySnapshot_.memTotal);
            }
            DrawLoadRow(y, L"RAM", FormatPercent(displaySnapshot_.ram), L"", ramDetail, displaySnapshot_.ram, true);
            nextRow();
        }

        if (settings_.showNet) {
            const std::wstring netValue = L"D " + FormatRate(displaySnapshot_.netDown) + L"   U " + FormatRate(displaySnapshot_.netUp);
            DrawInfoRow(y, L"NET", netValue);
            nextRow();
        }

        if (settings_.showDisk) {
            const std::wstring diskValue = L"R " + FormatRate(displaySnapshot_.diskRead) + L"   W " + FormatRate(displaySnapshot_.diskWrite);
            DrawInfoRow(y, L"DISK", diskValue);
        }

        renderTarget_->PopAxisAlignedClip();
        const HRESULT hr = renderTarget_->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) {
            DiscardDeviceResources();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

    void InvalidateMetrics() {
        RECT rc{};
        if (!GetClientRect(hwnd_, &rc)) return;
        rc.top = DipToPx(40);
        InvalidateRect(hwnd_, &rc, FALSE);
    }

    void SampleNow() {
        SampleOptions options{};
        options.cpu = settings_.showCpu;
        options.gpu = settings_.showGpu;
        options.ram = settings_.showRam;
        options.temp = settings_.showTemp;
        options.net = settings_.showNet;
        options.disk = settings_.showDisk;
        targetSnapshot_ = sampler_.Sample(options);
        if (!haveDisplaySnapshot_) {
            displaySnapshot_ = targetSnapshot_;
            haveDisplaySnapshot_ = true;
            InvalidateMetrics();
            return;
        }
        StartAnimation();
        InvalidateMetrics();
    }

    void ResizeForSettings() {
        RECT r{};
        GetWindowRect(hwnd_, &r);
        SetWindowPos(hwnd_, nullptr, r.left, r.top, DipToPx(kWidthDip), DipToPx(DesiredHeightDip()),
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void ApplyTopMost() {
        SetWindowPos(hwnd_, settings_.topMost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    void ShowContextMenu(POINT screenPt) {
        HMENU root = CreatePopupMenu();
        HMENU rows = CreatePopupMenu();
        HMENU interval = CreatePopupMenu();
        HMENU theme = CreatePopupMenu();
        HMENU sensors = CreatePopupMenu();

        AppendMenuW(rows, MF_STRING | (settings_.showCpu ? MF_CHECKED : 0), IDM_ROW_CPU, L"CPU");
        AppendMenuW(rows, MF_STRING | (settings_.showGpu ? MF_CHECKED : 0), IDM_ROW_GPU, L"GPU");
        AppendMenuW(rows, MF_STRING | (settings_.showRam ? MF_CHECKED : 0), IDM_ROW_RAM, L"RAM");
        AppendMenuW(rows, MF_STRING | (settings_.showTemp ? MF_CHECKED : 0), IDM_ROW_TEMP, L"Temperature column");
        AppendMenuW(rows, MF_STRING | (settings_.showNet ? MF_CHECKED : 0), IDM_ROW_NET, L"Network");
        AppendMenuW(rows, MF_STRING | (settings_.showDisk ? MF_CHECKED : 0), IDM_ROW_DISK, L"Disk I/O");
        AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(rows), L"Rows");
        AppendMenuW(root, MF_STRING | (settings_.topMost ? MF_CHECKED : 0), IDM_TOGGLE_TOPMOST, L"Always on top");
        AppendMenuW(root, MF_SEPARATOR, 0, nullptr);

        AppendMenuW(interval, MF_STRING | (settings_.intervalMs == 1000 ? MF_CHECKED : 0), IDM_INTERVAL_1000, L"1 second");
        AppendMenuW(interval, MF_STRING | (settings_.intervalMs == 2000 ? MF_CHECKED : 0), IDM_INTERVAL_2000, L"2 seconds");
        AppendMenuW(interval, MF_STRING | (settings_.intervalMs == 5000 ? MF_CHECKED : 0), IDM_INTERVAL_5000, L"5 seconds");
        AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(interval), L"Update rate");
        AppendMenuW(theme, MF_STRING | (settings_.theme == ThemeMode::Dark ? MF_CHECKED : 0), IDM_THEME_DARK, L"Dark");
        AppendMenuW(theme, MF_STRING | (settings_.theme == ThemeMode::Light ? MF_CHECKED : 0), IDM_THEME_LIGHT, L"Light");
        AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(theme), L"Theme");
        wchar_t opacityText[64]{};
        swprintf_s(opacityText, L"Opacity...   %d%%", settings_.opacityPercent);
        AppendMenuW(root, MF_STRING, IDM_OPACITY, opacityText);

        UINT sensorFlags = MF_STRING;
        std::wstring sensorText;
        if (sampler_.EnhancedThermalActive()) {
            sensorFlags |= MF_CHECKED;
            sensorText = L"Enhanced CPU temperature active";
        } else if (sampler_.EnhancedThermalAccessDenied()) {
            sensorText = L"Enable enhanced CPU temperature (admin)...";
        } else {
            sensorText = L"Install enhanced CPU temperature...";
        }
        AppendMenuW(sensors, sensorFlags, IDM_SENSOR_INSTALL, sensorText.c_str());
        AppendMenuW(sensors, MF_STRING | MF_GRAYED, 0,
                    sampler_.NvidiaThermalActive() ? L"NVIDIA GPU temp: NVML active" : L"NVIDIA GPU temp: automatic via NVML");
        AppendMenuW(sensors, MF_STRING, IDM_SENSOR_REFRESH, L"Refresh temperature backends");
        AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(sensors), L"Sensors");

        AppendMenuW(root, MF_STRING | (IsAutostartEnabled() ? MF_CHECKED : 0), IDM_AUTOSTART, L"Start with Windows");
        AppendMenuW(root, MF_STRING, IDM_RESET_POSITION, L"Reset position");
        AppendMenuW(root, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(root, MF_STRING, IDM_ABOUT, L"About");
        AppendMenuW(root, MF_STRING, IDM_EXIT, L"Exit");

        SetForegroundWindow(hwnd_);
        const UINT cmd = TrackPopupMenu(root, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                        screenPt.x, screenPt.y, 0, hwnd_, nullptr);
        DestroyMenu(root);
        if (cmd) ExecuteCommand(cmd);
    }

    void ExecuteCommand(UINT cmd) {
        switch (cmd) {
            case IDM_ROW_CPU:
                settings_.showCpu = !settings_.showCpu;
                ResizeForSettings();
                haveDisplaySnapshot_ = false;
                SampleNow();
                break;
            case IDM_ROW_GPU:
                settings_.showGpu = !settings_.showGpu;
                ResizeForSettings();
                haveDisplaySnapshot_ = false;
                SampleNow();
                break;
            case IDM_ROW_RAM:
                settings_.showRam = !settings_.showRam;
                ResizeForSettings();
                haveDisplaySnapshot_ = false;
                SampleNow();
                break;
            case IDM_ROW_TEMP:
                settings_.showTemp = !settings_.showTemp;
                haveDisplaySnapshot_ = false;
                SampleNow();
                break;
            case IDM_ROW_NET:
                settings_.showNet = !settings_.showNet;
                ResizeForSettings();
                haveDisplaySnapshot_ = false;
                SampleNow();
                break;
            case IDM_ROW_DISK:
                settings_.showDisk = !settings_.showDisk;
                ResizeForSettings();
                haveDisplaySnapshot_ = false;
                SampleNow();
                break;
            case IDM_TOGGLE_TOPMOST:
                settings_.topMost = !settings_.topMost;
                ApplyTopMost();
                break;
            case IDM_INTERVAL_1000:
            case IDM_INTERVAL_2000:
            case IDM_INTERVAL_5000:
                settings_.intervalMs = (cmd == IDM_INTERVAL_1000) ? 1000 : (cmd == IDM_INTERVAL_2000 ? 2000 : 5000);
                KillTimer(hwnd_, kSampleTimer);
                SetTimer(hwnd_, kSampleTimer, settings_.intervalMs, nullptr);
                InvalidateRect(hwnd_, nullptr, FALSE);
                break;
            case IDM_THEME_DARK:
                settings_.theme = ThemeMode::Dark;
                DiscardDeviceResources();
                ApplyWindowStyle();
                InvalidateRect(hwnd_, nullptr, FALSE);
                break;
            case IDM_THEME_LIGHT:
                settings_.theme = ThemeMode::Light;
                DiscardDeviceResources();
                ApplyWindowStyle();
                InvalidateRect(hwnd_, nullptr, FALSE);
                break;
            case IDM_OPACITY: {
                POINT pt{};
                GetCursorPos(&pt);
                ShowOpacityPopup(pt);
                break;
            }
            case IDM_SENSOR_INSTALL: {
                if (sampler_.EnhancedThermalActive()) {
                    MessageBoxW(hwnd_, L"Enhanced CPU temperature is already active.", kAppName, MB_OK | MB_ICONINFORMATION);
                    break;
                }
                if (sampler_.EnhancedThermalAccessDenied() && !IsProcessElevated()) {
                    const int answer = MessageBoxW(hwnd_,
                        L"PawnIO is installed, but its hardware device is restricted to administrators.\n\n"
                        L"Restart ResMon as administrator now? This allows the enhanced CPU-temperature reader to access the signed driver.",
                        kAppName, MB_YESNO | MB_ICONINFORMATION);
                    if (answer == IDYES) {
                        SHELLEXECUTEINFOW sei{};
                        sei.cbSize = sizeof(sei);
                        sei.hwnd = hwnd_;
                        sei.lpVerb = L"runas";
                        const std::wstring exe = GetExePath();
                        sei.lpFile = exe.c_str();
                        sei.nShow = SW_SHOWNORMAL;
                        if (ShellExecuteExW(&sei)) DestroyWindow(hwnd_);
                    }
                    break;
                }
                DWORD exitCode = ERROR_GEN_FAILURE;
                std::wstring detail;
                if (!InstallPawnIoFromEmbeddedSetup(hwnd_, exitCode, detail)) {
                    MessageBoxW(hwnd_, detail.c_str(), kAppName, MB_OK | MB_ICONERROR);
                    break;
                }
                const bool active = sampler_.RefreshEnhancedThermal();
                SampleNow();
                if (active) {
                    MessageBoxW(hwnd_, L"Enhanced CPU temperature is active.", kAppName, MB_OK | MB_ICONINFORMATION);
                } else if (exitCode == ERROR_SUCCESS_REBOOT_REQUIRED) {
                    MessageBoxW(hwnd_, L"PawnIO installed successfully. Restart Windows, then open ResMon as administrator to enable enhanced CPU temperatures.", kAppName, MB_OK | MB_ICONINFORMATION);
                } else if (sampler_.EnhancedThermalAccessDenied() && !IsProcessElevated()) {
                    MessageBoxW(hwnd_, L"PawnIO installed successfully. Restart ResMon as administrator to use enhanced CPU temperatures.", kAppName, MB_OK | MB_ICONINFORMATION);
                } else {
                    MessageBoxW(hwnd_, L"PawnIO installed, but the enhanced temperature module could not be activated on this CPU. ResMon will continue using the Windows ACPI fallback when available.", kAppName, MB_OK | MB_ICONWARNING);
                }
                break;
            }
            case IDM_SENSOR_REFRESH:
                sampler_.RefreshEnhancedThermal();
                SampleNow();
                break;
            case IDM_AUTOSTART:
                SetAutostart(!IsAutostartEnabled());
                break;
            case IDM_RESET_POSITION:
                SetWindowPos(hwnd_, settings_.topMost ? HWND_TOPMOST : HWND_NOTOPMOST,
                             DipToPx(60), DipToPx(60), 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
                break;
            case IDM_ABOUT:
                MessageBoxW(hwnd_,
                    L"ResMon\n\n"
                    L"A deliberately tiny Windows 11 resource monitor.\n"
                    L"CPU: GetSystemTimes\n"
                    L"RAM: GlobalMemoryStatusEx\n"
                    L"Network + GPU + disk I/O: Windows PDH counters\n"
                    L"CPU temperature: optional PawnIO reader; ACPI fallback\n"
                    L"NVIDIA GPU temperature: NVML from the installed driver\n"
                    L"Rows can be enabled or hidden independently.\n\n"
                    L"No Python, Qt, .NET, Electron, or background service.",
                    kAppName, MB_OK | MB_ICONINFORMATION);
                break;
            case IDM_EXIT:
                // WM_DESTROY persists the settings; returning here avoids a
                // second save through an already-destroyed window handle.
                DestroyWindow(hwnd_);
                return;
        }
        SaveSettings(settings_, hwnd_);
    }

    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
            case WM_CREATE:
                return 0;
            case WM_TIMER:
                if (wParam == kSampleTimer) {
                    SampleNow();
                } else if (wParam == kAnimationTimer && animationActive_) {
                    const ULONGLONG now = GetTickCount64();
                    const double dt = std::max(0.001, (now - lastAnimationTick_) / 1000.0);
                    lastAnimationTick_ = now;
                    const double blend = std::clamp(1.0 - std::exp(-8.0 * dt), 0.12, 0.45);
                    AnimateStep(blend);
                    if (SnapshotsCloseEnough()) {
                        StopAnimation(true);
                    }
                    InvalidateMetrics();
                }
                return 0;
            case WM_PAINT: {
                PAINTSTRUCT ps{};
                BeginPaint(hwnd_, &ps);
                Render(ps.rcPaint);
                EndPaint(hwnd_, &ps);
                return 0;
            }
            case WM_ERASEBKGND:
                return 1;
            case WM_SIZE:
                if (renderTarget_) {
                    renderTarget_->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
                }
                // Resizing discards the render target's contents, so the whole
                // window has to be repainted - not just the metric rows that
                // InvalidateMetrics() covers. Without this the header strip
                // keeps the discarded (blank) pixels after a row is toggled,
                // because the window class has no CS_VREDRAW.
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            case WM_DPICHANGED: {
                dpi_ = HIWORD(wParam);
                auto* suggested = reinterpret_cast<RECT*>(lParam);
                SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                if (renderTarget_) renderTarget_->SetDpi(static_cast<FLOAT>(dpi_), static_cast<FLOAT>(dpi_));
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            case WM_LBUTTONDOWN: {
                const int y = GET_Y_LPARAM(lParam);
                if (y < DipToPx(kHeaderDragHeightDip)) {
                    ReleaseCapture();
                    SendMessageW(hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                }
                return 0;
            }
            case WM_RBUTTONUP: {
                POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                ClientToScreen(hwnd_, &pt);
                ShowContextMenu(pt);
                return 0;
            }
            case WM_NCRBUTTONUP: {
                POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                ShowContextMenu(pt);
                return 0;
            }
            case WM_DISPLAYCHANGE:
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            case WM_DESTROY:
                KillTimer(hwnd_, kSampleTimer);
                KillTimer(hwnd_, kAnimationTimer);
                if (opacityPopup_) DestroyWindow(opacityPopup_);
                SaveSettings(settings_, hwnd_);
                PostQuitMessage(0);
                return 0;
            default:
                return DefWindowProcW(hwnd_, msg, wParam, lParam);
        }
    }
};

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES};
    InitCommonControlsEx(&controls);
    App app(hInstance);
    const int result = app.Run();
    if (SUCCEEDED(comHr)) CoUninitialize();
    return result;
}
