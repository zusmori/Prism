#pragma once

namespace gui {
void Fonts();          // Segoe UI laden
void Style();          // Farben im ZLauncher-Stil
void Render(bool* open);
constexpr float kToastSeconds = 6.5f;
void Toast(float t);   // Hinweis "Item Swapper aktiv" (t = Sekunden seit Start)
} // namespace gui
