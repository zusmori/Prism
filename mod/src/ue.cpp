#include "ue.h"

#include <psapi.h>
#include <sstream>
#include <unordered_map>

#include "log.h"

namespace ue {

static TArray<uint8_t*>* GNames = nullptr;   // TArray<FNameEntry*>
static TArray<uint8_t*>* GObjects = nullptr; // TArray<UObject*>
ProcessEventFn ProcessEventOrig = nullptr;

// ---------- Musters suche im Spielmodul ----------

static bool ModuleRange(uintptr_t& base, size_t& size) {
	HMODULE m = GetModuleHandleW(nullptr);
	MODULEINFO mi{};
	if (!m || !GetModuleInformation(GetCurrentProcess(), m, &mi, sizeof mi)) return false;
	base = reinterpret_cast<uintptr_t>(m);
	size = mi.SizeOfImage;
	return true;
}

static uintptr_t FindPattern(const char* pattern) {
	uintptr_t base;
	size_t size;
	if (!ModuleRange(base, size)) return 0;
	std::vector<int> bytes;
	std::stringstream ss(pattern);
	std::string b;
	while (ss >> b) bytes.push_back(b == "??" ? -1 : std::stoi(b, nullptr, 16));
	const size_t n = bytes.size();
	// nur lesbare Bereiche durchsuchen
	for (uintptr_t p = base; p < base + size;) {
		MEMORY_BASIC_INFORMATION mbi{};
		if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof mbi)) break;
		uintptr_t start = reinterpret_cast<uintptr_t>(mbi.BaseAddress), end = start + mbi.RegionSize;
		bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
			(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY));
		if (readable && end - start >= n) {
			const uint8_t* mem = reinterpret_cast<const uint8_t*>(start);
			for (size_t i = 0; i + n <= end - start; i++) {
				size_t j = 0;
				for (; j < n; j++)
					if (bytes[j] != -1 && mem[i + j] != bytes[j]) break;
				if (j == n) return start + i;
			}
		}
		p = end;
	}
	return 0;
}

static std::wstring EntryName(uint8_t* entry) {
	if (!entry) return L"";
	const wchar_t* w = reinterpret_cast<const wchar_t*>(entry + off::NameEntryName);
	return std::wstring(w, wcsnlen(w, 256));
}

static bool ValidNames(TArray<uint8_t*>* names) {
	try {
		if (!names || !names->data || names->count < 1000 || names->count > 5000000 || names->max < names->count) return false;
		return EntryName(names->data[0]) == L"None" && EntryName(names->data[1]) == L"ByteProperty";
	} catch (...) {
		return false;
	}
}

static bool ValidObjects(TArray<uint8_t*>* objs) {
	try {
		if (!objs || !objs->data || objs->count < 10000 || objs->count > 20000000 || objs->max < objs->count) return false;
		int good = 0;
		for (int i = 0; i < 64 && i < objs->count; i++) {
			uint8_t* o = objs->data[i];
			if (o && Read<void*>(o, off::Class)) good++;
		}
		return good > 32;
	} catch (...) {
		return false;
	}
}

// Notloesung: Datenbereich nach einem Array durchsuchen, dessen erste Namen "None" und "ByteProperty" sind
static TArray<uint8_t*>* ScanForNames() {
	uintptr_t base;
	size_t size;
	if (!ModuleRange(base, size)) return nullptr;
	for (uintptr_t p = base; p < base + size;) {
		MEMORY_BASIC_INFORMATION mbi{};
		if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof mbi)) break;
		uintptr_t start = reinterpret_cast<uintptr_t>(mbi.BaseAddress), end = start + mbi.RegionSize;
		if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY))) {
			for (uintptr_t a = start; a + sizeof(TArray<uint8_t*>) <= end; a += 8) {
				auto* t = reinterpret_cast<TArray<uint8_t*>*>(a);
				if (t->count < 1000 || t->count > 5000000 || t->max < t->count || !t->data) continue;
				if (ValidNames(t)) return t;
			}
		}
		p = end;
	}
	return nullptr;
}

