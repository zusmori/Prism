#include "items.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <set>
#include <unordered_map>

#include "log.h"
#include "tweaks.h"
#include "camera.h"
#include "customs.h"
#include "merge.h"
#include "ue.h"

namespace items {

const SlotInfo kSlots[] = {
	{0, "Car"}, {1, "Decal"}, {2, "Wheels"}, {3, "Boost"}, {5, "Topper"}, {4, "Antenna"},
	{15, "Goal Explosion"}, {14, "Trail"}, {13, "Engine Audio"}, {7, "Paint Finish"}, {12, "Accent Finish"},
};
const int kSlotCount = sizeof(kSlots) / sizeof(kSlots[0]);

std::mutex mu;
std::vector<Item> list;
std::map<int, int> overrides;
std::map<int, int> paints;
std::vector<PaintInfo> paintList;
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

static void TickColors(void* previewActor, void* car);
static void CarEventColors(void* car);
static void RestoreSharedPrefs();
static void TickCustoms();
static std::atomic<void*> localPRIForColors{nullptr}; // PRI deines Autos (fuer die Farben)
static void PatchColorEvent(void* mesh, void* params);
static void *clsCarMesh = nullptr, *fnApplyColors = nullptr;
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
static std::atomic<unsigned long long> applyAt{0};
static std::atomic<bool> inFreeplay{false};
static std::atomic<bool> inTweaks{false}; // Freeplay oder Training (eigene/fremde Trainings-Packs, Tutorials): Tweaks erlaubt
static void *clsTrainingEditor = nullptr, *clsTutorial = nullptr;
// Kontrolle nach dem Spawn: hat das Auto wirklich unsere Items bekommen?
static int offCarLoadout = -1, offLoaderRemaining = -1, offAIdId = -1, offAIdRet = -1;
static void* fnAssetById = nullptr;
static unsigned long long verifyAt = 0;
static int verifyTries = 0;
// Farben
static void *fnIsPaintable = nullptr, *clsPainted = nullptr, *cdoPainted = nullptr, *fnCreatePaint = nullptr;
static void *fnPathName = nullptr, *cdoObject = nullptr;
static int offIsPaintableRet = -1, offCreateId = -1, offCreateRet = -1, offPaintID = -1, offPathObj = -1, offPathRet = -1;
static int offClientOnline = -1, offFullAttr = -1, offClientOnline1 = -1, offFullOnline = -1;
static bool attrArraysOk = false;
static std::map<int, bool> paintableById;
static std::map<int, uint32_t> paintMaskById; // Zeitpunkt fuer das (gebuendelte) Anwenden
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
		offCarLoadout = ue::PropOffset(clsCar, "Loadout");
		if (void* clsLoader = ue::FindClass("Class TAGame.ProductLoader_TA")) {
			offLoaderRemaining = ue::PropOffset(clsLoader, "RemainingProducts");
			fnAssetById = ue::FindFunction(clsLoader, "GetAssetByID");
			if (fnAssetById) { offAIdId = ue::PropOffset(fnAssetById, "ProductID"); offAIdRet = ue::PropOffset(fnAssetById, "ReturnValue"); }
		}
		zl::Log("verify: carLoadout=%x remaining=%x GetAssetByID=%p (%x/%x)", offCarLoadout, offLoaderRemaining, fnAssetById, offAIdId, offAIdRet);
		fnCarSetLoadout = ue::FindFunction(clsCar, "SetLoadout");
		fnTeamIndex = ue::FindFunction(clsCar, "GetLoadoutTeamIndex");
		if (fnTeamIndex) offTeamRet = ue::PropOffset(fnTeamIndex, "ReturnValue");
	}
	if (clsLocalPlayer) offLPActor = ue::PropOffset(clsLocalPlayer, "Actor");
	if (clsLocalPlayer) localPlayers = ue::Instances(clsLocalPlayer);
	// Farben: Attribut-Objekte und die Listen im PRI
	fnIsPaintable = ue::FindFunction(clsProduct, "IsPaintable");
	if (fnIsPaintable) offIsPaintableRet = ue::PropOffset(fnIsPaintable, "ReturnValue");
	clsPainted = ue::FindClass("Class TAGame.ProductAttribute_Painted_TA");
	cdoPainted = ue::FindObject("ProductAttribute_Painted_TA TAGame.Default__ProductAttribute_Painted_TA");
	if (clsPainted) {
		fnCreatePaint = ue::FindFunction(clsPainted, "CreateInstance");
		offPaintID = ue::PropOffset(clsPainted, "PaintID");
		if (fnCreatePaint) { offCreateId = ue::PropOffset(fnCreatePaint, "InPaintID"); offCreateRet = ue::PropOffset(fnCreatePaint, "ReturnValue"); }
	}
	if (void* clsObject = ue::FindClass("Class Core.Object")) {
		fnPathName = ue::FindFunction(clsObject, "PathName");
		if (fnPathName) { offPathObj = ue::PropOffset(fnPathName, "CheckObject"); offPathRet = ue::PropOffset(fnPathName, "ReturnValue"); }
	}
	cdoObject = ue::FindObject("Object Core.Default__Object");
	offClientOnline = ue::PropOffset(clsPRI, "ClientLoadoutsOnline");
	offFullAttr = ue::PropOffset(clsPRI, "FullLoadoutAttributes");
	offClientOnline1 = ue::PropOffset(clsPRI, "ClientLoadoutOnline");
	offFullOnline = ue::PropOffset(clsPRI, "FullOnlineLoadout");
	zl::Log("paint lists: clientOnline=%x clientsOnline=%x fullOnline=%x fullAttr=%x", offClientOnline1, offClientOnline, offFullOnline, offFullAttr);
	{
		void* s1 = ue::FindObject("ScriptStruct TAGame._Types_TA.ClientLoadoutOnlineProduct");
		void* s2 = ue::FindObject("ScriptStruct TAGame._Types_TA.ProductAttributesArray");
		int z1 = s1 ? ue::StructSize(s1) : -1, z2 = s2 ? ue::StructSize(s2) : -1;
		attrArraysOk = z1 == 0x10;
		zl::Log("paint: IsPaintable=%p painted=%p cdo=%p create=%p (id=%x ret=%x) pathName=%p objCdo=%p online=%x fullAttr=%x sizes=%x/%x",
			fnIsPaintable, clsPainted, cdoPainted, fnCreatePaint, offCreateId, offCreateRet, fnPathName, cdoObject, offClientOnline, offFullAttr, z1, z2);
		if (z2 != 0x10) offFullAttr = -1;
	}
	clsGameEvent = ue::FindClass("Class TAGame.GameEvent_TA");
	clsFreeplay = ue::FindClass("Class TAGame.GameEvent_Tutorial_FreePlay_TA");
	clsFreeplayCar = ue::FindClass("Class TAGame.Car_Freeplay_TA");
	clsTrainingEditor = ue::FindClass("Class TAGame.GameEvent_TrainingEditor_TA");
	clsTutorial = ue::FindClass("Class TAGame.GameEvent_Tutorial_TA");
	zl::Log("training: editor=%p tutorial=%p", clsTrainingEditor, clsTutorial);
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

static bool IsPlaceholder(const std::string& n) {
	auto ends = [&](const char* x) { size_t l = strlen(x); return n.size() >= l && n.compare(n.size() - l, l, x) == 0; };
	return n.rfind("Mystery", 0) == 0 || n.rfind("Random ", 0) == 0 || ends(" Drop") || n.find("Preview") != std::string::npos;
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
	int hidden = 0;
	int offRequired = ue::PropOffset(clsProduct, "RequiredProduct");
	int offProdAttrs = ue::PropOffset(clsProduct, "Attributes");
	void* clsUpload = ue::FindClass("Class TAGame.ProductAttribute_PaintSettingsUpload_TA");
	int offExcl = clsUpload ? ue::PropOffset(clsUpload, "ExcludePaints") : -1, offShopEx = clsUpload ? ue::PropOffset(clsUpload, "ShopExclusivePaints") : -1;
	auto maskOf = [](ue::TArray<int32_t>* a) {
		uint32_t m = 0;
		for (int i = 0; a->data && i < a->count && i < 64; i++)
			if (a->data[i] > 0 && a->data[i] < 32) m |= 1u << a->data[i];
		return m;
	};
	std::map<void*, int> idOf;
	for (int id = 0; id < arr->count && id < 100000; id++)
		if (arr->data[id]) idOf[arr->data[id]] = id;
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
			if (offRequired >= 0) {
				void* rq = ue::Read<void*>(p, offRequired);
				auto f = rq ? idOf.find(rq) : idOf.end();
				if (f != idOf.end()) it.forCar = f->second;
			}
			if (fnIsPaintable && offIsPaintableRet >= 0) {
				std::vector<uint8_t> prm(size_t(ue::StructSize(fnIsPaintable)) + 16, 0);
				ue::Call(p, fnIsPaintable, prm.data());
				it.paintable = (*reinterpret_cast<uint32_t*>(prm.data() + offIsPaintableRet) & 1) != 0;
			}
			// Welche Lackierungen gibt es fuer genau dieses Item? (Ausschluesse / nur-im-Shop aus den Item-Daten)
			if (it.paintable && offProdAttrs >= 0 && clsUpload) {
				auto* attrs = ue::Ptr<ue::TArray<void*>>(p, offProdAttrs);
				for (int k = 0; attrs->data && k < attrs->count && k < 32; k++) {
					void* at = attrs->data[k];
					if (!at || !ue::IsA(at, clsUpload)) continue;
					if (offExcl >= 0) it.paintExclude |= maskOf(ue::Ptr<ue::TArray<int32_t>>(at, offExcl));
					if (offShopEx >= 0) it.paintShop |= maskOf(ue::Ptr<ue::TArray<int32_t>>(at, offShopEx));
				}
			}
			// Platzhalter aus dem Spiel (Belohnungs-Kisten, "Mystery"-Items, Vorschau-Tuecher) sind keine echten Items:
			// sie legen nur ein Tuch/Modell ueber das Auto, das beim Wechseln haengen bleibt
			if (IsPlaceholder(it.name)) { if (hidden++ < 40) zl::Log("hidden placeholder: %s (%d)", it.name.c_str(), id); continue; }
			out.push_back(std::move(it));
		} catch (...) {}
	}
	// Seltenste zuerst (Black Market, Exotic, Limited, Premium, Import, Very Rare, Rare, Uncommon, Common, Legacy), dann nach Namen
	auto rank = [](int q) {
		static const int r[] = {8, 7, 6, 5, 4, 1, 0, 3, 2, 9};
		return (q >= 0 && q < 10) ? r[q] : 10;
	};
	std::sort(out.begin(), out.end(), [&](const Item& a, const Item& b) {
		int ra = rank(a.quality), rb = rank(b.quality);
		return ra != rb ? ra < rb : a.name < b.name;
	});
	int np = 0;
	for (auto& it : out) { np += it.paintable; paintableById[it.id] = it.paintable; }
	zl::Log("items loaded: %d (%d paintable)", (int)out.size(), np);
	// Farbliste aus der PaintDatabase
	std::vector<PaintInfo> pl;
	if (void* clsDBp = ue::FindClass("Class TAGame.PaintDatabase_TA")) {
		void* clsPaint = ue::FindClass("Class TAGame.ProductPaint_TA");
		int offPaints = ue::PropOffset(clsDBp, "Paints");
		int offPLabel = clsPaint ? ue::PropOffset(clsPaint, "Label") : -1, offPColors = clsPaint ? ue::PropOffset(clsPaint, "Colors") : -1;
		int offPFlags = clsPaint ? ue::PropOffset(clsPaint, "bVisible") : -1; // bVisible = 1, bIncludeByDefault = 2
		auto dbsP = ue::Instances(clsDBp);
		if (!dbsP.empty() && offPaints >= 0) {
			auto* pa = ue::Ptr<ue::TArray<void*>>(dbsP[0], offPaints);
			for (int i = 1; pa->data && i < pa->count && i < 64; i++) {
				void* po = pa->data[i];
				if (!po) continue;
				try {
					PaintInfo pi{i, offPLabel >= 0 ? ue::Narrow(ue::Read<ue::FString>(po, offPLabel)) : "", {0.5f, 0.5f, 0.5f}};
					if (pi.name.empty()) continue;
					pi.flags = offPFlags >= 0 ? ue::Read<uint32_t>(po, offPFlags) : 3;
					if (offPColors >= 0) {
						const float* c = reinterpret_cast<const float*>(reinterpret_cast<uint8_t*>(po) + offPColors);
						for (int k = 0; k < 3; k++) pi.rgb[k] = powf(std::clamp(c[k], 0.0f, 1.0f), 1.0f / 2.2f);
					}
					pl.push_back(pi);
				} catch (...) {}
			}
		}
		std::string names;
		for (auto& p : pl) names += std::to_string(p.id) + "=" + p.name + "(" + std::to_string(p.flags & 3) + ") ";
		zl::Log("paints: %d %s", (int)pl.size(), names.c_str());
		// Die Lackierungen aus dem Spiel (ohne die Glow-Varianten, die es nur bei einzelnen Items gibt)
		// alle behalten: welche es pro Item gibt, steht in paintMask
	}
	// Lackierungen pro Item: normale minus Ausschluesse plus die besonderen aus den Item-Daten
	uint32_t defMask = 0;
	for (auto& p : pl)
		if ((p.flags & 3) == 3 && p.id < 32) defMask |= 1u << p.id; // normale, sichtbare Lackierungen (Crimson ... Gold)
	// Besondere (Rose Gold, White Gold, Onyx, Platinum, Glow) nur, wenn die Item-Daten sie ausdruecklich haben
	int logged = 0;
	for (auto& it : out) {
		// Alpha-Boost "Gold Rush": im Spiel nie lackiert, hier mit allen normalen Lackierungen (nur fuer dich sichtbar)
		if (it.slot == 3 && !it.paintable && it.name.find("Gold Rush") != std::string::npos) {
			it.paintable = true;
			paintableById[it.id] = true;
			zl::Log("paints unlocked for %s (%d)", it.name.c_str(), it.id);
		}
		if (!it.paintable) continue;
		it.paintMask = (defMask & ~it.paintExclude) | it.paintShop;
		paintMaskById[it.id] = it.paintMask;
		if (logged < 6 && (it.paintExclude || it.paintShop)) {
			logged++;
			zl::Log("paints for %s: mask %x (excluded %x, shop %x)", it.name.c_str(), it.paintMask, it.paintExclude, it.paintShop);
		}
	}
	{
		std::lock_guard<std::mutex> lk(mu);
		paintList = std::move(pl);
	}
	zl::Log("items loaded: %d", (int)out.size());
	{
		std::vector<int> want;
		{
			std::lock_guard<std::mutex> lk(mu);
			for (auto& [slot, id] : overrides) want.push_back(id);
			for (auto& pr : presets) for (auto& [slot, id] : pr.items) want.push_back(id);
		}
		for (int id : want) Preload(id);
	}
	std::lock_guard<std::mutex> lk(mu);
	list = std::move(out);
	loaded = !list.empty();
	status = loaded ? "" : "No items found. See zlmod.log";
}

// ---------- Loadout ----------
// Lokaler PlayerController: per PlayerTick erkannt, sonst ueber LocalPlayer.Actor bzw. Player != null
// Nur der erste lokale Spieler zaehlt (ein Splitscreen-Gast bekommt nie deine Items/Farben)
static void* PrimaryLP() {
	for (void* lp : localPlayers)
		if (lp && ue::IsValid(lp)) return lp;
	return nullptr;
}
static void* LocalPC() {
	if (void* pc = localPC.load()) {
		if (ue::IsAlive(pc)) return pc;
		localPC = nullptr; // Spieler gibt es nicht mehr (z. B. zurueck im Menue)
	}
	try {
		if (clsLocalPlayer && offLPActor >= 0)
			if (void* lp = PrimaryLP())
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
		return ue::IsAlive(pri) ? pri : nullptr;
	} catch (...) { return nullptr; }
}
static void* LocalCar() {
	void* pc = LocalPC();
	try {
		void* pawn = (pc && offPawn >= 0) ? ue::Read<void*>(pc, offPawn) : nullptr;
		return (ue::IsAlive(pawn) && ue::IsA(pawn, clsCar)) ? pawn : nullptr;
	} catch (...) { return nullptr; }
}

static void* pendingSet = nullptr;      // PRI, dem das neue Loadout noch gemeldet werden muss (sobald kein Auto faehrt)
static std::map<int, int> originals[2]; // Team -> Slot -> eigenes Item
static std::set<int> restore;           // Slots, die zurueck auf das eigene Item sollen
static bool loggedOnce = false;
static bool applying = false;

// Manche Autos (z. B. lizenzierte) erzwingen eigene Teile: mit dem Lamborghini werden deine Raeder durch die
// Lamborghini-Raeder ersetzt. Das sehen wir am Menue-Auto. Willst du so ein Teil lackieren, tragen wir genau
// dieses erzwungene Teil ein, damit die Farbe auf dem Teil landet, das wirklich am Auto ist. (mu gesperrt)
static std::map<int, std::map<int, int>> forcedBy; // Auto (Produkt-ID) -> Slot -> erzwungenes Teil
static void AddForced(std::map<int, int>& ov) {
	auto b = ov.find(0);
	if (b == ov.end()) return;
	auto f = forcedBy.find(b->second);
	if (f == forcedBy.end()) return;
	for (auto& [slot, id] : f->second)
		if (!ov.count(slot) && paints.count(slot)) ov[slot] = id;
}

static void Patch(ue::TArray<int32_t>* a, int team, const std::map<int, int>& ov) {
	if (!a || !a->data || a->count <= 0 || a->count > 64) return;
	for (auto& [slot, id] : ov) {
		if (slot >= a->count) continue;
		if (!originals[team].count(slot)) originals[team][slot] = a->data[slot];
		a->data[slot] = id;
	}
	for (int slot : restore) { // zurueck auf das eigene Item (nur Slots ohne aktuelles Item)
		if (!ov.count(slot) && slot < a->count && originals[team].count(slot)) a->data[slot] = originals[team][slot];
	}
}

// ---------- Farben im PRI ----------
// Jedes Produkt im Loadout hat eine Attribut-Liste (z. B. "Painted" mit einer Farb-ID). Wir setzen dort ein
// eigenes Painted-Attribut ein. Den Speicher fuer die Liste holen wir uns von der Engine (Text, den die Engine
// fuer uns anlegt), damit sie ihn spaeter ganz normal wieder freigeben kann.
static std::map<void*, ue::TArray<void*>> attrOrig; // Listen-Adresse -> urspruengliche Liste
static std::map<void*, void*> attrOurs;             // Listen-Adresse -> unser Speicher
static std::map<int, void*> attrCache;              // Farb-ID -> Attribut-Objekt
static std::set<std::pair<void*, int>> paintFailed; // hat nicht geklappt: nicht staendig neu versuchen

