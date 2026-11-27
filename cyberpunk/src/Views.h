#pragma once
#include "Manifest.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

// Every page, on one thread of its own (WebKit is one thread's). Other threads see the views only through what this
// header publishes: the layout, the frames, and the commands they queue. Nothing here is ever torn down: the renderer's
// destructor joins its threads, which deadlocks on the loader lock at unload.
namespace mlcp::views {

struct Pixels {
    const std::uint8_t* data = nullptr;   // BGRA, premultiplied
    std::uint32_t width = 0, height = 0, rowBytes = 0;
};

// One view as the input and the compositor see it, in backbuffer pixels.
struct Row {
    int id = 0;
    std::string mod, name;
    int x = 0, y = 0, w = 0, h = 0;
    double scale = 1.0;          // backbuffer pixels per page pixel
    int layer = 1;
    bool visible = false;
    bool clickThrough = false;   // never takes the pointer or UI mode
    std::uint32_t hotkey = 0;    // DirectInput scancode
};

// A message a page sent with window.magelight.send.
struct Message {
    std::string mod, view, channel, payload;
};

void Start(const std::filesystem::path& pluginDir, const std::filesystem::path& modsRoot,
           std::vector<manifest::ViewSpec> specs);
void Stop();

// The backbuffer size, from the presenting thread; views are laid out for it (1080p logical pixels, scaled).
void SetScreen(int width, int height);

// Drawing order: layer, then creation.
std::vector<Row> Layout();

// The view a mod and view name name (the mod case-insensitive, as Magelight's slugs), 0 = none.
int IdOf(const std::string& mod, const std::string& view);

// True once the page thread made its views.
bool Started();

// Frames that changed since the last take, swapped into `buffers[id]` and set in `out` (entries already there stay
// unless replaced, so a frame not yet drawn is kept); `out[id].data` points into it. Never waits.
void TakeFrames(std::map<int, std::vector<std::uint8_t>>& buffers, std::map<int, Pixels>& out);

// Commands, from any thread; applied on the page thread.
void Show(int id, bool visible);
void Focus(int id, bool focused);   // focused also dispatches '__uimode' '1' / '0' to the page
void Send(int id, const std::string& channel, const std::string& text);
void Reload(int id);
void TakeMessages(std::vector<Message>& out);   // pages' messages and the host's '__ready' / '__uimode'
}
