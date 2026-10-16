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
	if (it) {
		ImVec4 qc = QualityColor(it->quality);
		d->AddCircleFilled(ImVec2(p.x + size.x - 18, p.y + size.y / 2), 3.5f, U(qc), 16);
		auto pp = items::paints.find(s.index);
		if (it->paintable && pp != items::paints.end())
			for (auto& pi : items::paintList)
				if (pi.id == pp->second) {
					d->AddCircleFilled(ImVec2(p.x + size.x - 34, p.y + size.y / 2), 6.0f, U(ImVec4(pi.rgb[0], pi.rgb[1], pi.rgb[2], 1.0f)), 16);
					d->AddCircle(ImVec2(p.x + size.x - 34, p.y + size.y / 2), 6.0f, U(Rgb(255, 255, 255, 0.15f)), 16, 1.0f);
				}
	}
	float sa = Anim(ImGui::GetID("sa"), sel ? 1.0f : 0.0f);
	Text(d, fTiny, ImVec2(tx, p.y + 8), Mix(cFaint, Acc(1.0f), sa * 0.9f), s.label);
	std::string name = Fit(fRegular, fRegular->FontSize, it ? it->name : "Your own", size.x - 60);
	Text(d, fRegular, ImVec2(tx, p.y + 22), it ? Mix(Hex(0xc9c9d3), cText, std::max(sa, h)) : Mix(cMuted, Hex(0xa9a9b5), h), name.c_str());
	ImGui::PopID();
}