static void ResetPaintState() {
	paintFailed.clear();
	attrOrig.clear();
	attrOurs.clear();
	attrCache.clear();
}

static void* PaintAttr(int paintId) {
	auto it = attrCache.find(paintId);
	if (it != attrCache.end() && ue::IsAlive(it->second)) return it->second;
	if (!fnCreatePaint || !cdoPainted || offCreateId < 0 || offCreateRet < 0) return nullptr;
	std::vector<uint8_t> prm(size_t(ue::StructSize(fnCreatePaint)) + 16, 0);
	*reinterpret_cast<int32_t*>(prm.data() + offCreateId) = paintId;
	ue::Call(cdoPainted, fnCreatePaint, prm.data());
	void* a = *reinterpret_cast<void**>(prm.data() + offCreateRet);
	int got = (a && offPaintID >= 0) ? ue::Read<int32_t>(a, offPaintID) : -1;
	static int n = 0;
	if (n++ < 10) zl::Log("paint attribute for %d: %p (%s, PaintID %d)", paintId, a, a ? ue::Name(a).c_str() : "-", got);
	if (a) attrCache[paintId] = a;
	return a;
}

// Speicher von der Engine: Object.PathName() liefert einen neuen Text, dessen Puffer wir uebernehmen
static void* EngineBuffer(int& capPtrs) {
	capPtrs = 0;
	if (!fnPathName || !cdoObject || offPathObj < 0 || offPathRet < 0 || !clsPainted) return nullptr;
	std::vector<uint8_t> prm(size_t(ue::StructSize(fnPathName)) + 16, 0);
	*reinterpret_cast<void**>(prm.data() + offPathObj) = clsPainted;
	ue::Call(cdoObject, fnPathName, prm.data());
	auto* str = reinterpret_cast<ue::FString*>(prm.data() + offPathRet);
	if (!str->data || str->max * 2 < 8) return nullptr;
	capPtrs = (str->max * 2) / 8;
	return str->data;
}

uint32_t PaintMaskOf(int productId) {
	auto m = paintMaskById.find(productId);
	return m != paintMaskById.end() ? m->second : 0;
}

// Wunsch-Farbe fuer ein Item in einem Slot (0 = keine / gibt es fuer dieses Item nicht)
static int WantedPaint(int slot, int productId, const std::map<int, int>& pt) {
	auto p = pt.find(slot);
	if (p == pt.end() || p->second <= 0 || p->second >= 32 || productId <= 0) return 0;
	return (PaintMaskOf(productId) >> p->second) & 1 ? p->second : 0;
}

static bool SetSlotAttr(ue::TArray<void*>* elem, void* attr) {
	if (!attr) {
		auto o = attrOrig.find(elem);
		if (o != attrOrig.end()) { *elem = o->second; attrOrig.erase(o); attrOurs.erase(elem); }
		return true;
	}
	if (!attrOrig.count(elem)) attrOrig[elem] = *elem;
	void*& ours = attrOurs[elem];
	if (ours && elem->data == ours && elem->count == 1 && elem->data[0] == attr) return true;
	if (!(ours && elem->data == ours)) {
		int cap = 0;
		void* buf = EngineBuffer(cap);
		if (!buf || cap < 1) return false;
		ours = buf;
		elem->data = reinterpret_cast<void**>(buf);
		elem->max = cap;
	}
	elem->data[0] = attr;
	elem->count = 1;
	return true;
}

// Liste mit Produkt-Eintraegen (je 0x10 = eine Attribut-Liste) fuer ein Team
static void PaintList(ue::TArray<ue::TArray<void*>>* prods, ue::TArray<int32_t>* ids, const std::map<int, int>& pt, const char* what) {
	static int logged = 0;
	if (logged < 12) { logged++; zl::Log("paint: %s has %d entries", what, prods->data ? prods->count : -1); }
	if (!prods->data || prods->count <= 0 || prods->count > 64) return;
	for (int i = 0; i < kSlotCount; i++) {
		int slot = kSlots[i].index;
		if (slot >= prods->count) continue;
		int pid = (ids->data && slot < ids->count) ? ids->data[slot] : 0; // Item in diesem Slot (deins oder gewaehltes)
		int want = WantedPaint(slot, pid, pt);
		ue::TArray<void*>* elem = &prods->data[slot];
		if (!want) { SetSlotAttr(elem, nullptr); continue; }
		if (paintFailed.count({elem, want})) continue;
		void* a = PaintAttr(want);
		if (!a || !SetSlotAttr(elem, a)) {
			paintFailed.insert({elem, want});
			zl::Log("paint: couldn't set color %d on slot %d (%s)", want, slot, a ? "no memory" : "no attribute");
		}
	}
}

// Eigene (gekaufte) Items haben im PRI eine Inventar-ID (FullLoadouts[t].OnlineProducts). Mit dieser ID
// laedt das Spiel das echte Produkt aus deinem Inventar - samt dessen Attributen (also ohne unsere Farbe).
// Fuer eigene Items, die wir lackieren, nehmen wir die ID deshalb heraus (und setzen sie spaeter zurueck).
struct OnlineId { uint64_t a = 0, b = 0; bool operator==(const OnlineId& o) const { return a == o.a && b == o.b; } };
static bool PaintedId(int productId, int paintId, OnlineId& out);
struct InstId { uint64_t a = 0, b = 0; };
static int offLDOnline = -2;
static std::map<std::pair<void*, int>, InstId> instOrig; // (Listen-Element) -> urspruengliche Inventar-ID
static ue::TArray<InstId>* InstList(void* pri, int t) {
	if (offFull < 0) return nullptr;
	if (offLDOnline == -2) {
		void* st = ue::FindObject("ScriptStruct TAGame._Types_TA.LoadoutData");
		offLDOnline = st ? ue::PropOffset(st, "OnlineProducts") : -1;
		if (offLDOnline < 0) offLDOnline = 0x30;
		zl::Log("paint: inventory ids at loadout+%x", offLDOnline);
	}
	auto* l = ue::Ptr<ue::TArray<InstId>>(pri, offFull + t * 0x40 + offLDOnline);
	return (l->data && l->count > 0 && l->count <= 64) ? l : nullptr;
}
// true = alles so, wie es sein soll (check), sonst wird geschrieben
static bool OwnPaintIds(void* pri, const std::map<int, int>& ov, const std::map<int, int>& pt, bool check) {
	bool ok = true;
	for (int t = 0; t < 2; t++) {
		auto* l = InstList(pri, t);
		auto* ids = ue::Ptr<ue::TArray<int32_t>>(pri, offClient + t * 0x10);
		if (!l || !ids->data) continue;
		for (int i = 0; i < kSlotCount; i++) {
			int slot = kSlots[i].index;
			if (slot >= l->count || slot >= ids->count) continue;
			InstId* e = &l->data[slot];
			auto key = std::make_pair((void*)e, t);
			int wantPaint = ov.count(slot) ? 0 : WantedPaint(slot, ids->data[slot], pt);
			if (wantPaint) {
				// Statt der Inventar-ID deines Items die ID einer lackierten Kopie eintragen (wie die Garage es macht).
				// Nur die ID zu leeren reicht beim Auto nicht: dann bleibt es unlackiert.
				InstId target{};
				OnlineId oid;
				if (PaintedId(ids->data[slot], wantPaint, oid)) target = InstId{oid.a, oid.b};
				if (e->a != target.a || e->b != target.b) {
					if (check) { ok = false; continue; }
					if (!instOrig.count(key)) instOrig[key] = *e;
					*e = target;
					static int n = 0;
					if (n++ < 8) zl::Log("paint: your own item %d in slot %d -> painted copy (paint %d, id %llx:%llx)", ids->data[slot], slot, wantPaint,
						(unsigned long long)target.a, (unsigned long long)target.b);
				}
			} else {
				auto o = instOrig.find(key);
				if (o != instOrig.end()) {
					if (check) { ok = false; continue; }
					*e = o->second;
					instOrig.erase(o);
				}
			}
		}
	}
	return ok;
}

static void WritePaints(void* pri) {
	if (!attrArraysOk || offClientOnline < 0) return;
	std::map<int, int> ov, pt;
	{
		std::lock_guard<std::mutex> lk(mu);
		ov = overrides;
		AddForced(ov); // vom Auto erzwungene Teile, die du lackieren willst
		pt = paints;
	}
	// Alle Stellen, an denen das Spiel Item-Attribute fuer den Spieler ablegt
	(void)ov;
	auto ids = [&](int t) { return ue::Ptr<ue::TArray<int32_t>>(pri, offClient + t * 0x10); };
	if (offClientOnline1 >= 0) PaintList(ue::Ptr<ue::TArray<ue::TArray<void*>>>(pri, offClientOnline1), ids(0), pt, "ClientLoadoutOnline");
	for (int t = 0; t < 2; t++) {
		PaintList(ue::Ptr<ue::TArray<ue::TArray<void*>>>(pri, offClientOnline + t * 0x10), ids(t), pt, "ClientLoadoutsOnline");
		if (offFullOnline >= 0) PaintList(ue::Ptr<ue::TArray<ue::TArray<void*>>>(pri, offFullOnline + t * 0x10), ids(t), pt, "FullOnlineLoadout");
		if (offFullAttr >= 0) PaintList(ue::Ptr<ue::TArray<ue::TArray<void*>>>(pri, offFullAttr + t * 0x10), ids(t), pt, "FullLoadoutAttributes");
	}
	OwnPaintIds(pri, ov, pt, false);
}

// Stimmen die Farben im PRI?
static bool PaintsOk(void* pri) {
	if (!attrArraysOk || offClientOnline < 0) return true;
	std::map<int, int> ov, pt;
	{
		std::lock_guard<std::mutex> lk(mu);
		ov = overrides;
		AddForced(ov); // vom Auto erzwungene Teile, die du lackieren willst
		pt = paints;
	}
	auto* prods = ue::Ptr<ue::TArray<ue::TArray<void*>>>(pri, offClientOnline);
	if (!prods->data || prods->count <= 0 || prods->count > 64) return true;
	for (int i = 0; i < kSlotCount; i++) {
		int slot = kSlots[i].index;
		if (slot >= prods->count) continue;
		ue::TArray<void*>* elem = &prods->data[slot];
		auto* idl = ue::Ptr<ue::TArray<int32_t>>(pri, offClient);
		int want = WantedPaint(slot, (idl->data && slot < idl->count) ? idl->data[slot] : 0, pt);
		auto ours = attrOurs.find(elem);
		bool isOurs = ours != attrOurs.end() && elem->data == ours->second;
		if (want && paintFailed.count({elem, want})) continue;
		if (want && !(isOurs && elem->count == 1 && offPaintID >= 0 && elem->data[0] && ue::Read<int32_t>(elem->data[0], offPaintID) == want)) return false;
		if (!want && attrOrig.count(elem)) return false;
	}
	if (!OwnPaintIds(pri, ov, pt, true)) return false;
	return true;
}

static void WriteOverrides(void* pri) {
	static void* origPRI = nullptr;
	if (pri != origPRI) { // neuer Spieler/neues Spiel: eigene Items neu merken
		originals[0].clear();
		originals[1].clear();
		ResetPaintState();
		instOrig.clear();
		origPRI = pri;
	}
	std::map<int, int> ov;
	{
		std::lock_guard<std::mutex> lk(mu);
		ov = overrides;
		AddForced(ov); // vom Auto erzwungene Teile, die du lackieren willst
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
	try { WritePaints(pri); } catch (...) { zl::Log("ERROR: paint crashed (caught)"); }
}

void ApplyNow() {
	if (!Setup()) return;
	void* pc = LocalPC();
	void* pri = LocalPRI();
	if (!LocalCar()) { zl::Log("apply: no car right now - items come with the next spawn"); return; } // nicht im Spiel
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
		verifyTries = 0;
		verifyAt = GetTickCount64() + 800; // am neuen Auto nachsehen (Items + Hitbox ins Log)
	} else {
		// Kein Freeplay (Match, Training): ein fahrendes Auto nie umbauen - sonst passen Modell und Hitbox/Physik
		// nicht mehr zusammen. Das Loadout steht schon drin; das Spiel baut das Auto beim naechsten Spawn
		// (nach Tor/Reset) ganz normal damit auf. Bekannt gegeben wird es erst, wenn gerade kein Auto faehrt.
		pendingSet = pri;
		zl::Log("apply: not in freeplay (%s) - items come with the next spawn", ge ? ue::Name(ge).c_str() : "-");
	}
	zl::Log("apply: done for PRI %p", pri);
}

static void PatchParams(void* params) {
	std::map<int, int> ov;
	{
		std::lock_guard<std::mutex> lk(mu);
		ov = overrides;
		AddForced(ov); // vom Auto erzwungene Teile, die du lackieren willst
	}
	auto* a = reinterpret_cast<ue::TArray<int32_t>*>(params);
	if (!a || !a->data || a->count <= 0 || a->count > 64) return;
	for (auto& [slot, id] : ov)
		if (slot < a->count) a->data[slot] = id;
}

