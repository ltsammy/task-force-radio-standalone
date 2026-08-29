#include "Log.h"

#include <windows.h>

#include <cstdio>

namespace tfrs {
namespace voice {

void logLine(const std::string& message) {
    wchar_t appData[MAX_PATH];
    const DWORD len = GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return;

    // %APPDATA%\Tfrs\Extension, not \Tfrs\VoiceClient like the old bridge-era log -- there's no
    // separate voice client to name it after anymore, this is the extension's own log now.
    std::wstring dir(appData);
    dir += L"\\Tfrs";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\Extension";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\extension.log";

    HANDLE file = CreateFileW(dir.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "[%04d-%02d-%02d %02d:%02d:%02d] ", st.wYear, st.wMonth,
             st.wDay, st.wHour, st.wMinute, st.wSecond);

    const std::string line = std::string(prefix) + message + "\r\n";
    DWORD written = 0;
    WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(file);
}

std::string toHex(uint32_t value) {
    char buf[9];
    snprintf(buf, sizeof(buf), "%08X", value);
    return std::string(buf);
}

}  // namespace voice
}  // namespace tfrs
