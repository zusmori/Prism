#pragma once
// Minimaler Zugriff auf die Unreal-Engine-3-Objekte von Rocket League.
// Statt eines generierten SDKs wird alles zur Laufzeit ueber die Reflection der Engine gesucht
// (Klassen, Eigenschaften, Funktionen nach Namen). So uebersteht der Mod viele Spiel-Updates.
#include <cstdint>
#include <string>
#include <vector>
#include <windows.h>

namespace ue {

struct FName {
	int32_t index;
	int32_t number;
};
template <class T> struct TArray {
	T* data;
	int32_t count;
	int32_t max;
};
struct FString {
	wchar_t* data;
	int32_t count;
	int32_t max;
};

// Aufbau der Engine-Objekte (RLSDK-Generator, Stand 2026)
namespace off {
constexpr int Index = 0x38, Outer = 0x40, Name = 0x48, Class = 0x50;
constexpr int FieldNext = 0x60;
constexpr int StructSuper = 0x80, StructChildren = 0x88, StructSize = 0x90;
constexpr int PropArrayDim = 0x70, PropElementSize = 0x74, PropOffset = 0x98;
constexpr int NameEntryName = 0x18;
constexpr int ProcessEventIndex = 67;
} // namespace off

bool Init();  // GNames/GObjects suchen und pruefen
bool Ready();
// Prueft, ob ein Objekt im Objekt-Array an seinem Index steht; wenn nicht, wird das richtige Array gesucht
bool CheckObjects(void* knownObject);
// Lebt das Objekt noch (steht es noch an seinem Platz im Objekt-Array)?
bool IsValid(const void* obj);
// Lebt das Objekt und ist nicht zum Loeschen markiert (RF_PendingKill)?
bool IsAlive(const void* obj);
extern bool killFlagCheck; // wird abgeschaltet, falls die Flag-Pruefung nicht passt

int ObjectCount();
void* ObjectAt(int i);
std::string NameOf(int index);
std::string Name(const void* obj);
void* ClassOf(const void* obj);
void* OuterOf(const void* obj);
std::string FullName(const void* obj); // z. B. "Class TAGame.Product_TA"

void* FindObject(const char* fullName);
void* FindClass(const char* fullName) ;
std::vector<void*> Instances(void* cls); // ohne "Default__"-Objekte
bool IsA(const void* obj, const void* cls);
int PropOffset(void* cls, const char* name); // -1 = nicht gefunden
uint32_t BoolMask(void* cls, const char* name); // Bit einer bool-Eigenschaft, 0 = unbekannt
void* FindFunction(void* cls, const char* name);
int FirstParamOffset(void* fn, std::string* type = nullptr); // erster Parameter einer Funktion, -1 = keiner
int StructSize(void* strct);

std::string Narrow(const FString& s);

template <class T> inline T Read(const void* p, int offset) { return *reinterpret_cast<const T*>(reinterpret_cast<const uint8_t*>(p) + offset); }
template <class T> inline T* Ptr(void* p, int offset) { return reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(p) + offset); }

using ProcessEventFn = void(__fastcall*)(void* obj, void* fn, void* params, void* result);
extern ProcessEventFn ProcessEventOrig; // nach dem Hook: Original
void* ProcessEventAddress();
void Call(void* obj, void* fn, void* params = nullptr); // Engine-Funktion aufrufen (nur im Spiel-Thread!)

} // namespace ue
