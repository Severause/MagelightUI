#pragma once
#include "Input.h"
#include "Views.h"

#include <d3d12.h>
#include <dxgi1_4.h>

#include <filesystem>
#include <map>
#include <vector>

// Our own D3D12 work at the end of the game's frame: upload the pages that changed, draw every shown one into the
// backbuffer in layer order, then the cursor. The game presents from more than one thread, so the caller serialises
// Draw; the backbuffer arrives in PRESENT state.
namespace mlcp::gfx {
// Test switch: every 600 draws, the part of the backbuffer the pages cover is read back after our draw and written to
// `bmp`, so a run can see what reached the screen. Off unless called.
void EnableSnapshots(const std::filesystem::path& bmp);

// False when the frame was skipped before `changed` was uploaded: the caller hands the same frames again next time.
bool Draw(IDXGISwapChain3* swapChain, ID3D12CommandQueue* queue, const std::vector<views::Row>& rows,
          const std::map<int, views::Pixels>& changed, const input::Cursor& cursor);
}
