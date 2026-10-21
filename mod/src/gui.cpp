// F8-Menue im Prism-Stil: links die Slots, rechts die Auswahl fuer den Slot.
// Alles selbst gezeichnet, mit weichen Uebergaengen (Oeffnen/Schliessen, Hover, Auswahl, Scrollen).
#include "gui.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "imgui.h"
#include "imgui_internal.h"
#include "items.h"
#include "log.h"

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

static const ImVec4 cBg = Hex(0x0c0c0f), cPanel = Hex(0x111115), cPanel2 = Hex(0x15151a), cHover = Hex(0x1a1a20);
static const ImVec4 cLine = Hex(0x1f1f26), cLine2 = Hex(0x2a2a33);
static const ImVec4 cText = Hex(0xececf1), cMuted = Hex(0x8b8b97), cFaint = Hex(0x55555f);

// ---------- Animation ----------
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
	std::string s(buf);
	if (s.find("\"theme\":\"purple\"") != std::string::npos) set(139, 92, 246, 129, 140, 248, 167, 139, 250, 232, 121, 249);
	else if (s.find("\"theme\":\"blue\"") != std::string::npos) set(59, 130, 246, 129, 140, 248, 96, 165, 250, 56, 189, 248);
	else if (s.find("\"theme\":\"cyan\"") != std::string::npos) set(6, 182, 212, 96, 165, 250, 34, 211, 238, 45, 212, 191);
	else if (s.find("\"theme\":\"green\"") != std::string::npos) set(16, 185, 129, 34, 211, 238, 52, 211, 153, 163, 230, 53);
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

void SetOpen(bool open) {
	wantOpen = open;
	if (open) { focusSearch = false; ReadTheme(); }
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
	std::string name = Fit(fRegular, fRegular->FontSize, it ? it->name : (ownIt ? ownIt->name : ownLabel), size.x - 60);
	Text(d, fRegular, ImVec2(tx, p.y + 22), it ? Mix(Hex(0xc9c9d3), cText, std::max(sa, h)) : Mix(cMuted, Hex(0xa9a9b5), h), name.c_str());
	ImGui::PopID();
}

