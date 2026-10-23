#pragma once
// Freeplay-Tweaks: Spielgeschwindigkeit und Ball (Tempo, Groesse, Schwerkraft, Abprall).
// Gelten nur im Freeplay; beim Verlassen ist alles wieder normal (neue Welt, neuer Ball).

namespace tweaks {

struct Setting {
	const char* key;    // fuer die Datei
	const char* label;
	const char* hint;
	float def, min, max, step;
	float value;
};
extern Setting list[];
extern const int count;

void Load();
void Save();
void Set(int i, float v);      // GUI-Thread
void ResetAll();               // GUI-Thread
bool Changed();                // irgendetwas nicht auf Standard?

// Spiel-Thread (PlayerTick): Werte auf Ball/Welt anwenden. car = dein Auto, ge = GameEvent (Freeplay)
void Tick(void* car, void* gameEvent, bool freeplay);
void OnEvent(void* caller, void* fn); // aus dem ProcessEvent-Hook (z. B. Auto beruehrt den Ball)

} // namespace tweaks
