// What a support thread asks first: which game, which Windows (or Wine), which GPU and driver. Logged once so a
// Magelight.log answers it on its own.

#include "MagelightSysInfo.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>   // CommonLib before windows.h

#include <windows.h>
#include <dxgi.h>
#include <intrin.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <string_view>
#include <vector>

namespace Magelight::SysInfo {

    namespace {

        std::string Narrow(std::wstring_view w)
        {
            if (w.empty()) return {};
            const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
            std::string out(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
            if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
            return out;
        }

        std::string Trimmed(std::string s)
        {
            const auto first = s.find_first_not_of(' ');
            if (first == std::string::npos) return {};
            s.erase(0, first);
            s.erase(s.find_last_not_of(' ') + 1);
            return s;
        }

        // Major.minor.build.UBR plus the release name ("25H2"); the update revision (UBR) and DisplayVersion are
        // not in any version API, only in the registry.
        std::string WindowsVersion()
        {
            using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
            HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
            OSVERSIONINFOW vi{};
            vi.dwOSVersionInfoSize = sizeof(vi);
            if (const auto rtl = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion")))
                rtl(&vi);
            constexpr const wchar_t* kKey = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
            DWORD ubr = 0, size = sizeof(ubr);
            RegGetValueW(HKEY_LOCAL_MACHINE, kKey, L"UBR", RRF_RT_REG_DWORD, nullptr, &ubr, &size);
            wchar_t release[32]{};
            size = sizeof(release);
            if (RegGetValueW(HKEY_LOCAL_MACHINE, kKey, L"DisplayVersion", RRF_RT_REG_SZ, nullptr, release, &size) != ERROR_SUCCESS)
                release[0] = 0;
            std::string s = std::format("{}.{}.{}.{}", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber, ubr);
            if (release[0]) s += std::format(" ({})", Narrow(release));
            // Wine (Proton, the Steam Deck) answers a Windows version too; ntdll names the real one.
            using WineVersionFn = const char*(__cdecl*)();
            if (const auto wine = reinterpret_cast<WineVersionFn>(GetProcAddress(ntdll, "wine_get_version")))
                s += std::format(", Wine {}", wine());
            return s;
        }

        std::string CpuBrand()
        {
            int regs[4]{};
            __cpuid(regs, static_cast<int>(0x80000000));
            if (static_cast<unsigned>(regs[0]) < 0x80000004u) return "unknown CPU";
            char brand[49]{};
            for (unsigned leaf = 0; leaf < 3; ++leaf) {
                __cpuid(regs, static_cast<int>(0x80000002u + leaf));
                std::memcpy(brand + leaf * 16, regs, 16);
            }
            const std::string s = Trimmed(brand);
            return s.empty() ? "unknown CPU" : s;
        }

        // Physical cores (RelationProcessorCore entries) and logical processors across every group.
        void CpuCounts(unsigned& cores, unsigned& threads)
        {
            cores = 0;
            threads = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
            DWORD len = 0;
            GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
            if (!len) return;
            std::vector<std::byte> buf(len);
            auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data());
            if (!GetLogicalProcessorInformationEx(RelationProcessorCore, info, &len)) return;
            for (DWORD off = 0; off < len;) {
                const auto* e = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
                if (e->Size == 0) break;
                if (e->Relationship == RelationProcessorCore) ++cores;
                off += e->Size;
            }
        }

        std::wstring UserProfile()
        {
            wchar_t buf[MAX_PATH]{};
            const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
            if (n == 0 || n >= MAX_PATH) return {};
            std::wstring s(buf, n);
            while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
            return s;
        }

    }

    std::string ForLog(const std::filesystem::path& p)
    {
        static const std::wstring s_profile = UserProfile();
        std::wstring w = p.native();
        if (!s_profile.empty() && w.size() >= s_profile.size() &&
            CompareStringOrdinal(w.data(), static_cast<int>(s_profile.size()), s_profile.data(),
                                 static_cast<int>(s_profile.size()), TRUE) == CSTR_EQUAL &&
            (w.size() == s_profile.size() || w[s_profile.size()] == L'\\' || w[s_profile.size()] == L'/'))
            w.replace(0, s_profile.size(), L"%USERPROFILE%");
        return Narrow(w);
    }

