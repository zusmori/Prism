// Feine Schritte fuer die Kamera-Regler im Rocket-League-Menue.
// Camera_TA hat pro Einstellung eine Grenze (FOVLimits, HeightLimits, ...) aus drei Zahlen: Minimum, Maximum
// und Schrittweite. Die Regler im Menue lesen diese Werte. Hier wird nur die Schrittweite verkleinert
// (Minimum und Maximum bleiben wie im Spiel) - an der Vorlage (Archetyp/Standardobjekt) und an jeder Kamera.
#include "camera.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "log.h"
#include "ue.h"

namespace cam {

struct Limit {
	const char* prop;
	float fine;   // gewuenschte Schrittweite
	int off = -1; // Lage im Kamera-Objekt
};
static Limit limits[] = {
	{"FOVLimits", 1.0f},      {"HeightLimits", 1.0f},    {"AngleLimits", 0.1f},           {"DistanceLimits", 1.0f},
	{"StiffnessLimits", 0.01f}, {"SwivelSpeedLimits", 0.01f}, {"TransitionSpeedLimits", 0.01f},
};
static void* clsCam = nullptr;
static int fMin = -1, fMax = -1, fStep = -1; // Lage der drei Zahlen in der Grenze (Bytes)
static bool ready = false, tried = false;

static std::string Lower(std::string s) {
	for (auto& c : s) c = (char)tolower((unsigned char)c);
	return s;
}

// Aufbau der Grenze: Namen der drei Zahlen aus der Struktur, sonst aus den Werten schliessen
static void FindLayout(void* sample) {
	for (int i = 0, n = ue::ObjectCount(); i < n && fStep < 0; i++) {
		void* o = ue::ObjectAt(i);
		if (!o || ue::OuterOf(o) != clsCam || ue::Name(ue::ClassOf(o)) != "ScriptStruct") continue;
		int mn = -1, mx = -1, st = -1, floats = 0;
		std::string desc;
		for (void* f = ue::Read<void*>(o, ue::off::StructChildren); f; f = ue::Read<void*>(f, ue::off::FieldNext)) {
			if (ue::Name(ue::ClassOf(f)) != "FloatProperty") continue;
			floats++;
			std::string nm = Lower(ue::Name(f));
			int off = ue::Read<int32_t>(f, ue::off::PropOffset);
			desc += ue::Name(f) + "@" + std::to_string(off) + " ";
			if (nm.find("min") != std::string::npos) mn = off;
			else if (nm.find("max") != std::string::npos) mx = off;
			else st = off;
		}
		zl::Log("cam: struct %s: %s", ue::FullName(o).c_str(), desc.c_str());
		if (floats == 3 && mn >= 0 && mx >= 0 && st >= 0) { fMin = mn; fMax = mx; fStep = st; }
	}
	if (fStep >= 0 || !sample) return;
	// Notfall: an der Hoehe erkennen (z. B. 40 / 200 / 10): Schritt = kleinster positiver Wert, der den Bereich teilt
	const float* v = reinterpret_cast<const float*>(reinterpret_cast<uint8_t*>(sample) + limits[1].off);
	for (int s = 0; s < 3; s++) {
		int a = (s + 1) % 3, b = (s + 2) % 3;
		float lo = std::min(v[a], v[b]), hi = std::max(v[a], v[b]), st = v[s];
		if (st <= 0 || hi - lo < st * 2) continue;
		float k = (hi - lo) / st;
		if (fabsf(k - roundf(k)) < 0.01f) { fStep = s * 4; fMin = (v[a] < v[b] ? a : b) * 4; fMax = (v[a] < v[b] ? b : a) * 4; break; }
	}
	zl::Log("cam: layout from values (%.2f %.2f %.2f) -> min@%d max@%d step@%d", v[0], v[1], v[2], fMin, fMax, fStep);
}

static bool Setup() {
	if (tried) return ready;
	tried = true;
	clsCam = ue::FindClass("Class TAGame.Camera_TA");
	if (!clsCam) { zl::Log("cam: Camera_TA not found"); return false; }
	int found = 0;
	for (auto& l : limits) { l.off = ue::PropOffset(clsCam, l.prop); found += l.off >= 0; }
	void* cdo = ue::FindObject("Camera_TA TAGame.Default__Camera_TA");
	if (limits[1].off >= 0) FindLayout(cdo);
	ready = found > 0 && fStep >= 0;
	if (cdo) {
		std::string vals;
		for (auto& l : limits) {
			if (l.off < 0) continue;
			const float* f = reinterpret_cast<const float*>(reinterpret_cast<uint8_t*>(cdo) + l.off);
			char b[96];
			snprintf(b, sizeof b, "%s=(%.2f %.2f %.2f) ", l.prop, f[0], f[1], f[2]);
			vals += b;
		}
		zl::Log("cam: limits %d/7 step@%d default %s", found, fStep, vals.c_str());
	}
	return ready;
}

static int Patch(void* cam) {
	int n = 0;
	for (auto& l : limits) {
		if (l.off < 0) continue;
		float* st = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(cam) + l.off + fStep);
		if (*st > l.fine + 0.00001f) { *st = l.fine; n++; }
	}
	return n;
}

void Tick() {
	unsigned long long now = GetTickCount64();
	// alle Kameras und ihre Vorlagen (auch Unterklassen): ab und zu neu suchen, verteilt auf mehrere Bilder (kein Ruckler)
	static ue::SlicedScan scan;
	static std::vector<void*> known;
	static unsigned long long rescan = 0;
	if (clsCam && !scan.running && now >= rescan) { rescan = now + 5000; scan.Start(clsCam); }
	if (scan.running && scan.Step()) known = scan.result;
	static unsigned long long next = 0;
	if (now < next) return;
	next = now + 1000;
	if (!Setup()) return;
	int changed = 0, objs = 0;
	if (void* cdo = ue::FindObject("Camera_TA TAGame.Default__Camera_TA")) { changed += Patch(cdo); objs++; }
	for (void* c : known)
		if (ue::IsValid(c)) { changed += Patch(c); objs++; }
	static int logs = 0;
	if (changed && logs++ < 10) zl::Log("cam: finer steps on %d value(s) (%d camera objects)", changed, objs);
}

} // namespace cam
