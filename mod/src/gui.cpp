// F8-Menue im ZLauncher-Stil: links die Slots, rechts die Auswahl fuer den Slot.
// Alles selbst gezeichnet, mit weichen Uebergaengen (Oeffnen/Schliessen, Hover, Auswahl, Scrollen).
#include "gui.h"

#include <algorithm>
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

// ---------- Farben (wie im ZLauncher) ----------
static ImVec4 accent(0.545f, 0.361f, 0.965f, 1.0f);
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
	FILE* f = _wfopen((zl::DataDir() + L"\\settings.json").c_str(), L"r");
	if (!f) return;
	char buf[16384] = {0};
	fread(buf, 1, sizeof buf - 1, f);
	fclose(f);
	std::string s(buf);
	if (s.find("\"theme\":\"blue\"") != std::string::npos) accent = Rgb(59, 130, 246);
	else if (s.find("\"theme\":\"cyan\"") != std::string::npos) accent = Rgb(6, 182, 212);
	else if (s.find("\"theme\":\"green\"") != std::string::npos) accent = Rgb(16, 185, 129);
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

// Eingabefeld im ZLauncher-Stil, komplett selbst gebaut (ohne ImGui-InputText): Zeichen kommen direkt aus
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
// action: 1 = anwenden, 2 = loeschen, 3 = speichern
static void PresetsBody(ImDrawList* dl, ImVec2 o, ImVec2 W, float top, float pad, int& action, int& actionIdx) {
	const float leftW = 250, listTop = top + 14;
	dl->AddLine(ImVec2(o.x + pad + leftW + 14, o.y + top + 1), ImVec2(o.x + pad + leftW + 14, o.y + W.y - 1), U(cLine));

	// Links: aktuelles Loadout speichern
	float x = o.x + pad, y = o.y + listTop;
	Text(dl, fTiny, ImVec2(x + 2, y), cFaint, "SAVE CURRENT ITEMS");
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
	bool click = false;
	Button(dl, "##save", justSaved ? "Saved" : "Save preset", ImVec2(x, y), ImVec2(leftW, 42), changed > 0, click);
	if ((click || (enter && changed > 0))) { action = 3; savedAt = ImGui::GetTime(); }

	// Rechts: Liste
	const float px = pad + leftW + 30, pw = W.x - px - pad;
	int count = (int)items::presets.size();
	Text(dl, fTitle, ImVec2(o.x + px, o.y + listTop - 2), cText, "Presets");
	{
		char cnt[16];
		snprintf(cnt, sizeof cnt, "%d", count);
		float nw = TextW(fTitle, "Presets"), cw = TextW(fTiny, cnt);
		ImVec2 c0(o.x + px + nw + 10, o.y + listTop + 5);
		dl->AddRectFilled(c0, ImVec2(c0.x + cw + 14, c0.y + 18), U(Rgb(255, 255, 255, 0.06f)), 9);
		Text(dl, fTiny, ImVec2(c0.x + 7, c0.y + 2), cMuted, cnt);
	}
	const float ly = listTop + 40;
	if (!count) {
		ImVec2 c(o.x + px + pw / 2, o.y + ly + 140);
		dl->AddRectFilled(ImVec2(c.x - 26, c.y - 26), ImVec2(c.x + 26, c.y + 26), U(Acc(0.10f)), 16);
		// Symbol: gestapelte Karten
		dl->AddRect(ImVec2(c.x - 11, c.y - 6), ImVec2(c.x + 11, c.y + 12), U(Acc(0.9f)), 4, 0, 1.6f);
		dl->AddLine(ImVec2(c.x - 7, c.y - 11), ImVec2(c.x + 7, c.y - 11), U(Acc(0.6f)), 1.6f);
		const char* t1 = "No presets yet";
		const char* t2 = "Save your current items on the left";
		Text(dl, fBold, ImVec2(c.x - TextW(fBold, t1) / 2, c.y + 40), cText, t1);
		Text(dl, fSmall, ImVec2(c.x - TextW(fSmall, t2) / 2, c.y + 62), cMuted, t2);
		return;
	}
	if (pendingDelete >= 0 && ImGui::GetTime() - pendingDeleteAt > 2.5) pendingDelete = -1;
	ImGui::SetCursorScreenPos(ImVec2(o.x + px - 4, o.y + ly));
	ImGui::BeginChild("presets", ImVec2(pw + 8, W.y - ly - 14), false, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse);
	SmoothScroll(pScrollTarget, pScrollLast);
	const float cw = pw - 8, ch = 64;
	ImDrawList* d = ImGui::GetWindowDrawList();
	for (int i = 0; i < count; i++) {
		const items::Preset& pr = items::presets[i];
		ImGui::PushID(i);
		ImVec2 p = ImGui::GetCursorScreenPos(), q(p.x + cw, p.y + ch);
		// Loeschen-Knopf zuerst (hat Vorrang vor der Karte)
		bool confirm = pendingDelete == i;
		float delW = confirm ? 74.0f : 32.0f;
		ImVec2 dp(q.x - 14 - delW, p.y + (ch - 32) / 2);
		ImGui::SetCursorScreenPos(dp);
		bool delClick = ImGui::InvisibleButton("##del", ImVec2(delW, 32));
		bool delHov = ImGui::IsItemHovered();
		ImGui::SetCursorScreenPos(p);
		bool click = ImGui::InvisibleButton("##card", ImVec2(cw, ch));
		bool hov = ImGui::IsItemHovered() || delHov;
		bool active = pr.items == items::overrides && pr.paints == items::paints && !pr.items.empty();
		float h = Anim(ImGui::GetID("h"), hov ? 1.0f : 0.0f);
		float ac = Anim(ImGui::GetID("a"), active ? 1.0f : 0.0f);
		d->AddRectFilled(p, q, U(Mix(Mix(cPanel, cHover, h), Acc(0.12f), ac * 0.8f)), 14);
		d->AddRect(p, q, U(Mix(cLine, Acc(0.55f), ac)), 14);
		std::string nm = Fit(fBold, fBold->FontSize, pr.name, cw - 170);
		Text(d, fBold, ImVec2(p.x + 18, p.y + 13), cText, nm.c_str());
		std::string sum = pr.items.empty() ? std::string("Your own items") : Summary(pr.items, 3);
		Text(d, fSmall, ImVec2(p.x + 18, p.y + 35), cMuted, Fit(fSmall, fSmall->FontSize, sum, cw - 170).c_str());
		// "Active"-Chip
		if (ac > 0.01f) {
			const char* t = "Active";
			float tw = TextW(fTiny, t);
			ImVec2 c0(dp.x - 12 - tw - 18, p.y + (ch - 22) / 2);
			d->AddRectFilled(c0, ImVec2(c0.x + tw + 18, c0.y + 22), U(Acc(0.2f * ac)), 11);
			Text(d, fTiny, ImVec2(c0.x + 9, c0.y + 4), Alpha(Mix(accent, Rgb(255, 255, 255), 0.5f), ac), t);
		}
		// Loeschen
		float dh = Anim(ImGui::GetID("dh"), delHov ? 1.0f : 0.0f);
		float vis = std::max(h, confirm ? 1.0f : 0.0f);
		if (vis > 0.01f) {
			ImVec2 dq(dp.x + delW, dp.y + 32);
			if (confirm) {
				d->AddRectFilled(dp, dq, U(Rgb(235, 85, 85, (0.18f + 0.14f * dh))), 10);
				float tw = TextW(fBold, "Delete");
				Text(d, fBold, ImVec2(dp.x + (delW - tw) / 2, dp.y + 7), Rgb(255, 160, 160), "Delete");
			} else {
				d->AddRectFilled(dp, dq, U(Rgb(255, 255, 255, (0.04f + 0.06f * dh) * vis)), 10);
				ImU32 c = U(Alpha(Mix(cMuted, Rgb(255, 130, 130), dh), vis));
				ImVec2 m(dp.x + 16, dp.y + 16);
				d->AddLine(ImVec2(m.x - 6, m.y - 5), ImVec2(m.x + 6, m.y - 5), c, 1.5f);
				d->AddLine(ImVec2(m.x - 2, m.y - 7.5f), ImVec2(m.x + 2, m.y - 7.5f), c, 1.5f);
				d->AddRect(ImVec2(m.x - 4.5f, m.y - 3), ImVec2(m.x + 4.5f, m.y + 7), c, 2, 0, 1.5f);
			}
		}
		if (delClick) {
			if (confirm) { action = 2; actionIdx = i; pendingDelete = -1; }
			else { pendingDelete = i; pendingDeleteAt = ImGui::GetTime(); }
		} else if (click) {
			action = 1;
			actionIdx = i;
			pendingDelete = -1;
		}
		ImGui::SetCursorScreenPos(ImVec2(p.x, q.y + 8));
		ImGui::Dummy(ImVec2(cw, 0));
		ImGui::PopID();
	}
	ImGui::Dummy(ImVec2(0, 6));
	ImGui::EndChild();
}


