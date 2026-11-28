#pragma once
#include <Ultralight/Ultralight.h>

#include <filesystem>

// Ultralight's platform: files from the mods folder and the plugin's resources only, a logger, the renderer.
namespace mlcp::platform {

// file:///<ModId>/<path> reads <modsRoot>/<ModId>/<path>; resources/ reads the plugin's own resources folder.
ultralight::RefPtr<ultralight::Renderer> CreateRenderer(const std::filesystem::path& pluginDir,
                                                        const std::filesystem::path& modsRoot);

// Only file:// requests leave a page: Magelight's default network policy ("file").
ultralight::NetworkListener* FileOnly();
}
