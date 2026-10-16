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
std::atomic<bool> inGame{false};

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
static void *clsGameEvent = nullptr, *clsFreeplay = nullptr, *clsFreeplayCar = nullptr, *fnDestroyCars = nullptr, *fnRestartPlayers = nullptr;
static int offPRIGameEvent = -1;
static void *clsPreview = nullptr, *fnChangePart = nullptr;
static int offPrevLoadout = -1, offCPSlot = -1, offCPProduct = -1, offCPOnline = -1, offCPPaint = -1;
static std::atomic<unsigned long long> applyAt{0}; // Zeitpunkt fuer das (gebuendelte) Anwenden
static std::set<void*> tickFns;       // alle "PlayerTick"-Funktionen
static std::atomic<void*> localPC{nullptr};
static std::vector<void*> localPlayers; // LocalPlayer-Objekte (einmal gesucht)
static std::atomic<void*> writtenPRI{nullptr};  // PRI, in den wir beim Laden schon geschrieben haben
static std::atomic<unsigned long long> lastPlayerTick{0}; // letzter PlayerTick = gerade im Spiel
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
	if (clsLocalPlayer) localPlayers = ue::Instances(clsLocalPlayer);
	clsGameEvent = ue::FindClass("Class TAGame.GameEvent_TA");
	clsFreeplay = ue::FindClass("Class TAGame.GameEvent_Tutorial_FreePlay_TA");
	clsFreeplayCar = ue::FindClass("Class TAGame.Car_Freeplay_TA");
	offPRIGameEvent = ue::PropOffset(clsPRI, "GameEvent");
	if (clsGameEvent) {
		fnDestroyCars = ue::FindFunction(clsGameEvent, "DestroyCars");
		fnRestartPlayers = ue::FindFunction(clsGameEvent, "RestartPlayers");
	}
	zl::Log("respawn: freeplay=%p freeplayCar=%p priGameEvent=%x DestroyCars=%p RestartPlayers=%p", clsFreeplay, clsFreeplayCar, offPRIGameEvent, fnDestroyCars, fnRestartPlayers);
	// alle PlayerTick-Funktionen merken: wer sie aufruft, ist der lokale Spieler
	for (int i = 0; i < ue::ObjectCount(); i++) {
		void* o = ue::ObjectAt(i);
		try {
			if (o && ue::Name(o) == "PlayerTick" && ue::Name(ue::ClassOf(o)) == "Function") tickFns.insert(o);
		} catch (...) {}
	}
	clsPreview = ue::FindClass("Class TAGame.CarPreviewActor_TA");
	if (clsPreview) {
		offPrevLoadout = ue::PropOffset(clsPreview, "Loadout");
		fnChangePart = ue::FindFunction(clsPreview, "ChangeCarPart");
		if (fnChangePart) {
			offCPSlot = ue::PropOffset(fnChangePart, "SlotIndex");
			offCPProduct = ue::PropOffset(fnChangePart, "ProductID");
			offCPOnline = ue::PropOffset(fnChangePart, "OnlineID");
			offCPPaint = ue::PropOffset(fnChangePart, "TeamPaintIndex");
		}
	}
	zl::Log("menu car: preview=%p loadout=%x ChangeCarPart=%p (slot=%x product=%x online=%x paint=%x)",
		clsPreview, offPrevLoadout, fnChangePart, offCPSlot, offCPProduct, offCPOnline, offCPPaint);
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
	if (void* pc = localPC.load()) {
		if (ue::IsValid(pc)) return pc;
		localPC = nullptr; // Spieler gibt es nicht mehr (z. B. zurueck im Menue)
	}
	try {
		if (clsLocalPlayer && offLPActor >= 0)
			for (void* lp : localPlayers)
				if (void* a = ue::Read<void*>(lp, offLPActor)) return a;
		for (void* pc : ue::Instances(clsPC))
			if (ue::Read<void*>(pc, offPlayer)) return pc;
	} catch (...) {}
	return nullptr;
}
static void* LocalPRI() {
	void* pc = LocalPC();
	try {
		void* pri = pc ? ue::Read<void*>(pc, offPRI) : nullptr;
		return ue::IsValid(pri) ? pri : nullptr;
	} catch (...) { return nullptr; }
}
static void* LocalCar() {
	void* pc = LocalPC();
	try {
		void* pawn = (pc && offPawn >= 0) ? ue::Read<void*>(pc, offPawn) : nullptr;
		return (ue::IsValid(pawn) && ue::IsA(pawn, clsCar)) ? pawn : nullptr;
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
	if (!LocalCar()) return; // nicht im Spiel: Items sind gespeichert und kommen beim naechsten Spawn
	static int n = 0;
	inGame = pri != nullptr;
	if (pri) n = 0;
	if (!pri) {
		if (n++ < 2) {
			int pcs = (int)ue::Instances(clsPC).size(), lps = clsLocalPlayer ? (int)ue::Instances(clsLocalPlayer).size() : -1;
			zl::Log("apply: no local player yet (pc=%p, controllers=%d, localPlayers=%d, objects=%d, ticks seen=%s) - items work in Freeplay and matches, not in menus",
				pc, pcs, lps, ue::ObjectCount(), localPC.load() ? "yes" : "no");
		}
		return;
	}
	WriteOverrides(pri);
	restore.clear();
	// Ein laufendes Auto darf sein Loadout nicht neu bekommen (doppelte Bauteile -> kaputte Steuerung,
	// Boost-Einstellungen weg). Stattdessen im Freeplay das Auto sauber neu spawnen lassen:
	// das Spiel baut es dann ganz normal mit unserem Loadout auf.
	void* ge = nullptr;
	try { ge = offPRIGameEvent >= 0 ? ue::Read<void*>(pri, offPRIGameEvent) : nullptr; } catch (...) {}
	// Freeplay erkennt man am Auto (Car_Freeplay_TA); das Spiel selbst ist ein normales GameEvent_Soccar_TA
	void* car = LocalCar();
	bool freeplay = ue::IsValid(ge) && ((clsFreeplay && ue::IsA(ge, clsFreeplay)) || (car && clsFreeplayCar && ue::IsA(car, clsFreeplayCar)));
	// Funktionen der echten Klasse des Spiels nehmen (Soccar ueberschreibt z. B. RestartPlayer)
	void* gcls = ge ? ue::ClassOf(ge) : nullptr;
	void* destroy = gcls ? ue::FindFunction(gcls, "DestroyCars") : nullptr;
	void* restart = gcls ? ue::FindFunction(gcls, "RestartPlayers") : nullptr;
	if (freeplay && destroy && restart) {
		ue::Call(ge, destroy);
		ue::Call(pri, fnSet);
		ue::Call(ge, restart);
		void* now = LocalCar();
		zl::Log("apply: car respawned in freeplay (%s, old car %p, new car %p %s)", ue::Name(ge).c_str(), car, now, now ? ue::Name(now).c_str() : "-");
	} else {
		ue::Call(pri, fnSet);
		zl::Log("apply: not in freeplay (%s) - items show on the next spawn", ge ? ue::Name(ge).c_str() : "-");
	}
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

// Gehoert der PRI dem lokalen Spieler? (Controller des PRI hat einen LocalPlayer)
static bool IsLocalPRI(void* pri) {
	try {
		for (void* lp : localPlayers) {
			if (!ue::IsValid(lp)) continue;
			void* pc = ue::Read<void*>(lp, offLPActor);
			if (pc && ue::IsValid(pc) && ue::Read<void*>(pc, offPRI) == pri) return true;
		}
	} catch (...) {}
	return false;
}

// Fehlen unsere Items im Loadout des PRI?
static bool NeedsApply(void* pri) {
	std::map<int, int> ov;
	{
		std::lock_guard<std::mutex> lk(mu);
		ov = overrides;
	}
	if (ov.empty()) return false;
	try {
		for (int t = 0; t < 2; t++) {
			auto* cl = ue::Ptr<ue::TArray<int32_t>>(pri, offClient + t * 0x10);
			if (!cl->data || cl->count <= 0 || cl->count > 64) continue;
			for (auto& [slot, id] : ov)
				if (slot < cl->count && cl->data[slot] != id) return true;
		}
	} catch (...) {}
	return false;
}

void OnProcessEvent(void* caller, void* fn, void* params) {
	if (!ready || !caller || !fn) return;
	if (tickFns.count(fn)) { // lokaler Spieler
		// Im Hauptmenue tickt auch ein Controller - der zaehlt nur, wenn er wirklich ein Auto faehrt
		if (!ue::IsA(caller, clsPC)) return;
		void* pawn = nullptr;
		try { pawn = offPawn >= 0 ? ue::Read<void*>(caller, offPawn) : nullptr; } catch (...) {}
		if (!pawn || !clsCar || !ue::IsA(pawn, clsCar)) return;
		if (localPC.load() != caller) {
			localPC = caller;
			void* pri = nullptr; void* pawn = nullptr;
			try { pri = ue::Read<void*>(caller, offPRI); pawn = offPawn >= 0 ? ue::Read<void*>(caller, offPawn) : nullptr; } catch (...) {}
			zl::Log("local player controller: %p (%s) pri=%p pawn=%p (%s)", caller, ue::Name(caller).c_str(), pri, pawn, pawn ? ue::Name(pawn).c_str() : "-");
			ue::CheckObjects(caller);
			// Neues Spiel: pruefen, ob das Auto schon unsere Items hat - sonst einmal neu spawnen
			applyAt = 0;
			bool hasOv;
			{
				std::lock_guard<std::mutex> lk(mu);
				hasOv = !overrides.empty();
			}
			if (pri && hasOv) {
				bool missing = NeedsApply(pri), written = writtenPRI.load() == pri;
				zl::Log("new game: items in loadout=%s, written on load=%s", missing ? "no" : "yes", written ? "yes" : "no");
				if (missing || !written) applyAt = GetTickCount64() + 300; // sicherheitshalber einmal neu spawnen
			}
		}
		inGame = true;
		lastPlayerTick = GetTickCount64();
		// Gebuendeltes Anwenden hier im PlayerTick: sicherer Moment, um das Auto neu zu spawnen
		if (unsigned long long t = applyAt.load(); t && GetTickCount64() >= t) {
			applyAt = 0;
			try { ApplyNow(); } catch (...) { zl::Log("ERROR: apply crashed (caught)"); }
		}
		return;
	}
	if (applying) return;
	if (fn == fnCarSetLoadout) { // Auto bekommt sein Loadout (z. B. nach Respawn): unsere Items eintragen
		bool mine = false;
		void* cpri = nullptr;
		try { cpri = offPawnPRI >= 0 ? ue::Read<void*>(caller, offPawnPRI) : nullptr; } catch (...) {}
		mine = cpri ? cpri == LocalPRI() : (clsFreeplayCar && ue::IsA(caller, clsFreeplayCar));
		if (mine) {
			PatchParams(params);
			static int n = 0;
			if (n++ < 20) zl::Log("car %s gets its loadout -> patched", ue::Name(caller).c_str());
		}
		return;
	}
	if (fn != fnSet && fn != fnSetInternal) return;
	{
		std::lock_guard<std::mutex> lk(mu);
		if (overrides.empty() && restore.empty()) return;
	}
	// Spiel laedt das Loadout (z. B. beim Betreten von Freeplay/Match): unsere Items eintragen.
	// Nur bei einem PRI mit echtem Loadout (im Hauptmenue sind die Listen leer).
	if (ue::IsA(caller, clsPRI)) {
		auto* cl = ue::Ptr<ue::TArray<int32_t>>(caller, offClient);
		if (cl->data && cl->count > 0 && cl->count <= 64 && (IsLocalPRI(caller) || caller == LocalPRI())) {
			WriteOverrides(caller);
			writtenPRI = caller;
			static int n = 0;
			if (n++ < 10) zl::Log("loadout set for %s -> items written", ue::Name(caller).c_str());
		}
	}
}

// ---------- Auto im Hauptmenue / in der Garage (CarPreviewActor_TA) ----------
// Das Spiel setzt das Vorschau-Auto immer wieder aus deinem Profil. Wir schauen regelmaessig nach
// und tauschen nur die Teile, die nicht passen, ueber die Garage-Funktion ChangeCarPart aus.
struct PreviewState {
	std::map<int, int> orig;     // Slot -> eigenes Item
	std::map<int, int> lastSeen; // Slot -> zuletzt gesehener Wert
	std::map<int, int> target;   // Slot -> zuletzt gesetztes Item
	std::map<int, unsigned long long> until; // Slot -> nicht vor diesem Zeitpunkt erneut tauschen
};
static std::map<void*, PreviewState> previews;
static std::vector<void*> previewActors;
static unsigned long long nextPreviewScan = 0;
static std::atomic<unsigned long long> nextPreviewCheck{0};

static bool ChangePart(void* actor, int slot, int id) {
	if (!fnChangePart || offCPSlot < 0 || offCPProduct < 0) return false;
	std::vector<uint8_t> p(size_t(ue::StructSize(fnChangePart)) + 16, 0);
	*reinterpret_cast<int32_t*>(p.data() + offCPSlot) = slot;
	*reinterpret_cast<int32_t*>(p.data() + offCPProduct) = id;
	if (offCPPaint >= 0) *reinterpret_cast<int32_t*>(p.data() + offCPPaint) = 0;
	ue::Call(actor, fnChangePart, p.data());
	return true;
}

static void SyncPreview(void* actor) {
	auto* prods = ue::Ptr<ue::TArray<int32_t>>(actor, offPrevLoadout);
	if (!prods->data || prods->count <= 0 || prods->count > 64) return;
	std::map<int, int> ov;
	{
		std::lock_guard<std::mutex> lk(mu);
		ov = overrides;
	}
	PreviewState& st = previews[actor];
	for (auto& [slot, id] : ov) {
		if (slot >= prods->count) continue;
		int now = prods->data[slot];
		if (now == id) { st.lastSeen[slot] = now; st.target[slot] = id; continue; }
		auto ls = st.lastSeen.find(slot);
		auto tg = st.target.find(slot);
		if (ls != st.lastSeen.end() && ls->second == now && tg != st.target.end() && tg->second == id) continue; // schon gesetzt
		unsigned long long t = GetTickCount64();
		if (t < st.until[slot]) continue; // nicht staendig neu laden (z. B. beim Durchscrollen in der Garage)
		st.until[slot] = t + 2500;
		if (!st.orig.count(slot)) st.orig[slot] = now; // dein eigenes Item
		ChangePart(actor, slot, id);
		st.lastSeen[slot] = prods->data[slot];
		st.target[slot] = id;
		zl::Log("menu car %s: slot %d %d -> %d (now %d)", ue::Name(actor).c_str(), slot, now, id, prods->data[slot]);
	}
	// Slots zurueck auf das eigene Item
	for (auto it = st.orig.begin(); it != st.orig.end();) {
		int slot = it->first;
		if (ov.count(slot) || slot >= prods->count) { ++it; continue; }
		ChangePart(actor, slot, it->second);
		zl::Log("menu car %s: slot %d back to %d", ue::Name(actor).c_str(), slot, it->second);
		st.lastSeen.erase(slot);
		st.target.erase(slot);
		it = st.orig.erase(it);
	}
}

bool InMatch() { return GetTickCount64() - lastPlayerTick.load() < 1500; }

void TickPreview() {
	if (!ready || !clsPreview || offPrevLoadout < 0 || !fnChangePart) return;
	unsigned long long t = GetTickCount64();
	if (t < nextPreviewCheck) return;
	nextPreviewCheck = t + 500;
	// Im Spiel (Freeplay/Match) gibt es kein Vorschau-Auto: nicht suchen, damit nichts ruckelt
	if (t - lastPlayerTick.load() < 1500) return;
	if (t >= nextPreviewScan) {
		nextPreviewScan = t + 3000;
		previewActors = ue::Instances(clsPreview);
		static int logged = 0;
		if (logged++ < 3) zl::Log("menu cars: %d (scan %llu ms)", (int)previewActors.size(), GetTickCount64() - t);
	}
	for (auto it = previews.begin(); it != previews.end();) // alte Autos vergessen
		it = ue::IsValid(it->first) ? std::next(it) : previews.erase(it);
	for (void* a : previewActors) {
		if (!ue::IsValid(a)) continue;
		try { SyncPreview(a); } catch (...) {}
	}
}

void OnMenuOpened() {
	Queue([] {
		if (!Setup()) return;
		bool now = LocalPRI() != nullptr;
		if (now != inGame.load()) zl::Log("menu opened: %s", now ? "in game" : "not in a game (menu/garage)");
		inGame = now;
	});
}

void Set(int slot, int id) {
	{
		std::lock_guard<std::mutex> lk(mu);
		if (id > 0) overrides[slot] = id;
		else overrides.erase(slot);
	}
	Queue([slot, id] {
		if (id <= 0) restore.insert(slot);
	});
	nextPreviewCheck = 0; // Menue-Auto sofort aktualisieren
	applyAt = GetTickCount64() + 400; // mehrere Klicks kurz hintereinander = einmal neu spawnen
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
