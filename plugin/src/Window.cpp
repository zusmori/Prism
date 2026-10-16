// F8-Fenster im ZLauncher-Stil
#include "ZLauncherItems.h"

#include <algorithm>
#include <cctype>

#include "imgui.h"

namespace {

ImVec4 Rgb(int r, int g, int b, float a = 1.0f) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a); }

// Reihenfolge und Namen wie im Spiel
struct SlotInfo { int index; const char* label; };
const SlotInfo kSlotOrder[] = {
	{0, "Car"}, {1, "Decal"}, {7, "Paint Finish"}, {12, "Accent Finish"}, {2, "Wheels"}, {3, "Boost"},
	{5, "Topper"}, {4, "Antenna"}, {15, "Goal Explosion"}, {14, "Trail"}, {13, "Engine Audio"}, {18, "Anthem"},
};

ImVec4 QualityColor(int q) {
	switch (q) {
	case 1: return Rgb(110, 180, 255);   // Uncommon
	case 2: return Rgb(80, 140, 255);    // Rare
	case 3: return Rgb(160, 100, 255);   // Very Rare
	case 4: return Rgb(230, 70, 70);     // Import
	case 5: return Rgb(240, 200, 60);    // Exotic
	case 6: return Rgb(240, 90, 200);    // Black Market
	case 7: return Rgb(90, 220, 140);    // Premium
	case 8: return Rgb(250, 150, 50);    // Limited
	case 9: return Rgb(200, 200, 120);   // Legacy
	default: return Rgb(170, 170, 180);  // Common
	}
}

std::string Lower(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return s;
}

} // namespace

void ZLauncherItems::SetImGuiContext(uintptr_t ctx) { ImGui::SetCurrentContext(reinterpret_cast<ImGuiContext*>(ctx)); }

bool ZLauncherItems::ShouldBlockInput() { return ImGui::GetIO().WantCaptureMouse || ImGui::GetIO().WantCaptureKeyboard; }

