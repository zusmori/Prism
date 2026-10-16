#pragma once
#include <atomic>

namespace hooks {
extern std::atomic<bool> menuOpen;
bool HookProcessEvent();
bool HookD3D();
} // namespace hooks
