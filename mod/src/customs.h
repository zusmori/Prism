#pragma once
// Eigene Decals und Baelle (Format wie AlphaConsole: Ordner/zip mit _TexturesPackage.json + PNG-Bildern).
// Liegen in %APPDATA%\RLMapSwap\Customs. Nur fuer dich sichtbar.
#include <mutex>
#include <string>
#include <vector>

namespace customs {

struct Custom {
	std::string id;      // eindeutig: Ordner/Name
	std::string name;    // Anzeigename (Schluessel in der json)
	bool ball = false;   // Ball statt Decal
	int bodyId = 0;      // fuer welches Auto (Produkt-ID), 0 = egal
	int skinId = 0;
	std::wstring thumb;  // Bild fuer die Liste
};

extern std::mutex mu;
extern std::vector<Custom> list; // mit mu sperren
extern std::string selDecal, selBall; // gewaehlt ("" = keins)
extern std::string status;            // z. B. "Unpacking ..."

void Init();                                   // gespeicherte Auswahl laden, Ordner beobachten
void Select(bool ball, const std::string& id); // "" = aus
void OpenFolder();
unsigned long long Thumb(const std::string& id); // Render-Thread: Vorschaubild (ImTextureID) oder 0

// Spiel-Thread: auf diese Auto-Modelle (Menue/Spiel) und Baelle anwenden; body = aktuelles Auto (Produkt-ID)
void Tick(const std::vector<void*>& carMeshes, int body, const std::vector<void*>& balls);

// Spiel-Thread, aus dem ProcessEvent-Hook: ersetztes Ball-Material sofort wieder auf unseres setzen
void Fast();

} // namespace customs