// ---------- Rechts: eine Zeile ----------
static void ItemRow(int id, const char* name, const char* sub, ImVec4 qc, int cur, int& pick, float w) {
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

// Eingabefeld im ZLauncher-Stil (Hintergrund, Rand, optional Lupe). true = Enter gedrueckt
static bool Field(ImDrawList* dl, const char* id, const char* hint, char* buf, size_t n, ImVec2 sp, ImVec2 sq, bool& focusFlag, bool icon,
	bool clearBtn) {
	const float sh = sq.y - sp.y;
	ImGuiID aid = ImGui::GetID(id);
	float* f = ImGui::GetStateStorage()->GetFloatRef(aid + 7, 0.0f);
	dl->AddRectFilled(sp, sq, U(Mix(cPanel, cPanel2, *f)), 12);
	dl->AddRect(sp, sq, U(Mix(cLine2, Acc(0.7f), *f)), 12, 0, 1.0f);
	float tx = sp.x + 14;
	if (icon) {
		ImVec2 lc(sp.x + 20, sp.y + sh / 2 - 1);
		ImU32 c = U(Mix(cFaint, Acc(1.0f), *f));
		dl->AddCircle(lc, 5.5f, c, 20, 1.6f);
		dl->AddLine(ImVec2(lc.x + 4, lc.y + 4), ImVec2(lc.x + 8, lc.y + 8), c, 1.6f);
		tx = sp.x + 38;
	}
	bool showClr = clearBtn && buf[0];
	ImGui::SetCursorScreenPos(ImVec2(tx, sp.y + (sh - fRegular->FontSize) / 2 - 11));
	ImGui::SetNextItemWidth(sq.x - tx - (showClr ? 40 : 14));
	if (focusFlag) { ImGui::SetKeyboardFocusHere(); focusFlag = false; }
	ImGui::PushStyleColor(ImGuiCol_Text, cText);
	bool enter = ImGui::InputTextWithHint(id, hint, buf, n, ImGuiInputTextFlags_EnterReturnsTrue);
	ImGui::PopStyleColor();
	bool active = ImGui::IsItemActive();
	*f = Approach(*f, active ? 1.0f : 0.0f, 18.0f);
	// Klick irgendwo ins Feld aktiviert es zuverlaessig
	if (!active && ImGui::IsMouseClicked(0) && ImGui::IsMouseHoveringRect(sp, sq, false) &&
		!(showClr && ImGui::IsMouseHoveringRect(ImVec2(sq.x - 38, sp.y), sq, false)))
		focusFlag = true;
	if (showClr) {
		ImGui::SetCursorScreenPos(ImVec2(sq.x - 38, sp.y + (sh - 32) / 2));
		ImGui::PushID(id);
		if (ImGui::InvisibleButton("##clr", ImVec2(32, 32))) { buf[0] = 0; focusFlag = true; }
		float h = Anim(ImGui::GetID("clrh"), ImGui::IsItemHovered() ? 1.0f : 0.0f);
		ImGui::PopID();
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
	if (fabsf(ns - target) < 0.5f) ns = target;
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
	dl->AddCircleFilled(ImVec2(o.x + pad + 6, o.y + 34), 6, A(accent), 20);
	dl->AddCircleFilled(ImVec2(o.x + pad + 6, o.y + 34), 11, A(Acc(0.18f)), 24);
	Text(dl, fTitle, ImVec2(o.x + pad + 24, o.y + 21), cText, "ZLauncher");
	// Reiter "Items" / "Presets" mit gleitender Markierung
	{
		const char* names[2] = {"Items", "Presets"};
		float x0 = o.x + pad + 24 + TextW(fTitle, "ZLauncher") + 22, y0 = o.y + 15;
		float w0 = TextW(fBold, names[0]) + 34, w1 = TextW(fBold, names[1]) + 34;
		ImVec2 bp(x0, y0), bq(x0 + w0 + w1 + 8, y0 + 38);
		dl->AddRectFilled(bp, bq, A(cPanel), 19);
		dl->AddRect(bp, bq, A(cLine), 19);
		float tx[2] = {x0 + 4, x0 + 4 + w0}, tws[2] = {w0, w1};
		for (int i = 0; i < 2; i++) {
			ImGui::SetCursorScreenPos(ImVec2(tx[i], y0 + 4));
			ImGui::PushID(i);
			if (ImGui::InvisibleButton("##tab", ImVec2(tws[i], 30)) && tab != i) {
				tab = i;
				tabFade = 0.0f;
				pendingDelete = -1;
			}
			ImGui::PopID();
		}
		float target = tx[tab] - o.x;
		tabX = tabX < 0 ? target : Approach(tabX, target, 18.0f);
		tabW = tabW <= 0 ? tws[tab] : Approach(tabW, tws[tab], 18.0f);
		dl->AddRectFilled(ImVec2(o.x + tabX, y0 + 4), ImVec2(o.x + tabX + tabW, y0 + 34), A(Acc(0.20f)), 15);
		for (int i = 0; i < 2; i++) {
			float on = Anim(ImGui::GetID(names[i]), tab == i ? 1.0f : 0.0f);
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
	for (int i = 0; i < items::kSlotCount; i++) SlotRow(items::kSlots[i], ImVec2(leftW, rowH), selY);
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
	for (auto& it : items::list) {
		if (it.slot != curSlot) continue;
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
	bool showColors = curItem && curItem->paintable && !items::paintList.empty();
	static float colorH = 0.0f;
	colorH = Approach(colorH, showColors ? 46.0f : 0.0f, 16.0f);
	int pickPaint = -1;
	if (colorH > 1.0f) {
		float a = std::clamp(colorH / 46.0f, 0.0f, 1.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * a);
		int curPaint = items::paints.count(curSlot) ? items::paints[curSlot] : 0;
		float cy = o.y + listTop + 34 + 18;
		Text(dl, fTiny, ImVec2(o.x + px, cy - 7), cFaint, "COLOR");
		float x = o.x + px + 52;
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
			x += 25;
		};
		swatch(0, cMuted, "Default");
		for (auto& pi : items::paintList) swatch(pi.id, ImVec4(pi.rgb[0], pi.rgb[1], pi.rgb[2], 1.0f), pi.name.c_str());
		// Name der Farbe (beim Drueberfahren, sonst die gewaehlte)
		const char* shownName = hoverName;
		if (!shownName) {
			shownName = "Default";
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
	ItemRow(0, "Your own", "", Rgb(80, 80, 92), cur, pick, rw);
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
