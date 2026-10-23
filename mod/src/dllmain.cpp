// Prism-Mod: wird vom Prism-Launcher in Rocket League geladen (nur ohne Anti-Cheat).
// F8 oeffnet das Item-Menue.
#include <windows.h>

#include "MinHook.h"
#include "hooks.h"
#include "items.h"
#include "tweaks.h"
#include "log.h"
#include "ue.h"

static DWORD WINAPI Start(LPVOID) {
	zl::Log("Prism mod loaded");
	// warten, bis die Engine ihre Objekte angelegt hat
	bool ok = false;
	for (int i = 0; i < 180 && !ok; i++) {
		ok = ue::Init();
		if (!ok) Sleep(1000);
	}
	if (!ok) {
		zl::Log("ERROR: engine not found, giving up");
		return 0;
	}
	if (MH_Initialize() != MH_OK) {
		zl::Log("ERROR: MinHook init failed");
		return 0;
	}
	items::LoadSaved();
	items::LoadPresets();
	tweaks::Load();
	if (!hooks::HookProcessEvent()) return 0;
	hooks::HookD3D();
	// Klassen suchen, sobald das Spiel so weit ist (im Spiel-Thread)
	for (int i = 0; i < 120 && !items::IsReady(); i++) {
		items::Queue([] { items::Setup(); });
		Sleep(2000);
	}
	zl::Log(items::IsReady() ? "item system ready" : "ERROR: item system not found");
	return 0;
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
	if (reason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(mod);
		if (HANDLE t = CreateThread(nullptr, 0, Start, nullptr, 0, nullptr)) CloseHandle(t);
	}
	return TRUE;
}