// ---------- ZLauncher-Logo (Pixel wie im Launcher, Farben folgen der Akzentfarbe) ----------
struct LogoPx { unsigned char x, y, k; };
static const LogoPx kLogo[] = {{11,2,0},{12,2,0},{13,2,1},{9,3,0},{10,3,0},{11,3,1},{12,3,3},{7,4,0},{8,4,0},{9,4,1},{10,4,1},{11,4,3},{6,5,0},{7,5,1},{8,5,1},{9,5,1},{10,5,3},{5,6,0},{6,6,1},{7,6,1},{8,6,1},{9,6,3},{12,6,2},{13,6,4},{4,7,0},{5,7,1},{6,7,1},{7,7,1},{8,7,3},{10,7,2},{11,7,2},{12,7,6},{3,8,0},{4,8,1},{5,8,1},{6,8,1},{7,8,3},{8,8,2},{9,8,2},{10,8,4},{11,8,6},{3,9,0},{4,9,1},{5,9,1},{6,9,1},{7,9,3},{8,9,2},{9,9,4},{10,9,6},{2,10,0},{3,10,1},{4,10,1},{5,10,1},{6,10,3},{7,10,2},{8,10,4},{9,10,6},{12,10,5},{13,10,7},{2,11,1},{3,11,3},{4,11,3},{5,11,3},{6,11,2},{7,11,4},{8,11,6},{9,11,5},{10,11,5},{11,11,7},{12,11,8},{4,12,4},{5,12,2},{6,12,4},{7,12,6},{8,12,5},{9,12,7},{10,12,8},{5,13,4},{6,13,6},{7,13,5},{8,13,7},{9,13,8},{6,14,7},{7,14,8},{8,14,8}};
static ImVec4 LogoColor(int k) {
	static const float mixW[] = {0.0f, 0.12f, 0.28f, 0.40f, 0.58f, 0.75f, 0.92f, 1.0f};
	if (k == 8) return Mix(accent, Rgb(0, 0, 0), 0.32f);
	return Mix(Rgb(255, 255, 255), accent, mixW[k]);
}
// Hover wie im Launcher: Logo dreht sich leicht, wird groesser und leuchtet (mit Feder-Bewegung)
static void Logo(ImDrawList* dl, ImVec2 c, float size, float hover) {
	static float x = 0, v = 0;
	float dt = Dt();
	v += (hover - x) * 220.0f * dt; // Feder
	v *= expf(-14.0f * dt);         // Daempfung
	x += v * dt;
	float rot = -0.21f * x, sc = 1.0f + 0.16f * x;
	float glow = 0.22f + 0.30f * std::clamp(x, 0.0f, 1.2f);
	for (int i = 3; i >= 1; i--) dl->AddCircleFilled(c, size * (0.30f + 0.14f * i), U(Acc(glow * 0.18f)), 32);
	float px = size * sc / 16.0f, cs = cosf(rot), sn = sinf(rot);
	ImVec2 off(c.x + 1.0f * x, c.y - 1.0f * x);
	auto P = [&](float lx, float ly) { lx -= 8.0f; ly -= 8.0f; return ImVec2(off.x + (lx * cs - ly * sn) * px, off.y + (lx * sn + ly * cs) * px); };
	for (auto& p : kLogo) {
		ImU32 col = U(LogoColor(p.k));
		dl->AddQuadFilled(P(p.x, p.y), P(p.x + 1.04f, p.y), P(p.x + 1.04f, p.y + 1.04f), P(p.x, p.y + 1.04f), col);
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

static void Swatch(ImDrawList* dl, ImVec2 c, float r, const float* rgb) {
	dl->AddCircleFilled(c, r, U(ImVec4(rgb[0], rgb[1], rgb[2], 1.0f)), 32);
	dl->AddCircle(c, r, U(Rgb(255, 255, 255, 0.14f)), 32, 1.0f);
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

// Karte links (Farbe oder Lackierung)
static bool SideCard(ImDrawList* dl, int idx, ImVec2 cp, ImVec2 cq, const char* title, const char* sub, const float* swatch, ImVec4 dot) {
	ImGui::SetCursorScreenPos(cp);
	ImGui::PushID(idx + 50);
	bool click = ImGui::InvisibleButton("##ce", ImVec2(cq.x - cp.x, cq.y - cp.y));
	float h = Anim(ImGui::GetID("h"), ImGui::IsItemHovered() ? 1.0f : 0.0f);
	float sel = Anim(ImGui::GetID("s"), colorEdit == idx ? 1.0f : 0.0f);
	ImGui::PopID();
	dl->AddRectFilled(cp, cq, U(Mix(Mix(cPanel, cHover, h), Acc(0.12f), sel)), 12);
	dl->AddRect(cp, cq, U(Mix(cLine, Acc(0.55f), sel)), 12);
	float cy = (cp.y + cq.y) / 2;
	if (swatch) Swatch(dl, ImVec2(cp.x + 24, cy), 11, swatch);
	else {
		dl->AddRectFilled(ImVec2(cp.x + 13, cy - 11), ImVec2(cp.x + 35, cy + 11), U(Alpha(dot, 0.25f)), 6);
		dl->AddRect(ImVec2(cp.x + 13, cy - 11), ImVec2(cp.x + 35, cy + 11), U(Alpha(dot, 0.8f)), 6, 0, 1.2f);
	}
	Text(dl, fBold, ImVec2(cp.x + 46, cy - 18), cText, title);
	Text(dl, fSmall, ImVec2(cp.x + 46, cy + 1), cMuted, Fit(fSmall, fSmall->FontSize, sub, cq.x - cp.x - 58).c_str());
	return click;
}

static void ColorsBody(ImDrawList* dl, ImVec2 o, ImVec2 W, float top, float pad, ColorAction& act) {
	ImGuiIO& io = ImGui::GetIO();
	const float leftW = 250, listTop = top + 14;
	dl->AddLine(ImVec2(o.x + pad + leftW + 14, o.y + top + 1), ImVec2(o.x + pad + leftW + 14, o.y + W.y - 1), U(cLine));
	if (!editing) memcpy(editCols, items::colors, sizeof editCols);
	bool on = items::colorsOn, both = items::colorsBoth;
	int team = both ? 0 : colorTeam;
	float x = o.x + pad, y = o.y + listTop;

	// --- Links ---
	Text(dl, fTiny, ImVec2(x + 2, y), cFaint, "CAR COLORS");
	y += 20;
	{
		ImVec2 cp(x, y), cq(x + leftW, y + 92);
		dl->AddRectFilled(cp, cq, U(cPanel), 12);
		dl->AddRect(cp, cq, U(on ? Acc(0.45f) : cLine), 12);
		Text(dl, fBold, ImVec2(cp.x + 14, cp.y + 13), cText, "Custom colors");
		if (Toggle(dl, "##colon", ImVec2(cq.x - 56, cp.y + 11), on)) { on = !on; act.changed = act.save = true; }
		dl->AddLine(ImVec2(cp.x + 12, cp.y + 46), ImVec2(cq.x - 12, cp.y + 46), U(cLine));
		// "fuer beide Teams" als Kaestchen
		ImVec2 bp(cp.x + 14, cp.y + 59);
		ImGui::SetCursorScreenPos(ImVec2(cp.x + 8, cp.y + 52));
		bool bclick = ImGui::InvisibleButton("##both", ImVec2(leftW - 16, 32));
		float bh = Anim(ImGui::GetID("bothh"), ImGui::IsItemHovered() ? 1.0f : 0.0f);
		float bk = Anim(ImGui::GetID("bothk"), both ? 1.0f : 0.0f, 18.0f);
		dl->AddRectFilled(bp, ImVec2(bp.x + 18, bp.y + 18), U(Mix(Mix(cPanel2, cHover, bh), accent, bk)), 5);
		dl->AddRect(bp, ImVec2(bp.x + 18, bp.y + 18), U(Mix(cLine2, accent, bk)), 5);
		if (bk > 0.01f) {
			ImU32 w = U(Rgb(255, 255, 255, bk));
			dl->AddLine(ImVec2(bp.x + 4.5f, bp.y + 9.5f), ImVec2(bp.x + 7.5f, bp.y + 12.5f), w, 1.8f);
			dl->AddLine(ImVec2(bp.x + 7.5f, bp.y + 12.5f), ImVec2(bp.x + 13.5f, bp.y + 5.5f), w, 1.8f);
		}
		Text(dl, fRegular, ImVec2(bp.x + 28, bp.y), cText, "Use for both teams");
		if (bclick) { both = !both; act.changed = act.save = true; }
		y = cq.y + 10;
	}
	// Team-Auswahl (nur ohne "beide Teams")
	static float teamH = 0.0f;
	teamH = Approach(teamH, both ? 0.0f : 44.0f, 16.0f);
	if (teamH > 1.0f) {
		float a = std::clamp(teamH / 44.0f, 0.0f, 1.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * a);
		const char* tn[2] = {"Blue team", "Orange team"};
		ImVec4 tc[2] = {Rgb(70, 140, 255), Rgb(255, 140, 50)};
		float tw = (leftW - 8) / 2;
		for (int i = 0; i < 2; i++) {
			ImVec2 tp(x + i * (tw + 8), y), tq(tp.x + tw, y + 34);
			ImGui::SetCursorScreenPos(tp);
			ImGui::PushID(i + 300);
			if (ImGui::InvisibleButton("##team", ImVec2(tw, 34)) && !both) colorTeam = i;
			float h = Anim(ImGui::GetID("h"), ImGui::IsItemHovered() ? 1.0f : 0.0f);
			float sel = Anim(ImGui::GetID("s"), colorTeam == i ? 1.0f : 0.0f);
			ImGui::PopID();
			dl->AddRectFilled(tp, tq, U(Mix(Mix(cPanel, cHover, h), Alpha(tc[i], 0.18f), sel)), 10);
			dl->AddRect(tp, tq, U(Mix(cLine, Alpha(tc[i], 0.7f), sel)), 10);
			dl->AddCircleFilled(ImVec2(tp.x + 16, tp.y + 17), 4, U(tc[i]), 12);
			Text(dl, fBold, ImVec2(tp.x + 28, tp.y + 8), Mix(cMuted, cText, sel), tn[i]);
		}
		ImGui::PopStyleVar();
		y += teamH;
	}
	// Farben
	char hex[2][16];
	for (int c = 0; c < 2; c++)
		snprintf(hex[c], sizeof hex[c], "#%02X%02X%02X", int(editCols[team][c][0] * 255 + 0.5f), int(editCols[team][c][1] * 255 + 0.5f), int(editCols[team][c][2] * 255 + 0.5f));
	const float ch = 48, gap = 8;
	if (SideCard(dl, 0, ImVec2(x, y), ImVec2(x + leftW, y + ch), "Primary", hex[0], editCols[team][0], cMuted)) colorEdit = 0;
	y += ch + gap;
	if (SideCard(dl, 1, ImVec2(x, y), ImVec2(x + leftW, y + ch), "Accent", hex[1], editCols[team][1], cMuted)) colorEdit = 1;
	y += ch + 16;
	// Lackierungen (Paint Finish)
	Text(dl, fTiny, ImVec2(x + 2, y), cFaint, "PAINT FINISH");
	y += 20;
	for (int f = 0; f < 2; f++) {
		int slot = f == 0 ? 7 : 12;
		auto ov = items::overrides.find(slot);
		const items::Item* it = ov != items::overrides.end() ? FindItem(ov->second) : nullptr;
		if (!it) {
			auto ow = items::ownItems.find(slot);
			if (ow != items::ownItems.end()) it = FindItem(ow->second);
		}
		std::string sub = it ? it->name : std::string("Your own");
		if (ov == items::overrides.end() && it) sub += "  \xC2\xB7  yours";
		if (SideCard(dl, 2 + f, ImVec2(x, y), ImVec2(x + leftW, y + ch), f == 0 ? "Primary finish" : "Accent finish", sub.c_str(), nullptr,
				it ? QualityColor(it->quality) : cMuted))
			{ colorEdit = 2 + f; finishSearch[0] = 0; fScrollTarget = 0; fScrollLast = -1; }
		y += ch + gap;
	}

	// --- Rechts ---
	const float px = pad + leftW + 30, pw = W.x - px - pad;
	if (colorEdit <= 1) {
		const char* title = colorEdit == 0 ? "Primary color" : "Accent color";
		Text(dl, fTitle, ImVec2(o.x + px, o.y + listTop - 2), cText, title);
		if (!both) {
			const char* t = colorTeam == 0 ? "Blue team" : "Orange team";
			float w = TextW(fSmall, t);
			ImVec4 c = colorTeam == 0 ? Rgb(70, 140, 255) : Rgb(255, 140, 50);
			dl->AddCircleFilled(ImVec2(o.x + px + pw - w - 12, o.y + listTop + 9), 4, U(c), 12);
			Text(dl, fSmall, ImVec2(o.x + px + pw - w, o.y + listTop + 2), cMuted, t);
		}
		float* col = editCols[team][colorEdit];
		float before[3];
		memcpy(before, col, sizeof before);
		const float pickW = std::min(pw, 360.0f);
		ImGui::SetCursorScreenPos(ImVec2(o.x + px, o.y + listTop + 40));
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(12, 4));
		ImGui::SetNextItemWidth(pickW);
		ImGui::ColorPicker3("##pick", col, ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel |
			ImGuiColorEditFlags_PickerHueBar | ImGuiColorEditFlags_NoAlpha);
		bool active = ImGui::IsItemActive();
		ImVec2 pickMax = ImGui::GetItemRectMax();
		ImGui::PopStyleVar(2);
		// Schnellwahl: gleichmaessig ueber die volle Breite des Farbfelds verteilt
		static const float quick[][3] = {{1, 1, 1}, {0.05f, 0.05f, 0.06f}, {0.55f, 0.55f, 0.58f}, {0.9f, 0.1f, 0.12f}, {1, 0.45f, 0.05f}, {1, 0.82f, 0.1f},
			{0.35f, 0.85f, 0.2f}, {0.05f, 0.55f, 0.3f}, {0.15f, 0.75f, 1}, {0.1f, 0.3f, 0.95f}, {0.55f, 0.36f, 0.96f}, {1, 0.4f, 0.75f}};
		const int nq = 12;
		const float r = 10.0f;
		float qy = pickMax.y + 40, x0 = o.x + px + r, x1 = pickMax.x - r;
		Text(dl, fTiny, ImVec2(o.x + px, pickMax.y + 14), cFaint, "QUICK COLORS");
		for (int i = 0; i < nq; i++) {
			float qx = x0 + (x1 - x0) * i / (nq - 1);
			ImGui::SetCursorScreenPos(ImVec2(qx - 13, qy - 13));
			ImGui::PushID(i + 200);
			if (ImGui::InvisibleButton("##q", ImVec2(26, 26))) { memcpy(col, quick[i], sizeof before); act.save = true; }
			float h = Anim(ImGui::GetID("h"), ImGui::IsItemHovered() ? 1.0f : 0.0f, 22.0f);
			ImGui::PopID();
			Swatch(dl, ImVec2(qx, qy), r + 1.5f * h, quick[i]);
		}
		bool changedCol = memcmp(before, col, sizeof before) != 0;
		if (changedCol) { act.changed = true; on = true; } // Farbe waehlen schaltet die eigenen Farben ein
		if (both && changedCol) memcpy(editCols[1][colorEdit], col, sizeof before); // beide Teams gleich
		if (editing && !active) act.save = act.changed = true; // Ziehen beendet: speichern
		editing = active;
	} else {
		// Lackierung waehlen (Liste wie bei den Items)
		int slot = colorEdit == 2 ? 7 : 12;
		Text(dl, fTitle, ImVec2(o.x + px, o.y + listTop - 2), cText, colorEdit == 2 ? "Primary finish" : "Accent finish");
		std::string q = Lower(finishSearch);
		std::vector<const items::Item*> shown;
		for (auto& it : items::list)
			if (it.slot == 7 && (q.empty() || Lower(it.name).find(q) != std::string::npos)) shown.push_back(&it);
		const float sy = listTop + 36, sh = 42;
		Field(dl, "##fq", "Search", finishSearch, sizeof finishSearch, ImVec2(o.x + px, o.y + sy), ImVec2(o.x + px + pw, o.y + sy + sh), focusFinish, true, true);
		int cur = items::overrides.count(slot) ? items::overrides[slot] : 0;
		int pick = -1;
		const float ly = sy + sh + 12;
		ImGui::SetCursorScreenPos(ImVec2(o.x + px - 4, o.y + ly));
		ImGui::BeginChild("finishes", ImVec2(pw + 8, W.y - ly - 14), false, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse);
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
		(void)io;
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
	std::unique_lock<std::mutex> lk(items::mu);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImVec2 wp = ImGui::GetWindowPos();
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
		float tw = TextW(fTitle, "ZLauncher");
		bool hov = wantOpen && ImGui::IsMouseHoveringRect(ImVec2(lc.x - 16, lc.y - 18), ImVec2(o.x + pad + 34 + tw, lc.y + 18), false);
		Logo(dl, lc, 26.0f, hov ? 1.0f : 0.0f);
	}
	Text(dl, fTitle, ImVec2(o.x + pad + 34, o.y + 21), cText, "ZLauncher");
	// Reiter "Items" / "Presets" mit gleitender Markierung
	{
		const char* names[3] = {"Items", "Colors", "Presets"};
		const int ids[3] = {0, 2, 1}; // Reihenfolge auf dem Bildschirm -> Reiter-Nummer
		float x0 = o.x + pad + 34 + TextW(fTitle, "ZLauncher") + 22, y0 = o.y + 15;
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
			}
			ImGui::PopID();
		}
		for (int i = 0; i < 3; i++) if (ids[i] == tab) curPos = i;
		float target = tx[curPos] - o.x;
		tabX = tabX < 0 ? target : Approach(tabX, target, 18.0f);
		tabW = tabW <= 0 ? tws[curPos] : Approach(tabW, tws[curPos], 18.0f);
		dl->AddRectFilled(ImVec2(o.x + tabX, y0 + 4), ImVec2(o.x + tabX + tabW, y0 + 34), A(Acc(0.20f)), 15);
		for (int i = 0; i < 3; i++) {
			float on = Anim(ImGui::GetID(names[i]), tab == ids[i] ? 1.0f : 0.0f);
			float nw = TextW(fBold, names[i]);
			Text(dl, fBold, ImVec2(tx[i] + (tws[i] - nw) / 2, y0 + 10), Mix(cMuted, Mix(accent, Rgb(255, 255, 255), 0.65f), on), names[i]);
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
		if (action == 2) items::DeletePreset(idx);
		if (action == 3) { items::AddPreset(name); presetName[0] = 0; pScrollTarget = 0; }
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
	if (rem > 0.01f) d->AddRectFilled(ImVec2(lx0, p.y + sz.y - 4), ImVec2(lx0 + (lx1 - lx0) * rem, p.y + sz.y - 2), C(Acc(0.75f)), 1.0f);
}

} // namespace gui