bool Init() {
	// 1) Code-Muster (RLSDK-Generator)
	if (uintptr_t a = FindPattern("49 63 4E 08 48 8B 05 ?? ?? ?? ?? 4C 89 34 C8 EB 08")) {
		uintptr_t disp = a + 7;
		auto* names = reinterpret_cast<TArray<uint8_t*>*>(disp + 4 + *reinterpret_cast<int32_t*>(disp));
		zl::Log("GNames pattern 1 at %p -> %p", (void*)a, (void*)names);
		if (ValidNames(names)) GNames = names;
	}
	// 2) Daten-Muster (RocketDumper)
	if (!GNames) {
		if (uintptr_t a = FindPattern("?? ?? ?? ?? ?? ?? 00 00 ?? ?? 01 00 35 25 02 00")) {
			auto* names = reinterpret_cast<TArray<uint8_t*>*>(a);
			zl::Log("GNames pattern 2 at %p", (void*)names);
			if (ValidNames(names)) GNames = names;
		}
	}
	// 3) Datenbereich durchsuchen
	if (!GNames) {
		zl::Log("GNames patterns failed, scanning data");
		GNames = ScanForNames();
	}
	if (!GNames) {
		zl::Log("ERROR: GNames not found");
		return false;
	}
	auto* objs = reinterpret_cast<TArray<uint8_t*>*>(reinterpret_cast<uintptr_t>(GNames) + 0x48);
	if (!ValidObjects(objs)) {
		// GObjects liegt meist 0x48 hinter GNames; sonst in der Naehe suchen
		objs = nullptr;
		for (int d = -0x200; d <= 0x200 && !objs; d += 8) {
			auto* t = reinterpret_cast<TArray<uint8_t*>*>(reinterpret_cast<uintptr_t>(GNames) + d);
			if (t != GNames && ValidObjects(t)) objs = t;
		}
	}
	if (!objs) {
		zl::Log("ERROR: GObjects not found");
		return false;
	}
	GObjects = objs;
	zl::Log("GNames %p (%d names), GObjects %p (%d objects)", (void*)GNames, GNames->count, (void*)GObjects, GObjects->count);
	return true;
}

bool Ready() { return GNames && GObjects; }

bool CheckObjects(void* obj) {
	if (!obj || !GNames) return false;
	try {
		int idx = Read<int32_t>(obj, off::Index);
		if (GObjects && idx >= 0 && idx < GObjects->count && GObjects->data[idx] == obj) return true;
		zl::Log("object %p (index %d) not in GObjects %p (count %d) - searching", obj, idx, (void*)GObjects, GObjects ? GObjects->count : 0);
		for (int d = -0x2000; d <= 0x2000; d += 8) {
			auto* t = reinterpret_cast<TArray<uint8_t*>*>(reinterpret_cast<uintptr_t>(GNames) + d);
			try {
				if (t->data && idx >= 0 && idx < t->count && t->count <= t->max && t->data[idx] == obj) {
					GObjects = t;
					zl::Log("GObjects fixed: %p (GNames%+d, %d objects)", (void*)t, d, t->count);
					return true;
				}
			} catch (...) {}
		}
	} catch (...) {}
	zl::Log("GObjects check failed");
	return false;
}

int ObjectCount() { return GObjects ? GObjects->count : 0; }
void* ObjectAt(int i) { return (GObjects && i >= 0 && i < GObjects->count) ? GObjects->data[i] : nullptr; }

std::string NameOf(int index) {
	try {
		if (!GNames || index < 0 || index >= GNames->count) return "";
		uint8_t* e = GNames->data[index];
		if (!e) return "";
		const wchar_t* w = reinterpret_cast<const wchar_t*>(e + off::NameEntryName);
		std::string s;
		for (int i = 0; i < 1024 && w[i]; i++) s += (w[i] < 128) ? char(w[i]) : '?';
		return s;
	} catch (...) {
		return "";
	}
}

std::string Name(const void* obj) {
	if (!obj) return "";
	FName n = Read<FName>(obj, off::Name);
	std::string s = NameOf(n.index);
	if (n.number > 0) s += "_" + std::to_string(n.number - 1);
	return s;
}
void* ClassOf(const void* obj) { return obj ? Read<void*>(obj, off::Class) : nullptr; }
void* OuterOf(const void* obj) { return obj ? Read<void*>(obj, off::Outer) : nullptr; }

