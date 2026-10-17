#pragma once

namespace gui {
void Fonts();          // Segoe UI laden
void Style();          // Farben im ZLauncher-Stil
void Render(bool* open);
void SetOpen(bool open); // F8: oeffnen/schliessen (animiert)
bool Visible();
bool BlurField();       // Esc: erst das Eingabefeld verlassen (true), sonst Menue schliessen         // noch sichtbar (auch waehrend der Schliess-Animation)
constexpr float kToastSeconds = 6.5f;
void Toast(float t);   // Hinweis "Item Swapper aktiv" (t = Sekunden seit Start)
} // namespace gui
