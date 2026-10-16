// F8-Menue im ZLauncher-Stil: links die Slots, rechts die Auswahl fuer den Slot.
// Alles selbst gezeichnet, mit weichen Uebergaengen (Oeffnen/Schliessen, Hover, Auswahl, Scrollen).
#include "gui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
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
static float scrollTarget = 0.0f, scrollLastSet = -1.0f;
static float indicatorY = -1.0f;  // gleitende Markierung links

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
		scrollLastSet = -1.0f;
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
	}
	float sa = Anim(ImGui::GetID("sa"), sel ? 1.0f : 0.0f);
	Text(d, fTiny, ImVec2(tx, p.y + 8), Mix(cFaint, Acc(1.0f), sa * 0.9f), s.label);
	std::string name = Fit(fRegular, fRegular->FontSize, it ? it->name : "Your own", size.x - 46);
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
	Text(dl, fTitle, ImVec2(o.x + pad + 24, o.y + 21), cText, "Items");
	float tw = TextW(fTitle, "Items");
	Text(dl, fSmall, ImVec2(o.x + pad + 34 + tw, o.y + 28), cFaint, "ZLauncher  ·  only you can see these");

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

	// Suchfeld
	const float sy = listTop + 36, sh = 42;
	ImVec2 sp(o.x + px, o.y + sy), sq(o.x + px + pw, o.y + sy + sh);
	static float sFocus = 0.0f;
	// Feld zeichnen (vor dem Text, damit es darunter liegt)
	{
		dl->AddRectFilled(sp, sq, A(Mix(cPanel, cPanel2, sFocus)), 12);
		dl->AddRect(sp, sq, A(Mix(cLine2, Acc(0.7f), sFocus)), 12, 0, 1.0f);
		ImVec2 lc(sp.x + 20, sp.y + sh / 2 - 1);
		ImU32 lcCol = A(Mix(cFaint, Acc(1.0f), sFocus));
		dl->AddCircle(lc, 5.5f, lcCol, 20, 1.6f);
		dl->AddLine(ImVec2(lc.x + 4, lc.y + 4), ImVec2(lc.x + 8, lc.y + 8), lcCol, 1.6f);
	}
	ImGui::SetCursorScreenPos(ImVec2(sp.x + 38, sp.y + (sh - fRegular->FontSize) / 2 - 11));
	ImGui::SetNextItemWidth(pw - 38 - (search[0] ? 40 : 14));
	if (focusSearch) { ImGui::SetKeyboardFocusHere(); focusSearch = false; }
	ImGui::PushStyleColor(ImGuiCol_Text, cText);
	ImGui::InputTextWithHint("##q", "Search", search, sizeof search);
	ImGui::PopStyleColor();
	bool active = ImGui::IsItemActive();
	sFocus = Approach(sFocus, active ? 1.0f : 0.0f, 18.0f);
	// Klick irgendwo ins Feld aktiviert es zuverlaessig
	if (!active && ImGui::IsMouseClicked(0) && ImGui::IsMouseHoveringRect(sp, sq, false) &&
		!(search[0] && ImGui::IsMouseHoveringRect(ImVec2(sq.x - 38, sp.y), sq, false)))
		focusSearch = true;
	// Leeren-Knopf
	if (search[0]) {
		ImGui::SetCursorScreenPos(ImVec2(sq.x - 38, sp.y + 5));
		if (ImGui::InvisibleButton("##clr", ImVec2(32, 32))) { search[0] = 0; focusSearch = true; }
		float h = Anim(ImGui::GetID("clrh"), ImGui::IsItemHovered() ? 1.0f : 0.0f);
		ImVec2 c(sq.x - 22, sp.y + sh / 2);
		dl->AddCircleFilled(c, 9, A(Rgb(255, 255, 255, 0.06f + 0.08f * h)), 20);
		ImU32 xc = A(Mix(cMuted, cText, h));
		dl->AddLine(ImVec2(c.x - 3, c.y - 3), ImVec2(c.x + 3, c.y + 3), xc, 1.4f);
		dl->AddLine(ImVec2(c.x + 3, c.y - 3), ImVec2(c.x - 3, c.y + 3), xc, 1.4f);
	}

	// Liste
	int cur = items::overrides.count(curSlot) ? items::overrides[curSlot] : 0;
	int pick = -1;
	const float ly = sy + sh + 12;
	ImGui::SetCursorScreenPos(ImVec2(o.x + px - 4, o.y + ly + (1.0f - EaseOut(listFade)) * 8.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_Alpha, e * EaseOut(listFade));
	ImGui::BeginChild("rows", ImVec2(pw + 8, W.y - ly - 14), false, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse);
	{
		// weiches Scrollen mit dem Mausrad
		float sc = ImGui::GetScrollY(), mx = ImGui::GetScrollMaxY();
		if (scrollLastSet >= 0 && fabsf(sc - scrollLastSet) > 1.0f) scrollTarget = sc; // Scrollleiste gezogen
		if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && io.MouseWheel != 0.0f) scrollTarget -= io.MouseWheel * 140.0f;
		scrollTarget = std::clamp(scrollTarget, 0.0f, mx);
		float ns = Approach(sc, scrollTarget, 16.0f);
		if (fabsf(ns - scrollTarget) < 0.5f) ns = scrollTarget;
		if (ns != sc) ImGui::SetScrollY(ns);
		scrollLastSet = ns;
	}
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

	lk.unlock();
	ImGui::End();
	ImGui::PopFont();
	ImGui::PopStyleVar();

	if (pick >= 0 && pick != cur) items::Set(curSlot, pick);
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
