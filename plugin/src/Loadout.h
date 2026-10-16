#pragma once
// BakkesMod-Itemmod-Code erzeugen (Format v4, siehe github.com/bakkesmodorg/BakkesModLoadoutLib)
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace zl {

struct LoadoutItem {
	int id = 0;     // Produkt-ID
	int paint = 0;  // 0 = keine Farbe
};

class BitWriter {
public:
	std::vector<uint8_t> buf = std::vector<uint8_t>(2048, 0);
	int bit = 0;
	void Write(uint32_t v, int bits) {
		for (int i = 0; i < bits; i++) {
			if (v & 1) buf[bit / 8] |= (uint8_t)(1u << (bit % 8));
			else buf[bit / 8] &= (uint8_t)~(1u << (bit % 8));
			bit++;
			v >>= 1;
		}
	}
	void Bool(bool b) { Write(b ? 1 : 0, 1); }
	uint8_t Crc(int from, int to) const {
		uint8_t c = 0xFF;
		for (int i = from; i < to; i++) c ^= buf[i];
		return c;
	}
};

inline std::string Base64(const uint8_t* d, size_t n) {
	static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	size_t i = 0;
	for (; i + 2 < n; i += 3) {
		uint32_t v = (d[i] << 16) | (d[i + 1] << 8) | d[i + 2];
		out += t[(v >> 18) & 63]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += t[v & 63];
	}
	if (i + 1 == n) {
		uint32_t v = d[i] << 16;
		out += t[(v >> 18) & 63]; out += t[(v >> 12) & 63]; out += "==";
	} else if (i + 2 == n) {
		uint32_t v = (d[i] << 16) | (d[i + 1] << 8);
		out += t[(v >> 18) & 63]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += '=';
	}
	return out;
}

inline void WriteItems(BitWriter& w, const std::map<int, LoadoutItem>& items) {
	const int at = w.bit;
	w.Write(0, 4);
	int n = 0;
	for (auto& [slot, it] : items) {
		if (it.id <= 0 || n >= 15) continue;
		n++;
		w.Write(slot, 5);
		w.Write(it.id, 16);
		w.Bool(it.paint > 0);
		if (it.paint > 0) w.Write(it.paint, 6);
	}
	const int end = w.bit;
	w.bit = at;
	w.Write(n, 4);
	w.bit = end;
}

// Ein Loadout fuer beide Teams
inline std::string MakeCode(const std::map<int, LoadoutItem>& items) {
	BitWriter w;
	w.Write(4, 6);    // Version
	w.Write(0, 18);   // Platz fuer Groesse + CRC
	w.Bool(true);     // blau = orange
	WriteItems(w, items);
	w.Bool(false);    // keine eigenen Farben
	w.Write(0, 6);    // Team-ID Felgen (blau)
	w.Write(0, 6);    // Team-ID Felgen (orange)
	const int cur = w.bit;
	const int bytes = cur / 8 + (cur % 8 ? 1 : 0);
	w.bit = 6;
	w.Write(bytes, 10);
	w.Write(w.Crc(3, bytes), 8);
	w.bit = cur;
	return Base64(w.buf.data(), cur / 8 + 1);
}

} // namespace zl
