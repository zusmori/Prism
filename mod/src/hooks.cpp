// Hooks: ProcessEvent (Spiel-Thread) und DirectX 11 Present (Overlay mit ImGui)
#include "hooks.h"

#include <atomic>
#include <d3d11.h>
#include <dxgi.h>

#include "MinHook.h"
#include "gui.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "items.h"
#include "log.h"
#include "ue.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace hooks {

std::atomic<bool> menuOpen{false};

// ---------- ProcessEvent ----------
static void __fastcall ProcessEventHook(void* obj, void* fn, void* params, void* result) {
	items::RunQueue();
	try {
		items::OnProcessEvent(obj, fn, params);
	} catch (...) {}
	ue::ProcessEventOrig(obj, fn, params, result);
}

bool HookProcessEvent() {
	void* target = ue::ProcessEventAddress();
	if (!target) {
		zl::Log("ERROR: ProcessEvent not found");
		return false;
	}
	if (MH_CreateHook(target, reinterpret_cast<void*>(&ProcessEventHook), reinterpret_cast<void**>(&ue::ProcessEventOrig)) != MH_OK ||
		MH_EnableHook(target) != MH_OK) {
		zl::Log("ERROR: couldn't hook ProcessEvent");
		return false;
	}
	zl::Log("ProcessEvent hooked at %p", target);
	return true;
}

// ---------- DirectX 11 ----------
using PresentFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
using ResizeFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
static PresentFn presentOrig = nullptr;
static ResizeFn resizeOrig = nullptr;
static ID3D11Device* device = nullptr;
static ID3D11DeviceContext* context = nullptr;
static ID3D11RenderTargetView* rtv = nullptr;
static HWND gameWnd = nullptr;
static WNDPROC wndOrig = nullptr;
static bool imguiReady = false;

static void CreateRTV(IDXGISwapChain* sc) {
	ID3D11Texture2D* back = nullptr;
	if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) && back) {
		device->CreateRenderTargetView(back, nullptr, &rtv);
		back->Release();
	}
}

// ---------- Maus ----------
// Rocket League sperrt im Spiel die Maus (ClipCursor/SetCursorPos) und liest sie nur ueber Raw Input
// (keine normalen Maus-Nachrichten). Solange das Menue offen ist, geben wir die Maus frei und
// berechnen die Position selbst.
using SetCursorPosFn = BOOL(WINAPI*)(int, int);
using ClipCursorFn = BOOL(WINAPI*)(const RECT*);
static SetCursorPosFn setCursorPosOrig = nullptr;
static ClipCursorFn clipCursorOrig = nullptr;
static std::atomic<long> rawDX{0}, rawDY{0}, rawWheel{0};
static POINT lastSys{-100000, -100000};
static float vmX = 0, vmY = 0;

static BOOL WINAPI SetCursorPosHook(int x, int y) {
	if (menuOpen) return TRUE; // Spiel darf die Maus nicht zuruecksetzen
	return setCursorPosOrig(x, y);
}
static BOOL WINAPI ClipCursorHook(const RECT* r) {
	if (menuOpen) return clipCursorOrig(nullptr);
	return clipCursorOrig(r);
}

static void HookCursor() {
	HMODULE u = GetModuleHandleW(L"user32.dll");
	void* scp = u ? reinterpret_cast<void*>(GetProcAddress(u, "SetCursorPos")) : nullptr;
	void* cc = u ? reinterpret_cast<void*>(GetProcAddress(u, "ClipCursor")) : nullptr;
	bool ok = scp && cc &&
		MH_CreateHook(scp, reinterpret_cast<void*>(&SetCursorPosHook), reinterpret_cast<void**>(&setCursorPosOrig)) == MH_OK &&
		MH_CreateHook(cc, reinterpret_cast<void*>(&ClipCursorHook), reinterpret_cast<void**>(&clipCursorOrig)) == MH_OK &&
		MH_EnableHook(scp) == MH_OK && MH_EnableHook(cc) == MH_OK;
	zl::Log(ok ? "cursor hooks ready" : "WARNING: cursor hooks failed");
}

