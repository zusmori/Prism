#pragma once
// Kamera-Einstellungen in Rocket League (Einstellungen -> Kamera) in feinen Schritten:
// z. B. Hoehe 201 statt nur 200/210. Wie beim Kirkode-Plugin: an den Grenzen der Kamera-Klasse (Camera_TA).
namespace cam {
void Tick(); // Spiel-Thread, oft aufrufen (prueft selbst nur jede Sekunde)
}
