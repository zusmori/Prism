// F8-Menue im Prism-Stil: links die Slots, rechts die Auswahl fuer den Slot.
// Alles selbst gezeichnet, mit weichen Uebergaengen (Oeffnen/Schliessen, Hover, Auswahl, Scrollen).
#include "gui.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "imgui.h"
#include "imgui_internal.h"
#include "items.h"
#include "log.h"
#include "maps.h"
#include "tweaks.h"
#include "customs.h"
#include "merge.h"

namespace gui {

// ---------- Farben (wie im Launcher) ----------
static ImVec4 accent(124 / 255.0f, 108 / 255.0f, 252 / 255.0f, 1.0f);
static ImVec4 spec[3] = {ImVec4(167 / 255.0f, 139 / 255.0f, 250 / 255.0f, 1), ImVec4(96 / 255.0f, 165 / 255.0f, 250 / 255.0f, 1), ImVec4(34 / 255.0f, 211 / 255.0f, 238 / 255.0f, 1)}; // Prisma-Spektrum
static ImFont *fRegular = nullptr, *fSmall = nullptr, *fBold = nullptr, *fTitle = nullptr, *fTiny = nullptr;

static ImVec4 Rgb(int r, int g, int b, float a = 1.0f) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a); }
static ImVec4 Hex(unsigned v, float a = 1.0f) { return Rgb((v >> 16) & 255, (v >> 8) & 255, v & 255, a); }
static ImU32 U(ImVec4 c) { return ImGui::GetColorU32(c); }
static ImVec4 Acc(float a) { return ImVec4(accent.x, accent.y, accent.z, a); }
static ImVec4 Mix(ImVec4 a, ImVec4 b, float t) { return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t); }
static ImVec4 Alpha(ImVec4 c, float a) { c.w *= a; return c; }

// abgerundete Flaeche mit Farbverlauf (diagonal von c0 nach c1)
static void GradRect(ImDrawList* d, ImVec2 a, ImVec2 b, float r, ImVec4 c0, ImVec4 c1, float alpha) {
	int v0 = d->VtxBuffer.Size;
	d->AddRectFilled(a, b, U(Rgb(255, 255, 255, alpha)), r);
	ImGui::ShadeVertsLinearColorGradientKeepAlpha(d, v0, d->VtxBuffer.Size, a, b, U(ImVec4(c0.x, c0.y, c0.z, 1)), U(ImVec4(c1.x, c1.y, c1.z, 1)));
}

static const ImVec4 cBg = Hex(0x0c0c0f), cPanel = Hex(0x111115), cPanel2 = Hex(0x15151a), cHover = Hex(0x1a1a20);
static const ImVec4 cLine = Hex(0x1f1f26), cLine2 = Hex(0x2a2a33);
static const ImVec4 cText = Hex(0xececf1), cMuted = Hex(0x8b8b97), cFaint = Hex(0x55555f);

// ---------- Animation ----------
// Abgerundete Flaeche mit weichem Verlauf der Deckkraft (statt AddRectFilledMultiColor, das ist eckig
// und guckt an runden Ecken heraus). fade(x, y) bekommt 0..1 ueber die Flaeche und liefert 0..1.
template <class F>
static void FadeRect(ImDrawList* d, ImVec2 a, ImVec2 b, float r, ImDrawFlags fl, ImVec4 c, F fade) {
	int v0 = d->VtxBuffer.Size;
	d->AddRectFilled(a, b, ImGui::GetColorU32(c), r, fl);
	float w = std::max(1.0f, b.x - a.x), h = std::max(1.0f, b.y - a.y);
	for (int i = v0; i < d->VtxBuffer.Size; i++) {
		ImDrawVert& v = d->VtxBuffer[i];
		float t = std::clamp(fade((v.pos.x - a.x) / w, (v.pos.y - a.y) / h), 0.0f, 1.0f);
		unsigned al = (v.col >> 24) & 0xFF;
		v.col = (v.col & 0x00FFFFFF) | (unsigned(al * t + 0.5f) << 24);
	}
}

static float Dt() { return std::min(ImGui::GetIO().DeltaTime, 0.05f); }
static float Approach(float cur, float target, float speed) { return cur + (target - cur) * (1.0f - expf(-speed * Dt())); }
static float EaseOut(float t) { t = std::clamp(t, 0.0f, 1.0f); return 1.0f - powf(1.0f - t, 4.0f); }
// Pro Element gemerkter Wert (z. B. Hover 0..1), gleitet zum Ziel
static float Anim(ImGuiID id, float target, float speed = 18.0f) {
	float* v = ImGui::GetStateStorage()->GetFloatRef(id, target);
	*v = Approach(*v, target, speed);
	if (fabsf(*v - target) < 0.002f) *v = target;
	return *v;
}

// ---------- Zustand ----------
static int curSlot = 0;
static char search[64] = {0};
static bool requested = false;
static bool focusSearch = false;
static bool wantOpen = false;
static float vis = 0.0f;          // 0 = zu, 1 = offen (animiert)
static float listFade = 1.0f;     // Liste nach Slot-Wechsel einblenden
static float scrollTarget = 0.0f, scrollLast = -1.0f;
static float indicatorY = -1.0f;  // gleitende Markierung links
static int tab = 0;               // 0 = Items, 1 = Presets
// Zuletzt benutzter Reiter: einer fuers Hauptmenue, einer fuers Freeplay/Training (bleibt auch nach Neustart)
static int tabMenu = 0, tabFp = 0;
static bool tabLoaded = false;
static void LoadTabs() {
	tabLoaded = true;
	FILE* f = _wfopen((zl::DataDir() + L"\\menu_tab.txt").c_str(), L"r");
	if (!f) return;
	int a = 0, b = 0;
	if (fscanf(f, "%d %d", &a, &b) == 2) {
		if (a >= 0 && a <= 5 && a != 4) tabMenu = a;
		if (b >= 0 && b <= 4 && b != 3) tabFp = b;
	}
	fclose(f);
}
static void SaveTabs() {
	FILE* f = _wfopen((zl::DataDir() + L"\\menu_tab.txt").c_str(), L"w");
	if (!f) return;
	fprintf(f, "%d %d\n", tabMenu, tabFp);
	fclose(f);
}
static float tabX = -1.0f, tabW = 0.0f, tabFade = 1.0f;
static char presetName[48] = {0};
static bool focusName = false;
static int pendingDelete = -1;
static double pendingDeleteAt = 0, savedAt = -10;
static float pScrollTarget = 0.0f, pScrollLast = -1.0f;
static int renameIdx = -1;        // Preset, dessen Name gerade geaendert wird
static char renameBuf[48] = {0};
static bool focusRename = false;
static int pendingUpdate = -1, savedIdx = -1;
static double pendingUpdateAt = 0, savedIdxAt = -10;
static bool resetArmed = false, doReset = false;
static double resetArmedAt = 0;

static const char* kQuality[] = {"Common", "Uncommon", "Rare", "Very Rare", "Import", "Exotic", "Black Market", "Premium", "Limited", "Legacy"};
static ImVec4 QualityColor(int q) {
	switch (q) {
	case 1: return Rgb(110, 180, 255);
	case 2: return Rgb(90, 140, 255);
	case 3: return Rgb(160, 100, 255);
	case 4: return Rgb(235, 85, 85);
	case 5: return Rgb(240, 200, 60);
	case 6: return Rgb(240, 90, 200);
	case 7: return Rgb(90, 220, 140);
	case 8: return Rgb(250, 150, 50);
	case 9: return Rgb(200, 200, 120);
	default: return Rgb(150, 150, 162);
	}
}

static void ReadTheme() {
	// Standard: Prism-Look (wie im Launcher); andere Farbthemen mit eigenem Spektrum
	auto set = [](int r, int g, int b, int a0, int a1, int a2, int b0, int b1, int b2, int c0, int c1, int c2) {
		accent = Rgb(r, g, b);
		spec[0] = Rgb(a0, a1, a2); spec[1] = Rgb(b0, b1, b2); spec[2] = Rgb(c0, c1, c2);
	};
	set(124, 108, 252, 167, 139, 250, 96, 165, 250, 34, 211, 238);
	FILE* f = _wfopen((zl::DataDir() + L"\\settings.json").c_str(), L"r");
	if (!f) return;
	char buf[16384] = {0};
	fread(buf, 1, sizeof buf - 1, f);
	fclose(f);
	std::string s;
	for (char ch : std::string(buf)) if (ch != ' ' && ch != '\n' && ch != '\r' && ch != '\t') s += ch; // Launcher speichert eingerueckt
	if (s.find("\"theme\":\"purple\"") != std::string::npos) set(139, 92, 246, 129, 140, 248, 167, 139, 250, 232, 121, 249);
	else if (s.find("\"theme\":\"cyan\"") != std::string::npos) set(6, 182, 212, 96, 165, 250, 34, 211, 238, 45, 212, 191);
	else if (s.find("\"theme\":\"green\"") != std::string::npos) set(16, 185, 129, 34, 211, 238, 52, 211, 153, 163, 230, 53);
}

// ---------- Menue-Taste (im Launcher einstellbar, Standard F8) ----------
static std::mutex keyMu;
static std::atomic<int> menuVk{VK_F8};
static std::string keyName = "F8";
static unsigned long long keyReadAt = 0;

static std::string NameOfKey(int vk) {
	if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
	UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
	switch (vk) { // Tasten mit "erweitertem" Scancode
	case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
	case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_DIVIDE: case VK_NUMLOCK:
		sc |= 0x100;
	}
	wchar_t w[64] = {0};
	if (sc && GetKeyNameTextW(LONG(sc << 16), w, 64) > 0) {
		char b[128] = {0};
		WideCharToMultiByte(CP_UTF8, 0, w, -1, b, sizeof b, nullptr, nullptr);
		std::string n(b);
		if (n.size() == 1) n[0] = (char)toupper((unsigned char)n[0]);
		if (!n.empty()) return n;
	}
	char b[16];
	snprintf(b, sizeof b, "Key %d", vk);
	return b;
}

static void ReadKey() {
	int vk = VK_F8;
	FILE* f = _wfopen((zl::DataDir() + L"\\settings.json").c_str(), L"r");
	if (f) {
		char buf[16384] = {0};
		fread(buf, 1, sizeof buf - 1, f);
		fclose(f);
		std::string s;
		for (char ch : std::string(buf)) if (ch != ' ' && ch != '\n' && ch != '\r' && ch != '\t') s += ch;
		size_t p = s.find("\"menuKey\":");
		if (p != std::string::npos) {
			int v = atoi(s.c_str() + p + 10);
			// keine Tasten, die man zum Spielen oder Tippen braucht
			if (v > 7 && v < 255 && v != VK_ESCAPE && v != VK_RETURN && v != VK_TAB && v != VK_SPACE && v != VK_BACK && v != VK_SHIFT &&
				v != VK_CONTROL && v != VK_MENU && v != VK_LWIN && v != VK_RWIN && v != VK_CAPITAL)
				vk = v;
		}
	}
	std::string n = NameOfKey(vk);
	std::lock_guard<std::mutex> lk(keyMu);
	if (vk != menuVk.load()) zl::Log("menu key: %s (%d)", n.c_str(), vk);
	menuVk = vk;
	keyName = n;
}

int MenuKey() {
	unsigned long long now = GetTickCount64();
	if (now - keyReadAt > 1500) { keyReadAt = now; ReadKey(); }
	return menuVk.load();
}
std::string MenuKeyName() {
	MenuKey();
	std::lock_guard<std::mutex> lk(keyMu);
	return keyName;
}

void Fonts() {
	ImGuiIO& io = ImGui::GetIO();
	static const ImWchar ranges[] = {0x0020, 0x00FF, 0x0100, 0x017F, 0x2010, 0x2027, 0x2122, 0x2122, 0};
	auto load = [&](const char* file, float size) -> ImFont* {
		char path[MAX_PATH];
		snprintf(path, sizeof path, "C:\\Windows\\Fonts\\%s", file);
		if (FILE* f = fopen(path, "rb")) { fclose(f); return io.Fonts->AddFontFromFileTTF(path, size, nullptr, ranges); }
		return nullptr;
	};
	fRegular = load("segoeui.ttf", 15.5f);
	if (!fRegular) fRegular = io.Fonts->AddFontDefault();
	fSmall = load("segoeui.ttf", 13.0f);
	fTiny = load("seguisb.ttf", 11.0f);
	fBold = load("seguisb.ttf", 15.5f);
	fTitle = load("seguisb.ttf", 21.0f);
	if (!fSmall) fSmall = fRegular;
	if (!fTiny) fTiny = fSmall;
	if (!fBold) fBold = fRegular;
	if (!fTitle) fTitle = fBold;
}

void Style() {
	ReadTheme();
	ImGuiStyle& s = ImGui::GetStyle();
	s.WindowRounding = 20; s.ChildRounding = 14; s.FrameRounding = 10; s.GrabRounding = 6; s.ScrollbarRounding = 6; s.PopupRounding = 10;
	s.WindowPadding = ImVec2(0, 0); s.FramePadding = ImVec2(0, 11); s.ItemSpacing = ImVec2(0, 2);
	s.WindowBorderSize = 0; s.ChildBorderSize = 0; s.FrameBorderSize = 0; s.ScrollbarSize = 10;
	ImVec4* c = s.Colors;
	c[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0); // Hintergrund zeichnen wir selbst
	c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
	c[ImGuiCol_Border] = cLine;
	c[ImGuiCol_FrameBg] = ImVec4(0, 0, 0, 0);
	c[ImGuiCol_FrameBgHovered] = ImVec4(0, 0, 0, 0);
	c[ImGuiCol_FrameBgActive] = ImVec4(0, 0, 0, 0);
	c[ImGuiCol_Text] = cText;
	c[ImGuiCol_TextDisabled] = cFaint;
	c[ImGuiCol_TextSelectedBg] = Acc(0.35f);
	c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
	c[ImGuiCol_ScrollbarGrab] = Rgb(255, 255, 255, 0.10f);
	c[ImGuiCol_ScrollbarGrabHovered] = Rgb(255, 255, 255, 0.18f);
	c[ImGuiCol_ScrollbarGrabActive] = Acc(0.6f);
}

static double logoOpenFx = -1;
void SetOpen(bool open) {
	wantOpen = open;
	if (open) { focusSearch = false; ReadTheme(); logoOpenFx = 0.25; if (tab == 3) maps::Refresh(); } // Lichtbrechung kurz nach dem Oeffnen
}
bool Visible() { return wantOpen || vis > 0.001f; }

static std::string Lower(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return s;
}

static const items::Item* FindItem(int id) {
	for (auto& it : items::list)
		if (it.id == id) return &it;
	return nullptr;
}

static std::string Fit(ImFont* f, float size, std::string s, float maxw) {
	if (f->CalcTextSizeA(size, FLT_MAX, 0, s.c_str()).x <= maxw) return s;
	while (s.size() > 1 && f->CalcTextSizeA(size, FLT_MAX, 0, (s + "…").c_str()).x > maxw) s.pop_back();
	return s + "…";
}

static void Text(ImDrawList* d, ImFont* f, ImVec2 p, ImVec4 c, const char* t) { d->AddText(f, f->FontSize, p, U(c), t); }
static float TextW(ImFont* f, const char* t) { return f->CalcTextSizeA(f->FontSize, FLT_MAX, 0, t).x; }

static void CheckMark(ImDrawList* d, ImVec2 c, float a) {
	d->AddCircleFilled(c, 9, U(Acc(a)), 24);
	ImU32 w = U(Rgb(255, 255, 255, a));
	d->AddLine(ImVec2(c.x - 4, c.y + 0.5f), ImVec2(c.x - 1, c.y + 3.5f), w, 1.8f);
	d->AddLine(ImVec2(c.x - 1, c.y + 3.5f), ImVec2(c.x + 4.5f, c.y - 3), w, 1.8f);
}

static void Spinner(ImDrawList* d, ImVec2 c, float r) {
	float t = (float)ImGui::GetTime();
	float a0 = t * 5.0f, a1 = a0 + 1.6f + sinf(t * 2.2f) * 0.8f;
	d->AddCircle(c, r, U(Rgb(255, 255, 255, 0.07f)), 40, 2.5f);
	d->PathArcTo(c, r, a0, a1, 32);
	d->PathStroke(U(accent), 0, 2.5f);
}

// Name des gewaehlten eigenen Decals/Balls ("" = keins)
static std::string CustomName(bool ball) {
	std::lock_guard<std::mutex> lk(customs::mu);
	const std::string& sel = ball ? customs::selBall : customs::selDecal;
	for (auto& c : customs::list) if (c.ball == ball && c.id == sel) return c.name;
	return "";
}
constexpr int kBallSlot = 100; // eigener Eintrag "Ball" (nur fuer eigene Baelle)

// ---------- Links: ein Slot ----------
static void SlotRow(const items::SlotInfo& s, ImVec2 size, float& selY) {
	ImGui::PushID(s.index);
	ImVec2 p = ImGui::GetCursorScreenPos();
	bool click = ImGui::InvisibleButton("##s", size);
	bool sel = curSlot == s.index;
	float h = Anim(ImGui::GetID("h"), ImGui::IsItemHovered() && !sel ? 1.0f : 0.0f);
	if (click && !sel) {
		curSlot = s.index;
		search[0] = 0;
		listFade = 0.0f;
		scrollTarget = 0.0f;
		scrollLast = -1.0f;
	}
	if (sel) selY = p.y;
	ImDrawList* d = ImGui::GetWindowDrawList();
	if (h > 0.01f) d->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), U(Alpha(cHover, h)), 12);

	auto ov = items::overrides.find(s.index);
	const items::Item* it = ov != items::overrides.end() ? FindItem(ov->second) : nullptr;
	// Punkt in der Seltenheitsfarbe, wenn ein Item gewaehlt ist
	float tx = p.x + 16;
	const items::Item* shownIt = it;
	if (!shownIt) {
		auto ow = items::ownItems.find(s.index);
		if (ow != items::ownItems.end()) shownIt = FindItem(ow->second);
	}
	if (it) d->AddCircleFilled(ImVec2(p.x + size.x - 18, p.y + size.y / 2), 3.5f, U(accent), 16); // gewaehlt: Punkt in Lila
	// Lackierung (bei gewaehltem oder eigenem Item), nur wenn es sie fuer das Item gibt
	auto pp = items::paints.find(s.index);
	if (shownIt && pp != items::paints.end() && pp->second > 0 && pp->second < 32 && ((shownIt->paintMask >> pp->second) & 1))
		for (auto& pi : items::paintList)
			if (pi.id == pp->second) {
				float px2 = p.x + size.x - (it ? 34 : 20);
				d->AddCircleFilled(ImVec2(px2, p.y + size.y / 2), 6.0f, U(ImVec4(pi.rgb[0], pi.rgb[1], pi.rgb[2], 1.0f)), 16);
				d->AddCircle(ImVec2(px2, p.y + size.y / 2), 6.0f, U(Rgb(255, 255, 255, 0.15f)), 16, 1.0f);
			}
	float sa = Anim(ImGui::GetID("sa"), sel ? 1.0f : 0.0f);
	Text(d, fTiny, ImVec2(tx, p.y + 8), Mix(cFaint, Acc(1.0f), sa * 0.9f), s.label);
	const items::Item* ownIt = nullptr;
	if (!it) {
		auto ow = items::ownItems.find(s.index);
		if (ow != items::ownItems.end()) ownIt = FindItem(ow->second);
	}
	const char* ownLabel = items::ownItems.count(s.index) ? "Nothing equipped" : "Your own"; // bekannt, aber leer
	std::string shownName = it ? it->name : (ownIt ? ownIt->name : ownLabel);
	bool custom = false;
	if (s.index == 1 || s.index == kBallSlot) { // eigenes Decal / eigener Ball
		std::string cn = CustomName(s.index == kBallSlot);
		if (!cn.empty()) { shownName = cn; custom = true; }
		else if (s.index == kBallSlot) shownName = "Default";
	}
	if (custom) d->AddCircleFilled(ImVec2(p.x + size.x - 18, p.y + size.y / 2), 3.5f, U(spec[2]), 16);
	std::string name = Fit(fRegular, fRegular->FontSize, shownName, size.x - 60);
	Text(d, fRegular, ImVec2(tx, p.y + 22), (it || custom) ? Mix(Hex(0xc9c9d3), cText, std::max(sa, h)) : Mix(cMuted, Hex(0xa9a9b5), h), name.c_str());
	ImGui::PopID();
}