// Mausrad: kommt im Spiel weder als normale Nachricht noch zuverlaessig als Raw Input an.
// Ein Low-Level-Maus-Hook (eigener Thread) sieht es immer.
static HHOOK llHook = nullptr;
static LRESULT CALLBACK LowLevelMouse(int code, WPARAM wp, LPARAM lp) {
	if (code == HC_ACTION && wp == WM_MOUSEWHEEL && menuOpen && GetForegroundWindow() == gameWnd) {
		auto* m = reinterpret_cast<MSLLHOOKSTRUCT*>(lp);
		rawWheel += short(HIWORD(m->mouseData));
		return 1; // Spiel bekommt das Rad nicht, solange das Menue offen ist
	}
	return CallNextHookEx(llHook, code, wp, lp);
}
static void StartWheelHook() {
	CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
		HMODULE self = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&LowLevelMouse), &self);
		llHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouse, self, 0);
		zl::Log(llHook ? "wheel hook ready" : "WARNING: wheel hook failed (%lu)", GetLastError());
		MSG msg;
		while (GetMessageW(&msg, nullptr, 0, 0) > 0) {}
		return 0;
	}, nullptr, 0, nullptr);
}

static void ResetKeys();
static void OnMenuToggled() {
	if (!imguiReady) return;
	ImGuiIO& io = ImGui::GetIO();
	io.MouseDrawCursor = menuOpen;
	gui::SetOpen(menuOpen);
	if (menuOpen) {
		if (clipCursorOrig) clipCursorOrig(nullptr); else ClipCursor(nullptr);
		RECT rc{};
		GetClientRect(gameWnd, &rc);
		vmX = float(rc.right) / 2;
		vmY = float(rc.bottom) / 2;
		lastSys = {-100000, -100000};
		rawDX = 0; rawDY = 0; rawWheel = 0;
		io.AddMousePosEvent(vmX, vmY);
		ResetKeys();
		items::OnMenuOpened();
	} else {
		for (int b = 0; b < 3; b++) io.AddMouseButtonEvent(b, false);
	}
}

// Jede Frame: Mausposition und Tasten selbst abfragen
static void PollMouse() {
	ImGuiIO& io = ImGui::GetIO();
	if (clipCursorOrig) clipCursorOrig(nullptr);
	RECT rc{};
	GetClientRect(gameWnd, &rc);
	POINT p{};
	long dx = rawDX.exchange(0), dy = rawDY.exchange(0);
	if (GetCursorPos(&p) && ScreenToClient(gameWnd, &p) && (p.x != lastSys.x || p.y != lastSys.y) && lastSys.x != -100000) {
		vmX = float(p.x); // echte Maus bewegt sich: die nehmen
		vmY = float(p.y);
	} else {
		vmX += float(dx); // sonst aus den Raw-Input-Bewegungen rechnen
		vmY += float(dy);
	}
	lastSys = p;
	if (vmX < 0) vmX = 0;
	if (vmY < 0) vmY = 0;
	if (vmX > rc.right - 1) vmX = float(rc.right - 1);
	if (vmY > rc.bottom - 1) vmY = float(rc.bottom - 1);
	io.AddMousePosEvent(vmX, vmY);
	// Im Menue zeigt das Spiel seinen eigenen Mauszeiger - dann keinen zweiten zeichnen
	io.MouseDrawCursor = items::InMatch();
	bool fg = GetForegroundWindow() == gameWnd;
	io.AddMouseButtonEvent(0, fg && (GetAsyncKeyState(VK_LBUTTON) & 0x8000));
	io.AddMouseButtonEvent(1, fg && (GetAsyncKeyState(VK_RBUTTON) & 0x8000));
	io.AddMouseButtonEvent(2, fg && (GetAsyncKeyState(VK_MBUTTON) & 0x8000));
	long w = rawWheel.exchange(0);
	if (w) io.AddMouseWheelEvent(0, float(w) / WHEEL_DELTA);
}

// Tastatur: im Spiel kommen nicht zuverlaessig Tasten-/Zeichen-Nachrichten an, daher selbst abfragen
static bool keyPrev[256];
static ImGuiKey MapKey(int vk) {
	switch (vk) {
	case VK_BACK: return ImGuiKey_Backspace;
	case VK_DELETE: return ImGuiKey_Delete;
	case VK_LEFT: return ImGuiKey_LeftArrow;
	case VK_RIGHT: return ImGuiKey_RightArrow;
	case VK_UP: return ImGuiKey_UpArrow;
	case VK_DOWN: return ImGuiKey_DownArrow;
	case VK_HOME: return ImGuiKey_Home;
	case VK_END: return ImGuiKey_End;
	case VK_PRIOR: return ImGuiKey_PageUp;
	case VK_NEXT: return ImGuiKey_PageDown;
	case VK_RETURN: return ImGuiKey_Enter;
	case VK_TAB: return ImGuiKey_Tab;
	case VK_ESCAPE: return ImGuiKey_Escape;
	case 'A': return ImGuiKey_A;
	case 'C': return ImGuiKey_C;
	case 'V': return ImGuiKey_V;
	case 'X': return ImGuiKey_X;
	case 'Y': return ImGuiKey_Y;
	case 'Z': return ImGuiKey_Z;
	default: return ImGuiKey_None;
	}
}