// Gehoert der PRI dem lokalen Spieler? (Controller des PRI hat einen LocalPlayer)
static bool IsLocalPRI(void* pri) {
	try {
		if (void* lp = PrimaryLP()) {
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
		AddForced(ov); // vom Auto erzwungene Teile, die du lackieren willst
	}
	try { if (!PaintsOk(pri)) return true; } catch (...) {}
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

// Laufend (alle 40 ms) pruefen, ob das Loadout des lokalen Spielers unsere Items hat. So stehen sie schon
// drin, bevor das Auto beim Laden ins Freeplay/Match gebaut wird (kein kurzes Aufblitzen der eigenen Items).
static void EnsureLocalLoadout() {
	static void* lastPRI = nullptr;
	static int lastCount = -1;
	for (void* lp : localPlayers) {
		if (lp != PrimaryLP()) continue; // nur Spieler 1
		void* pc = ue::Read<void*>(lp, offLPActor);
		if (!pc || !ue::IsAlive(pc)) continue;
		void* pri = ue::Read<void*>(pc, offPRI);
		if (!pri || !ue::IsAlive(pri) || !ue::IsA(pri, clsPRI)) continue;
		auto* cl = ue::Ptr<ue::TArray<int32_t>>(pri, offClient);
		if (pri != lastPRI || cl->count != lastCount) {
			zl::Log("local PRI %p (%s): loadout entries %d", pri, ue::Name(pri).c_str(), cl->count);
			lastPRI = pri;
			lastCount = cl->count;
		}
		if (!cl->data || cl->count <= 0 || cl->count > 64) continue; // Hauptmenue: kein Loadout
		if (!NeedsApply(pri)) continue;
		WriteOverrides(pri);
		writtenPRI = pri;
		static int n = 0;
		if (n++ < 10) zl::Log("loadout of %s ready -> items written", ue::Name(pri).c_str());
	}
}

// Fehlt dem Auto eines unserer Items (Auto, Raeder, Boost)? 1 = ja, 0 = nein, -1 = (noch) nicht pruefbar
static int CarMissesItems(void* car) {
	if (offCarLoadout < 0 || !fnAssetById || offAIdId < 0 || offAIdRet < 0) return -1;
	void* ld = ue::Read<void*>(car, offCarLoadout);
	if (!ld || !ue::IsAlive(ld)) return -1;
	if (offLoaderRemaining >= 0 && ue::Ptr<ue::TArray<void*>>(ld, offLoaderRemaining)->count > 0) return -1; // laedt noch
	std::map<int, int> ov;
	{
		std::lock_guard<std::mutex> lk(mu);
		ov = overrides;
		AddForced(ov); // vom Auto erzwungene Teile, die du lackieren willst
	}
	for (int slot : {0, 2, 3}) {
		auto it = ov.find(slot);
		if (it == ov.end()) continue;
		std::vector<uint8_t> p(size_t(ue::StructSize(fnAssetById)) + 16, 0);
		*reinterpret_cast<int32_t*>(p.data() + offAIdId) = it->second;
		ue::Call(ld, fnAssetById, p.data());
		if (!*reinterpret_cast<void**>(p.data() + offAIdRet)) {
			zl::Log("verify: car %s has no item %d in slot %d", ue::Name(car).c_str(), it->second, slot);
			return 1;
		}
	}
	return 0;
}

// Kontrolle fuers Log: alles rund um Kollision/Hitbox des fertigen Autos (zeigt, dass das Spiel die Hitbox
// des getauschten Autos benutzt - das Auto wird ganz normal aus dem Loadout gebaut, nichts wird umgebogen)
static std::string LowerS(std::string v);
static void LogHitbox(void* car, int body) {
	static int n = 0;
	if (n >= 12 || !car) return;
	n++;
	std::string out;
	for (void* st = ue::ClassOf(car); st && out.size() < 1200; st = ue::Read<void*>(st, ue::off::StructSuper))
		for (void* f = ue::Read<void*>(st, ue::off::StructChildren); f && out.size() < 1200; f = ue::Read<void*>(f, ue::off::FieldNext)) {
			std::string nm = ue::Name(f), lo = LowerS(nm), cn = ue::Name(ue::ClassOf(f));
			if (lo.find("hitbox") == std::string::npos && lo.find("collision") == std::string::npos && lo.find("extent") == std::string::npos) continue;
			int o = ue::Read<int32_t>(f, ue::off::PropOffset), sz = ue::Read<int32_t>(f, ue::off::PropElementSize);
			char buf[160] = {};
			if (cn == "FloatProperty") snprintf(buf, sizeof buf, "%s=%.1f ", nm.c_str(), ue::Read<float>(car, o));
			else if (cn == "StructProperty" && sz == 12) {
				const float* v = ue::Ptr<float>(car, o);
				snprintf(buf, sizeof buf, "%s=(%.1f %.1f %.1f) ", nm.c_str(), v[0], v[1], v[2]);
			} else if (cn == "ObjectProperty") {
				void* ob = ue::Read<void*>(car, o);
				snprintf(buf, sizeof buf, "%s=%s ", nm.c_str(), ob && ue::IsValid(ob) ? ue::Name(ob).c_str() : "-");
			} else continue;
			out += buf;
		}
	zl::Log("hitbox: %s body %d: %s", ue::Name(car).c_str(), body, out.empty() ? "(no collision values found)" : out.c_str());
}

// ---------- Lackierter Gold Rush ----------
// Der Alpha-Boost "Gold Rush" hat im Spiel keine Lackierungen: Partikel und Materialien ignorieren das Lack-Attribut.
// Deshalb faerbt der Mod alles aus seinem Paket (Boost_AlphaReward) selbst um: Farben der Partikel-Module
// und die Farb-Parameter seiner Materialien (Flamme, Funken und der Schweif ohne Boost). Nur auf deinem PC sichtbar,
// beim Abwaehlen kommen die Originalwerte zurueck. Funktioniert im Hauptmenue (Garage) und im Spiel.
static const int kGoldRush = 32;
struct FloatPatch { void* owner; float* p; float orig; };
static std::unordered_map<float*, FloatPatch> grPatches; // Originalwerte der Partikel-Farben
struct MicPatch { void* mic; ue::FName name; float orig[4]; };
static std::vector<MicPatch> grMicPatches;                // Originalwerte der Material-Farben
static std::vector<void*> grTemplates, grMics;            // gefundene Partikel-Vorlagen und Materialien
static std::string grPkg;
static int grApplied = 0;                                 // aktuell angewendete Lackierung (0 = original)
static unsigned long long grNext = 0, grScanAt = 0;
static bool grDumped = false, grModLogged = false;
static std::vector<std::string> grParams; // Laufzeit-Parameter der Partikel (z. B. "CoreColor")
static std::map<std::string, int32_t> grParamIdx;
static float grTarget[3] = {1, 1, 1};
static bool grCoreOn = false;

static std::string LowerS(std::string v) { for (auto& c : v) c = (char)tolower((unsigned char)c); return v; }
static std::string Outermost(void* o) {
	void* last = o;
	for (int i = 0; i < 16 && o; i++) { last = o; o = ue::OuterOf(o); }
	return ue::Name(last);
}

// Farbe aendern: Helligkeit des Originals bleibt, Farbton kommt von der Lackierung
// Lackfarbe: Farbton (hoechster Anteil = 1) und Helligkeit der Lackierung
static float grTn[3] = {1, 1, 1}, grV = 1.0f;
static std::string grPaintName;

// Umfaerben wie beim Original: jede Farbe = weisser Anteil (heisser Kern, bleibt weiss) + Farbanteil (Gold -> Lackfarbe).
// So bleiben Verlauf, Glanz und Leuchten des Originals erhalten, nur der Farbton wechselt.
static void Recolor(const float* o, float* out) {
	float mx = std::max({o[0], o[1], o[2]}), mn = std::min({o[0], o[1], o[2]});
	// kraeftig statt verwaschen: nur ein kleiner Teil bleibt weiss, der Rest bekommt die volle Lackfarbe
	float white = mn * 0.12f;
	for (int k = 0; k < 3; k++) out[k] = (white + (mx - white) * grTn[k]) * grV;
}

static void Tint(float* rgb, const float* target, bool grayToo) {
	(void)target;
	float mx = std::max({rgb[0], rgb[1], rgb[2]}), mn = std::min({rgb[0], rgb[1], rgb[2]});
	if (mx <= 0.0001f) return;
	bool colored = (mx - mn) > 0.05f * mx;
	if (!grayToo) { // Helligkeits-Verlauf (Skalierung): goldenen Stich entfernen, Helligkeit behalten
		if (colored) rgb[0] = rgb[1] = rgb[2] = mx;
		return;
	}
	float o[3] = {rgb[0], rgb[1], rgb[2]};
	Recolor(o, rgb);
}

static void PatchTriples(void* owner, float* f, int n, const float* target, bool grayToo, int& changed) {
	for (int i = 0; i + 2 < n; i += 3) {
		for (int k = 0; k < 3; k++) {
			float* q = f + i + k;
			auto it = grPatches.find(q);
			if (it != grPatches.end()) *q = it->second.orig; // immer vom Original aus
			else grPatches[q] = {owner, q, *q};
		}
		float before[3] = {f[i], f[i + 1], f[i + 2]};
		Tint(f + i, target, grayToo);
		if (before[0] != f[i] || before[1] != f[i + 1] || before[2] != f[i + 2]) changed++;
	}
}


// Alle Farb-Module einer Vorlage umfaerben (target = lineare Farbe 0..1)
static int TintTemplate(void* ps, const float* target) {
	static void *clsEm = ue::FindClass("Class Engine.ParticleEmitter"), *clsLod = ue::FindClass("Class Engine.ParticleLODLevel");
	static void *mColor = ue::FindClass("Class Engine.ParticleModuleColor"), *mCol = ue::FindClass("Class Engine.ParticleModuleColorOverLife"),
				*mScale = ue::FindClass("Class Engine.ParticleModuleColorScaleOverLife");
	static void* raw = ue::FindObject("ScriptStruct Core.DistributionVector.RawDistributionVector");
	static int oTable = raw ? ue::PropOffset(raw, "LookupTable") : 8, oChunk = raw ? ue::PropOffset(raw, "LookupTableChunkSize") : 3,
			   oDist = raw ? ue::PropOffset(raw, "Distribution") : 32;
	static void* curvePt = ue::FindObject("ScriptStruct Core.Object.InterpCurvePointVector");
	static int curveStride = curvePt ? ue::StructSize(curvePt) : 44, curveOut = curvePt ? ue::PropOffset(curvePt, "OutVal") : 4;
	static void* clsPS = ue::FindClass("Class Engine.ParticleSystem");
	int oEmitters = ue::PropOffset(clsPS, "Emitters");
	int oLods = clsEm ? ue::PropOffset(clsEm, "LODLevels") : -1, oMods = clsLod ? ue::PropOffset(clsLod, "Modules") : -1;
	if (oEmitters < 0 || oLods < 0 || oMods < 0) return 0;
	int changed = 0;
	auto* ems = ue::Ptr<ue::TArray<void*>>(ps, oEmitters);
	for (int e = 0; ems->data && e < ems->count && e < 64; e++) {
		void* em = ems->data[e];
		if (!em) continue;
		auto* lods = ue::Ptr<ue::TArray<void*>>(em, oLods);
		for (int l = 0; lods->data && l < lods->count && l < 8; l++) {
			void* lod = lods->data[l];
			if (!lod) continue;
			auto* mods = ue::Ptr<ue::TArray<void*>>(lod, oMods);
			for (int m = 0; mods->data && m < mods->count && m < 64; m++) {
				void* mod = mods->data[m];
				if (!mod) continue;
				const char* prop = nullptr;
				bool grayToo = true;
				if (!grModLogged && l == 0) {
					std::string extra;
					// Partikel-Parameter (vom Spiel pro Boost setzbar) und Materialien der Module
					for (const char* pn : {"ColorParam", "ParameterName", "MeshMaterials", "Mesh"}) {
						int po = ue::PropOffset(ue::ClassOf(mod), pn);
						if (po >= 0) extra += std::string(" ") + pn;
					}
					zl::Log("gold rush:   emitter %d module %s%s", e, ue::FullName(mod).c_str(), extra.c_str());
				}
				if (mCol && ue::IsA(mod, mCol)) prop = "ColorOverLife";
				else if (mScale && ue::IsA(mod, mScale)) { prop = "ColorScaleOverLife"; grayToo = false; }
				else if (mColor && ue::IsA(mod, mColor)) prop = "StartColor";
				if (!prop) continue;
				int o = ue::PropOffset(ue::ClassOf(mod), prop);
				if (o < 0) continue;
				uint8_t* rd = reinterpret_cast<uint8_t*>(mod) + o;
				int before = changed;
				// 1) vorberechnete Tabelle (wird im fertigen Spiel benutzt): [Min, Max, dann Werte]
				auto* tab = reinterpret_cast<ue::TArray<float>*>(rd + oTable);
				uint8_t chunk = rd[oChunk];
				if (tab->data && tab->count > 2 && tab->count < 4096 && (chunk == 3 || chunk == 6))
					PatchTriples(ps, tab->data + 2, tab->count - 2, target, grayToo, changed);
				// 2) die Verteilung selbst (falls ohne Tabelle)
				void* dist = *reinterpret_cast<void**>(rd + oDist);
				if (dist && ue::IsValid(dist)) {
					void* dc = ue::ClassOf(dist);
					std::string dn = ue::Name(dc);
					if (dn == "DistributionVectorParticleParameter") {
						// Farbe kommt zur Laufzeit vom Boost (z. B. "CoreColor"): Name merken, Ausgabebereich und Ersatzwert umfaerben
						int pn = ue::PropOffset(dc, "ParameterName");
						if (pn >= 0) {
							int32_t idx = ue::Read<int32_t>(dist, pn);
							std::string nm = ue::NameOf(idx);
							if (std::find(grParams.begin(), grParams.end(), nm) == grParams.end()) grParams.push_back(nm);
							grParamIdx[nm] = idx;
						}
						for (const char* nm : {"Constant", "MinOutput", "MaxOutput"}) {
							int oc = ue::PropOffset(dc, nm);
							if (oc >= 0) PatchTriples(ps, reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(dist) + oc), 3, target, true, changed);
						}
						if (!grModLogged) {
							auto V = [&](const char* nm) {
								int oc = ue::PropOffset(dc, nm);
								char b[64] = "-";
								if (oc >= 0) {
									const float* f = reinterpret_cast<const float*>(reinterpret_cast<uint8_t*>(dist) + oc);
									snprintf(b, sizeof b, "(%.2f %.2f %.2f)", f[0], f[1], f[2]);
								}
								return std::string(nm) + "=" + b;
							};
							int pm = ue::PropOffset(dc, "ParamModes");
							zl::Log("gold rush:     param dist %s %s %s %s %s modes=%d/%d/%d", V("MinInput").c_str(), V("MaxInput").c_str(), V("MinOutput").c_str(),
								V("MaxOutput").c_str(), V("Constant").c_str(), pm >= 0 ? ue::Read<uint8_t>(dist, pm) : -1, pm >= 0 ? ue::Read<uint8_t>(dist, pm + 1) : -1,
								pm >= 0 ? ue::Read<uint8_t>(dist, pm + 2) : -1);
						}
					} else if (dn == "DistributionVectorConstant") {
						int oc = ue::PropOffset(dc, "Constant");
						if (oc >= 0) PatchTriples(ps, reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(dist) + oc), 3, target, grayToo, changed);
					} else if (dn == "DistributionVectorUniform") {
						for (const char* nm : {"Min", "Max"}) {
							int oc = ue::PropOffset(dc, nm);
							if (oc >= 0) PatchTriples(ps, reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(dist) + oc), 3, target, grayToo, changed);
						}
					} else if (dn == "DistributionVectorConstantCurve") {
						int oc = ue::PropOffset(dc, "ConstantCurve");
						if (oc >= 0) {
							auto* pts = reinterpret_cast<ue::TArray<uint8_t>*>(reinterpret_cast<uint8_t*>(dist) + oc);
							for (int i = 0; pts->data && i < pts->count && i < 256; i++)
								PatchTriples(ps, reinterpret_cast<float*>(pts->data + i * curveStride + curveOut), 3, target, grayToo, changed);
						}
					}
				}
				if (!grModLogged) {
					std::string vals;
					if (tab->data && tab->count > 4) {
						char b[96];
						snprintf(b, sizeof b, " now=(%.2f %.2f %.2f)", tab->data[2], tab->data[3], tab->data[4]);
						vals = b;
					}
					std::string dname;
					if (dist && ue::IsValid(dist)) {
						dname = ue::Name(ue::ClassOf(dist));
						int pn = ue::PropOffset(ue::ClassOf(dist), "ParameterName");
						if (pn >= 0) dname += " param '" + ue::NameOf(ue::Read<int32_t>(dist, pn)) + "'";
					}
					zl::Log("gold rush:   emitter %d lod %d %s.%s table=%d chunk=%d dist=%s -> %d colors changed%s", e, l, ue::Name(ue::ClassOf(mod)).c_str(), prop,
						tab->data ? tab->count : 0, chunk, dname.empty() ? "-" : dname.c_str(), changed - before, vals.c_str());
				}
			}
		}
	}
	return changed;
}


// ----- Materialien (MaterialInstanceConstant): Farb-Parameter -----
static void* clsMIC = nullptr;
static void* fnSetVec = nullptr;
static int oVecParams = -1, vpStride = 40, vpName = 0, vpValue = 8, oSetName = -1, oSetValue = -1, oMicParent = -1, oTexParams = -1, tpStride = 32;

static bool MicSetup() {
	if (fnSetVec) return true;
	clsMIC = ue::FindClass("Class Engine.MaterialInstanceConstant");
	if (!clsMIC) return false;
	oVecParams = ue::PropOffset(clsMIC, "VectorParameterValues");
	oTexParams = ue::PropOffset(clsMIC, "TextureParameterValues");
	void* clsMI = ue::FindClass("Class Engine.MaterialInstance");
	oMicParent = clsMI ? ue::PropOffset(clsMI, "Parent") : -1;
	if (void* st = ue::FindObject("ScriptStruct Engine.MaterialInstanceConstant.VectorParameterValue")) {
		vpStride = ue::StructSize(st);
		vpName = ue::PropOffset(st, "ParameterName");
		vpValue = ue::PropOffset(st, "ParameterValue");
	}
	if (void* st = ue::FindObject("ScriptStruct Engine.MaterialInstanceConstant.TextureParameterValue")) tpStride = ue::StructSize(st);
	fnSetVec = ue::FindFunction(clsMIC, "SetVectorParameterValue");
	if (fnSetVec) { oSetName = ue::PropOffset(fnSetVec, "ParameterName"); oSetValue = ue::PropOffset(fnSetVec, "Value"); }
	zl::Log("gold rush: mic vec=%x stride=%d name=%d value=%d set=%p (%d/%d)", oVecParams, vpStride, vpName, vpValue, fnSetVec, oSetName, oSetValue);
	return fnSetVec && oVecParams >= 0 && oSetName >= 0 && oSetValue >= 0 && vpName >= 0 && vpValue >= 0;
}

static void MicSet(void* mic, ue::FName n, const float* c) {
	std::vector<uint8_t> prm(size_t(ue::StructSize(fnSetVec)) + 16, 0);
	*reinterpret_cast<ue::FName*>(prm.data() + oSetName) = n;
	memcpy(prm.data() + oSetValue, c, 16);
	ue::Call(mic, fnSetVec, prm.data());
}

static void TintMic(void* mic, const float* target, int& changed) {
	auto* arr = ue::Ptr<ue::TArray<uint8_t>>(mic, oVecParams);
	std::vector<std::pair<ue::FName, std::array<float, 4>>> todo;
	for (int i = 0; arr->data && i < arr->count && i < 64; i++) {
		uint8_t* e = arr->data + size_t(i) * vpStride;
		ue::FName n = *reinterpret_cast<ue::FName*>(e + vpName);
		const float* v = reinterpret_cast<const float*>(e + vpValue);
		// Original merken (nur beim ersten Mal)
		const float* orig = nullptr;
		for (auto& mp : grMicPatches)
			if (mp.mic == mic && mp.name.index == n.index && mp.name.number == n.number) { orig = mp.orig; break; }
		if (!orig) {
			grMicPatches.push_back({mic, n, {v[0], v[1], v[2], v[3]}});
			orig = grMicPatches.back().orig;
		}
		std::string nm = LowerS(ue::NameOf(n.index));
		float mx = std::max({orig[0], orig[1], orig[2]}), mn = std::min({orig[0], orig[1], orig[2]});
		bool colored = mx > 0.0001f && (mx - mn) > 0.12f * mx;
		bool colorName = nm.find("color") != std::string::npos || nm.find("colour") != std::string::npos || nm.find("tint") != std::string::npos ||
			nm.find("emis") != std::string::npos || nm.find("glow") != std::string::npos;
		if (!colored && !colorName) continue;
		if (mx <= 0.0001f) mx = 1.0f;
		float rc[3];
		Recolor(orig, rc);
		todo.push_back({n, {rc[0], rc[1], rc[2], orig[3]}});
	}
	for (auto& [n, c] : todo) { MicSet(mic, n, c.data()); changed++; }
}

static void RestoreGoldRush() {
	for (auto& [q, pt] : grPatches)
		if (ue::IsValid(pt.owner)) *pt.p = pt.orig;
	grPatches.clear();
	if (fnSetVec)
		for (auto& mp : grMicPatches)
			if (ue::IsValid(mp.mic)) MicSet(mp.mic, mp.name, mp.orig);
	grMicPatches.clear();
}

// Materialien, die die Partikel-Emitter benutzen (auch ausserhalb des Pakets)
static void CollectTemplateMics(void* ps, std::vector<void*>& out) {
	static void *clsEm = ue::FindClass("Class Engine.ParticleEmitter"), *clsLod = ue::FindClass("Class Engine.ParticleLODLevel");
	static void* clsReq = ue::FindClass("Class Engine.ParticleModuleRequired");
	static void* clsMeshMat = ue::FindClass("Class Engine.ParticleModuleMeshMaterial");
	static void* clsPS = ue::FindClass("Class Engine.ParticleSystem");
	int oEmitters = ue::PropOffset(clsPS, "Emitters");
	int oLods = clsEm ? ue::PropOffset(clsEm, "LODLevels") : -1, oMods = clsLod ? ue::PropOffset(clsLod, "Modules") : -1;
	int oReq = clsLod ? ue::PropOffset(clsLod, "RequiredModule") : -1, oMat = clsReq ? ue::PropOffset(clsReq, "Material") : -1;
	int oMeshMats = clsMeshMat ? ue::PropOffset(clsMeshMat, "MeshMaterials") : -1;
	if (oEmitters < 0 || oLods < 0) return;
	auto add = [&](void* m) {
		for (int d = 0; d < 4 && m && ue::IsValid(m); d++) { // MIC und ihre Eltern-MICs
			if (!ue::IsA(m, clsMIC)) break;
			if (std::find(out.begin(), out.end(), m) == out.end()) out.push_back(m);
			m = oMicParent >= 0 ? ue::Read<void*>(m, oMicParent) : nullptr;
		}
	};
	auto* ems = ue::Ptr<ue::TArray<void*>>(ps, oEmitters);
	for (int e = 0; ems->data && e < ems->count && e < 64; e++) {
		void* em = ems->data[e];
		if (!em) continue;
		auto* lods = ue::Ptr<ue::TArray<void*>>(em, oLods);
		for (int l = 0; lods->data && l < lods->count && l < 8; l++) {
			void* lod = lods->data[l];
			if (!lod) continue;
			if (oReq >= 0 && oMat >= 0)
				if (void* rq = ue::Read<void*>(lod, oReq)) {
					void* mat = ue::Read<void*>(rq, oMat);
					if (!grDumped && l == 0) {
						std::string chain;
						for (void* mm = mat; mm && chain.size() < 300; mm = (oMicParent >= 0 && ue::IsA(mm, clsMIC)) ? ue::Read<void*>(mm, oMicParent) : nullptr)
							chain += " -> " + ue::FullName(mm);
						zl::Log("gold rush:   %s emitter %d material%s", ue::Name(ps).c_str(), e, chain.empty() ? " none" : chain.c_str());
					}
					add(mat);
				}
			if (oMods >= 0 && oMeshMats >= 0) {
				auto* mods = ue::Ptr<ue::TArray<void*>>(lod, oMods);
				for (int m = 0; mods->data && m < mods->count && m < 64; m++) {
					void* mod = mods->data[m];
					if (!mod || !ue::IsA(mod, clsMeshMat)) continue;
					auto* mm = ue::Ptr<ue::TArray<void*>>(mod, oMeshMats);
					for (int k = 0; mm->data && k < mm->count && k < 16; k++) add(mm->data[k]);
				}
			}
		}
	}
}