// ---------- Rechts: eine Zeile ----------
static void ItemRow(int id, const char* name, const char* sub, ImVec4 qc, int cur, int& pick, float w, bool paintable = false) {
	ImGui::PushID(id);
	ImVec2 p = ImGui::GetCursorScreenPos();
	const float rh = 46;
	if (ImGui::InvisibleButton("##r", ImVec2(w, rh))) pick = id;
	if (id > 0 && ImGui::IsItemHovered()) items::Preload(id); // schon laden, bevor du klickst
	bool sel = cur == id;
	float h = Anim(ImGui::GetID("h"), ImGui::IsItemHovered() ? 1.0f : 0.0f, 22.0f);
	float s = Anim(ImGui::GetID("s"), sel ? 1.0f : 0.0f, 16.0f);
	ImDrawList* d = ImGui::GetWindowDrawList();
	ImVec2 q(p.x + w, p.y + rh);
	if (h > 0.01f || s > 0.01f) {
		ImVec4 bg = Mix(Alpha(cHover, h), Acc(0.13f), s);
		d->AddRectFilled(p, q, U(bg), 12);
	}
	if (s > 0.01f) d->AddRect(p, q, U(Acc(0.35f * s)), 12, 0, 1.0f);
	// Seltenheits-Balken
	d->AddRectFilled(ImVec2(p.x + 12, p.y + 14), ImVec2(p.x + 15, p.y + rh - 14), U(qc), 2);
	float tx = p.x + 28;
	std::string n = Fit(fRegular, fRegular->FontSize, name, w - 190);
	Text(d, fRegular, ImVec2(tx + h * 2.0f, p.y + (rh - fRegular->FontSize) / 2 - 1), Mix(Hex(0xd4d4dc), cText, std::max(h, s)), n.c_str());
	// Seltenheit rechts
	if (sub && sub[0]) {
		float sw = TextW(fSmall, sub);
		float rx = q.x - 18 - sw - (s > 0.01f ? 26 * s : 0);
		Text(d, fSmall, ImVec2(rx, p.y + (rh - fSmall->FontSize) / 2), Alpha(qc, 0.85f), sub);
	}
	if (s > 0.01f) CheckMark(d, ImVec2(q.x - 22, p.y + rh / 2), s);
	ImGui::PopID();
}

// Zeile fuer ein eigenes Decal/einen eigenen Ball: Vorschaubild, Name, Hinweis
static bool CustomRow(const customs::Custom& c, bool sel, bool fits, const char* sub, float w) {
	ImGui::PushID(c.id.c_str());
	ImVec2 p = ImGui::GetCursorScreenPos();
	const float rh = 54;
	bool click = ImGui::InvisibleButton("##c", ImVec2(w, rh));
	float h = Anim(ImGui::GetID("h"), ImGui::IsItemHovered() ? 1.0f : 0.0f, 22.0f);
	float s = Anim(ImGui::GetID("s"), sel ? 1.0f : 0.0f, 16.0f);
	ImGui::PopID();
	ImDrawList* d = ImGui::GetWindowDrawList();
	ImVec2 q(p.x + w, p.y + rh);
	if (h > 0.01f || s > 0.01f) d->AddRectFilled(p, q, U(Mix(Alpha(cHover, h), Acc(0.13f), s)), 12);
	if (s > 0.01f) d->AddRect(p, q, U(Acc(0.35f * s)), 12, 0, 1.0f);
	ImVec2 ta(p.x + 8, p.y + 7), tb(ta.x + 40, ta.y + 40);
	if (unsigned long long t = customs::Thumb(c.id)) d->AddImageRounded((ImTextureID)t, ta, tb, ImVec2(0, 0), ImVec2(1, 1), U(Rgb(255, 255, 255, fits ? 1.0f : 0.45f)), 8);
	else GradRect(d, ta, tb, 8, spec[0], spec[2], 0.25f);
	d->AddRect(ta, tb, U(Rgb(255, 255, 255, 0.08f)), 8);
	float tx = tb.x + 12;
	std::string n = Fit(fRegular, fRegular->FontSize, c.name, w - 180);
	Text(d, fRegular, ImVec2(tx + h * 2.0f, p.y + 9), fits ? Mix(Hex(0xd4d4dc), cText, std::max(h, s)) : cMuted, n.c_str());
	Text(d, fSmall, ImVec2(tx + h * 2.0f, p.y + 29), fits ? Alpha(Mix(spec[1], Rgb(255, 255, 255), 0.4f), 0.9f) : cFaint, sub);
	if (s > 0.01f) CheckMark(d, ImVec2(q.x - 22, p.y + rh / 2), s);
	return click;
}

// Karte "eigene hinzufuegen": oeffnet den Ordner
static bool AddCustomCard(const char* what, float w) {
	ImVec2 p = ImGui::GetCursorScreenPos();
	const float rh = 50;
	bool click = ImGui::InvisibleButton(what, ImVec2(w, rh));
	float h = Anim(ImGui::GetID(what), ImGui::IsItemHovered() ? 1.0f : 0.0f, 20.0f);
	ImDrawList* d = ImGui::GetWindowDrawList();
	ImVec2 q(p.x + w, p.y + rh);
	d->AddRectFilled(p, q, U(Rgb(255, 255, 255, 0.02f + 0.03f * h)), 12);
	// gestrichelter Rand
	const float dash = 7, gap = 5;
	ImU32 lc = U(Mix(cLine2, Acc(0.7f), h));
	for (float x = p.x + 12; x < q.x - 12; x += dash + gap) {
		d->AddLine(ImVec2(x, p.y + 0.5f), ImVec2(std::min(x + dash, q.x - 12), p.y + 0.5f), lc, 1.0f);
		d->AddLine(ImVec2(x, q.y - 0.5f), ImVec2(std::min(x + dash, q.x - 12), q.y - 0.5f), lc, 1.0f);
	}
	ImVec2 c(p.x + 28, p.y + rh / 2);
	d->AddCircle(c, 10, lc, 24, 1.4f);
	d->AddLine(ImVec2(c.x - 4.5f, c.y), ImVec2(c.x + 4.5f, c.y), lc, 1.6f);
	d->AddLine(ImVec2(c.x, c.y - 4.5f), ImVec2(c.x, c.y + 4.5f), lc, 1.6f);
	Text(d, fBold, ImVec2(p.x + 50, p.y + 8), Mix(cMuted, cText, h), what);
	Text(d, fSmall, ImVec2(p.x + 50, p.y + 27), cFaint, "Opens the folder - drop a .zip or folder in, it shows up here.");
	return click;
}

// Eingabefeld im Prism-Stil, komplett selbst gebaut (ohne ImGui-InputText): Zeichen kommen direkt aus
// der Tastatur-Abfrage. So kann das Spiel den Fokus nicht mehr "wegnehmen". true = Enter gedrueckt
static ImGuiID focusedField = 0;
static double caretT = 0;

static void AppendUtf8(char* buf, size_t n, unsigned c) {
	char enc[5] = {0};
	if (c < 0x80) enc[0] = (char)c;
	else if (c < 0x800) { enc[0] = (char)(0xC0 | (c >> 6)); enc[1] = (char)(0x80 | (c & 0x3F)); }
	else { enc[0] = (char)(0xE0 | (c >> 12)); enc[1] = (char)(0x80 | ((c >> 6) & 0x3F)); enc[2] = (char)(0x80 | (c & 0x3F)); }
	size_t len = strlen(buf), el = strlen(enc);
	if (len + el < n) memcpy(buf + len, enc, el + 1);
}
static void Backspace(char* buf) {
	size_t len = strlen(buf);
	while (len > 0) {
		len--;
		if ((buf[len] & 0xC0) != 0x80) break; // Anfang eines UTF-8-Zeichens
	}
	buf[len] = 0;
}

bool BlurField() {
	if (!focusedField) return false;
	focusedField = 0;
	return true;
}
bool FieldFocused() { return focusedField != 0; }

// Fensterbereich des Menues (fuer die Eingabe-Weiterleitung im Hauptmenue)
static std::atomic<float> mX{0}, mY{0}, mW{0}, mH{0};
bool OverMenu(float x, float y) {
	if (!Visible()) return false;
	const float m = 6; // etwas Rand (Schatten)
	return x >= mX - m && y >= mY - m && x < mX + mW + m && y < mY + mH + m;
}

static bool Field(ImDrawList* dl, const char* id, const char* hint, char* buf, size_t n, ImVec2 sp, ImVec2 sq, bool& focusFlag, bool icon,
	bool clearBtn) {
	ImGuiIO& io = ImGui::GetIO();
	const float sh = sq.y - sp.y;
	ImGuiID fid = ImGui::GetID(id);
	bool showClr = clearBtn && buf[0];
	ImVec2 clrP(sq.x - 38, sp.y + (sh - 32) / 2);
	// Leeren-Knopf zuerst (hat Vorrang vor dem Feld)
	bool clr = false, clrHov = false;
	if (showClr) {
		ImGui::SetCursorScreenPos(clrP);
		ImGui::PushID(id);
		clr = ImGui::InvisibleButton("##clr", ImVec2(32, 32));
		clrHov = ImGui::IsItemHovered();
		ImGui::PopID();
	}
	ImGui::SetCursorScreenPos(sp);
	bool click = ImGui::InvisibleButton(id, ImVec2(sq.x - sp.x, sh));
	if (click || clr || focusFlag) { focusedField = fid; caretT = ImGui::GetTime(); focusFlag = false; }
	else if (focusedField == fid && ImGui::IsMouseClicked(0) && !ImGui::IsMouseHoveringRect(sp, sq, false)) focusedField = 0;
	if (clr) buf[0] = 0;
	bool focused = focusedField == fid;
	bool enter = false;
	if (focused) {
		bool typed = false;
		for (int i = 0; i < io.InputQueueCharacters.Size; i++) {
			unsigned c = io.InputQueueCharacters[i];
			if (c >= 32 && c != 127) { AppendUtf8(buf, n, c); typed = true; }
		}
		io.InputQueueCharacters.resize(0);
		if (ImGui::IsKeyPressed(ImGuiKey_Backspace, true)) {
			if (io.KeyCtrl) buf[0] = 0;
			else Backspace(buf);
			typed = true;
		}
		if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) {
			if (const char* clip = ImGui::GetClipboardText())
				for (const char* c = clip; *c && strlen(buf) + 1 < n; c++)
					if ((unsigned char)*c >= 32) { size_t l = strlen(buf); buf[l] = *c; buf[l + 1] = 0; }
			typed = true;
		}
		if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) { buf[0] = 0; typed = true; } // Strg+A: alles neu tippen
		if (ImGui::IsKeyPressed(ImGuiKey_Enter, false)) enter = true;
		if (typed) caretT = ImGui::GetTime();
	}
	float* f = ImGui::GetStateStorage()->GetFloatRef(fid + 7, 0.0f);
	*f = Approach(*f, focused ? 1.0f : 0.0f, 18.0f);
	float hov = Anim(fid + 11, ImGui::IsItemHovered() && !focused ? 1.0f : 0.0f);
	dl->AddRectFilled(sp, sq, U(Mix(Mix(cPanel, cHover, hov * 0.6f), cPanel2, *f)), 12);
	dl->AddRect(sp, sq, U(Mix(Mix(cLine2, Rgb(255, 255, 255, 0.16f), hov), Acc(0.7f), *f)), 12, 0, 1.0f);
	float tx = sp.x + 14;
	if (icon) {
		ImVec2 lc(sp.x + 20, sp.y + sh / 2 - 1);
		ImU32 c = U(Mix(cFaint, Acc(1.0f), *f));
		dl->AddCircle(lc, 5.5f, c, 20, 1.6f);
		dl->AddLine(ImVec2(lc.x + 4, lc.y + 4), ImVec2(lc.x + 8, lc.y + 8), c, 1.6f);
		tx = sp.x + 38;
	}
	float right = sq.x - (showClr ? 44 : 14);
	float ty = sp.y + (sh - fRegular->FontSize) / 2 - 1;
	float tw = TextW(fRegular, buf);
	float off = std::max(0.0f, tw - (right - tx - 4)); // langer Text: Ende sichtbar
	dl->PushClipRect(ImVec2(tx - 1, sp.y), ImVec2(right, sq.y), true);
	if (!buf[0]) Text(dl, fRegular, ImVec2(tx, ty), cFaint, hint);
	else Text(dl, fRegular, ImVec2(tx - off, ty), cText, buf);
	if (focused && fmod(ImGui::GetTime() - caretT, 1.06) < 0.6) {
		float cx = tx + tw - off + 1;
		dl->AddRectFilled(ImVec2(cx, ty + 1), ImVec2(cx + 1.6f, ty + fRegular->FontSize + 1), U(Acc(1.0f)), 0);
	}
	dl->PopClipRect();
	if (showClr) {
		float h = Anim(fid + 13, clrHov ? 1.0f : 0.0f);
		ImVec2 c(sq.x - 22, sp.y + sh / 2);
		dl->AddCircleFilled(c, 9, U(Rgb(255, 255, 255, 0.06f + 0.08f * h)), 20);
		ImU32 xc = U(Mix(cMuted, cText, h));
		dl->AddLine(ImVec2(c.x - 3, c.y - 3), ImVec2(c.x + 3, c.y + 3), xc, 1.4f);
		dl->AddLine(ImVec2(c.x + 3, c.y - 3), ImVec2(c.x - 3, c.y + 3), xc, 1.4f);
	}
	return enter;
}

// weiches Scrollen mit dem Mausrad im aktuellen Child-Fenster
static void SmoothScroll(float& target, float& last) {
	ImGuiIO& io = ImGui::GetIO();
	float sc = ImGui::GetScrollY(), mx = ImGui::GetScrollMaxY();
	if (last >= 0 && fabsf(sc - last) > 1.0f) target = sc; // Scrollleiste gezogen
	if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && io.MouseWheel != 0.0f) target -= io.MouseWheel * 140.0f;
	target = std::clamp(target, 0.0f, mx);
	float ns = Approach(sc, target, 16.0f);
	// immer mindestens 1 Pixel weiter, sonst bleibt es durch das Runden kurz vor dem Ziel haengen
	if (ns != target && fabsf(ns - sc) < 1.0f) ns = sc + (target > sc ? 1.0f : -1.0f);
	if (fabsf(ns - target) < 2.0f) ns = target;
	ns = roundf(ns);
	if (ns != sc) ImGui::SetScrollY(ns);
	last = ns;
}

// Kurzbeschreibung eines Presets: "Octane · Cristiano · +2"
static std::string Summary(const std::map<int, int>& m, int maxNames) {
	std::string out;
	int shown = 0, rest = 0;
	for (int i = 0; i < items::kSlotCount; i++) {
		auto it = m.find(items::kSlots[i].index);
		if (it == m.end()) continue;
		const items::Item* item = FindItem(it->second);
		if (shown < maxNames) {
			if (!out.empty()) out += "  \xC2\xB7  ";
			out += item ? item->name : "Item";
			shown++;
		} else rest++;
	}
	if (rest) out += "  \xC2\xB7  +" + std::to_string(rest);
	return out;
}

static void Button(ImDrawList* dl, const char* id, const char* label, ImVec2 p, ImVec2 size, bool enabled, bool& clicked) {
	ImGui::SetCursorScreenPos(p);
	clicked = ImGui::InvisibleButton(id, size) && enabled;
	float h = Anim(ImGui::GetID(id), enabled && ImGui::IsItemHovered() ? 1.0f : 0.0f);
	float pr = Anim(ImGui::GetID(id) + 1, enabled && ImGui::IsItemActive() ? 1.0f : 0.0f, 30.0f);
	ImVec2 q(p.x + size.x, p.y + size.y);
	float in = pr * 1.5f;
	ImVec2 a(p.x + in, p.y + in), b(q.x - in, q.y - in);
	if (enabled) {
		dl->AddRectFilled(a, b, U(Mix(Mix(accent, Rgb(0, 0, 0), 0.08f), Mix(accent, Rgb(255, 255, 255), 0.08f), h)), 12);
	} else {
		dl->AddRectFilled(a, b, U(cPanel2), 12);
		dl->AddRect(a, b, U(cLine2), 12);
	}
	float tw = TextW(fBold, label);
	Text(dl, fBold, ImVec2(p.x + (size.x - tw) / 2, p.y + (size.y - fBold->FontSize) / 2 - 1), enabled ? Rgb(255, 255, 255) : cFaint, label);
}

// ---------- Presets ----------
static std::string Trim(const char* s) {
	std::string t(s);
	while (!t.empty() && t.back() == ' ') t.pop_back();
	while (!t.empty() && t.front() == ' ') t.erase(t.begin());
	return t;
}

// kleiner Text-Knopf auf einer Preset-Karte; Eingabe zuerst (Vorrang), gezeichnet wird spaeter
struct CardBtn { ImVec2 p; float w; bool hov, click; };
static CardBtn CardButton(const char* id, ImVec2 p, float w) {
	ImGui::SetCursorScreenPos(p);
	CardBtn b{p, w, false, false};
	b.click = ImGui::InvisibleButton(id, ImVec2(w, 30));
	b.hov = ImGui::IsItemHovered();
	return b;
}
static void DrawCardButton(ImDrawList* d, const CardBtn& b, const char* label, float vis, float hov, ImVec4 tint, bool strong) {
	if (vis < 0.01f) return;
	ImVec2 q(b.p.x + b.w, b.p.y + 30);
	ImVec4 bg = strong ? Alpha(tint, (0.18f + 0.14f * hov) * vis) : Rgb(255, 255, 255, (0.05f + 0.07f * hov) * vis);
	d->AddRectFilled(b.p, q, U(bg), 9);
	if (strong) d->AddRect(b.p, q, U(Alpha(tint, 0.45f * vis)), 9);
	float tw = TextW(fBold, label);
	ImVec4 tc = strong ? Mix(tint, Rgb(255, 255, 255), 0.55f) : Mix(cMuted, cText, hov);
	Text(d, fBold, ImVec2(b.p.x + (b.w - tw) / 2, b.p.y + 6), Alpha(tc, vis), label);
}

