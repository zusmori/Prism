#include "items.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <set>

#include "log.h"
#include "ue.h"

namespace items {

const SlotInfo kSlots[] = {
	{0, "Car"}, {1, "Decal"}, {2, "Wheels"}, {3, "Boost"}, {5, "Topper"}, {4, "Antenna"},
	{15, "Goal Explosion"}, {14, "Trail"}, {13, "Engine Audio"}, {7, "Paint Finish"},
};
const int kSlotCount = sizeof(kSlots) / sizeof(kSlots[0]);

std::mutex mu;
std::vector<Item> list;
std::map<int, int> overrides;
bool loaded = false;
std::string status = "Starting …";

// ---------- Spiel-Thread-Warteschlange ----------
static std::mutex qmu;
static std::vector<std::function<void()>> queue;
static std::atomic<bool> pending{false};
void Queue(std::function<void()> fn) {
	std::lock_guard<std::mutex> lk(qmu);
	queue.push_back(std::move(fn));
	pending = true;
}
void RunQueue() {
	if (!pending.load(std::memory_order_relaxed)) return;
	pending = false;
	std::vector<std::function<void()>> run;
	{
		std::lock_guard<std::mutex> lk(qmu);
		if (queue.empty()) return;
		run.swap(queue);
	}
	for (auto& f : run) {
		try { f(); } catch (...) { zl::Log("ERROR: task crashed (caught)"); }
	}
}

// ---------- Engine-Infos ----------
static void *clsDB, *clsProduct, *clsSlot, *clsPC, *clsPRI;
static int offProducts = -1, offLabel = -1, offLongLabel = -1, offSlot = -1, offQuality = -1, offSlotIndex = -1;
static int offPlayer = -1, offPRI = -1, offClient = -1, offFull = -1, offPawn = -1, offPawnPRI = -1;
static void *fnSet = nullptr, *fnSetInternal = nullptr;
static void *clsCar = nullptr, *clsLocalPlayer = nullptr, *fnCarSetLoadout = nullptr, *fnTeamIndex = nullptr;
static int offLPActor = -1, offTeamRet = -1;
static std::set<void*> tickFns;       // alle "PlayerTick"-Funktionen
static std::atomic<void*> localPC{nullptr};
static bool ready = false;

bool IsReady() { return ready; }

bool Setup() {
	if (ready) return true;
	clsDB = ue::FindClass("Class TAGame.ProductDatabase_TA");
	clsProduct = ue::FindClass("Class TAGame.Product_TA");
	clsSlot = ue::FindClass("Class TAGame.ProductSlot_TA");
	clsPC = ue::FindClass("Class TAGame.PlayerController_TA");
	clsPRI = ue::FindClass("Class TAGame.PRI_TA");
	zl::Log("classes: db=%p product=%p slot=%p pc=%p pri=%p", clsDB, clsProduct, clsSlot, clsPC, clsPRI);
	if (!clsDB || !clsProduct || !clsSlot || !clsPC || !clsPRI) return false;
	offProducts = ue::PropOffset(clsDB, "Products_New");
	if (offProducts < 0) offProducts = ue::PropOffset(clsDB, "Products");
	offLabel = ue::PropOffset(clsProduct, "Label");
	offLongLabel = ue::PropOffset(clsProduct, "LongLabel");
	offSlot = ue::PropOffset(clsProduct, "Slot");
	offQuality = ue::PropOffset(clsProduct, "Quality");
	offSlotIndex = ue::PropOffset(clsSlot, "SlotIndex");
	offPlayer = ue::PropOffset(clsPC, "Player");
	offPRI = ue::PropOffset(clsPC, "PlayerReplicationInfo");
	offClient = ue::PropOffset(clsPRI, "ClientLoadouts");
	offFull = ue::PropOffset(clsPRI, "FullLoadouts");
	fnSet = ue::FindFunction(clsPRI, "OnLoadoutsSet");
	fnSetInternal = ue::FindFunction(clsPRI, "OnLoadoutsSetInternal");
	zl::Log("offsets: products=%x label=%x long=%x slot=%x quality=%x slotIndex=%x player=%x pri=%x client=%x full=%x",
		offProducts, offLabel, offLongLabel, offSlot, offQuality, offSlotIndex, offPlayer, offPRI, offClient, offFull);
	zl::Log("functions: OnLoadoutsSet=%p OnLoadoutsSetInternal=%p", fnSet, fnSetInternal);
	clsCar = ue::FindClass("Class TAGame.Car_TA");
	clsLocalPlayer = ue::FindClass("Class Engine.LocalPlayer");
	offPawn = ue::PropOffset(clsPC, "Pawn");
	if (clsCar) {
		offPawnPRI = ue::PropOffset(clsCar, "PlayerReplicationInfo");
		fnCarSetLoadout = ue::FindFunction(clsCar, "SetLoadout");
		fnTeamIndex = ue::FindFunction(clsCar, "GetLoadoutTeamIndex");
		if (fnTeamIndex) offTeamRet = ue::PropOffset(fnTeamIndex, "ReturnValue");
	}
	if (clsLocalPlayer) offLPActor = ue::PropOffset(clsLocalPlayer, "Actor");
	// alle PlayerTick-Funktionen merken: wer sie aufruft, ist der lokale Spieler
	for (int i = 0; i < ue::ObjectCount(); i++) {
		void* o = ue::ObjectAt(i);
		try {
			if (o && ue::Name(o) == "PlayerTick" && ue::Name(ue::ClassOf(o)) == "Function") tickFns.insert(o);
		} catch (...) {}
	}
	zl::Log("car=%p localPlayer=%p pawn=%x pawnPRI=%x lpActor=%x Car.SetLoadout=%p GetLoadoutTeamIndex=%p ret=%x tickFns=%d",
		clsCar, clsLocalPlayer, offPawn, offPawnPRI, offLPActor, fnCarSetLoadout, fnTeamIndex, offTeamRet, (int)tickFns.size());
	ready = offProducts >= 0 && offSlot >= 0 && offSlotIndex >= 0 && offPlayer >= 0 && offPRI >= 0 && offClient >= 0 && fnSet;
	return ready;
}

void LoadProducts() {
	if (!Setup()) {
		std::lock_guard<std::mutex> lk(mu);
		status = "Couldn't find the item system. See zlmod.log";
		return;
	}
	auto dbs = ue::Instances(clsDB);
	if (dbs.empty()) {
		std::lock_guard<std::mutex> lk(mu);
		status = "Item list not ready yet …";
		return;
	}
	std::set<int> wanted;
	for (int i = 0; i < kSlotCount; i++) wanted.insert(kSlots[i].index);
	std::vector<Item> out;
	auto* arr = ue::Ptr<ue::TArray<void*>>(dbs[0], offProducts);
	zl::Log("product database %p: %d products", dbs[0], arr->count);
	for (int id = 0; id < arr->count && id < 100000; id++) {
		void* p = arr->data[id];
		if (!p) continue;
		try {
			void* slot = ue::Read<void*>(p, offSlot);
			if (!slot) continue;
			int si = ue::Read<int32_t>(slot, offSlotIndex);
			if (!wanted.count(si)) continue;
			Item it;
			it.id = id;
			it.slot = si;
			it.quality = offQuality >= 0 ? ue::Read<uint8_t>(p, offQuality) : 0;
			if (offLongLabel >= 0) it.name = ue::Narrow(ue::Read<ue::FString>(p, offLongLabel));
			if (it.name.empty() && offLabel >= 0) it.name = ue::Narrow(ue::Read<ue::FString>(p, offLabel));
			if (it.name.empty()) it.name = ue::Name(p);
			out.push_back(std::move(it));
		} catch (...) {}
	}
	std::sort(out.begin(), out.end(), [](const Item& a, const Item& b) { return a.name < b.name; });
	zl::Log("items loaded: %d", (int)out.size());
	std::lock_guard<std::mutex> lk(mu);
	list = std::move(out);
	loaded = !list.empty();
	status = loaded ? "" : "No items found. See zlmod.log";
}

// ---------- Loadout ----------
// Lokaler PlayerController: per PlayerTick erkannt, sonst ueber LocalPlayer.Actor bzw. Player != null
static void* LocalPC() {
	if (void* pc = localPC.load()) return pc;
	try {
		if (clsLocalPlayer && offLPActor >= 0)
			for (void* lp : ue::Instances(clsLocalPlayer))
				if (void* a = ue::Read<void*>(lp, offLPActor)) return a;
		for (void* pc : ue::Instances(clsPC))
			if (ue::Read<void*>(pc, offPlayer)) return pc;
	} catch (...) {}
	return nullptr;
}
static void* LocalPRI() {
	void* pc = LocalPC();
	try { return pc ? ue::Read<void*>(pc, offPRI) : nullptr; } catch (...) { return nullptr; }
}
static void* LocalCar() {
	void* pc = LocalPC();
	try {
		void* pawn = (pc && offPawn >= 0) ? ue::Read<void*>(pc, offPawn) : nullptr;
		return (pawn && ue::IsA(pawn, clsCar)) ? pawn : nullptr;
	} catch (...) { return nullptr; }
}

static std::map<int, int> originals[2]; // Team -> Slot -> eigenes Item
static std::set<int> restore;           // Slots, die zurueck auf das eigene Item sollen
static bool loggedOnce = false;
static bool applying = false;

static void Patch(ue::TArray<int32_t>* a, int team, const std::map<int, int>& ov) {
	if (!a || !a->data || a->count <= 0 || a->count > 64) return;
	for (auto& [slot, id] : ov) {
		if (slot >= a->count) continue;
		if (!originals[team].count(slot)) originals[team][slot] = a->data[slot];
		a->data[slot] = id;
	}
	for (int slot : restore) {
		if (slot < a->count && originals[team].count(slot)) a->data[slot] = originals[team][slot];
	}
}

static void WriteOverrides(void* pri) {
	std::map<int, int> ov;
	{
		std::lock_guard<std::mutex> lk(mu);
		ov = overrides;
	}
	for (int t = 0; t < 2; t++) {
		auto* client = ue::Ptr<ue::TArray<int32_t>>(pri, offClient + t * 0x10);
		if (!loggedOnce) {
			std::string s;
			for (int i = 0; client->data && i < client->count && i < 32; i++) s += std::to_string(client->data[i]) + " ";
			zl::Log("ClientLoadouts[%d] (%d): %s", t, client->count, s.c_str());
		}
		Patch(client, t, ov);
		if (offFull >= 0) Patch(ue::Ptr<ue::TArray<int32_t>>(pri, offFull + t * 0x40), t, ov);
	}
	loggedOnce = true;
}

void ApplyNow() {
	if (!Setup()) return;
	void* pc = LocalPC();
	void* pri = LocalPRI();
	if (!pri) {
		static int n = 0;
		if (n++ < 3) {
			int pcs = (int)ue::Instances(clsPC).size(), lps = clsLocalPlayer ? (int)ue::Instances(clsLocalPlayer).size() : -1;
			zl::Log("apply: no local player yet (pc=%p, controllers=%d, localPlayers=%d, objects=%d, ticks seen=%s) - items work in Freeplay and matches, not in menus",
				pc, pcs, lps, ue::ObjectCount(), localPC.load() ? "yes" : "no");
		}
		return;
	}
	WriteOverrides(pri);
	applying = true;
	void* car = LocalCar();
	if (car && fnCarSetLoadout) {
		int team = 0;
		if (fnTeamIndex && offTeamRet >= 0) {
			std::vector<uint8_t> p(size_t(ue::StructSize(fnTeamIndex)) + 16, 0);
			ue::Call(car, fnTeamIndex, p.data());
			team = *reinterpret_cast<int32_t*>(p.data() + offTeamRet);
			if (team < 0 || team > 1) team = 0;
		}
		// Kopie des (schon geaenderten) Loadouts ans Auto geben
		ue::TArray<int32_t> data = *ue::Ptr<ue::TArray<int32_t>>(pri, offClient + team * 0x10);
		std::vector<uint8_t> p(size_t(ue::StructSize(fnCarSetLoadout)) + 16, 0);
		memcpy(p.data(), &data, sizeof data);
		ue::Call(car, fnCarSetLoadout, p.data());
		zl::Log("apply: Car.SetLoadout on %p (team %d, %d products)", car, team, data.count);
	} else {
		zl::Log("apply: no car (pawn) right now");
	}
	ue::Call(pri, fnSet);
	applying = false;
	restore.clear();
	zl::Log("apply: done for PRI %p", pri);
}

static void PatchParams(void* params) {
	std::map<int, int> ov;
	{
		std::lock_guard<std::mutex> lk(mu);
		ov = overrides;
	}
	auto* a = reinterpret_cast<ue::TArray<int32_t>*>(params);
	if (!a || !a->data || a->count <= 0 || a->count > 64) return;
	for (auto& [slot, id] : ov)
		if (slot < a->count) a->data[slot] = id;
}

void OnProcessEvent(void* caller, void* fn, void* params) {
	if (!ready || !caller || !fn) return;
	if (tickFns.count(fn)) { // lokaler Spieler
		if (localPC.load() != caller && ue::IsA(caller, clsPC)) {
			localPC = caller;
			void* pri = nullptr; void* pawn = nullptr;
			try { pri = ue::Read<void*>(caller, offPRI); pawn = offPawn >= 0 ? ue::Read<void*>(caller, offPawn) : nullptr; } catch (...) {}
			zl::Log("local player controller: %p (%s) pri=%p pawn=%p (%s)", caller, ue::Name(caller).c_str(), pri, pawn, pawn ? ue::Name(pawn).c_str() : "-");
			ue::CheckObjects(caller);
		}
		return;
	}
	if (applying) return;
	if (fn == fnCarSetLoadout) { // Auto bekommt sein Loadout (z. B. nach Respawn): unsere Items eintragen
		bool mine = false;
		try { mine = offPawnPRI >= 0 && ue::Read<void*>(caller, offPawnPRI) == LocalPRI(); } catch (...) {}
		if (mine) PatchParams(params);
		return;
	}
	if (fn != fnSet && fn != fnSetInternal) return;
	{
		std::lock_guard<std::mutex> lk(mu);
		if (overrides.empty() && restore.empty()) return;
	}
	if (caller == LocalPRI()) WriteOverrides(caller); // Spiel laedt das Loadout neu: unsere Items wieder eintragen
}

void Set(int slot, int id) {
	{
		std::lock_guard<std::mutex> lk(mu);
		if (id > 0) overrides[slot] = id;
		else overrides.erase(slot);
	}
	Queue([slot, id] {
		if (id <= 0) restore.insert(slot);
		ApplyNow();
	});
	Save();
}

static std::wstring SaveFile() { return zl::DataDir() + L"\\zlmod.json"; }

void Save() {
	std::map<int, int> ov;
	{
		std::lock_guard<std::mutex> lk(mu);
		ov = overrides;
	}
	FILE* f = _wfopen(SaveFile().c_str(), L"w");
	if (!f) return;
	fputc('{', f);
	bool first = true;
	for (auto& [s, id] : ov) {
		fprintf(f, "%s\"%d\":%d", first ? "" : ",", s, id);
		first = false;
	}
	fputs("}\n", f);
	fclose(f);
}

void LoadSaved() {
	FILE* f = _wfopen(SaveFile().c_str(), L"r");
	if (!f) return;
	char buf[4096] = {0};
	size_t n = fread(buf, 1, sizeof buf - 1, f);
	fclose(f);
	buf[n] = 0;
	std::lock_guard<std::mutex> lk(mu);
	for (char* p = buf; (p = strchr(p, '"'));) {
		int s = 0, id = 0;
		if (sscanf(p, "\"%d\":%d", &s, &id) == 2 && id > 0) overrides[s] = id;
		p = strchr(p + 1, '"');
		if (!p) break;
		p++;
	}
	zl::Log("saved items: %d", (int)overrides.size());
}

} // namespace items
