#include "merge.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <map>

#include "log.h"
#include "ue.h"

namespace merge {

std::mutex mu;
std::vector<Arena> list;
bool loaded = false;
unsigned long long launcherSeen = 0;

static std::wstring File(const wchar_t* n) { return zl::DataDir() + L"\\" + n; }

static std::vector<std::vector<std::string>> ReadLines(const std::wstring& p, unsigned long long* mtime = nullptr) {
	std::vector<std::vector<std::string>> out;
	HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return out;
	if (mtime) {
		FILETIME ft{};
		GetFileTime(h, nullptr, nullptr, &ft);
		*mtime = (((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10000;
	}
	LARGE_INTEGER sz{};
	GetFileSizeEx(h, &sz);
	std::string t;
	if (sz.QuadPart > 0 && sz.QuadPart < (4 << 20)) {
		t.resize((size_t)sz.QuadPart);
		DWORD got = 0;
		ReadFile(h, t.data(), (DWORD)t.size(), &got, nullptr);
		t.resize(got);
	}
	CloseHandle(h);
	size_t s = 0;
	while (s < t.size()) {
		size_t e = t.find('\n', s);
		if (e == std::string::npos) e = t.size();
		std::string line = t.substr(s, e - s);
		if (!line.empty() && line.back() == '\r') line.pop_back();
		s = e + 1;
		if (line.empty()) continue;
		std::vector<std::string> cols;
		size_t a = 0;
		while (true) {
			size_t b = line.find('\t', a);
			cols.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
			if (b == std::string::npos) break;
			a = b + 1;
		}
		out.push_back(cols);
	}
	return out;
}

static bool WriteLines(const std::wstring& p, const std::vector<std::string>& lines) {
	std::string t;
	for (auto& l : lines) t += l + "\n";
	std::wstring tmp = p + L".tmp";
	HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	DWORD put = 0;
	WriteFile(h, t.data(), (DWORD)t.size(), &put, nullptr);
	CloseHandle(h);
	return MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}

static std::string Lower(std::string s) {
	for (auto& c : s) c = (char)tolower((unsigned char)c);
	return s;
}

void Refresh() {
	static unsigned long long next = 0;
	unsigned long long now = GetTickCount64();
	if (now < next) return;
	next = now + 700;
	unsigned long long catT = 0, statT = 0;
	auto cat = ReadLines(File(L"merge_catalog.txt"), &catT);
	auto req = ReadLines(File(L"merge_requests.txt"));
	auto st = ReadLines(File(L"merge_status.txt"), &statT);
	std::map<std::string, std::vector<std::string>> reqM, stM;
	for (auto& r : req) if (r.size() >= 2) reqM[Lower(r[0])] = r;
	for (auto& r : st) if (r.size() >= 4) stM[Lower(r[0])] = r;
	std::vector<Arena> out;
	std::map<std::string, std::string> names;
	for (auto& c : cat) if (c.size() >= 2) names[Lower(c[0])] = c[1];
	for (auto& c : cat) {
		if (c.size() < 4) continue;
		Arena a;
		a.file = c[0];
		a.name = c[1];
		a.backdrop = c[2] == "1";
		a.base = c[3] == "1";
		a.own = c.size() > 4 && c[4] == "1";
		a.lower = Lower(a.name + " " + a.file);
		auto r = reqM.find(Lower(a.file));
		if (r != reqM.end()) {
			a.want = r->second[1];
			a.replace = r->second.size() > 2 && r->second[2] == "1";
			auto n = names.find(Lower(a.want));
			a.wantName = a.want == "-" ? "no backdrop" : n != names.end() ? n->second : a.want;
		}
		auto s = stM.find(Lower(a.file));
		if (s != stM.end()) {
			bool same = Lower(s->second[1]) == Lower(a.want) && (s->second[2] == "1") == a.replace;
			if (same || a.want.empty()) {
				a.state = s->second[3];
				a.stateMsg = s->second.size() > 4 ? s->second[4] : "";
			}
		}
		out.push_back(std::move(a));
	}
	std::lock_guard<std::mutex> lk(mu);
	list = std::move(out);
	loaded = !cat.empty();
	launcherSeen = std::max(catT, statT);
}

void Request(const std::string& file, const std::string& backdrop, bool replace) {
	auto req = ReadLines(File(L"merge_requests.txt"));
	std::vector<std::string> lines;
	for (auto& r : req)
		if (r.size() >= 2 && Lower(r[0]) != Lower(file)) lines.push_back(r[0] + "\t" + r[1] + "\t" + (r.size() > 2 ? r[2] : "0"));
	if (!backdrop.empty()) lines.push_back(file + "\t" + backdrop + "\t" + (replace ? "1" : "0"));
	WriteLines(File(L"merge_requests.txt"), lines);
	zl::Log("merge: %s <- %s%s", file.c_str(), backdrop.empty() ? "(original)" : backdrop.c_str(), replace ? " (replace)" : "");
	// sofort anzeigen
	std::lock_guard<std::mutex> lk(mu);
	for (auto& a : list)
		if (Lower(a.file) == Lower(file)) {
			a.want = backdrop;
			a.replace = replace;
			a.state = backdrop.empty() ? "" : "working";
			a.stateMsg.clear();
			if (backdrop == "-") a.wantName = "no backdrop";
			for (auto& b : list) if (Lower(b.file) == Lower(backdrop)) a.wantName = b.name;
		}
}

void RestoreAll() {
	WriteLines(File(L"merge_requests.txt"), {});
	std::lock_guard<std::mutex> lk(mu);
	for (auto& a : list) { a.want.clear(); a.state.clear(); }
}

unsigned long long restartAsked = 0;
void RequestRestart() {
	restartAsked = GetTickCount64();
	WriteLines(File(L"merge_restart.txt"), {std::to_string(GetTickCount64())});
	zl::Log("merge: restart requested");
}
bool NeedsRestart(const Arena& a) { return a.state == "pending" && a.stateMsg.find("Restart") != std::string::npos; }

// ---------- Map-Namen aus dem Spiel ----------
// Die Arenen-Daten (MapData_TA) kennen den Namen, den das Spiel anzeigt. Prism liest die Datei und benennt
// die Arenen im Katalog so. Je Zeile: Objektname \t Eigenschaft=Wert \t ...
void TickNames() {
	static void* cls = nullptr;
	static bool searched = false;
	static ue::SlicedScan scan;
	static unsigned long long next = 0;
	static std::string last;
	if (!searched) {
		searched = true;
		cls = ue::FindClass("Class TAGame.MapData_TA");
		zl::Log("merge: MapData_TA %s", cls ? "found" : "not found");
	}
	if (!cls) return;
	if (!scan.running) {
		if (GetTickCount64() < next) return;
		scan.Start(cls);
	}
	if (!scan.Step(8000)) return;
	next = GetTickCount64() + 60000;
	static bool described = false;
	if (!described && !scan.result.empty()) {
		described = true;
		for (auto& l : ue::DescribeClass(ue::ClassOf(scan.result[0]))) zl::Log("mapdata: %s", l.c_str());
	}
	std::vector<std::string> lines;
	for (void* o : scan.result) {
		if (!ue::IsAlive(o)) continue;
		std::string l = ue::Name(o);
		auto props = ue::TextProps(o);
		for (auto& kv : ue::CallTextGetters(o)) props.push_back(kv);
		for (auto& kv : props) {
			std::string v = kv.second;
			for (auto& c : v)
				if (c == '\t' || c == '\n' || c == '\r') c = ' ';
			l += "\t" + kv.first + "=" + v;
		}
		lines.push_back(l);
	}
	if (lines.empty()) return;
	std::sort(lines.begin(), lines.end());
	std::string all;
	for (auto& l : lines) all += l + "\n";
	if (all == last) return;
	last = all;
	WriteLines(File(L"map_names.txt"), lines);
	zl::Log("merge: %d map names written", (int)lines.size());
}

} // namespace merge