// action: 1 = laden, 2 = loeschen, 3 = neu speichern (gleicher Name = ueberschreiben),
//         4 = aktuelle Items in dieses Preset speichern, 7 = umbenennen
static void PresetsBody(ImDrawList* dl, ImVec2 o, ImVec2 W, float top, float pad, int& action, int& actionIdx) {
	const float leftW = 250, listTop = top + 14;
	dl->AddLine(ImVec2(o.x + pad + leftW + 14, o.y + top + 1), ImVec2(o.x + pad + leftW + 14, o.y + W.y - 1), U(cLine));
	int count = (int)items::presets.size();
	if (renameIdx >= count) renameIdx = -1;

	// Links: neues Preset aus den aktuellen Items
	float x = o.x + pad, y = o.y + listTop;
	Text(dl, fTiny, ImVec2(x + 2, y), cFaint, "NEW PRESET FROM CURRENT ITEMS");
	y += 22;
	int changed = (int)items::overrides.size();
	{
		int rows = std::min(changed, 5) + (changed > 5 ? 1 : 0);
		ImVec2 cp(x, y), cq(x + leftW, y + (changed ? 24.0f + rows * 22.0f : 100.0f));
		dl->AddRectFilled(cp, cq, U(cPanel), 14);
		dl->AddRect(cp, cq, U(cLine), 14);
		float ly = cp.y + 12;
		if (!changed) {
			Text(dl, fRegular, ImVec2(cp.x + 16, cp.y + 44), cMuted, "Nothing changed yet");
			Text(dl, fSmall, ImVec2(cp.x + 16, cp.y + 66), cFaint, "Pick some items first");
		} else {
			int n = 0;
			for (int i = 0; i < items::kSlotCount && n < 5; i++) {
				auto it = items::overrides.find(items::kSlots[i].index);
				if (it == items::overrides.end()) continue;
				const items::Item* item = FindItem(it->second);
				ImVec4 qc = item ? QualityColor(item->quality) : cMuted;
				dl->AddCircleFilled(ImVec2(cp.x + 18, ly + 10), 3.0f, U(qc), 12);
				Text(dl, fSmall, ImVec2(cp.x + 30, ly + 2), cFaint, items::kSlots[i].label);
				std::string nm = Fit(fSmall, fSmall->FontSize, item ? item->name : "Item", leftW - 130);
				float nw = TextW(fSmall, nm.c_str());
				Text(dl, fSmall, ImVec2(cq.x - 14 - nw, ly + 2), cText, nm.c_str());
				ly += 22;
				n++;
			}
			if (changed > 5) {
				char more[32];
				snprintf(more, sizeof more, "+%d more", changed - 5);
				Text(dl, fSmall, ImVec2(cp.x + 30, ly + 2), cFaint, more);
			}
		}
		y = cq.y + 12;
	}
	bool enter = Field(dl, "##pname", "Preset name", presetName, sizeof presetName, ImVec2(x, y), ImVec2(x + leftW, y + 42), focusName, false, false);
	y += 52;
	bool justSaved = ImGui::GetTime() - savedAt < 1.4;
	{
		std::string nm = Trim(presetName);
		bool exists = false;
		for (auto& p : items::presets)
			if (!nm.empty() && Lower(p.name) == Lower(nm)) exists = true;
		bool click = false;
		Button(dl, "##save", justSaved ? "Saved" : (exists ? "Replace preset" : "Save preset"), ImVec2(x, y), ImVec2(leftW, 42), changed > 0, click);
		if ((click || (enter && changed > 0))) { action = 3; savedAt = ImGui::GetTime(); }
		if (exists && !justSaved) Text(dl, fSmall, ImVec2(x + 2, y + 52), cFaint, "A preset with this name gets replaced");
	}

	// Rechts: Liste
	const float px = pad + leftW + 30, pw = W.x - px - pad;
	Text(dl, fTitle, ImVec2(o.x + px, o.y + listTop - 2), cText, "Presets");
	{
		char cnt[16];
		snprintf(cnt, sizeof cnt, "%d", count);
		float nw = TextW(fTitle, "Presets"), cw = TextW(fTiny, cnt);
		ImVec2 c0(o.x + px + nw + 10, o.y + listTop + 5);
		dl->AddRectFilled(c0, ImVec2(c0.x + cw + 14, c0.y + 18), U(Rgb(255, 255, 255, 0.06f)), 9);
		Text(dl, fTiny, ImVec2(c0.x + 7, c0.y + 2), cMuted, cnt);
		if (count) {
			const char* hint = "Click to load  \xC2\xB7  Update saves your current items into it";
			Text(dl, fSmall, ImVec2(o.x + px, o.y + listTop + 28), cFaint, Fit(fSmall, fSmall->FontSize, hint, pw).c_str());
		}
	}
	const float ly = listTop + 56;
	if (!count) {
		ImVec2 c(o.x + px + pw / 2, o.y + ly + 124);
		dl->AddRectFilled(ImVec2(c.x - 26, c.y - 26), ImVec2(c.x + 26, c.y + 26), U(Acc(0.10f)), 16);
		dl->AddRect(ImVec2(c.x - 11, c.y - 6), ImVec2(c.x + 11, c.y + 12), U(Acc(0.9f)), 4, 0, 1.6f);
		dl->AddLine(ImVec2(c.x - 7, c.y - 11), ImVec2(c.x + 7, c.y - 11), U(Acc(0.6f)), 1.6f);
		const char* t1 = "No presets yet";
		const char* t2 = "Save your current items on the left";
		Text(dl, fBold, ImVec2(c.x - TextW(fBold, t1) / 2, c.y + 40), cText, t1);
		Text(dl, fSmall, ImVec2(c.x - TextW(fSmall, t2) / 2, c.y + 62), cMuted, t2);
		return;
	}
	double now = ImGui::GetTime();
	if (pendingDelete >= 0 && now - pendingDeleteAt > 2.5) pendingDelete = -1;
	if (pendingUpdate >= 0 && now - pendingUpdateAt > 2.5) pendingUpdate = -1;
	ImGui::SetCursorScreenPos(ImVec2(o.x + px - 4, o.y + ly));
	ImGui::BeginChild("presets", ImVec2(pw + 8, W.y - ly - 14), false, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse);
	SmoothScroll(pScrollTarget, pScrollLast);
	const float cw = pw - 8, ch = 64;
	ImDrawList* d = ImGui::GetWindowDrawList();
	ImVec4 red = Rgb(235, 85, 85);
	d->ChannelsSplit(2); // 0 = Karten-Hintergrund, 1 = alles darauf (auch das Namensfeld)
	d->ChannelsSetCurrent(1);
	std::string curDecal, curBall;
	std::map<std::string, std::string> cNames;
	{
		std::lock_guard<std::mutex> lk(customs::mu);
		curDecal = customs::selDecal;
		curBall = customs::selBall;
		for (auto& c : customs::list) cNames[c.id] = c.name;
	}
	for (int i = 0; i < count; i++) {
		const items::Preset& pr = items::presets[i];
		ImGui::PushID(i);
		ImVec2 p = ImGui::GetCursorScreenPos(), q(p.x + cw, p.y + ch);
		bool confirmDel = pendingDelete == i, confirmUpd = pendingUpdate == i, renaming = renameIdx == i;
		bool justUpd = savedIdx == i && now - savedIdxAt < 1.4;
		// Knoepfe von rechts: Loeschen, Umbenennen, Update (Eingabe zuerst, damit sie Vorrang vor der Karte haben)
		const float by = p.y + (ch - 30) / 2;
		float delW = confirmDel ? 70.0f : 34.0f;
		CardBtn del = CardButton("##del", ImVec2(q.x - 14 - delW, by), delW);
		float renW = TextW(fBold, "Rename") + 22;
		CardBtn ren = CardButton("##ren", ImVec2(del.p.x - 6 - renW, by), renW);
		const char* updLabel = justUpd ? "Saved" : (confirmUpd ? "Save here?" : "Update");
		float updW = TextW(fBold, updLabel) + 22;
		CardBtn upd = CardButton("##upd", ImVec2(ren.p.x - 6 - updW, by), updW);
		// Name aendern: Feld direkt auf der Karte
		bool renEnter = false;
		if (renaming) {
			renEnter = Field(d, "##rn", "Preset name", renameBuf, sizeof renameBuf, ImVec2(p.x + 10, p.y + 12), ImVec2(upd.p.x - 12, p.y + ch - 12),
				focusRename, false, false);
		}
		ImGui::SetCursorScreenPos(p);
		bool click = ImGui::InvisibleButton("##card", ImVec2(cw, ch));
		bool hov = ImGui::IsItemHovered() || del.hov || ren.hov || upd.hov;
		bool hasC = pr.hasCustoms && (!pr.customDecal.empty() || !pr.customBall.empty());
		bool active = pr.items == items::overrides && pr.paints == items::paints && (!pr.items.empty() || !pr.paints.empty() || hasC) &&
			(!pr.hasCustoms || (pr.customDecal == curDecal && pr.customBall == curBall));
		float h = Anim(ImGui::GetID("h"), hov ? 1.0f : 0.0f);
		float ac = Anim(ImGui::GetID("a"), active ? 1.0f : 0.0f);
		float vis = std::max({h, confirmDel || confirmUpd || renaming || justUpd ? 1.0f : 0.0f});
		d->ChannelsSetCurrent(0);
		d->AddRectFilled(p, q, U(Mix(Mix(cPanel, cHover, h), Acc(0.12f), ac * 0.8f)), 14);
		d->AddRect(p, q, U(Mix(cLine, Acc(renaming ? 0.7f : 0.55f), std::max(ac, renaming ? 1.0f : 0.0f))), 14);
		d->ChannelsSetCurrent(1);
		if (!renaming) {
			std::string nm = Fit(fBold, fBold->FontSize, pr.name, upd.p.x - p.x - 30 - (ac > 0.01f ? 64 : 0));
			Text(d, fBold, ImVec2(p.x + 18, p.y + 13), cText, nm.c_str());
			if (ac > 0.01f) { // "Active" direkt hinter dem Namen
				float nx = p.x + 18 + TextW(fBold, nm.c_str()) + 10, tw = TextW(fTiny, "Active");
				d->AddRectFilled(ImVec2(nx, p.y + 13), ImVec2(nx + tw + 16, p.y + 32), U(Acc(0.2f * ac)), 9);
				Text(d, fTiny, ImVec2(nx + 8, p.y + 16), Alpha(Mix(accent, Rgb(255, 255, 255), 0.5f), ac), "Active");
			}
			int ownPainted = 0;
			for (auto& [slot, pid] : pr.paints) if (!pr.items.count(slot)) ownPainted++;
			std::string sum = pr.items.empty() ? std::string(hasC ? "" : (ownPainted ? "Your own items painted" : "Your own items")) : Summary(pr.items, 3);
			if (!pr.items.empty() && ownPainted) sum += "  \xC2\xB7  " + std::to_string(ownPainted) + " own painted";
			if (pr.hasCustoms)
				for (auto* cid : {&pr.customDecal, &pr.customBall}) {
					if (cid->empty()) continue;
					auto cn = cNames.find(*cid);
					if (!sum.empty()) sum += "  \xC2\xB7  ";
					sum += (cn != cNames.end() ? cn->second : std::string("Custom")) + (cid == &pr.customBall ? " (ball)" : " (custom)");
				}
			Text(d, fSmall, ImVec2(p.x + 18, p.y + 36), cMuted, Fit(fSmall, fSmall->FontSize, sum, upd.p.x - p.x - 30).c_str());
		}
		float vh = std::max(vis, 0.55f); // Knoepfe immer sichtbar, beim Drueberfahren deutlicher
		DrawCardButton(d, upd, updLabel, std::max(vh, justUpd ? 1.0f : 0.0f), Anim(ImGui::GetID("uh"), upd.hov ? 1.0f : 0.0f), justUpd ? Rgb(90, 220, 140) : accent,
			confirmUpd || justUpd);
		DrawCardButton(d, ren, renaming ? "Save" : "Rename", vh, Anim(ImGui::GetID("rh"), ren.hov ? 1.0f : 0.0f), accent, renaming);
		// Loeschen: Muelleimer bzw. "Delete"
		float dh = Anim(ImGui::GetID("dh"), del.hov ? 1.0f : 0.0f);
		if (confirmDel) DrawCardButton(d, del, "Delete", 1.0f, dh, red, true);
		else if (vh > 0.01f) {
			ImVec2 dq(del.p.x + del.w, del.p.y + 30);
			d->AddRectFilled(del.p, dq, U(Rgb(255, 255, 255, (0.05f + 0.07f * dh) * vh)), 9);
			ImU32 c = U(Alpha(Mix(cMuted, Rgb(255, 130, 130), dh), vh));
			ImVec2 m(del.p.x + del.w / 2, del.p.y + 15);
			d->AddLine(ImVec2(m.x - 6, m.y - 5), ImVec2(m.x + 6, m.y - 5), c, 1.5f);
			d->AddLine(ImVec2(m.x - 2, m.y - 7.5f), ImVec2(m.x + 2, m.y - 7.5f), c, 1.5f);
			d->AddRect(ImVec2(m.x - 4.5f, m.y - 3), ImVec2(m.x + 4.5f, m.y + 7), c, 2, 0, 1.5f);
		}
		// Klicks
		bool renFocused = renaming && FieldFocused();
		if (del.click) {
			if (confirmDel) { action = 2; actionIdx = i; pendingDelete = -1; }
			else { pendingDelete = i; pendingDeleteAt = now; pendingUpdate = -1; }
		} else if (upd.click) {
			if (confirmUpd) { action = 4; actionIdx = i; pendingUpdate = -1; savedIdx = i; savedIdxAt = now; }
			else { pendingUpdate = i; pendingUpdateAt = now; pendingDelete = -1; }
		} else if (ren.click) {
			if (renaming) { action = 7; actionIdx = i; }
			else { renameIdx = i; snprintf(renameBuf, sizeof renameBuf, "%s", pr.name.c_str()); focusRename = true; }
			pendingDelete = pendingUpdate = -1;
		} else if (renEnter || (renaming && !renFocused && !focusRename)) {
			action = 7; actionIdx = i; // Enter oder daneben geklickt: Namen uebernehmen
		} else if (click && !renaming) {
			action = 1;
			actionIdx = i;
			pendingDelete = pendingUpdate = -1;
		}
		ImGui::SetCursorScreenPos(ImVec2(p.x, q.y + 8));
		ImGui::Dummy(ImVec2(cw, 0));
		ImGui::PopID();
	}
	d->ChannelsMerge();
	ImGui::Dummy(ImVec2(0, 6));
	ImGui::EndChild();
}

// ---------- Prism-Logo: Dreieck + Lichtstrahl im Spektrum (wie im Launcher) ----------
// feine Linie im Spektrum (links -> rechts)
static void SpecLine(ImDrawList* dl, float x0, float x1, float y, float h, float a) {
	float xm = x0 + (x1 - x0) * 0.52f;
	dl->AddRectFilledMultiColor(ImVec2(x0, y), ImVec2(xm, y + h), U(Alpha(spec[0], a)), U(Alpha(spec[1], a)), U(Alpha(spec[1], a)), U(Alpha(spec[0], a)));
	dl->AddRectFilledMultiColor(ImVec2(xm, y), ImVec2(x1, y + h), U(Alpha(spec[1], a)), U(Alpha(spec[2], a)), U(Alpha(spec[2], a)), U(Alpha(spec[1], a)));
}

static ImVec4 SpecAt(float t) {
	t = std::clamp(t, 0.0f, 1.0f);
	return t < 0.55f ? Mix(spec[0], spec[1], t / 0.55f) : Mix(spec[1], spec[2], (t - 0.55f) / 0.45f);
}
// Lichtbrechung (wie im Launcher): weisses Licht faellt links ins Prisma, rechts tritt ein Spektrum-Faecher aus.
// Laeuft beim Oeffnen des Menues und beim Drueberfahren.
static double logoFx = -10;
static void Logo(ImDrawList* dl, ImVec2 c, float size, float hover) {
	static bool was = false;
	double now = ImGui::GetTime();
	if (hover > 0.5f && !was && now - logoFx > 1.2) logoFx = now;
	was = hover > 0.5f;
	float t = float(now - logoFx);
	float glow = 0.18f + 0.22f * hover;
	for (int i = 3; i >= 1; i--) dl->AddCircleFilled(ImVec2(c.x, c.y + size * 0.08f), size * (0.28f + 0.13f * i), U(Alpha(spec[1], glow * 0.16f)), 32);
	float u = size / 200.0f;
	auto P = [&](float lx, float ly) { return ImVec2(c.x + (lx - 100.0f) * u, c.y + (ly - 96.0f) * u); };
	// ein Strahl: zeichnet sich von a nach b, bleibt kurz und blendet aus
	auto ray = [&](ImVec2 a, ImVec2 b, ImVec4 c0, ImVec4 c1, float w, float delay, float dur) {
		float p = (t - delay) / dur;
		if (p <= 0.0f || p >= 1.0f) return;
		float f = std::min(1.0f, EaseOut(p / 0.45f)), al = p < 0.6f ? 1.0f : 1.0f - (p - 0.6f) / 0.4f;
		const int n = 10;
		for (int i = 0; i < n; i++) {
			float s0 = f * i / n, s1 = f * (i + 1) / n;
			ImVec2 p0(a.x + (b.x - a.x) * s0, a.y + (b.y - a.y) * s0), p1(a.x + (b.x - a.x) * s1, a.y + (b.y - a.y) * s1);
			dl->AddLine(p0, p1, U(Alpha(Mix(c0, c1, (s0 + s1) * 0.5f), al)), w);
		}
	};
	ImVec4 white = Rgb(255, 255, 255), clear = Rgb(255, 255, 255, 0.0f);
	float rw = std::max(1.2f, 6.0f * u);
	ray(P(-74, 114), P(66, 98), clear, white, rw, 0.0f, 1.05f);
	ray(P(66, 98), P(138, 104), white, white, rw, 0.17f, 0.9f);
	ImVec2 ex = P(138, 104);
	ray(ex, P(236, 78), spec[0], Alpha(spec[0], 0.0f), rw, 0.26f, 1.0f);
	ray(ex, P(240, 106), spec[1], Alpha(spec[1], 0.0f), rw, 0.305f, 1.0f);
	ray(ex, P(236, 134), spec[2], Alpha(spec[2], 0.0f), rw, 0.35f, 1.0f);
	// Prisma
	ImVec2 t0 = P(100, 34), t1 = P(164, 152), t2 = P(36, 152);
	float lw = std::max(1.6f, 15.0f * u);
	ImU32 wc = U(Rgb(244, 244, 250));
	dl->AddTriangle(t0, t1, t2, wc, lw);
	for (ImVec2 p : {t0, t1, t2}) dl->AddCircleFilled(p, lw * 0.5f, wc, 12); // runde Ecken
	// Strahl im Spektrum; leuchtet auf, wenn Licht hindurchgeht
	float pulse = (t > 0.15f && t < 1.05f) ? sinf((t - 0.15f) / 0.9f * 3.14159f) : 0.0f;
	ImVec2 b0 = P(100, 34), b1 = P(120, 152);
	float bw = std::max(1.4f, 12.0f * u);
	const int n = 14;
	for (int i = 0; i < n; i++) {
		float a = float(i) / n, b = float(i + 1) / n;
		ImVec2 p0(b0.x + (b1.x - b0.x) * a, b0.y + (b1.y - b0.y) * a), p1(b0.x + (b1.x - b0.x) * b, b0.y + (b1.y - b0.y) * b);
		dl->AddLine(p0, p1, U(Mix(SpecAt((a + b) * 0.5f), white, 0.45f * pulse)), bw);
	}
	dl->AddCircleFilled(b0, bw * 0.5f, U(Mix(spec[0], white, 0.45f * pulse)), 12);
	dl->AddCircleFilled(b1, bw * 0.5f, U(Mix(spec[2], white, 0.45f * pulse)), 12);
}

// ---------- Farben ----------
struct ColorAction {
	bool changed = false, save = false, on = false, both = true;
	float cols[2][2][3];
	int finishSlot = -1, finishId = -1;
};
static int colorEdit = 0;    // 0 Primaer, 1 Akzent, 2 Primaer-Finish, 3 Akzent-Finish
static int colorTeam = 0;    // bearbeitetes Team (0 blau, 1 orange), wenn nicht fuer beide
static float editCols[2][2][3];
static bool editing = false;
static float fScrollTarget = 0.0f, fScrollLast = -1.0f;
static char finishSearch[48] = {0};
static bool focusFinish = false;
static float hsv[2][2][3];   // gemerkter Farbton/Saettigung/Helligkeit je Team und Farbe
static bool hsvOk[2][2] = {};
static float segX = -1.0f;   // gleitende Markierung im Team-Schalter

static void Swatch(ImDrawList* dl, ImVec2 c, float r, const float* rgb, float a = 1.0f) {
	dl->AddCircleFilled(c, r, U(ImVec4(rgb[0], rgb[1], rgb[2], a)), 32);
	dl->AddCircle(c, r, U(Rgb(255, 255, 255, 0.14f * a)), 32, 1.0f);
}

// kleiner Schalter (animiert)
static bool Toggle(ImDrawList* dl, const char* id, ImVec2 p, bool on) {
	ImGui::SetCursorScreenPos(p);
	bool click = ImGui::InvisibleButton(id, ImVec2(42, 22));
	float k = Anim(ImGui::GetID(id) + 3, on ? 1.0f : 0.0f, 16.0f);
	dl->AddRectFilled(p, ImVec2(p.x + 42, p.y + 22), U(Mix(cLine2, accent, k)), 11);
	dl->AddCircleFilled(ImVec2(p.x + 11 + 20 * k, p.y + 11), 8, U(Rgb(255, 255, 255)), 20);
	return click;
}

// Farbe -> HSV, aber bei Schwarz/Grau Farbton und Saettigung behalten (sonst springt die Auswahl weg)
static void SyncHsv(int t, int c, const float* rgb) {
	float* h = hsv[t][c];
	if (hsvOk[t][c]) {
		float r, g, b;
		ImGui::ColorConvertHSVtoRGB(h[0], h[1], h[2], r, g, b);
		if (fabsf(r - rgb[0]) < 0.0045f && fabsf(g - rgb[1]) < 0.0045f && fabsf(b - rgb[2]) < 0.0045f) return;
	}
	float nh, ns, nv;
	ImGui::ColorConvertRGBtoHSV(rgb[0], rgb[1], rgb[2], nh, ns, nv);
	if (hsvOk[t][c]) {
		if (nv < 0.002f) { nh = h[0]; ns = h[1]; }
		else if (ns < 0.002f) nh = h[0];
	}
	h[0] = nh; h[1] = ns; h[2] = nv;
	hsvOk[t][c] = true;
}

// runde Ecken fuer Flaechen ohne Rundung (Farbverlaeufe): Ecken mit der Hintergrundfarbe abdecken
static void RoundCorners(ImDrawList* dl, ImVec2 a, ImVec2 b, float r, ImU32 bg) {
	const float pi = 3.14159265f;
	struct C { float x, y, cx, cy, a0; } cs[4] = {
		{a.x, a.y, a.x + r, a.y + r, pi}, {b.x, a.y, b.x - r, a.y + r, 1.5f * pi}, {b.x, b.y, b.x - r, b.y - r, 0}, {a.x, b.y, a.x + r, b.y - r, 0.5f * pi}};
	for (auto& c : cs) {
		dl->PathLineTo(ImVec2(c.x, c.y));
		dl->PathArcTo(ImVec2(c.cx, c.cy), r, c.a0, c.a0 + 0.5f * pi, 8);
		dl->PathFillConvex(bg);
	}
}

