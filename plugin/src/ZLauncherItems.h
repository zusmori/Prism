#pragma once
#include "bakkesmod/plugin/bakkesmodplugin.h"
#include "bakkesmod/plugin/pluginwindow.h"
#include "Loadout.h"

#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace fs = std::filesystem;

extern std::shared_ptr<CVarManagerWrapper> _globalCvarManager;

struct ZLItem {
	int id = 0;
	std::string name;
	int slot = 0;
	bool paintable = false;
	int quality = 0;
	std::vector<int> bodies; // nur bei Decals: passende Autos
};

struct ZLPaint {
	int id = 0;
	std::string name;
	float r = 1, g = 1, b = 1;
};

struct ZLSlot {
	int index = 0;
	std::string label;
};

class ZLauncherItems : public BakkesMod::Plugin::BakkesModPlugin, public BakkesMod::Plugin::PluginWindow {
public:
	void onLoad() override;
	void onUnload() override;

	// F8-Fenster
	void Render() override;
	std::string GetMenuName() override { return "zlauncheritems"; }
	std::string GetMenuTitle() override { return "ZLauncher Items"; }
	void SetImGuiContext(uintptr_t ctx) override;
	bool ShouldBlockInput() override;
	bool IsActiveOverlay() override { return true; }
	void OnOpen() override { open_ = true; }
	void OnClose() override { open_ = false; }

private:
	// Daten
	fs::path dir_;           // %APPDATA%\RLMapSwap
	fs::path loadoutFile_;   // items.json (Loadout, gemeinsam mit ZLauncher)
	fs::path dbFile_;        // items_db.json (Item-Liste fuer ZLauncher)
	std::mutex mu_;
	std::vector<ZLItem> items_;
	std::vector<ZLPaint> paints_;
	std::vector<ZLSlot> slots_;
	std::map<int, zl::LoadoutItem> loadout_;
	bool enabled_ = true;
	fs::file_time_type loadoutTime_{};
	fs::file_time_type settingsTime_{};
	std::string lastCode_;
	bool open_ = false;
	bool dbDone_ = false;
	float accent_[3] = {0.545f, 0.361f, 0.965f};
	std::vector<std::pair<std::string, std::map<int, zl::LoadoutItem>>> presets_; // aus ZLauncher (settings.json)

	void DumpDatabase();
	void ReadLoadout(bool force);
	void SaveLoadout();
	void Apply();
	void Poll();
	void ReadTheme();

	// Fenster-Zustand
	int curSlot_ = 0;
	char search_[64] = {0};
	friend struct ZLWindow;
};
