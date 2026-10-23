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
	{"ballspeed", "Ball speed", "How fast the ball can fly.", 1.0f, 0.25f, 4.0f, 0.05f, 1.0f},
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
static void *clsBall = nullptr, *clsActor = nullptr;
static int offWorldInfo = -1, offDilation = -1, offGameBalls = -1;
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
	zl::Log("tweaks: ball=%p worldInfo=%x dilation=%x gameBalls=%x %s", clsBall, offWorldInfo, offDilation, offGameBalls, have.c_str());
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

void Tick(void* car, void* ge, bool freeplay) {
	static unsigned long long next = 0;
	unsigned long long now = GetTickCount64();
	if (now < next) return;
	next = now + 250;
	SetupOnce(ge);
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
		ballGen[b] = g;
		static int logged = 0;
		if (any && logged++ < 12)
			zl::Log("tweaks: %s speed %.2f size %.2f gravity %.2f bounce %.2f", ue::Name(b).c_str(), v[1], v[2], v[3], v[4]);
	}
	if (ballGen.size() > 32)
		for (auto it = ballGen.begin(); it != ballGen.end();) it = ue::IsAlive(it->first) ? std::next(it) : ballGen.erase(it);
}

} // namespace tweaks