// Eigener Farbwaehler: grosses Feld (Saettigung/Helligkeit) und Farbton-Leiste
static bool Picker(ImDrawList* dl, ImVec2 p, ImVec2 size, float* h, bool& active) {
	ImGuiIO& io = ImGui::GetIO();
	bool changed = false;
	const float hueW = 16, gap = 14, r = 12;
	ImVec2 s0 = p, s1(p.x + size.x - hueW - gap, p.y + size.y);
	ImVec2 h0(s1.x + gap, p.y), h1(p.x + size.x, p.y + size.y);
	ImGui::SetCursorScreenPos(s0);
	ImGui::InvisibleButton("##sv", ImVec2(s1.x - s0.x, s1.y - s0.y));
	bool sa = ImGui::IsItemActive(), sh = ImGui::IsItemHovered();
	if (sa) {
		float sat = std::clamp((io.MousePos.x - s0.x) / (s1.x - s0.x), 0.0f, 1.0f);
		float val = 1.0f - std::clamp((io.MousePos.y - s0.y) / (s1.y - s0.y), 0.0f, 1.0f);
		if (sat != h[1] || val != h[2]) { h[1] = sat; h[2] = val; changed = true; }
	}
	ImGui::SetCursorScreenPos(ImVec2(h0.x - 4, h0.y));
	ImGui::InvisibleButton("##hue", ImVec2(h1.x - h0.x + 8, h1.y - h0.y));
	bool ha = ImGui::IsItemActive(), hh = ImGui::IsItemHovered();
	if (ha) {
		float hue = std::clamp((io.MousePos.y - h0.y) / (h1.y - h0.y), 0.0f, 0.9999f);
		if (hue != h[0]) { h[0] = hue; changed = true; }
	}
	active = sa || ha;
	float r0, g0, b0;
	ImGui::ColorConvertHSVtoRGB(h[0], 1, 1, r0, g0, b0);
	ImU32 white = U(Rgb(255, 255, 255)), pure = U(ImVec4(r0, g0, b0, 1)), clear = U(ImVec4(0, 0, 0, 0)), black = U(ImVec4(0, 0, 0, 1));
	dl->AddRectFilledMultiColor(s0, s1, white, pure, pure, white);
	dl->AddRectFilledMultiColor(s0, s1, clear, clear, black, black);
	RoundCorners(dl, s0, s1, r, U(cBg));
	dl->AddRect(s0, s1, U(Rgb(255, 255, 255, 0.08f)), r, 0, 1.0f);
	const float seg = (h1.y - h0.y) / 6;
	for (int i = 0; i < 6; i++) {
		float ra, ga, ba, rb, gb, bb;
		ImGui::ColorConvertHSVtoRGB(i / 6.0f, 1, 1, ra, ga, ba);
		ImGui::ColorConvertHSVtoRGB((i + 1) / 6.0f, 1, 1, rb, gb, bb);
		ImU32 ca = U(ImVec4(ra, ga, ba, 1)), cb = U(ImVec4(rb, gb, bb, 1));
		dl->AddRectFilledMultiColor(ImVec2(h0.x, h0.y + seg * i), ImVec2(h1.x, h0.y + seg * (i + 1) + (i < 5 ? 0.5f : 0)), ca, ca, cb, cb);
	}
	RoundCorners(dl, h0, h1, hueW / 2, U(cBg));
	// Markierungen
	float cr, cg, cb;
	ImGui::ColorConvertHSVtoRGB(h[0], h[1], h[2], cr, cg, cb);
	ImVec2 m(s0.x + h[1] * (s1.x - s0.x), s0.y + (1.0f - h[2]) * (s1.y - s0.y));
	float k = Anim(ImGui::GetID("svk"), sa ? 1.0f : (sh ? 0.4f : 0.0f), 20.0f);
	float mr = 8.0f + 2.5f * k;
	dl->AddCircleFilled(ImVec2(m.x, m.y + 1), mr + 2, U(Rgb(0, 0, 0, 0.35f)), 28);
	dl->AddCircleFilled(m, mr, U(ImVec4(cr, cg, cb, 1)), 28);
	dl->AddCircle(m, mr, white, 28, 2.5f);
	float hk = Anim(ImGui::GetID("hk"), ha ? 1.0f : (hh ? 0.4f : 0.0f), 20.0f);
	float hy = h0.y + h[0] * (h1.y - h0.y), ex = 3.0f + 1.5f * hk;
	dl->AddRectFilled(ImVec2(h0.x - ex, hy - 5 - hk), ImVec2(h1.x + ex, hy + 5 + hk), U(Rgb(0, 0, 0, 0.35f)), 6);
	dl->AddRectFilled(ImVec2(h0.x - ex + 1, hy - 4 - hk), ImVec2(h1.x + ex - 1, hy + 4 + hk), pure, 5);
	dl->AddRect(ImVec2(h0.x - ex + 1, hy - 4 - hk), ImVec2(h1.x + ex - 1, hy + 4 + hk), white, 5, 0, 2.0f);
	return changed;
}

// Karte links: Farbe (oben) + Lackierung (unten) einer Ebene (Primaer/Akzent)
static void LayerCard(ImDrawList* dl, int layer, ImVec2 cp, float w, const float* rgb, bool on, const char* finish, ImVec4 finishDot) {
	const float topH = 60, botH = 52;
	ImVec2 cq(cp.x + w, cp.y + topH + botH);
	ImGui::PushID(layer + 400);
	ImGui::SetCursorScreenPos(cp);
	if (ImGui::InvisibleButton("##col", ImVec2(w, topH))) colorEdit = layer;
	float ht = Anim(ImGui::GetID("ht"), ImGui::IsItemHovered() ? 1.0f : 0.0f);
	ImGui::SetCursorScreenPos(ImVec2(cp.x, cp.y + topH));
	if (ImGui::InvisibleButton("##fin", ImVec2(w, botH))) {
		if (colorEdit != layer + 2) { finishSearch[0] = 0; fScrollTarget = 0; fScrollLast = -1; }
		colorEdit = layer + 2;
	}
	float hb = Anim(ImGui::GetID("hb"), ImGui::IsItemHovered() ? 1.0f : 0.0f);
	float st = Anim(ImGui::GetID("st"), colorEdit == layer ? 1.0f : 0.0f);
	float sb = Anim(ImGui::GetID("sb"), colorEdit == layer + 2 ? 1.0f : 0.0f);
	ImGui::PopID();
	float sel = std::max(st, sb);
	dl->AddRectFilled(cp, cq, U(cPanel), 14);
	// Hervorhebung des gewaehlten Teils (oben oder unten)
	if (ht > 0.01f || st > 0.01f)
		dl->AddRectFilled(cp, ImVec2(cq.x, cp.y + topH), U(Mix(Alpha(cHover, ht), Acc(0.12f), st)), 14, ImDrawFlags_RoundCornersTop);
	if (hb > 0.01f || sb > 0.01f)
		dl->AddRectFilled(ImVec2(cp.x, cp.y + topH), cq, U(Mix(Alpha(cHover, hb), Acc(0.12f), sb)), 14, ImDrawFlags_RoundCornersBottom);
	dl->AddRect(cp, cq, U(Mix(cLine, Acc(0.55f), sel)), 14);
	dl->AddLine(ImVec2(cp.x + 1, cp.y + topH), ImVec2(cq.x - 1, cp.y + topH), U(Mix(cLine, Acc(0.25f), sel)));
	// oben: Farbe
	float cy = cp.y + topH / 2;
	Swatch(dl, ImVec2(cp.x + 30, cy), 14, rgb, on ? 1.0f : 0.35f);
	Text(dl, fBold, ImVec2(cp.x + 56, cy - 18), cText, layer == 0 ? "Primary" : "Accent");
	char hex[16];
	snprintf(hex, sizeof hex, "#%02X%02X%02X", int(rgb[0] * 255 + 0.5f), int(rgb[1] * 255 + 0.5f), int(rgb[2] * 255 + 0.5f));
	Text(dl, fSmall, ImVec2(cp.x + 56, cy + 1), cMuted, on ? hex : "Game color");
	// unten: Lackierung
	float by = cp.y + topH + botH / 2;
	dl->AddRectFilled(ImVec2(cp.x + 22, by - 7), ImVec2(cp.x + 38, by + 7), U(Alpha(finishDot, 0.25f)), 4);
	dl->AddRect(ImVec2(cp.x + 22, by - 7), ImVec2(cp.x + 38, by + 7), U(Alpha(finishDot, 0.8f)), 4, 0, 1.2f);
	Text(dl, fTiny, ImVec2(cp.x + 56, by - 16), cFaint, "FINISH");
	Text(dl, fSmall, ImVec2(cp.x + 56, by - 1), Mix(Hex(0xb9b9c3), cText, std::max(hb, sb)), Fit(fSmall, fSmall->FontSize, finish, w - 90).c_str());
	// Pfeil rechts
	ImU32 ac = U(Mix(cFaint, cText, std::max(hb, sb)));
	float ax = cq.x - 20 + 2 * hb;
	dl->AddLine(ImVec2(ax - 3, by - 5), ImVec2(ax + 2, by), ac, 1.6f);
	dl->AddLine(ImVec2(ax + 2, by), ImVec2(ax - 3, by + 5), ac, 1.6f);
}

static void ColorsBody(ImDrawList* dl, ImVec2 o, ImVec2 W, float top, float pad, ColorAction& act) {
	if (!editing) memcpy(editCols, items::colors, sizeof editCols);
	bool on = items::colorsOn, both = items::colorsBoth;
	int team = both ? 0 : colorTeam;
	const float x = o.x + pad, fullW = W.x - 2 * pad;

	// --- Oben: an/aus + fuer welches Team ---
	{
		ImVec2 cp(x, o.y + top + 14), cq(x + fullW, cp.y + 56);
		dl->AddRectFilled(cp, cq, U(cPanel), 14);
		dl->AddRect(cp, cq, U(on ? Acc(0.45f) : cLine), 14);
		if (Toggle(dl, "##colon", ImVec2(cp.x + 16, cp.y + 17), on)) { on = !on; act.changed = act.save = true; }
		Text(dl, fBold, ImVec2(cp.x + 72, cp.y + 10), cText, "Custom colors");
		Text(dl, fSmall, ImVec2(cp.x + 72, cp.y + 30), cMuted, on ? "On \xE2\x80\x93 only you see them" : "Off \xE2\x80\x93 your car keeps its normal colors");
		// Team-Schalter: Both teams | Blue | Orange
		const char* names[3] = {"Both teams", "Blue", "Orange"};
		ImVec4 dots[3] = {cMuted, Rgb(70, 140, 255), Rgb(255, 140, 50)};
		float sw[3], total = 0;
		for (int i = 0; i < 3; i++) { sw[i] = TextW(fBold, names[i]) + (i ? 46.0f : 30.0f); total += sw[i]; }
		ImVec2 bp(cq.x - 10 - total - 8, cp.y + 10), bq(cq.x - 10, cp.y + 46);
		dl->AddRectFilled(bp, bq, U(cBg), 12);
		dl->AddRect(bp, bq, U(cLine), 12);
		int cur = both ? 0 : 1 + colorTeam;
		float sx[3], xx = bp.x + 4;
		for (int i = 0; i < 3; i++) { sx[i] = xx; xx += sw[i]; }
		segX = segX < 0 ? sx[cur] - o.x : Approach(segX, sx[cur] - o.x, 18.0f);
		float segW = sw[cur];
		ImVec4 segC = cur == 0 ? Acc(0.20f) : Alpha(dots[cur], 0.20f);
		dl->AddRectFilled(ImVec2(o.x + segX, bp.y + 4), ImVec2(o.x + segX + segW, bq.y - 4), U(segC), 9);
		for (int i = 0; i < 3; i++) {
			ImGui::SetCursorScreenPos(ImVec2(sx[i], bp.y + 4));
			ImGui::PushID(i + 300);
			if (ImGui::InvisibleButton("##seg", ImVec2(sw[i], bq.y - bp.y - 8)) && i != cur) {
				if (i == 0) both = true;
				else { both = false; colorTeam = i - 1; }
				act.changed = act.save = true;
			}
			float hv = Anim(ImGui::GetID("h"), ImGui::IsItemHovered() ? 1.0f : 0.0f);
			float k = Anim(ImGui::GetID("k"), i == cur ? 1.0f : 0.0f);
			ImGui::PopID();
			float tx = sx[i] + 15;
			if (i) { dl->AddCircleFilled(ImVec2(sx[i] + 19, bp.y + 18), 4, U(dots[i]), 12); tx = sx[i] + 31; }
			Text(dl, fBold, ImVec2(tx, bp.y + 8), Mix(Mix(cMuted, Hex(0xc9c9d3), hv), cText, k), names[i]);
		}
		team = both ? 0 : colorTeam;
	}

	// --- Links: Primaer / Akzent ---
	const float leftW = 250, cy = o.y + top + 90;
	dl->AddLine(ImVec2(o.x + pad + leftW + 14, cy - 4), ImVec2(o.x + pad + leftW + 14, o.y + W.y - 1), U(cLine));
	for (int f = 0; f < 2; f++) {
		int slot = f == 0 ? 7 : 12;
		auto ov = items::overrides.find(slot);
		const items::Item* it = ov != items::overrides.end() ? FindItem(ov->second) : nullptr;
		bool own = false;
		if (!it) {
			auto ow = items::ownItems.find(slot);
			if (ow != items::ownItems.end()) { it = FindItem(ow->second); own = it != nullptr; }
		}
		std::string fin = it ? it->name : std::string("Your own");
		if (own) fin += "  \xC2\xB7  yours";
		LayerCard(dl, f, ImVec2(x, cy + f * 124.0f), leftW, editCols[team][f], on, fin.c_str(), it ? QualityColor(it->quality) : cMuted);
	}

	// --- Rechts ---
	const float px = pad + leftW + 30, pw = W.x - px - pad;
	const float ry = cy - 2;
	if (colorEdit <= 1) {
		const char* title = colorEdit == 0 ? "Primary color" : "Accent color";
		Text(dl, fTitle, ImVec2(o.x + px, ry), cText, title);
		if (!both) {
			const char* t = colorTeam == 0 ? "Blue team" : "Orange team";
			float w = TextW(fSmall, t);
			ImVec4 c = colorTeam == 0 ? Rgb(70, 140, 255) : Rgb(255, 140, 50);
			dl->AddCircleFilled(ImVec2(o.x + px + pw - w - 12, ry + 11), 4, U(c), 12);
			Text(dl, fSmall, ImVec2(o.x + px + pw - w, ry + 4), cMuted, t);
		}
		float* col = editCols[team][colorEdit];
		float before[3];
		memcpy(before, col, sizeof before);
		SyncHsv(team, colorEdit, col);
		float* h = hsv[team][colorEdit];
		bool active = false;
		ImGui::PushID(team * 2 + colorEdit + 700);
		bool moved = Picker(dl, ImVec2(o.x + px, ry + 40), ImVec2(pw, 270), h, active);
		ImGui::PopID();
		if (moved) ImGui::ColorConvertHSVtoRGB(h[0], h[1], h[2], col[0], col[1], col[2]);
		// Schnellwahl: gleichmaessig ueber die volle Breite verteilt
		static const float quick[][3] = {{1, 1, 1}, {0.05f, 0.05f, 0.06f}, {0.55f, 0.55f, 0.58f}, {0.9f, 0.1f, 0.12f}, {1, 0.45f, 0.05f}, {1, 0.82f, 0.1f},
			{0.35f, 0.85f, 0.2f}, {0.05f, 0.55f, 0.3f}, {0.15f, 0.75f, 1}, {0.1f, 0.3f, 0.95f}, {0.55f, 0.36f, 0.96f}, {1, 0.4f, 0.75f}};
		const int nq = 12;
		const float r = 11.0f;
		float qy = ry + 40 + 270 + 50, x0 = o.x + px + r, x1 = o.x + px + pw - r;
		Text(dl, fTiny, ImVec2(o.x + px, ry + 40 + 270 + 16), cFaint, "QUICK COLORS");
		for (int i = 0; i < nq; i++) {
			float qx = x0 + (x1 - x0) * i / (nq - 1);
			ImGui::SetCursorScreenPos(ImVec2(qx - 14, qy - 14));
			ImGui::PushID(i + 200);
			if (ImGui::InvisibleButton("##q", ImVec2(28, 28))) {
				memcpy(col, quick[i], sizeof before);
				hsvOk[team][colorEdit] = false;
				act.save = true;
			}
			float hv = Anim(ImGui::GetID("h"), ImGui::IsItemHovered() ? 1.0f : 0.0f, 22.0f);
			ImGui::PopID();
			bool same = fabsf(col[0] - quick[i][0]) < 0.004f && fabsf(col[1] - quick[i][1]) < 0.004f && fabsf(col[2] - quick[i][2]) < 0.004f;
			if (same) dl->AddCircle(ImVec2(qx, qy), r + 4, U(Rgb(255, 255, 255, 0.9f)), 28, 1.8f);
			Swatch(dl, ImVec2(qx, qy), r + 1.5f * hv, quick[i]);
		}
		bool changedCol = memcmp(before, col, sizeof before) != 0;
		if (changedCol) { act.changed = true; on = true; } // Farbe waehlen schaltet die eigenen Farben ein
		if (both && changedCol) memcpy(editCols[1][colorEdit], col, sizeof before); // beide Teams gleich
		if (editing && !active) act.save = act.changed = true; // Ziehen beendet: speichern
		editing = active;
	} else {
		// Lackierung waehlen (Liste wie bei den Items)
		int slot = colorEdit == 2 ? 7 : 12;
		Text(dl, fTitle, ImVec2(o.x + px, ry), cText, colorEdit == 2 ? "Primary finish" : "Accent finish");
		std::string q = Lower(finishSearch);
		std::vector<const items::Item*> shown;
		for (auto& it : items::list)
			if (it.slot == 7 && (q.empty() || Lower(it.name).find(q) != std::string::npos)) shown.push_back(&it);
		const float sy = ry + 38, sh = 42;
		Field(dl, "##fq", "Search", finishSearch, sizeof finishSearch, ImVec2(o.x + px, sy), ImVec2(o.x + px + pw, sy + sh), focusFinish, true, true);
		int cur = items::overrides.count(slot) ? items::overrides[slot] : 0;
		int pick = -1;
		const float ly = sy + sh + 12;
		ImGui::SetCursorScreenPos(ImVec2(o.x + px - 4, ly));
		ImGui::BeginChild("finishes", ImVec2(pw + 8, o.y + W.y - ly - 14), false, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse);
		SmoothScroll(fScrollTarget, fScrollLast);
		const float rw = pw - 8;
		auto ow = items::ownItems.find(slot);
		const items::Item* own = ow != items::ownItems.end() ? FindItem(ow->second) : nullptr;
		if (own) ItemRow(0, own->name.c_str(), "Your item", QualityColor(own->quality), cur, pick, rw);
		else ItemRow(0, "Your own", "", Rgb(80, 80, 92), cur, pick, rw);
		ImGui::Dummy(ImVec2(0, 2));
		ImGuiListClipper clip;
		clip.Begin((int)shown.size(), 46.0f + ImGui::GetStyle().ItemSpacing.y);
		while (clip.Step())
			for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++)
				ItemRow(shown[r]->id, shown[r]->name.c_str(), kQuality[std::clamp(shown[r]->quality, 0, 9)], QualityColor(shown[r]->quality), cur, pick, rw);
		ImGui::Dummy(ImVec2(0, 6));
		ImGui::EndChild();
		if (pick >= 0 && pick != cur) { act.finishSlot = slot; act.finishId = pick; }
	}
	if (act.changed) {
		act.on = on;
		act.both = both;
		memcpy(act.cols, editCols, sizeof act.cols);
	}
}

// ---------- Fenster ----------
// ---------- Maps ----------
static int mapArena = 0;           // gewaehlte Arena (Index in maps::arenas)
static char mapSearch[64] = {0};
static bool focusMapSearch = false;
static float mScrollTarget = 0.0f, mScrollLast = -1.0f;
static float arenaIndY = -1.0f;
static float mListFade = 1.0f;

struct MapAction {
	int kind = 0; // 1 tauschen, 2 Arena zuruecksetzen, 3 alle zuruecksetzen, 4 spielen, 5 Favorit an/aus
	std::wstring arena, path;
	std::string arenaU8;
};

