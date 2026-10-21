// Maps tauschen im F8-Menue.
// Gleiches Verfahren wie der Launcher (main.go: Swap/restoreOne):
//   - vor dem ersten Tausch wird die Original-Arena nach %APPDATA%\RLMapSwap\backup\<Arena> kopiert
//   - die Map-Datei ersetzt dann CookedPCConsole\<Arena>
//   - state.json merkt sich Arena -> Map-Name
//   - Restore kopiert die Sicherung zurueck
// Kopiert wird in einem eigenen Thread, damit das Spiel nicht haengt.
#include "maps.h"

#include <d3d11.h>
#include <wincodec.h>
#include <windows.h>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <thread>
#include <unordered_map>

#include "hooks.h"
#include "items.h"
#include "log.h"
#include "ue.h"

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")

namespace maps {

std::mutex mu;
std::vector<Map> list;
std::vector<Arena> arenas;
bool scanned = false, found = false;
std::string busyArena, busyMap, msg;
bool msgErr = false;
double msgAt = -100;

static std::atomic<bool> busy{false};
std::atomic<bool>& Busy() { return busy; }

// ---------- Hilfen ----------

static std::wstring W(const std::string& s) {
	if (s.empty()) return L"";
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
	std::wstring w(n, 0);
	MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
	return w;
}
static std::string U8(const std::wstring& w) {
	if (w.empty()) return "";
	int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s(n, 0);
	WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
	return s;
}
static std::string LowerA(std::string s) {
	for (auto& c : s) c = (char)tolower((unsigned char)c);
	return s;
}
static std::wstring LowerW(std::wstring s) {
	for (auto& c : s) c = (wchar_t)towlower(c);
	return s;
}
static bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
static bool IsDir(const std::wstring& p) {
	DWORD a = GetFileAttributesW(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static std::wstring Parent(const std::wstring& p) {
	size_t i = p.find_last_of(L"\\/");
	return i == std::wstring::npos ? L"" : p.substr(0, i);
}
static std::wstring Stem(const std::wstring& p) {
	size_t a = p.find_last_of(L"\\/");
	std::wstring f = a == std::wstring::npos ? p : p.substr(a + 1);
	size_t d = f.find_last_of(L'.');
	return d == std::wstring::npos ? f : f.substr(0, d);
}
static bool ReadFileU8(const std::wstring& p, std::string& out) {
	HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	LARGE_INTEGER sz{};
	GetFileSizeEx(h, &sz);
	if (sz.QuadPart > (64ll << 20)) { CloseHandle(h); return false; }
	out.resize((size_t)sz.QuadPart);
	DWORD got = 0;
	BOOL ok = out.empty() || ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr);
	CloseHandle(h);
	out.resize(got);
	return ok != FALSE;
}
static bool WriteFileU8(const std::wstring& p, const std::string& data) {
	std::wstring tmp = p + L".tmp";
	HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	DWORD put = 0;
	BOOL ok = WriteFile(h, data.data(), (DWORD)data.size(), &put, nullptr);
	CloseHandle(h);
	if (!ok || put != data.size()) { DeleteFileW(tmp.c_str()); return false; }
	if (!MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(tmp.c_str()); return false; }
	return true;
}

// ---------- kleines JSON (nur Objekte und Texte werden gebraucht) ----------

struct J {
	bool obj = false;
	std::string str;
	std::map<std::string, J> kids;
	const J* get(const std::string& k) const {
		auto i = kids.find(k);
		return i == kids.end() ? nullptr : &i->second;
	}
};
struct JP {
	const char* p;
	const char* e;
	void ws() { while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++; }
	static void put(std::string& s, unsigned c) {
		if (c < 0x80) s += (char)c;
		else if (c < 0x800) { s += (char)(0xC0 | (c >> 6)); s += (char)(0x80 | (c & 63)); }
		else if (c < 0x10000) { s += (char)(0xE0 | (c >> 12)); s += (char)(0x80 | ((c >> 6) & 63)); s += (char)(0x80 | (c & 63)); }
		else { s += (char)(0xF0 | (c >> 18)); s += (char)(0x80 | ((c >> 12) & 63)); s += (char)(0x80 | ((c >> 6) & 63)); s += (char)(0x80 | (c & 63)); }
	}
	unsigned hex4() {
		unsigned v = 0;
		for (int i = 0; i < 4 && p < e; i++, p++) {
			char c = *p;
			v = v * 16 + (c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0);
		}
		return v;
	}
	bool str(std::string& out) {
		if (p >= e || *p != '"') return false;
		p++;
		while (p < e && *p != '"') {
			if (*p == '\\' && p + 1 < e) {
				p++;
				char c = *p++;
				switch (c) {
				case 'n': out += '\n'; break;
				case 't': out += '\t'; break;
				case 'r': out += '\r'; break;
				case 'b': out += '\b'; break;
				case 'f': out += '\f'; break;
				case 'u': {
					unsigned u = hex4();
					if (u >= 0xD800 && u < 0xDC00 && p + 6 <= e && p[0] == '\\' && p[1] == 'u') {
						p += 2;
						unsigned lo = hex4();
						u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
					}
					put(out, u);
					break;
				}
				default: out += c;
				}
			} else out += *p++;
		}
		if (p < e) p++;
		return true;
	}
	bool value(J& v, int depth) {
		ws();
		if (p >= e || depth > 32) return false;
		if (*p == '"') return str(v.str);
		if (*p == '{') {
			v.obj = true;
			p++;
			ws();
			if (p < e && *p == '}') { p++; return true; }
			while (p < e) {
				ws();
				std::string k;
				if (!str(k)) return false;
				ws();
				if (p >= e || *p != ':') return false;
				p++;
				if (!value(v.kids[k], depth + 1)) return false;
				ws();
				if (p < e && *p == ',') { p++; continue; }
				if (p < e && *p == '}') { p++; return true; }
				return false;
			}
			return false;
		}
		if (*p == '[') { // Listen werden nicht gebraucht: ueberspringen
			p++;
			ws();
			if (p < e && *p == ']') { p++; return true; }
			while (p < e) {
				J skip;
				if (!value(skip, depth + 1)) return false;
				ws();
				if (p < e && *p == ',') { p++; continue; }
				if (p < e && *p == ']') { p++; return true; }
				return false;
			}
			return false;
		}
		// Zahl, true, false, null
		const char* s = p;
		while (p < e && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') p++;
		v.str.assign(s, p);
		return p > s;
	}
};
static bool ParseJson(const std::string& text, J& out) {
	JP jp{text.data(), text.data() + text.size()};
	if (text.size() >= 3 && (unsigned char)text[0] == 0xEF) jp.p += 3; // BOM
	return jp.value(out, 0);
}
static std::string Quote(const std::string& s) {
	std::string o = "\"";
	for (unsigned char c : s) {
		if (c == '"') o += "\\\"";
		else if (c == '\\') o += "\\\\";
		else if (c == '\n') o += "\\n";
		else if (c == '\r') o += "\\r";
		else if (c == '\t') o += "\\t";
		else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
		else o += (char)c;
	}
	return o + "\"";
}

// ---------- Orte ----------

static std::wstring DataDir() { return zl::DataDir(); }
static std::wstring BackupDir() { return DataDir() + L"\\backup"; }
static std::wstring StateFile() { return DataDir() + L"\\state.json"; }

static bool IsCooked(const std::wstring& dir) {
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW((dir + L"\\*_P.upk").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return false;
	FindClose(h);
	return true;
}

// CookedPCConsole: neben der laufenden RocketLeague.exe (Binaries\Win64), sonst wie im Launcher eingestellt
static std::wstring FindCooked() {
	wchar_t buf[MAX_PATH * 2];
	DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
	if (n) {
		std::wstring root = Parent(Parent(Parent(std::wstring(buf, n)))); // ...\rocketleague
		std::wstring c = root + L"\\TAGame\\CookedPCConsole";
		if (IsCooked(c)) return c;
	}
	std::string cfg;
	J j;
	if (ReadFileU8(DataDir() + L"\\config.json", cfg) && ParseJson(cfg, j))
		if (const J* c = j.get("Cooked")) {
			std::wstring p = W(c->str);
			if (IsCooked(p)) return p;
		}
	return L"";
}

static std::vector<std::wstring> SteamLibraries(const std::wstring& cooked) {
	std::vector<std::wstring> libs;
	auto add = [&](std::wstring p) {
		while (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
		for (auto& c : p) if (c == L'/') c = L'\\';
		if (p.empty()) return;
		for (auto& l : libs) if (LowerW(l) == LowerW(p)) return;
		libs.push_back(p);
	};
	// Bibliothek, in der das Spiel liegt: ...\steamapps\common\rocketleague\TAGame\CookedPCConsole
	if (!cooked.empty()) {
		std::wstring sa = Parent(Parent(Parent(Parent(cooked)))); // ...\steamapps
		if (LowerW(Stem(sa)) == L"steamapps") add(Parent(sa));
	}
	HKEY k;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
		wchar_t v[MAX_PATH * 2];
		DWORD sz = sizeof v, type = 0;
		if (RegQueryValueExW(k, L"SteamPath", nullptr, &type, reinterpret_cast<BYTE*>(v), &sz) == ERROR_SUCCESS && type == REG_SZ) {
			std::wstring sp(v, wcsnlen(v, sz / sizeof(wchar_t)));
			add(sp);
			std::string vdf;
			if (ReadFileU8(sp + L"\\steamapps\\libraryfolders.vdf", vdf)) {
				size_t pos = 0;
				while ((pos = vdf.find("\"path\"", pos)) != std::string::npos) {
					pos += 6;
					size_t a = vdf.find('"', pos);
					if (a == std::string::npos) break;
					size_t b = a + 1;
					std::string val;
					while (b < vdf.size() && vdf[b] != '"') {
						if (vdf[b] == '\\' && b + 1 < vdf.size()) b++;
						val += vdf[b++];
					}
					pos = b + 1;
					add(W(val));
				}
			}
		}
		RegCloseKey(k);
	}
	return libs;
}

// ---------- Arenen ----------

struct KnownArena {
	const char* stem;
	const char* name;
};
// Nur diese Arenen haelt Rocket League nicht dauerhaft offen (wie im Launcher)
static const KnownArena kArenas[] = {
	{"park_p", "Beckwith Park"},      {"farm_p", "Farmstead"},        {"beach_p", "Salty Shores"},
	{"labs_underpass_p", "Underpass"}, {"utopiastadium_p", "Utopia Coliseum"}, {"wasteland_s_p", "Wasteland"},
};

static std::wstring cooked;

// Laesst sich die Datei gerade ersetzen? (nicht vom Spiel geladen)
static bool Replaceable(const std::wstring& p) {
	const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
	for (DWORD access : {DWORD(DELETE), DWORD(GENERIC_WRITE)}) {
		HANDLE h = CreateFileW(p.c_str(), access, share, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); return true; }
		DWORD e = GetLastError();
		if (e != ERROR_SHARING_VIOLATION && e != ERROR_LOCK_VIOLATION) return true; // anderer Grund: nicht als "belegt" melden
	}
	return false;
}

static std::map<std::string, std::string> LoadState() {
	std::map<std::string, std::string> m;
	std::string t;
	J j;
	if (ReadFileU8(StateFile(), t) && ParseJson(t, j) && j.obj)
		for (auto& [k, v] : j.kids) if (!v.obj && !v.str.empty()) m[k] = v.str;
	return m;
}
static bool SaveState(const std::map<std::string, std::string>& m) {
	std::string o = "{";
	bool first = true;
	for (auto& [k, v] : m) {
		o += first ? "\n  " : ",\n  ";
		first = false;
		o += Quote(k) + ": " + Quote(v);
	}
	o += m.empty() ? "}" : "\n}";
	return WriteFileU8(StateFile(), o);
}
// state.json-Schluessel gross/klein egal finden
static std::string StateKey(const std::map<std::string, std::string>& m, const std::string& fileU8) {
	for (auto& [k, v] : m) if (LowerA(k) == LowerA(fileU8)) return k;
	return "";
}

// ---------- Einlesen ----------

static std::string FavKey(const Map& m) { return m.id.empty() ? U8(m.path) : m.id; }
// Favoriten oben, dann nach Titel
static void SortMaps(std::vector<Map>& ms) {
	std::stable_sort(ms.begin(), ms.end(), [](const Map& a, const Map& b) {
		if (a.fav != b.fav) return a.fav;
		return LowerA(a.title) < LowerA(b.title);
	});
}

void ToggleFav(const std::wstring& path) {
	std::lock_guard<std::mutex> lk(mu);
	std::string key;
	bool on = false;
	for (auto& m : list)
		if (m.path == path) { m.fav = !m.fav; on = m.fav; key = FavKey(m); }
	if (key.empty()) return;
	// Datei lesen und nur diesen Eintrag aendern (andere Favoriten bleiben)
	std::map<std::string, bool> fv;
	std::string t;
	J fj;
	if (ReadFileU8(DataDir() + L"\\favorites.json", t) && ParseJson(t, fj) && fj.obj)
		for (auto& [k, v] : fj.kids) if (v.str == "true") fv[k] = true;
	if (on) fv[key] = true;
	else fv.erase(key);
	std::string o = "{";
	bool first = true;
	for (auto& [k, v] : fv) { o += (first ? "\n  " : ",\n  ") + Quote(k) + ": true"; first = false; }
	o += fv.empty() ? "}" : "\n}";
	WriteFileU8(DataDir() + L"\\favorites.json", o);
	SortMaps(list);
}

static void ScanNow() {
	std::wstring c = FindCooked();
	std::vector<Arena> ar;
	if (!c.empty()) {
		auto state = LoadState();
		WIN32_FIND_DATAW fd;
		std::map<std::string, std::wstring> files; // klein -> echter Dateiname
		HANDLE h = FindFirstFileW((c + L"\\*_P.upk").c_str(), &fd);
		if (h != INVALID_HANDLE_VALUE) {
			do files[LowerA(U8(Stem(fd.cFileName)))] = fd.cFileName;
			while (FindNextFileW(h, &fd));
			FindClose(h);
		}
		for (auto& k : kArenas) {
			auto f = files.find(k.stem);
			if (f == files.end()) continue;
			Arena a;
			a.file = f->second;
			a.fileU8 = U8(a.file);
			a.name = k.name;
			std::string key = StateKey(state, a.fileU8);
			if (!key.empty()) a.swap = state[key];
			a.locked = !Replaceable(c + L"\\" + a.file);
			ar.push_back(a);
		}
	}

	// Titel aus dem Launcher (meta.json: id -> {title, author})
	// Favoriten (gleiche Datei wie frueher im Launcher: id oder Pfad -> true)
	std::set<std::string> fv;
	{
		std::string t;
		J fj;
		if (ReadFileU8(DataDir() + L"\\favorites.json", t) && ParseJson(t, fj) && fj.obj)
			for (auto& [k, v] : fj.kids) if (v.str == "true") fv.insert(k);
	}
	J meta;
	{
		std::string t;
		if (ReadFileU8(DataDir() + L"\\meta.json", t)) ParseJson(t, meta);
	}
	std::vector<std::wstring> roots{DataDir() + L"\\My Maps"};
	for (auto& lib : SteamLibraries(c)) roots.push_back(lib + L"\\steamapps\\workshop\\content\\252950");

	std::vector<Map> ms;
	std::vector<std::wstring> seen;
	std::function<void(const std::wstring&, const std::wstring&, std::string, int)> walk =
		[&](const std::wstring& root, const std::wstring& dir, std::string id, int depth) {
			if (depth > 4) return;
			WIN32_FIND_DATAW fd;
			HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
			if (h == INVALID_HANDLE_VALUE) return;
			do {
				std::wstring n = fd.cFileName;
				if (n == L"." || n == L"..") continue;
				std::wstring p = dir + L"\\" + n;
				if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
					std::string sub = id;
					if (depth == 0) { // erster Ordner unter der Wurzel: Workshop-ID, wenn nur Ziffern
						std::string nn = U8(n);
						bool digits = !nn.empty() && std::all_of(nn.begin(), nn.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
						sub = digits ? nn : "";
					}
					walk(root, p, sub, depth + 1);
					continue;
				}
				std::wstring lo = LowerW(n);
				if (lo.size() < 5) continue;
				std::wstring ext = lo.substr(lo.size() - 4);
				if (ext != L".udk" && ext != L".upk") continue;
				std::wstring key = LowerW(p);
				if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
				seen.push_back(key);
				Map m;
				m.path = p;
				m.id = id;
				m.name = U8(Stem(n));
				m.title = m.name;
				m.mb = float(((unsigned long long)fd.nFileSizeHigh << 32 | fd.nFileSizeLow) / 1048576.0);
				if (const J* e = id.empty() ? nullptr : meta.get(id)) {
					if (const J* t = e->get("title"); t && !t->str.empty()) m.title = t->str;
					if (const J* a = e->get("author"); a) m.author = a->str;
				}
				m.lower = LowerA(m.title + " " + m.author + " " + m.name);
				m.fav = fv.count(FavKey(m)) > 0;
				ms.push_back(std::move(m));
			} while (FindNextFileW(h, &fd));
			FindClose(h);
		};
	for (auto& r : roots) if (IsDir(r)) walk(r, r, "", 0);
	SortMaps(ms);

	for (auto& a : ar) {
		if (a.swap.empty()) continue;
		a.swapTitle = a.swap;
		for (auto& m : ms) if (LowerA(m.name) == LowerA(a.swap)) { a.swapTitle = m.title; break; }
	}

	std::lock_guard<std::mutex> lk(mu);
	cooked = c;
	found = !c.empty();
	arenas = std::move(ar);
	list = std::move(ms);
	scanned = true;
}

static void Say(const std::string& m, bool err) {
	std::lock_guard<std::mutex> lk(mu);
	msg = m;
	msgErr = err;
	msgAt = GetTickCount64() / 1000.0;
}

static void RunAsync(std::function<void()> fn) {
	bool expected = false;
	if (!busy.compare_exchange_strong(expected, true)) return; // schon beschaeftigt
	std::thread([fn] {
		try {
			fn();
		} catch (...) {
			Say("Something went wrong.", true);
		}
		try { ScanNow(); } catch (...) {}
		{
			std::lock_guard<std::mutex> lk(mu);
			busyArena.clear();
			busyMap.clear();
		}
		busy = false;
	}).detach();
}

void Refresh() {
	RunAsync([] {});
}

// ---------- Kopieren ----------

static std::string FriendlyErr(DWORD e) {
	if (e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION || e == ERROR_USER_MAPPED_FILE)
		return "Rocket League has this arena loaded right now (often it's your main-menu background). Pick another arena.";
	if (e == ERROR_ACCESS_DENIED) return "No write access to the Rocket League folder. Start Prism as administrator.";
	if (e == ERROR_DISK_FULL || e == ERROR_HANDLE_DISK_FULL) return "Your disk is full.";
	char b[64];
	snprintf(b, sizeof b, "Couldn't copy the file (error %lu).", e);
	return b;
}

// Kopiert src nach dst: erst daneben, dann austauschen (wie copyFile/replaceFile im Launcher)
static DWORD CopyOver(const std::wstring& src, const std::wstring& dst) {
	std::wstring tmp = dst + L".rlswap-tmp";
	DeleteFileW(tmp.c_str());
	if (!CopyFileW(src.c_str(), tmp.c_str(), FALSE)) {
		DWORD e = GetLastError();
		DeleteFileW(tmp.c_str());
		return e;
	}
	SetFileAttributesW(tmp.c_str(), FILE_ATTRIBUTE_NORMAL);
	if (MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING)) return 0;
	DWORD err = GetLastError();
	// alte Datei zur Seite schieben, neue einsetzen
	std::wstring aside = dst + L".zms-old-" + std::to_wstring(GetTickCount64());
	if (MoveFileW(dst.c_str(), aside.c_str())) {
		if (MoveFileW(tmp.c_str(), dst.c_str())) {
			DeleteFileW(aside.c_str()); // klappt erst, wenn RL die alte Datei loslaesst
			return 0;
		}
		MoveFileW(aside.c_str(), dst.c_str());
	}
	// direkt ueberschreiben
	if (CopyFileW(src.c_str(), dst.c_str(), FALSE)) { DeleteFileW(tmp.c_str()); return 0; }
	DeleteFileW(tmp.c_str());
	return err;
}

static DWORD RestoreOne(std::map<std::string, std::string>& st, const std::string& key, const std::wstring& c, bool& missing) {
	missing = false;
	std::wstring bk = BackupDir() + L"\\" + W(key);
	if (!Exists(bk)) {
		st.erase(key);
		missing = true;
		return 0;
	}
	if (DWORD e = CopyOver(bk, c + L"\\" + W(key))) return e;
	DeleteFileW(bk.c_str());
	st.erase(key);
	return 0;
}

static std::string ArenaName(const std::string& fileU8) {
	std::string lo = LowerA(fileU8);
	for (auto& k : kArenas) if (lo == std::string(k.stem) + ".upk") return k.name;
	return fileU8;
}

void Swap(const std::wstring& arenaFile, const std::wstring& mapPath) {
	{
		std::lock_guard<std::mutex> lk(mu);
		busyArena = U8(arenaFile);
		busyMap = U8(Stem(mapPath));
	}
	RunAsync([arenaFile, mapPath] {
		std::wstring c;
		{
			std::lock_guard<std::mutex> lk(mu);
			c = cooked;
		}
		if (c.empty()) { Say("Rocket League folder not found.", true); return; }
		std::string fileU8 = U8(arenaFile);
		std::wstring target = c + L"\\" + arenaFile;
		if (!Exists(target)) { Say("Arena file not found.", true); return; }
		if (!Exists(mapPath)) { Say("This map file is gone. Download it again in Prism.", true); return; }
		if (!Replaceable(target)) { Say(FriendlyErr(ERROR_SHARING_VIOLATION), true); return; }
		CreateDirectoryW(BackupDir().c_str(), nullptr);
		auto st = LoadState();
		std::string key = StateKey(st, fileU8);
		if (key.empty()) {
			key = fileU8;
			std::wstring bk = BackupDir() + L"\\" + arenaFile;
			if (DWORD e = CopyOver(target, bk)) { Say("Backup failed: " + FriendlyErr(e), true); return; }
		}
		if (DWORD e = CopyOver(mapPath, target)) { Say(FriendlyErr(e), true); return; }
		std::string mapName = U8(Stem(mapPath));
		st[key] = mapName;
		// eine Map steckt nur in einer Arena: die alte Arena zuruecksetzen
		std::string moveErr;
		std::vector<std::string> others;
		for (auto& [k, v] : st) if (k != key && LowerA(v) == LowerA(mapName)) others.push_back(k);
		for (auto& k : others) {
			bool miss = false;
			if (DWORD e = RestoreOne(st, k, c, miss)) moveErr = "Swapped, but " + ArenaName(k) + " couldn't be restored: " + FriendlyErr(e);
		}
		if (!SaveState(st)) { Say("Swapped, but couldn't save it for Prism.", true); return; }
		zl::Log("maps: %s -> %s", mapName.c_str(), fileU8.c_str());
		if (!moveErr.empty()) Say(moveErr, true);
		else Say("Ready! Start Freeplay in " + ArenaName(fileU8) + ".", false);
	});
}

void Restore(const std::wstring& arenaFile) {
	{
		std::lock_guard<std::mutex> lk(mu);
		busyArena = U8(arenaFile);
		busyMap.clear();
	}
	RunAsync([arenaFile] {
		std::wstring c;
		{
			std::lock_guard<std::mutex> lk(mu);
			c = cooked;
		}
		if (c.empty()) return;
		auto st = LoadState();
		std::string key = StateKey(st, U8(arenaFile));
		if (key.empty()) return;
		bool miss = false;
		if (DWORD e = RestoreOne(st, key, c, miss)) { SaveState(st); Say(FriendlyErr(e), true); return; }
		SaveState(st);
		if (miss) Say("Backup missing for " + ArenaName(key) + ". Use \"Verify integrity of game files\" in Steam.", true);
		else Say(ArenaName(key) + " is back to normal.", false);
	});
}

void RestoreAll() {
	{
		std::lock_guard<std::mutex> lk(mu);
		busyArena = "*";
		busyMap.clear();
	}
	RunAsync([] {
		std::wstring c;
		{
			std::lock_guard<std::mutex> lk(mu);
			c = cooked;
		}
		if (c.empty()) return;
		auto st = LoadState();
		std::vector<std::string> keys;
		for (auto& [k, v] : st) keys.push_back(k);
		std::string missing;
		for (auto& k : keys) {
			bool miss = false;
			if (DWORD e = RestoreOne(st, k, c, miss)) { SaveState(st); Say(FriendlyErr(e), true); return; }
			if (miss) missing += (missing.empty() ? "" : ", ") + ArenaName(k);
		}
		SaveState(st);
		if (!missing.empty()) Say("Backup missing for " + missing + ". Use \"Verify integrity of game files\" in Steam.", true);
		else Say("All arenas are back to normal.", false);
	});
}

// ---------- Freeplay starten ----------

void Play(const std::string& arenaFileU8) {
	std::string stem = arenaFileU8;
	size_t d = stem.find_last_of('.');
	if (d != std::string::npos) stem = stem.substr(0, d);
	std::wstring cmd = L"start " + W(stem) + L"?Game=TAGame.GameInfo_Tutorial_TA?GameTags=Freeplay";
	items::Queue([cmd, stem] {
		void* pcCls = ue::FindClass("Class Engine.PlayerController");
		void* fn = pcCls ? ue::FindFunction(pcCls, "ConsoleCommand") : nullptr;
		int offCmd = fn ? ue::PropOffset(fn, "Command") : -1;
		int offRet = fn ? ue::PropOffset(fn, "ReturnValue") : -1;
		if (!fn || offCmd < 0) { zl::Log("maps: ConsoleCommand not found"); Say("Couldn't start Freeplay. Start it from the game menu.", true); return; }
		// der eigene Spieler: LocalPlayer.Actor (nur dieser Controller fuehrt Befehle aus)
		void* pc = nullptr;
		if (void* lpCls = ue::FindClass("Class Engine.LocalPlayer")) {
			int offActor = ue::PropOffset(lpCls, "Actor");
			if (offActor >= 0)
				for (void* lp : ue::Instances(lpCls)) {
					if (!ue::IsAlive(lp)) continue;
					void* a = ue::Read<void*>(lp, offActor);
					if (a && ue::IsAlive(a)) { pc = a; break; }
				}
		}
		if (!pc) { // sonst ein Controller mit Spieler
			int offPlayer = ue::PropOffset(pcCls, "Player");
			for (void* o : ue::Instances(pcCls))
				if (ue::IsAlive(o) && (offPlayer < 0 || ue::Read<void*>(o, offPlayer))) { pc = o; break; }
		}
		if (!pc) { zl::Log("maps: no local player controller"); Say("Couldn't start Freeplay. Start it from the game menu.", true); return; }
		std::vector<uint8_t> prm(size_t(ue::StructSize(fn)) + 16, 0);
		std::wstring c = cmd;
		auto* s = reinterpret_cast<ue::FString*>(prm.data() + offCmd);
		s->data = c.data();
		s->count = int32_t(c.size() + 1);
		s->max = s->count;
		ue::Call(pc, fn, prm.data());
		std::string ret = offRet >= 0 ? ue::Narrow(*reinterpret_cast<ue::FString*>(prm.data() + offRet)) : "";
		zl::Log("maps: console '%s' via %s -> '%s'", U8(cmd).c_str(), ue::Name(pc).c_str(), ret.c_str());
		// Rueckgabetext gehoert der Engine; unser Befehlstext bleibt unser (nicht freigeben lassen)
		Say("Loading " + ArenaName(stem + ".upk") + " …", false);
	});
}

// ---------- Vorschaubilder ----------

constexpr int kTW = 192, kTH = 108; // 16:9, doppelte Anzeigegroesse

struct ThumbE {
	int state = 0; // 0 neu, 1 wird geladen, 2 Pixel da, 3 fertig, 4 keins
	std::vector<uint8_t> px;
	ID3D11ShaderResourceView* srv = nullptr;
};
static std::mutex tmu;
static std::unordered_map<std::string, ThumbE> thumbs;
static std::deque<std::string> tqueue;
static std::condition_variable tcv;
static bool tworker = false;

static bool Decode(const std::wstring& path, std::vector<uint8_t>& out) {
	static thread_local IWICImagingFactory* f = nullptr;
	if (!f && FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
	IWICBitmapDecoder* dec = nullptr;
	IWICBitmapFrameDecode* fr = nullptr;
	IWICBitmapScaler* sc = nullptr;
	IWICBitmapClipper* cl = nullptr;
	IWICFormatConverter* cv = nullptr;
	bool ok = false;
	do {
		if (FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) break;
		if (FAILED(dec->GetFrame(0, &fr))) break;
		UINT w = 0, h = 0;
		fr->GetSize(&w, &h);
		if (!w || !h) break;
		float s = std::max(float(kTW) / w, float(kTH) / h);
		UINT sw = std::max<UINT>(kTW, UINT(w * s + 0.99f)), sh = std::max<UINT>(kTH, UINT(h * s + 0.99f));
		if (FAILED(f->CreateBitmapScaler(&sc)) || FAILED(sc->Initialize(fr, sw, sh, WICBitmapInterpolationModeFant))) break;
		WICRect r{INT((sw - kTW) / 2), INT((sh - kTH) / 2), kTW, kTH};
		if (FAILED(f->CreateBitmapClipper(&cl)) || FAILED(cl->Initialize(sc, &r))) break;
		if (FAILED(f->CreateFormatConverter(&cv)) ||
			FAILED(cv->Initialize(cl, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) break;
		out.resize(size_t(kTW) * kTH * 4);
		ok = SUCCEEDED(cv->CopyPixels(nullptr, kTW * 4, (UINT)out.size(), out.data()));
	} while (false);
	if (cv) cv->Release();
	if (cl) cl->Release();
	if (sc) sc->Release();
	if (fr) fr->Release();
	if (dec) dec->Release();
	return ok;
}

static void ThumbWorker() {
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	std::wstring dir = DataDir() + L"\\previews\\";
	for (;;) {
		std::string id;
		{
			std::unique_lock<std::mutex> lk(tmu);
			tcv.wait(lk, [] { return !tqueue.empty(); });
			id = tqueue.front();
			tqueue.pop_front();
		}
		std::vector<uint8_t> px;
		bool ok = Decode(dir + W(id) + L".hq.jpg", px) || Decode(dir + W(id) + L".img", px);
		std::lock_guard<std::mutex> lk(tmu);
		ThumbE& t = thumbs[id];
		if (ok) { t.px = std::move(px); t.state = 2; }
		else t.state = 4;
	}
}

unsigned long long Thumb(const std::string& id) {
	if (id.empty()) return 0;
	static int madeThisFrame = 0, frame = -1;
	std::lock_guard<std::mutex> lk(tmu);
	ThumbE& t = thumbs[id];
	if (t.state == 3) return (unsigned long long)(uintptr_t)t.srv;
	if (t.state == 0) {
		t.state = 1;
		tqueue.push_back(id);
		if (!tworker) {
			tworker = true;
			std::thread(ThumbWorker).detach();
		}
		tcv.notify_one();
		return 0;
	}
	if (t.state != 2) return 0;
	// Textur im Render-Thread anlegen, hoechstens ein paar pro Bild
	int f = (int)(GetTickCount64() / 16);
	if (f != frame) { frame = f; madeThisFrame = 0; }
	if (madeThisFrame >= 3) return 0;
	ID3D11Device* dev = hooks::Device();
	if (!dev) return 0;
	madeThisFrame++;
	D3D11_TEXTURE2D_DESC d{};
	d.Width = kTW;
	d.Height = kTH;
	d.MipLevels = 1;
	d.ArraySize = 1;
	d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	d.SampleDesc.Count = 1;
	d.Usage = D3D11_USAGE_IMMUTABLE;
	d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	D3D11_SUBRESOURCE_DATA sd{t.px.data(), UINT(kTW * 4), 0};
	ID3D11Texture2D* tex = nullptr;
	if (FAILED(dev->CreateTexture2D(&d, &sd, &tex))) { t.state = 4; return 0; }
	D3D11_SHADER_RESOURCE_VIEW_DESC v{};
	v.Format = d.Format;
	v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	v.Texture2D.MipLevels = 1;
	HRESULT hr = dev->CreateShaderResourceView(tex, &v, &t.srv);
	tex->Release();
	if (FAILED(hr)) { t.state = 4; return 0; }
	t.px.clear();
	t.px.shrink_to_fit();
	t.state = 3;
	return (unsigned long long)(uintptr_t)t.srv;
}

} // namespace maps