void ZLauncherItems::Render() {
	const ImVec4 ac(accent_[0], accent_[1], accent_[2], 1.0f);
	auto acA = [&](float a) { return ImVec4(accent_[0], accent_[1], accent_[2], a); };

	int pushedCol = 0, pushedVar = 0;
	auto col = [&](ImGuiCol c, ImVec4 v) { ImGui::PushStyleColor(c, v); pushedCol++; };
	auto var = [&](ImGuiStyleVar s, float v) { ImGui::PushStyleVar(s, v); pushedVar++; };
	auto var2 = [&](ImGuiStyleVar s, ImVec2 v) { ImGui::PushStyleVar(s, v); pushedVar++; };

	col(ImGuiCol_WindowBg, Rgb(13, 13, 17, 0.97f));
	col(ImGuiCol_ChildBg, Rgb(20, 20, 26, 1.0f));
	col(ImGuiCol_TitleBg, Rgb(13, 13, 17));
	col(ImGuiCol_TitleBgActive, Rgb(17, 16, 24));
	col(ImGuiCol_Border, Rgb(255, 255, 255, 0.08f));
	col(ImGuiCol_FrameBg, Rgb(26, 26, 33));
	col(ImGuiCol_FrameBgHovered, Rgb(32, 32, 41));
	col(ImGuiCol_FrameBgActive, Rgb(36, 36, 46));
	col(ImGuiCol_Header, acA(0.28f));
	col(ImGuiCol_HeaderHovered, acA(0.18f));
	col(ImGuiCol_HeaderActive, acA(0.38f));
	col(ImGuiCol_Button, acA(0.85f));
	col(ImGuiCol_ButtonHovered, ac);
	col(ImGuiCol_ButtonActive, acA(0.7f));
	col(ImGuiCol_CheckMark, Rgb(255, 255, 255));
	col(ImGuiCol_Text, Rgb(236, 236, 242));
	col(ImGuiCol_TextDisabled, Rgb(130, 130, 145));
	col(ImGuiCol_ScrollbarBg, Rgb(0, 0, 0, 0));
	col(ImGuiCol_ScrollbarGrab, Rgb(60, 60, 72));
	col(ImGuiCol_SliderGrab, ac);
	var(ImGuiStyleVar_WindowRounding, 14.0f);
	var(ImGuiStyleVar_ChildRounding, 11.0f);
	var(ImGuiStyleVar_FrameRounding, 8.0f);
	var(ImGuiStyleVar_GrabRounding, 8.0f);
	var(ImGuiStyleVar_ScrollbarRounding, 8.0f);
	var2(ImGuiStyleVar_WindowPadding, ImVec2(16, 14));
	var2(ImGuiStyleVar_ItemSpacing, ImVec2(8, 6));
	var2(ImGuiStyleVar_FramePadding, ImVec2(10, 6));

	ImGui::SetNextWindowSize(ImVec2(820, 540), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSizeConstraints(ImVec2(620, 420), ImVec2(4000, 4000));
	bool keepOpen = true;
	if (!ImGui::Begin("ZLauncher Items##zlitems", &keepOpen, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		ImGui::PopStyleVar(pushedVar);
		ImGui::PopStyleColor(pushedCol);
		return;
	}

	std::unique_lock<std::mutex> lk(mu_);
	bool changed = false;

	// Kopfzeile
	ImGui::TextColored(ac, "ZLAUNCHER");
	ImGui::SameLine();
	ImGui::TextDisabled("Items");
	if (!presets_.empty()) {
		ImGui::SameLine(ImGui::GetWindowWidth() - 340);
		ImGui::PushItemWidth(190);
		int act = -1;
		for (size_t i = 0; i < presets_.size(); i++) {
			bool same = presets_[i].second.size() == loadout_.size();
			if (same) for (auto& [k, v] : presets_[i].second) { auto f = loadout_.find(k); if (f == loadout_.end() || f->second.id != v.id || f->second.paint != v.paint) { same = false; break; } }
			if (same) { act = (int)i; break; }
		}
		if (ImGui::BeginCombo("##preset", act >= 0 ? presets_[act].first.c_str() : "Presets")) {
			for (size_t i = 0; i < presets_.size(); i++)
				if (ImGui::Selectable(presets_[i].first.c_str(), (int)i == act)) { loadout_ = presets_[i].second; changed = true; }
			ImGui::EndCombo();
		}
		ImGui::PopItemWidth();
	}
	ImGui::SameLine(ImGui::GetWindowWidth() - 130);
	if (ImGui::Checkbox("Item swap", &enabled_)) changed = true;
	ImGui::Spacing();

	if (items_.empty()) {
		ImGui::TextDisabled("Loading the item list from Rocket League ...");
		ImGui::End();
		ImGui::PopStyleVar(pushedVar);
		ImGui::PopStyleColor(pushedCol);
		return;
	}

	const float footer = 40.0f;
	const float h = ImGui::GetContentRegionAvail().y - footer;

	// Links: Slots
	ImGui::BeginChild("slots", ImVec2(170, h), false);
	ImGui::Dummy(ImVec2(0, 2));
	for (auto& s : kSlotOrder) {
		bool any = std::any_of(items_.begin(), items_.end(), [&](const ZLItem& i) { return i.slot == s.index; });
		if (!any) continue;
		std::string cur = "Default";
		auto it = loadout_.find(s.index);
		if (it != loadout_.end()) {
			auto f = std::find_if(items_.begin(), items_.end(), [&](const ZLItem& i) { return i.id == it->second.id; });
			if (f != items_.end()) cur = f->name;
		}
		ImGui::PushID(s.index);
		ImVec2 p = ImGui::GetCursorScreenPos();
		bool sel = curSlot_ == s.index;
		if (ImGui::Selectable("##slot", sel, 0, ImVec2(0, 26))) { curSlot_ = s.index; search_[0] = 0; }
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", cur.c_str());
		ImDrawList* dl = ImGui::GetWindowDrawList();
		const float w = ImGui::GetWindowContentRegionWidth();
		dl->AddText(ImVec2(p.x + 10, p.y + 5), ImGui::GetColorU32(sel ? ImGuiCol_Text : ImGuiCol_TextDisabled), s.label);
		if (it != loadout_.end()) dl->AddCircleFilled(ImVec2(p.x + w - 10, p.y + 13), 3.0f, ImGui::GetColorU32(ac));
		ImGui::PopID();
	}
	ImGui::EndChild();
	ImGui::SameLine();

	// Rechts: Items des Slots
	ImGui::BeginChild("list", ImVec2(0, h), false);
	ImGui::PushItemWidth(-1);
	ImGui::InputTextWithHint("##search", "Search ...", search_, sizeof search_);
	ImGui::PopItemWidth();
	const std::string q = Lower(search_);
	const int body = loadout_.count(0) ? loadout_[0].id : 0;
	std::vector<const ZLItem*> shown;
	for (auto& i : items_) {
		if (i.slot != curSlot_) continue;
		if (!q.empty() && Lower(i.name).find(q) == std::string::npos) continue;
		if (curSlot_ == 1 && body && !i.bodies.empty() && std::find(i.bodies.begin(), i.bodies.end(), body) == i.bodies.end()) continue;
		shown.push_back(&i);
	}
	ImGui::BeginChild("rows", ImVec2(0, 0), false);
	{
		bool none = !loadout_.count(curSlot_);
		if (ImGui::Selectable("  Your own", none, 0, ImVec2(0, 26))) { loadout_.erase(curSlot_); changed = true; }
	}
	ImGuiListClipper clip((int)shown.size(), 28.0f);
	while (clip.Step()) {
		for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
			const ZLItem* i = shown[r];
			ImGui::PushID(i->id);
			ImVec2 p = ImGui::GetCursorScreenPos();
			bool sel = loadout_.count(curSlot_) && loadout_[curSlot_].id == i->id;
			if (ImGui::Selectable("##it", sel, 0, ImVec2(0, 24))) {
				int paint = loadout_.count(curSlot_) ? loadout_[curSlot_].paint : 0;
				loadout_[curSlot_] = {i->id, i->paintable ? paint : 0};
				changed = true;
			}
			ImDrawList* dl = ImGui::GetWindowDrawList();
			dl->AddRectFilled(ImVec2(p.x + 6, p.y + 8), ImVec2(p.x + 14, p.y + 16), ImGui::GetColorU32(QualityColor(i->quality)), 3.0f);
			dl->AddText(ImVec2(p.x + 22, p.y + 4), ImGui::GetColorU32(ImGuiCol_Text), i->name.c_str());
			ImGui::PopID();
		}
	}
	ImGui::EndChild();
	ImGui::EndChild();

	// Unten: Farbe des gewaehlten Items
	ImGui::Spacing();
	auto cur = loadout_.find(curSlot_);
	const ZLItem* ci = nullptr;
	if (cur != loadout_.end())
		for (auto& i : items_) if (i.id == cur->second.id) { ci = &i; break; }
	if (ci && ci->paintable) {
		ImGui::TextDisabled("Paint");
		ImGui::SameLine();
		auto chip = [&](int id, const char* name, ImVec4 c) {
			ImGui::PushID(id + 1000);
			bool on = cur->second.paint == id;
			ImGui::PushStyleColor(ImGuiCol_Button, c);
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, c);
			ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, on ? 2.0f : 0.0f);
			ImGui::PushStyleColor(ImGuiCol_Border, Rgb(255, 255, 255));
			if (ImGui::Button("##p", ImVec2(24, 24))) { cur->second.paint = id; changed = true; }
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", name);
			ImGui::PopStyleColor(3);
			ImGui::PopStyleVar();
			ImGui::PopID();
			ImGui::SameLine();
		};
		chip(0, "Unpainted", Rgb(60, 60, 70));
		for (auto& p : paints_) chip(p.id, p.name.c_str(), ImVec4(p.r, p.g, p.b, 1.0f));
		ImGui::NewLine();
	} else {
		ImGui::TextDisabled(ci ? "No paint for this item." : "Only you can see these items.");
	}

	ImGui::End();
	ImGui::PopStyleVar(pushedVar);
	ImGui::PopStyleColor(pushedCol);

	lk.unlock();
	if (changed) SaveLoadout();
	if (!keepOpen) _globalCvarManager->executeCommand("togglemenu " + GetMenuName());
}