// kleines Tausch-Symbol (zwei Pfeile)
static void SwapIcon(ImDrawList* d, ImVec2 c, float s, ImU32 col, float th = 1.5f) {
	d->AddLine(ImVec2(c.x - s, c.y - s * 0.45f), ImVec2(c.x + s, c.y - s * 0.45f), col, th);
	d->AddLine(ImVec2(c.x + s, c.y - s * 0.45f), ImVec2(c.x + s * 0.45f, c.y - s), col, th);
	d->AddLine(ImVec2(c.x + s, c.y + s * 0.45f), ImVec2(c.x - s, c.y + s * 0.45f), col, th);
	d->AddLine(ImVec2(c.x - s, c.y + s * 0.45f), ImVec2(c.x - s * 0.45f, c.y + s), col, th);
}
// Stern (gefuellt = Favorit)
static void Star(ImDrawList* d, ImVec2 c, float r, ImU32 col, bool filled, float th = 1.4f) {
	ImVec2 pts[10];
	for (int i = 0; i < 10; i++) {
		float a = -1.5707963f + i * 0.6283185f, rr = (i & 1) ? r * 0.45f : r;
		pts[i] = ImVec2(c.x + cosf(a) * rr, c.y + sinf(a) * rr);
	}
	if (filled) d->AddConcavePolyFilled(pts, 10, col);
	else d->AddPolyline(pts, 10, col, ImDrawFlags_Closed, th);
}
static void PlayIcon(ImDrawList* d, ImVec2 c, float s, ImU32 col) {
	d->AddTriangleFilled(ImVec2(c.x - s * 0.55f, c.y - s), ImVec2(c.x - s * 0.55f, c.y + s), ImVec2(c.x + s * 0.85f, c.y), col);
}

// Knopf mit Text (+ Symbol); strong = Akzentfarbe
static bool PillButton(ImDrawList* dl, const char* id, const char* label, ImVec2 p, float h, bool strong, bool enabled, int icon) {
	float w = TextW(fBold, label) + 32 + (icon ? 18 : 0);
	ImGui::SetCursorScreenPos(p);
	bool click = ImGui::InvisibleButton(id, ImVec2(w, h)) && enabled;
	float hv = Anim(ImGui::GetID(id), enabled && ImGui::IsItemHovered() ? 1.0f : 0.0f);
	float pr = Anim(ImGui::GetID(id) + 1, enabled && ImGui::IsItemActive() ? 1.0f : 0.0f, 30.0f);
	float in = pr * 1.2f;
	ImVec2 a(p.x + in, p.y + in), b(p.x + w - in, p.y + h - in);
	float en = enabled ? 1.0f : 0.4f;
	if (strong) {
		dl->AddRectFilled(a, b, U(Alpha(Mix(Mix(accent, Rgb(0, 0, 0), 0.06f), Mix(accent, Rgb(255, 255, 255), 0.10f), hv), en)), 11);
		FadeRect(dl, ImVec2(a.x + 1, a.y + 1), ImVec2(b.x - 1, b.y - 1), 10, 0, Rgb(255, 255, 255, 0.10f * en),
			[](float, float y) { return 1.0f - y * 2.0f; });
	} else {
		dl->AddRectFilled(a, b, U(Rgb(255, 255, 255, (0.04f + 0.06f * hv) * en)), 11);
		dl->AddRect(a, b, U(Rgb(255, 255, 255, (0.08f + 0.06f * hv) * en)), 11);
	}
	ImVec4 tc = strong ? Rgb(255, 255, 255, en) : Alpha(Mix(Hex(0xc9c9d3), cText, hv), en);
	float tx = p.x + 16;
	if (icon == 1) { PlayIcon(dl, ImVec2(tx + 5, p.y + h / 2), 5.0f, U(tc)); tx += 18; }
	if (icon == 2) { // Pfeil zurueck
		ImVec2 c(tx + 6, p.y + h / 2);
		dl->PathArcTo(c, 5.5f, 0.6f, 5.4f, 18);
		dl->PathStroke(U(tc), 0, 1.5f);
		ImVec2 tip(c.x + 5.5f * cosf(0.6f), c.y + 5.5f * sinf(0.6f));
		dl->AddLine(tip, ImVec2(tip.x + 3.2f, tip.y - 1.0f), U(tc), 1.5f);
		dl->AddLine(tip, ImVec2(tip.x - 0.4f, tip.y - 3.6f), U(tc), 1.5f);
		tx += 18;
	}
	Text(dl, fBold, ImVec2(tx, p.y + (h - fBold->FontSize) / 2 - 1), tc, label);
	return click;
}
static float PillW(const char* label, int icon) { return TextW(fBold, label) + 32 + (icon ? 18 : 0); }

// Platzhalter ohne Vorschaubild: dunkle Flaeche mit feinem Prisma-Schimmer
static void ThumbHolder(ImDrawList* d, ImVec2 a, ImVec2 b, unsigned seed) {
	d->AddRectFilled(a, b, U(cPanel2), 8);
	float t = (seed % 97) / 97.0f;
	ImVec4 c0 = SpecAt(t), c1 = SpecAt(fmodf(t + 0.35f, 1.0f));
	GradRect(d, a, b, 8, c0, c1, 0.14f);
	ImVec2 c((a.x + b.x) / 2, (a.y + b.y) / 2);
	float s = (b.y - a.y) * 0.22f;
	d->AddTriangle(ImVec2(c.x, c.y - s), ImVec2(c.x + s * 1.05f, c.y + s * 0.8f), ImVec2(c.x - s * 1.05f, c.y + s * 0.8f), U(Rgb(255, 255, 255, 0.22f)), 1.4f);
}


// ---------- Merge: Kulisse einer Map in eine Arena einbauen ----------
static int mgSel = -1;                 // gewaehlte Arena (Index in merge::list)
static char mgSearch[64] = {0};
static bool mgFocus = false;
static float mgScrollA = 0, mgLastA = -1, mgScrollB = 0, mgLastB = -1;
struct MergeAction { int kind = 0; std::string file, backdrop, arenaU8; bool replace = false; };

static void MergeBody(ImDrawList* dl, ImVec2 o, ImVec2 W, float top, float pad, MergeAction& act) {
	merge::Refresh();
	std::lock_guard<std::mutex> lk(merge::mu);
	auto& L = merge::list;
	const float leftW = 290, listTop = top + 14;
	if (!merge::loaded) {
		ImVec2 c(o.x + W.x / 2, o.y + top + (W.y - top) / 2 - 30);
		Spinner(dl, c, 15);
		const char* a = "Prism is reading your arenas …";
		const char* b = "Keep the Prism launcher open (it can sit in the tray).";
		Text(dl, fRegular, ImVec2(c.x - TextW(fRegular, a) / 2, c.y + 28), cMuted, a);
		Text(dl, fSmall, ImVec2(c.x - TextW(fSmall, b) / 2, c.y + 52), cFaint, b);
		return;
	}
	if (mgSel >= (int)L.size()) mgSel = -1;
	if (mgSel < 0)
		for (int i = 0; i < (int)L.size(); i++) if (L[i].base) { mgSel = i; break; }
	int merged = 0;
	for (auto& a : L) if (!a.want.empty()) merged++;

	// ---------- Links: Arenen ----------
	dl->AddLine(ImVec2(o.x + pad + leftW + 14, o.y + top + 1), ImVec2(o.x + pad + leftW + 14, o.y + W.y - 1), U(cLine));
	Field(dl, "##mgq", "Search arenas", mgSearch, sizeof mgSearch, ImVec2(o.x + pad, o.y + listTop - 2), ImVec2(o.x + pad + leftW, o.y + listTop + 38), mgFocus, true, true);
	std::string q = Lower(mgSearch);
	float ly = listTop + 48;
	float bottom = merged ? 54.0f : 14.0f;
	ImGui::SetCursorScreenPos(ImVec2(o.x + pad - 4, o.y + ly));
	ImGui::BeginChild("mgarenas", ImVec2(leftW + 8, W.y - ly - bottom), false, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse);
	SmoothScroll(mgScrollA, mgLastA);
	ImDrawList* cd = ImGui::GetWindowDrawList();
	const float rh = 52;
	// gemergte Arenen ganz oben, dann der Rest (jeweils nach Namen)
	std::vector<int> order;
	for (int pass = 0; pass < 2; pass++)
		for (int i = 0; i < (int)L.size(); i++)
			if (L[i].want.empty() == (pass == 1)) order.push_back(i);
	bool needRestart = false;
	for (auto& a : L) if (merge::NeedsRestart(a)) needRestart = true;
	for (int i : order) {
		auto& a = L[i];
		if (!a.base || (!q.empty() && a.lower.find(q) == std::string::npos)) continue;
		ImGui::PushID(i + 31000);
		ImVec2 p = ImGui::GetCursorScreenPos();
		bool click = ImGui::InvisibleButton("##a", ImVec2(leftW, rh));
		bool sel = mgSel == i;
		float h = Anim(ImGui::GetID("h"), ImGui::IsItemHovered() && !sel ? 1.0f : 0.0f);
		float sa = Anim(ImGui::GetID("s"), sel ? 1.0f : 0.0f);
		ImGui::PopID();
		if (click) mgSel = i;
		ImVec2 q2(p.x + leftW, p.y + rh);
		if (h > 0.01f || sa > 0.01f) cd->AddRectFilled(p, q2, U(Mix(Alpha(cHover, h), Acc(0.12f), sa)), 12);
		if (sa > 0.01f) cd->AddRectFilled(ImVec2(p.x, p.y + 13), ImVec2(p.x + 3, q2.y - 13), U(Alpha(accent, sa)), 2);
		bool on = !a.want.empty();
		ImVec2 ic(p.x + 14, p.y + 11), iq(ic.x + 30, ic.y + 30);
		if (on) { GradRect(cd, ic, iq, 9, spec[0], spec[2], 0.32f); cd->AddRect(ic, iq, U(Alpha(spec[1], 0.45f)), 9, 0, 1.0f); }
		else { cd->AddRectFilled(ic, iq, U(Mix(cPanel2, cHover, h)), 9); cd->AddRect(ic, iq, U(cLine2), 9, 0, 1.0f); }
		// Symbol: zwei Berge (Kulisse)
		{
			ImVec2 c(ic.x + 15, ic.y + 17);
			ImU32 lc = U(on ? Rgb(255, 255, 255, 0.92f) : Mix(cFaint, cMuted, std::max(h, sa)));
			cd->AddTriangleFilled(ImVec2(c.x - 9, c.y + 5), ImVec2(c.x - 3, c.y - 5), ImVec2(c.x + 3, c.y + 5), lc);
			cd->AddTriangleFilled(ImVec2(c.x - 1, c.y + 5), ImVec2(c.x + 4, c.y - 2), ImVec2(c.x + 9, c.y + 5), lc);
		}
		float tx = p.x + 56;
		Text(cd, fBold, ImVec2(tx, p.y + 9), Mix(Hex(0xc9c9d3), cText, std::max(sa, h)), Fit(fBold, fBold->FontSize, a.name, leftW - 66).c_str());
		std::string sub = "Original";
		ImVec4 sc = cFaint;
		if (on) {
			sub = a.want == "-" ? "Own backdrop off" : "+ " + a.wantName + (a.replace ? " (own off)" : "");
			sc = Mix(spec[0], Rgb(255, 255, 255), 0.35f);
			if (merge::NeedsRestart(a)) sub += "  \xC2\xB7  restart";
			else if (a.state == "pending") sub += "  \xC2\xB7  waiting";
			else if (a.state == "error") { sub = "Couldn't merge"; sc = Rgb(255, 140, 140); }
		}
		Text(cd, fSmall, ImVec2(tx, p.y + 29), sc, Fit(fSmall, fSmall->FontSize, sub, leftW - 66).c_str());
	}
	ImGui::Dummy(ImVec2(0, 6));
	ImGui::EndChild();
	if (merged) {
		float rx = o.x + pad + 4;
		if (needRestart) {
			bool asked = merge::restartAsked && GetTickCount64() - merge::restartAsked < 60000;
			const char* lbl = asked ? "Restarting …" : "Restart game";
			if (PillButton(dl, "##mgrestart", lbl, ImVec2(rx, o.y + W.y - 46), 34, true, !asked, 1)) act.kind = 5;
			rx += PillW(lbl, 1) + 8;
		}
		if (PillButton(dl, "##mgrestall", "Restore all", ImVec2(rx, o.y + W.y - 46), 34, false, true, 2)) act.kind = 3;
	}

	// ---------- Rechts ----------
	const float px = pad + leftW + 30, pw = W.x - px - pad;
	if (mgSel < 0) { Text(dl, fRegular, ImVec2(o.x + px, o.y + listTop + 4), cMuted, "No arenas found."); return; }
	auto& cur = L[mgSel];
	bool curOn = !cur.want.empty();
	Text(dl, fTitle, ImVec2(o.x + px, o.y + listTop - 2), cText, Fit(fTitle, fTitle->FontSize, cur.name, pw - 220).c_str());
	{
		float bx = o.x + px + pw;
		float w1 = PillW("Play", 1);
		bx -= w1;
		if (merge::NeedsRestart(cur)) {
			bool asked = merge::restartAsked && GetTickCount64() - merge::restartAsked < 60000;
			const char* lbl = asked ? "Restarting …" : "Restart game";
			w1 = PillW(lbl, 1);
			bx = o.x + px + pw - w1;
			if (PillButton(dl, "##mgplay", lbl, ImVec2(bx, o.y + listTop - 6), 34, true, !asked, 1)) act.kind = 5;
		} else if (PillButton(dl, "##mgplay", "Play", ImVec2(bx, o.y + listTop - 6), 34, true, true, 1)) { act.kind = 4; act.arenaU8 = cur.file; }
		if (curOn) {
			float w2 = PillW("Restore", 2);
			bx -= w2 + 8;
			if (PillButton(dl, "##mgrest", "Restore", ImVec2(bx, o.y + listTop - 6), 34, false, true, 2)) { act.kind = 2; act.file = cur.file; }
		}
	}
	// Zustand
	{
		float y = o.y + listTop + 30, x = o.x + px;
		std::string line;
		ImVec4 col = cMuted;
		if (!curOn) line = "Original. Pick a backdrop below to merge it in.";
		else if (cur.state == "applied") {
			line = cur.want == "-" ? "Own backdrop is off. Start Freeplay here to see it." : "Backdrop of " + cur.wantName + " is in. Start Freeplay here to see it.";
			col = Rgb(120, 220, 160);
		}
		else if (cur.state == "pending") { line = cur.stateMsg.empty() ? "Waiting …" : cur.stateMsg; col = Rgb(255, 200, 120); }
		else if (cur.state == "error") { line = cur.stateMsg; col = Rgb(255, 140, 140); }
		else { Spinner(dl, ImVec2(x + 7, y + 8), 6); x += 22; line = "Prism is merging … (the Prism launcher has to be open)"; }
		if (col.x != cMuted.x || cur.state == "applied") { dl->AddCircleFilled(ImVec2(x + 4, y + 8), 3.5f, U(col), 12); x += 14; }
		Text(dl, fSmall, ImVec2(x, y), col, Fit(fSmall, fSmall->FontSize, line, o.x + px + pw - x).c_str());
	}
	// Schalter: eigene Kulisse entfernen
	{
		float y = o.y + listTop + 58;
		ImGui::SetCursorScreenPos(ImVec2(o.x + px, y));
		bool tog = ImGui::InvisibleButton("##mgrepl", ImVec2(pw, 30)) && cur.own;
		float hv = Anim(ImGui::GetID("mgreplh"), ImGui::IsItemHovered() && cur.own ? 1.0f : 0.0f);
		bool val = curOn && (cur.replace || cur.want == "-");
		if (tog) {
			val = !val;
			act.file = cur.file;
			bool foreign = curOn && cur.want != "-";
			if (foreign) { act.kind = 1; act.backdrop = cur.want; act.replace = val; }
			else if (val) { act.kind = 1; act.backdrop = "-"; act.replace = true; } // nur die eigene Kulisse aus
			else act.kind = 2;                                                      // wieder Original
		}
		if (!cur.own) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.4f);
		float k = Anim(ImGui::GetID("mgreplk"), val ? 1.0f : 0.0f, 16.0f);
		ImVec2 t0(o.x + px, y + 6), t1(t0.x + 34, t0.y + 18);
		dl->AddRectFilled(t0, t1, U(Mix(Rgb(255, 255, 255, 0.10f + 0.05f * hv), accent, k)), 9);
		dl->AddCircleFilled(ImVec2(t0.x + 9 + 16 * k, t0.y + 9), 6.5f, U(Rgb(255, 255, 255)), 16);
		Text(dl, fBold, ImVec2(t1.x + 12, y + 6), Mix(cMuted, cText, std::max(hv, k)), cur.own ? "Turn off the arena's own backdrop" : "Own backdrop is part of the arena (can't be turned off)");
		if (!cur.own) ImGui::PopStyleVar();
	}
	Text(dl, fTiny, ImVec2(o.x + px, o.y + listTop + 96), cFaint, "BACKDROPS");
	float by = listTop + 116;
	ImGui::SetCursorScreenPos(ImVec2(o.x + px - 4, o.y + by));
	ImGui::BeginChild("mgbackdrops", ImVec2(pw + 8, W.y - by - 30), false, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse);
	SmoothScroll(mgScrollB, mgLastB);
	cd = ImGui::GetWindowDrawList();
	const float rw = pw - 8, rh2 = 46;
	int shown = 0;
	for (int i = 0; i < (int)L.size(); i++) {
		auto& b = L[i];
		if (!b.backdrop || i == mgSel) continue;
		shown++;
		ImGui::PushID(i + 32000);
		ImVec2 p = ImGui::GetCursorScreenPos();
		bool click = ImGui::InvisibleButton("##b", ImVec2(rw, rh2));
		bool here = Lower(cur.want) == Lower(b.file);
		float h = Anim(ImGui::GetID("h"), ImGui::IsItemHovered() ? 1.0f : 0.0f, 22.0f);
		float s = Anim(ImGui::GetID("s"), here ? 1.0f : 0.0f, 16.0f);
		ImGui::PopID();
		if (click && !here) { act.kind = 1; act.file = cur.file; act.backdrop = b.file; act.replace = curOn && (cur.replace || cur.want == "-"); }
		ImVec2 q2(p.x + rw, p.y + rh2);
		if (h > 0.01f || s > 0.01f) cd->AddRectFilled(p, q2, U(Mix(Alpha(cHover, h), Acc(0.13f), s)), 11);
		if (s > 0.01f) cd->AddRect(p, q2, U(Acc(0.35f * s)), 11, 0, 1.0f);
		Text(cd, fBold, ImVec2(p.x + 14 + 2 * h, p.y + 13), Mix(Hex(0xd4d4dc), cText, std::max(h, s)), Fit(fBold, fBold->FontSize, b.name, rw - 150).c_str());
		const char* badge = here ? (cur.state == "applied" ? "Active" : "Selected") : (h > 0.5f ? "Merge" : nullptr);
		if (badge) {
			float bw = TextW(fTiny, badge) + 18;
			ImVec2 b0(q2.x - 12 - bw, p.y + rh2 / 2 - 11), b1(q2.x - 12, p.y + rh2 / 2 + 11);
			cd->AddRectFilled(b0, b1, U(here ? Acc(0.22f) : Alpha(accent, h)), 11);
			Text(cd, fTiny, ImVec2(b0.x + 9, b0.y + 4), here ? Mix(accent, Rgb(255, 255, 255), 0.55f) : Rgb(255, 255, 255), badge);
		}
	}
	if (!shown) {
		ImVec2 p = ImGui::GetCursorScreenPos();
		Text(cd, fSmall, ImVec2(p.x + 14, p.y + 14), cMuted, "No backdrops found yet.");
		ImGui::Dummy(ImVec2(0, 40));
	}
	ImGui::Dummy(ImVec2(0, 6));
	ImGui::EndChild();
	Text(dl, fTiny, ImVec2(o.x + px, o.y + W.y - 24), cFaint, "Only without Anti-Cheat. Prism puts everything back before you play with Anti-Cheat.");
}

