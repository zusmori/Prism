#pragma once
#include <atomic>

struct ID3D11Device;

namespace hooks {
extern std::atomic<bool> menuOpen;
bool HookProcessEvent();
bool HookD3D();
ID3D11Device* Device(); // nach dem Start des Overlays, sonst nullptr
} // namespace hooks
