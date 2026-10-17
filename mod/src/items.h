#pragma once
// Item-Tausch: eigene Items im Loadout des lokalen Spielers (nur fuer dich sichtbar)
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace items {

struct Item {
	int id = 0;
	std::string name;
	int slot = -1;
	int quality = 0;
	bool paintable = false;
	int forCar = 0; // z. B. Decal: gehoert zu diesem Auto (Produkt-ID), 0 = passt ueberall
	uint32_t paintMask = 0; // Bit n = Lackierung n gibt es fuer dieses Item
	uint32_t paintExclude = 0, paintShop = 0;
};

struct PaintInfo {
	int id;
	std::string name;
	float rgb[3]; // sRGB 0..1
	uint32_t flags = 0;
};

struct SlotInfo {
	int index;
	const char* label;
};
extern const SlotInfo kSlots[];
extern const int kSlotCount;

// Spiel-Thread: Aufgaben, die im ProcessEvent-Hook ausgefuehrt werden
void Queue(std::function<void()> fn);
void RunQueue();
void TickPreview();
bool InMatch(); // gerade im Freeplay/Match (eigenes Auto faehrt) // Auto im Hauptmenue/Garage nachziehen (Spiel-Thread)

// im Spiel-Thread
bool Setup();          // Klassen/Eigenschaften suchen
bool IsReady();
void LoadProducts();   // Item-Liste lesen
void ApplyNow();       // Overrides schreiben und Loadout neu laden
void OnProcessEvent(void* caller, void* fn, void* params); // vor dem Original aufgerufen

// GUI-Thread (mit Sperre)
extern std::mutex mu;
extern std::vector<Item> list;        // alle Items, nach Namen sortiert
extern std::map<int, int> overrides;  // Slot -> Produkt-ID
extern std::map<int, int> paints;     // Slot -> Farb-ID (nur bei lackierbaren Items)
extern std::vector<PaintInfo> paintList;
extern std::map<int, int> ownItems;   // Slot -> dein echtes Item (aus dem Hauptmenue)
void SetPaint(int slot, int paintId); // 0 = Standard
uint32_t PaintMaskOf(int productId);
// Eigene Lackfarben (frei waehlbar), sRGB 0..1
extern bool colorsOn;
extern float colorPrimary[3], colorAccent[3];
void SetColors(bool on, const float* primary, const float* accent, bool save = true);  // welche Lackierungen es fuer ein Item gibt
extern bool loaded;
extern std::string status;
extern std::atomic<bool> inGame;      // lokaler Spieler mit Auto vorhanden (Freeplay/Match)
void OnMenuOpened();         // F8: Zustand neu pruefen
void Set(int slot, int id);  // id 0 = eigenes Item

// Presets: gespeicherte Item-Kombinationen
struct Preset {
	std::string name;
	std::map<int, int> items;  // Slot -> Produkt-ID
	std::map<int, int> paints; // Slot -> Farb-ID
};
extern std::vector<Preset> presets; // mit mu sperren
void LoadPresets();
void SavePresets();
void AddPreset(const std::string& name); // aktuelle Items speichern
void DeletePreset(int i);
void ApplyPreset(int i);
void Save();
void LoadSaved();

} // namespace items