// Einmalig ins Log: was steckt im Paket des Boosts (zum Nachbessern)
static void DumpGoldRush(const std::vector<void*>& all) {
	if (grDumped || grTemplates.empty()) return;
	std::map<std::string, int> byClass;
	for (void* o : all) byClass[ue::Name(ue::ClassOf(o))]++;
	std::string cls;
	for (auto& [c, n] : byClass) cls += c + "=" + std::to_string(n) + " ";
	zl::Log("gold rush: package %s has %d objects: %s", grPkg.c_str(), (int)all.size(), cls.c_str());
	for (void* t : grTemplates) zl::Log("gold rush: fx %s", ue::FullName(t).c_str());
	int lines = 0;
	for (void* m : grMics) {
		std::string params;
		auto* arr = ue::Ptr<ue::TArray<uint8_t>>(m, oVecParams);
		for (int i = 0; arr->data && i < arr->count && i < 24; i++) {
			uint8_t* e = arr->data + size_t(i) * vpStride;
			const float* v = reinterpret_cast<const float*>(e + vpValue);
			char b[128];
			snprintf(b, sizeof b, "%s=(%.2f %.2f %.2f %.2f) ", ue::NameOf(reinterpret_cast<ue::FName*>(e + vpName)->index).c_str(), v[0], v[1], v[2], v[3]);
			params += b;
		}
		int tex = oTexParams >= 0 ? ue::Ptr<ue::TArray<uint8_t>>(m, oTexParams)->count : 0;
		void* par = oMicParent >= 0 ? ue::Read<void*>(m, oMicParent) : nullptr;
		if (lines++ < 60)
			zl::Log("gold rush: mic %s parent=%s textures=%d vectors: %s", ue::FullName(m).c_str(), par ? ue::Name(par).c_str() : "-", tex, params.c_str());
	}
	for (void* o : all) {
		std::string c = ue::Name(ue::ClassOf(o));
		if (c == "Material" || c.find("Texture") != std::string::npos || c.find("StaticMesh") != std::string::npos || c.find("FXActor") != std::string::npos)
			if (lines++ < 120) zl::Log("gold rush: %s", ue::FullName(o).c_str());
	}
}

// Welche Partikel zeigt das Spiel gerade am Boost? (einmal pro Spielstart ins Log)
static bool FindGoldRushFx() {
	grTemplates.clear();
	grMics.clear();
	void* clsPS = ue::FindClass("Class Engine.ParticleSystem");
	if (!clsPS || !MicSetup()) { if (!clsPS) zl::Log("gold rush: ParticleSystem class missing"); }
	if (grPkg.empty()) {
		auto dbs = ue::Instances(clsDB);
		if (!dbs.empty()) {
			auto* arr = ue::Ptr<ue::TArray<void*>>(dbs[0], offProducts);
			void* prod = (arr->data && kGoldRush < arr->count) ? arr->data[kGoldRush] : nullptr;
			int offPkg = ue::PropOffset(clsProduct, "AssetPackageName");
			if (prod && offPkg >= 0) grPkg = ue::NameOf(ue::Read<int32_t>(prod, offPkg));
		}
		if (grPkg.empty()) grPkg = "Boost_AlphaReward";
	}
	std::string lp = LowerS(grPkg);
	// erst das Paket selbst finden (Objekt ohne Outer), dann nur Zeiger vergleichen - schnell
	std::vector<void*> pkgs;
	for (int i = 0, n = ue::ObjectCount(); i < n; i++) {
		void* o = ue::ObjectAt(i);
		if (o && !ue::OuterOf(o) && LowerS(ue::Name(o)) == lp) pkgs.push_back(o);
	}
	std::vector<void*> all;
	if (pkgs.empty()) { static int miss = 0; if (miss++ < 3) zl::Log("gold rush: package %s not loaded yet", grPkg.c_str()); return false; }
	for (int i = 0, n = ue::ObjectCount(); i < n; i++) {
		void* o = ue::ObjectAt(i);
		if (!o) continue;
		try {
			void* top = o;
			for (int d = 0; d < 16; d++) { void* up = ue::OuterOf(top); if (!up) break; top = up; }
			if (top == o || std::find(pkgs.begin(), pkgs.end(), top) == pkgs.end()) continue;
			if (ue::Name(o).rfind("Default__", 0) == 0) continue;
			all.push_back(o);
			if (clsPS && ue::IsA(o, clsPS)) grTemplates.push_back(o);
			else if (clsMIC && ue::IsA(o, clsMIC)) grMics.push_back(o);
		} catch (...) {}
	}
	if (clsMIC) for (void* t : grTemplates) CollectTemplateMics(t, grMics);
	DumpGoldRush(all);
	if (!grTemplates.empty()) grDumped = true;
	return !all.empty();
}

// ----- Laufzeit-Farbe der Partikel ("CoreColor"): der Boost setzt sie selbst an seinen Partikel-Komponenten.
// So kommt das Gold zustande - hier wird sie laufend mit der Lackfarbe ueberschrieben (so macht es auch AlphaConsole).
static void *clsPSC = nullptr, *fnPscSetVec = nullptr, *fnPscSetCol = nullptr;
static int oPscTpl = -1, oPscInst = -1, spStride = 0, spName = -1, spType = -1, spVector = -1, spColor = -1;
static int oSvName = -1, oSvVal = -1, oScName = -1, oScVal = -1;
static std::vector<void*> grComps;
static std::map<std::string, std::array<float, 3>> grParamOrig; // Originalwert je Parameter (vor dem Ueberschreiben)
static unsigned long long grCompsAt = 0;

static bool PscSetup() {
	if (clsPSC) return oPscTpl >= 0;
	clsPSC = ue::FindClass("Class Engine.ParticleSystemComponent");
	if (!clsPSC) return false;
	oPscTpl = ue::PropOffset(clsPSC, "Template");
	oPscInst = ue::PropOffset(clsPSC, "InstanceParameters");
	if (void* st = ue::FindObject("ScriptStruct Engine.ParticleSystemComponent.ParticleSysParam")) {
		spStride = ue::StructSize(st);
		spName = ue::PropOffset(st, "Name");
		spType = ue::PropOffset(st, "ParamType");
		spVector = ue::PropOffset(st, "Vector");
		spColor = ue::PropOffset(st, "Color");
	}
	fnPscSetVec = ue::FindFunction(clsPSC, "SetVectorParameter");
	fnPscSetCol = ue::FindFunction(clsPSC, "SetColorParameter");
	if (fnPscSetVec) { oSvName = ue::PropOffset(fnPscSetVec, "ParameterName"); oSvVal = ue::PropOffset(fnPscSetVec, "Param"); }
	if (fnPscSetCol) { oScName = ue::PropOffset(fnPscSetCol, "ParameterName"); oScVal = ue::PropOffset(fnPscSetCol, "Param"); }
	zl::Log("gold rush: psc tpl=%x inst=%x param stride=%d name=%d type=%d vector=%d color=%d setVec=%p(%d/%d) setCol=%p(%d/%d)", oPscTpl, oPscInst, spStride,
		spName, spType, spVector, spColor, fnPscSetVec, oSvName, oSvVal, fnPscSetCol, oScName, oScVal);
	return oPscTpl >= 0;
}

// Wert eines Instanz-Parameters lesen (Vektor oder Farbe), false = nicht gesetzt
static bool PscGetParam(void* c, const std::string& name, float* out, int& type) {
	if (oPscInst < 0 || spStride <= 0 || spName < 0) return false;
	auto* arr = ue::Ptr<ue::TArray<uint8_t>>(c, oPscInst);
	for (int i = 0; arr->data && i < arr->count && i < 64; i++) {
		uint8_t* e = arr->data + size_t(i) * spStride;
		if (ue::NameOf(reinterpret_cast<ue::FName*>(e + spName)->index) != name) continue;
		type = spType >= 0 ? e[spType] : 2;
		if (type == 3 && spColor >= 0) { // FColor (B, G, R, A)
			out[0] = e[spColor + 2] / 255.0f; out[1] = e[spColor + 1] / 255.0f; out[2] = e[spColor] / 255.0f;
		} else if (spVector >= 0) memcpy(out, e + spVector, 12);
		return true;
	}
	return false;
}

static void PscSetParam(void* c, int32_t nameIdx, const float* v, int type) {
	if (type == 3 && fnPscSetCol && oScName >= 0 && oScVal >= 0) {
		std::vector<uint8_t> prm(size_t(ue::StructSize(fnPscSetCol)) + 16, 0);
		*reinterpret_cast<ue::FName*>(prm.data() + oScName) = ue::FName{nameIdx, 0};
		uint8_t* col = prm.data() + oScVal;
		col[2] = (uint8_t)std::clamp(v[0] * 255.0f, 0.0f, 255.0f); col[1] = (uint8_t)std::clamp(v[1] * 255.0f, 0.0f, 255.0f);
		col[0] = (uint8_t)std::clamp(v[2] * 255.0f, 0.0f, 255.0f); col[3] = 255;
		ue::Call(c, fnPscSetCol, prm.data());
		return;
	}
	if (!fnPscSetVec || oSvName < 0 || oSvVal < 0) return;
	std::vector<uint8_t> prm(size_t(ue::StructSize(fnPscSetVec)) + 16, 0);
	*reinterpret_cast<ue::FName*>(prm.data() + oSvName) = ue::FName{nameIdx, 0};
	memcpy(prm.data() + oSvVal, v, 12);
	ue::Call(c, fnPscSetVec, prm.data());
}

static int32_t NameIndex(const std::string& n) {
	auto f = grParamIdx.find(n);
	return f != grParamIdx.end() ? f->second : -1;
}

// jedes Mal (alle ~50 ms): Laufzeit-Farbe der Partikel auf die Lackfarbe setzen bzw. zuruecksetzen
static void ApplyCoreColor(const float* target, bool restore) {
	static int logged = 0;
	for (void* c : grComps) {
		if (!ue::IsValid(c)) continue;
		for (auto& pname : grParams) {
			float cur[3] = {0, 0, 0};
			int type = 2;
			bool has = PscGetParam(c, pname, cur, type);
			auto o = grParamOrig.find(pname);
			if (restore) {
				if (o != grParamOrig.end() && has) PscSetParam(c, NameIndex(pname), o->second.data(), type);
				continue;
			}
			// Original merken: der erste Wert, der nicht von uns stammt
			float mine[3];
			bool isMine = false;
			if (o != grParamOrig.end()) {
				Recolor(o->second.data(), mine);
				isMine = has && fabsf(cur[0] - mine[0]) < 0.02f && fabsf(cur[1] - mine[1]) < 0.02f && fabsf(cur[2] - mine[2]) < 0.02f;
			}
			if (has && !isMine && (o == grParamOrig.end() || std::max({cur[0], cur[1], cur[2]}) > 0.0001f)) {
				grParamOrig[pname] = {cur[0], cur[1], cur[2]};
				o = grParamOrig.find(pname);
				if (logged++ < 6) zl::Log("gold rush: %s on %s was (%.2f %.2f %.2f) type %d", pname.c_str(), ue::Name(c).c_str(), cur[0], cur[1], cur[2], type);
			}
			if (o == grParamOrig.end()) continue;
			float v[3];
			Recolor(o->second.data(), v);
			int32_t ni = NameIndex(pname);
			if (ni >= 0 && has) PscSetParam(c, ni, v, type); // nur Parameter, die der Boost wirklich benutzt
		}
	}
}

static void TickBoostPaint() {
	unsigned long long now = GetTickCount64();
	// schnell: die Laufzeit-Farbe setzt das Spiel immer wieder neu (z. B. bei jedem Boost-Start)
	static unsigned long long fast = 0;
	if (grCoreOn && now >= fast) { fast = now + 40; try { ApplyCoreColor(grTarget, false); } catch (...) {} }
	// Partikel-Komponenten des Boosts (Garage und Spiel) regelmaessig neu suchen - verteilt auf mehrere Bilder,
	// damit das Spiel dabei nicht ruckelt
	static ue::SlicedScan grScan;
	if (grCoreOn && !grTemplates.empty() && PscSetup()) {
		static void* grCar = nullptr; // neues Auto (Respawn) = neuer Boost: gleich neu suchen
		void* car = LocalCar();
		if (car != grCar) { grCar = car; grCompsAt = 0; if (grScan.running) grScan.Start(clsPSC); }
		if (!grScan.running && now >= grCompsAt) grScan.Start(clsPSC);
		if (grScan.running && grScan.Step()) {
			grCompsAt = now + 1500;
			size_t before = grComps.size();
			grComps.clear();
			for (void* c : grScan.result) {
				if (!ue::IsValid(c)) continue;
				void* t = ue::Read<void*>(c, oPscTpl);
				if (t && std::find(grTemplates.begin(), grTemplates.end(), t) != grTemplates.end()) grComps.push_back(c);
			}
			if (grComps.size() != before) zl::Log("gold rush: %d particle components, params %d", (int)grComps.size(), (int)grParams.size());
		}
	}
	if (now < grNext || !loaded) return;
	// solange die Lackierung noch nicht sitzt (z. B. beim Laden eines Spiels): sehr oft pruefen
	grNext = now + (grApplied == 0 ? 50 : 400);
	// gewuenschte Lackierung fuer den Gold Rush (nur wenn er gerade ausgewaehlt ist)
	int want = 0;
	float paint[3] = {1, 1, 1};
	{
		std::unique_lock<std::mutex> lk(mu, std::try_to_lock);
		if (!lk.owns_lock()) return;
		auto ov = overrides.find(3);
		int boost = ov != overrides.end() && ov->second > 0 ? ov->second : (ownItems.count(3) ? ownItems[3] : 0);
		auto pp = paints.find(3);
		if (boost == kGoldRush && pp != paints.end() && pp->second > 0) {
			want = pp->second;
			for (auto& pi : paintList)
				if (pi.id == want && (grPaintName = pi.name, true))
					for (int k = 0; k < 3; k++) paint[k] = powf(std::clamp(pi.rgb[k], 0.0f, 1.0f), 2.2f); // sRGB -> linear
		}
	}
	if (want == 0) {
		if (grApplied) {
			RestoreGoldRush();
			ApplyCoreColor(grTarget, true);
			grCoreOn = false;
			grApplied = 0;
			zl::Log("gold rush: original colors back");
		}
		return;
	}
	// Farbe: Farbton auf volle Helligkeit; die Helligkeit kommt aus dem Original (Leuchten bleibt)
	float mx = std::max({paint[0], paint[1], paint[2]});
	float val = std::min(1.0f, mx * 1.6f);
	float target[3];
	for (int k = 0; k < 3; k++) target[k] = (mx > 0.0001f ? paint[k] / mx : 1.0f) * std::max(val, 0.02f);
	memcpy(grTarget, target, sizeof grTarget);
	// Farbton und Helligkeit: Schwarz ist wirklich (fast) schwarz, Grau gedaempft, kraeftige Farben leuchten wie das Gold
	// Farbton satter machen (Lackfarben aus dem Spiel sind eher pastellig)
	for (int k = 0; k < 3; k++) grTn[k] = mx > 0.0001f ? powf(paint[k] / mx, 1.8f) : 1.0f;
	bool blackish = mx < 0.06f || grPaintName == "Black" || grPaintName == "Onyx";
	grV = blackish ? 0.0f : std::min(1.25f, powf(mx, 0.45f) * 1.3f);
	grCoreOn = true;
	// Vorlagen/Materialien noch da? (werden beim Laden einer Map neu angelegt)
	bool alive = !grTemplates.empty() || !grMics.empty();
	for (void* t : grTemplates) if (!ue::IsValid(t)) alive = false;
	for (void* m : grMics) if (!ue::IsValid(m)) alive = false;
	if (!alive) {
		if (now < grScanAt) return;
		grScanAt = now + 200; // beim Laden einer Map sofort umfaerben, sobald der Boost da ist
		grPatches.clear();
		grMicPatches.clear();
		grComps.clear();
		grCompsAt = 0;
		grApplied = 0;
		if (!FindGoldRushFx()) return;
	}
	// Materialien setzt das Spiel beim Neuaufbau des Boosts manchmal zurueck: regelmaessig erneuern
	static unsigned long long refresh = 0;
	if (grApplied == want && now < refresh) return;
	refresh = now + 2000;
	int n = 0, m = 0;
	for (void* t : grTemplates) n += TintTemplate(t, target);
	if (!grTemplates.empty()) grModLogged = true;
	if (fnSetVec) for (void* mic : grMics) TintMic(mic, target, m);
	if (grApplied != want)
		zl::Log("gold rush: paint %d -> %d particle colors, %d material colors (%d fx, %d materials, target %.2f %.2f %.2f)", want, n, m,
			(int)grTemplates.size(), (int)grMics.size(), target[0], target[1], target[2]);
	grApplied = want;
}

// ---------- Analyse: Kamera-Einstellungen im Rocket-League-Menue ----------
// Schreibt ins Log, welche Funktionen/Klassen das Spiel fuer die Kamera-Regler benutzt (fuer feinere Schritte).
static std::unordered_map<void*, int> camSeen; // Funktion -> wie oft schon geloggt (-1 = uninteressant)
static int camLines = 0;
static bool camDumped = false;

static void CamDumpClass(void* cls) {
	std::string props;
	for (void* s = cls; s; s = ue::Read<void*>(s, ue::off::StructSuper)) {
		for (void* f = ue::Read<void*>(s, ue::off::StructChildren); f; f = ue::Read<void*>(f, ue::off::FieldNext)) {
			std::string cn = ue::Name(ue::ClassOf(f));
			if (cn.size() < 8 || cn.compare(cn.size() - 8, 8, "Property") != 0) continue;
			props += ue::Name(f) + ":" + cn.substr(0, cn.size() - 8) + "@" + std::to_string(ue::Read<int32_t>(f, ue::off::PropOffset)) + " ";
			if (props.size() > 1500) break;
		}
		if (ue::Name(s) == "Object" || props.size() > 1500) break;
	}
	zl::Log("cam: class %s: %s", ue::FullName(cls).c_str(), props.c_str());
}

