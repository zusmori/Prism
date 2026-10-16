// ZLauncher Items - BakkesMod-Plugin
// Zeigt mit F8 ein Fenster zum Tauschen von Items (nur fuer dich sichtbar, nur ohne Anti-Cheat).
// Das Loadout liegt in %APPDATA%\RLMapSwap\items.json und wird mit ZLauncher geteilt.
#include "ZLauncherItems.h"

#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

#include "bakkesmod/wrappers/items/ItemsWrapper.h"
#include "bakkesmod/wrappers/items/attributes/ProductAttribute_BodyCompatibilityWrapper.h"
#include "bakkesmod/wrappers/items/dbs/PaintDatabaseWrapper.h"
#include "bakkesmod/wrappers/items/ProductPaintWrapper.h"

using json = nlohmann::json;

BAKKESMOD_PLUGIN(ZLauncherItems, "ZLauncher Items", "1.0.0", PLUGINTYPE_FREEPLAY)

std::shared_ptr<CVarManagerWrapper> _globalCvarManager;

// Slots, die der Item Mod von BakkesMod unterstuetzt
static const std::set<int> kSlots = {0, 1, 2, 3, 4, 5, 7, 12, 13, 14, 15, 18};

static std::string ReadFile(const fs::path& p) {
	std::ifstream f(p, std::ios::binary);
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

static void WriteFileAtomic(const fs::path& p, const std::string& s) {
	fs::path tmp = p;
	tmp += ".tmp";
	{
		std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
		f << s;
	}
	std::error_code ec;
	fs::rename(tmp, p, ec);
	if (ec) {
		fs::remove(p, ec);
		fs::rename(tmp, p, ec);
	}
}

void ZLauncherItems::onLoad() {
	_globalCvarManager = cvarManager;
	const char* app = std::getenv("APPDATA");
	dir_ = fs::path(app ? app : ".") / "RLMapSwap";
	std::error_code ec;
	fs::create_directories(dir_, ec);
	loadoutFile_ = dir_ / "items.json";
	dbFile_ = dir_ / "items_db.json";

	ReadTheme();
	ReadLoadout(true);

	// F8 oeffnet das Fenster
	cvarManager->setBind("F8", "togglemenu " + GetMenuName());
	cvarManager->registerNotifier("zl_items_reload", [this](std::vector<std::string>) { ReadLoadout(true); }, "Reload ZLauncher items", PERMISSION_ALL);

	// Item-Liste exportieren, sobald das Spiel bereit ist; danach regelmaessig nach Aenderungen schauen
	gameWrapper->SetTimeout([this](GameWrapper*) { DumpDatabase(); Apply(); }, 2.0f);
	gameWrapper->SetTimeout([this](GameWrapper*) { Poll(); }, 1.0f);
	cvarManager->log("ZLauncher Items loaded. Press F8.");
}

void ZLauncherItems::onUnload() {}

void ZLauncherItems::Poll() {
	ReadLoadout(false);
	if (!dbDone_) DumpDatabase();
	gameWrapper->SetTimeout([this](GameWrapper*) { Poll(); }, 1.0f);
}

void ZLauncherItems::ReadTheme() {
	try {
		auto j = json::parse(ReadFile(dir_ / "settings.json"), nullptr, false);
		if (j.is_discarded() || !j.contains("theme")) return;
		std::string t = j["theme"].get<std::string>();
		if (t == "blue") { accent_[0] = 0.231f; accent_[1] = 0.510f; accent_[2] = 0.965f; }
		else if (t == "cyan") { accent_[0] = 0.024f; accent_[1] = 0.714f; accent_[2] = 0.831f; }
		else if (t == "green") { accent_[0] = 0.063f; accent_[1] = 0.725f; accent_[2] = 0.506f; }
		else { accent_[0] = 0.545f; accent_[1] = 0.361f; accent_[2] = 0.965f; }
	} catch (...) {}
}

// Alle Items aus dem Spiel auslesen und fuer ZLauncher speichern
void ZLauncherItems::DumpDatabase() {
	ItemsWrapper iw = gameWrapper->GetItemsWrapper();
	if (!iw) return;
	auto all = iw.GetAllProducts();
	if (all.Count() < 100) return; // Spiel noch nicht fertig geladen

	std::vector<ZLItem> items;
	std::map<int, std::string> slotNames;
	for (int i = 0; i < all.Count(); i++) {
		ProductWrapper p = all.Get(i);
		if (!p) continue;
		try {
			if (p.IsContainer() || p.IsBlueprint() || p.IsCurrency() || p.IsContainerKey() || p.IsSchematic()) continue;
			auto slot = p.GetSlot();
			if (!slot) continue;
			int si = slot.GetSlotIndex();
			if (!kSlots.count(si)) continue;
			ZLItem it;
			it.id = p.GetID();
			if (it.id <= 0 || it.id > 65535) continue;
			auto lbl = p.GetLongLabel();
			it.name = lbl.IsNull() ? "" : lbl.ToString();
			if (it.name.empty()) { auto l2 = p.GetLabel(); it.name = l2.IsNull() ? "" : l2.ToString(); }
			if (it.name.empty()) continue;
			it.slot = si;
			it.paintable = p.IsPaintable();
			it.quality = p.GetQuality();
			if (!slotNames.count(si)) { auto sl = slot.GetLabel(); slotNames[si] = sl.IsNull() ? "" : sl.ToString(); }
			if (si == 1) { // Decals passen nur zu bestimmten Autos
				auto attrs = p.GetAttributes();
				for (int a = 0; a < attrs.Count(); a++) {
					auto at = attrs.Get(a);
					if (!at || at.GetAttributeType() != "ProductAttribute_BodyCompatibility_TA") continue;
					ProductAttribute_BodyCompatibilityWrapper bc(at.memory_address);
					auto bodies = bc.GetCompatibleBodies();
					for (int b = 0; b < bodies.Count(); b++) { auto bp = bodies.Get(b); if (bp) it.bodies.push_back(bp.GetID()); }
				}
			}
			items.push_back(std::move(it));
		} catch (...) {}
	}
	if (items.empty()) return;

	std::vector<ZLPaint> paints;
	try {
		auto pdb = iw.GetPaintDB();
		if (pdb) {
			auto ps = pdb.GetPaints();
			for (int i = 0; i < ps.Count(); i++) {
				auto pp = ps.Get(i);
				if (!pp || !pp.GetbVisible()) continue;
				ZLPaint z;
				z.id = pp.GetId();
				z.name = pp.GetLabel();
				auto cols = pp.GetColors();
				if (!cols.empty()) { z.r = cols[0].R; z.g = cols[0].G; z.b = cols[0].B; }
				if (z.id > 0 && z.id < 64 && !z.name.empty()) paints.push_back(z);
			}
		}
	} catch (...) {}

	std::sort(items.begin(), items.end(), [](const ZLItem& a, const ZLItem& b) { return a.name < b.name; });

	json j;
	j["version"] = 1;
	j["items"] = json::array();
	for (auto& it : items) {
		json o = {{"id", it.id}, {"name", it.name}, {"slot", it.slot}, {"paint", it.paintable}, {"q", it.quality}};
		if (!it.bodies.empty()) o["bodies"] = it.bodies;
		j["items"].push_back(o);
	}
	j["paints"] = json::array();
	for (auto& p : paints) {
		auto c = [](float v) { return (int)(std::min(1.0f, std::max(0.0f, v)) * 255.0f + 0.5f); };
		char hex[8];
		snprintf(hex, sizeof hex, "#%02x%02x%02x", c(p.r), c(p.g), c(p.b));
		j["paints"].push_back({{"id", p.id}, {"name", p.name}, {"color", hex}});
	}
	j["slots"] = json::array();
	std::vector<ZLSlot> slots;
	for (auto& [i, n] : slotNames) { j["slots"].push_back({{"index", i}, {"label", n}}); slots.push_back({i, n}); }
	WriteFileAtomic(dbFile_, j.dump());

	std::lock_guard<std::mutex> lk(mu_);
	items_ = std::move(items);
	paints_ = std::move(paints);
	slots_ = std::move(slots);
	dbDone_ = true;
	cvarManager->log("ZLauncher Items: exported " + std::to_string(items_.size()) + " items");
}

// items.json lesen (von ZLauncher oder dem F8-Fenster geschrieben)
void ZLauncherItems::ReadLoadout(bool force) {
	std::error_code ec;
	auto t = fs::last_write_time(loadoutFile_, ec);
	if (ec) { if (force) Apply(); return; }
	if (!force && t == loadoutTime_) return;
	loadoutTime_ = t;
	auto j = json::parse(ReadFile(loadoutFile_), nullptr, false);
	if (j.is_discarded()) return;
	{
		std::lock_guard<std::mutex> lk(mu_);
		enabled_ = j.value("enabled", true);
		loadout_.clear();
		if (j.contains("items") && j["items"].is_object()) {
			for (auto& [k, v] : j["items"].items()) {
				int slot = std::atoi(k.c_str());
				if (!kSlots.count(slot) || !v.is_object()) continue;
				zl::LoadoutItem li;
				li.id = v.value("id", 0);
				li.paint = v.value("paint", 0);
				if (li.id > 0) loadout_[slot] = li;
			}
		}
	}
	ReadTheme();
	Apply();
}

void ZLauncherItems::SaveLoadout() {
	json j;
	{
		std::lock_guard<std::mutex> lk(mu_);
		j["enabled"] = enabled_;
		j["items"] = json::object();
		for (auto& [slot, it] : loadout_) j["items"][std::to_string(slot)] = {{"id", it.id}, {"paint", it.paint}};
	}
	j["source"] = "game";
	WriteFileAtomic(loadoutFile_, j.dump(2));
	std::error_code ec;
	loadoutTime_ = fs::last_write_time(loadoutFile_, ec);
	Apply();
}

// Code an den Item Mod von BakkesMod geben (laeuft im Spiel-Thread)
void ZLauncherItems::Apply() {
	std::string code;
	bool on;
	{
		std::lock_guard<std::mutex> lk(mu_);
		on = enabled_ && !loadout_.empty();
		if (on) code = zl::MakeCode(loadout_);
	}
	gameWrapper->Execute([this, on, code](GameWrapper*) {
		if (!on) {
			cvarManager->executeCommand("cl_itemmod_enabled 0", false);
			lastCode_.clear();
			return;
		}
		cvarManager->executeCommand("cl_itemmod_enabled 1", false);
		if (code != lastCode_) {
			cvarManager->executeCommand("cl_itemmod_code " + code, false);
			lastCode_ = code;
		}
	});
}