// ---------- Rechts: eine Zeile ----------
static void ItemRow(int id, const char* name, const char* sub, ImVec4 qc, int cur, int& pick, float w, bool paintable = false) {
	ImGui::PushID(id);
	ImVec2 p = ImGui::GetCursorScreenPos();
	const float rh = 46;
	if (ImGui::InvisibleButton("##r", ImVec2(w, rh))) pick = id;
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
		bool active = pr.items == items::overrides && pr.paints == items::paints && !pr.items.empty();
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
			std::string sum = pr.items.empty() ? std::string("Your own items") : Summary(pr.items, 3);
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
// Hover: Logo kippt leicht und waechst mit Feder-Bewegung, ein Lichtfunke laeuft den Strahl hinunter
static void Logo(ImDrawList* dl, ImVec2 c, float size, float hover) {
	static float x = 0, v = 0;
	static double hoverAt = -10;
	static bool was = false;
	float dt = Dt();
	v += (hover - x) * 220.0f * dt; // Feder
	v *= expf(-14.0f * dt);         // Daempfung
	x += v * dt;
	if (hover > 0.5f && !was) hoverAt = ImGui::GetTime();
	was = hover > 0.5f;
	float rot = -0.14f * x, sc = 1.0f + 0.14f * x;
	float glow = 0.20f + 0.30f * std::clamp(x, 0.0f, 1.2f);
	for (int i = 3; i >= 1; i--) dl->AddCircleFilled(ImVec2(c.x, c.y + size * 0.08f), size * (0.28f + 0.13f * i), U(Alpha(spec[1], glow * 0.16f)), 32);
	float u = size * sc / 200.0f, cs = cosf(rot), sn = sinf(rot);
	auto P = [&](float lx, float ly) { lx -= 100.0f; ly -= 96.0f; return ImVec2(c.x + (lx * cs - ly * sn) * u, c.y + (lx * sn + ly * cs) * u); };
	ImVec2 t0 = P(100, 34), t1 = P(164, 152), t2 = P(36, 152);
	float lw = std::max(1.6f, 15.0f * u);
	ImU32 white = U(Rgb(244, 244, 250));
	dl->AddTriangle(t0, t1, t2, white, lw);
	for (ImVec2 p : {t0, t1, t2}) dl->AddCircleFilled(p, lw * 0.5f, white, 12); // runde Ecken
	// Strahl: viele kurze Stuecke mit fliessender Farbe
	ImVec2 b0 = P(100, 34), b1 = P(120, 152);
	float bw = std::max(1.4f, 12.0f * u);
	const int n = 14;
	for (int i = 0; i < n; i++) {
		float a = float(i) / n, b = float(i + 1) / n;
		ImVec2 p0(b0.x + (b1.x - b0.x) * a, b0.y + (b1.y - b0.y) * a), p1(b0.x + (b1.x - b0.x) * b, b0.y + (b1.y - b0.y) * b);
		dl->AddLine(p0, p1, U(SpecAt((a + b) * 0.5f)), bw);
	}
	dl->AddCircleFilled(b0, bw * 0.5f, U(spec[0]), 12);
	dl->AddCircleFilled(b1, bw * 0.5f, U(spec[2]), 12);
	// Lichtfunke beim Drueberfahren
	float g = float(ImGui::GetTime() - hoverAt) / 0.6f;
	if (g > 0.0f && g < 1.0f) {
		float e = 1.0f - powf(1.0f - g, 2.0f), a = sinf(g * 3.14159f);
		float s0 = std::max(0.0f, e - 0.18f), s1 = e;
		ImVec2 p0(b0.x + (b1.x - b0.x) * s0, b0.y + (b1.y - b0.y) * s0), p1(b0.x + (b1.x - b0.x) * s1, b0.y + (b1.y - b0.y) * s1);
		dl->AddLine(p0, p1, U(Rgb(255, 255, 255, 0.9f * a)), bw);
	}
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
	dl->AddRectFilledMultiColor(ImVec2(o.x + 1, o.y + 1), ImVec2(o.x + 380, o.y + 150), A(Acc(0.10f)), A(Acc(0.0f)), A(Acc(0.0f)), A(Acc(0.0f)));
	dl->AddRect(o, ImVec2(o.x + W.x, o.y + W.y), A(cLine2), 20, 0, 1.0f);
	dl->PopClipRect();

	// Kopf: Logo-Punkt, Titel, Hinweis
	const float pad = 22;
	{
		ImVec2 lc(o.x + pad + 8, o.y + 33);
		float tw = TextW(fTitle, "Prism");
		bool hov = wantOpen && ImGui::IsMouseHoveringRect(ImVec2(lc.x - 16, lc.y - 18), ImVec2(o.x + pad + 34 + tw, lc.y + 18), false);
		Logo(dl, lc, 26.0f, hov ? 1.0f : 0.0f);
	}
	Text(dl, fTitle, ImVec2(o.x + pad + 34, o.y + 21), cText, "Prism");
	// Reiter "Items" / "Presets" mit gleitender Markierung
	{
		const char* names[3] = {"Items", "Colors", "Presets"};
		const int ids[3] = {0, 2, 1}; // Reihenfolge auf dem Bildschirm -> Reiter-Nummer
		float x0 = o.x + pad + 34 + TextW(fTitle, "Prism") + 22, y0 = o.y + 15;
		float tws[3], tx[3], xx = x0 + 4;
		for (int i = 0; i < 3; i++) { tws[i] = TextW(fBold, names[i]) + 34; tx[i] = xx; xx += tws[i]; }
		ImVec2 bp(x0, y0), bq(xx + 4, y0 + 38);
		dl->AddRectFilled(bp, bq, A(cPanel), 19);
		dl->AddRect(bp, bq, A(cLine), 19);
		int curPos = 0;
		for (int i = 0; i < 3; i++) {
			if (ids[i] == tab) curPos = i;
			ImGui::SetCursorScreenPos(ImVec2(tx[i], y0 + 4));
			ImGui::PushID(i);
			if (ImGui::InvisibleButton("##tab", ImVec2(tws[i], 30)) && tab != ids[i]) {
				tab = ids[i];
				tabFade = 0.0f;
				pendingDelete = -1;
				renameIdx = -1;
			}
			ImGui::PopID();
		}
		for (int i = 0; i < 3; i++) if (ids[i] == tab) curPos = i;
		float target = tx[curPos] - o.x;
		tabX = tabX < 0 ? target : Approach(tabX, target, 18.0f);
		tabW = tabW <= 0 ? tws[curPos] : Approach(tabW, tws[curPos], 18.0f);
		dl->AddRectFilled(ImVec2(o.x + tabX, y0 + 4), ImVec2(o.x + tabX + tabW, y0 + 34), A(Acc(0.20f)), 15);
		SpecLine(dl, o.x + tabX + tabW * 0.26f, o.x + tabX + tabW * 0.74f, y0 + 32, 1.5f, 0.9f); // Spektrum-Strich unter dem Reiter
		for (int i = 0; i < 3; i++) {
			float on = Anim(ImGui::GetID(names[i]), tab == ids[i] ? 1.0f : 0.0f);
			float nw = TextW(fBold, names[i]);
			Text(dl, fBold, ImVec2(tx[i] + (tws[i] - nw) / 2, y0 + 10), Mix(cMuted, Mix(accent, Rgb(255, 255, 255), 0.65f), on), names[i]);
		}
	}

	// Zuruecksetzen (zweimal klicken): alles wieder auf deine echten Items
	{
		bool any = !items::overrides.empty() || !items::paints.empty() || items::colorsOn;
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
	const char* slotName = "";
	for (int i = 0; i < items::kSlotCount; i++)
		if (items::kSlots[i].index == curSlot) slotName = items::kSlots[i].label;
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
		auto ow = items::ownItems.find(curSlot);
		const items::Item* own = ow != items::ownItems.end() ? FindItem(ow->second) : nullptr;
		if (own) ItemRow(0, own->name.c_str(), "Your item", QualityColor(own->quality), cur, pick, rw);
		else ItemRow(0, items::ownItems.count(curSlot) ? "Nothing equipped" : "Your own", "", Rgb(80, 80, 92), cur, pick, rw);
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

	if (pick >= 0 && pick != cur) items::Set(curSlot, pick);
	if (pickPaint >= 0) items::SetPaint(curSlot, pickPaint);
}

// ---------- Hinweis beim Spielstart ----------
void Toast(float t) {
	ImGuiIO& io = ImGui::GetIO();
	float in = EaseOut(t / 0.45f);
	float out = std::clamp((kToastSeconds - t) / 0.45f, 0.0f, 1.0f);
	float a = std::min(in, out * out);
	const char* title = "Item menu ready";
	const char* sub = "Press F8 in game to pick your items";
	float w = std::max(TextW(fBold, title), TextW(fSmall, sub)) + 120;
	const ImVec2 sz(w, 64);
	ImVec2 p((io.DisplaySize.x - sz.x) / 2, 26 + (in - 1.0f) * 18 - (1.0f - out) * 10);
	ImDrawList* d = ImGui::GetForegroundDrawList();
	auto C = [&](ImVec4 c) { return U(Alpha(c, a)); };
	for (int i = 1; i <= 3; i++)
		d->AddRectFilled(ImVec2(p.x - i * 2, p.y - i + 3), ImVec2(p.x + sz.x + i * 2, p.y + sz.y + i * 3), C(Rgb(0, 0, 0, 0.12f)), 18 + i * 2);
	d->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), C(Hex(0x111115, 0.98f)), 18);
	d->AddRectFilledMultiColor(ImVec2(p.x + 1, p.y + 1), ImVec2(p.x + 160, p.y + sz.y - 1), C(Acc(0.12f)), C(Acc(0.0f)), C(Acc(0.0f)), C(Acc(0.12f)));
	d->AddRect(p, ImVec2(p.x + sz.x, p.y + sz.y), C(cLine2), 18);
	// Symbol: "F8"-Taste in der Akzentfarbe
	ImVec2 k(p.x + 16, p.y + 15);
	d->AddRectFilled(k, ImVec2(k.x + 34, k.y + 34), C(Acc(0.18f)), 9);
	d->AddRect(k, ImVec2(k.x + 34, k.y + 34), C(Acc(0.55f)), 9);
	float kw = TextW(fBold, "F8");
	Text(d, fBold, ImVec2(k.x + (34 - kw) / 2, k.y + 8), Alpha(Mix(accent, Rgb(255, 255, 255), 0.6f), a), "F8");
	Text(d, fBold, ImVec2(p.x + 64, p.y + 13), Alpha(cText, a), title);
	Text(d, fSmall, ImVec2(p.x + 64, p.y + 34), Alpha(cMuted, a), sub);
	// verbleibende Zeit als feine Linie unten
	float rem = std::clamp(1.0f - t / kToastSeconds, 0.0f, 1.0f);
	float lx0 = p.x + 18, lx1 = p.x + sz.x - 18;
	if (rem > 0.01f) SpecLine(d, lx0, lx0 + (lx1 - lx0) * rem, p.y + sz.y - 4, 2.0f, 0.85f * a);
}

} // namespace gui
