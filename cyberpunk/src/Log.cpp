#include "Log.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <windows.h>

namespace mlcp {
namespace {
std::mutex g_mutex;
FILE* g_file = nullptr;
}

void LogOpen(const std::filesystem::path& file) {
    std::lock_guard lock(g_mutex);
    if (!g_file) g_file = _wfsopen(file.c_str(), L"w", _SH_DENYWR);
}

void Log(const char* fmt, ...) {
    char line[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof line, fmt, args);
    va_end(args);
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::lock_guard lock(g_mutex);
    if (!g_file) return;
    fprintf(g_file, "%02u:%02u:%02u.%03u [%lu] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
            GetCurrentThreadId(), line);
    fflush(g_file);
}
}