static void MapsBody(ImDrawList* dl, ImVec2 o, ImVec2 W, float top, float pad, MapAction& act) {
	std::lock_guard<std::mutex> mk(maps::mu);
	const float leftW = 250, listTop = top + 14, rowH = 58;
	const bool busy = maps::Busy().load();
	const double now = GetTickCount64() / 1000.0;
	auto& ar = maps::arenas;
	if (mapArena >= (int)ar.size()) mapArena = 0;

	// Noch nicht eingelesen / Ordner fehlt
	if (!maps::scanned || !maps::found) {
		ImVec2 c(o.x + W.x / 2, o.y + top + (W.y - top) / 2 - 24);
		const char* m = !maps::scanned ? "Looking for maps …" : "Rocket League folder not found.";
		if (!maps::scanned) Spinner(dl, c, 15);
		float mw = TextW(fRegular, m);
		Text(dl, fRegular, ImVec2(c.x - mw / 2, c.y + 28), cMuted, m);
		return;
	}

	// ---------- Links: Arenen ----------
	dl->AddLine(ImVec2(o.x + pad + leftW + 14, o.y + top + 1), ImVec2(o.x + pad + leftW + 14, o.y + W.y - 1), U(cLine));
	Text(dl, fTiny, ImVec2(o.x + pad + 16, o.y + listTop), cFaint, "ARENAS");
	float selY = -1;
	int swappedCount = 0;
	ImDrawListSplitter split;
	split.Split(dl, 2);
	split.SetCurrentChannel(dl, 1);
	for (int i = 0; i < (int)ar.size(); i++) {
		const auto& a = ar[i];
		if (!a.swap.empty()) swappedCount++;
		ImVec2 p(o.x + pad, o.y + listTop + 22 + i * rowH);
		ImGui::SetCursorScreenPos(p);
		ImGui::PushID(i + 7000);
		bool click = ImGui::InvisibleButton("##ar", ImVec2(leftW, rowH));
		bool sel = mapArena == i;
		float h = Anim(ImGui::GetID("h"), ImGui::IsItemHovered() && !sel ? 1.0f : 0.0f);
		float sa = Anim(ImGui::GetID("sa"), sel ? 1.0f : 0.0f);
		bool working = busy && (maps::busyArena == a.fileU8 || maps::busyArena == "*");
		ImGui::PopID();
		if (click && !sel) {
			mapArena = i;
			mListFade = 0.0f;
		}
		if (sel) selY = p.y;
		if (h > 0.01f) dl->AddRectFilled(p, ImVec2(p.x + leftW, p.y + rowH), U(Alpha(cHover, h)), 12);
		// Symbol: Kachel mit Zustand
		ImVec2 ic(p.x + 16, p.y + 13), iq(ic.x + 32, ic.y + 32);
		bool sw = !a.swap.empty();
		if (sw) {
			GradRect(dl, ic, iq, 9, spec[0], spec[2], 0.30f + 0.08f * std::max(h, sa));
			dl->AddRect(ic, iq, U(Alpha(spec[1], 0.45f)), 9, 0, 1.0f);
		} else {
			dl->AddRectFilled(ic, iq, U(Mix(cPanel2, cHover, h)), 9);
			dl->AddRect(ic, iq, U(cLine2), 9, 0, 1.0f);
		}
		ImVec2 cc(ic.x + 16, ic.y + 16);
		if (working) Spinner(dl, cc, 7);
		else if (sw) SwapIcon(dl, cc, 6.0f, U(Rgb(255, 255, 255, 0.9f)));
		else { // kleines Stadion: Oval mit Mittellinie
			ImU32 lc = U(Mix(cFaint, cMuted, std::max(h, sa)));
			dl->AddEllipse(cc, ImVec2(8.5f, 6.0f), lc, 0, 20, 1.3f);
			dl->AddLine(ImVec2(cc.x, cc.y - 6), ImVec2(cc.x, cc.y + 6), lc, 1.2f);
		}
		float tx = p.x + 60;
		Text(dl, fBold, ImVec2(tx, p.y + 11), Mix(Hex(0xc9c9d3), cText, std::max(sa, h)), a.name.c_str());
		std::string sub;
		ImVec4 sc = cFaint;
		if (working) { sub = maps::busyMap.empty() ? "Restoring …" : "Copying map …"; sc = cMuted; }
		else if (sw) { sub = a.swapTitle; sc = Mix(spec[0], Rgb(255, 255, 255), 0.35f); }
		else if (a.locked) { sub = "Loaded right now"; sc = cFaint; }
		else { sub = "Original"; sc = cFaint; }
		sub = Fit(fSmall, fSmall->FontSize, sub, leftW - 76);
		Text(dl, fSmall, ImVec2(tx, p.y + 31), sc, sub.c_str());
		if (a.locked && !sw) { // Schloss
			ImVec2 l(p.x + leftW - 22, p.y + rowH / 2);
			ImU32 lc = U(cFaint);
			dl->AddRect(ImVec2(l.x - 5, l.y - 1), ImVec2(l.x + 5, l.y + 6), lc, 2, 0, 1.3f);
			dl->PathArcTo(ImVec2(l.x, l.y - 2), 3.2f, 3.14159f, 6.28318f, 10);
			dl->PathStroke(lc, 0, 1.3f);
		}
	}
	split.SetCurrentChannel(dl, 0);
	if (selY >= 0) {
		float rel = selY - o.y;
		arenaIndY = arenaIndY < 0 ? rel : Approach(arenaIndY, rel, 20.0f);
		ImVec2 a(o.x + pad, o.y + arenaIndY), b(o.x + pad + leftW, o.y + arenaIndY + rowH);
		dl->AddRectFilled(a, b, U(Acc(0.12f)), 12);
		dl->AddRectFilled(ImVec2(a.x, a.y + 14), ImVec2(a.x + 3, b.y - 14), U(accent), 2);
	}
	split.Merge(dl);

	// alle zuruecksetzen
	float by = o.y + listTop + 22 + ar.size() * rowH + 10;
	{
		float ra = Anim(ImGui::GetID("restall"), swappedCount > 0 ? 1.0f : 0.0f, 14.0f);
		if (ra > 0.01f) {
			ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * ra);
			if (PillButton(dl, "##restall", "Restore all", ImVec2(o.x + pad + 8, by), 34, false, swappedCount > 0 && !busy, 2)) act.kind = 3;
			ImGui::PopStyleVar();
		}
	}
	// So geht's
	{
		const char* steps[3] = {"Pick an arena", "Pick a map", "Start Freeplay in that arena"};
		float sy = o.y + W.y - 22 - 3 * 24;
		for (int i = 0; i < 3; i++) {
			ImVec2 c(o.x + pad + 26, sy + i * 24 + 9);
			dl->AddCircleFilled(c, 9, U(Rgb(255, 255, 255, 0.05f)), 20);
			char n[4];
			snprintf(n, sizeof n, "%d", i + 1);
			float nw = TextW(fTiny, n);
			Text(dl, fTiny, ImVec2(c.x - nw / 2, c.y - fTiny->FontSize / 2 - 1), cMuted, n);
			Text(dl, fSmall, ImVec2(c.x + 18, c.y - fSmall->FontSize / 2 - 1), cFaint, steps[i]);
		}
	}

	// ---------- Rechts ----------
	const float px = pad + leftW + 30, pw = W.x - px - pad;
	if (ar.empty()) {
		Text(dl, fRegular, ImVec2(o.x + px, o.y + listTop + 4), cMuted, "No swappable arenas found.");
		return;
	}
	const auto& cur = ar[mapArena];
	const bool curSw = !cur.swap.empty();
	const bool curWorking = busy && (maps::busyArena == cur.fileU8 || maps::busyArena == "*");

	// Kopf: Arena-Name + Knoepfe
	Text(dl, fTitle, ImVec2(o.x + px, o.y + listTop - 2), cText, cur.name.c_str());
	{
		float bx = o.x + px + pw;
		float ba = Anim(ImGui::GetID("hdrbtn"), curSw ? 1.0f : 0.0f, 16.0f);
		if (ba > 0.01f) {
			ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * ba);
			float w1 = PillW("Play", 1);
			bx -= w1;
			if (PillButton(dl, "##play", "Play", ImVec2(bx, o.y + listTop - 6), 34, true, curSw && !busy, 1)) act.kind = 4;
			float w2 = PillW("Restore", 2);
			bx -= w2 + 8;
			if (PillButton(dl, "##rest1", "Restore", ImVec2(bx, o.y + listTop - 6), 34, false, curSw && !busy, 2)) act.kind = 2;
			ImGui::PopStyleVar();
		}
	}
	// Zeile darunter: Zustand oder letzte Meldung
	{
		float y = o.y + listTop + 30;
		bool fresh = now - maps::msgAt < 6.0 && !maps::msg.empty();
		std::string line;
		ImVec4 col = cMuted;
		float x = o.x + px;
		if (curWorking) {
			Spinner(dl, ImVec2(x + 7, y + 8), 6);
			x += 22;
			line = maps::busyMap.empty() ? "Restoring the original arena …" : "Copying the map into " + cur.name + " …";
		} else if (fresh) {
			col = maps::msgErr ? Rgb(255, 140, 140) : Rgb(120, 220, 160);
			dl->AddCircleFilled(ImVec2(x + 4, y + 8), 3.5f, U(col), 12);
			x += 14;
			line = maps::msg;
		} else if (curSw) {
			line = "Now: " + cur.swapTitle;
			col = Mix(cMuted, cText, 0.3f);
		} else if (cur.locked) {
			line = "Rocket League has this arena loaded right now. Pick another one.";
			col = cFaint;
		} else line = "Original arena. Pick a map to put in its place.";
		line = Fit(fSmall, fSmall->FontSize, line, o.x + px + pw - x);
		Text(dl, fSmall, ImVec2(x, y), col, line.c_str());
	}

	// Suche
	const float sy = listTop + 56, sh = 42;
	Field(dl, "##mq", "Search maps", mapSearch, sizeof mapSearch, ImVec2(o.x + px, o.y + sy), ImVec2(o.x + px + pw, o.y + sy + sh), focusMapSearch, true, true);

	std::string q = Lower(mapSearch);
	std::vector<const maps::Map*> shown;
	for (auto& m : maps::list)
		if (q.empty() || m.lower.find(q) != std::string::npos) shown.push_back(&m);
	// wo steckt welche Map gerade?
	auto whereIs = [&](const std::string& name) -> int {
		std::string ln = Lower(name);
		for (int i = 0; i < (int)ar.size(); i++)
			if (!ar[i].swap.empty() && Lower(ar[i].swap) == ln) return i;
		return -1;
	};

	mListFade = Approach(mListFade, 1.0f, 12.0f);
	const float ly = sy + sh + 12;
	ImGui::SetCursorScreenPos(ImVec2(o.x + px - 4, o.y + ly + (1.0f - EaseOut(mListFade)) * 8.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * EaseOut(mListFade));
	ImGui::BeginChild("maprows", ImVec2(pw + 8, W.y - ly - 14), false, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse);
	SmoothScroll(mScrollTarget, mScrollLast);
	ImDrawList* cd = ImGui::GetWindowDrawList();
	const float rw = pw - 8, rh = 66;
	if (maps::list.empty()) {
		ImVec2 p = ImGui::GetCursorScreenPos();
		Text(cd, fBold, ImVec2(p.x + 14, p.y + 22), cText, "No maps yet");
		Text(cd, fSmall, ImVec2(p.x + 14, p.y + 46), cMuted, "Download maps in Prism under Browse Maps,");
		Text(cd, fSmall, ImVec2(p.x + 14, p.y + 66), cMuted, "or subscribe to some in the Steam Workshop.");
		ImGui::Dummy(ImVec2(0, 96));
	}
	ImGuiListClipper clip;
	clip.Begin((int)shown.size(), rh + ImGui::GetStyle().ItemSpacing.y);
	while (clip.Step())
		for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
			const maps::Map& m = *shown[r];
			ImGui::PushID(r + 20000);
			ImVec2 p = ImGui::GetCursorScreenPos();
			// Stern zuerst (hat Vorrang vor der Zeile)
			ImVec2 stc(p.x + rw - 26, p.y + rh / 2);
			ImGui::SetCursorScreenPos(ImVec2(stc.x - 15, stc.y - 15));
			bool starClick = ImGui::InvisibleButton("##fav", ImVec2(30, 30));
			bool starHov = ImGui::IsItemHovered();
			ImGui::SetCursorScreenPos(p);
			bool click = ImGui::InvisibleButton("##m", ImVec2(rw, rh));
			bool rowHov = ImGui::IsItemHovered() || starHov;
			if (starClick) { act.kind = 5; act.path = m.path; }
			int in = whereIs(m.name);
			bool here = in == mapArena;
			bool working = busy && Lower(maps::busyMap) == Lower(m.name);
			bool canSwap = !busy && !here && !cur.locked;
			float h = Anim(ImGui::GetID("h"), rowHov ? 1.0f : 0.0f, 22.0f);
			float s = Anim(ImGui::GetID("s"), here ? 1.0f : 0.0f, 16.0f);
			float fs = Anim(ImGui::GetID("fs"), m.fav ? 1.0f : 0.0f, 14.0f);
			float sh2 = Anim(ImGui::GetID("sh"), starHov ? 1.0f : 0.0f, 22.0f);
			ImGui::PopID();
			if (click && canSwap) {
				act.kind = 1;
				act.arena = cur.file;
				act.path = m.path;
			}
			ImVec2 q(p.x + rw, p.y + rh);
			if (h > 0.01f || s > 0.01f) cd->AddRectFilled(p, q, U(Mix(Alpha(cHover, h), Acc(0.13f), s)), 12);
			if (s > 0.01f) cd->AddRect(p, q, U(Acc(0.35f * s)), 12, 0, 1.0f);
			// Vorschaubild
			ImVec2 ta(p.x + 8, p.y + 7), tb(ta.x + 92, ta.y + 52);
			unsigned long long tex = maps::Thumb(m.id);
			if (tex) {
				float z = 1.0f + 0.06f * h; // beim Drueberfahren leicht hineinzoomen
				float du = (1.0f - 1.0f / z) * 0.5f;
				cd->AddImageRounded((ImTextureID)tex, ta, tb, ImVec2(du, du), ImVec2(1 - du, 1 - du), U(Rgb(255, 255, 255)), 8);
			} else {
				unsigned seed = 0;
				for (char ch : m.name) seed = seed * 31 + (unsigned char)ch;
				ThumbHolder(cd, ta, tb, seed);
			}
			cd->AddRect(ta, tb, U(Rgb(255, 255, 255, 0.07f)), 8);
			// Text
			// Stern: Favorit gold, sonst nur beim Drueberfahren
			{
				ImVec4 gold = Rgb(255, 196, 77);
				float vis = std::max(fs, h * 0.9f);
				if (vis > 0.01f) {
					if (sh2 > 0.01f) cd->AddCircleFilled(stc, 14, U(Rgb(255, 255, 255, 0.07f * sh2)), 24);
					float pop = 1.0f + 0.25f * sinf(std::clamp(fs, 0.0f, 1.0f) * 3.14159f) * (m.fav ? 1.0f : 0.0f);
					if (fs > 0.01f) Star(cd, stc, 7.5f * pop, U(Alpha(gold, fs)), true);
					if (fs < 0.99f) Star(cd, stc, 7.5f, U(Alpha(Mix(cFaint, cText, sh2), vis * (1.0f - fs))), false);
				}
			}
			float tx = tb.x + 14, right = q.x - 46;
			float badgeW = 0;
			const char* badge = nullptr;
			std::string inName;
			if (here) badge = "Active";
			else if (in >= 0) { inName = "In " + ar[in].name; badge = inName.c_str(); }
			if (badge) badgeW = TextW(fTiny, badge) + 18;
			bool hoverSwap = canSwap && h > 0.01f;
			float swapW = TextW(fBold, "Swap") + 46;
			float textR = right - std::max(badge && !hoverSwap ? badgeW + 10 : 0.0f, hoverSwap ? swapW * h + 10 : 0.0f);
			std::string title = Fit(fBold, fBold->FontSize, m.title, textR - tx);
			Text(cd, fBold, ImVec2(tx + 2 * h, p.y + 15), Mix(Hex(0xd4d4dc), cText, std::max(h, s)), title.c_str());
			char sz[32];
			snprintf(sz, sizeof sz, "%.1f MB", m.mb);
			std::string sub = m.author.empty() ? std::string(sz) : m.author + "  \xC2\xB7  " + sz;
			sub = Fit(fSmall, fSmall->FontSize, sub, textR - tx);
			Text(cd, fSmall, ImVec2(tx + 2 * h, p.y + 37), cFaint, sub.c_str());
			// rechts: Zustand / Tauschen
			if (working) Spinner(cd, ImVec2(right - 10, p.y + rh / 2), 8);
			else if (badge && h < 0.99f) {
				float ba = hoverSwap ? 1.0f - h : 1.0f;
				ImVec2 b0(right - badgeW, p.y + rh / 2 - 11), b1(right, p.y + rh / 2 + 11);
				ImVec4 bc = here ? accent : cMuted;
				cd->AddRectFilled(b0, b1, U(Alpha(here ? Acc(0.22f) : Rgb(255, 255, 255, 0.06f), ba)), 11);
				Text(cd, fTiny, ImVec2(b0.x + 9, b0.y + 4), Alpha(Mix(bc, Rgb(255, 255, 255), 0.55f), ba), badge);
			}
			if (hoverSwap && !working) {
				ImVec2 b0(right - swapW, p.y + rh / 2 - 15), b1(right, p.y + rh / 2 + 15);
				cd->AddRectFilled(b0, b1, U(Alpha(accent, h)), 10);
				SwapIcon(cd, ImVec2(b0.x + 18, (b0.y + b1.y) / 2), 5.0f, U(Rgb(255, 255, 255, h)), 1.4f);
				Text(cd, fBold, ImVec2(b0.x + 32, b0.y + 6), Rgb(255, 255, 255, h), "Swap");
			}
		}
	if (!q.empty() && shown.empty() && !maps::list.empty()) {
		ImVec2 p = ImGui::GetCursorScreenPos();
		std::string m = "Nothing matches \"" + std::string(mapSearch) + "\"";
		Text(cd, fRegular, ImVec2(p.x + 14, p.y + 18), cMuted, Fit(fRegular, fRegular->FontSize, m, rw - 28).c_str());
		ImGui::Dummy(ImVec2(0, 50));
	}
	ImGui::Dummy(ImVec2(0, 6));
	{
		ImVec2 wp2 = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
		float sc = ImGui::GetScrollY(), mx = ImGui::GetScrollMaxY();
		float ft = std::min(sc, 14.0f) / 14.0f, fb = std::min(mx - sc, 14.0f) / 14.0f;
		if (ft > 0) cd->AddRectFilledMultiColor(wp2, ImVec2(wp2.x + ws.x, wp2.y + 18), U(Alpha(cBg, ft)), U(Alpha(cBg, ft)), U(Alpha(cBg, 0)), U(Alpha(cBg, 0)));
		if (fb > 0) cd->AddRectFilledMultiColor(ImVec2(wp2.x, wp2.y + ws.y - 18), ImVec2(wp2.x + ws.x, wp2.y + ws.y), U(Alpha(cBg, 0)), U(Alpha(cBg, 0)), U(Alpha(cBg, fb)), U(Alpha(cBg, fb)));
	}
	ImGui::EndChild();
	ImGui::PopStyleVar();
	if (act.kind == 2 || act.kind == 4) { act.arena = cur.file; act.arenaU8 = cur.fileU8; }
}

// ---------- Tweaks (nur im Freeplay) ----------
static float SliderPos(int i) {
	const auto& t = tweaks::list[i];
	return std::clamp((t.value - t.min) / (t.max - t.min), 0.0f, 1.0f);
}

