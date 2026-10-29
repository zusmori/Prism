#pragma once
// Kulissen-Merge im F8-Menue. Die eigentliche Arbeit (Map-Dateien entschluesseln, Unter-Level eintragen,
// neu packen) macht der Prism-Launcher; hier werden nur Wuensche geschrieben und der Stand gelesen:
//   merge_catalog.txt  (Prism)  Datei \t Name \t hat Kulisse \t kann Arena sein
//   merge_requests.txt (hier)   Datei \t Kulissen-Datei \t eigene Kulisse entfernen
//   merge_status.txt   (Prism)  Datei \t Kulissen-Datei \t ersetzen \t Stand \t Text
#include <mutex>
#include <string>
#include <vector>

namespace merge {

struct Arena {
	std::string file, name;
	bool backdrop = false, base = false, own = false; // own: eigene Kulisse laesst sich abschalten
	std::string want, wantName;   // gewuenschte Kulisse (Datei/Name), "" = Original, "-" = keine fremde Kulisse
	bool replace = false;
	std::string state, stateMsg;  // applied | pending | error | "" (noch nicht bearbeitet)
	std::string lower;
};

extern std::mutex mu;
extern std::vector<Arena> list; // mit mu sperren
extern bool loaded;             // Katalog gefunden
extern unsigned long long launcherSeen; // letzte Aenderung von Prism (ms), 0 = nie

void Refresh();                                                   // Dateien neu lesen (hoechstens alle 700 ms)
void Request(const std::string& file, const std::string& backdrop, bool replace); // backdrop "" = zuruecksetzen
void RestoreAll();

} // namespace merge