static void CamDump() {
	if (camDumped) return;
	camDumped = true;
	int n = 0;
	for (int i = 0, c = ue::ObjectCount(); i < c && n < 40; i++) {
		void* o = ue::ObjectAt(i);
		if (!o) continue;
		try {
			if (ue::Name(ue::ClassOf(o)) != "Class") continue;
			std::string nm = ue::Name(o), lo = nm;
			for (auto& ch : lo) ch = (char)tolower((unsigned char)ch);
			if (lo.find("camera") == std::string::npos && lo.find("slider") == std::string::npos) continue;
			CamDumpClass(o);
			n++;
		} catch (...) {}
	}
}

static void CamOnEvent(void* caller, void* fn, void* params) {
	auto it = camSeen.find(fn);
	if (it != camSeen.end() && (it->second < 0 || it->second >= 3)) return;
	if (it == camSeen.end()) {
		std::string full = ue::FullName(fn), lo = full;
		for (auto& ch : lo) ch = (char)tolower((unsigned char)ch);
		bool cam = lo.find("camera") != std::string::npos || lo.find("slider") != std::string::npos;
		it = camSeen.emplace(fn, cam ? 0 : -1).first;
		if (!cam) return;
	}
	if (camLines++ > 400) { it->second = -1; return; }
	it->second++;
	// Parameter mit Werten (Zahlen)
	std::string args;
	try {
		for (void* f = ue::Read<void*>(fn, ue::off::StructChildren); f && params; f = ue::Read<void*>(f, ue::off::FieldNext)) {
			std::string cn = ue::Name(ue::ClassOf(f));
			int off = ue::Read<int32_t>(f, ue::off::PropOffset);
			char b[96];
			if (cn == "FloatProperty") snprintf(b, sizeof b, "%s=%.3f ", ue::Name(f).c_str(), *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(params) + off));
			else if (cn == "IntProperty") snprintf(b, sizeof b, "%s=%d ", ue::Name(f).c_str(), *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(params) + off));
			else snprintf(b, sizeof b, "%s:%s ", ue::Name(f).c_str(), cn.c_str());
			args += b;
			if (args.size() > 400) break;
		}
	} catch (...) {}
	zl::Log("cam: %s on %s %s", ue::FullName(fn).c_str(), ue::Name(ue::ClassOf(caller)).c_str(), args.c_str());
	CamDump();
}