std::string FullName(const void* obj) {
	if (!obj || !ClassOf(obj)) return "";
	std::string path = Name(obj);
	for (void* o = OuterOf(obj); o; o = OuterOf(o)) path = Name(o) + "." + path;
	return Name(ClassOf(obj)) + " " + path;
}

void* FindObject(const char* fullName) {
	static std::unordered_map<std::string, void*> cache;
	if (auto it = cache.find(fullName); it != cache.end()) return it->second;
	// zuerst ueber den Namen vorfiltern (schnell), dann den vollen Namen pruefen
	std::string full(fullName);
	std::string last = full.substr(full.find_last_of(". ") + 1);
	for (int i = 0; i < ObjectCount(); i++) {
		void* o = ObjectAt(i);
		if (!o) continue;
		try {
			if (Name(o) != last) continue;
			if (FullName(o) == full) {
				cache[full] = o;
				return o;
			}
		} catch (...) {}
	}
	return nullptr;
}

void* FindClass(const char* fullName) { return FindObject(fullName); }

bool IsA(const void* obj, const void* cls) {
	if (!obj || !cls) return false;
	for (void* c = ClassOf(obj); c; c = Read<void*>(c, off::StructSuper))
		if (c == cls) return true;
	return false;
}

std::vector<void*> Instances(void* cls) {
	std::vector<void*> out;
	if (!cls) return out;
	for (int i = 0; i < ObjectCount(); i++) {
		void* o = ObjectAt(i);
		try {
			if (o && IsA(o, cls) && Name(o).rfind("Default__", 0) != 0) out.push_back(o);
		} catch (...) {}
	}
	return out;
}

static void* FindChild(void* strct, const char* name, bool function) {
	for (void* s = strct; s; s = Read<void*>(s, off::StructSuper)) {
		for (void* f = Read<void*>(s, off::StructChildren); f; f = Read<void*>(f, off::FieldNext)) {
			if (Name(f) != name) continue;
			std::string cn = Name(ClassOf(f));
			bool isFn = cn == "Function";
			bool isProp = cn.size() > 8 && cn.compare(cn.size() - 8, 8, "Property") == 0;
			if ((function && isFn) || (!function && isProp)) return f;
		}
	}
	return nullptr;
}

int PropOffset(void* cls, const char* name) {
	try {
		void* p = FindChild(cls, name, false);
		return p ? Read<int32_t>(p, off::PropOffset) : -1;
	} catch (...) {
		return -1;
	}
}

void* FindFunction(void* cls, const char* name) {
	try {
		return FindChild(cls, name, true);
	} catch (...) {
		return nullptr;
	}
}

int StructSize(void* strct) { return strct ? Read<int32_t>(strct, off::StructSize) : 0; }

std::string Narrow(const FString& s) {
	std::string out;
	if (!s.data || s.count <= 0 || s.count > 4096) return out;
	for (int i = 0; i < s.count && s.data[i]; i++) {
		wchar_t c = s.data[i];
		if (c < 0x80) out += char(c);
		else if (c < 0x800) { out += char(0xC0 | (c >> 6)); out += char(0x80 | (c & 0x3F)); }
		else { out += char(0xE0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
	}
	return out;
}

void* ProcessEventAddress() {
	// ProcessEvent steht in der VTable jedes UObject an Index 67
	for (int i = 0; i < ObjectCount(); i++) {
		void* o = ObjectAt(i);
		if (!o) continue;
		void** vt = Read<void**>(o, 0);
		if (vt) return vt[off::ProcessEventIndex];
	}
	return nullptr;
}

void Call(void* obj, void* fn, void* params) {
	if (!obj || !fn) return;
	std::vector<uint8_t> buf;
	if (!params) {
		buf.resize(size_t(StructSize(fn)) + 16, 0);
		params = buf.data();
	}
	auto pe = ProcessEventOrig ? ProcessEventOrig : reinterpret_cast<ProcessEventFn>(ProcessEventAddress());
	if (pe) pe(obj, fn, params, nullptr);
}

} // namespace ue
