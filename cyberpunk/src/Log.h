#pragma once
#include <filesystem>

namespace mlcp {
void LogOpen(const std::filesystem::path& file);
void Log(const char* fmt, ...);
}
