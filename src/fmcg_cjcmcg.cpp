// fmcg_cjcmcg.cpp -- reader for CJCMCG preprocessed files (.cjcmcg).
//
// CJCMCG (another MIDI note counter) can save a MIDI's counting data as a
// .cjcmcg file: resolution, tempo changes, lyrics, the total note count and,
// for every tick, the number of note-on and note-off events. No pitches,
// channels, velocities, tracks or CC. Wire format (all fixed-width integers
// big-endian; "varint" = base-128, most significant group first):
//
//   u32 resolution, u32 tempo_count, tempo_count x {u32 tick, u24 us/quarter},
//   u32 lyric_count, lyric_count x {u32 tick, u32 len, len bytes UTF-8},
//   varint total_note_count, u32 tick_count, tick_count x {varint ons, varint offs}
//
// The tick table is converted into the same sparse per-tick cells the layered
// walk produces (TickData::sparse: ons + polyphony delta per touched tick), and
// the tempo records into tempo_raw, so the regular sweep computes every fMCG
// stat (cumulative notes, exact inclusive-window NPS, polyphony, peaks, BPM,
// time, ticks) exactly as for a MIDI with the same per-tick note counts.
//
// What a .cjcmcg cannot carry, and how it is handled:
//  - Velocity-0 note-ons: CJCMCG counted every 0x9n event as a note-on, so
//    they are notes here (fMCG's "Vel-0 as Note-Off" cannot be applied).
//  - Per-key note-off matching: CJCMCG keeps only counts. Polyphony is the
//    running ons - offs, never below zero (offs beyond the held count are
//    ignored, as fMCG ignores a note-off for a key that is not held).
//  - CC events: not stored ({cc} stays 0).
//  - Song length: the last tick with a note, tempo or lyric event (the file
//    has no End-of-Track ticks).
//  - CJCMCG's writer seeds its tempo list with a synthetic record at tick 0
//    holding 5e9/resolution -- its internal unit for 120 BPM -- in the field
//    that otherwise holds microseconds per quarter note. That record is
//    recognised byte for byte and treated as the MIDI default (120 BPM).
#include "fmcg_engine_internal.h"
#include "fmcg_util.h"
#include "fmcg_engine.h"   // close_held_notes()

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <sstream>

