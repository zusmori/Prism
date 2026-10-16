// F8-Menue im ZLauncher-Stil
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
static int curSlot = 0;
static char search[64] = {0};
static bool requested = false;

static ImVec4 Rgb(int r, int g, int b, float a = 1.0f) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a); }
static ImVec4 Acc(float a) { return ImVec4(accent.x, accent.y, accent.z, a); }

static void ReadTheme() {
	FILE* f = _wfopen((zl::DataDir() + L"\\settings.json").c_str(), L"r");
	if (!f) return;
	char buf[8192] = {0};
	fread(buf, 1, sizeof buf - 1, f);
	fclose(f);
	std::string s(buf);
	if (s.find("\"theme\":\"blue\"") != std::string::npos) accent = Rgb(59, 130, 246);
	else if (s.find("\"theme\":\"cyan\"") != std::string::npos) accent = Rgb(6, 182, 212);
	else if (s.find("\"theme\":\"green\"") != std::string::npos) accent = Rgb(16, 185, 129);
}

void Style() {
	ReadTheme();
	ImGuiStyle& s = ImGui::GetStyle();
	s.WindowRounding = 14; s.ChildRounding = 11; s.FrameRounding = 8; s.GrabRounding = 8; s.ScrollbarRounding = 8; s.PopupRounding = 10;
	s.WindowPadding = ImVec2(16, 14); s.FramePadding = ImVec2(10, 7); s.ItemSpacing = ImVec2(8, 6);
	s.WindowBorderSize = 1; s.ChildBorderSize = 0; s.ScrollbarSize = 10; s.WindowTitleAlign = ImVec2(0.0f, 0.5f);
	ImVec4* c = s.Colors;
	c[ImGuiCol_WindowBg] = Rgb(13, 13, 17, 0.97f);
	c[ImGuiCol_ChildBg] = Rgb(20, 20, 26);
	c[ImGuiCol_Border] = Rgb(255, 255, 255, 0.08f);
	c[ImGuiCol_TitleBg] = Rgb(13, 13, 17);
	c[ImGuiCol_TitleBgActive] = Rgb(18, 17, 25);
	c[ImGuiCol_FrameBg] = Rgb(26, 26, 33);
	c[ImGuiCol_FrameBgHovered] = Rgb(32, 32, 41);
	c[ImGuiCol_FrameBgActive] = Rgb(36, 36, 46);
	c[ImGuiCol_Header] = Acc(0.28f);
	c[ImGuiCol_HeaderHovered] = Acc(0.18f);
	c[ImGuiCol_HeaderActive] = Acc(0.38f);
	c[ImGuiCol_Button] = Acc(0.85f);
	c[ImGuiCol_ButtonHovered] = accent;
	c[ImGuiCol_ButtonActive] = Acc(0.7f);
	c[ImGuiCol_Text] = Rgb(236, 236, 242);
	c[ImGuiCol_TextDisabled] = Rgb(130, 130, 145);
	c[ImGuiCol_ScrollbarBg] = Rgb(0, 0, 0, 0);
	c[ImGuiCol_ScrollbarGrab] = Rgb(60, 60, 72);
	c[ImGuiCol_CheckMark] = Rgb(255, 255, 255);
}

static ImVec4 QualityColor(int q) {
	switch (q) {
	case 1: return Rgb(110, 180, 255);
	case 2: return Rgb(80, 140, 255);
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

static std::string Lower(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return s;
}

void Render(bool* open) {
	if (!requested) {
		requested = true;
		items::Queue([] { items::LoadProducts(); });
	}
	ImGui::SetNextWindowSize(ImVec2(780, 520), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSizeConstraints(ImVec2(560, 380), ImVec2(4000, 4000));
	if (!ImGui::Begin("ZLauncher  ·  Items##zl", open, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	std::lock_guard<std::mutex> lk(items::mu);

	ImGui::TextColored(accent, "ITEMS");
	ImGui::SameLine();
	ImGui::TextDisabled("Only you can see these. F8 to close.");

	if (!items::loaded) {
		ImGui::Spacing();
		ImGui::TextDisabled("%s", items::status.empty() ? "Loading items …" : items::status.c_str());
		if (ImGui::Button("Load items")) items::Queue([] { items::LoadProducts(); });
		ImGui::End();
		return;
	}

	const float h = ImGui::GetContentRegionAvail().y;
	// Slots
	ImGui::BeginChild("slots", ImVec2(180, h), false);
	for (int i = 0; i < items::kSlotCount; i++) {
		const auto& s = items::kSlots[i];
		ImGui::PushID(s.index);
		bool sel = curSlot == s.index;
		ImVec2 p = ImGui::GetCursorScreenPos();
		if (ImGui::Selectable("##s", sel, 0, ImVec2(0, 28))) { curSlot = s.index; search[0] = 0; }
		ImDrawList* dl = ImGui::GetWindowDrawList();
		dl->AddText(ImVec2(p.x + 10, p.y + 6), ImGui::GetColorU32(sel ? ImGuiCol_Text : ImGuiCol_TextDisabled), s.label);
		if (items::overrides.count(s.index))
			dl->AddCircleFilled(ImVec2(p.x + ImGui::GetContentRegionAvail().x - 10, p.y + 14), 3.5f, ImGui::GetColorU32(accent));
		ImGui::PopID();
	}
	ImGui::EndChild();
	ImGui::SameLine();

	// Items des Slots
	ImGui::BeginChild("list", ImVec2(0, h), false);
	ImGui::SetNextItemWidth(-1);
	ImGui::InputTextWithHint("##q", "Search …", search, sizeof search);
	std::string q = Lower(search);
	std::vector<const items::Item*> shown;
	for (auto& it : items::list)
		if (it.slot == curSlot && (q.empty() || Lower(it.name).find(q) != std::string::npos)) shown.push_back(&it);
	int cur = items::overrides.count(curSlot) ? items::overrides[curSlot] : 0;
	int pick = -1;
	ImGui::BeginChild("rows");
	if (ImGui::Selectable("   Your own item", cur == 0, 0, ImVec2(0, 24))) pick = 0;
	ImGuiListClipper clip;
	clip.Begin((int)shown.size(), 26.0f);
	while (clip.Step()) {
		for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
			const items::Item* it = shown[r];
			ImGui::PushID(it->id);
			ImVec2 p = ImGui::GetCursorScreenPos();
			if (ImGui::Selectable("##i", cur == it->id, 0, ImVec2(0, 22))) pick = it->id;
			ImDrawList* dl = ImGui::GetWindowDrawList();
			dl->AddRectFilled(ImVec2(p.x + 4, p.y + 4), ImVec2(p.x + 7, p.y + 18), ImGui::GetColorU32(QualityColor(it->quality)), 2.0f);
			dl->AddText(ImVec2(p.x + 16, p.y + 3), ImGui::GetColorU32(ImGuiCol_Text), it->name.c_str());
			ImGui::PopID();
		}
	}
	ImGui::EndChild();
	ImGui::EndChild();
	ImGui::End();

	if (pick >= 0 && pick != cur) {
		int slot = curSlot;
		// Sperre wird in Set neu genommen
		items::mu.unlock();
		items::Set(slot, pick);
		items::mu.lock();
	}
}

} // namespace gui