static void TweaksBody(ImDrawList* dl, ImVec2 o, ImVec2 W, float top, float pad, bool& resetAll) {
	const float listTop = top + 14;
	Text(dl, fTitle, ImVec2(o.x + pad + 2, o.y + listTop - 2), cText, "Tweaks");
	Text(dl, fSmall, ImVec2(o.x + pad + 2, o.y + listTop + 30), cMuted, "In Freeplay and Training. Everything is back to normal when you leave.");
	{
		bool any = tweaks::Changed();
		float w = PillW("Reset all", 2);
		float ra = Anim(ImGui::GetID("twreset"), any ? 1.0f : 0.35f, 14.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * ra);
		if (PillButton(dl, "##twreset", "Reset all", ImVec2(o.x + W.x - pad - w, o.y + listTop - 4), 34, false, any, 2)) resetAll = true;
		ImGui::PopStyleVar();
	}
	const float cx = o.x + pad, cw = W.x - pad * 2, ch = 70, gap = 10;
	float y = o.y + listTop + 62;
	for (int i = 0; i < tweaks::count; i++, y += ch + gap) {
		const auto& t = tweaks::list[i];
		ImGui::PushID(i + 9000);
		ImVec2 a(cx, y), b(cx + cw, y + ch);
		bool changed = fabsf(t.value - t.def) > 0.001f;
		float hov = Anim(ImGui::GetID("cardh"), ImGui::IsMouseHoveringRect(a, b, false) ? 1.0f : 0.0f);
		dl->AddRectFilled(a, b, U(Mix(cPanel, cPanel2, hov)), 14);
		dl->AddRect(a, b, U(Mix(cLine, changed ? Acc(0.45f) : cLine2, std::max(hov * 0.6f, changed ? 1.0f : 0.0f))), 14);
		// Text links
		Text(dl, fBold, ImVec2(a.x + 18, a.y + 15), cText, t.label);
		Text(dl, fSmall, ImVec2(a.x + 18, a.y + 37), cMuted, Fit(fSmall, fSmall->FontSize, t.hint, 270).c_str());
		// Wert rechts
		char vb[16];
		snprintf(vb, sizeof vb, "%.2fx", t.value);
		float vw = TextW(fBold, vb) + 20;
		ImVec2 vp(b.x - 18 - vw, a.y + (ch - 28) / 2);
		dl->AddRectFilled(vp, ImVec2(vp.x + vw, vp.y + 28), U(changed ? Acc(0.20f) : Rgb(255, 255, 255, 0.05f)), 9);
		Text(dl, fBold, ImVec2(vp.x + 10, vp.y + 5), changed ? Mix(accent, Rgb(255, 255, 255), 0.6f) : cMuted, vb);
		// Regler dazwischen
		float sx0 = a.x + 320, sx1 = vp.x - 22, sy = a.y + ch / 2;
		ImGui::SetCursorScreenPos(ImVec2(sx0 - 10, sy - 16));
		ImGui::InvisibleButton("##sl", ImVec2(sx1 - sx0 + 20, 32));
		bool active = ImGui::IsItemActive(), sh = ImGui::IsItemHovered();
		if (sh && ImGui::IsMouseDoubleClicked(0)) tweaks::Set(i, t.def); // Doppelklick: Standard
		else if (active) {
			float f = std::clamp((ImGui::GetIO().MousePos.x - sx0) / (sx1 - sx0), 0.0f, 1.0f);
			tweaks::Set(i, t.min + f * (t.max - t.min));
		}
		if (ImGui::IsItemDeactivated()) tweaks::Save();
		float* shown = ImGui::GetStateStorage()->GetFloatRef(ImGui::GetID("pos"), SliderPos(i));
		*shown = active ? SliderPos(i) : Approach(*shown, SliderPos(i), 18.0f);
		float kx = sx0 + (sx1 - sx0) * *shown;
		float kh = Anim(ImGui::GetID("kh"), (sh || active) ? 1.0f : 0.0f, 20.0f);
		dl->AddRectFilled(ImVec2(sx0, sy - 2.5f), ImVec2(sx1, sy + 2.5f), U(Rgb(255, 255, 255, 0.08f)), 3);
		// Spektrum bis zum Knopf
		if (kx > sx0 + 1) {
			int v0 = dl->VtxBuffer.Size;
			dl->AddRectFilled(ImVec2(sx0, sy - 2.5f), ImVec2(kx, sy + 2.5f), U(Rgb(255, 255, 255)), 3);
			ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, dl->VtxBuffer.Size, ImVec2(sx0, sy), ImVec2(sx1, sy), U(spec[0]), U(spec[2]));
		}
		// Markierung beim Standardwert
		float dx = sx0 + (sx1 - sx0) * std::clamp((t.def - t.min) / (t.max - t.min), 0.0f, 1.0f);
		dl->AddRectFilled(ImVec2(dx - 1, sy - 7), ImVec2(dx + 1, sy + 7), U(Rgb(255, 255, 255, 0.22f)), 1);
		float kr = 7.5f + 2.0f * kh;
		if (kh > 0.01f) dl->AddCircleFilled(ImVec2(kx, sy), kr + 6, U(Acc(0.18f * kh)), 24);
		dl->AddCircleFilled(ImVec2(kx, sy), kr, U(Rgb(255, 255, 255)), 24);
		dl->AddCircle(ImVec2(kx, sy), kr, U(Acc(0.8f)), 24, 2.0f);
		ImGui::PopID();
	}
	Text(dl, fTiny, ImVec2(cx + 2, o.y + W.y - 26), cFaint, "Double-click a slider to set it back to normal.");
}