static void ResetKeys() {
	for (int vk = 0; vk < 256; vk++) keyPrev[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0; // schon gedrueckte Tasten (F8) ignorieren
}

static void PollKeys() {
	ImGuiIO& io = ImGui::GetIO();
	if (GetForegroundWindow() != gameWnd) return;
	BYTE ks[256] = {0};
	bool down[256];
	for (int vk = 1; vk < 256; vk++) {
		down[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
		if (down[vk]) ks[vk] = 0x80;
	}
	if (GetKeyState(VK_CAPITAL) & 1) ks[VK_CAPITAL] |= 1;
	bool ctrl = down[VK_CONTROL], shift = down[VK_SHIFT], alt = down[VK_MENU];
	io.AddKeyEvent(ImGuiMod_Ctrl, ctrl);
	io.AddKeyEvent(ImGuiMod_Shift, shift);
	io.AddKeyEvent(ImGuiMod_Alt, alt);
	for (int vk = 8; vk < 256; vk++) {
		if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || (vk >= VK_LBUTTON && vk <= VK_XBUTTON2)) continue;
		bool d = down[vk];
		if (d == keyPrev[vk]) continue;
		keyPrev[vk] = d;
		ImGuiKey k = MapKey(vk);
		if (k != ImGuiKey_None) io.AddKeyEvent(k, d);
		if (d && vk == VK_ESCAPE) { menuOpen = false; return; }
		// Zeichen (AltGr = Strg+Alt erlaubt, z. B. @)
		if (d && (!ctrl || alt)) {
			wchar_t buf[4] = {0};
			UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
			if (ToUnicode(vk, sc, ks, buf, 4, 0x4) == 1 && buf[0] >= 32) io.AddInputCharacterUTF16(buf[0]);
		}
	}
}

static void ReadRawMouse(LPARAM lp) {
	RAWINPUT ri{};
	UINT size = sizeof ri;
	if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) == UINT(-1)) return;
	if (ri.header.dwType != RIM_TYPEMOUSE) return;
	const RAWMOUSE& m = ri.data.mouse;
	if (!(m.usFlags & MOUSE_MOVE_ABSOLUTE)) {
		rawDX += m.lLastX;
		rawDY += m.lLastY;
	}
}

