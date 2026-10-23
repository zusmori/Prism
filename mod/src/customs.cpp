// Eigene Decals und Baelle.
//
// Format (wie AlphaConsole / "Custom Textures"): ein Ordner (oder eine .zip) mit _TexturesPackage.json, z. B.
//   { "RSDomHero": { "BodyID": 403, "SkinID": 0,
//                    "Chassis": { "Diffuse": "" },
//                    "Body":    { "Diffuse": "Mainbody.png", "Skin": "Decal.png" } } }
// Fuer Baelle: { "MyBall": { "Ball": { "Diffuse": "Ball.png" } } } (oder ein Ordner nur mit einem Bild).
// "Body"/"Chassis"/"Ball" = Teil des Modells, darin: Textur-Parameter des Materials -> Bilddatei.
//
// Anwenden: pro Auto-Modell/Ball legt das Spiel eine eigene Material-Instanz an
// (MeshComponent.CreateAndSetMaterialInstanceConstant) und bekommt unsere Bilder als Texturen
// (Texture2DDynamic.Create + UpdateMip). Abwaehlen setzt das alte Material zurueck.
#include "customs.h"

#include <d3d11.h>
#include <shellapi.h>
#include <wincodec.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <thread>
#include <unordered_map>

#include "hooks.h"
#include "log.h"
#include "ue.h"

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shell32.lib")