void OnProcessEvent(void* caller, void* fn, void* params) {
	if (!ready || !caller || !fn) return;
	if (fn == fnApplyColors && fnApplyColors) { try { PatchColorEvent(caller, params); } catch (...) {} }
	tweaks::OnEvent(caller, fn);
	try { customs::Fast(); } catch (...) {}
	static unsigned long long nextCheck = 0;
	unsigned long long now = GetTickCount64();
	// Ereignisse eines Autos (z. B. beim Spawnen) -> sofort pruefen, bevor es sein Loadout baut
	static void* lastCls = nullptr;
	static bool lastIsCar = false, lastIsPRI = false;
	void* cls = ue::ClassOf(caller);
	if (cls != lastCls) { lastCls = cls; lastIsCar = clsCar && ue::IsA(caller, clsCar); lastIsPRI = clsPRI && ue::IsA(caller, clsPRI); }
	if (lastIsCar) try { CarEventColors(caller); } catch (...) {}
	// Beim Laden (noch kein Auto unterwegs) bei jedem Auto-/PRI-Ereignis pruefen: das Loadout kommt per
	// Replikation und das Auto wird direkt danach gebaut - hier zaehlt jede Millisekunde.
	// Danach (beim Fahren) reicht alle 40 ms - das haelt den Hook schlank, das Spiel laeuft fluessiger.
	bool loading = now - lastPlayerTick.load() > 1500;
	if (((lastIsCar || lastIsPRI) && loading) || now >= nextCheck) {
		nextCheck = now + 40;
		try { EnsureLocalLoadout(); } catch (...) {}
	}
	if (tickFns.count(fn)) { // lokaler Spieler
		try { TickBoostPaint(); } catch (...) {}
		try { cam::Tick(); } catch (...) {}
		try { TickCustoms(); } catch (...) {}
		// LocalPlayer merken (gibt es auch schon im Hauptmenue)
		try {
			void* lp = ue::Read<void*>(caller, offPlayer);
			if (lp && clsLocalPlayer && std::find(localPlayers.begin(), localPlayers.end(), lp) == localPlayers.end() && ue::IsA(lp, clsLocalPlayer)) {
				localPlayers.push_back(lp);
				zl::Log("local player: %p (%s)", lp, ue::Name(lp).c_str());
				// Zweiter Spieler (Splitscreen) - z. B. wenn der Controller doppelt erkannt wird: gleich wieder entfernen
				void* first = PrimaryLP();
				if (first && lp != first) {
					Queue([lp] {
						if (!ue::IsValid(lp)) return;
						static void* clsLP = ue::FindClass("Class Engine.Player");
						static int offVC = clsLP ? ue::PropOffset(clsLocalPlayer, "ViewportClient") : -1;
						void* vc = offVC >= 0 ? ue::Read<void*>(lp, offVC) : nullptr;
						void* fn = vc ? ue::FindFunction(ue::ClassOf(vc), "RemovePlayer") : nullptr;
						int off = fn ? ue::FirstParamOffset(fn, nullptr) : -1;
						if (!vc || !fn || off < 0) { zl::Log("splitscreen: couldn't remove 2nd player (vc %p fn %p)", vc, fn); return; }
						std::vector<uint8_t> p(size_t(ue::StructSize(fn)) + 16, 0);
						*reinterpret_cast<void**>(p.data() + off) = lp;
						ue::Call(vc, fn, p.data());
						localPlayers.erase(std::remove(localPlayers.begin(), localPlayers.end(), lp), localPlayers.end());
						zl::Log("splitscreen: 2nd player removed (%s)", ue::Name(lp).c_str());
					});
				}
			}
		} catch (...) {}
		// Im Hauptmenue tickt auch ein Controller - der zaehlt nur, wenn er wirklich ein Auto faehrt
		void* pawn = nullptr;
		try { pawn = (ue::IsA(caller, clsPC) && offPawn >= 0) ? ue::Read<void*>(caller, offPawn) : nullptr; } catch (...) {}
		if (!pawn || !clsCar || !ue::IsA(pawn, clsCar)) {
			// zwischen zwei Spawns (kein Auto unterwegs): sicherer Moment, dem Spiel das neue Loadout zu melden
			if (void* pp = pendingSet) {
				pendingSet = nullptr;
				void* cpri = nullptr;
				try { cpri = offPRI >= 0 ? ue::Read<void*>(caller, offPRI) : nullptr; } catch (...) {}
				if (cpri == pp && ue::IsAlive(pp)) {
					try { WriteOverrides(pp); ue::Call(pp, fnSet); zl::Log("apply: no car right now -> loadout announced for the next spawn"); } catch (...) {}
				}
			}
			// Hauptmenue/Garage: sicherer Moment (einmal pro Frame), um das Vorschau-Auto anzupassen
			// Gerade kein Auto - im Freeplay passiert das bei jedem Tor (Explosion + Replay). Dann die Einstellung
			// "Standard-Teamfarben" NICHT zuruecksetzen, sonst lackiert das Replay dein Auto blau/orange.
			// Erst wenn du das Freeplay wirklich verlaesst (kein Spiel mehr) kommt sie zurueck.
			{
				bool stillInGame = false;
				try {
					void* cpri = offPRI >= 0 ? ue::Read<void*>(caller, offPRI) : nullptr;
					void* ge = (cpri && offPRIGameEvent >= 0 && ue::IsValid(cpri)) ? ue::Read<void*>(cpri, offPRIGameEvent) : nullptr;
					stillInGame = ge && ue::IsAlive(ge) && inFreeplay.load();
				} catch (...) {}
				if (!stillInGame) try { RestoreSharedPrefs(); } catch (...) {}
			}
			try { TickPreview(); } catch (...) {}
			try { merge::TickNames(); } catch (...) {}
			// Item-Liste schon im Hauptmenue laden (wird fuer Farben gebraucht, auch ohne F8)
			static unsigned long long nextLoad = 0;
			if (!loaded && GetTickCount64() >= nextLoad) {
				nextLoad = GetTickCount64() + 5000;
				try { LoadProducts(); } catch (...) {}
			}
			return;
		}
		{ // Splitscreen-Gast: nicht dein Auto
			void* plp = PrimaryLP();
			void* ppc = (plp && offLPActor >= 0) ? ue::Read<void*>(plp, offLPActor) : nullptr;
			if (ppc && caller != ppc) return;
		}
		if (ue::killFlagCheck && !ue::IsAlive(caller)) { // ein tickender Controller wird nicht geloescht
			ue::killFlagCheck = false;
			zl::Log("WARNING: delete flag check doesn't fit this game version - turned off");
		}
		if (localPC.load() != caller) {
			localPC = caller;
			void* pri = nullptr; void* pawn = nullptr;
			try { pri = ue::Read<void*>(caller, offPRI); pawn = offPawn >= 0 ? ue::Read<void*>(caller, offPawn) : nullptr; } catch (...) {}
			zl::Log("local player controller: %p (%s) pri=%p pawn=%p (%s)", caller, ue::Name(caller).c_str(), pri, pawn, pawn ? ue::Name(pawn).c_str() : "-");
			ue::CheckObjects(caller);
			// Neues Spiel: pruefen, ob das Auto schon unsere Items hat - sonst einmal neu spawnen
			applyAt = 0;
			verifyTries = 0;
			verifyAt = GetTickCount64() + 800; // und kurz danach am fertigen Auto nachsehen
			bool hasOv;
			{
				std::lock_guard<std::mutex> lk(mu);
				hasOv = !overrides.empty();
			}
			if (pri && hasOv) {
				bool missing = NeedsApply(pri), written = writtenPRI.load() == pri;
				zl::Log("new game: items in loadout=%s, written on load=%s", missing ? "no" : "yes", written ? "yes" : "no");
				if (missing) applyAt = GetTickCount64() + 300; // Auto wurde ohne unsere Items gebaut: einmal neu spawnen
			}
		}
		inGame = true;
		lastPlayerTick = GetTickCount64();
		try { localPRIForColors = offPRI >= 0 ? ue::Read<void*>(caller, offPRI) : nullptr; } catch (...) {}
		try { TickColors(nullptr, pawn); } catch (...) {}
		// Freeplay-Tweaks (Ball, Spielgeschwindigkeit)
		try {
			void* tpri = offPRI >= 0 ? ue::Read<void*>(caller, offPRI) : nullptr;
			void* tge = (tpri && offPRIGameEvent >= 0) ? ue::Read<void*>(tpri, offPRIGameEvent) : nullptr;
			bool fp = clsFreeplayCar && ue::IsA(pawn, clsFreeplayCar);
			inFreeplay = fp;
			bool vge = ue::IsValid(tge);
			// Training (Custom Training, Trainings-Packs von Spielern, Tutorials) ist wie Freeplay offline: Tweaks gehen dort auch
			bool training = vge && ((clsTrainingEditor && ue::IsA(tge, clsTrainingEditor)) || (clsTutorial && ue::IsA(tge, clsTutorial)));
			static bool lastTraining = false;
			if (training != lastTraining) { lastTraining = training; zl::Log("training: %s", training ? "yes" : "no"); }
			inTweaks = fp || training;
			tweaks::Tick(pawn, vge ? tge : nullptr, fp || training);
		} catch (...) {}
		// Das Auto kann schon gebaut sein, bevor unsere Items im Loadout stehen (dann faehrt man kurz das eigene
		// Auto). Am fertigen Auto nachsehen und im Freeplay einmal sauber neu spawnen.
		if (verifyAt && GetTickCount64() >= verifyAt) {
			verifyAt = 0;
			int miss = -1;
			try { miss = CarMissesItems(pawn); } catch (...) {}
			if (miss < 0 && verifyTries < 12) { verifyTries++; verifyAt = GetTickCount64() + 300; } // noch nicht fertig geladen
			else if (miss == 1 && verifyTries < 20) {
				verifyTries = 20;
				zl::Log("verify: car was built without your items -> respawn");
				applyAt = GetTickCount64();
			} else if (miss == 0) {
				static int ok = 0;
				if (ok++ < 5) zl::Log("verify: car has your items");
				int body = 0;
				{
					std::lock_guard<std::mutex> lk(mu);
					body = overrides.count(0) ? overrides[0] : (ownItems.count(0) ? ownItems[0] : 0);
				}
				try { LogHitbox(pawn, body); } catch (...) {}
			}
		}
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
struct OwnPart { int id = 0; OnlineId online; };
struct PreviewState {
	std::map<int, OwnPart> orig;  // Slot -> eigenes Item (mit Online-ID, z. B. deine Lackierung)
	std::map<int, int> lastSeen;  // Slot -> zuletzt gesehener Wert
	std::map<int, long long> target; // Slot -> zuletzt gesetztes Item + Farbe
	std::map<int, unsigned long long> until; // Slot -> nicht vor diesem Zeitpunkt erneut tauschen
	std::map<int, OwnPart> base;  // dein Loadout, wie das Spiel es zeigt, bevor wir etwas tauschen
	int gen = -1;
};
static std::map<void*, PreviewState> previews;
static std::vector<void*> previewActors;
static unsigned long long nextPreviewScan = 0;
static std::atomic<unsigned long long> nextPreviewCheck{0};
static std::atomic<int> userGen{0}; // steigt bei jeder Aenderung im Menue -> Wartezeiten aufheben
std::map<int, int> ownItems;        // Slot -> dein echtes Item (fuer die Anzeige)
// Was wir selbst ans Menue-Auto gehaengt haben (Slot -> Items). Baut das Spiel ein neues Vorschau-Auto aus dem
// alten (mit unseren Items dran), sind das NICHT deine Items - dann gilt dein zuletzt bekanntes echtes Item.
static std::map<int, std::set<int>> issued;
static std::map<int, OwnPart> realOwn; // Slot -> dein echtes Item (bleibt ueber neue Vorschau-Autos hinweg)

// ---------- Lackierte Items fuer das Menue-Auto ----------
// Das Spiel zeigt lackierte Shop-Items ueber "temporaere" Online-Produkte an. Genau so machen wir es auch:
// eigene Produkt-Liste anlegen, Produkt + Painted-Attribut hinein, dann mit dessen ID ans Vorschau-Auto.
static void *clsStore = nullptr, *store = nullptr, *ourSet = nullptr, *fnTempId = nullptr, *fnAddData = nullptr, *fnAddAttribute = nullptr;
static int offAddData = -1, offAddRet = -1, offTempRet = -1, offOPInstance = -1, offAddAttr = -1, offLoadoutOnline = 0x30;
static bool storeTried = false;
static std::map<long long, OnlineId> paintedIds; // Produkt*1000+Farbe -> Online-ID

static bool EnsureStore() {
	if (ourSet) return true;
	if (storeTried) return false;
	storeTried = true;
	clsStore = ue::FindClass("Class TAGame.OnlineProductStore_TA");
	void* clsSet = ue::FindClass("Class TAGame.OnlineProductStoreSet_TA");
	void* clsOP = ue::FindClass("Class TAGame.OnlineProduct_TA");
	if (!clsStore || !clsSet || !clsOP) { zl::Log("paint menu: classes missing"); return false; }
	void* fnGet = ue::FindFunction(clsStore, "GetInstance");
	void* cdo = ue::FindObject("OnlineProductStore_TA TAGame.Default__OnlineProductStore_TA");
	if (fnGet && cdo) {
		std::vector<uint8_t> p(size_t(ue::StructSize(fnGet)) + 16, 0);
		ue::Call(cdo, fnGet, p.data());
		int ro = ue::PropOffset(fnGet, "ReturnValue");
		store = ro >= 0 ? *reinterpret_cast<void**>(p.data() + ro) : nullptr;
	}
	void* fnCreate = ue::FindFunction(clsStore, "CreateSet");
	if (store && fnCreate) {
		std::vector<uint8_t> p(size_t(ue::StructSize(fnCreate)) + 16, 0);
		ue::Call(store, fnCreate, p.data());
		int ro = ue::PropOffset(fnCreate, "ReturnValue");
		ourSet = ro >= 0 ? *reinterpret_cast<void**>(p.data() + ro) : nullptr;
	}
	if (ourSet && !localPlayers.empty()) {
		if (void* fnInit = ue::FindFunction(clsSet, "InitLocal")) {
			std::vector<uint8_t> p(size_t(ue::StructSize(fnInit)) + 16, 0);
			int io = ue::PropOffset(fnInit, "InPlayer");
			if (io >= 0) *reinterpret_cast<void**>(p.data() + io) = localPlayers[0];
			ue::Call(ourSet, fnInit, p.data());
		}
	}
	fnTempId = ue::FindFunction(clsStore, "GetTempInstanceID");
	if (fnTempId) offTempRet = ue::PropOffset(fnTempId, "ReturnValue");
	fnAddData = ue::FindFunction(clsSet, "AddProductData");
	if (fnAddData) { offAddData = ue::PropOffset(fnAddData, "Data"); offAddRet = ue::PropOffset(fnAddData, "ReturnValue"); }
	fnAddAttribute = ue::FindFunction(clsOP, "AddAttribute");
	if (fnAddAttribute) offAddAttr = ue::PropOffset(fnAddAttribute, "InAttribute");
	offOPInstance = ue::PropOffset(clsOP, "InstanceID");
	if (void* ld = ue::FindObject("ScriptStruct TAGame._Types_TA.LoadoutData")) {
		int o = ue::PropOffset(ld, "OnlineProducts");
		if (o >= 0) offLoadoutOnline = o;
	}
	zl::Log("paint menu: store=%p set=%p tempId=%p addData=%p (data=%x ret=%x) addAttr=%p instance=%x loadoutOnline=%x",
		store, ourSet, fnTempId, fnAddData, offAddData, offAddRet, fnAddAttribute, offOPInstance, offLoadoutOnline);
	return ourSet != nullptr;
}

static bool PaintedId(int productId, int paintId, OnlineId& out) {
	long long key = (long long)productId * 1000 + paintId;
	static std::map<long long, void*> paintedOps; // Kopie lebt noch? (beim Laden einer Map kann sie weg sein)
	auto it = paintedIds.find(key);
	if (it != paintedIds.end()) {
		auto po = paintedOps.find(key);
		if (po == paintedOps.end() || ue::IsAlive(po->second)) { out = it->second; return true; }
		paintedIds.erase(it);
		paintedOps.erase(po);
	}
	static std::map<long long, unsigned long long> failed; // nach einem Fehlschlag erst nach 3 s wieder versuchen
	auto fl = failed.find(key);
	if (fl != failed.end() && GetTickCount64() < fl->second) return false;
	failed[key] = GetTickCount64() + 3000; // bei Erfolg unten wieder raus
	if (!EnsureStore() || !fnTempId || !fnAddData || offAddData < 0 || offAddRet < 0 || offOPInstance < 0 || !fnAddAttribute) return false;
	std::vector<uint8_t> t(size_t(ue::StructSize(fnTempId)) + 16, 0);
	ue::Call(store, fnTempId, t.data());
	OnlineId id = *reinterpret_cast<OnlineId*>(t.data() + offTempRet);
	// FOnlineProductData: ProductID 0x00, InstanceID 0x08, SeriesID 0x18, Attributes 0x20 (leer)
	std::vector<uint8_t> p(size_t(ue::StructSize(fnAddData)) + 16, 0);
	*reinterpret_cast<int32_t*>(p.data() + offAddData) = productId;
	*reinterpret_cast<OnlineId*>(p.data() + offAddData + 8) = id;
	ue::Call(ourSet, fnAddData, p.data());
	void* op = *reinterpret_cast<void**>(p.data() + offAddRet);
	void* attr = op ? PaintAttr(paintId) : nullptr;
	if (!op || !attr) { zl::Log("paint menu: couldn't create painted product %d/%d (op %p attr %p)", productId, paintId, op, attr); return false; }
	std::vector<uint8_t> a(size_t(ue::StructSize(fnAddAttribute)) + 16, 0);
	*reinterpret_cast<void**>(a.data() + offAddAttr) = attr;
	ue::Call(op, fnAddAttribute, a.data());
	out = ue::Read<OnlineId>(op, offOPInstance);
	paintedIds[key] = out;
	paintedOps[key] = op;
	failed.erase(key);
	zl::Log("paint menu: product %d in %d -> %s (id %llx:%llx)", productId, paintId, ue::Name(op).c_str(), (unsigned long long)out.a, (unsigned long long)out.b);
	return true;
}

static bool ChangePart(void* actor, int slot, int id, OnlineId online = {}) {
	if (!fnChangePart || offCPSlot < 0 || offCPProduct < 0) return false;
	std::vector<uint8_t> p(size_t(ue::StructSize(fnChangePart)) + 16, 0);
	*reinterpret_cast<int32_t*>(p.data() + offCPSlot) = slot;
	*reinterpret_cast<int32_t*>(p.data() + offCPProduct) = id;
	if (offCPOnline >= 0) *reinterpret_cast<OnlineId*>(p.data() + offCPOnline) = online;
	if (offCPPaint >= 0) *reinterpret_cast<int32_t*>(p.data() + offCPPaint) = 0;
	ue::Call(actor, fnChangePart, p.data());
	return true;
}

static void SyncPreview(void* actor, bool main) {
	auto* prods = ue::Ptr<ue::TArray<int32_t>>(actor, offPrevLoadout);
	if (!prods->data || prods->count <= 0 || prods->count > 64) return;
	auto* onl = ue::Ptr<ue::TArray<OnlineId>>(actor, offPrevLoadout + offLoadoutOnline);
	auto onlineAt = [&](int slot) { return (onl->data && slot < onl->count) ? onl->data[slot] : OnlineId{}; };
	std::map<int, int> ov, pt;
	{
		std::lock_guard<std::mutex> lk(mu);
		ov = overrides;
		pt = paints;
	}
	PreviewState& st = previews[actor];
	if (st.gen != userGen.load()) { st.until.clear(); st.gen = userGen.load(); } // du hast gerade etwas geaendert: sofort
	auto snapshot = [&] {
		st.base.clear();
		for (int i = 0; i < kSlotCount; i++) {
			int slot = kSlots[i].index;
			if (slot >= prods->count) continue;
			OwnPart now{prods->data[slot], onlineAt(slot)};
			auto is = issued.find(slot);
			bool ours = is != issued.end() && is->second.count(now.id);
			{ // vom Auto erzwungene Teile (z. B. Lamborghini-Raeder) sind auch nicht deine
				std::lock_guard<std::mutex> lk(mu);
				for (auto& [body, parts] : forcedBy) { auto f = parts.find(slot); if (f != parts.end() && f->second == now.id && issued[0].count(body)) ours = true; }
			}
			auto ro = realOwn.find(slot);
			if (ours && ro != realOwn.end() && ro->second.id != now.id) st.base[slot] = ro->second;
			else { st.base[slot] = now; realOwn[slot] = now; }
		}
	};
	auto ourId = [&](int slot) { auto tg = st.target.find(slot); return tg != st.target.end() ? int(tg->second / 1000) : -1; };
	int bodyWant = ov.count(0) ? ov[0] : 0;
	// Dein Loadout merken, bevor wir tauschen. Baut das Spiel das Auto wieder aus deinem Profil auf
	// (Auto ist wieder deins statt unserem), neu merken - so kommen auch Items mit, die du inzwischen
	// in der Garage ausgeruestet hast.
	if (st.base.empty()) snapshot();
	else if (bodyWant && prods->data[0] != bodyWant && prods->data[0] != ourId(0)) {
		snapshot();
		st.target.clear();
		st.lastSeen.clear();
		st.orig.clear();
	} else if (!bodyWant) {
		for (auto& [slot, own] : st.base) { // ohne eigenes Auto: geaenderter Slot = du hast etwas anderes ausgeruestet
			if (slot >= prods->count || ov.count(slot)) continue;
			int v = prods->data[slot];
			if (v != own.id && v != ourId(slot)) {
				zl::Log("menu car: your own item in slot %d is now %d (was %d)", slot, v, own.id);
				own = OwnPart{v, onlineAt(slot)};
				realOwn[slot] = own;
				st.orig.erase(slot);
				st.target.erase(slot);
				st.lastSeen.erase(slot);
			}
		}
	}
	auto baseId = [&](int slot) { auto b = st.base.find(slot); return b != st.base.end() ? b->second.id : prods->data[slot]; };
	// Teile tauschen: zuerst das Auto (es kann andere Teile erzwingen), dann der Rest
	auto apply = [&](int slot, int id) {
		int want = WantedPaint(slot, id, pt);
		OnlineId wantId;
		if (want && !PaintedId(id, want, wantId)) want = 0;
		long long tgt = (long long)id * 1000 + want;
		int now = prods->data[slot];
		bool ok = now == id && onlineAt(slot) == wantId;
		if (ok) { st.lastSeen[slot] = now; st.target[slot] = tgt; return; }
		auto ls = st.lastSeen.find(slot);
		auto tg = st.target.find(slot);
		if (ls != st.lastSeen.end() && ls->second == now && tg != st.target.end() && tg->second == tgt) return; // schon gesetzt
		unsigned long long t = GetTickCount64();
		if (t < st.until[slot]) return; // nicht staendig neu laden (z. B. beim Durchscrollen in der Garage)
		st.until[slot] = t + 1200;
		if (!st.orig.count(slot)) { auto b = st.base.find(slot); st.orig[slot] = b != st.base.end() ? b->second : OwnPart{now, onlineAt(slot)}; }
		ChangePart(actor, slot, id, wantId);
		issued[slot].insert(id);
		st.lastSeen[slot] = prods->data[slot];
		st.target[slot] = tgt;
		zl::Log("menu car %s: slot %d %d -> %d%s (now %d)", ue::Name(actor).c_str(), slot, now, id, want ? " painted" : "", prods->data[slot]);
	};
	if (bodyWant && 0 < prods->count) apply(0, bodyWant);
	// Hat unser Auto Teile erzwungen (Slot ohne gewaehltes Item zeigt etwas anderes als dein Teil)?
	std::map<int, int> forced;
	if (bodyWant && prods->data[0] == bodyWant) {
		for (auto& [slot, own] : st.base) {
			if (slot == 0 || slot >= prods->count || ov.count(slot)) continue;
			int v = prods->data[slot];
			if (v > 0 && v != own.id && v != ourId(slot)) forced[slot] = v;
		}
		std::lock_guard<std::mutex> lk(mu);
		auto& fb = forcedBy[bodyWant];
		for (auto& [slot, v] : forced)
			if (fb[slot] != v) { fb[slot] = v; zl::Log("menu car: car %d brings its own part %d in slot %d", bodyWant, v, slot); }
		for (auto& [slot, v] : fb) forced.emplace(slot, v);
	}
	// Was soll im Slot sein? Gewaehltes Item - oder dein (bzw. das erzwungene) Teil, wenn du es lackieren willst
	std::map<int, int> desired = ov;
	for (auto& [slot, paint] : pt) {
		if (ov.count(slot) || slot >= prods->count) continue;
		auto f = forced.find(slot);
		int ownId = f != forced.end() ? f->second : baseId(slot);
		if (WantedPaint(slot, ownId, pt)) desired[slot] = ownId;
	}
	if (!bodyWant && desired.count(0) && 0 < prods->count) apply(0, desired[0]); // dein eigenes Auto lackiert
	for (auto& [slot, id] : desired)
		if (slot != 0 && slot < prods->count) apply(slot, id);
	// Slots zurueck auf das eigene Item
	for (auto it = st.orig.begin(); it != st.orig.end();) {
		int slot = it->first;
		if (desired.count(slot) || slot >= prods->count) { ++it; continue; }
		ChangePart(actor, slot, it->second.id, it->second.online);
		zl::Log("menu car %s: slot %d back to %d", ue::Name(actor).c_str(), slot, it->second.id);
		st.lastSeen.erase(slot);
		st.target.erase(slot);
		it = st.orig.erase(it);
	}
	// Deine Items fuer die Liste ("Your own" zeigt den Namen) - bei erzwungenen Teilen das, was wirklich dran ist
	if (main) {
		std::map<int, int> own;
		for (auto& [slot, b] : st.base) {
			auto f = forced.find(slot);
			own[slot] = (!ov.count(slot) && f != forced.end()) ? f->second : b.id;
		}
		std::lock_guard<std::mutex> lk(mu);
		for (auto& [k, v] : own) ownItems[k] = v;
	}
}

bool InMatch() { return GetTickCount64() - lastPlayerTick.load() < 1500; }
bool InFreeplay() { return inFreeplay.load() && InMatch(); }
bool InTweaksMode() { return inTweaks.load() && InMatch(); }
void* MyPRI() { return localPRIForColors.load(); }

void TickPreview() {
	if (!ready || !clsPreview || offPrevLoadout < 0 || !fnChangePart) return;
	unsigned long long t = GetTickCount64();
	if (t < nextPreviewCheck) return;
	nextPreviewCheck = t + 120; // schnell reagieren, wenn das Spiel das Menue-Auto neu aufbaut
	// Im Spiel (Freeplay/Match) gibt es kein Vorschau-Auto: nicht suchen, damit nichts ruckelt
	if (t - lastPlayerTick.load() < 1500) return;
	// Nach einer Pause (Ladebildschirm, Spiel) die Liste neu aufbauen: alte Autos koennen schon geloescht sein
	static unsigned long long lastRun = 0;
	if (t - lastRun > 1000) {
		previewActors.clear();
		previews.clear();
		nextPreviewScan = t + 1500; // dem neuen Menue kurz Zeit zum Aufbauen geben
	}
	lastRun = t;
	if (t >= nextPreviewScan) {
		nextPreviewScan = t + 3000;
		previewActors = ue::Instances(clsPreview);
		static int logged = 0;
		if (logged++ < 3) zl::Log("menu cars: %d (scan %llu ms)", (int)previewActors.size(), GetTickCount64() - t);
	}
	for (auto it = previews.begin(); it != previews.end();) // alte Autos vergessen
		it = ue::IsAlive(it->first) ? std::next(it) : previews.erase(it);
	bool mainDone = false;
	for (void* a : previewActors) {
		if (!ue::IsAlive(a)) continue;
		bool main = false;
		if (!mainDone) {
			auto* pr = ue::Ptr<ue::TArray<int32_t>>(a, offPrevLoadout);
			if (pr->data && pr->count > 0) main = mainDone = true; // erstes Auto mit Loadout = deins
		}
		try { SyncPreview(a, main); } catch (...) {}
		if (main) try { TickColors(a, nullptr); } catch (...) {}
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

// ---------- Items vorladen ----------
// Rocket League laedt Modelle/Texturen eines Items erst, wenn es gebraucht wird (das dauert). Prism stoesst das
// Laden schon vorher an: fuer deine gewaehlten Items beim Start und fuer Items, ueber die du im Menue faehrst.
static std::set<int> preloaded;
static std::mutex preMu;
void Preload(int id) {
	if (id <= 0) return;
	{
		std::lock_guard<std::mutex> lk(preMu);
		if (!preloaded.insert(id).second) return;
	}
	Queue([id] {
		static void* fn = clsProduct ? ue::FindFunction(clsProduct, "LoadAsset") : nullptr;
		static bool logged = false;
		if (!logged) {
			logged = true;
			std::string args;
			for (void* f = fn ? ue::Read<void*>(fn, ue::off::StructChildren) : nullptr; f; f = ue::Read<void*>(f, ue::off::FieldNext))
				args += ue::Name(f) + ":" + ue::Name(ue::ClassOf(f)) + " ";
			zl::Log("preload: Product_TA.LoadAsset=%p (%s)", fn, args.c_str());
		}
		if (!fn) return;
		auto dbs = ue::Instances(clsDB);
		if (dbs.empty()) return;
		auto* arr = ue::Ptr<ue::TArray<void*>>(dbs[0], offProducts);
		void* prod = (arr->data && id < arr->count) ? arr->data[id] : nullptr;
		if (!prod) return;
		std::vector<uint8_t> p(size_t(ue::StructSize(fn)) + 16, 0); // ohne Rueckmeldung, im Hintergrund
		int oa = ue::PropOffset(fn, "bAsync");
		if (oa >= 0) p[oa] = 1;
		ue::Call(prod, fn, p.data());
	});
}

void Set(int slot, int id) {
	Preload(id);
	{
		std::lock_guard<std::mutex> lk(mu);
		if (id > 0) overrides[slot] = id;
		else overrides.erase(slot);
	}
	Queue([slot, id] {
		if (id <= 0) restore.insert(slot);
		else restore.erase(slot);
	});
	nextPreviewCheck = 0; // Menue-Auto sofort aktualisieren
	userGen++;
	applyAt = GetTickCount64() + 400; // mehrere Klicks kurz hintereinander = einmal neu spawnen
	Save();
}

static std::wstring SaveFile() { return zl::DataDir() + L"\\zlmod.json"; }

void Save() {
	std::map<int, int> ov, pt;
	{
		std::lock_guard<std::mutex> lk(mu);
		ov = overrides;
		pt = paints;
	}
	FILE* f = _wfopen(SaveFile().c_str(), L"w");
	if (!f) return;
	fputc('{', f);
	bool first = true;
	for (auto& [s, id] : ov) {
		fprintf(f, "%s\"%d\":%d", first ? "" : ",", s, id);
		first = false;
	}
	for (auto& [s, id] : pt) {
		fprintf(f, "%s\"p%d\":%d", first ? "" : ",", s, id);
		first = false;
	}
	auto hex = [](const float* c) { return (int(c[0] * 255 + 0.5f) << 16) | (int(c[1] * 255 + 0.5f) << 8) | int(c[2] * 255 + 0.5f); };
	fprintf(f, "%s\"co\":%d,\"cb\":%d,\"cp\":%d,\"ca\":%d,\"cq\":%d,\"cr\":%d", first ? "" : ",", colorsOn ? 1 : 0, colorsBoth ? 1 : 0,
		hex(colors[0][0]), hex(colors[0][1]), hex(colors[1][0]), hex(colors[1][1]));
	fputs("}\n", f);
	fclose(f);
}

void SetPaint(int slot, int paintId) {
	{
		std::lock_guard<std::mutex> lk(mu);
		if (paintId > 0) paints[slot] = paintId;
		else paints.erase(slot);
	}
	Queue([] { paintFailed.clear(); });
	nextPreviewCheck = 0;
	userGen++;
	applyAt = GetTickCount64() + 400;
	Save();
}

// ---------- Eigene Lackfarben (Primaer/Akzent) ----------
bool colorsOn = false;
bool colorsBoth = true;
float colors[2][2][3] = {{{0.95f, 0.45f, 0.10f}, {0.10f, 0.10f, 0.12f}}, {{0.95f, 0.45f, 0.10f}, {0.10f, 0.10f, 0.12f}}}; // sRGB 0..1
static std::atomic<int> colorGen{1};
static int offApplyTeam = -1, offApplyCustom = -1, offTeamOverride = -1, offCustomOverride = -1, offPreviewMesh = -1, offCarMesh = -1, offMeshTeam = -1;
static std::atomic<void*> ourMeshes[2]; // [0] Menue-Auto, [1] dein Auto im Spiel
static bool colorsTried = false;
// "Use Default Team Colors" (bzw. Standard-Teamfarben im Freeplay) steckt in TeamColorPreferences_TA, auf das
// jedes Auto-Modell zeigt. Ist es an, nimmt das Spiel statt deiner Farben immer blau/orange.
// Dein Auto bekommt deshalb saubere Einstellungen (die Standardwerte der Klasse), alle anderen Autos nicht.
static int offColorPrefs = -1, offPrefsBits = -1;
static uint32_t prefsMask = 0;
static void* cleanPrefs = nullptr;
static std::map<void*, void*> prefsOrig; // Modell -> urspruengliche Einstellungen
static std::map<void*, int> colorsApplied; // Modell -> Farb-Stand, der schon drauf ist

static bool EnsureColors() {
	if (colorsTried) return fnApplyColors != nullptr;
	colorsTried = true;
	clsCarMesh = ue::FindClass("Class TAGame.CarMeshComponentBase_TA");
	if (clsCarMesh) {
		fnApplyColors = ue::FindFunction(clsCarMesh, "ApplyColorsToCar");
		if (fnApplyColors) {
			offApplyTeam = ue::PropOffset(fnApplyColors, "TeamColor");
			offApplyCustom = ue::PropOffset(fnApplyColors, "CustomColor");
			std::string ps;
			for (void* f = ue::Read<void*>(fnApplyColors, ue::off::StructChildren); f; f = ue::Read<void*>(f, ue::off::FieldNext))
				ps += ue::Name(f) + ":" + ue::Name(ue::ClassOf(f)) + " ";
			zl::Log("colors: ApplyColorsToCar(%s)", ps.c_str());
		}
		offTeamOverride = ue::PropOffset(clsCarMesh, "TeamColorOverride");
		offCustomOverride = ue::PropOffset(clsCarMesh, "CustomColorOverride");
		offMeshTeam = ue::PropOffset(clsCarMesh, "Team");
		offColorPrefs = ue::PropOffset(clsCarMesh, "ColorPrefs");
	}
	if (clsPreview) offPreviewMesh = ue::PropOffset(clsPreview, "CarMesh");
	if (clsCar) offCarMesh = ue::PropOffset(clsCar, "CarMesh");
	if (void* clsPrefs = ue::FindClass("Class TAGame.TeamColorPreferences_TA")) {
		offPrefsBits = ue::PropOffset(clsPrefs, "bForceDefaultColors");
		int off2 = ue::PropOffset(clsPrefs, "bFreeplayDefaultTeamColors");
		uint32_t m1 = ue::BoolMask(clsPrefs, "bForceDefaultColors"), m2 = ue::BoolMask(clsPrefs, "bFreeplayDefaultTeamColors");
		if (!m1) m1 = 0x2;
		if (!m2) m2 = 0x4;
		if (offPrefsBits >= 0) prefsMask = m1 | (off2 == offPrefsBits ? m2 : 0);
		void* cdo = ue::FindObject("TeamColorPreferences_TA TAGame.Default__TeamColorPreferences_TA");
		uint32_t bits = (cdo && offPrefsBits >= 0) ? ue::Read<uint32_t>(cdo, offPrefsBits) : 0xFFFFFFFF;
		if (cdo && prefsMask && !(bits & prefsMask)) cleanPrefs = cdo;
		zl::Log("colors: prefs=%x bits=%x mask=%x (%x/%x) clean=%p (default bits %x)", offColorPrefs, offPrefsBits, prefsMask, m1, m2, cleanPrefs, bits);
	}
	zl::Log("colors: mesh=%p apply=%p (team=%x custom=%x) overrides=%x/%x previewMesh=%x carMesh=%x", clsCarMesh, fnApplyColors, offApplyTeam, offApplyCustom,
		offTeamOverride, offCustomOverride, offPreviewMesh, offCarMesh);
	return fnApplyColors != nullptr;
}

// Farben fuer ein Modell (Team aus dem Modell, ausser "fuer beide Teams")
static void LinearColors(void* mesh, float lin[2][4]) {
	int team = 0;
	if (!colorsBoth && offMeshTeam >= 0) team = std::clamp(ue::Read<int32_t>(mesh, offMeshTeam), 0, 1);
	// zwischengespeichert (wird bei jedem Auto-Ereignis gebraucht)
	static float cache[2][2][4];
	static int cacheGen = -1;
	int g = colorGen.load();
	if (cacheGen != g) {
		for (int t = 0; t < 2; t++)
			for (int c = 0; c < 2; c++) {
				for (int k = 0; k < 3; k++) cache[t][c][k] = std::max(powf(std::clamp(colors[t][c][k], 0.0f, 1.0f), 2.2f), 0.0004f); // nie ganz 0 (reines Schwarz)
				cache[t][c][3] = 1.0f;
			}
		cacheGen = g;
	}
	memcpy(lin, cache[team], sizeof cache[team]);
}

// Das Spiel legt Farben ueber das Ereignis ApplyColorsToCar an: dort direkt unsere Farben einsetzen
static void PatchColorEvent(void* mesh, void* params) {
	if (!colorsOn || !params || offApplyTeam < 0 || offApplyCustom < 0) return;
	if (mesh != ourMeshes[0].load() && mesh != ourMeshes[1].load()) return;
	float lin[2][4];
	LinearColors(mesh, lin);
	memcpy(reinterpret_cast<uint8_t*>(params) + offApplyTeam, lin[0], 16);
	memcpy(reinterpret_cast<uint8_t*>(params) + offApplyCustom, lin[1], 16);
	static int n = 0;
	if (n++ < 6) zl::Log("colors: game applied colors on %s -> replaced", ue::Name(mesh).c_str());
}

// Dein Modell: Standard-Teamfarben aus, unsere Farben als Override eintragen (nur Speicher, kein Aufruf -
// geht auch mitten im Aufbau des Autos, damit es gar nicht erst in der normalen Farbe erscheint)
static void MarkOurs(void* mesh) {
	if (offColorPrefs >= 0 && cleanPrefs) {
		void* cur = ue::Read<void*>(mesh, offColorPrefs);
		if (cur != cleanPrefs) {
			// Original merken - auch nachholen, wenn das Modell beim ersten Mal noch keine Einstellungen hatte
			// (sonst findet das Freeplay die geteilte Einstellung "Standard-Teamfarben" nicht mehr)
			auto po = prefsOrig.find(mesh);
			if (po == prefsOrig.end() || (!po->second && cur)) prefsOrig[mesh] = cur;
			*ue::Ptr<void*>(mesh, offColorPrefs) = cleanPrefs;
			static int n = 0;
			if (n++ < 12) zl::Log("colors: %s uses its own color settings (was %p, bits %x)", ue::Name(mesh).c_str(), cur,
				(cur && offPrefsBits >= 0) ? ue::Read<uint32_t>(cur, offPrefsBits) : 0);
		}
	}
	float lin[2][4];
	LinearColors(mesh, lin);
	if (offTeamOverride >= 0) memcpy(reinterpret_cast<uint8_t*>(mesh) + offTeamOverride, lin[0], 16);
	if (offCustomOverride >= 0) memcpy(reinterpret_cast<uint8_t*>(mesh) + offCustomOverride, lin[1], 16);
}

// Stimmt bei deinem Modell noch alles (Override + Einstellungen)?
static bool ColorsIntact(void* mesh) {
	if (offColorPrefs >= 0 && cleanPrefs && ue::Read<void*>(mesh, offColorPrefs) != cleanPrefs) return false;
	float lin[2][4];
	LinearColors(mesh, lin);
	if (offTeamOverride >= 0 && memcmp(reinterpret_cast<uint8_t*>(mesh) + offTeamOverride, lin[0], 16) != 0) return false;
	if (offCustomOverride >= 0 && memcmp(reinterpret_cast<uint8_t*>(mesh) + offCustomOverride, lin[1], 16) != 0) return false;
	return true;
}


// Eigene Farben aus: auf allen Modellen, die wir angefasst haben, Overrides loeschen, die echten
// Farb-Einstellungen zurueckgeben und die Lackierung neu anwenden lassen
struct MyMesh { unsigned long long last = 0, born = 0; bool pending = false, copy = false; int step = 0; };
static std::unordered_map<void*, MyMesh> myMeshes; // deine Modelle (auch Replay-Kopien) -> Stand
static void ClearOurColors() {
	std::set<void*> meshes;
	for (auto& [m, g] : colorsApplied) meshes.insert(m);
	for (auto& [m, p] : prefsOrig) meshes.insert(m);
	meshes.insert(ourMeshes[0].load());
	meshes.insert(ourMeshes[1].load());
	void* fnClear = clsCarMesh ? ue::FindFunction(clsCarMesh, "ClearColorOverrides") : nullptr;
	int n = 0;
	for (void* m : meshes) {
		if (!m || !ue::IsAlive(m) || !clsCarMesh || !ue::IsA(m, clsCarMesh)) continue;
		if (offColorPrefs >= 0 && cleanPrefs && ue::Read<void*>(m, offColorPrefs) == cleanPrefs) {
			auto o = prefsOrig.find(m);
			*ue::Ptr<void*>(m, offColorPrefs) = o != prefsOrig.end() ? o->second : nullptr;
		}
		// Overrides loescht das Spiel selbst; NICHT mit 0 ueberschreiben - 0 ist fuer das Spiel "schwarz"
		if (fnClear) ue::Call(m, fnClear);
		else {
			static const float none[4] = {0, 0, 0, -1};
			if (offTeamOverride >= 0) memcpy(reinterpret_cast<uint8_t*>(m) + offTeamOverride, none, 16);
			if (offCustomOverride >= 0) memcpy(reinterpret_cast<uint8_t*>(m) + offCustomOverride, none, 16);
		}
		if (void* fnPaint = ue::FindFunction(ue::ClassOf(m), "ApplyPaintSettings")) ue::Call(m, fnPaint);
		n++;
	}
	RestoreSharedPrefs();
	prefsOrig.clear();
	colorsApplied.clear();
	myMeshes.clear();
	ourMeshes[0] = nullptr;
	ourMeshes[1] = nullptr;
	zl::Log("colors: off -> game colors back on %d model(s) (clear=%p)", n, fnClear);
}

// Farben auf ein Auto-Modell legen (merkt sich pro Modell, ob es schon die aktuellen Farben hat)
static void ApplyColorsTo(void* mesh, bool force) {
	if (!mesh || !ue::IsAlive(mesh) || !EnsureColors() || offApplyTeam < 0 || offApplyCustom < 0) return;
	int gen = colorGen.load();
	if (!force && colorsApplied[mesh] == gen) return;
	MarkOurs(mesh);
	float lin[2][4];
	LinearColors(mesh, lin);
	// Falls das Spiel die Einstellung trotzdem liest: waehrend unseres Aufrufs "Standard-Teamfarben" aus
	void* pr = offColorPrefs >= 0 ? ue::Read<void*>(mesh, offColorPrefs) : nullptr;
	uint32_t* bits = (pr && offPrefsBits >= 0 && prefsMask && ue::IsValid(pr)) ? ue::Ptr<uint32_t>(pr, offPrefsBits) : nullptr;
	uint32_t saved = bits ? *bits : 0;
	if (bits) *bits &= ~prefsMask;
	// Die Lackierung vom Spiel selbst neu aufbauen lassen: es nimmt dabei unsere Overrides (MarkOurs) UND die
	// Lackierung der Items (z. B. Black Octane). ApplyColorsToCar direkt aufzurufen hat die Lackierung des
	// Autos geloescht (Auto war kurz schwarz und dann wieder normal).
	void* fnPaint = ue::FindFunction(ue::ClassOf(mesh), "ApplyPaintSettings");
	if (fnPaint) ue::Call(mesh, fnPaint);
	else {
		std::vector<uint8_t> p(size_t(ue::StructSize(fnApplyColors)) + 16, 0);
		memcpy(p.data() + offApplyTeam, lin[0], 16);
		memcpy(p.data() + offApplyCustom, lin[1], 16);
		ue::Call(mesh, fnApplyColors, p.data());
	}
	static int logged = 0;
	if (logged++ < 3) zl::Log("colors: applied on %s via %s", ue::Name(mesh).c_str(), fnPaint ? "ApplyPaintSettings" : "ApplyColorsToCar");
	if (bits) *bits = saved;
	colorsApplied[mesh] = gen;
	if (colorsApplied.size() > 48) // alte Modelle (z. B. nach vielen Resets) vergessen
		for (auto it = colorsApplied.begin(); it != colorsApplied.end();) it = ue::IsAlive(it->first) ? std::next(it) : colorsApplied.erase(it);
	if (prefsOrig.size() > 48)
		for (auto it = prefsOrig.begin(); it != prefsOrig.end();) it = ue::IsAlive(it->first) ? std::next(it) : prefsOrig.erase(it);
}

// Ereignis eines Autos: ist es deins und hat es ein neues Modell (Reset/Respawn), sofort markieren.
// So baut das Spiel das neue Auto direkt in deinen Farben auf.
// ---- Farben der Replay-Kopie: Farb-Parameter der Materialien von deinem echten Auto uebernehmen ----
// Das Replay lackiert seine Kopie mit den aufgezeichneten Spiel-Farben und schaut dabei nicht auf unsere
// Overrides. Dein echtes Auto hat die richtigen Farben in seinen Materialien - genau diese Werte werden
// auf die Kopie geschrieben (gleiches Auto = gleiche Materialien).
struct ColorParam { int nameIdx, nameNum; float v[4]; };
static std::map<int, std::vector<ColorParam>> liveColorParams; // Element -> Farb-Parameter deines Autos
static void* MeshMaterial(void* mesh, int i) {
	static void* fn = nullptr;
	static int oIdx = -1, oRet = -1;
	if (!fn) {
		void* cls = ue::FindClass("Class Engine.MeshComponent");
		fn = cls ? ue::FindFunction(cls, "GetMaterial") : nullptr;
		if (fn) { oIdx = ue::PropOffset(fn, "ElementIndex"); oRet = ue::PropOffset(fn, "ReturnValue"); }
	}
	if (!fn || oIdx < 0 || oRet < 0) return nullptr;
	std::vector<uint8_t> p(size_t(ue::StructSize(fn)) + 16, 0);
	*reinterpret_cast<int32_t*>(p.data() + oIdx) = i;
	ue::Call(mesh, fn, p.data());
	return *reinterpret_cast<void**>(p.data() + oRet);
}
static bool ColorName(const std::string& n) {
	std::string l = LowerS(n);
	for (const char* k : {"color", "colour", "paint", "tint", "team", "accent", "primary", "secondary"})
		if (l.find(k) != std::string::npos) return true;
	return false;
}
static void SnapshotLiveColors(void* mesh) {
	if (!MicSetup()) return;
	std::map<int, std::vector<ColorParam>> snap;
	for (int i = 0; i < 12; i++) {
		void* m = MeshMaterial(mesh, i);
		if (!m) continue;
		if (!ue::IsA(m, clsMIC)) continue;
		auto* arr = ue::Ptr<ue::TArray<uint8_t>>(m, oVecParams);
		for (int k = 0; arr->data && k < arr->count && k < 96; k++) {
			uint8_t* e = arr->data + size_t(k) * vpStride;
			ue::FName n = *reinterpret_cast<ue::FName*>(e + vpName);
			if (!ColorName(ue::NameOf(n.index))) continue;
			ColorParam cp{n.index, n.number, {}};
			memcpy(cp.v, e + vpValue, 16);
			snap[i].push_back(cp);
		}
	}
	static int logs = 0;
	if (logs++ < 3) {
		size_t n = 0;
		for (auto& [i, v] : snap) n += v.size();
		zl::Log("colors: your car's color values: %d on %d material(s)", (int)n, (int)snap.size());
	}
	if (!snap.empty()) liveColorParams.swap(snap);
}
static int CopyLiveColors(void* mesh) {
	if (liveColorParams.empty() || !MicSetup()) return 0;
	int n = 0;
	for (auto& [i, params] : liveColorParams) {
		void* m = MeshMaterial(mesh, i);
		if (!m || !ue::IsA(m, clsMIC)) continue;
		auto* arr = ue::Ptr<ue::TArray<uint8_t>>(m, oVecParams);
		for (auto& cp : params) {
			bool same = false;
			for (int k = 0; arr->data && k < arr->count && k < 96; k++) {
				uint8_t* e = arr->data + size_t(k) * vpStride;
				ue::FName fn = *reinterpret_cast<ue::FName*>(e + vpName);
				if (fn.index == cp.nameIdx && fn.number == cp.nameNum) { same = memcmp(e + vpValue, cp.v, 16) == 0; break; }
			}
			if (same) continue;
			MicSet(m, ue::FName{cp.nameIdx, cp.nameNum}, cp.v);
			n++;
		}
	}
	return n;
}

// Ist das dein Auto? Auch die Kopie deines Autos im Tor-Replay (eigener Replay-Spieler mit deinem Namen).
static bool IsMyCar(void* car) {
	static std::unordered_map<void*, std::pair<void*, bool>> cache; // Auto -> (PRI, deins?)
	void* local = localPRIForColors.load();
	void* pri = ue::Read<void*>(car, offPawnPRI);
	if (pri && pri == local) return true;
	// Tor-Replay im Freeplay: dort faehrt nur dein Auto - die Replay-Kopie ist also deine
	if (inFreeplay.load() && ue::IsA(car, clsCar) && !(clsPreview && ue::IsA(car, clsPreview))) {
		if (localPlayers.size() <= 1 || !pri) {
			static int n = 0;
			if (pri != local && n++ < 4) zl::Log("colors: replay copy of your car: %s", ue::Name(car).c_str());
			return true;
		}
	}
	if (!pri) return false;
	auto c = cache.find(car);
	if (c != cache.end() && c->second.first == pri) return c->second.second;
	bool mine = false;
	static int offName = clsPRI ? ue::PropOffset(clsPRI, "PlayerName") : -1;
	if (local && offName >= 0 && ue::IsValid(pri) && ue::IsValid(local)) {
		auto* a = ue::Ptr<ue::FString>(pri, offName);
		auto* b = ue::Ptr<ue::FString>(local, offName);
		mine = a->data && b->data && a->count > 1 && a->count == b->count && wcsncmp(a->data, b->data, a->count) == 0;
	}
	if (cache.size() > 64) cache.clear();
	cache[car] = {pri, mine};
	return mine;
}

// Bei jedem Ereignis eines Autos (auch beim Neuaufbau nach Reset, Tor-Replay): sobald das Spiel deine Farben
// ersetzt hat, sofort wieder unsere setzen - im selben Bild, bevor es angezeigt wird.
static void ApplyColorsTo(void* mesh, bool force);
static void CarEventColors(void* car) {
	if (!colorsOn || offCarMesh < 0 || offPawnPRI < 0 || !clsCarMesh) return;
	if (!IsMyCar(car)) return;
	void* m = ue::Read<void*>(car, offCarMesh);
	if (!m) return;
	unsigned long long now = GetTickCount64();
	auto it = myMeshes.find(m);
	bool fresh = it == myMeshes.end();
	if (fresh) {
		if (!ue::IsAlive(m) || !ue::IsA(m, clsCarMesh)) return;
		if (myMeshes.size() > 32)
			for (auto i = myMeshes.begin(); i != myMeshes.end();) i = ue::IsAlive(i->first) ? std::next(i) : myMeshes.erase(i);
		MyMesh st;
		st.born = now;
		st.copy = ue::Read<void*>(car, offPawnPRI) != localPRIForColors.load(); // z. B. dein Auto im Tor-Replay (eigener Replay-Spieler)
		it = myMeshes.emplace(m, st).first;
		MarkOurs(m);
		if (st.copy && ue::Read<void*>(car, offPawnPRI)) {
			static int n = 0;
			if (n++ < 6) zl::Log("colors: your car in a replay (%s) -> your colors", ue::Name(car).c_str());
		}
	}
	MyMesh& st = it->second;
	st.copy = ue::Read<void*>(car, offPawnPRI) != localPRIForColors.load(); // jedes Mal neu (beim Spawn ist der Spieler kurz noch leer)
	if (!fresh && !ColorsIntact(m)) { MarkOurs(m); st.pending = true; } // Spiel hat unsere Farben ersetzt
	// Replay-Kopien: das Replay lackiert das Auto beim Abspielen neu -> anfangs mehrmals, danach regelmaessig
	bool copyDue = false;
	if (st.copy) {
		static const unsigned long long at[] = {120, 400, 1000, 2000};
		unsigned long long age = now - st.born;
		if (st.step < 4 && age >= at[st.step]) { copyDue = true; st.step++; }
		else if (st.step >= 4 && now - st.last >= 500) copyDue = true;
	}
	if (fresh || copyDue || (st.pending && now - st.last >= 30)) { // Neu-Lackieren hoechstens alle 30 ms
		st.last = now;
		st.pending = false;
		ApplyColorsTo(m, true);
		if (st.copy) {
			int n = CopyLiveColors(m);
			static int logs = 0;
			if (logs++ < 6) zl::Log("colors: replay car %s -> %d color value(s) from your car (known %d material(s))", ue::Name(car).c_str(), n, (int)liveColorParams.size());
		}
	}
	// Kopie: jedes Bild pruefen (nur Speicher lesen), ob das Replay die Farben wieder ueberschrieben hat
	else if (st.copy && now - st.last >= 50) { st.last = now; CopyLiveColors(m); }
}

// Dein Auto im Spiel: nicht mehr jedes Bild (das war teuer und hat bei Resets kurz eingefroren), sondern
// sofort bei einem neuen Modell, danach kurz oefter (Teile laden nach) und dann nur noch zur Kontrolle.
// Freeplay: dort faehrt nur dein Auto. "Use Default Team Colors" / Standardfarben im Freeplay werden dort
// fuer die Zeit im Freeplay ganz ausgeschaltet (und beim Verlassen wieder an), sonst setzt das Spiel
// bei deinem Auto immer wieder blau/orange.
static void* sharedPrefs = nullptr;
static uint32_t sharedSaved = 0;
static void RestoreSharedPrefs() {
	if (!sharedPrefs) return;
	if (ue::IsAlive(sharedPrefs) && offPrefsBits >= 0) {
		uint32_t* b = ue::Ptr<uint32_t>(sharedPrefs, offPrefsBits);
		*b = (*b & ~prefsMask) | (sharedSaved & prefsMask);
	}
	zl::Log("colors: default team colors setting back as it was");
	sharedPrefs = nullptr;
}
static bool FreeplayPrefs(void* car, void* mesh) {
	if (!clsFreeplayCar || !ue::IsA(car, clsFreeplayCar) || offColorPrefs < 0 || offPrefsBits < 0 || !prefsMask) return false;
	void* pr = ue::Read<void*>(mesh, offColorPrefs);
	if (pr == cleanPrefs) {
		auto o = prefsOrig.find(mesh);
		pr = o != prefsOrig.end() ? o->second : nullptr;
	}
	if (!pr || pr == cleanPrefs || !ue::IsAlive(pr)) return false;
	uint32_t* b = ue::Ptr<uint32_t>(pr, offPrefsBits);
	if (!(*b & prefsMask)) return false;
	if (sharedPrefs != pr) { RestoreSharedPrefs(); sharedPrefs = pr; sharedSaved = *b; }
	*b &= ~prefsMask;
	zl::Log("colors: freeplay -> default team colors off while you're here (bits %x)", sharedSaved);
	return true;
}

static void TickCarColors(void* car) {
	if (!car || offCarMesh < 0 || !EnsureColors()) return;
	void* m = ue::Read<void*>(car, offCarMesh);
	if (!m || !ue::IsAlive(m)) return;
	if (FreeplayPrefs(car, m)) { // Einstellung gerade umgestellt: Lackierung neu aufbauen lassen
		if (void* fnPaint = ue::FindFunction(ue::ClassOf(m), "ApplyPaintSettings")) ue::Call(m, fnPaint);
		ourMeshes[1] = nullptr;
	}
	static unsigned long long seenAt = 0, next = 0, lastFull = 0;
	static int lastGen = -1;
	unsigned long long now = GetTickCount64();
	bool fresh = m != ourMeshes[1].load();
	if (fresh) {
		zl::Log("colors: your car's model %p (%s)", m, ue::Name(m).c_str());
		ourMeshes[1] = m;
		seenAt = now;
	}
	int gen = colorGen.load();
	if (!fresh && gen == lastGen && now < next) return;
	// Nach dem Spawn laden Teile nach: kurz danach noch zweimal neu anwenden, sonst nur, wenn das Spiel
	// unsere Farben ersetzt hat (kein staendiges Neu-Lackieren -> keine kleinen Ruckler)
	unsigned long long age = now - seenAt;
	bool early = age < 1500;
	next = now + (early ? 0 : 250);
	static int earlyStep = 0;
	if (fresh) earlyStep = 0;
	bool due = fresh || gen != lastGen || !ColorsIntact(m) || (earlyStep == 0 && age >= 300) || (earlyStep == 1 && age >= 1000);
	// Farbwerte deines echten Autos fuer Replay-Kopien merken (nicht vom Replay-Auto selbst)
	bool live = offPawnPRI >= 0 && ue::Read<void*>(car, offPawnPRI) == localPRIForColors.load();
	static unsigned long long lastSnap = 0;
	if (!due && live && now - lastSnap > 2000) { lastSnap = now; try { SnapshotLiveColors(m); } catch (...) {} }
	if (!due) return;
	if (!fresh && age >= 300 && earlyStep < 2) earlyStep = age >= 1000 ? 2 : 1;
	lastGen = gen;
	lastFull = now;
	ApplyColorsTo(m, true);
	if (live) try { SnapshotLiveColors(m); } catch (...) {}
}

static void TickColors(void* previewActor, void* car) {
	if (!colorsOn) return;
	if (car) {
		TickCarColors(car);
		if (!previewActor) return;
	}
	static unsigned long long next = 0;
	unsigned long long t = GetTickCount64();
	bool beat = t >= next; // alle 0,4 s erneut, weil das Spiel die Farben beim Neuladen ueberschreibt
	if (beat) next = t + 400;
	if (!beat && colorsApplied.size() && colorGen.load() == colorsApplied.begin()->second) return;
	if (!EnsureColors()) return;
	if (previewActor && offPreviewMesh >= 0) {
		void* m = ue::Read<void*>(previewActor, offPreviewMesh);
		ourMeshes[0] = m;
		ApplyColorsTo(m, beat);
	}
}

void SetColors(bool on, bool both, const float (*cols)[2][3], bool save) {
	{
		std::lock_guard<std::mutex> lk(mu);
		bool wasOn = colorsOn;
		colorsOn = on;
		colorsBoth = both;
		if (cols) memcpy(colors, cols, sizeof colors);
		if (wasOn && !on) { // aus: Spiel-Farben zurueck -> Karosserie neu laden bzw. Auto neu spawnen
			Queue([] {
				ClearOurColors();
				applyAt = GetTickCount64() + 150; // im Freeplay: Auto frisch spawnen, damit es sicher die Spiel-Farben hat
				for (void* a : previewActors) {
					if (!ue::IsAlive(a)) continue;
					auto* pr = ue::Ptr<ue::TArray<int32_t>>(a, offPrevLoadout);
					if (!pr->data || pr->count <= 0) continue;
					auto* onl = ue::Ptr<ue::TArray<OnlineId>>(a, offPrevLoadout + offLoadoutOnline);
					ChangePart(a, 0, pr->data[0], (onl->data && onl->count > 0) ? onl->data[0] : OnlineId{});
					break;
				}
			});
			applyAt = GetTickCount64() + 400;
		}
	}
	colorGen++;
	nextPreviewCheck = 0;
	if (save) Save();
}

// ---------- Eigene Decals/Baelle: Modelle sammeln und anwenden ----------
static void* BallMesh(void* ball) {
	static void* clsBallC = ue::FindClass("Class TAGame.Ball_TA");
	static void* clsSMC = ue::FindClass("Class Engine.StaticMeshComponent");
	static int oSM = clsBallC ? ue::PropOffset(clsBallC, "StaticMesh") : -1;
	static int oComps = -1;
	static bool init = false;
	if (!init) {
		init = true;
		if (void* clsActor = ue::FindClass("Class Engine.Actor")) oComps = ue::PropOffset(clsActor, "Components");
		zl::Log("customs: ball mesh prop=%x components=%x", oSM, oComps);
	}
	if (oSM >= 0) {
		void* m = ue::Read<void*>(ball, oSM);
		if (m && ue::IsAlive(m)) return m;
	}
	if (oComps >= 0 && clsSMC) {
		auto* arr = ue::Ptr<ue::TArray<void*>>(ball, oComps);
		for (int i = 0; arr->data && i < arr->count && i < 32; i++)
			if (arr->data[i] && ue::IsA(arr->data[i], clsSMC)) return arr->data[i];
	}
	return nullptr;
}

static void TickCustoms() {
	static unsigned long long next = 0;
	unsigned long long now = GetTickCount64();
	if (now < next) return;
	next = now + 15;
	if (!EnsureColors()) return;
	std::vector<void*> meshes, balls;
	int body = 0;
	void* car = LocalCar();
	if (car && offCarMesh >= 0) {
		if (void* m = ue::Read<void*>(car, offCarMesh)) meshes.push_back(m);
		std::lock_guard<std::mutex> lk(mu);
		body = overrides.count(0) ? overrides[0] : (ownItems.count(0) ? ownItems[0] : 0);
	} else if (offPreviewMesh >= 0) {
		for (void* a : previewActors) {
			if (!ue::IsAlive(a)) continue;
			if (void* m = ue::Read<void*>(a, offPreviewMesh)) meshes.push_back(m);
			auto* pr = ue::Ptr<ue::TArray<int32_t>>(a, offPrevLoadout);
			if (!body && pr->data && pr->count > 0) body = pr->data[0];
		}
	}
	// Baelle im aktuellen Spiel
	if (car) {
		void* pri = LocalPRI();
		void* ge = (pri && offPRIGameEvent >= 0) ? ue::Read<void*>(pri, offPRIGameEvent) : nullptr;
		static int oBalls = -1;
		if (ge && ue::IsValid(ge)) {
			if (oBalls < 0) oBalls = ue::PropOffset(ue::ClassOf(ge), "GameBalls");
			if (oBalls >= 0) {
				auto* arr = ue::Ptr<ue::TArray<void*>>(ge, oBalls);
				for (int i = 0; arr->data && i < arr->count && i < 8; i++)
					if (arr->data[i] && ue::IsAlive(arr->data[i]))
						if (void* bm = BallMesh(arr->data[i])) balls.push_back(bm);
			}
		}
	}
	customs::Tick(meshes, body, balls);
}

// ---------- Presets ----------
std::vector<Preset> presets;
static std::wstring PresetFile() { return zl::DataDir() + L"\\zlpresets.txt"; }

void SavePresets() {
	FILE* f = _wfopen(PresetFile().c_str(), L"wb");
	if (!f) return;
	std::lock_guard<std::mutex> lk(mu);
	for (auto& p : presets) {
		fputs(p.name.c_str(), f);
		fputc('\t', f);
		bool first = true;
		for (auto& [s, id] : p.items) {
			auto pp = p.paints.find(s);
			if (pp != p.paints.end()) fprintf(f, "%s%d:%d:%d", first ? "" : ",", s, id, pp->second);
			else fprintf(f, "%s%d:%d", first ? "" : ",", s, id);
			first = false;
		}
		if (p.hasCustoms) fprintf(f, "\tcd=%s\tcb=%s", p.customDecal.c_str(), p.customBall.c_str());
		fputc('\n', f);
	}
	fclose(f);
}

void LoadPresets() {
	FILE* f = _wfopen(PresetFile().c_str(), L"rb");
	if (!f) return;
	std::vector<Preset> out;
	static char line[16384];
	while (fgets(line, sizeof line, f)) {
		for (size_t n = strlen(line); n && (line[n - 1] == '\n' || line[n - 1] == '\r');) line[--n] = 0;
		char* tab = strchr(line, '\t');
		if (!tab) continue;
		*tab = 0;
		Preset p;
		p.name = line;
		// Zusatzfelder (eigenes Decal/Ball) hinter weiteren Tabs
		if (char* extra = strchr(tab + 1, '\t')) {
			*extra = 0;
			for (char* fld = extra + 1; fld;) {
				char* nx = strchr(fld, '\t');
				if (nx) *nx++ = 0;
				if (!strncmp(fld, "cd=", 3)) { p.customDecal = fld + 3; p.hasCustoms = true; }
				else if (!strncmp(fld, "cb=", 3)) { p.customBall = fld + 3; p.hasCustoms = true; }
				fld = nx;
			}
		}
		for (char* q = tab + 1; *q;) {
			int s = 0, id = 0, used = 0;
			if (sscanf(q, "%d:%d%n", &s, &id, &used) != 2) break;
			if (id > 0) p.items[s] = id;
			q += used;
			int paint = 0, used2 = 0;
			if (*q == ':' && sscanf(q, ":%d%n", &paint, &used2) == 1) {
				if (paint > 0) p.paints[s] = paint;
				q += used2;
			}
			if (*q == ',') q++;
			else break;
		}
		if (!p.name.empty()) out.push_back(std::move(p));
	}
	fclose(f);
	std::lock_guard<std::mutex> lk(mu);
	presets = std::move(out);
	zl::Log("presets: %d", (int)presets.size());
}

static std::string CleanName(const std::string& raw) {
	std::string name;
	for (char c : raw)
		if (c != '\t' && c != '\n' && c != '\r') name += c;
	while (!name.empty() && name.back() == ' ') name.pop_back();
	while (!name.empty() && name.front() == ' ') name.erase(name.begin());
	return name;
}
static bool SameName(const std::string& a, const std::string& b) {
	if (a.size() != b.size()) return false;
	for (size_t i = 0; i < a.size(); i++)
		if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
	return true;
}
// aktuelle Items in ein Preset schreiben (mu muss gesperrt sein)
static void FillPreset(Preset& p) {
	p.items = overrides;
	p.paints = paints; // auch Lackierungen deiner eigenen Items (ohne gewaehltes Item im Slot)
	std::lock_guard<std::mutex> lk(customs::mu);
	p.hasCustoms = true;
	p.customDecal = customs::selDecal;
	p.customBall = customs::selBall;
}

int FindPreset(const std::string& rawName) {
	std::string name = CleanName(rawName);
	if (name.empty()) return -1;
	std::lock_guard<std::mutex> lk(mu);
	for (int i = 0; i < (int)presets.size(); i++)
		if (SameName(presets[i].name, name)) return i;
	return -1;
}

void AddPreset(const std::string& rawName) {
	std::string name = CleanName(rawName);
	{
		std::lock_guard<std::mutex> lk(mu);
		if (name.empty()) name = "Preset " + std::to_string(presets.size() + 1);
		bool found = false;
		for (auto& p : presets) // gleicher Name: dieses Preset ueberschreiben statt doppelt anlegen
			if (!found && SameName(p.name, name)) {
				p.name = name;
				FillPreset(p);
				found = true;
			}
		if (!found) {
			Preset p;
			p.name = name;
			FillPreset(p);
			presets.insert(presets.begin(), std::move(p)); // neuestes oben
		}
	}
	SavePresets();
}

void RenamePreset(int i, const std::string& rawName) {
	std::string name = CleanName(rawName);
	if (name.empty()) return;
	{
		std::lock_guard<std::mutex> lk(mu);
		if (i < 0 || i >= (int)presets.size()) return;
		presets[i].name = name;
	}
	SavePresets();
}

void UpdatePreset(int i, const std::string& rawName) {
	std::string name = CleanName(rawName);
	{
		std::lock_guard<std::mutex> lk(mu);
		if (i < 0 || i >= (int)presets.size()) return;
		if (!name.empty()) presets[i].name = name;
		FillPreset(presets[i]);
	}
	SavePresets();
}

void DeletePreset(int i) {
	{
		std::lock_guard<std::mutex> lk(mu);
		if (i < 0 || i >= (int)presets.size()) return;
		presets.erase(presets.begin() + i);
	}
	SavePresets();
}

void ApplyPreset(int i) {
	std::map<int, int> next, before;
	bool hasCustoms = false;
	std::string cDecal, cBall;
	{
		std::lock_guard<std::mutex> lk(mu);
		if (i < 0 || i >= (int)presets.size()) return;
		hasCustoms = presets[i].hasCustoms;
		cDecal = presets[i].customDecal;
		cBall = presets[i].customBall;
		next = presets[i].items;
		before = overrides;
		overrides = next;
		paints = presets[i].paints;
	}
	userGen++;
	Queue([next, before] {
		for (auto& [slot, id] : before)
			if (!next.count(slot)) restore.insert(slot); // Slots, die das Preset nicht hat: eigenes Item
		for (auto& [slot, id] : next) restore.erase(slot);
	});
	nextPreviewCheck = 0;
	applyAt = GetTickCount64() + 400;
	Save();
	if (hasCustoms) {
		for (auto* id : {&cDecal, &cBall}) { // nur, wenn es das Custom noch gibt
			std::lock_guard<std::mutex> lk(customs::mu);
			bool found = false;
			for (auto& c : customs::list) if (c.id == *id) found = true;
			if (!found) id->clear();
		}
		customs::Select(false, cDecal);
		customs::Select(true, cBall);
	}
}

// Alles zurueck auf deine echten Items (Lackierungen, Finishes und eigene Farben aus)
void ResetAll() {
	std::map<int, int> before;
	{
		std::lock_guard<std::mutex> lk(mu);
		before = overrides;
		overrides.clear();
		paints.clear();
	}
	userGen++;
	Queue([before] {
		for (auto& [slot, id] : before) restore.insert(slot);
		paintFailed.clear();
	});
	SetColors(false, colorsBoth, nullptr, false);
	customs::Select(false, ""); // eigenes Decal und eigener Ball aus
	customs::Select(true, "");
	nextPreviewCheck = 0;
	applyAt = GetTickCount64() + 400;
	Save();
	zl::Log("reset: everything back to your own items");
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
		else if (sscanf(p, "\"p%d\":%d", &s, &id) == 2 && id > 0) paints[s] = id;
		else if (sscanf(p, "\"co\":%d", &id) == 1) colorsOn = id != 0;
		else if (sscanf(p, "\"cb\":%d", &id) == 1) colorsBoth = id != 0;
		else {
			const char* keys[4] = {"\"cp\":%d", "\"ca\":%d", "\"cq\":%d", "\"cr\":%d"};
			for (int k = 0; k < 4; k++)
				if (sscanf(p, keys[k], &id) == 1) {
					float* c = colors[k / 2][k % 2];
					c[0] = ((id >> 16) & 255) / 255.0f; c[1] = ((id >> 8) & 255) / 255.0f; c[2] = (id & 255) / 255.0f;
				}
		}
		p = strchr(p + 1, '"');
		if (!p) break;
		p++;
	}
	zl::Log("saved items: %d", (int)overrides.size());
}

} // namespace items