static LRESULT CALLBACK WndProcHook(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
	if (msg == WM_KEYDOWN && wp == VK_F8 && !(lp & (1 << 30))) {
		menuOpen = !menuOpen;
		OnMenuToggled();
		return 0;
	}
	if (menuOpen && imguiReady) {
		if (msg == WM_INPUT) ReadRawMouse(lp);
		// Maustasten und -position kommen aus PollMouse, nur Tastatur/Rad hier
		bool mouseMsg = (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_SETCURSOR; // alles aus PollMouse
		bool keyMsg = (msg >= WM_KEYFIRST && msg <= WM_KEYLAST);
		// Nichts an ImGui weitergeben: Maus und Tastatur fragen wir selbst ab. Fokus-Nachrichten des Spiels
		// (z. B. in der Garage) wuerden sonst das Suchfeld sofort wieder deaktivieren.
		(void)mouseMsg; (void)keyMsg;
		// Eingaben nicht ans Spiel weitergeben, solange das Menue offen ist
		switch (msg) {
		case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP:
		case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MOUSEWHEEL: case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK:
		case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR: case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_INPUT:
			return msg == WM_INPUT ? DefWindowProcW(h, msg, wp, lp) : 0;
		}
	}
	return CallWindowProcW(wndOrig, h, msg, wp, lp);
}

static HRESULT __stdcall PresentHook(IDXGISwapChain* sc, UINT sync, UINT flags) {
	try {
		if (!imguiReady) {
			if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device)))) {
				device->GetImmediateContext(&context);
				DXGI_SWAP_CHAIN_DESC d{};
				sc->GetDesc(&d);
				gameWnd = d.OutputWindow;
				CreateRTV(sc);
				ImGui::CreateContext();
				ImGui::GetIO().IniFilename = nullptr;
				gui::Fonts();
				ImGui_ImplWin32_Init(gameWnd);
				ImGui_ImplDX11_Init(device, context);
				gui::Style();
				wndOrig = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(gameWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProcHook)));
				imguiReady = true;
				ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
				HookCursor();
				StartWheelHook();
				zl::Log("overlay ready (window %p)", gameWnd);
			}
		}
		// Hinweis beim Spielstart: kurz einblenden, nicht anklickbar
		static unsigned long long toastStart = 0;
		if (imguiReady && !toastStart) toastStart = GetTickCount64() + 1500;
		float toastT = toastStart ? float((long long)(GetTickCount64() - toastStart)) / 1000.0f : -1.0f;
		bool toast = toastT >= 0.0f && toastT < gui::kToastSeconds;
		if (imguiReady && (menuOpen || toast || gui::Visible())) {
			ImGui_ImplDX11_NewFrame();
			ImGui_ImplWin32_NewFrame();
			if (menuOpen) {
				PollMouse();
				PollKeys();
				if (!menuOpen) { OnMenuToggled(); return presentOrig(sc, sync, flags); }
			} else {
				ImGui::GetIO().MouseDrawCursor = false;
			}
			ImGui::NewFrame();
			if (gui::Visible()) {
				bool open = true;
				gui::Render(&open);
				if (!open && menuOpen) { menuOpen = false; OnMenuToggled(); }
			}
			if (toast) gui::Toast(toastT);
			ImGui::Render();
			if (!rtv) CreateRTV(sc);
			context->OMSetRenderTargets(1, &rtv, nullptr);
			ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
		}
	} catch (...) {
		zl::Log("ERROR: overlay crashed (caught)");
	}
	return presentOrig(sc, sync, flags);
}

static HRESULT __stdcall ResizeHook(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl) {
	if (rtv) { rtv->Release(); rtv = nullptr; }
	HRESULT r = resizeOrig(sc, n, w, h, f, fl);
	if (imguiReady) CreateRTV(sc);
	return r;
}

bool HookD3D() {
	// Dummy-Fenster + Geraet anlegen, nur um die Adressen von Present/ResizeBuffers zu bekommen
	WNDCLASSEXW wc{sizeof wc, CS_CLASSDC, DefWindowProcW, 0, 0, GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr, L"ZLModDummy", nullptr};
	RegisterClassExW(&wc);
	HWND wnd = CreateWindowW(L"ZLModDummy", L"", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
	DXGI_SWAP_CHAIN_DESC sd{};
	sd.BufferCount = 1;
	sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.OutputWindow = wnd;
	sd.SampleDesc.Count = 1;
	sd.Windowed = TRUE;
	sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
	IDXGISwapChain* sc = nullptr;
	ID3D11Device* dev = nullptr;
	ID3D11DeviceContext* ctx = nullptr;
	D3D_FEATURE_LEVEL fl;
	HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
	if (FAILED(hr)) hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
	if (FAILED(hr) || !sc) {
		zl::Log("ERROR: dummy D3D11 device failed (%08x)", (unsigned)hr);
		DestroyWindow(wnd);
		UnregisterClassW(L"ZLModDummy", wc.hInstance);
		return false;
	}
	void** vt = *reinterpret_cast<void***>(sc);
	void* present = vt[8];
	void* resize = vt[13];
	sc->Release();
	dev->Release();
	ctx->Release();
	DestroyWindow(wnd);
	UnregisterClassW(L"ZLModDummy", wc.hInstance);
	if (MH_CreateHook(present, reinterpret_cast<void*>(&PresentHook), reinterpret_cast<void**>(&presentOrig)) != MH_OK ||
		MH_CreateHook(resize, reinterpret_cast<void*>(&ResizeHook), reinterpret_cast<void**>(&resizeOrig)) != MH_OK ||
		MH_EnableHook(present) != MH_OK || MH_EnableHook(resize) != MH_OK) {
		zl::Log("ERROR: couldn't hook Present");
		return false;
	}
	zl::Log("Present hooked at %p", present);
	return true;
}

} // namespace hooks