void Render(bool* open) {
	if (!requested) {
		requested = true;
		items::Queue([] { items::LoadProducts(); });
	}
	ImGuiIO& io = ImGui::GetIO();
	vis = Approach(vis, wantOpen ? 1.0f : 0.0f, wantOpen ? 14.0f : 20.0f);
	if (!wantOpen && vis < 0.01f) { vis = 0.0f; return; }
	float e = EaseOut(vis);

	const ImVec2 W(860, 600);
	ImGui::SetNextWindowSize(W, ImGuiCond_Always);
	ImGui::SetNextWindowPos(ImVec2((io.DisplaySize.x - W.x) * 0.5f, (io.DisplaySize.y - W.y) * 0.5f), ImGuiCond_FirstUseEver);
	ImGui::PushStyleVar(ImGuiStyleVar_Alpha, e);
	ImGui::PushFont(fRegular);
	ImGuiWindowFlags fl = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar |
		ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground;
	if (!wantOpen) fl |= ImGuiWindowFlags_NoInputs;
	if (!ImGui::Begin("##zlitems", nullptr, fl)) {
		ImGui::End();
		ImGui::PopFont();
		ImGui::PopStyleVar();
		return;
	}
	if (doReset) { doReset = false; items::ResetAll(); } // ausserhalb der Sperre
	std::unique_lock<std::mutex> lk(items::mu);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImVec2 wp = ImGui::GetWindowPos();
	mX = wp.x; mY = wp.y; mW = W.x; mH = W.y;
	// Oeffnen: leicht von unten hereingleiten
	ImVec2 o(wp.x, wp.y + (1.0f - e) * 16.0f);
	auto A = [&](ImVec4 c) { return U(c); }; // Fenster-Alpha kommt aus dem Style

	// Hintergrund + Schatten + Rand (Schatten darf ueber den Fensterrand hinaus)
	dl->PushClipRectFullScreen();
	for (int i = 1; i <= 4; i++)
		dl->AddRectFilled(ImVec2(o.x - i * 3, o.y - i * 2), ImVec2(o.x + W.x + i * 3, o.y + W.y + i * 4), A(Rgb(0, 0, 0, 0.09f)), 20 + i * 3);
	dl->AddRectFilled(o, ImVec2(o.x + W.x, o.y + W.y), A(cBg), 20);
	FadeRect(dl, ImVec2(o.x + 1, o.y + 1), ImVec2(o.x + W.x - 1, o.y + W.y - 1), 19, 0, Acc(0.10f),
		[&](float x, float y) { return (1.0f - x * W.x / 380.0f) * (1.0f - y * W.y / 150.0f); });
	dl->AddRect(o, ImVec2(o.x + W.x, o.y + W.y), A(cLine2), 20, 0, 1.0f);
	dl->PopClipRect();

	// Kopf: Logo-Punkt, Titel, Hinweis
	const float pad = 22;
	{
		ImVec2 lc(o.x + pad + 8, o.y + 33);
		float tw = TextW(fTitle, "Prism");
		bool hov = wantOpen && ImGui::IsMouseHoveringRect(ImVec2(lc.x - 16, lc.y - 18), ImVec2(o.x + pad + 34 + tw, lc.y + 18), false);
		if (logoOpenFx > 0) { logoOpenFx -= Dt(); if (logoOpenFx <= 0) logoFx = ImGui::GetTime(); }
		Logo(dl, lc, 26.0f, hov ? 1.0f : 0.0f);
	}
	Text(dl, fTitle, ImVec2(o.x + pad + 34, o.y + 21), cText, "Prism");
	// Reiter "Items" / "Presets" mit gleitender Markierung
	{
		// Im Freeplay/Training: kein "Maps"/"Merge" (macht man im Hauptmenue), dafuer "Tweaks" gleich nach "Items".
		// Jede Umgebung merkt sich ihren zuletzt benutzten Reiter, so geht beim Schliessen/Oeffnen nichts verloren.
		if (!tabLoaded) LoadTabs();
		const bool fp = items::InTweaksMode();
		{
			int want = fp ? tabFp : tabMenu;
			if (want != tab) { tab = want; tabFade = 0.0f; if (tab == 3) maps::Refresh(); }
		}
		const int N = fp ? 4 : 5;
		static const char* namesMenu[5] = {"Items", "Colors", "Presets", "Maps", "Merge"};
		static const char* namesFp[4] = {"Items", "Tweaks", "Colors", "Presets"};
		static const int idsMenu[5] = {0, 2, 1, 3, 5}, idsFp[4] = {0, 4, 2, 1}; // Reihenfolge auf dem Bildschirm -> Reiter-Nummer
		const char* const* names = fp ? namesFp : namesMenu;
		const int* ids = fp ? idsFp : idsMenu;
		float x0 = o.x + pad + 34 + TextW(fTitle, "Prism") + 22, y0 = o.y + 15;
		float tws[5], tx[5], xx = x0 + 4;
		for (int i = 0; i < N; i++) { tws[i] = TextW(fBold, names[i]) + 34; tx[i] = xx; xx += tws[i]; }
		ImVec2 bp(x0, y0), bq(xx + 4, y0 + 38);
		dl->AddRectFilled(bp, bq, A(cPanel), 19);
		dl->AddRect(bp, bq, A(cLine), 19);
		int curPos = 0;
		for (int i = 0; i < N; i++) {
			if (ids[i] == tab) curPos = i;
			ImGui::SetCursorScreenPos(ImVec2(tx[i], y0 + 4));
			ImGui::PushID(i);
			if (ImGui::InvisibleButton("##tab", ImVec2(tws[i], 30)) && tab != ids[i]) {
				tab = ids[i];
				tabFade = 0.0f;
				// Items/Colors/Presets gibt es ueberall: dort weitermachen, egal wo man das Menue als Naechstes oeffnet
				if (tab != 4) tabMenu = tab;
				if (tab != 3 && tab != 5) tabFp = tab;
				SaveTabs();
				pendingDelete = -1;
				renameIdx = -1;
				if (tab == 3) maps::Refresh();
			}
			ImGui::PopID();
		}
		for (int i = 0; i < N; i++) if (ids[i] == tab) curPos = i;
		float target = tx[curPos] - o.x;
		tabX = tabX < 0 ? target : Approach(tabX, target, 18.0f);
		tabW = tabW <= 0 ? tws[curPos] : Approach(tabW, tws[curPos], 18.0f);
		dl->AddRectFilled(ImVec2(o.x + tabX, y0 + 4), ImVec2(o.x + tabX + tabW, y0 + 34), A(Acc(0.20f)), 15);
		SpecLine(dl, o.x + tabX + tabW * 0.26f, o.x + tabX + tabW * 0.74f, y0 + 32, 1.5f, 0.9f); // Spektrum-Strich unter dem Reiter
		for (int i = 0; i < N; i++) {
			float on = Anim(ImGui::GetID(names[i]), tab == ids[i] ? 1.0f : 0.0f);
			float nw = TextW(fBold, names[i]);
			Text(dl, fBold, ImVec2(tx[i] + (tws[i] - nw) / 2, y0 + 10), Mix(cMuted, Mix(accent, Rgb(255, 255, 255), 0.65f), on), names[i]);
		}
	}

	// Zuruecksetzen (zweimal klicken): alles wieder auf deine echten Items (nicht im Reiter Maps)
	if (tab != 3 && tab != 4 && tab != 5) {
		bool any = !items::overrides.empty() || !items::paints.empty() || items::colorsOn;
		{
			std::lock_guard<std::mutex> ck(customs::mu);
			any = any || !customs::selDecal.empty() || !customs::selBall.empty();
		}
		if (resetArmed && (ImGui::GetTime() - resetArmedAt > 3.0 || !any)) resetArmed = false;
		const char* label = resetArmed ? "Reset all?" : "Reset";
		float want = TextW(fBold, label) + (resetArmed ? 28.0f : 46.0f);
		static float rw = 0;
		rw = rw <= 0 ? want : Approach(rw, want, 20.0f);
		ImVec2 rp(o.x + W.x - 62 - rw, o.y + 17);
		ImGui::SetCursorScreenPos(ImVec2(rp.x, wp.y + 17));
		bool click = ImGui::InvisibleButton("##reset", ImVec2(rw, 32)) && any;
		float h = Anim(ImGui::GetID("reseth"), any && ImGui::IsItemHovered() ? 1.0f : 0.0f);
		float k = Anim(ImGui::GetID("resetk"), resetArmed ? 1.0f : 0.0f);
		float en = Anim(ImGui::GetID("resete"), any ? 1.0f : 0.0f);
		ImVec2 rq(rp.x + rw, rp.y + 32);
		ImVec4 red = Rgb(235, 85, 85);
		dl->AddRectFilled(rp, rq, A(Mix(Rgb(255, 255, 255, 0.03f + 0.06f * h), Alpha(red, 0.16f + 0.10f * h), k)), 10);
		dl->AddRect(rp, rq, A(Mix(Rgb(255, 255, 255, 0.06f), Alpha(red, 0.5f), k)), 10);
		ImVec4 tc = Alpha(Mix(Mix(cMuted, cText, h), Rgb(255, 165, 165), k), 0.45f + 0.55f * en);
		float tx = rp.x + 14;
		if (k < 0.99f) { // Pfeil im Kreis
			ImVec2 c(rp.x + 21, rp.y + 16);
			ImU32 ic = A(Alpha(tc, 1.0f - k));
			dl->PathArcTo(c, 6.0f, 0.6f, 5.6f, 20);
			dl->PathStroke(ic, 0, 1.6f);
			ImVec2 tip(c.x + 6.0f * cosf(0.6f), c.y + 6.0f * sinf(0.6f));
			dl->AddLine(tip, ImVec2(tip.x + 3.5f, tip.y - 1.0f), ic, 1.6f);
			dl->AddLine(tip, ImVec2(tip.x - 0.5f, tip.y - 4.0f), ic, 1.6f);
			tx = rp.x + 34 - 20 * k;
		}
		dl->PushClipRect(rp, rq, true);
		Text(dl, fBold, ImVec2(tx, rp.y + 7), tc, label);
		dl->PopClipRect();
		if (click) {
			if (resetArmed) { resetArmed = false; doReset = true; }
			else { resetArmed = true; resetArmedAt = ImGui::GetTime(); }
		}
	}

	// Fenster am Kopf verschieben
	ImGui::SetCursorPos(ImVec2(0, 0));
	ImGui::InvisibleButton("##drag", ImVec2(W.x - 64, 64));
	if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 0.0f)) {
		ImVec2 np(wp.x + io.MouseDelta.x, wp.y + io.MouseDelta.y);
		np.x = std::clamp(np.x, 40.0f - W.x, io.DisplaySize.x - 40.0f);
		np.y = std::clamp(np.y, 0.0f, io.DisplaySize.y - 40.0f);
		ImGui::SetWindowPos(np);
	}
	// Schliessen
	ImGui::SetCursorPos(ImVec2(W.x - 52, 16));
	if (ImGui::InvisibleButton("##close", ImVec2(34, 34))) *open = false;
	{
		float h = Anim(ImGui::GetID("closeh"), ImGui::IsItemHovered() ? 1.0f : 0.0f);
		ImVec2 c(o.x + W.x - 35, o.y + 33);
		dl->AddRectFilled(ImVec2(c.x - 16, c.y - 16), ImVec2(c.x + 16, c.y + 16), A(Rgb(255, 255, 255, 0.03f + 0.07f * h)), 10);
		ImU32 xc = A(Mix(cMuted, cText, h));
		dl->AddLine(ImVec2(c.x - 4.5f, c.y - 4.5f), ImVec2(c.x + 4.5f, c.y + 4.5f), xc, 1.6f);
		dl->AddLine(ImVec2(c.x + 4.5f, c.y - 4.5f), ImVec2(c.x - 4.5f, c.y + 4.5f), xc, 1.6f);
	}
	const float top = 66;
	dl->AddLine(ImVec2(o.x + 1, o.y + top), ImVec2(o.x + W.x - 1, o.y + top), A(cLine));

	// Tweaks: nur im Freeplay, braucht die Items nicht
	if (tab == 4) {
		tabFade = Approach(tabFade, 1.0f, 12.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_Alpha, e * EaseOut(tabFade));
		bool resetTw = false;
		TweaksBody(dl, ImVec2(o.x, o.y + (1.0f - EaseOut(tabFade)) * 6.0f), W, top, pad, resetTw);
		ImGui::PopStyleVar();
		lk.unlock();
		ImGui::End();
		ImGui::PopFont();
		ImGui::PopStyleVar();
		if (resetTw) tweaks::ResetAll();
		return;
	}
	// Merge: braucht die Items nicht
	if (tab == 5) {
		tabFade = Approach(tabFade, 1.0f, 12.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_Alpha, e * EaseOut(tabFade));
		MergeAction ma;
		MergeBody(dl, ImVec2(o.x, o.y + (1.0f - EaseOut(tabFade)) * 6.0f), W, top, pad, ma);
		ImGui::PopStyleVar();
		lk.unlock();
		ImGui::End();
		ImGui::PopFont();
		ImGui::PopStyleVar();
		if (ma.kind == 1) merge::Request(ma.file, ma.backdrop, ma.replace);
		if (ma.kind == 2) merge::Request(ma.file, "", false);
		if (ma.kind == 3) merge::RestoreAll();
		if (ma.kind == 5) merge::RequestRestart();
		if (ma.kind == 4) { maps::Play(ma.arenaU8); *open = false; }
		return;
	}
	// Maps: braucht die Items nicht
	if (tab == 3) {
		static double lastScan = 0;
		if (wantOpen && !maps::Busy().load() && ImGui::GetTime() - lastScan > 4.0) { lastScan = ImGui::GetTime(); maps::Refresh(); }
		tabFade = Approach(tabFade, 1.0f, 12.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_Alpha, e * EaseOut(tabFade));
		MapAction ma;
		MapsBody(dl, ImVec2(o.x, o.y + (1.0f - EaseOut(tabFade)) * 6.0f), W, top, pad, ma);
		ImGui::PopStyleVar();
		lk.unlock();
		ImGui::End();
		ImGui::PopFont();
		ImGui::PopStyleVar();
		if (ma.kind == 1) maps::Swap(ma.arena, ma.path);
		if (ma.kind == 2) maps::Restore(ma.arena);
		if (ma.kind == 3) maps::RestoreAll();
		if (ma.kind == 4) { maps::Play(ma.arenaU8); *open = false; }
		if (ma.kind == 5) maps::ToggleFav(ma.path);
		return;
	}

	// Noch keine Items: Laden / Fehler
	if (!items::loaded) {
		ImVec2 c(o.x + W.x / 2, o.y + top + (W.y - top) / 2 - 24);
		bool failed = !items::status.empty() && items::status.find("…") == std::string::npos;
		if (!failed) Spinner(dl, c, 15);
		const char* msg = items::status.empty() ? "Loading items …" : items::status.c_str();
		float mw = TextW(fRegular, msg);
		Text(dl, fRegular, ImVec2(c.x - mw / 2, c.y + 28), cMuted, msg);
		ImGui::SetCursorScreenPos(ImVec2(c.x - 60, c.y + 62));
		if (ImGui::InvisibleButton("##reload", ImVec2(120, 36))) items::Queue([] { items::LoadProducts(); });
		float h = Anim(ImGui::GetID("rh"), ImGui::IsItemHovered() ? 1.0f : 0.0f);
		ImVec2 b(c.x - 60, c.y + 62);
		dl->AddRectFilled(b, ImVec2(b.x + 120, b.y + 36), A(Acc(0.16f + 0.12f * h)), 10);
		float bw = TextW(fBold, "Try again");
		Text(dl, fBold, ImVec2(b.x + (120 - bw) / 2, b.y + 9), Mix(Hex(0xd8d0ff), cText, h), "Try again");
		lk.unlock();
		ImGui::End();
		ImGui::PopFont();
		ImGui::PopStyleVar();
		return;
	}

	tabFade = Approach(tabFade, 1.0f, 12.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_Alpha, e * EaseOut(tabFade));
	if (tab == 2) {
		ColorAction ca;
		ColorsBody(dl, ImVec2(o.x, o.y + (1.0f - EaseOut(tabFade)) * 6.0f), W, top, pad, ca);
		ImGui::PopStyleVar();
		lk.unlock();
		ImGui::End();
		ImGui::PopFont();
		ImGui::PopStyleVar();
		if (ca.changed) items::SetColors(ca.on, ca.both, ca.cols, ca.save);
		if (ca.finishSlot >= 0) items::Set(ca.finishSlot, ca.finishId);
		return;
	}
	if (tab == 1) {
		int action = 0, idx = -1;
		PresetsBody(dl, ImVec2(o.x, o.y + (1.0f - EaseOut(tabFade)) * 6.0f), W, top, pad, action, idx);
		ImGui::PopStyleVar();
		std::string name = presetName;
		lk.unlock();
		ImGui::End();
		ImGui::PopFont();
		ImGui::PopStyleVar();
		if (action == 1) items::ApplyPreset(idx);
		if (action == 2) { items::DeletePreset(idx); if (renameIdx == idx) renameIdx = -1; }
		if (action == 3) { items::AddPreset(name); presetName[0] = 0; pScrollTarget = 0; }
		if (action == 4) items::UpdatePreset(idx, "");
		if (action == 7) { items::RenamePreset(idx, renameBuf); renameIdx = -1; BlurField(); }
		return;
	}

	// ---------- Links: Slots ----------
	const float leftW = 250, rowH = 47, listTop = top + 14;
	dl->AddLine(ImVec2(o.x + pad + leftW + 14, o.y + top + 1), ImVec2(o.x + pad + leftW + 14, o.y + W.y - 1), A(cLine));
	Text(dl, fTiny, ImVec2(o.x + pad + 16, o.y + listTop), cFaint, "LOADOUT");
	ImGui::SetCursorScreenPos(ImVec2(o.x + pad, o.y + listTop + 22));
	ImGui::BeginGroup();
	float selY = -1;
	// gleitende Markierung hinter dem gewaehlten Slot (zuerst zeichnen, damit sie unter dem Text liegt)
	ImDrawListSplitter split;
	split.Split(dl, 2);
	split.SetCurrentChannel(dl, 1);
	for (int i = 0; i < items::kSlotCount; i++)
		if (items::kSlots[i].index != 7 && items::kSlots[i].index != 12) SlotRow(items::kSlots[i], ImVec2(leftW, rowH), selY); // Lackierungen: Reiter Colors
	SlotRow(items::SlotInfo{kBallSlot, "Ball"}, ImVec2(leftW, rowH), selY); // eigene Baelle
	split.SetCurrentChannel(dl, 0);
	if (selY >= 0) {
		float rel = selY - o.y;
		indicatorY = indicatorY < 0 ? rel : Approach(indicatorY, rel, 20.0f);
		ImVec2 a(o.x + pad, o.y + indicatorY), b(o.x + pad + leftW, o.y + indicatorY + rowH);
		dl->AddRectFilled(a, b, A(Acc(0.12f)), 12);
		dl->AddRectFilled(ImVec2(a.x, a.y + 12), ImVec2(a.x + 3, b.y - 12), A(accent), 2);
	}
	split.Merge(dl);
	ImGui::EndGroup();

	// ---------- Rechts: Auswahl ----------
	const float px = pad + leftW + 30, pw = W.x - px - pad;
	const char* slotName = curSlot == kBallSlot ? "Ball" : "";
	for (int i = 0; i < items::kSlotCount; i++)
		if (items::kSlots[i].index == curSlot) slotName = items::kSlots[i].label;
	// eigene Decals/Baelle fuer diesen Slot
	std::vector<customs::Custom> myCustoms;
	std::string selCustom, customStatus;
	if (curSlot == 1 || curSlot == kBallSlot) {
		std::lock_guard<std::mutex> ck(customs::mu);
		for (auto& c : customs::list)
			if (c.ball == (curSlot == kBallSlot)) myCustoms.push_back(c);
		selCustom = curSlot == kBallSlot ? customs::selBall : customs::selDecal;
		customStatus = customs::status;
	}
	std::string pickCustom = "\x01"; // \x01 = nichts geklickt
	static int autoDecal = 0; // von uns fuer ein eigenes Decal gewaehltes Spiel-Decal
	int autoDecalWant = -1;
	bool openFolder = false;
	std::string q = Lower(search);
	std::vector<const items::Item*> shown;
	int total = 0;
	// Decals gibt es nur fuer ein bestimmtes Auto: nur die fuer dein aktuelles Auto zeigen
	int body = items::overrides.count(0) ? items::overrides[0] : (items::ownItems.count(0) ? items::ownItems[0] : 0);
	for (auto& it : items::list) {
		if (it.slot != curSlot) continue;
		if (curSlot == 1 && body && it.forCar && it.forCar != body) continue;
		total++;
		if (q.empty() || Lower(it.name).find(q) != std::string::npos) shown.push_back(&it);
	}
	listFade = Approach(listFade, 1.0f, 12.0f);
	Text(dl, fTitle, ImVec2(o.x + px, o.y + listTop - 2), cText, slotName);
	{
		char cnt[32];
		snprintf(cnt, sizeof cnt, "%d", total);
		float nw = TextW(fTitle, slotName), cw = TextW(fTiny, cnt);
		ImVec2 c0(o.x + px + nw + 10, o.y + listTop + 5);
		dl->AddRectFilled(c0, ImVec2(c0.x + cw + 14, c0.y + 18), A(Rgb(255, 255, 255, 0.06f)), 9);
		Text(dl, fTiny, ImVec2(c0.x + 7, c0.y + 2), cMuted, cnt);
	}

	// Farben (nur bei lackierbaren Items)
	int cur = items::overrides.count(curSlot) ? items::overrides[curSlot] : 0;
	const items::Item* curItem = cur > 0 ? FindItem(cur) : nullptr;
	if (!curItem) { // dein eigenes Item kann man auch lackieren
		auto ow = items::ownItems.find(curSlot);
		if (ow != items::ownItems.end()) curItem = FindItem(ow->second);
	}
	uint32_t mask = curItem && curItem->paintable ? curItem->paintMask : 0;
	bool showColors = mask != 0 && !items::paintList.empty();
	static float colorH = 0.0f;
	colorH = Approach(colorH, showColors ? 46.0f : 0.0f, 16.0f);
	int pickPaint = -1;
	if (colorH > 1.0f) {
		float a = std::clamp(colorH / 46.0f, 0.0f, 1.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * a);
		int curPaint = items::paints.count(curSlot) ? items::paints[curSlot] : 0;
		if (curPaint <= 0 || curPaint >= 32 || !((mask >> curPaint) & 1)) curPaint = 0; // gibt es fuer dieses Item nicht
		float cy = o.y + listTop + 34 + 18;
		Text(dl, fTiny, ImVec2(o.x + px, cy - 7), cFaint, "PAINT");
		float x = o.x + px + 50;
		const char* hoverName = nullptr;
		auto swatch = [&](int id, ImVec4 col, const char* name) {
			ImGui::SetCursorScreenPos(ImVec2(x - 11, cy - 11));
			ImGui::PushID(id + 1000);
			if (ImGui::InvisibleButton("##sw", ImVec2(22, 22))) pickPaint = id;
			bool hov = ImGui::IsItemHovered();
			float h = Anim(ImGui::GetID("h"), hov ? 1.0f : 0.0f, 22.0f);
			float sel = Anim(ImGui::GetID("s"), curPaint == id ? 1.0f : 0.0f, 18.0f);
			ImGui::PopID();
			if (hov) hoverName = name;
			float r = 8.0f + 1.5f * h;
			if (sel > 0.01f) dl->AddCircle(ImVec2(x, cy), r + 3.5f, U(Alpha(Rgb(255, 255, 255), sel)), 24, 1.8f);
			if (id == 0) {
				dl->AddCircleFilled(ImVec2(x, cy), r, U(cPanel2), 24);
				dl->AddCircle(ImVec2(x, cy), r, U(cLine2), 24, 1.2f);
				dl->AddLine(ImVec2(x - r * 0.6f, cy + r * 0.6f), ImVec2(x + r * 0.6f, cy - r * 0.6f), U(cMuted), 1.4f);
			} else {
				dl->AddCircleFilled(ImVec2(x, cy), r, U(col), 24);
				dl->AddCircle(ImVec2(x, cy), r, U(Rgb(255, 255, 255, 0.12f)), 24, 1.0f);
			}
			x += 23;
		};
		swatch(0, cMuted, "Unpainted");
		bool sep = false;
		for (auto& pi : items::paintList) {
			if (pi.id <= 0 || pi.id >= 32 || !((mask >> pi.id) & 1)) continue; // nur Lackierungen, die es fuer dieses Item gibt
			if (pi.id >= 15 && !sep) { // besondere Lackierungen etwas abgesetzt
				sep = true;
				dl->AddLine(ImVec2(x - 6, cy - 8), ImVec2(x - 6, cy + 8), U(cLine2), 1.0f);
				x += 6;
			}
			swatch(pi.id, ImVec4(pi.rgb[0], pi.rgb[1], pi.rgb[2], 1.0f), pi.name.c_str());
		}
		// Name der Farbe (beim Drueberfahren, sonst die gewaehlte)
		const char* shownName = hoverName;
		if (!shownName) {
			shownName = "Unpainted";
			for (auto& pi : items::paintList)
				if (pi.id == curPaint) shownName = pi.name.c_str();
		}
		// rechts in der Titelzeile: kleiner Farbpunkt + Name
		float nw = TextW(fSmall, shownName);
		float nx = o.x + px + pw - nw, ny = o.y + listTop + 4;
		ImVec4 sc = cMuted;
		for (auto& pi : items::paintList)
			if (strcmp(pi.name.c_str(), shownName) == 0) sc = ImVec4(pi.rgb[0], pi.rgb[1], pi.rgb[2], 1.0f);
		dl->AddCircleFilled(ImVec2(nx - 12, ny + 8), 4.5f, U(sc), 16);
		Text(dl, fSmall, ImVec2(nx, ny), hoverName ? cText : cMuted, shownName);
		ImGui::PopStyleVar();
	}

	// Suchfeld
	const float sy = listTop + 36 + colorH, sh = 42;
	ImVec2 sp(o.x + px, o.y + sy), sq(o.x + px + pw, o.y + sy + sh);
	Field(dl, "##q", "Search", search, sizeof search, sp, sq, focusSearch, true, true);

	// Liste
	int pick = -1;
	const float ly = sy + sh + 12;
	ImGui::SetCursorScreenPos(ImVec2(o.x + px - 4, o.y + ly + (1.0f - EaseOut(listFade)) * 8.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_Alpha, e * EaseOut(listFade));
	ImGui::BeginChild("rows", ImVec2(pw + 8, W.y - ly - 14), false, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse);
	SmoothScroll(scrollTarget, scrollLast);
	const float rw = pw - 8;
	{
		// ganz oben: dein echtes Item aus Rocket League
		bool customOn = !selCustom.empty();
		if (curSlot == kBallSlot) {
			int dummy = -1;
			ItemRow(0, "Default ball", "", Rgb(80, 80, 92), customOn ? -999 : 0, dummy, rw);
			if (dummy == 0) pickCustom = "";
		} else {
			auto ow = items::ownItems.find(curSlot);
			const items::Item* own = ow != items::ownItems.end() ? FindItem(ow->second) : nullptr;
			int shownCur = customOn ? -999 : cur;
			if (own) ItemRow(0, own->name.c_str(), "Your item", QualityColor(own->quality), shownCur, pick, rw);
			else ItemRow(0, items::ownItems.count(curSlot) ? "Nothing equipped" : "Your own", "", Rgb(80, 80, 92), shownCur, pick, rw);
		}
		// eigene Decals/Baelle (passende zuerst)
		if (curSlot == 1 || curSlot == kBallSlot) {
			ImGui::Dummy(ImVec2(0, 4));
			ImVec2 hp = ImGui::GetCursorScreenPos();
			Text(ImGui::GetWindowDrawList(), fTiny, ImVec2(hp.x + 12, hp.y), cFaint, curSlot == kBallSlot ? "YOUR BALLS" : "CUSTOM DECALS");
			if (!customStatus.empty()) {
				float sw = TextW(fTiny, customStatus.c_str());
				Text(ImGui::GetWindowDrawList(), fTiny, ImVec2(hp.x + rw - 12 - sw, hp.y), cMuted, customStatus.c_str());
			}
			ImGui::Dummy(ImVec2(0, 18));
			std::stable_sort(myCustoms.begin(), myCustoms.end(), [&](const customs::Custom& a, const customs::Custom& b) {
				bool fa = !a.bodyId || !body || a.bodyId == body, fb = !b.bodyId || !body || b.bodyId == body;
				return fa > fb;
			});
			for (auto& c : myCustoms) {
				bool fits = c.ball || !c.bodyId || !body || c.bodyId == body;
				std::string sub = c.ball ? "Custom ball" : "Custom decal";
				if (!fits) {
					const items::Item* car = FindItem(c.bodyId);
					sub = "Only on " + (car ? car->name : "car " + std::to_string(c.bodyId));
				}
				if (CustomRow(c, c.id == selCustom, fits, sub.c_str(), rw)) {
					pickCustom = c.id == selCustom ? "" : c.id;
					// Eigene Decals brauchen ein Spiel-Decal am Auto (sonst zeichnet das Material gar kein Decal-Bild):
					// hat das Auto keins, eins dafuer aussuchen (SkinID aus dem Paket, sonst irgendeins fuer dieses Auto)
					if (curSlot == 1 && !pickCustom.empty() && fits) {
						int curDecal = items::overrides.count(1) ? items::overrides[1] : (items::ownItems.count(1) ? items::ownItems[1] : 0);
						if (!curDecal) {
							int pickId = (c.skinId > 0 && FindItem(c.skinId)) ? c.skinId : 0;
							// ein ganz normales Decal (gewoehnlichste Seltenheit) - besondere Decals haben eigene Materialien
							int bestRank = 99;
							for (auto& it : items::list) {
								if (it.slot != 1 || !it.forCar || it.forCar != (c.bodyId ? c.bodyId : body)) continue;
								int rank = it.quality == 0 ? 0 : it.quality == 1 ? 1 : it.quality == 9 ? 2 : it.quality == 2 ? 3 : 10;
								if (!pickId || rank < bestRank) { bestRank = rank; pickId = it.id; }
							}
							if (pickId) { autoDecalWant = pickId; autoDecal = pickId; }
						}
					} else if (curSlot == 1 && pickCustom.empty() && autoDecal && items::overrides.count(1) && items::overrides[1] == autoDecal) {
						autoDecalWant = 0; // wieder wie vorher
						autoDecal = 0;
					}
				}
			}
			if (AddCustomCard(curSlot == kBallSlot ? "Add your own balls" : "Add your own decals", rw)) openFolder = true;
			ImGui::Dummy(ImVec2(0, 6));
			if (curSlot == 1 && !shown.empty()) {
				ImVec2 hp2 = ImGui::GetCursorScreenPos();
				Text(ImGui::GetWindowDrawList(), fTiny, ImVec2(hp2.x + 12, hp2.y), cFaint, "GAME DECALS");
				ImGui::Dummy(ImVec2(0, 18));
			}
		}
	}
	if (q.empty() || shown.size()) ImGui::Dummy(ImVec2(0, 2));
	ImGuiListClipper clip;
	clip.Begin((int)shown.size(), 46.0f + ImGui::GetStyle().ItemSpacing.y);
	while (clip.Step())
		for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
			const items::Item* it = shown[r];
			ItemRow(it->id, it->name.c_str(), kQuality[std::clamp(it->quality, 0, 9)], QualityColor(it->quality), cur, pick, rw);
		}
	if (!q.empty() && shown.empty()) {
		ImVec2 p = ImGui::GetCursorScreenPos();
		std::string m = "Nothing matches \"" + std::string(search) + "\"";
		Text(ImGui::GetWindowDrawList(), fRegular, ImVec2(p.x + 14, p.y + 18), cMuted, Fit(fRegular, fRegular->FontSize, m, rw - 28).c_str());
		ImGui::Dummy(ImVec2(0, 50));
	}
	ImGui::Dummy(ImVec2(0, 6));
	{
		// weiche Kanten oben/unten, wenn gescrollt ist
		ImDrawList* cd = ImGui::GetWindowDrawList();
		ImVec2 wp2 = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
		float sc = ImGui::GetScrollY(), mx = ImGui::GetScrollMaxY();
		float ft = std::min(sc, 14.0f) / 14.0f, fb = std::min(mx - sc, 14.0f) / 14.0f;
		if (ft > 0) cd->AddRectFilledMultiColor(wp2, ImVec2(wp2.x + ws.x, wp2.y + 18), U(Alpha(cBg, ft)), U(Alpha(cBg, ft)), U(Alpha(cBg, 0)), U(Alpha(cBg, 0)));
		if (fb > 0) cd->AddRectFilledMultiColor(ImVec2(wp2.x, wp2.y + ws.y - 18), ImVec2(wp2.x + ws.x, wp2.y + ws.y), U(Alpha(cBg, 0)), U(Alpha(cBg, 0)), U(Alpha(cBg, fb)), U(Alpha(cBg, fb)));
	}
	ImGui::EndChild();
	ImGui::PopStyleVar();
	ImGui::PopStyleVar(); // Reiter-Einblendung

	lk.unlock();
	ImGui::End();
	ImGui::PopFont();
	ImGui::PopStyleVar();

	if (curSlot == kBallSlot) pick = -1; // eigener Ball ist kein Spiel-Item
	if (pick >= 0 && curSlot == 1 && !selCustom.empty()) { customs::Select(false, ""); autoDecal = 0; } // Spiel-Decal gewaehlt: eigenes aus
	if (pickCustom != "\x01") customs::Select(curSlot == kBallSlot, pickCustom);
	if (autoDecalWant >= 0) items::Set(1, autoDecalWant);
	if (openFolder) customs::OpenFolder();
	if (pick >= 0 && pick != cur) items::Set(curSlot, pick);
	if (pickPaint >= 0) items::SetPaint(curSlot, pickPaint);
}

// ---------- Hinweis beim Spielstart ----------
void Toast(float t) {
	ImGuiIO& io = ImGui::GetIO();
	float in = EaseOut(t / 0.45f);
	float out = std::clamp((kToastSeconds - t) / 0.45f, 0.0f, 1.0f);
	float a = std::min(in, out * out);
	const char* title = "Prism menu ready";
	std::string kn = MenuKeyName();
	std::string subS = "Press " + kn + " in game for items, colors and maps";
	const char* sub = subS.c_str();
	float w = std::max(TextW(fBold, title), TextW(fSmall, sub)) + 86 + std::max(34.0f, TextW(fBold, kn.c_str()) + 18);
	const ImVec2 sz(w, 64);
	ImVec2 p((io.DisplaySize.x - sz.x) / 2, 26 + (in - 1.0f) * 18 - (1.0f - out) * 10);
	ImDrawList* d = ImGui::GetForegroundDrawList();
	auto C = [&](ImVec4 c) { return U(Alpha(c, a)); };
	for (int i = 1; i <= 3; i++)
		d->AddRectFilled(ImVec2(p.x - i * 2, p.y - i + 3), ImVec2(p.x + sz.x + i * 2, p.y + sz.y + i * 3), C(Rgb(0, 0, 0, 0.12f)), 18 + i * 2);
	d->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), C(Hex(0x111115, 0.98f)), 18);
	FadeRect(d, ImVec2(p.x + 1, p.y + 1), ImVec2(p.x + sz.x - 1, p.y + sz.y - 1), 17, 0, Alpha(Acc(0.12f), a),
		[&](float x, float) { return 1.0f - x * sz.x / 160.0f; });
	d->AddRect(p, ImVec2(p.x + sz.x, p.y + sz.y), C(cLine2), 18);
	// Symbol: "F8"-Taste in der Akzentfarbe
	ImVec2 k(p.x + 16, p.y + 15);
	std::string kl = Fit(fBold, fBold->FontSize, kn, 90);
	float kw = TextW(fBold, kl.c_str()), kb = std::max(34.0f, kw + 18);
	d->AddRectFilled(k, ImVec2(k.x + kb, k.y + 34), C(Acc(0.18f)), 9);
	d->AddRect(k, ImVec2(k.x + kb, k.y + 34), C(Acc(0.55f)), 9);
	Text(d, fBold, ImVec2(k.x + (kb - kw) / 2, k.y + 8), Alpha(Mix(accent, Rgb(255, 255, 255), 0.6f), a), kl.c_str());
	Text(d, fBold, ImVec2(k.x + kb + 14, p.y + 13), Alpha(cText, a), title);
	Text(d, fSmall, ImVec2(k.x + kb + 14, p.y + 34), Alpha(cMuted, a), sub);
	// verbleibende Zeit als feine Linie unten
	float rem = std::clamp(1.0f - t / kToastSeconds, 0.0f, 1.0f);
	float lx0 = p.x + 18, lx1 = p.x + sz.x - 18;
	if (rem > 0.01f) SpecLine(d, lx0, lx0 + (lx1 - lx0) * rem, p.y + sz.y - 4, 2.0f, 0.85f * a);
}

} // namespace gui
