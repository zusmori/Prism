// Freeplay-Tweaks. Der Ball hat im Spiel dieselben Regler wie die Mutatoren privater Spiele
// (Ball_TA.SetBallMaxLinearSpeedScale usw.), die Spielgeschwindigkeit ist WorldInfo.TimeDilation.
// Wird nur im Freeplay angewendet und alle 250 ms geprueft (neuer Ball nach Reset bekommt die Werte sofort).
#include "tweaks.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "log.h"
#include "ue.h"

namespace tweaks {

Setting list[] = {
	{"speed", "Game speed", "Slow motion or faster than normal.", 1.0f, 0.1f, 2.0f, 0.05f, 1.0f},
	{"ballspeed", "Ball speed", "How fast the ball flies when you hit it.", 1.0f, 0.25f, 4.0f, 0.05f, 1.0f},
	{"ballsize", "Ball size", "Tiny or huge ball.", 1.0f, 0.3f, 4.0f, 0.05f, 1.0f},
	{"ballgravity", "Ball gravity", "0 = the ball floats.", 1.0f, 0.0f, 3.0f, 0.05f, 1.0f},
	{"bounce", "Ball bounce", "How much the ball bounces off walls and floor.", 1.0f, 0.0f, 3.0f, 0.05f, 1.0f},
};
const int count = sizeof(list) / sizeof(list[0]);

static std::mutex mu;
static std::atomic<int> gen{1};

static std::wstring File() { return zl::DataDir() + L"\\zltweaks.txt"; }

void Load() {
	FILE* f = _wfopen(File().c_str(), L"r");
	if (!f) return;
	char k[64];
	float v;
	std::lock_guard<std::mutex> lk(mu);
	while (fscanf(f, "%63s %f", k, &v) == 2)
		for (int i = 0; i < count; i++)
			if (!strcmp(k, list[i].key)) list[i].value = std::clamp(v, list[i].min, list[i].max);
	fclose(f);
}

void Save() {
	FILE* f = _wfopen(File().c_str(), L"w");
	if (!f) return;
	std::lock_guard<std::mutex> lk(mu);
	for (int i = 0; i < count; i++) fprintf(f, "%s %.3f\n", list[i].key, list[i].value);
	fclose(f);
}

void Set(int i, float v) {
	if (i < 0 || i >= count) return;
	{
		std::lock_guard<std::mutex> lk(mu);
		v = std::clamp(roundf(v / list[i].step) * list[i].step, list[i].min, list[i].max);
		if (fabsf(v - list[i].value) < 0.0001f) return;
		list[i].value = v;
	}
	gen++;
}

void ResetAll() {
	{
		std::lock_guard<std::mutex> lk(mu);
		for (int i = 0; i < count; i++) list[i].value = list[i].def;
	}
	gen++;
	Save();
}

bool Changed() {
	std::lock_guard<std::mutex> lk(mu);
	for (int i = 0; i < count; i++)
		if (fabsf(list[i].value - list[i].def) > 0.001f) return true;
	return false;
}

// ---------- Spiel-Thread ----------
struct BallFn {
	const char* fn;   // Setter am Ball
	const char* prop; // aktueller Wert (zum Pruefen)
	int setting;
	void* f = nullptr;
	int param = -1, offProp = -1;
};
static BallFn ballFns[] = {
	{"SetBallMaxLinearSpeedScale", "ReplicatedBallMaxLinearSpeedScale", 1},
	{"SetBallScale", "ReplicatedBallScale", 2},
	{"SetBallGravityScale", "ReplicatedBallGravityScale", 3},
	{"SetWorldBounceScale", "ReplicatedWorldBounceScale", 4},
};
// Ballgeschwindigkeit wirklich spuerbar: wie stark ein Auto den Ball wegschiesst (wie der Boomer-Mutator).
// Der Standardwert des Balls wird gemerkt und mit dem Regler multipliziert.
static void *fnCarBounce = nullptr;
static int offCarBounceArg = -1, offCarBounce = -1;
static std::map<void*, float> carBounceBase;
static void *clsBall = nullptr, *clsActor = nullptr;
static int offWorldInfo = -1, offDilation = -1, offGameBalls = -1;
// Ballgeschwindigkeit: die Hoechstgeschwindigkeit allein merkt man kaum (sie wird fast nie erreicht).
// Deshalb wird der Ball direkt nach jeder Beruehrung durch ein Auto entsprechend schneller (bzw. langsamer).
static void *fnTouch = nullptr, *fnSetVel = nullptr;
static int offVelocity = -1, offSetVel = -1;
static std::atomic<bool> fpActive{false};
static std::mutex touchMu;
static std::vector<std::pair<void*, unsigned long long>> touched; // Ball, Zeitpunkt der Beruehrung
static bool setupDone = false;

static void SetupOnce(void* ge) {
	if (setupDone) return;
	setupDone = true;
	clsBall = ue::FindClass("Class TAGame.Ball_TA");
	clsActor = ue::FindClass("Class Engine.Actor");
	if (clsActor) offWorldInfo = ue::PropOffset(clsActor, "WorldInfo");
	if (void* wi = ue::FindClass("Class Engine.WorldInfo")) offDilation = ue::PropOffset(wi, "TimeDilation");
	if (ge) offGameBalls = ue::PropOffset(ue::ClassOf(ge), "GameBalls");
	std::string have;
	for (auto& b : ballFns) {
		if (!clsBall) break;
		b.f = ue::FindFunction(clsBall, b.fn);
		std::string type;
		if (b.f) b.param = ue::FirstParamOffset(b.f, &type);
		if (b.f && type != "FloatProperty") b.f = nullptr;
		b.offProp = ue::PropOffset(clsBall, b.prop);
		have += std::string(b.fn) + (b.f ? "=yes " : "=no ");
	}
	if (clsBall) {
		for (const char* n : {"OnCarTouch", "EventCarTouch", "OnHitByCar", "CarTouch"})
			if (!fnTouch) fnTouch = ue::FindFunction(clsBall, n);
		for (const char* n : {"SetPhysicsVelocity", "SetVelocity"})
			if (!fnSetVel) fnSetVel = ue::FindFunction(clsBall, n);
		std::string type;
		if (fnSetVel) offSetVel = ue::FirstParamOffset(fnSetVel, &type);
		if (fnSetVel && type != "StructProperty") { fnSetVel = nullptr; offSetVel = -1; }
	}
	if (clsActor) offVelocity = ue::PropOffset(clsActor, "Velocity");
	if (clsBall) {
		fnCarBounce = ue::FindFunction(clsBall, "SetCarBounceScale");
		std::string type;
		if (fnCarBounce) offCarBounceArg = ue::FirstParamOffset(fnCarBounce, &type);
		if (fnCarBounce && type != "FloatProperty") fnCarBounce = nullptr;
		for (const char* n : {"ReplicatedAddedCarBounceScale", "ReplicatedCarBounceScale", "AddedCarBounceScale", "CarBounceScale"})
			if (offCarBounce < 0) offCarBounce = ue::PropOffset(clsBall, n);
		// alle Funktionen des Balls rund um Tempo/Physik ins Log (zum Nachbessern)
		std::string fns;
		for (void* st = clsBall; st && fns.size() < 1800; st = ue::Read<void*>(st, ue::off::StructSuper))
			for (void* f = ue::Read<void*>(st, ue::off::StructChildren); f && fns.size() < 1800; f = ue::Read<void*>(f, ue::off::FieldNext)) {
				if (ue::Name(ue::ClassOf(f)) != "Function") continue;
				std::string n = ue::Name(f), lo = n;
				for (auto& c : lo) c = (char)tolower((unsigned char)c);
				for (const char* k : {"veloc", "impulse", "force", "rbstate", "physic", "touch", "hit", "speed", "scale", "kick"})
					if (lo.find(k) != std::string::npos) { fns += n + " "; break; }
			}
		zl::Log("tweaks: carBounce=%s(arg %d, prop %x) ball functions: %s", fnCarBounce ? "yes" : "no", offCarBounceArg, offCarBounce, fns.c_str());
	}
	zl::Log("tweaks: ball=%p worldInfo=%x dilation=%x gameBalls=%x %s touch=%s setVel=%s(%d) velocity=%x", clsBall, offWorldInfo, offDilation, offGameBalls,
		have.c_str(), fnTouch ? ue::Name(fnTouch).c_str() : "-", fnSetVel ? ue::Name(fnSetVel).c_str() : "-", offSetVel, offVelocity);
}

static std::vector<void*> Balls(void* ge) {
	std::vector<void*> out;
	if (ge && offGameBalls >= 0) {
		auto* arr = ue::Ptr<ue::TArray<void*>>(ge, offGameBalls);
		for (int i = 0; arr->data && i < arr->count && i < 16; i++)
			if (arr->data[i] && ue::IsAlive(arr->data[i])) out.push_back(arr->data[i]);
		if (!out.empty()) return out;
	}
	// Notfall: alle Baelle suchen (selten, nur wenn das GameEvent keine Liste hat)
	static unsigned long long nextScan = 0;
	static std::vector<void*> cached;
	unsigned long long now = GetTickCount64();
	if (now >= nextScan) {
		nextScan = now + 2000;
		cached.clear();
		if (clsBall)
			for (void* b : ue::Instances(clsBall))
				if (ue::IsAlive(b)) cached.push_back(b);
	}
	return cached;
}

static std::map<void*, int> ballGen;    // Ball -> Stand der Werte, die schon drauf sind
static void* lastWorld = nullptr;
static float worldOrig = 1.0f;
static bool worldTouched = false;

// Spiel-Thread: Ereignis vor dem Original (aus dem ProcessEvent-Hook)
void OnEvent(void* caller, void* fn) {
	if (!fnTouch || fn != fnTouch || !fpActive.load()) return;
	std::lock_guard<std::mutex> lk(touchMu);
	for (auto& t : touched) if (t.first == caller) return; // schon vorgemerkt
	touched.push_back({caller, GetTickCount64()});
}

static void ApplyTouches(float scale) {
	std::vector<std::pair<void*, unsigned long long>> todo;
	{
		std::lock_guard<std::mutex> lk(touchMu);
		todo.swap(touched);
	}
	if (fabsf(scale - 1.0f) < 0.001f || offVelocity < 0) return;
	static std::map<void*, unsigned long long> last;
	unsigned long long now = GetTickCount64();
	for (auto& [b, at] : todo) {
		if (!b || !ue::IsAlive(b)) continue;
		auto l = last.find(b);
		if (l != last.end() && now - l->second < 120) continue; // eine Beruehrung = ein Schub (Dribbeln nicht aufschaukeln)
		last[b] = now;
		float v[3];
		memcpy(v, reinterpret_cast<uint8_t*>(b) + offVelocity, 12);
		for (float& c : v) c *= scale;
		if (fnSetVel && offSetVel >= 0) {
			std::vector<uint8_t> p(size_t(ue::StructSize(fnSetVel)) + 16, 0);
			memcpy(p.data() + offSetVel, v, 12);
			ue::Call(b, fnSetVel, p.data());
		} else continue; // ohne passende Funktion nicht anfassen (Speicher schreiben erreicht die Physik nicht)
		static int n = 0;
		if (n++ < 6) zl::Log("tweaks: ball touched -> speed x%.2f (%.0f %.0f %.0f)", scale, v[0], v[1], v[2]);
	}
	if (last.size() > 16) last.clear();
}

void Tick(void* car, void* ge, bool freeplay) {
	SetupOnce(ge);
	fpActive = freeplay;
	if (freeplay) { // jede Runde (Bild): nach einer Beruehrung sofort den Schub geben
		float bs;
		{
			std::lock_guard<std::mutex> lk(mu);
			bs = list[1].value;
		}
		ApplyTouches(bs);
	}
	static unsigned long long next = 0;
	unsigned long long now = GetTickCount64();
	if (now < next) return;
	next = now + 250;
	float v[8];
	{
		std::lock_guard<std::mutex> lk(mu);
		for (int i = 0; i < count; i++) v[i] = list[i].value;
	}
	int g = gen.load();
	// Spielgeschwindigkeit
	void* wi = (car && offWorldInfo >= 0) ? ue::Read<void*>(car, offWorldInfo) : nullptr;
	if (wi && offDilation >= 0 && ue::IsAlive(wi)) {
		float* d = ue::Ptr<float>(wi, offDilation);
		if (wi != lastWorld) { lastWorld = wi; worldOrig = *d > 0.0f ? *d : 1.0f; worldTouched = false; }
		if (freeplay) {
			float want = v[0];
			if (fabsf(*d - want) > 0.001f && (worldTouched || fabsf(want - 1.0f) > 0.001f)) { *d = want; worldTouched = true; }
		} else if (worldTouched) { *d = worldOrig; worldTouched = false; } // nie ausserhalb des Freeplays
	}
	if (!freeplay) return;
	// Ball: neue Baelle (nach Reset/Tor) und geaenderte Werte
	for (void* b : Balls(ge)) {
		auto bg = ballGen.find(b);
		bool fresh = bg == ballGen.end();
		bool drift = false;
		for (auto& f : ballFns)
			if (f.offProp >= 0 && fabsf(ue::Read<float>(b, f.offProp) - v[f.setting]) > 0.001f) drift = true;
		if (offCarBounce >= 0 && carBounceBase.count(b)) {
			float base = carBounceBase[b], want = base > 0.01f ? base * v[1] : (v[1] - 1.0f);
			if (fabsf(ue::Read<float>(b, offCarBounce) - want) > 0.001f) drift = true;
		}
		if (!fresh && bg->second == g && !drift) continue;
		bool any = false;
		for (auto& f : ballFns) {
			if (!f.f || f.param < 0) continue;
			float cur = f.offProp >= 0 ? ue::Read<float>(b, f.offProp) : -1.0f;
			if (fresh && f.offProp < 0 && fabsf(v[f.setting] - 1.0f) < 0.001f) continue; // Standard und nicht pruefbar: nicht anfassen
			if (f.offProp >= 0 && fabsf(cur - v[f.setting]) < 0.001f) continue;
			std::vector<uint8_t> p(size_t(ue::StructSize(f.f)) + 16, 0);
			*reinterpret_cast<float*>(p.data() + f.param) = v[f.setting];
			ue::Call(b, f.f, p.data());
			any = true;
		}
		// Schussstaerke (Ball speed)
		if (fnCarBounce && offCarBounceArg >= 0) {
			auto cb = carBounceBase.find(b);
			if (cb == carBounceBase.end()) {
				float base = offCarBounce >= 0 ? ue::Read<float>(b, offCarBounce) : 1.0f;
				cb = carBounceBase.emplace(b, base).first;
				static int lb = 0;
				if (lb++ < 3) zl::Log("tweaks: %s car bounce default %.3f", ue::Name(b).c_str(), base);
			}
			float base = cb->second;
			float want = base > 0.01f ? base * v[1] : (v[1] - 1.0f);
			float cur = offCarBounce >= 0 ? ue::Read<float>(b, offCarBounce) : -999.0f;
			if (fabsf(cur - want) > 0.001f && (fabsf(v[1] - 1.0f) > 0.001f || cur != -999.0f)) {
				std::vector<uint8_t> p(size_t(ue::StructSize(fnCarBounce)) + 16, 0);
				*reinterpret_cast<float*>(p.data() + offCarBounceArg) = want;
				ue::Call(b, fnCarBounce, p.data());
				any = true;
			}
		}
		ballGen[b] = g;
		static int logged = 0;
		if (any && logged++ < 12)
			zl::Log("tweaks: %s speed %.2f size %.2f gravity %.2f bounce %.2f", ue::Name(b).c_str(), v[1], v[2], v[3], v[4]);
	}
	if (carBounceBase.size() > 32)
		for (auto it = carBounceBase.begin(); it != carBounceBase.end();) it = ue::IsAlive(it->first) ? std::next(it) : carBounceBase.erase(it);
	if (ballGen.size() > 32)
		for (auto it = ballGen.begin(); it != ballGen.end();) it = ue::IsAlive(it->first) ? std::next(it) : ballGen.erase(it);
}

} // namespace tweaks