namespace fmcg_stream {

namespace {

struct NotCjcmcg { std::string why; };

class Reader {
public:
    Reader(const ByteReader& read) : read_(read), buf_(1u << 20) {}
    uint64_t consumed() const { return consumed_; }
    bool at_end() {
        if (pos_ < len_) return false;
        refill();
        return pos_ >= len_;
    }
    uint8_t u8(const char* what) {
        if (pos_ >= len_) {
            refill();
            if (pos_ >= len_) throw NotCjcmcg{std::string("file ends inside ") + what};
        }
        ++consumed_;
        return buf_[pos_++];
    }
    uint32_t u32(const char* what) {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v = (v << 8) | u8(what);
        return v;
    }
    uint32_t u24(const char* what) {
        uint32_t v = 0;
        for (int i = 0; i < 3; ++i) v = (v << 8) | u8(what);
        return v;
    }
    uint64_t varint(const char* what) {
        uint64_t v = 0;
        for (int i = 0; i < 10; ++i) {
            const uint8_t b = u8(what);
            if (v > (UINT64_MAX >> 7)) throw NotCjcmcg{std::string("oversized number in ") + what};
            v = (v << 7) | (b & 0x7F);
            if (b < 0x80) return v;
        }
        throw NotCjcmcg{std::string("oversized number in ") + what};
    }
    void skip(uint64_t n, const char* what) {
        while (n > 0) {
            if (pos_ >= len_) {
                refill();
                if (pos_ >= len_) throw NotCjcmcg{std::string("file ends inside ") + what};
            }
            const size_t k = (size_t)std::min<uint64_t>(n, len_ - pos_);
            pos_ += k;
            consumed_ += k;
            n -= k;
        }
    }
    // Copies up to `max` bytes of the next `n` into `out` and skips the rest.
    void take(uint64_t n, size_t max, std::string& out, const char* what) {
        out.clear();
        while (n > 0 && out.size() < max) {
            out.push_back((char)u8(what));
            --n;
        }
        skip(n, what);
    }

private:
    void refill() {
        pos_ = 0;
        len_ = 0;
        while (len_ == 0) {
            const size_t got = read_(buf_.data(), buf_.size());
            if (got == 0) return;
            len_ = got;
        }
    }
    const ByteReader& read_;
    std::vector<uint8_t> buf_;
    size_t pos_ = 0, len_ = 0;
    uint64_t consumed_ = 0;
};

// The 24-bit field CJCMCG's writer emits for its synthetic first tempo record,
// reproduced with the C# arithmetic: y = 5e9 / resol (a double), each byte =
// (byte)(y / 256^k % 256) -- fmod, then truncation.
uint32_t synthetic_tempo_field(uint32_t resol) {
    if (resol == 0) return UINT32_MAX;   // CJCMCG would have divided by zero
    const double y = 5000000000.0 / (double)resol;
    const uint32_t b0 = (uint32_t)std::fmod(y / 256.0 / 256.0, 256.0);
    const uint32_t b1 = (uint32_t)std::fmod(y / 256.0, 256.0);
    const uint32_t b2 = (uint32_t)std::fmod(y, 256.0);
    return (b0 << 16) | (b1 << 8) | b2;
}

} // namespace

bool has_cjcmcg_name(const std::string& filename) {
    std::string f = filename;
    for (char& c : f) c = (char)std::tolower((unsigned char)c);
    const size_t slash = f.find_last_of("/\\");
    if (slash != std::string::npos) f = f.substr(slash + 1);
    return f.find(".cjcmcg") != std::string::npos;   // song.cjcmcg, song.cjcmcg.xz, ...
}

int scan_cjcmcg(const ByteReader& read, uint64_t total_bytes, TickData& td,
                std::vector<TempoChange>& tempo_raw, uint16_t& out_division,
                const ProgressCallbacks& cb, bool sniffing, bool vel0_as_note_off, std::string* why) {
    using clock = std::chrono::steady_clock;
    const clock::time_point t0 = clock::now();
    td.reset();
    tempo_raw.clear();
    Reader r(read);
    try {
        // ---- header + tempo table ------------------------------------------
        const uint32_t resol = r.u32("the header");
        const uint32_t ntempo = r.u32("the header");
        if (resol > 0xFFFF) throw NotCjcmcg{"resolution " + std::to_string(resol) + " is not a 16-bit MIDI division"};
        if (total_bytes && (uint64_t)ntempo * 7 > total_bytes)
            throw NotCjcmcg{"tempo count " + std::to_string(ntempo) + " exceeds the file size"};
        // Same division rule as a MIDI header (CJCMCG copied the 16 bits verbatim).
        uint16_t division = (uint16_t)resol;
        if ((division >> 8) < 0xE7) division &= 0x7FFF;
        if (division == 0) division = 480;
        uint64_t last_event_tick = 0;
        bool synthetic = false;
        for (uint32_t i = 0; i < ntempo; ++i) {
            const uint32_t tick = r.u32("the tempo table");
            const uint32_t us = r.u24("the tempo table");
            if (i == 0 && tick == 0 && us == synthetic_tempo_field(resol)) { synthetic = true; continue; }
            if (us > 0) tempo_raw.push_back({tick, 0.0, us, 60000000.0 / us});
            last_event_tick = std::max<uint64_t>(last_event_tick, tick);
        }
        // ---- lyrics (not an fMCG stat: counted for the log only) ----------
        const uint32_t nlyric = r.u32("the lyric count");
        if (total_bytes && (uint64_t)nlyric * 8 > total_bytes)
            throw NotCjcmcg{"lyric count " + std::to_string(nlyric) + " exceeds the file size"};
        uint64_t lyrics = 0;
        std::string first_lyric, text;
        for (uint32_t i = 0; i < nlyric; ++i) {
            const uint32_t tick = r.u32("the lyric table");
            const uint32_t len = r.u32("the lyric table");
            if (total_bytes && len > total_bytes) throw NotCjcmcg{"lyric length exceeds the file size"};
            r.take(len, 64, text, "the lyric table");
            if (len > 0) {
                ++lyrics;
                if (first_lyric.empty()) first_lyric = text;
                last_event_tick = std::max<uint64_t>(last_event_tick, tick);
            }
        }
        // ---- note counts ------------------------------------------------------
        const uint64_t declared_notes = r.varint("the total note count");
        const uint32_t nticks = r.u32("the tick count");
        if (total_bytes && (uint64_t)nticks * 2 > total_bytes)
            throw NotCjcmcg{"tick count " + std::to_string(nticks) + " exceeds the file size"};

        uint64_t total_ons = 0, total_offs = 0, unmatched_offs = 0, poly = 0;
        clock::time_point last_ping = t0;
        for (uint32_t t = 0; t < nticks; ++t) {
            const uint64_t on = r.varint("the tick table");
            const uint64_t off = r.varint("the tick table");
            if ((t & 0xFFFF) == 0) {
                if (cb.cancel_flag && cb.cancel_flag->load(std::memory_order_relaxed)) throw ScanCancelled{};
                const clock::time_point now = clock::now();
                if (cb.on_scan_progress && now - last_ping > std::chrono::milliseconds(100)) {
                    last_ping = now;
                    const double el = std::chrono::duration<double>(now - t0).count();
                    cb.on_scan_progress(total_ons + total_offs, el, el > 0 ? (double)(total_ons + total_offs) / el : 0.0,
                                        total_bytes ? (double)r.consumed() / (double)total_bytes
                                                    : (double)t / (double)nticks);
                }
            }
            if (on == 0 && off == 0) continue;
            // Polyphony never goes below zero: offs beyond what is held are
            // ignored (ons are taken to come first within the tick).
            const uint64_t held = poly + on;
            const uint64_t eff_off = std::min(off, held);
            unmatched_offs += off - eff_off;
            poly = held - eff_off;
            td.sparse.push_back({t, on, (int64_t)on - (int64_t)eff_off, 0});
            total_ons += on;
            total_offs += off;
        }
        uint64_t trailing = 0;
        while (!r.at_end() && trailing < (1u << 20)) { r.skip(1, "trailing data"); ++trailing; }
        if (sniffing && trailing)
            throw NotCjcmcg{"data continues after the tick table"};

        // ---- hand over to the sweep ---------------------------------------------
        if (td.sparse.empty()) td.sparse.push_back({0, 0, 0, 0});   // keep event mode (no dense padding)
        td.total_ons = total_ons;
        td.total_cc = 0;
        td.max_tick = std::max(last_event_tick, nticks ? (uint64_t)(nticks - 1) : 0);
        td.ntracks = 0;   // a .cjcmcg has no track structure
        td.total_events_seen = total_ons + total_offs;
        out_division = division;

        std::ostringstream os;
        const double el = std::chrono::duration<double>(clock::now() - t0).count();
        os << "  CJCMCG preprocessed file: resolution " << resol << ", " << tempo_raw.size()
           << " tempo change(s)" << (synthetic ? " (+ CJCMCG's default-tempo record)" : "") << ", "
           << lyrics << " lyric(s), " << format_with_commas(nticks) << " ticks, "
           << format_with_commas(total_ons) << " notes, " << format_with_commas(td.sparse.size())
           << " active tick(s); read in " << std::fixed;
        os.precision(2);
        os << el << "s\n";
        emit(cb, os.str());
        if (!first_lyric.empty())
            emit(cb, "  Lyrics are not an fMCG stat and are ignored (first: \"" + first_lyric + "\").\n");
        if (declared_notes != total_ons)
            emit(cb, "  Warning: the file declares " + format_with_commas(declared_notes)
                     + " notes but its tick table holds " + format_with_commas(total_ons)
                     + "; the tick table is used.\n", true);
        if (unmatched_offs)
            emit(cb, "  " + format_with_commas(unmatched_offs)
                     + " note-off(s) without a held note were ignored (polyphony stays >= 0).\n");
        if (trailing)
            emit(cb, "  Warning: " + format_with_commas(trailing) + (trailing >= (1u << 20) ? "+" : "")
                     + " byte(s) after the tick table were ignored.\n", true);
        emit(cb, std::string("  Note: a .cjcmcg stores only per-tick note-on/note-off counts: velocity-0 "
                             "note-ons count as notes")
                 + (vel0_as_note_off ? " (\"Vel-0 as Note-Off\" cannot be applied)" : "")
                 + (close_held_notes() ? ", \"Close held notes\" cannot be applied (no tracks)" : "")
                 + ", there is no CC data, and the song ends at the last note/tempo/lyric tick.\n");
        return 1;
    } catch (const NotCjcmcg& e) {
        if (why) *why = e.why;
        if (!sniffing)
            emit(cb, "Error: not a valid .cjcmcg file: " + e.why + ".\n", true);
        td.reset();
        tempo_raw.clear();
        return sniffing ? 0 : -1;
    }
}

} // namespace fmcg_stream