    std::string RedactProfile(std::string text)
    {
        static const std::string s_profile = Narrow(UserProfile());
        if (s_profile.empty()) return text;
        const auto fold = [](char a, char b) {
            const auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
            return lower(a) == lower(b);
        };
        constexpr std::string_view kToken = "%USERPROFILE%";
        std::size_t from = 0;
        for (;;) {
            const auto it = std::search(text.begin() + static_cast<std::ptrdiff_t>(from), text.end(),
                                        s_profile.begin(), s_profile.end(), fold);
            if (it == text.end()) break;
            const auto at = static_cast<std::size_t>(it - text.begin());
            const std::size_t end = at + s_profile.size();
            if (end == text.size() || text[end] == '\\' || text[end] == '/') {
                text.replace(at, s_profile.size(), kToken);
                from = at + kToken.size();
            } else {
                from = end;
            }
        }
        return text;
    }

    void LogPlatform(const SKSE::LoadInterface* skse)
    {
        const char* flavour = REL::Module::IsVR() ? "VR" : REL::Module::IsAE() ? "AE" : "SE";
        std::string skseVer = "?";
        if (skse) {
            // Packed like the game's own version (8.8.12.4 bits, REL::Version::pack).
            const auto v = REL::Version::unpack(skse->SKSEVersion());
            skseVer = std::format("{}.{}.{}", v.major(), v.minor(), v.patch());
        }
        SKSE::log::info("Magelight: Skyrim {} {}, SKSE {}", flavour, REL::Module::get().version().string("."), skseVer);
        SKSE::log::info("Magelight: Windows {}", WindowsVersion());
        unsigned cores = 0, threads = 0;
        CpuCounts(cores, threads);
        MEMORYSTATUSEX mem{};
        mem.dwLength = sizeof(mem);
        GlobalMemoryStatusEx(&mem);
        SKSE::log::info("Magelight: {} ({} cores, {} threads), {:.1f} GB RAM", CpuBrand(), cores, threads,
                        static_cast<double>(mem.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0));
    }

    void LogGraphicsAdapters()
    {
        wchar_t sys[MAX_PATH]{};
        const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return;
        const std::wstring dxgiPath = std::wstring(sys, n) + L"\\dxgi.dll";
        HMODULE dxgi = LoadLibraryExW(dxgiPath.c_str(), nullptr, 0);
        if (!dxgi) {
            SKSE::log::warn("Magelight: graphics adapters not listed (system dxgi.dll did not load, error {})", GetLastError());
            return;
        }
        using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
        const auto create = reinterpret_cast<CreateFactoryFn>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
        // Under Wine the system dxgi is DXVK, whose driver version is a fixed stand-in, not the host driver's.
        const bool wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
        {   // every COM object is released before FreeLibrary
            Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
            if (!create || FAILED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.GetAddressOf())))) {
                SKSE::log::warn("Magelight: graphics adapters not listed (no DXGI factory)");
            } else {
                // The adapter index the game created its device on (its own DXGI enumeration, same order).
                std::uint32_t gameAdapter = UINT32_MAX;
                if (const auto* data = RE::BSGraphics::Renderer::GetRendererDataSingleton()) gameAdapter = data->uiAdapter;
                Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
                for (UINT i = 0; SUCCEEDED(factory->EnumAdapters1(i, adapter.ReleaseAndGetAddressOf())); ++i) {
                    DXGI_ADAPTER_DESC1 d{};
                    if (FAILED(adapter->GetDesc1(&d)) || (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
                    // The user-mode driver version, four 16-bit parts (32.0.16.1088). NVIDIA's own number is the
                    // last five digits (610.88).
                    std::string driver = "?";
                    LARGE_INTEGER umd{};
                    if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
                        const auto q = static_cast<std::uint64_t>(umd.QuadPart);
                        const unsigned a = (q >> 48) & 0xFFFF, b = (q >> 32) & 0xFFFF, c = (q >> 16) & 0xFFFF, e = q & 0xFFFF;
                        driver = std::format("{}.{}.{}.{}", a, b, c, e);
                        if (wine) {
                            driver += " (as Wine reports it)";
                        } else if (d.VendorId == 0x10DE) {
                            const unsigned nv = (c % 10) * 10000 + e;
                            driver += std::format(" (NVIDIA {}.{:02})", nv / 100, nv % 100);
                        }
                    }
                    SKSE::log::info("Magelight: GPU {} {} [{:04X}:{:04X}], {} MB VRAM, driver {}{}",
                                    i, Narrow(d.Description), d.VendorId, d.DeviceId,
                                    d.DedicatedVideoMemory / (1024 * 1024), driver,
                                    i == gameAdapter ? " - the game's" : "");
                }
            }
        }
        FreeLibrary(dxgi);
    }

}
