#pragma once
// Einfaches Log nach %APPDATA%\RLMapSwap\zlmod.log (zum Fehlersuchen ohne Debugger)
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <windows.h>

namespace zl {

inline std::wstring DataDir() {
	wchar_t buf[MAX_PATH];
	DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
	std::wstring d = n ? std::wstring(buf, n) : L".";
	d += L"\\RLMapSwap";
	CreateDirectoryW(d.c_str(), nullptr);
	return d;
}

inline void Log(const char* fmt, ...) {
	static std::mutex mu;
	static FILE* f = nullptr;
	std::lock_guard<std::mutex> lk(mu);
	if (!f) {
		f = _wfopen((DataDir() + L"\\zlmod.log").c_str(), L"w");
		if (!f) return;
	}
	SYSTEMTIME t;
	GetLocalTime(&t);
	fprintf(f, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fflush(f);
}

} // namespace zl
