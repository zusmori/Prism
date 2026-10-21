#include "items.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <set>

#include "log.h"
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
	std::lock_guard<std::mutex> lk(mu);
	list = std::move(out);
	loaded = !list.empty();
	status = loaded ? "" : "No items found. See zlmod.log";
}

// ---------- Loadout ----------
// Lokaler PlayerController: per PlayerTick erkannt, sonst ueber LocalPlayer.Actor bzw. Player != null
static void* LocalPC() {
	if (void* pc = localPC.load()) {
		if (ue::IsAlive(pc)) return pc;
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

static void WritePaints(void* pri) {
	if (!attrArraysOk || offClientOnline < 0) return;
	std::map<int, int> ov, pt;
	{
		std::lock_guard<std::mutex> lk(mu);
		ov = overrides;
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
}

// Stimmen die Farben im PRI?
static bool PaintsOk(void* pri) {
	if (!attrArraysOk || offClientOnline < 0) return true;
	std::map<int, int> ov, pt;
	{
		std::lock_guard<std::mutex> lk(mu);
		ov = overrides;
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
	return true;
}

static void WriteOverrides(void* pri) {
	static void* origPRI = nullptr;
	if (pri != origPRI) { // neuer Spieler/neues Spiel: eigene Items neu merken
		originals[0].clear();
		originals[1].clear();
		ResetPaintState();
		origPRI = pri;
	}
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
	try { WritePaints(pri); } catch (...) { zl::Log("ERROR: paint crashed (caught)"); }
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

void OnProcessEvent(void* caller, void* fn, void* params) {
	if (!ready || !caller || !fn) return;
	if (fn == fnApplyColors && fnApplyColors) { try { PatchColorEvent(caller, params); } catch (...) {} }
	static unsigned long long nextCheck = 0;
	unsigned long long now = GetTickCount64();
	// Ereignisse eines Autos (z. B. beim Spawnen) -> sofort pruefen, bevor es sein Loadout baut
	static void* lastCls = nullptr;
	static bool lastIsCar = false;
	void* cls = ue::ClassOf(caller);
	if (cls != lastCls) { lastCls = cls; lastIsCar = clsCar && ue::IsA(caller, clsCar); }
	if (lastIsCar) try { CarEventColors(caller); } catch (...) {}
	// Autos feuern sehr viele Ereignisse pro Bild - hoechstens alle 4 ms pruefen
	static unsigned long long nextCarCheck = 0;
	if ((lastIsCar && now >= nextCarCheck) || now >= nextCheck) {
		nextCheck = now + 40;
		nextCarCheck = now + 4;
		try { EnsureLocalLoadout(); } catch (...) {}
	}
	if (tickFns.count(fn)) { // lokaler Spieler
		// LocalPlayer merken (gibt es auch schon im Hauptmenue)
		try {
			void* lp = ue::Read<void*>(caller, offPlayer);
			if (lp && clsLocalPlayer && std::find(localPlayers.begin(), localPlayers.end(), lp) == localPlayers.end() && ue::IsA(lp, clsLocalPlayer)) {
				localPlayers.push_back(lp);
				zl::Log("local player: %p (%s)", lp, ue::Name(lp).c_str());
			}
		} catch (...) {}
		// Im Hauptmenue tickt auch ein Controller - der zaehlt nur, wenn er wirklich ein Auto faehrt
		void* pawn = nullptr;
		try { pawn = (ue::IsA(caller, clsPC) && offPawn >= 0) ? ue::Read<void*>(caller, offPawn) : nullptr; } catch (...) {}
		if (!pawn || !clsCar || !ue::IsA(pawn, clsCar)) {
			// Hauptmenue/Garage: sicherer Moment (einmal pro Frame), um das Vorschau-Auto anzupassen
			try { TickPreview(); } catch (...) {}
			// Item-Liste schon im Hauptmenue laden (wird fuer Farben gebraucht, auch ohne F8)
			static unsigned long long nextLoad = 0;
			if (!loaded && GetTickCount64() >= nextLoad) {
				nextLoad = GetTickCount64() + 5000;
				try { LoadProducts(); } catch (...) {}
			}
			return;
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
			verifyAt = GetTickCount64() + 1500; // und kurz danach am fertigen Auto nachsehen
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
		// Das Auto kann schon gebaut sein, bevor unsere Items im Loadout stehen (dann faehrt man kurz das eigene
		// Auto). Am fertigen Auto nachsehen und im Freeplay einmal sauber neu spawnen.
		if (verifyAt && GetTickCount64() >= verifyAt) {
			verifyAt = 0;
			int miss = -1;
			try { miss = CarMissesItems(pawn); } catch (...) {}
			if (miss < 0 && verifyTries < 8) { verifyTries++; verifyAt = GetTickCount64() + 500; } // noch nicht fertig geladen
			else if (miss == 1 && verifyTries < 10) {
				verifyTries = 10;
				zl::Log("verify: car was built without your items -> respawn");
				applyAt = GetTickCount64();
			} else if (miss == 0) {
				static int ok = 0;
				if (ok++ < 5) zl::Log("verify: car has your items");
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
struct OnlineId { uint64_t a = 0, b = 0; bool operator==(const OnlineId& o) const { return a == o.a && b == o.b; } };
struct OwnPart { int id = 0; OnlineId online; };
struct PreviewState {
	std::map<int, OwnPart> orig;  // Slot -> eigenes Item (mit Online-ID, z. B. deine Lackierung)
	std::map<int, int> lastSeen;  // Slot -> zuletzt gesehener Wert
	std::map<int, long long> target; // Slot -> zuletzt gesetztes Item + Farbe
	std::map<int, unsigned long long> until; // Slot -> nicht vor diesem Zeitpunkt erneut tauschen
	int gen = -1;
};
static std::map<void*, PreviewState> previews;
static std::vector<void*> previewActors;
static unsigned long long nextPreviewScan = 0;
static std::atomic<unsigned long long> nextPreviewCheck{0};
static std::atomic<int> userGen{0}; // steigt bei jeder Aenderung im Menue -> Wartezeiten aufheben
std::map<int, int> ownItems;        // Slot -> dein echtes Item (fuer die Anzeige)

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
	auto it = paintedIds.find(key);
	if (it != paintedIds.end()) { out = it->second; return true; }
	static std::set<long long> failed;
	if (failed.count(key)) return false;
	failed.insert(key); // nur einmal versuchen; bei Erfolg unten wieder raus
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
	// Was soll im Slot sein? Gewaehltes Item - oder dein eigenes Item, wenn du es lackieren willst
	std::map<int, int> desired = ov;
	for (auto& [slot, paint] : pt) {
		if (ov.count(slot) || slot >= prods->count) continue;
		auto o = st.orig.find(slot);
		int ownId = o != st.orig.end() ? o->second.id : prods->data[slot];
		if (WantedPaint(slot, ownId, pt)) desired[slot] = ownId;
	}
	for (auto& [slot, id] : desired) {
		if (slot >= prods->count) continue;
		int want = WantedPaint(slot, id, pt);
		OnlineId wantId;
		if (want && !PaintedId(id, want, wantId)) want = 0;
		long long tgt = (long long)id * 1000 + want;
		int now = prods->data[slot];
		bool ok = now == id && onlineAt(slot) == wantId;
		if (ok) { st.lastSeen[slot] = now; st.target[slot] = tgt; continue; }
		auto ls = st.lastSeen.find(slot);
		auto tg = st.target.find(slot);
		if (ls != st.lastSeen.end() && ls->second == now && tg != st.target.end() && tg->second == tgt) continue; // schon gesetzt
		unsigned long long t = GetTickCount64();
		if (t < st.until[slot]) continue; // nicht staendig neu laden (z. B. beim Durchscrollen in der Garage)
		st.until[slot] = t + 2500;
		if (!st.orig.count(slot)) st.orig[slot] = OwnPart{now, onlineAt(slot)}; // dein eigenes Item
		ChangePart(actor, slot, id, wantId);
		st.lastSeen[slot] = prods->data[slot];
		st.target[slot] = tgt;
		zl::Log("menu car %s: slot %d %d -> %d%s (now %d)", ue::Name(actor).c_str(), slot, now, id, want ? " painted" : "", prods->data[slot]);
	}
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
	// Deine echten Items merken (fuer die Liste: "Your own" zeigt den Namen)
	if (main) {
		std::map<int, int> own;
		for (int i = 0; i < kSlotCount; i++) {
			int slot = kSlots[i].index;
			if (slot >= prods->count) continue;
			auto o = st.orig.find(slot);
			if (o != st.orig.end()) own[slot] = o->second.id;
			else if (!ov.count(slot)) own[slot] = prods->data[slot];
		}
		std::lock_guard<std::mutex> lk(mu);
		for (auto& [k, v] : own) ownItems[k] = v;
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

void Set(int slot, int id) {
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
		if (fnApplyColors) { offApplyTeam = ue::PropOffset(fnApplyColors, "TeamColor"); offApplyCustom = ue::PropOffset(fnApplyColors, "CustomColor"); }
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
	for (int c = 0; c < 2; c++) {
		for (int k = 0; k < 3; k++) lin[c][k] = std::max(powf(std::clamp(colors[team][c][k], 0.0f, 1.0f), 2.2f), 0.0004f); // nie ganz 0 (reines Schwarz)
		lin[c][3] = 1.0f;
	}
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
			if (!prefsOrig.count(mesh)) prefsOrig[mesh] = cur;
			*ue::Ptr<void*>(mesh, offColorPrefs) = cleanPrefs;
			static int n = 0;
			if (n++ < 4) zl::Log("colors: %s uses its own color settings (was %p, bits %x)", ue::Name(mesh).c_str(), cur,
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
		if (fnClear) ue::Call(m, fnClear);
		static const float zero[4] = {0, 0, 0, 0};
		if (offTeamOverride >= 0) memcpy(reinterpret_cast<uint8_t*>(m) + offTeamOverride, zero, 16);
		if (offCustomOverride >= 0) memcpy(reinterpret_cast<uint8_t*>(m) + offCustomOverride, zero, 16);
		if (void* fnPaint = ue::FindFunction(ue::ClassOf(m), "ApplyPaintSettings")) ue::Call(m, fnPaint);
		n++;
	}
	prefsOrig.clear();
	colorsApplied.clear();
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
	std::vector<uint8_t> p(size_t(ue::StructSize(fnApplyColors)) + 16, 0);
	memcpy(p.data() + offApplyTeam, lin[0], 16);
	memcpy(p.data() + offApplyCustom, lin[1], 16);
	ue::Call(mesh, fnApplyColors, p.data());
	if (bits) *bits = saved;
	colorsApplied[mesh] = gen;
	if (colorsApplied.size() > 48) // alte Modelle (z. B. nach vielen Resets) vergessen
		for (auto it = colorsApplied.begin(); it != colorsApplied.end();) it = ue::IsAlive(it->first) ? std::next(it) : colorsApplied.erase(it);
	if (prefsOrig.size() > 48)
		for (auto it = prefsOrig.begin(); it != prefsOrig.end();) it = ue::IsAlive(it->first) ? std::next(it) : prefsOrig.erase(it);
}

// Ereignis eines Autos: ist es deins und hat es ein neues Modell (Reset/Respawn), sofort markieren.
// So baut das Spiel das neue Auto direkt in deinen Farben auf.
static void CarEventColors(void* car) {
	if (!colorsOn || offCarMesh < 0 || offPawnPRI < 0 || !clsCarMesh) return;
	void* pri = localPRIForColors.load();
	if (!pri || ue::Read<void*>(car, offPawnPRI) != pri) return;
	void* m = ue::Read<void*>(car, offCarMesh);
	if (!m || m == ourMeshes[1].load() || !ue::IsAlive(m) || !ue::IsA(m, clsCarMesh)) return;
	MarkOurs(m);
}

// Dein Auto im Spiel: nicht mehr jedes Bild (das war teuer und hat bei Resets kurz eingefroren), sondern
// sofort bei einem neuen Modell, danach kurz oefter (Teile laden nach) und dann nur noch zur Kontrolle.
static void TickCarColors(void* car) {
	if (!car || offCarMesh < 0 || !EnsureColors()) return;
	void* m = ue::Read<void*>(car, offCarMesh);
	if (!m || !ue::IsAlive(m)) return;
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
	bool early = now - seenAt < 1500;
	next = now + (early ? 100 : 1000);
	if (!fresh && gen == lastGen && !early && ColorsIntact(m) && now - lastFull < 5000) return;
	lastGen = gen;
	lastFull = now;
	ApplyColorsTo(m, true);
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
		fputc('\n', f);
	}
	fclose(f);
}

void LoadPresets() {
	FILE* f = _wfopen(PresetFile().c_str(), L"rb");
	if (!f) return;
	std::vector<Preset> out;
	char line[2048];
	while (fgets(line, sizeof line, f)) {
		char* tab = strchr(line, '\t');
		if (!tab) continue;
		*tab = 0;
		Preset p;
		p.name = line;
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
	p.paints.clear();
	for (auto& [slot, pid] : paints)
		if (overrides.count(slot)) p.paints[slot] = pid;
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
	{
		std::lock_guard<std::mutex> lk(mu);
		if (i < 0 || i >= (int)presets.size()) return;
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