namespace customs {

std::mutex mu;
std::vector<Custom> list;
std::string selDecal, selBall, status;

struct Part {
	std::string part;                                          // Body, Chassis, Ball
	std::vector<std::pair<std::string, std::wstring>> params;  // Textur-Parameter -> Datei
};
static std::map<std::string, std::vector<Part>> parts; // id -> Teile (mit mu)

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
static std::string Lower(std::string s) {
	for (auto& c : s) c = (char)tolower((unsigned char)c);
	return s;
}
static std::wstring Dir() {
	std::wstring d = zl::DataDir() + L"\\Customs";
	CreateDirectoryW(d.c_str(), nullptr);
	return d;
}
static bool ReadAll(const std::wstring& p, std::string& out) {
	HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	LARGE_INTEGER sz{};
	GetFileSizeEx(h, &sz);
	if (sz.QuadPart > (8ll << 20)) { CloseHandle(h); return false; }
	out.resize((size_t)sz.QuadPart);
	DWORD got = 0;
	BOOL ok = out.empty() || ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr);
	CloseHandle(h);
	out.resize(got);
	return ok != FALSE;
}
static bool IsFile(const std::wstring& p) {
	DWORD a = GetFileAttributesW(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// ---------- kleines JSON (Objekte, Texte, Zahlen als Text) ----------
struct J {
	bool obj = false;
	std::string str;
	std::vector<std::pair<std::string, J>> kids; // Reihenfolge behalten
	const J* get(const char* k) const {
		std::string lk = Lower(k);
		for (auto& [n, v] : kids) if (Lower(n) == lk) return &v;
		return nullptr;
	}
};
struct JP {
	const char* p;
	const char* e;
	void ws() { while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++; }
	bool str(std::string& out) {
		if (p >= e || *p != '"') return false;
		p++;
		while (p < e && *p != '"') {
			if (*p == '\\' && p + 1 < e) {
				p++;
				char c = *p++;
				if (c == 'n') out += '\n';
				else if (c == 't') out += '\t';
				else if (c == 'u') { p += std::min<ptrdiff_t>(4, e - p); out += '?'; }
				else out += c;
			} else out += *p++;
		}
		if (p < e) p++;
		return true;
	}
	bool value(J& v, int depth) {
		ws();
		if (p >= e || depth > 16) return false;
		if (*p == '"') return str(v.str);
		if (*p == '{' || *p == '[') {
			char close = *p == '{' ? '}' : ']';
			bool isObj = *p == '{';
			v.obj = isObj;
			p++;
			ws();
			if (p < e && *p == close) { p++; return true; }
			while (p < e) {
				ws();
				std::string k;
				if (isObj) {
					if (!str(k)) return false;
					ws();
					if (p >= e || *p != ':') return false;
					p++;
				}
				J child;
				if (!value(child, depth + 1)) return false;
				if (isObj) v.kids.push_back({k, std::move(child)});
				ws();
				if (p < e && *p == ',') { p++; continue; }
				if (p < e && *p == close) { p++; return true; }
				return false;
			}
			return false;
		}
		const char* s = p;
		while (p < e && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') p++;
		v.str.assign(s, p);
		return p > s;
	}
};
static bool Parse(const std::string& t, J& out) {
	JP jp{t.data(), t.data() + t.size()};
	if (t.size() >= 3 && (unsigned char)t[0] == 0xEF) jp.p += 3;
	return jp.value(out, 0);
}

// ---------- Ordner einlesen ----------
static void Unzip(const std::wstring& zip, const std::wstring& dest) {
	// Windows 10/11 bringen tar mit, das auch .zip entpackt
	CreateDirectoryW(dest.c_str(), nullptr);
	wchar_t sys[MAX_PATH];
	GetSystemDirectoryW(sys, MAX_PATH);
	std::wstring cmd = L"\"" + std::wstring(sys) + L"\\tar.exe\" -xf \"" + zip + L"\" -C \"" + dest + L"\"";
	STARTUPINFOW si{sizeof si};
	PROCESS_INFORMATION pi{};
	if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
		WaitForSingleObject(pi.hProcess, 60000);
		DWORD code = 1;
		GetExitCodeProcess(pi.hProcess, &code);
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
		zl::Log("customs: unpacked %s (%lu)", U8(zip).c_str(), code);
	} else zl::Log("customs: couldn't unpack %s (%lu)", U8(zip).c_str(), GetLastError());
}

struct Found {
	Custom c;
	std::vector<Part> parts;
};

static void ReadPackage(const std::wstring& dir, const std::wstring& json, const std::string& folderName, std::vector<Found>& out) {
	std::string t;
	J j;
	if (!ReadAll(json, t) || !Parse(t, j) || !j.obj) { zl::Log("customs: can't read %s", U8(json).c_str()); return; }
	for (auto& [key, e] : j.kids) {
		if (!e.obj) continue;
		Found f;
		f.c.name = key;
		f.c.id = folderName + "/" + key;
		if (const J* b = e.get("BodyID")) f.c.bodyId = atoi(b->str.c_str());
		if (const J* s = e.get("SkinID")) f.c.skinId = atoi(s->str.c_str());
		for (auto& [pk, pv] : e.kids) {
			if (!pv.obj) continue;
			Part p;
			p.part = pk;
			for (auto& [param, file] : pv.kids) {
				if (file.obj || file.str.empty()) continue;
				std::wstring path = dir + L"\\" + W(file.str);
				if (IsFile(path)) p.params.push_back({param, path});
			}
			if (Lower(pk) == "ball") f.c.ball = true;
			if (!p.params.empty()) f.parts.push_back(std::move(p));
		}
		if (f.parts.empty()) continue;
		// Ball erkennen: Teil "Ball", oder kein Auto angegeben und "ball" im Namen (Paket, Ordner oder Datei)
		if (!f.c.ball && !f.c.bodyId) {
			std::string hay = Lower(key + " " + folderName + " " + U8(dir));
			for (auto& p : f.parts) { hay += " " + Lower(p.part); for (auto& [param, file] : p.params) hay += " " + Lower(param + " " + U8(file)); }
			if (hay.find("ball") != std::string::npos) f.c.ball = true;
		}
		if (!f.c.ball && !f.c.bodyId) zl::Log("customs: %s has no BodyID - shown for every car", key.c_str());
		// Vorschaubild: das groesste Bild (meist die Karosserie bzw. der Ball)
		for (auto& p : f.parts)
			for (auto& [param, file] : p.params)
				if (f.c.thumb.empty() || Lower(param) == "diffuse") f.c.thumb = file;
		out.push_back(std::move(f));
	}
}

static void Scan() {
	std::wstring root = Dir();
	// neue .zip-Dateien entpacken (in einen gleichnamigen Ordner)
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW((root + L"\\*.zip").c_str(), &fd);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			std::wstring zip = root + L"\\" + fd.cFileName;
			std::wstring name = fd.cFileName;
			name = name.substr(0, name.size() - 4);
			std::wstring dest = root + L"\\" + name;
			if (GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES) continue; // schon entpackt
			{
				std::lock_guard<std::mutex> lk(mu);
				status = "Unpacking " + U8(name) + " …";
			}
			Unzip(zip, dest);
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	// Ordner durchsuchen (bis 3 Ebenen tief, zips enthalten oft noch einen Unterordner)
	std::vector<Found> found;
	std::function<void(const std::wstring&, const std::string&, int)> walk = [&](const std::wstring& dir, const std::string& top, int depth) {
		bool hadJson = false;
		std::vector<std::wstring> images;
		WIN32_FIND_DATAW f;
		HANDLE hh = FindFirstFileW((dir + L"\\*").c_str(), &f);
		if (hh == INVALID_HANDLE_VALUE) return;
		std::vector<std::wstring> subs;
		do {
			std::wstring n = f.cFileName;
			if (n == L"." || n == L"..") continue;
			std::wstring p = dir + L"\\" + n;
			if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { subs.push_back(n); continue; }
			std::string lo = Lower(U8(n));
			if (lo.size() > 5 && lo.substr(lo.size() - 5) == ".json") { ReadPackage(dir, p, top, found); hadJson = true; }
			else if (lo.size() > 4 && (lo.substr(lo.size() - 4) == ".png" || lo.substr(lo.size() - 4) == ".jpg" || lo.substr(lo.size() - 4) == ".tga")) images.push_back(p);
		} while (FindNextFileW(hh, &f));
		FindClose(hh);
		// Ordner ohne json mit genau einem Bild, das "ball" heisst: Ball
		if (!hadJson && images.size() == 1 && Lower(U8(dir)).find("ball") != std::string::npos) {
			Found b;
			b.c.ball = true;
			b.c.name = top;
			b.c.id = top + "/ball";
			b.c.thumb = images[0];
			b.parts.push_back(Part{"Ball", {{"Diffuse", images[0]}}});
			found.push_back(std::move(b));
		}
		if (depth < 3)
			for (auto& s : subs) walk(dir + L"\\" + s, top.empty() ? U8(s) : top, depth + 1);
	};
	walk(root, "", 0);
	std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return Lower(a.c.name) < Lower(b.c.name); });
	std::lock_guard<std::mutex> lk(mu);
	bool changed = found.size() != list.size();
	list.clear();
	parts.clear();
	for (auto& f : found) {
		list.push_back(f.c);
		parts[f.c.id] = f.parts;
	}
	status.clear();
	static size_t lastCount = (size_t)-1;
	if (changed || lastCount != list.size()) {
		lastCount = list.size();
		std::string names;
		for (auto& c : list) names += c.name + (c.ball ? "(ball) " : "(" + std::to_string(c.bodyId) + ") ");
		zl::Log("customs: %d found: %s", (int)list.size(), names.c_str());
	}
}

