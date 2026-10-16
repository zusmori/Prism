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
		items::OnProcessEvent(obj, fn);
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

static LRESULT CALLBACK WndProcHook(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
	if (msg == WM_KEYDOWN && wp == VK_F8 && !(lp & (1 << 30))) {
		menuOpen = !menuOpen;
		if (imguiReady) ImGui::GetIO().MouseDrawCursor = menuOpen;
		return 0;
	}
	if (menuOpen && imguiReady) {
		ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp);
		// Eingaben nicht ans Spiel weitergeben, solange das Menue offen ist
		switch (msg) {
		case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP:
		case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MOUSEWHEEL: case WM_LBUTTONDBLCLK:
		case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR: case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_INPUT:
			if (msg == WM_KEYDOWN && wp == VK_ESCAPE) { menuOpen = false; ImGui::GetIO().MouseDrawCursor = false; }
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
				ImGui_ImplWin32_Init(gameWnd);
				ImGui_ImplDX11_Init(device, context);
				gui::Style();
				wndOrig = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(gameWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProcHook)));
				imguiReady = true;
				zl::Log("overlay ready (window %p)", gameWnd);
			}
		}
		if (imguiReady && menuOpen) {
			ImGui_ImplDX11_NewFrame();
			ImGui_ImplWin32_NewFrame();
			ImGui::NewFrame();
			bool open = true;
			gui::Render(&open);
			if (!open) { menuOpen = false; ImGui::GetIO().MouseDrawCursor = false; }
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
