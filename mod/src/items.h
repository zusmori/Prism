#pragma once
// Item-Tausch: eigene Items im Loadout des lokalen Spielers (nur fuer dich sichtbar)
#include <atomic>
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
void TickPreview(); // Auto im Hauptmenue/Garage nachziehen (Spiel-Thread)

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
extern bool loaded;
extern std::string status;
extern std::atomic<bool> inGame;      // lokaler Spieler mit Auto vorhanden (Freeplay/Match)
void OnMenuOpened();         // F8: Zustand neu pruefen
void Set(int slot, int id);  // id 0 = eigenes Item
void Save();
void LoadSaved();

} // namespace items