static std::wstring SelFile() { return zl::DataDir() + L"\\zlcustoms.txt"; }
static void SaveSel() {
	FILE* f = _wfopen(SelFile().c_str(), L"w");
	if (!f) return;
	std::lock_guard<std::mutex> lk(mu);
	fprintf(f, "decal\t%s\nball\t%s\n", selDecal.c_str(), selBall.c_str());
	fclose(f);
}

void Init() {
	FILE* f = _wfopen(SelFile().c_str(), L"r");
	if (f) {
		char line[512];
		std::lock_guard<std::mutex> lk(mu);
		while (fgets(line, sizeof line, f)) {
			std::string s(line);
			while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
			size_t t = s.find('\t');
			if (t == std::string::npos) continue;
			if (s.substr(0, t) == "decal") selDecal = s.substr(t + 1);
			if (s.substr(0, t) == "ball") selBall = s.substr(t + 1);
		}
		fclose(f);
	}
	std::thread([] {
		CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		for (;;) {
			try { Scan(); } catch (...) {}
			Sleep(3000);
		}
	}).detach();
}

void Select(bool ball, const std::string& id) {
	{
		std::lock_guard<std::mutex> lk(mu);
		(ball ? selBall : selDecal) = id;
	}
	SaveSel();
}

void OpenFolder() {
	std::wstring d = Dir();
	ShellExecuteW(nullptr, L"open", d.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ---------- Bilder laden (Hintergrund) ----------
static IWICImagingFactory* Factory() {
	static thread_local IWICImagingFactory* f = nullptr;
	if (!f) {
		CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
	}
	return f;
}

// Bild als BGRA laden; maxSide > 0: auf diese Groesse verkleinern (quadratisch zugeschnitten fuer Vorschauen)
static bool Decode(const std::wstring& path, std::vector<uint8_t>& out, UINT& w, UINT& h, UINT thumbSide, UINT forceW = 0, UINT forceH = 0) {
	IWICImagingFactory* f = Factory();
	if (!f) return false;
	IWICBitmapDecoder* dec = nullptr;
	IWICBitmapFrameDecode* fr = nullptr;
	IWICBitmapScaler* sc = nullptr;
	IWICFormatConverter* cv = nullptr;
	bool ok = false;
	do {
		if (FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) break;
		if (FAILED(dec->GetFrame(0, &fr))) break;
		fr->GetSize(&w, &h);
		if (!w || !h) break;
		IWICBitmapSource* src = fr;
		UINT tw = w, th = h;
		if (thumbSide) { tw = th = thumbSide; }
		else if (forceW && forceH) { tw = forceW; th = forceH; }
		else if (std::max(w, h) > 4096) { float s = 4096.0f / std::max(w, h); tw = UINT(w * s); th = UINT(h * s); }
		if (tw != w || th != h) {
			if (FAILED(f->CreateBitmapScaler(&sc)) || FAILED(sc->Initialize(fr, tw, th, WICBitmapInterpolationModeFant))) break;
			src = sc;
		}
		if (FAILED(f->CreateFormatConverter(&cv)) ||
			FAILED(cv->Initialize(src, thumbSide ? GUID_WICPixelFormat32bppRGBA : GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
				WICBitmapPaletteTypeCustom))) break;
		w = tw;
		h = th;
		out.resize(size_t(w) * h * 4);
		ok = SUCCEEDED(cv->CopyPixels(nullptr, w * 4, (UINT)out.size(), out.data()));
	} while (false);
	if (cv) cv->Release();
	if (sc) sc->Release();
	if (fr) fr->Release();
	if (dec) dec->Release();
	return ok;
}

struct Tex {
	int state = 0; // 0 neu, 1 laedt, 2 Pixel da, 3 fertig, 4 Fehler
	std::vector<uint8_t> px;
	UINT w = 0, h = 0;
	void* obj = nullptr; // Texture2DDynamic im Spiel
};
static std::mutex tmu;
static std::map<std::wstring, Tex> texs;
static std::deque<std::wstring> tq;
static std::condition_variable tcv;
static bool worker = false;

static void Worker() {
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	for (;;) {
		std::wstring p;
		{
			std::unique_lock<std::mutex> lk(tmu);
			tcv.wait(lk, [] { return !tq.empty(); });
			p = tq.front();
			tq.pop_front();
		}
		std::vector<uint8_t> px;
		UINT w = 0, h = 0;
		bool ok = false;
		if (p == L"black") { // leeres Decal (keine Decal-Flaechen)
			w = h = 4;
			px.assign(64, 0);
			ok = true;
		} else if (p.rfind(L"composite:", 0) == 0) {
			// Ebenen zusammenrechnen: Basis + (Decal-Bild ueber Maske)
			std::wstring rest = p.substr(10);
			std::wstring parts3[3];
			for (int k = 0, a = 0; k < 3; k++) {
				size_t b = rest.find(L'|', a);
				parts3[k] = rest.substr(a, b == std::wstring::npos ? std::wstring::npos : b - a);
				a = b == std::wstring::npos ? (int)rest.size() : (int)b + 1;
			}
			std::vector<uint8_t> over, base, mask;
			if (Decode(parts3[1], over, w, h, 0)) {
				UINT bw = 0, bh = 0;
				bool hasBase = !parts3[0].empty() && Decode(parts3[0], base, bw, bh, 0, w, h);
				bool hasMask = !parts3[2].empty() && Decode(parts3[2], mask, bw, bh, 0, w, h);
				px.resize(over.size());
				for (size_t i = 0; i + 3 < over.size(); i += 4) {
					float m = hasMask ? std::max({mask[i], mask[i + 1], mask[i + 2]}) / 255.0f * (mask[i + 3] / 255.0f) : over[i + 3] / 255.0f;
					for (int c = 0; c < 4; c++) {
						float b = hasBase ? base[i + c] : (c == 3 ? 0.0f : over[i + c]);
						float o = c == 3 ? 255.0f : over[i + c];
						px[i + c] = (uint8_t)std::clamp(b + (o - b) * m + 0.5f, 0.0f, 255.0f);
					}
				}
				ok = true;
			}
		} else ok = Decode(p, px, w, h, 0);
		std::lock_guard<std::mutex> lk(tmu);
		Tex& t = texs[p];
		if (ok) { t.px = std::move(px); t.w = w; t.h = h; t.state = 2; }
		else { t.state = 4; zl::Log("customs: can't read image %s", U8(p).c_str()); }
	}
}

// ---------- Spiel-Objekte ----------
static void *fnCreateTex = nullptr, *cdoTex = nullptr, *fnUpdateMip = nullptr;
static int oCrX = -1, oCrY = -1, oCrFmt = -1, oCrRet = -1, oMipIdx = -1, oMipData = -1;
static void *clsMesh = nullptr, *fnGetMat = nullptr, *fnSetMat = nullptr, *fnNumEl = nullptr, *fnCreateMIC = nullptr, *clsMIC = nullptr, *fnSetTex = nullptr;
static int oGetIdx = -1, oGetRet = -1, oSetIdx = -1, oSetMat = -1, oNumRet = -1, oCrMicIdx = -1, oCrMicRet = -1, oSetTexName = -1, oSetTexVal = -1, oMicParent = -1;
static bool setupTried = false, setupOk = false;

static bool Setup() {
	if (setupTried) return setupOk;
	setupTried = true;
	void* clsTex = ue::FindClass("Class Engine.Texture2DDynamic");
	cdoTex = ue::FindObject("Texture2DDynamic Engine.Default__Texture2DDynamic");
	if (clsTex) {
		fnCreateTex = ue::FindFunction(clsTex, "Create");
		fnUpdateMip = ue::FindFunction(clsTex, "UpdateMip");
	}
	if (fnCreateTex) {
		oCrX = ue::PropOffset(fnCreateTex, "InSizeX");
		oCrY = ue::PropOffset(fnCreateTex, "InSizeY");
		oCrFmt = ue::PropOffset(fnCreateTex, "InFormat");
		oCrRet = ue::PropOffset(fnCreateTex, "ReturnValue");
	}
	if (fnUpdateMip) {
		oMipIdx = ue::PropOffset(fnUpdateMip, "MipIdx");
		oMipData = ue::PropOffset(fnUpdateMip, "MipData");
	}
	clsMesh = ue::FindClass("Class Engine.MeshComponent");
	if (clsMesh) {
		fnGetMat = ue::FindFunction(clsMesh, "GetMaterial");
		fnSetMat = ue::FindFunction(clsMesh, "SetMaterial");
		fnNumEl = ue::FindFunction(clsMesh, "GetNumElements");
		fnCreateMIC = ue::FindFunction(clsMesh, "CreateAndSetMaterialInstanceConstant");
	}
	if (fnGetMat) { oGetIdx = ue::FirstParamOffset(fnGetMat); oGetRet = ue::PropOffset(fnGetMat, "ReturnValue"); }
	if (fnSetMat) { oSetIdx = ue::PropOffset(fnSetMat, "ElementIndex"); oSetMat = ue::PropOffset(fnSetMat, "Material"); if (oSetIdx < 0) oSetIdx = ue::FirstParamOffset(fnSetMat); }
	if (fnNumEl) oNumRet = ue::PropOffset(fnNumEl, "ReturnValue");
	if (fnCreateMIC) { oCrMicIdx = ue::FirstParamOffset(fnCreateMIC); oCrMicRet = ue::PropOffset(fnCreateMIC, "ReturnValue"); }
	clsMIC = ue::FindClass("Class Engine.MaterialInstanceConstant");
	if (clsMIC) {
		fnSetTex = ue::FindFunction(clsMIC, "SetTextureParameterValue");
		if (fnSetTex) { oSetTexName = ue::PropOffset(fnSetTex, "ParameterName"); oSetTexVal = ue::PropOffset(fnSetTex, "Value"); }
	}
	if (void* clsMI = ue::FindClass("Class Engine.MaterialInstance")) oMicParent = ue::PropOffset(clsMI, "Parent");
	setupOk = fnCreateTex && cdoTex && oCrRet >= 0 && fnUpdateMip && oMipData >= 0 && fnGetMat && oGetRet >= 0 && fnSetMat && oSetMat >= 0 && fnCreateMIC &&
		oCrMicRet >= 0 && fnSetTex && oSetTexName >= 0 && oSetTexVal >= 0;
	zl::Log("customs: tex create=%p(%d/%d/%d/%d) updateMip=%p(%d/%d) mesh get=%p set=%p(%d/%d) num=%p createMIC=%p(%d/%d) setTex=%p(%d/%d) -> %s",
		fnCreateTex, oCrX, oCrY, oCrFmt, oCrRet, fnUpdateMip, oMipIdx, oMipData, fnGetMat, fnSetMat, oSetIdx, oSetMat, fnNumEl, fnCreateMIC, oCrMicIdx, oCrMicRet,
		fnSetTex, oSetTexName, oSetTexVal, setupOk ? "ok" : "MISSING");
	return setupOk;
}

// Texture fuer eine Datei: laden lassen, im Spiel-Thread anlegen (eine pro Aufruf, damit nichts ruckelt)
static void* TexFor(const std::wstring& file, bool& createdOne) {
	std::lock_guard<std::mutex> lk(tmu);
	Tex& t = texs[file];
	static void* clsDyn = ue::FindClass("Class Engine.Texture2DDynamic");
	if (t.state == 3) {
		if (ue::IsValid(t.obj) && (!clsDyn || ue::ClassOf(t.obj) == clsDyn)) return t.obj;
		// vom Spiel weggeraeumt (z. B. beim Laden einer Map): aus den behaltenen Pixeln neu anlegen
		zl::Log("customs: texture for %s was removed by the game - recreating", U8(file).c_str());
		t.obj = nullptr;
		t.state = t.px.empty() ? 0 : 2;
		if (t.state == 0) return nullptr;
	}
	if (t.state == 0) {
		t.state = 1;
		tq.push_back(file);
		if (!worker) { worker = true; for (int k = 0; k < 3; k++) std::thread(Worker).detach(); }
		tcv.notify_one();
		return nullptr;
	}
	if (t.state != 2) return nullptr;
	(void)createdOne; // Anlegen + Hochladen ist schnell (Dekodieren passiert vorher im Hintergrund)
	std::vector<uint8_t> prm(size_t(ue::StructSize(fnCreateTex)) + 16, 0);
	if (oCrX >= 0) *reinterpret_cast<int32_t*>(prm.data() + oCrX) = (int32_t)t.w;
	if (oCrY >= 0) *reinterpret_cast<int32_t*>(prm.data() + oCrY) = (int32_t)t.h;
	if (oCrFmt >= 0) prm[oCrFmt] = 2; // PF_A8R8G8B8
	ue::Call(cdoTex, fnCreateTex, prm.data());
	void* tex = *reinterpret_cast<void**>(prm.data() + oCrRet);
	if (!tex) { t.state = 4; zl::Log("customs: couldn't create texture %ux%u", t.w, t.h); return nullptr; }
	// nie aufraeumen lassen (Speicherbereinigung des Spiels): RF_RootSet
	*ue::Ptr<uint64_t>(tex, 0x10) |= 0x4000ull;
	std::vector<uint8_t> mp(size_t(ue::StructSize(fnUpdateMip)) + 16, 0);
	if (oMipIdx >= 0) *reinterpret_cast<int32_t*>(mp.data() + oMipIdx) = 0;
	auto* arr = reinterpret_cast<ue::TArray<uint8_t>*>(mp.data() + oMipData);
	arr->data = t.px.data();
	arr->count = arr->max = (int32_t)t.px.size();
	ue::Call(tex, fnUpdateMip, mp.data());
	t.obj = tex;
	t.state = 3; // Pixel bleiben im Speicher: falls das Spiel die Textur wegraeumt, ist sie sofort wieder da
	zl::Log("customs: texture %s (%ux%u) -> %s", U8(file).c_str(), t.w, t.h, ue::Name(tex).c_str());
	return tex;
}

static void* GetMat(void* mesh, int i) {
	std::vector<uint8_t> p(size_t(ue::StructSize(fnGetMat)) + 16, 0);
	*reinterpret_cast<int32_t*>(p.data() + oGetIdx) = i;
	ue::Call(mesh, fnGetMat, p.data());
	return *reinterpret_cast<void**>(p.data() + oGetRet);
}
static void SetMat(void* mesh, int i, void* m) {
	std::vector<uint8_t> p(size_t(ue::StructSize(fnSetMat)) + 16, 0);
	*reinterpret_cast<int32_t*>(p.data() + oSetIdx) = i;
	*reinterpret_cast<void**>(p.data() + oSetMat) = m;
	ue::Call(mesh, fnSetMat, p.data());
}
static int NumElements(void* mesh) {
	if (!fnNumEl || oNumRet < 0) return 4;
	std::vector<uint8_t> p(size_t(ue::StructSize(fnNumEl)) + 16, 0);
	ue::Call(mesh, fnNumEl, p.data());
	return std::clamp(*reinterpret_cast<int32_t*>(p.data() + oNumRet), 0, 16);
}
static void* WrapMIC(void* mesh, int i) {
	std::vector<uint8_t> p(size_t(ue::StructSize(fnCreateMIC)) + 16, 0);
	*reinterpret_cast<int32_t*>(p.data() + oCrMicIdx) = i;
	ue::Call(mesh, fnCreateMIC, p.data());
	return *reinterpret_cast<void**>(p.data() + oCrMicRet);
}
static void SetTex(void* mic, int nameIdx, void* tex) {
	std::vector<uint8_t> p(size_t(ue::StructSize(fnSetTex)) + 16, 0);
	*reinterpret_cast<ue::FName*>(p.data() + oSetTexName) = ue::FName{nameIdx, 0};
	*reinterpret_cast<void**>(p.data() + oSetTexVal) = tex;
	ue::Call(mic, fnSetTex, p.data());
}

// Textur-Parameter, die die Materialien dieses Elements kennen (alle Instanzen der Kette)
static int oTexParams = -1, tpStride = 32, tpName = 0;
static std::vector<std::string> ChainTexParams(void* mat, std::string* desc) {
	static bool init = false;
	if (!init) {
		init = true;
		if (clsMIC) oTexParams = ue::PropOffset(clsMIC, "TextureParameterValues");
		if (void* st = ue::FindObject("ScriptStruct Engine.MaterialInstanceConstant.TextureParameterValue")) {
			tpStride = ue::StructSize(st);
			tpName = ue::PropOffset(st, "ParameterName");
		}
	}
	std::vector<std::string> out;
	for (int d = 0; d < 6 && mat; d++) {
		std::string here;
		if (oTexParams >= 0 && clsMIC && ue::IsA(mat, clsMIC)) {
			auto* arr = ue::Ptr<ue::TArray<uint8_t>>(mat, oTexParams);
			for (int i = 0; arr->data && i < arr->count && i < 32; i++) {
				std::string n = ue::NameOf(reinterpret_cast<ue::FName*>(arr->data + size_t(i) * tpStride + tpName)->index);
				if (n.empty()) continue;
				if (std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
				here += (here.empty() ? "" : ",") + n;
			}
		}
		if (desc) *desc += ue::Name(mat) + "[" + here + "] > ";
		mat = (oMicParent >= 0 && clsMIC && ue::IsA(mat, clsMIC)) ? ue::Read<void*>(mat, oMicParent) : nullptr;
	}
	return out;
}

// Parameter-Name aus dem Paket auf den Namen im Material abbilden (z. B. "Skin" -> "Skin" oder "DecalSkin")
static std::string MapParam(const std::string& want, const std::vector<std::string>& have) {
	std::string w = Lower(want);
	for (auto& h : have) if (Lower(h) == w) return h;
	std::vector<std::string> alts{w};
	if (w == "skin") alts = {"skin", "decal"};
	if (w == "diffuse") alts = {"diffuse", "albedo", "basecolor"};
	for (auto& a : alts)
		for (auto& h : have) if (Lower(h).find(a) != std::string::npos) return h;
	return want;
}

// Welcher Teil ist dieses Material? (Name des Materials bzw. seiner Eltern)
static std::string PartOf(void* mat, int element) {
	std::string chain;
	for (int d = 0; d < 6 && mat; d++) {
		chain += Lower(ue::Name(mat)) + " ";
		mat = (oMicParent >= 0 && clsMIC && ue::IsA(mat, clsMIC)) ? ue::Read<void*>(mat, oMicParent) : nullptr;
	}
	(void)element;
	// nur eindeutige Namen: Glas, Lichter, Zierleisten usw. bleiben unberuehrt
	if (chain.find("chassis") != std::string::npos) return "chassis";
	if (chain.find("ball") != std::string::npos) return "ball";
	if (chain.find("body_paintable") != std::string::npos || chain.find("_body") != std::string::npos || chain.find("mic_skin_") != std::string::npos)
		return "body";
	return "";
}

struct Applied {
	void* orig = nullptr; // Material vorher
	void* mic = nullptr;  // unsere Instanz
	std::string id;       // welches Custom
};
static std::map<std::pair<void*, int>, Applied> applied; // (Modell, Element) -> Stand

static void Restore(void* mesh, int i, Applied& a) {
	if (ue::IsAlive(mesh) && a.mic && GetMat(mesh, i) == a.mic && a.orig && ue::IsValid(a.orig)) SetMat(mesh, i, a.orig);
}

// Bilder eines Teils auf die Namen des Spiel-Materials umsetzen. Pakete benennen ihre Bilder unterschiedlich:
//   klassisch:  Diffuse (Karosserie-Bild) + Skin (Decal-Maske, schwarz = keine Decal-Flaechen)
//   in Ebenen:  1_Diffuse_Skin (Decal-Bild) + 2_Diffuse_Skin_Mask (wo es liegt) [+ Diffuse als Basis]
// Karosserie ohne eigenes "Skin": schwarzes Skin, damit kein Spiel-Decal durchscheint.
static std::vector<std::pair<std::string, std::wstring>> Resolve(const std::vector<std::pair<std::string, std::wstring>>& in, const std::string& part) {
	std::vector<std::pair<std::string, std::wstring>> out;
	std::wstring base, over, mask, skin;
	for (auto& [name, file] : in) {
		std::string n = Lower(name);
		size_t k = 0;
		while (k < n.size() && (isdigit((unsigned char)n[k]) || n[k] == '_')) k++;
		n = n.substr(k);
		std::string fl = Lower(U8(file));
		if (part == "ball") {
			bool isMask = n.find("mask") != std::string::npos || (fl.find("mask") != std::string::npos && n != "diffuse");
			out.push_back({isMask ? "Masks" : name.substr(k), file});
			continue;
		}
		if (n == "diffuse") base = file;
		else if (n.find("skin") != std::string::npos && n.find("mask") != std::string::npos) mask = file;
		else if (n.find("skin") != std::string::npos && n.find("diffuse") != std::string::npos) over = file;
		else if (n == "skin") skin = file;
		else out.push_back({name.substr(k), file});
	}
	if (part == "ball") return out;
	if (!over.empty()) out.push_back({"Diffuse", L"composite:" + base + L"|" + over + L"|" + mask});
	else if (!base.empty()) out.push_back({"Diffuse", base});
	if (part == "body") out.push_back({"Skin", skin.empty() ? L"black" : skin});
	return out;
}

// Ein Modell mit einem Custom versehen (oder zuruecksetzen, wenn id leer). ball: alle Teile auf jedes Element
static void ApplyTo(void* mesh, const std::string& id, const std::vector<Part>& ps, bool& createdOne, bool ball) {
	int n = NumElements(mesh);
	// Ball: nur die Elemente, deren Material die Bild-Namen wirklich kennt (Glanz/Leuchten/Kanten bleiben unberuehrt)
	std::vector<bool> ballOk(n, false);
	if (ball && !id.empty()) {
		bool any = false;
		for (int i = 0; i < n; i++) {
			auto ap = applied.find(std::make_pair(mesh, i));
			void* m = GetMat(mesh, i);
			if (ap != applied.end() && m == ap->second.mic) m = ap->second.orig;
			std::vector<std::string> have = ChainTexParams(m, nullptr);
			for (auto& p : ps)
				for (auto& pr : p.params)
					for (auto& h : have)
						if (Lower(h) == Lower(MapParam(pr.first, have))) { ballOk[i] = true; any = true; }
		}
		if (!any && n > 0) ballOk[0] = true;
	}
	for (int i = 0; i < n; i++) {
		auto key = std::make_pair(mesh, i);
		auto ap = applied.find(key);
		void* cur = GetMat(mesh, i);
		if (id.empty()) {
			if (ap != applied.end()) { Restore(mesh, i, ap->second); applied.erase(ap); }
			continue;
		}
		// schon unseres und noch dran?
		if (ap != applied.end() && ap->second.id == id && ap->second.mic && cur == ap->second.mic && ue::IsAlive(ap->second.mic)) continue;
		std::string part = ball ? (ballOk[i] ? "ball" : "") : PartOf(cur, i);
		std::vector<std::pair<std::string, std::wstring>> raw;
		for (auto& p : ps)
			if (ball || Lower(p.part) == part)
				for (auto& pr : p.params) raw.push_back(pr);
		std::vector<std::pair<std::string, std::wstring>> want = raw.empty() ? raw : Resolve(raw, part);
		if (want.empty() || part.empty()) {
			if (ap != applied.end()) { Restore(mesh, i, ap->second); applied.erase(ap); }
			continue;
		}
		// Texturen bereit?
		std::string desc;
		std::vector<std::string> have = ChainTexParams(cur, &desc);
		std::vector<std::pair<int, void*>> texs2;
		std::string mapped;
		bool allReady = true;
		for (auto& [param, file] : want) {
			std::string real = MapParam(param, have);
			int ni = ue::FindName(real);
			void* t = TexFor(file, createdOne);
			if (ni < 0) { zl::Log("customs: unknown texture parameter %s", real.c_str()); continue; }
			if (!t) { allReady = false; continue; }
			texs2.push_back({ni, t});
			mapped += param + "->" + real + " ";
		}
		if (!allReady || texs2.empty()) continue;
		void* orig = (ap != applied.end() && cur == ap->second.mic) ? ap->second.orig : cur;
		void* mic = WrapMIC(mesh, i);
		if (!mic) { zl::Log("customs: couldn't make material for %s/%d", ue::Name(mesh).c_str(), i); continue; }
		for (auto& [ni, t] : texs2) SetTex(mic, ni, t);
		applied[key] = Applied{orig, mic, id};
		static int logs = 0;
		if (logs++ < 40)
			zl::Log("customs: %s on %s element %d (%s): %s| %s", id.c_str(), ue::Name(mesh).c_str(), i, part.c_str(), mapped.c_str(), desc.c_str());
	}
}

// Bilder eines Customs schon im Voraus laden (beim Auswaehlen und beim Start), damit es im Spiel sofort da ist
static void Prefetch(const std::string& id) {
	std::vector<std::wstring> files;
	{
		std::lock_guard<std::mutex> lk(mu);
		auto it = parts.find(id);
		if (it == parts.end()) return;
		bool isBall = false;
		for (auto& c : list) if (c.id == id) isBall = c.ball;
		for (auto& p : it->second) {
			std::string part = isBall ? "ball" : Lower(p.part);
			for (auto& [param, file] : Resolve(p.params, part)) files.push_back(file);
		}
	}
	std::lock_guard<std::mutex> lk(tmu);
	for (auto& f : files) {
		Tex& t = texs[f];
		if (t.state != 0) continue;
		t.state = 1;
		tq.push_back(f);
	}
	if (!worker) { worker = true; for (int k = 0; k < 3; k++) std::thread(Worker).detach(); }
	tcv.notify_all();
}

void Tick(const std::vector<void*>& carMeshes, int body, const std::vector<void*>& balls) {
	static unsigned long long next = 0;
	unsigned long long now = GetTickCount64();
	if (now < next) return;
	next = now + 60;
	std::string dId, bId;
	std::vector<Part> dParts, bParts;
	int dBody = 0;
	{
		std::lock_guard<std::mutex> lk(mu);
		for (auto& c : list) {
			if (!c.ball && c.id == selDecal) { dId = c.id; dParts = parts[c.id]; dBody = c.bodyId; }
			if (c.ball && c.id == selBall) { bId = c.id; bParts = parts[c.id]; }
		}
	}
	static std::string pre;
	if (pre != dId + "|" + bId) { pre = dId + "|" + bId; Prefetch(dId); Prefetch(bId); }
	bool need = !dId.empty() || !bId.empty() || !applied.empty();
	if (!need || !Setup()) return;
	bool createdOne = false;
	// Decal nur auf dem passenden Auto (die Bilder passen nur zu dessen Form)
	std::string useD = (dId.empty() || (dBody && body && dBody != body)) ? "" : dId;
	for (void* m : carMeshes)
		if (m && ue::IsAlive(m)) ApplyTo(m, useD, dParts, createdOne, false);
	for (void* b : balls)
		if (b && ue::IsAlive(b)) ApplyTo(b, bId, bParts, createdOne, true);
	// Eintraege von geloeschten Modellen vergessen
	for (auto it = applied.begin(); it != applied.end();) it = ue::IsAlive(it->first.first) ? std::next(it) : applied.erase(it);
}

// ---------- Vorschaubilder fuers Menue ----------
struct Th {
	int state = 0;
	std::vector<uint8_t> px;
	ID3D11ShaderResourceView* srv = nullptr;
};
static std::mutex thmu;
static std::map<std::string, Th> thumbs;
constexpr UINT kTh = 96;

unsigned long long Thumb(const std::string& id) {
	std::wstring file;
	{
		std::lock_guard<std::mutex> lk(mu);
		for (auto& c : list) if (c.id == id) file = c.thumb;
	}
	if (file.empty()) return 0;
	std::lock_guard<std::mutex> lk(thmu);
	Th& t = thumbs[id];
	if (t.state == 3) return (unsigned long long)(uintptr_t)t.srv;
	if (t.state == 0) {
		t.state = 1;
		std::thread([id, file] {
			std::vector<uint8_t> px;
			UINT w = 0, h = 0;
			bool ok = Decode(file, px, w, h, kTh);
			std::lock_guard<std::mutex> lk2(thmu);
			Th& tt = thumbs[id];
			if (ok) { tt.px = std::move(px); tt.state = 2; }
			else tt.state = 4;
		}).detach();
		return 0;
	}
	if (t.state != 2) return 0;
	ID3D11Device* dev = hooks::Device();
	if (!dev) return 0;
	D3D11_TEXTURE2D_DESC d{};
	d.Width = d.Height = kTh;
	d.MipLevels = d.ArraySize = 1;
	d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	d.SampleDesc.Count = 1;
	d.Usage = D3D11_USAGE_IMMUTABLE;
	d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	D3D11_SUBRESOURCE_DATA sd{t.px.data(), kTh * 4, 0};
	ID3D11Texture2D* tex = nullptr;
	if (FAILED(dev->CreateTexture2D(&d, &sd, &tex))) { t.state = 4; return 0; }
	D3D11_SHADER_RESOURCE_VIEW_DESC v{};
	v.Format = d.Format;
	v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	v.Texture2D.MipLevels = 1;
	HRESULT hr = dev->CreateShaderResourceView(tex, &v, &t.srv);
	tex->Release();
	t.px.clear();
	t.state = SUCCEEDED(hr) ? 3 : 4;
	return SUCCEEDED(hr) ? (unsigned long long)(uintptr_t)t.srv : 0;
}

} // namespace customs
