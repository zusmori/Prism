// F8-Menue im ZLauncher-Stil: links das Loadout als Kacheln, rechts die Auswahl fuer den Slot
#include "gui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>

#include "imgui.h"
#include "items.h"
#include "log.h"

namespace gui {

static ImVec4 accent(0.545f, 0.361f, 0.965f, 1.0f);
static ImFont *fRegular = nullptr, *fSmall = nullptr, *fBold = nullptr, *fTitle = nullptr;
static int curSlot = 0;
static char search[64] = {0};
static bool requested = false;
static bool focusSearch = true;

static ImVec4 Rgb(int r, int g, int b, float a = 1.0f) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a); }
static ImU32 U(ImVec4 c) { return ImGui::GetColorU32(c); }
static ImVec4 Acc(float a) { return ImVec4(accent.x, accent.y, accent.z, a); }

static const char* kQuality[] = {"Common", "Uncommon", "Rare", "Very Rare", "Import", "Exotic", "Black Market", "Premium", "Limited", "Legacy"};
static ImVec4 QualityColor(int q) {
	switch (q) {
	case 1: return Rgb(110, 180, 255);
	case 2: return Rgb(90, 140, 255);
	case 3: return Rgb(160, 100, 255);
	case 4: return Rgb(230, 70, 70);
	case 5: return Rgb(240, 200, 60);
	case 6: return Rgb(240, 90, 200);
	case 7: return Rgb(90, 220, 140);
	case 8: return Rgb(250, 150, 50);
	case 9: return Rgb(200, 200, 120);
	default: return Rgb(170, 170, 180);
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
	fRegular = load("segoeui.ttf", 17.0f);
	if (!fRegular) fRegular = io.Fonts->AddFontDefault();
	fSmall = load("segoeui.ttf", 13.5f);
	fBold = load("seguisb.ttf", 16.0f);
	fTitle = load("segoeuib.ttf", 24.0f);
	if (!fSmall) fSmall = fRegular;
	if (!fBold) fBold = fRegular;
	if (!fTitle) fTitle = fBold;
}

void Style() {
	ReadTheme();
	ImGuiStyle& s = ImGui::GetStyle();
	s.WindowRounding = 18; s.ChildRounding = 14; s.FrameRounding = 10; s.GrabRounding = 8; s.ScrollbarRounding = 8; s.PopupRounding = 10;
	s.WindowPadding = ImVec2(0, 0); s.FramePadding = ImVec2(12, 9); s.ItemSpacing = ImVec2(8, 4);
	s.WindowBorderSize = 1; s.ChildBorderSize = 0; s.FrameBorderSize = 1; s.ScrollbarSize = 8;
	ImVec4* c = s.Colors;
	c[ImGuiCol_WindowBg] = Rgb(12, 12, 16, 0.97f);
	c[ImGuiCol_ChildBg] = Rgb(0, 0, 0, 0);
	c[ImGuiCol_Border] = Rgb(255, 255, 255, 0.09f);
	c[ImGuiCol_FrameBg] = Rgb(9, 9, 12);
	c[ImGuiCol_FrameBgHovered] = Rgb(14, 14, 18);
	c[ImGuiCol_FrameBgActive] = Rgb(14, 14, 18);
	c[ImGuiCol_Text] = Rgb(236, 236, 242);
	c[ImGuiCol_TextDisabled] = Rgb(120, 120, 135);
	c[ImGuiCol_TextSelectedBg] = Acc(0.4f);
	c[ImGuiCol_ScrollbarBg] = Rgb(0, 0, 0, 0);
	c[ImGuiCol_ScrollbarGrab] = Rgb(55, 55, 66);
	c[ImGuiCol_ScrollbarGrabHovered] = Rgb(75, 75, 88);
}

static std::string Lower(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return s;
}

static const items::Item* FindItem(int id) {
	for (auto& it : items::list)
		if (it.id == id) return &it;
	return nullptr;
}

static void Check(ImDrawList* dl, ImVec2 c) {
	dl->AddCircleFilled(c, 9, U(accent));
	dl->AddLine(ImVec2(c.x - 4, c.y + 0.5f), ImVec2(c.x - 1, c.y + 3.5f), U(Rgb(255, 255, 255)), 2);
	dl->AddLine(ImVec2(c.x - 1, c.y + 3.5f), ImVec2(c.x + 4.5f, c.y - 3), U(Rgb(255, 255, 255)), 2);
}

// ---------- Loadout-Kachel ----------
static void Tile(const items::SlotInfo& s, ImVec2 size) {
	ImGui::PushID(s.index);
	ImVec2 p = ImGui::GetCursorScreenPos();
	bool click = ImGui::InvisibleButton("##t", size);
	bool hov = ImGui::IsItemHovered();
	if (click) { curSlot = s.index; search[0] = 0; focusSearch = true; }
	ImDrawList* dl = ImGui::GetWindowDrawList();
	bool sel = curSlot == s.index;
	auto ov = items::overrides.find(s.index);
	const items::Item* it = ov != items::overrides.end() ? FindItem(ov->second) : nullptr;
	ImVec4 qc = it ? QualityColor(it->quality) : Rgb(120, 120, 135);
	ImVec2 q(p.x + size.x, p.y + size.y);
	dl->AddRectFilled(p, q, U(hov || sel ? Rgb(26, 26, 34) : Rgb(20, 20, 26)), 14);
	if (it) dl->AddRectFilledMultiColor(ImVec2(p.x + 1, p.y + 1), ImVec2(p.x + size.x * 0.6f, q.y - 1),
		U(ImVec4(qc.x, qc.y, qc.z, 0.14f)), U(ImVec4(qc.x, qc.y, qc.z, 0.0f)), U(ImVec4(qc.x, qc.y, qc.z, 0.0f)), U(ImVec4(qc.x, qc.y, qc.z, 0.14f)));
	dl->AddRect(p, q, U(sel ? Acc(0.85f) : Rgb(255, 255, 255, hov ? 0.12f : 0.06f)), 14, 0, sel ? 1.6f : 1.0f);
	// Icon: Anfangsbuchstabe im Kreis, leuchtet in der Seltenheit
	ImVec2 ic(p.x + 30, p.y + size.y / 2);
	if (it) dl->AddCircleFilled(ic, 19, U(ImVec4(qc.x, qc.y, qc.z, 0.18f)));
	dl->AddCircle(ic, 18, U(it ? ImVec4(qc.x, qc.y, qc.z, 0.7f) : Rgb(255, 255, 255, 0.1f)), 0, 1.4f);
	char letter[2] = {s.label[0], 0};
	ImGui::PushFont(fBold);
	ImVec2 ls = ImGui::CalcTextSize(letter);
	dl->AddText(ImVec2(ic.x - ls.x / 2, ic.y - ls.y / 2), U(it ? Rgb(255, 255, 255) : Acc(1.0f)), letter);
	ImGui::PopFont();
	// Texte
	float tx = p.x + 58, maxw = size.x - 70;
	ImGui::PushFont(fSmall);
	std::string lbl = s.label;
	std::transform(lbl.begin(), lbl.end(), lbl.begin(), ::toupper);
	dl->AddText(ImVec2(tx, p.y + 13), U(Rgb(120, 120, 135)), lbl.c_str());
	ImGui::PopFont();
	ImGui::PushFont(it ? fBold : fRegular);
	std::string name = it ? it->name : "Your own";
	while (name.size() > 3 && ImGui::CalcTextSize(name.c_str()).x > maxw) name = name.substr(0, name.size() - 4) + "…";
	dl->AddText(ImVec2(tx, p.y + 31), U(it ? Rgb(245, 245, 250) : Rgb(150, 150, 162)), name.c_str());
	ImGui::PopFont();
	ImGui::PopID();
}

// ---------- Fenster ----------
void Render(bool* open) {
	if (!requested) {
		requested = true;
		items::Queue([] { items::LoadProducts(); });
	}
	const ImVec2 W(900, 580);
	ImGuiIO& io = ImGui::GetIO();
	ImGui::SetNextWindowSize(W, ImGuiCond_Always);
	ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
	ImGui::PushFont(fRegular);
	if (!ImGui::Begin("##zlitems", open, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
		ImGui::End();
		ImGui::PopFont();
		return;
	}
	std::unique_lock<std::mutex> lk(items::mu);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImVec2 o = ImGui::GetWindowPos();

	// Leuchten oben links in der Akzentfarbe
	dl->AddRectFilledMultiColor(ImVec2(o.x + 1, o.y + 1), ImVec2(o.x + 420, o.y + 120),
		U(Acc(0.16f)), U(Acc(0.0f)), U(Acc(0.0f)), U(Acc(0.0f)));

	// Kopf
	ImGui::PushFont(fSmall);
	dl->AddText(ImVec2(o.x + 24, o.y + 18), U(accent), "ZLAUNCHER");
	ImGui::PopFont();
	ImGui::PushFont(fTitle);
	dl->AddText(ImVec2(o.x + 23, o.y + 32), U(Rgb(255, 255, 255)), "Items");
	ImGui::PopFont();
	ImGui::PushFont(fSmall);
	const char* hint = "Only you can see these  ·  F8 to close";
	dl->AddText(ImVec2(o.x + 102, o.y + 42), U(Rgb(120, 120, 135)), hint);
	ImGui::PopFont();
	// Schliessen
	ImGui::SetCursorPos(ImVec2(W.x - 52, 18));
	if (ImGui::InvisibleButton("##close", ImVec2(32, 32))) *open = false;
	{
		ImVec2 c(o.x + W.x - 36, o.y + 34);
		bool h = ImGui::IsItemHovered();
		dl->AddRectFilled(ImVec2(c.x - 16, c.y - 16), ImVec2(c.x + 16, c.y + 16), U(h ? Rgb(255, 255, 255, 0.1f) : Rgb(255, 255, 255, 0.04f)), 10);
		ImU32 xc = U(h ? Rgb(255, 255, 255) : Rgb(170, 170, 180));
		dl->AddLine(ImVec2(c.x - 5, c.y - 5), ImVec2(c.x + 5, c.y + 5), xc, 1.8f);
		dl->AddLine(ImVec2(c.x + 5, c.y - 5), ImVec2(c.x - 5, c.y + 5), xc, 1.8f);
	}

	const float top = 82, pad = 20;
	if (!items::loaded) {
		const char* msg = items::status.empty() ? "Loading items …" : items::status.c_str();
		ImVec2 ts = ImGui::CalcTextSize(msg);
		dl->AddText(ImVec2(o.x + (W.x - ts.x) / 2, o.y + W.y / 2 - 20), U(Rgb(160, 160, 172)), msg);
		ImGui::SetCursorPos(ImVec2((W.x - 120) / 2, W.y / 2 + 8));
		if (ImGui::InvisibleButton("##reload", ImVec2(120, 34))) items::Queue([] { items::LoadProducts(); });
		ImVec2 b(o.x + (W.x - 120) / 2, o.y + W.y / 2 + 8);
		dl->AddRectFilled(b, ImVec2(b.x + 120, b.y + 34), U(ImGui::IsItemHovered() ? accent : Acc(0.85f)), 10);
		ImVec2 bs = ImGui::CalcTextSize("Try again");
		dl->AddText(ImVec2(b.x + (120 - bs.x) / 2, b.y + (34 - bs.y) / 2), U(Rgb(255, 255, 255)), "Try again");
		lk.unlock();
		ImGui::End();
		ImGui::PopFont();
		return;
	}

	// Links: Loadout-Kacheln (2 Spalten)
	const float leftW = 430, tileH = 70, gap = 10;
	const float tileW = (leftW - gap) / 2;
	ImGui::SetCursorPos(ImVec2(pad, top));
	ImGui::BeginChild("tiles", ImVec2(leftW, W.y - top - pad), false, ImGuiWindowFlags_NoBackground);
	for (int i = 0; i < items::kSlotCount; i++) {
		if (i % 2) ImGui::SameLine(0, gap);
		else if (i) ImGui::Dummy(ImVec2(0, gap - 4));
		Tile(items::kSlots[i], ImVec2(tileW, tileH));
	}
	ImGui::EndChild();

	// Rechts: Auswahl
	const float px = pad + leftW + 14, pw = W.x - px - pad, ph = W.y - top - pad;
	ImVec2 P(o.x + px, o.y + top);
	dl->AddRectFilled(P, ImVec2(P.x + pw, P.y + ph), U(Rgb(18, 18, 24)), 16);
	dl->AddRect(P, ImVec2(P.x + pw, P.y + ph), U(Acc(0.22f)), 16);
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
	ImGui::PushFont(fBold);
	dl->AddText(ImVec2(P.x + 18, P.y + 16), U(Rgb(255, 255, 255)), slotName);
	float nw = ImGui::CalcTextSize(slotName).x;
	ImGui::PopFont();
	ImGui::PushFont(fSmall);
	char cnt[48];
	snprintf(cnt, sizeof cnt, "%d items", total);
	dl->AddText(ImVec2(P.x + 26 + nw, P.y + 19), U(Rgb(120, 120, 135)), cnt);
	ImGui::PopFont();

	ImGui::SetCursorPos(ImVec2(px + 14, top + 46));
	ImGui::PushStyleColor(ImGuiCol_Border, focusSearch ? Acc(0.5f) : Rgb(255, 255, 255, 0.08f));
	ImGui::SetNextItemWidth(pw - 28);
	if (focusSearch) { ImGui::SetKeyboardFocusHere(); focusSearch = false; }
	ImGui::InputTextWithHint("##q", "Search …", search, sizeof search);
	ImGui::PopStyleColor();

	int cur = items::overrides.count(curSlot) ? items::overrides[curSlot] : 0;
	int pick = -1;
	ImGui::SetCursorPos(ImVec2(px + 8, top + 92));
	ImGui::BeginChild("rows", ImVec2(pw - 16, ph - 100), false, ImGuiWindowFlags_NoBackground);
	auto row = [&](int id, const char* name, const char* sub, ImVec4 qc) {
		ImGui::PushID(id);
		ImVec2 p = ImGui::GetCursorScreenPos();
		float w = ImGui::GetContentRegionAvail().x;
		if (ImGui::InvisibleButton("##r", ImVec2(w, 38))) pick = id;
		bool hov = ImGui::IsItemHovered(), sel = cur == id;
		ImDrawList* d = ImGui::GetWindowDrawList();
		if (sel || hov) d->AddRectFilled(p, ImVec2(p.x + w, p.y + 38), U(sel ? Acc(0.16f) : Rgb(255, 255, 255, 0.04f)), 10);
		d->AddRectFilled(ImVec2(p.x + 10, p.y + 9), ImVec2(p.x + 14, p.y + 29), U(qc), 2);
		d->AddText(fRegular, 17.0f, ImVec2(p.x + 24, p.y + 3), U(Rgb(240, 240, 246)), name);
		d->AddText(fSmall, 13.5f, ImVec2(p.x + 24, p.y + 20), U(sub[0] ? ImVec4(qc.x, qc.y, qc.z, 0.9f) : Rgb(120, 120, 135)), sub);
		if (sel) Check(d, ImVec2(p.x + w - 20, p.y + 19));
		ImGui::PopID();
	};
	row(0, "Your own", "What you have equipped", Rgb(80, 80, 92));
	ImGuiListClipper clip;
	clip.Begin((int)shown.size(), 38.0f + ImGui::GetStyle().ItemSpacing.y);
	while (clip.Step())
		for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
			const items::Item* it = shown[r];
			row(it->id, it->name.c_str(), kQuality[std::clamp(it->quality, 0, 9)], QualityColor(it->quality));
		}
	if (!q.empty() && shown.empty()) ImGui::TextDisabled("   Nothing found");
	ImGui::EndChild();

	lk.unlock();
	ImGui::End();
	ImGui::PopFont();

	if (pick >= 0 && pick != cur) items::Set(curSlot, pick);
}

} // namespace gui
