// fmcg_layered.cpp -- exact dedup walk over layered (nested, tiled) xz archives.
//
// Construction this handles: a MIDI byte stream is cut into tiles (ANY
// consecutive byte ranges: whole files, header pieces, whole tracks, or raw
// slices cut mid-event), each tile is xz-compressed, the compressed tiles are
// concatenated (with repetition), and the result is compressed and tiled
// again, several layers deep. Expanded sizes reach terabytes.
//
// Definition of correct: the result must equal the STANDARD scan
// (fmcg_engine.cpp scan_stream + parse_track_body) of the fully expanded flat
// byte stream. test/dialects/oracle.py is the executable reference.
//
// How (nothing is ever expanded in full):
//  1. Discovery. The file is a chain of xz streams (cut backward via each
//     stream's footer + index). Every member is keyed by (xxh64, length);
//     each UNIQUE member is decoded once: if its payload starts with the xz
//     magic it is an internal node whose payload is cut (forward, at
//     CRC-validated stream footers) into a run-length list of child members;
//     otherwise it is a leaf (raw MIDI bytes). Result: a grammar (DAG) whose
//     in-order leaf expansion is the flat stream.
//  2. Walk. A resumable byte-level transducer that mirrors the standard
//     scanner exactly (chunk framing, MThd quirks, running status, VLQs,
//     meta/sysex skips, refcounted note-offs) is run over the grammar. The
//     effect of a node on the parser is memoized per (node, parser shape):
//     the shape is the full parser state EXCEPT the affine parts (current
//     tick, remaining chunk bytes, remaining skip bytes) and the refcount
//     table. Effects store their output relative to the start tick, their
//     refcount behaviour as per-key (deficit, net) pairs (an effect computed
//     with "every note-off fires" is exact whenever the incoming refcount of
//     each key is >= its deficit; otherwise the node is re-walked with the
//     concrete refcounts), and validity conditions on the affine counters.
//     Runs of identical children (x65535 per layer is typical) are applied
//     analytically once the parser shape repeats (period <= 8 children).
//  3. Materialize. Effects reference per-tile tick contributions
//     (run-length cells) with (shift, arithmetic progression, weight); these
//     expand into sparse cells and arithmetic tick runs for the sweep.
//
// Anything the walk cannot interpret is reported loudly; nothing is skipped
// silently.

#include "fmcg_engine_internal.h"
#include "fmcg_engine.h"
#include "fmcg_util.h"
#include "xxhash64.h"

#include "archive.h"
#include "archive_entry.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <memory>
#include <sstream>
#include <unordered_map>

namespace fmcg_stream {
namespace {

// ============================================================================
// xz container helpers
// ============================================================================

constexpr uint8_t kXzMagic[6] = {0xFD, '7', 'z', 'X', 'Z', 0x00};

uint32_t crc32_ieee(const uint8_t* p, size_t n) {
    static const std::array<uint32_t, 256> tab = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

inline uint32_t rd_le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool xz_vli(const uint8_t* b, size_t end, size_t& i, uint64_t& v) {
    v = 0;
    for (int k = 0; k < 9; ++k) {
        if (i >= end) return false;
        const uint8_t c = b[i++];
        v |= (uint64_t)(c & 0x7F) << (7 * k);
        if (!(c & 0x80)) return true;
    }
    return false;
}

// Validate the index + footer ending at `fe` (footer end) of a stream whose
// header starts at b[0]. Returns false unless b[0..fe) is exactly one complete
// xz stream. `unc_out` receives the declared uncompressed payload size.
bool xz_stream_exact(const uint8_t* b, size_t fe, uint64_t* unc_out) {
    if (fe < 32 || std::memcmp(b, kXzMagic, 6) != 0) return false;
    const uint8_t* f = b + fe - 12;
    if (f[10] != 'Y' || f[11] != 'Z' || f[8] != b[6] || f[9] != b[7]) return false;
    if (crc32_ieee(f + 4, 6) != rd_le32(f)) return false;
    if (crc32_ieee(b + 6, 2) != rd_le32(b + 8)) return false;
    const uint64_t isz = ((uint64_t)rd_le32(f + 4) + 1) * 4;
    if (isz + 24 > fe) return false;
    const size_t ix = fe - 12 - (size_t)isz;
    if (b[ix] != 0x00 || crc32_ieee(b + ix, (size_t)isz - 4) != rd_le32(b + ix + isz - 4)) return false;
    size_t i = ix + 1;
    const size_t iend = ix + (size_t)isz - 4;
    uint64_t nrec = 0;
    if (!xz_vli(b, iend, i, nrec)) return false;
    uint64_t comp = 0, unc = 0;
    for (uint64_t r = 0; r < nrec; ++r) {
        uint64_t up = 0, us = 0;
        if (!xz_vli(b, iend, i, up) || !xz_vli(b, iend, i, us)) return false;
        comp += (up + 3) & ~(uint64_t)3;
        unc += us;
        if (comp > fe) return false;
    }
    if (12 + comp != ix) return false;
    if (unc_out) *unc_out = unc;
    return true;
}

// Cut a whole in-memory buffer of concatenated xz streams BACKWARD (footer ->
// index -> stream start). No decoding needed. False if the buffer is not a
// clean stream chain (stream padding allowed).
struct MemberRange { uint64_t off, len, unc; };
bool xz_cut_backward(const uint8_t* b, uint64_t n, std::vector<MemberRange>& out) {
    out.clear();
    uint64_t end = n;
    while (end > 0) {
        while (end >= 4 && rd_le32(b + end - 4) == 0) end -= 4;   // stream padding
        if (end == 0) break;
        if (end < 32) return false;
        const uint8_t* f = b + end - 12;
        if (f[10] != 'Y' || f[11] != 'Z' || crc32_ieee(f + 4, 6) != rd_le32(f)) return false;
        const uint64_t isz = ((uint64_t)rd_le32(f + 4) + 1) * 4;
        if (isz + 24 > end) return false;
        const uint64_t ix = end - 12 - isz;
        const uint8_t* xb = b + ix;
        if (xb[0] != 0x00 || crc32_ieee(xb, (size_t)isz - 4) != rd_le32(xb + isz - 4)) return false;
        size_t i = 1;
        uint64_t nrec = 0, comp = 0, unc = 0;
        if (!xz_vli(xb, (size_t)isz - 4, i, nrec)) return false;
        for (uint64_t r = 0; r < nrec; ++r) {
            uint64_t up = 0, us = 0;
            if (!xz_vli(xb, (size_t)isz - 4, i, up) || !xz_vli(xb, (size_t)isz - 4, i, us)) return false;
            comp += (up + 3) & ~(uint64_t)3;
            unc += us;
            if (comp > ix) return false;
        }
        if (ix < comp + 12) return false;
        const uint64_t start = ix - comp - 12;
        if (std::memcmp(b + start, kXzMagic, 6) != 0 || b[start + 6] != f[8] || b[start + 7] != f[9])
            return false;
        out.push_back({start, end - start, unc});
        end = start;
    }
    std::reverse(out.begin(), out.end());
    return !out.empty();
}

// Streaming xz decoder over an in-memory compressed member (libarchive raw
// format + xz filter). Decode errors stop the stream; what decoded is kept.
class XzDec {
    struct archive* a_ = nullptr;
    const uint8_t* p_;
    size_t n_;
    bool given_ = false, ok_ = false, err_ = false, eof_ = false;
    static la_ssize_t read_cb(struct archive*, void* self, const void** buf) {
        XzDec* d = static_cast<XzDec*>(self);
        if (d->given_) return 0;
        d->given_ = true;
        *buf = d->p_;
        return (la_ssize_t)d->n_;
    }
public:
    XzDec(const uint8_t* p, size_t n) : p_(p), n_(n) {
        a_ = archive_read_new();
        archive_read_support_format_raw(a_);
        archive_read_append_filter(a_, ARCHIVE_FILTER_XZ);
        archive_read_set_read_callback(a_, read_cb);
        archive_read_set_callback_data(a_, this);
        if (archive_read_open1(a_) != ARCHIVE_OK) { err_ = true; return; }
        struct archive_entry* e = nullptr;
        if (archive_read_next_header(a_, &e) != ARCHIVE_OK) { err_ = true; return; }
        ok_ = true;
    }
    ~XzDec() { if (a_) archive_read_free(a_); }
    XzDec(const XzDec&) = delete;
    XzDec& operator=(const XzDec&) = delete;
    size_t pull(uint8_t* out, size_t cap) {
        if (!ok_ || err_ || eof_) return 0;
        const la_ssize_t r = archive_read_data(a_, out, cap);
        if (r < 0) { err_ = true; return 0; }
        if (r == 0) eof_ = true;
        return (size_t)r;
    }
    // Zero-copy: next decoded block (valid until the next call); 0 = end.
    size_t pull_block(const uint8_t*& p) {
        if (!ok_ || err_ || eof_) return 0;
        const void* b = nullptr;
        size_t sz = 0;
        la_int64_t off = 0;
        const int r = archive_read_data_block(a_, &b, &sz, &off);
        if (r == ARCHIVE_EOF) { eof_ = true; return 0; }
        if (r < ARCHIVE_WARN) { err_ = true; return 0; }
        p = static_cast<const uint8_t*>(b);
        return sz;
    }
    bool error() const { return err_; }
};

std::string fmt_bytes(uint64_t b) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(2);
    if (b >= ((uint64_t)1 << 40)) os << (double)b / (double)((uint64_t)1 << 40) << " TiB";
    else if (b >= ((uint64_t)1 << 30)) os << (double)b / (double)((uint64_t)1 << 30) << " GiB";
    else if (b >= ((uint64_t)1 << 20)) os << (double)b / (double)((uint64_t)1 << 20) << " MiB";
    else if (b >= ((uint64_t)1 << 10)) os << (double)b / (double)((uint64_t)1 << 10) << " KiB";
    else os << b << " B";
    return os.str();
}

// Hard failure inside the walk: reported, scan returns -1.
struct WalkError { std::string msg; };
// The flat stream is not a MIDI file (e.g. an xz-wrapped .rar): the caller
// falls back to the standard scan, which knows the other container formats.
struct NotMidi {};

// ============================================================================
// Grammar
// ============================================================================

struct Node {
    enum Kind : uint8_t { PENDING, INTERNAL, LEAF, RAW } kind = PENDING;
    uint64_t hash = 0, clen = 0, unc = 0;   // unc: declared payload size (xz index)
    bool truncated = false;                 // no valid footer: decode prefix only
    bool decode_error = false;
    const uint8_t* cptr = nullptr;          // compressed bytes (mmap'd file range, or `comp`)
    std::vector<uint8_t> comp;              // owned compressed bytes (members of decoded payloads)
    std::vector<uint8_t> raw;               // RAW: payload bytes by value
    std::vector<std::pair<uint32_t, uint64_t>> kids;   // INTERNAL: run-length child list
};

// ============================================================================
// Parser state
// ============================================================================

enum : uint8_t { M_HDR, M_HDR_SKIP, M_TAG, M_LEN, M_SKIPCHUNK, M_BODY, M_ERR };
enum : uint8_t { E_TOP, E_VLQ, E_STATUS, E_META_TYPE, E_META_LEN, E_TEMPO, E_SYSEX_LEN, E_SKIP, E_DATA };
enum : uint8_t { REL = 0, ABS = 1 };

// Everything in the parser state except the affine parts (tick, remaining
// chunk bytes `rem`, remaining skip bytes `ctr`) and the refcount table.
// Fields not used by the current mode are kept zero so equal states compare
// equal.
struct Shape {
    uint8_t mode = M_HDR, sub = E_TOP, k = 0, running = 0;
    uint8_t status = 0, mt_split = 0, mtype = 0, n1 = 0;
    uint32_t acc = 0;    // tag / length / VLQ / tempo accumulator
    uint32_t tag = 0;    // completed chunk tag while the length is read
    uint8_t hdr[14] = {};
    bool operator==(const Shape& o) const {
        return mode == o.mode && sub == o.sub && k == o.k && running == o.running &&
               status == o.status && mt_split == o.mt_split && mtype == o.mtype && n1 == o.n1 &&
               acc == o.acc && tag == o.tag && std::memcmp(hdr, o.hdr, sizeof hdr) == 0;
    }
    bool operator!=(const Shape& o) const { return !(*this == o); }
    uint64_t hash() const {
        uint64_t h = (uint64_t)mode | ((uint64_t)sub << 8) | ((uint64_t)k << 16) | ((uint64_t)running << 24) |
                     ((uint64_t)status << 32) | ((uint64_t)mt_split << 40) | ((uint64_t)mtype << 48) |
                     ((uint64_t)n1 << 56);
        h ^= ((uint64_t)acc << 1) * 0x9E3779B97F4A7C15ull;
        h ^= ((uint64_t)tag + 0x632BE59BD9B4E019ull) * 0xC2B2AE3D27D4EB4Full;
        if (mode == M_HDR) h ^= XXHash64::hash(hdr, sizeof hdr, 7);
        return h;
    }
};

inline bool uses_rem(uint8_t mode) { return mode == M_BODY || mode == M_SKIPCHUNK || mode == M_HDR_SKIP; }
inline bool uses_ctr(const Shape& s) { return s.mode == M_BODY && s.sub == E_SKIP; }

struct State { Shape s; uint64_t rem = 0, ctr = 0; };

constexpr int kKeys = 16 * 128;   // (channel, note) refcount keys

// ============================================================================
// Contributions and effects
// ============================================================================

// Arithmetic run of identical cells: ticks start, start+step, ...
struct Run { uint64_t start, step, count, ons; int64_t delta; uint64_t cc; };

struct Contrib {
    std::vector<Run> runs;      // nondecreasing start; one track's (or one cell's) output
    uint64_t ons = 0, cc = 0;   // totals over all runs (x count)
    void add_cell(uint64_t t, uint64_t o, int64_t d, uint64_t c) {
        if (!o && !d && !c) return;
        ons += o; cc += c;
        if (!runs.empty()) {
            Run& L = runs.back();
            const uint64_t last_t = L.start + (L.count - 1) * L.step;
            if (t == last_t) {                       // same tick as the last cell: merge
                if (L.count == 1) { L.ons += o; L.delta += d; L.cc += c; return; }
                L.count--;
                runs.push_back({t, 1, 1, L.ons + o, L.delta + d, L.cc + c});
                return;
            }
            if (L.ons == o && L.delta == d && L.cc == c) {
                if (L.count == 1) { L.step = t - L.start; L.count = 2; return; }
                if (t == last_t + L.step) { L.count++; return; }
            }
        }
        runs.push_back({t, 1, 1, o, d, c});
    }
};

struct Item {
    uint32_t contrib;
    uint8_t frame;           // REL: relative to the effect's start tick; ABS: song tick
    uint64_t shift, step, count, weight;   // contrib shifted by shift + j*step, j < count; x weight
};
struct ItemKey {
    uint32_t contrib; uint8_t frame; uint64_t shift, step, count;
    bool operator==(const ItemKey& o) const {
        return contrib == o.contrib && frame == o.frame && shift == o.shift && step == o.step && count == o.count;
    }
};
struct ItemKeyHash {
    size_t operator()(const ItemKey& k) const {
        uint64_t h = (uint64_t)k.contrib * 0x9E3779B97F4A7C15ull ^ (uint64_t)k.frame;
        h ^= k.shift * 0xC2B2AE3D27D4EB4Full + (h << 6) + (h >> 2);
        h ^= k.step * 0x165667B19E3779F9ull + (h << 6) + (h >> 2);
        h ^= k.count * 0x27D4EB2F165667C5ull + (h << 6) + (h >> 2);
        return (size_t)h;
    }
};

struct TempoEv { uint8_t frame; uint64_t tick; uint32_t us; };
struct KeyFx { uint16_t key; int64_t d, net; };

struct Effect {
    Shape start, end;
    // affine counters: *_track = the start mode uses the counter; *_abs =
    // the counter's value at the end is absolute (the start chunk/skip ended,
    // or was never active), else *_val is the amount consumed from it.
    bool rem_track = false, rem_abs = true; uint64_t rem0 = 0, rem_val = 0;
    bool ctr_track = false, ctr_abs = true; uint64_t ctr0 = 0, ctr_val = 0;
    bool generic = true;        // initial-track refcounts unknown (valid for R >= d)
    uint64_t rhash = 0;         // !generic: hash of the refcounts it was computed for
    bool start_body = false;    // started inside an open track (the "initial track")
    bool init_ended = true;     // initial track ended inside (or never existed)
    uint64_t tick_val = 0;      // !init_ended: tick advance; init_ended: ABS tick of the open track
    bool has_init_end = false; uint64_t init_end_rel = 0;   // initial track ended at rel tick
    std::vector<KeyFx> rc_init; // initial-track refcount effect (until its end)
    int64_t init_net_sum = 0;   // sum of rc_init nets
    std::vector<std::pair<uint16_t, int64_t>> rc_end;   // init_ended: refcounts of the open track
    bool pending_close = false; // generic + close-held: initial track's closure needs sum(R_in)
    std::vector<Item> items;
    std::vector<TempoEv> tempos;
    bool has_abs_end = false; uint64_t max_abs_end = 0;
    uint64_t ntracks = 0, events = 0;
    bool div_set = false; uint16_t division = 480;
    bool not_midi = false, data_oor = false;
};
using EffPtr = std::shared_ptr<const Effect>;

struct MemoKey {
    uint32_t node; Shape s;
    bool operator==(const MemoKey& o) const { return node == o.node && s == o.s; }
};
struct MemoKeyHash {
    size_t operator()(const MemoKey& k) const { return (size_t)(k.s.hash() ^ ((uint64_t)k.node * 0x9E3779B97F4A7C15ull)); }
};

// ============================================================================
// The walk
// ============================================================================

class Walk;

// Accumulates the composed effect of a sequence of child effects.
class Composer {
public:
    Composer(Walk& w, const State& st, const int64_t* R);
    void apply(const Effect& c);
    void run_child(uint32_t child, uint64_t r);
    Effect finish();
    State state() const { return {cur_, rem_, ctr_}; }
    const int64_t* known_R() const { return r_known_ ? R_.data() : nullptr; }
private:
    void add_item(const Item& it);
    void add_cell(uint8_t frame, uint64_t t, int64_t delta, uint64_t weight);
    void note_abs_end(uint64_t t) { if (!e_.has_abs_end || t > e_.max_abs_end) { e_.has_abs_end = true; e_.max_abs_end = t; } }
    int64_t sum_R() const { int64_t s = 0; for (int64_t v : R_) s += v; return s; }
    uint64_t bound(const Effect& e) const;

    Walk& W_;
    Effect e_;
    Shape cur_;
    uint64_t rem_, ctr_;
    bool r_known_;
    std::vector<int64_t> R_;
    std::vector<int64_t> d_acc_, net_acc_;
    std::vector<uint16_t> touched_;
    std::vector<uint8_t> tflag_;
    std::unordered_map<ItemKey, uint64_t, ItemKeyHash> items_;
};

class Walk {
public:
    Walk(const ProgressCallbacks& cb, bool vel0) : cb_(cb), vel0_(vel0), close_held_(close_held_notes()) {}

    const ProgressCallbacks& cb_;
    const bool vel0_;
    const bool close_held_;
    std::vector<Node> nodes;
    std::vector<Contrib> contribs;
    std::unordered_map<MemoKey, std::vector<EffPtr>, MemoKeyHash> memo;

    // stats
    uint64_t leaf_parses = 0, leaf_bytes = 0, events_parsed = 0, decode_errors = 0;
    uint64_t memo_hits = 0, fast_runs = 0;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point last_ping = t0;

    void check_cancel() const {
        if (cb_.cancel_flag && cb_.cancel_flag->load(std::memory_order_relaxed)) throw ScanCancelled{};
    }
    // Throttled progress: `frac` is the fraction of the current unit of work
    // (one member's payload being cut, or one leaf being parsed), -1 = unknown.
    void ping(const char* phase = nullptr, double frac = -1.0) {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_ping < std::chrono::milliseconds(200)) return;
        last_ping = now;
        if (cb_.on_scan_progress) {
            const double el = std::chrono::duration<double>(now - t0).count();
            cb_.on_scan_progress(events_parsed, el, el > 0 ? (double)events_parsed / el : 0.0, frac);
        }
        if (phase && cb_.on_progress && frac >= 0.0)
            cb_.on_progress(phase, (int)(std::min(1.0, frac) * 100.0));
    }

    uint32_t new_contrib() { contribs.emplace_back(); return (uint32_t)(contribs.size() - 1); }

    static bool counters_ok(const Effect& e, const State& st) {
        if (e.rem_track) { if (e.rem_abs ? st.rem != e.rem0 : st.rem <= e.rem_val) return false; }
        if (e.ctr_track) { if (e.ctr_abs ? st.ctr != e.ctr0 : st.ctr <= e.ctr_val) return false; }
        return true;
    }
    static bool deficits_ok(const Effect& e, const int64_t* R) {
        for (const KeyFx& k : e.rc_init) if (k.d > 0 && R[k.key] < k.d) return false;
        return true;
    }
    static uint64_t hash_R(const int64_t* R) { return XXHash64::hash(R, sizeof(int64_t) * kKeys, 11); }

    // Effect of node `id` started at `st`. R: concrete refcounts of the open
    // track, or null when unknown (generic computation).
    EffPtr get_effect(uint32_t id, const State& st, const int64_t* R) {
        const bool body = st.s.mode == M_BODY;
        std::vector<EffPtr>& lst = memo[MemoKey{id, st.s}];
        bool generic_found = false;
        for (const EffPtr& e : lst) {
            if (!e->generic || !counters_ok(*e, st)) continue;
            generic_found = true;
            if (!body || !R || deficits_ok(*e, R)) { ++memo_hits; return e; }
            break;
        }
        if (!generic_found) {
            EffPtr g = compute(id, st, nullptr);
            memo[MemoKey{id, st.s}].push_back(g);
            if (!body || !R || deficits_ok(*g, R)) return g;
        }
        // Incoming refcounts below a deficit: exact re-walk with them.
        const uint64_t h = hash_R(R);
        for (const EffPtr& e : memo[MemoKey{id, st.s}])
            if (!e->generic && e->rhash == h && counters_ok(*e, st)) { ++memo_hits; return e; }
        auto k = std::make_shared<Effect>(*compute(id, st, R));
        k->rhash = h;
        EffPtr kp = k;
        memo[MemoKey{id, st.s}].push_back(kp);
        return kp;
    }

    EffPtr compute(uint32_t id, const State& st, const int64_t* R);
    EffPtr parse_leaf(uint32_t id, const State& st, const int64_t* R);
};

// ============================================================================
// Byte-level parser (one leaf from a given state) -- mirrors the standard scan
// ============================================================================

class Parser {
public:
    Parser(Walk& w, const State& st, const int64_t* R)
        : W_(w), vel0_(w.vel0_), close_held_(w.close_held_), R_(kKeys, 0), vmin_(kKeys, 0), tflag_(kKeys, 0) {
        s_ = st.s; rem_ = st.rem; ctr_ = st.ctr;
        e_.start = s_;
        e_.rem_track = uses_rem(s_.mode); e_.rem0 = rem_;
        e_.ctr_track = uses_ctr(s_); e_.ctr0 = ctr_;
        e_.start_body = s_.mode == M_BODY;
        init_open_ = e_.start_body;
        generic_ = !e_.start_body || R == nullptr;
        e_.generic = generic_;
        if (e_.start_body && R) { std::copy(R, R + kKeys, R_.begin()); Rstart_.assign(R, R + kKeys); }
        frame_ = init_open_ ? REL : ABS;
    }

    void feed(const uint8_t* p, size_t n) {
        size_t i = 0;
        while (i < n) {
            switch (s_.mode) {
            case M_HDR:
                s_.hdr[s_.k++] = p[i++];
                if (s_.k == 14) hdr_done();
                break;
            case M_HDR_SKIP:
            case M_SKIPCHUNK: {
                const uint64_t take = std::min<uint64_t>(rem_, n - i);
                i += (size_t)take; rem_ -= take;
                if (rem_ == 0) { rem_broken_ = true; enter_tag(); }
                break;
            }
            case M_TAG:
                s_.acc = (s_.acc << 8) | p[i++];
                if (++s_.k == 4) { s_.tag = s_.acc; s_.acc = 0; s_.k = 0; s_.mode = M_LEN; }
                break;
            case M_LEN:
                s_.acc = (s_.acc << 8) | p[i++];
                if (++s_.k == 4) { const uint32_t len = s_.acc, tag = s_.tag; s_.acc = 0; s_.k = 0; s_.tag = 0; start_chunk(tag, len); }
                break;
            case M_BODY:
                i += body(p + i, n - i);
                break;
            default:   // M_ERR: not a MIDI stream
                return;
            }
        }
    }

    Effect finish() {
        flush();
        Effect& e = e_;
        e.end = s_;
        e.rem_abs = !e.rem_track || rem_broken_;
        e.rem_val = e.rem_abs ? (uses_rem(s_.mode) ? rem_ : 0) : e.rem0 - rem_;
        e.ctr_abs = !e.ctr_track || ctr_broken_;
        e.ctr_val = e.ctr_abs ? (uses_ctr(s_) ? ctr_ : 0) : e.ctr0 - ctr_;
        e.init_ended = !init_open_;
        e.tick_val = tick_;
        if (init_open_) {
            build_rc_init();
        } else {
            for (int k = 0; k < kKeys; ++k) if (R_[k]) e.rc_end.push_back({(uint16_t)k, R_[k]});
        }
        e.events = events_;
        e.data_oor = data_oor_;
        return std::move(e);
    }

    uint64_t events() const { return events_; }

private:
    // ---- chunk level ----
    void hdr_done() {
        const uint8_t* h = s_.hdr;
        if (!(h[0] == 'M' && h[1] == 'T' && h[2] == 'h' && h[3] == 'd')) {
            e_.not_midi = true; s_ = Shape(); s_.mode = M_ERR; return;
        }
        const uint32_t hlen = ((uint32_t)h[4] << 24) | ((uint32_t)h[5] << 16) | ((uint32_t)h[6] << 8) | h[7];
        const uint8_t d2 = h[12], d1 = h[13];
        uint16_t division = (uint16_t)(((uint16_t)d2 << 8) | d1);
        bool mt_split = false;
        if (division == 0x4D54) { division = 480; mt_split = true; }
        else if (d2 < 0xE7) division &= 0x7FFF;
        if (division == 0) division = 480;
        e_.div_set = true; e_.division = division;
        std::memset(s_.hdr, 0, sizeof s_.hdr);
        s_.k = 0;
        s_.mt_split = mt_split ? 1 : 0;
        if (hlen > 6) { s_.mode = M_HDR_SKIP; rem_ = hlen - 6; }
        else enter_tag();
    }
    void enter_tag() {
        s_.mode = M_TAG;
        if (s_.mt_split) { s_.acc = 0x4D54; s_.k = 2; s_.mt_split = 0; }
        else { s_.acc = 0; s_.k = 0; }
    }
    void start_chunk(uint32_t tag, uint32_t len) {
        if (tag == 0x4D54726Bu) {   // "MTrk"
            s_.mode = M_BODY; s_.sub = E_TOP; s_.running = 0;
            rem_ = len; tick_ = 0; frame_ = ABS; cur_ = UINT32_MAX;
            if (len == 0) end_track();
        } else if (len == 0) {
            enter_tag();
        } else {
            s_.mode = M_SKIPCHUNK; rem_ = len;
        }
    }

    // ---- track body ----
    size_t body(const uint8_t* p, size_t n) {
        const size_t lim = (size_t)std::min<uint64_t>(n, rem_);
        size_t i = 0;
        while (i < lim) {
            if (s_.sub == E_TOP && lim - i >= 8) {
                // Fast path: whole [delta][status?][data..] channel events
                // with >= 8 bytes in hand (VLQ <= 4 + status + 2 data). Any
                // other event leaves at E_STATUS (delta already applied) for
                // the byte-wise machine below; semantics are identical.
                const size_t fast_end = lim - 8;
                while (i <= fast_end) {
                    size_t j = i;
                    uint8_t b = p[j++];
                    uint32_t v = b;
                    if (b >= 0x80) {
                        v &= 0x7F; b = p[j++]; v = (v << 7) | (b & 0x7F);
                        if (b >= 0x80) {
                            b = p[j++]; v = (v << 7) | (b & 0x7F);
                            if (b >= 0x80) { b = p[j++]; v = (v << 7) | (b & 0x7F); }
                        }
                    }
                    if (v) { tick_ += v; if (has_p_ && tick_ != p_tick_) flush(); }
                    const uint8_t st = p[j];
                    uint8_t status;
                    if (st >= 0x80) {
                        if (st >= 0xF0) { i = j; s_.sub = E_STATUS; break; }
                        status = st; s_.running = st; ++j;
                    } else {
                        status = s_.running;
                        if (!status) { i = j; s_.sub = E_STATUS; break; }
                    }
                    const uint8_t et = status & 0xF0;
                    if (et == 0xC0 || et == 0xD0) ++j;
                    else { channel(status, p[j], p[j + 1]); j += 2; }
                    ++events_;
                    i = j;
                }
                continue;
            }
            const uint8_t b = p[i];
            switch (s_.sub) {
            case E_TOP:
                s_.sub = E_VLQ; s_.k = 0; s_.acc = 0;
                [[fallthrough]];
            case E_VLQ:
                ++i;
                s_.acc = (s_.acc << 7) | (b & 0x7F);
                if (b < 0x80 || ++s_.k == 4) {
                    const uint32_t v = s_.acc;
                    s_.k = 0; s_.acc = 0; s_.sub = E_STATUS;
                    tick_ += v;
                    if (has_p_ && tick_ != p_tick_) flush();
                }
                break;
            case E_STATUS:
                if (b >= 0x80) { ++i; s_.status = b; s_.running = b < 0xF0 ? b : (b >= 0xF8 && b != 0xFF) ? s_.running : 0; }   // F8..FE keep it
                else s_.status = s_.running;
                dispatch();
                break;
            case E_META_TYPE:
                ++i; s_.mtype = b; s_.status = 0; s_.sub = E_META_LEN; s_.k = 0; s_.acc = 0;
                break;
            case E_META_LEN:
            case E_SYSEX_LEN:
                ++i;
                s_.acc = (s_.acc << 7) | (b & 0x7F);
                if (b < 0x80 || ++s_.k == 4) {
                    const uint32_t L = s_.acc;
                    const bool tempo = s_.sub == E_META_LEN && s_.mtype == 0x51 && L == 3;
                    s_.k = 0; s_.acc = 0; s_.mtype = 0;
                    if (tempo) s_.sub = E_TEMPO;
                    else start_skip(L);
                }
                break;
            case E_TEMPO:
                ++i;
                s_.acc = (s_.acc << 8) | b;
                if (++s_.k == 3) {
                    const uint32_t us = s_.acc;
                    if (us) e_.tempos.push_back({frame_, tick_, us});
                    event_done();
                }
                break;
            case E_SKIP: {
                const uint64_t take = std::min<uint64_t>(ctr_, lim - i);
                i += (size_t)take; ctr_ -= take;
                if (ctr_ == 0) { ctr_broken_ = true; event_done(); }
                break;
            }
            case E_DATA: {
                ++i;
                const uint8_t et = s_.status & 0xF0;
                if (s_.k == 0 && (et == 0xC0 || et == 0xD0)) { event_done(); break; }
                if (s_.k == 0) { s_.n1 = b; s_.k = 1; break; }
                channel(s_.status, s_.n1, b);
                event_done();
                break;
            }
            }
        }
        rem_ -= i;
        if (rem_ == 0) {
            if (s_.sub == E_VLQ) tick_ += s_.acc;   // truncated delta still advances (ByteStream::vlq)
            end_track();
        }
        return i;
    }

    void dispatch() {
        const uint8_t st = s_.status;
        if (st == 0xFF) { s_.sub = E_META_TYPE; s_.status = 0; }
        else if (st == 0xF0 || st == 0xF7) { s_.sub = E_SYSEX_LEN; s_.status = 0; s_.k = 0; s_.acc = 0; }
        else if (st == 0xF1 || st == 0xF3) { s_.status = 0; start_skip(1); }
        else if (st == 0xF2) { s_.status = 0; start_skip(2); }
        else if (st >= 0xF0 || st < 0x80) event_done();   // no data / running status 0: consume nothing
        else { s_.sub = E_DATA; s_.k = 0; }
    }
    void start_skip(uint32_t L) {
        if (L == 0) { event_done(); return; }
        s_.sub = E_SKIP; ctr_ = L;
    }
    void event_done() {
        ++events_;
        s_.sub = E_TOP; s_.k = 0; s_.acc = 0; s_.status = 0; s_.mtype = 0; s_.n1 = 0;
        ctr_ = 0;
    }
    void touch(int k) { if (!tflag_[k]) { tflag_[k] = 1; touched_.push_back((uint16_t)k); } }
    void pend() { if (!has_p_) { has_p_ = true; p_tick_ = tick_; } }
    void channel(uint8_t st, uint8_t n1, uint8_t n2) {
        const uint8_t et = st & 0xF0;
        if (n1 > 0x7F) { data_oor_ = true; n1 &= 0x7F; }
        const int key = (st & 0x0F) * 128 + n1;
        if (et == 0x90 && (n2 > 0 || !vel0_)) {
            if (init_open_) touch(key);
            R_[key]++;
            pend(); p_ons_++; p_delta_++;
        } else if (et == 0x90 || et == 0x80) {
            if (init_open_ && generic_) {
                touch(key);
                if (--R_[key] < vmin_[key]) vmin_[key] = R_[key];
                pend(); p_delta_--;
            } else if (R_[key] > 0) {
                if (init_open_) touch(key);
                R_[key]--;
                pend(); p_delta_--;
            }
        } else if (et == 0xB0) {
            pend(); p_cc_++;
        }
    }
    void flush() {
        if (!has_p_) return;
        cell(p_tick_, p_ons_, p_delta_, p_cc_);
        has_p_ = false; p_ons_ = 0; p_delta_ = 0; p_cc_ = 0;
    }
    void cell(uint64_t t, uint64_t o, int64_t d, uint64_t c) {
        if (!o && !d && !c) return;
        if (cur_ == UINT32_MAX) {
            cur_ = W_.new_contrib();
            e_.items.push_back({cur_, frame_, 0, 0, 1, 1});
        }
        W_.contribs[cur_].add_cell(t, o, d, c);
    }
    void build_rc_init() {
        int64_t sum = 0;
        for (uint16_t k : touched_) {
            KeyFx fx{k, 0, 0};
            if (generic_) { fx.net = R_[k]; fx.d = vmin_[k] < 0 ? -vmin_[k] : 0; }
            else fx.net = R_[k] - Rstart_[k];
            sum += fx.net;
            if (fx.net || fx.d) e_.rc_init.push_back(fx);
        }
        std::sort(e_.rc_init.begin(), e_.rc_init.end(), [](const KeyFx& a, const KeyFx& b) { return a.key < b.key; });
        e_.init_net_sum = sum;
    }
    void end_track() {
        flush();
        if (init_open_) {
            e_.has_init_end = true; e_.init_end_rel = tick_;
            build_rc_init();
            if (close_held_) {
                if (generic_) e_.pending_close = true;
                else { int64_t s = 0; for (int64_t v : R_) s += v; if (s) cell(tick_, 0, -s, 0); }
            }
            init_open_ = false;
        } else {
            if (!e_.has_abs_end || tick_ > e_.max_abs_end) { e_.has_abs_end = true; e_.max_abs_end = tick_; }
            if (close_held_) { int64_t s = 0; for (int64_t v : R_) s += v; if (s) cell(tick_, 0, -s, 0); }
        }
        e_.ntracks++;
        std::fill(R_.begin(), R_.end(), 0);
        rem_broken_ = true;
        ctr_broken_ = true;
        ctr_ = 0;
        cur_ = UINT32_MAX; frame_ = ABS; tick_ = 0;
        s_.sub = E_TOP; s_.k = 0; s_.acc = 0; s_.status = 0; s_.mtype = 0; s_.n1 = 0; s_.running = 0;
        enter_tag();
    }

    Walk& W_;
    const bool vel0_, close_held_;
    Effect e_;
    Shape s_;
    uint64_t rem_ = 0, ctr_ = 0, tick_ = 0;
    bool rem_broken_ = false, ctr_broken_ = false;
    bool init_open_ = false, generic_ = true;
    std::vector<int64_t> R_, vmin_, Rstart_;
    std::vector<uint8_t> tflag_;
    std::vector<uint16_t> touched_;
    uint8_t frame_ = ABS;
    uint32_t cur_ = UINT32_MAX;
    bool has_p_ = false;
    uint64_t p_tick_ = 0, p_ons_ = 0, p_cc_ = 0;
    int64_t p_delta_ = 0;
    uint64_t events_ = 0;
    bool data_oor_ = false;
};

EffPtr Walk::parse_leaf(uint32_t id, const State& st, const int64_t* R) {
    Node& n = nodes[id];
    Parser p(*this, st, R);
    ++leaf_parses;
    uint64_t before = 0;
    uint64_t fed = 0;
    const uint64_t total = n.kind == Node::RAW ? n.raw.size() : n.unc;
    auto progress = [&](size_t got) {
        leaf_bytes += got;
        fed += got;
        const uint64_t ev = p.events();
        events_parsed += ev - before;
        before = ev;
        check_cancel();
        ping("Layered parse", total ? (double)fed / (double)total : -1.0);
    };
    if (n.kind == Node::RAW) {
        constexpr size_t kStep = 4u << 20;
        for (size_t off = 0; off < n.raw.size(); off += kStep) {
            const size_t m = std::min(kStep, n.raw.size() - off);
            p.feed(n.raw.data() + off, m);
            progress(m);
        }
    } else if (n.unc > 0 || n.truncated) {
        XzDec dec(n.cptr, (size_t)n.clen);
        std::vector<uint8_t> buf(1u << 20);
        for (;;) {
            const size_t got = dec.pull(buf.data(), buf.size());
            if (!got) break;
            p.feed(buf.data(), got);
            progress(got);
        }
        if (dec.error() && !n.decode_error) { n.decode_error = true; ++decode_errors; }
    }
    return std::make_shared<Effect>(p.finish());
}

EffPtr Walk::compute(uint32_t id, const State& st, const int64_t* R) {
    check_cancel();
    const Node& n = nodes[id];
    if (n.kind == Node::LEAF || n.kind == Node::RAW) {
        EffPtr e = parse_leaf(id, st, R);
        if (e->not_midi) throw NotMidi{};
        return e;
    }
    Composer c(*this, st, R);
    for (const auto& kid : nodes[id].kids) c.run_child(kid.first, kid.second);
    return std::make_shared<Effect>(c.finish());
}

// ============================================================================
// Composer
// ============================================================================

Composer::Composer(Walk& w, const State& st, const int64_t* R)
    : W_(w), cur_(st.s), rem_(st.rem), ctr_(st.ctr), R_(kKeys, 0),
      d_acc_(kKeys, 0), net_acc_(kKeys, 0), tflag_(kKeys, 0) {
    e_.start = st.s;
    e_.rem_track = uses_rem(st.s.mode); e_.rem0 = st.rem; e_.rem_abs = !e_.rem_track;
    e_.rem_val = e_.rem_abs ? st.rem : 0;
    e_.ctr_track = uses_ctr(st.s); e_.ctr0 = st.ctr; e_.ctr_abs = !e_.ctr_track;
    e_.ctr_val = e_.ctr_abs ? st.ctr : 0;
    e_.start_body = st.s.mode == M_BODY;
    e_.init_ended = !e_.start_body;
    e_.tick_val = 0;
    e_.generic = !e_.start_body || R == nullptr;
    r_known_ = !e_.generic || !e_.start_body;   // outside a track the (fresh) refcounts are known zeros
    if (R && e_.start_body) std::copy(R, R + kKeys, R_.begin());
}

void Composer::add_item(const Item& it) {
    if (!it.weight) return;
    ItemKey k{it.contrib, it.frame, it.shift, it.count == 1 ? 0 : it.step, it.count};
    items_[k] += it.weight;
}

void Composer::add_cell(uint8_t frame, uint64_t t, int64_t delta, uint64_t weight) {
    if (!delta || !weight) return;
    const uint32_t c = W_.new_contrib();
    W_.contribs[c].add_cell(t, 0, delta, 0);
    add_item({c, frame, 0, 0, 1, weight});
}

void Composer::apply(const Effect& c) {
    // affine counters
    auto comp = [](bool& abs, uint64_t& val, bool cabs, uint64_t cval) {
        if (abs) val = cabs ? cval : val - cval;
        else if (cabs) { abs = true; val = cval; }
        else val += cval;
    };
    comp(e_.rem_abs, e_.rem_val, c.rem_abs, c.rem_val);
    rem_ = c.rem_abs ? c.rem_val : rem_ - c.rem_val;
    comp(e_.ctr_abs, e_.ctr_val, c.ctr_abs, c.ctr_val);
    ctr_ = c.ctr_abs ? c.ctr_val : ctr_ - c.ctr_val;

    // c's REL frame is the track open at c's start: ours (REL, shifted by our
    // tick advance) or an ABS track that started inside us.
    const uint8_t relf = e_.init_ended ? ABS : REL;
    const uint64_t sh = e_.tick_val;
    for (const Item& it : c.items) {
        if (it.frame == REL) add_item({it.contrib, relf, it.shift + sh, it.step, it.count, it.weight});
        else add_item(it);
    }
    for (const TempoEv& t : c.tempos)
        e_.tempos.push_back(t.frame == REL ? TempoEv{relf, t.tick + sh, t.us} : t);

    // c's initial track ended inside c (before its refcount effects apply)
    if (c.has_init_end) {
        const uint64_t t = c.init_end_rel + sh;
        if (e_.init_ended) {
            note_abs_end(t);
            if (c.pending_close) add_cell(ABS, t, -(sum_R() + c.init_net_sum), 1);
        } else {
            e_.has_init_end = true; e_.init_end_rel = t;
            if (c.pending_close) {
                if (r_known_) add_cell(REL, t, -(sum_R() + c.init_net_sum), 1);
                else e_.pending_close = true;
            }
        }
    }

    // refcounts
    if (!e_.init_ended) {
        for (const KeyFx& k : c.rc_init) {
            if (!tflag_[k.key]) { tflag_[k.key] = 1; touched_.push_back(k.key); }
            d_acc_[k.key] = std::max(d_acc_[k.key], k.d - net_acc_[k.key]);
            net_acc_[k.key] += k.net;
            if (r_known_) R_[k.key] += k.net;
        }
        e_.init_net_sum += c.init_net_sum;
    } else {
        for (const KeyFx& k : c.rc_init) R_[k.key] += k.net;
    }
    if (c.init_ended) {
        std::fill(R_.begin(), R_.end(), 0);
        for (const auto& kv : c.rc_end) R_[kv.first] = kv.second;
        r_known_ = true;
        e_.init_ended = true;
        e_.tick_val = c.tick_val;
    } else {
        e_.tick_val += c.tick_val;
    }

    if (c.has_abs_end) note_abs_end(c.max_abs_end);
    e_.ntracks += c.ntracks;
    e_.events += c.events;
    if (!e_.div_set && c.div_set) { e_.div_set = true; e_.division = c.division; }
    e_.not_midi |= c.not_midi;
    e_.data_oor |= c.data_oor;
    cur_ = c.end;
}

Effect Composer::finish() {
    Effect& e = e_;
    e.end = cur_;
    if (!e.init_ended) {
        for (uint16_t k : touched_) {
            if (d_acc_[k] || net_acc_[k]) e.rc_init.push_back({k, e.generic ? d_acc_[k] : 0, net_acc_[k]});
        }
        std::sort(e.rc_init.begin(), e.rc_init.end(), [](const KeyFx& a, const KeyFx& b) { return a.key < b.key; });
    } else {
        // rc_init still describes the initial track up to its end
        if (e.start_body) {
            for (uint16_t k : touched_)
                if (d_acc_[k] || net_acc_[k]) e.rc_init.push_back({k, e.generic ? d_acc_[k] : 0, net_acc_[k]});
            std::sort(e.rc_init.begin(), e.rc_init.end(), [](const KeyFx& a, const KeyFx& b) { return a.key < b.key; });
        }
        for (int k = 0; k < kKeys; ++k) if (R_[k]) e.rc_end.push_back({(uint16_t)k, R_[k]});
    }
    e.items.clear();
    e.items.reserve(items_.size());
    for (const auto& kv : items_)
        e.items.push_back({kv.first.contrib, kv.first.frame, kv.first.shift, kv.first.step, kv.first.count, kv.second});
    return std::move(e);
}

// How many further consecutive applications of `e` (which was just applied
// and is valid at the current state) stay valid.
uint64_t Composer::bound(const Effect& e) const {
    uint64_t k = UINT64_MAX;
    if (e.rem_track && !e.rem_abs && e.rem_val) {
        if (rem_ <= e.rem_val) return 0;
        k = std::min(k, (rem_ - 1) / e.rem_val);
    }
    if (e.ctr_track && !e.ctr_abs && e.ctr_val) {
        if (ctr_ <= e.ctr_val) return 0;
        k = std::min(k, (ctr_ - 1) / e.ctr_val);
    }
    if (r_known_ && !e.init_ended) {
        for (const KeyFx& f : e.rc_init) {
            if (f.net >= 0) continue;
            if (!e.generic) return 0;              // exact-R effect: refcounts change per copy
            if (R_[f.key] < f.d) return 0;
            k = std::min(k, (uint64_t)((R_[f.key] - f.d) / (-f.net)) + 1);
        }
    }
    return k;
}

// e applied k times in a row (caller verified the fixed point and bound()).
Effect power(Walk& W, const Effect& c, uint64_t k) {
    Effect p;
    p.start = c.start; p.end = c.end;
    p.rem_track = c.rem_track; p.rem_abs = c.rem_abs; p.rem0 = c.rem0;
    p.rem_val = c.rem_abs ? c.rem_val : c.rem_val * k;
    p.ctr_track = c.ctr_track; p.ctr_abs = c.ctr_abs; p.ctr0 = c.ctr0;
    p.ctr_val = c.ctr_abs ? c.ctr_val : c.ctr_val * k;
    p.generic = c.generic; p.rhash = c.rhash; p.start_body = c.start_body;
    p.init_ended = c.init_ended;
    p.has_abs_end = c.has_abs_end; p.max_abs_end = c.max_abs_end;
    p.ntracks = c.ntracks * k; p.events = c.events * k;
    p.div_set = c.div_set; p.division = c.division;
    p.not_midi = c.not_midi; p.data_oor = c.data_oor;
    auto note_abs = [&](uint64_t t) { if (!p.has_abs_end || t > p.max_abs_end) { p.has_abs_end = true; p.max_abs_end = t; } };
    constexpr uint64_t kItemBudget = (uint64_t)1 << 22;
    if (c.init_ended) {
        // Copy 1 runs in the incoming track; copies 2..k start in the track
        // the previous copy opened (ABS at c.tick_val) with refcounts c.rc_end.
        p.tick_val = c.tick_val;
        p.has_init_end = c.has_init_end; p.init_end_rel = c.init_end_rel;
        p.rc_init = c.rc_init; p.init_net_sum = c.init_net_sum; p.rc_end = c.rc_end;
        p.pending_close = c.pending_close;
        for (const Item& it : c.items) {
            if (it.frame == REL) {
                p.items.push_back(it);
                p.items.push_back({it.contrib, ABS, it.shift + c.tick_val, it.step, it.count, it.weight * (k - 1)});
            } else {
                p.items.push_back({it.contrib, it.frame, it.shift, it.step, it.count, it.weight * k});
            }
        }
        p.tempos = c.tempos;   // copies 2..k repeat one identical list: last-wins needs one copy
        for (const TempoEv& t : c.tempos)
            p.tempos.push_back(t.frame == REL ? TempoEv{ABS, t.tick + c.tick_val, t.us} : t);
        if (c.has_init_end) {
            const uint64_t te = c.tick_val + c.init_end_rel;
            note_abs(te);
            if (c.pending_close) {
                int64_t s = 0;
                for (const auto& kv : c.rc_end) s += kv.second;
                const int64_t d = -(s + c.init_net_sum);
                if (d) {
                    const uint32_t id = W.new_contrib();
                    W.contribs[id].add_cell(te, 0, d, 0);
                    p.items.push_back({id, ABS, 0, 0, 1, k - 1});
                }
            }
        }
    } else {
        const uint64_t D = c.tick_val;
        p.tick_val = D * k;
        for (const Item& it : c.items) {
            if (it.frame == ABS || D == 0) {
                p.items.push_back({it.contrib, it.frame, it.shift, it.step, it.count, it.weight * k});
            } else if (it.count == 1) {
                p.items.push_back({it.contrib, REL, it.shift, D, k, it.weight});
            } else if (it.step * it.count == D) {
                p.items.push_back({it.contrib, REL, it.shift, it.step, it.count * k, it.weight});
            } else {
                if (p.items.size() + k > kItemBudget)
                    throw WalkError{"layered walk: tick-shifted tile repetition too irregular to fold"};
                for (uint64_t j = 0; j < k; ++j)
                    p.items.push_back({it.contrib, REL, it.shift + j * D, it.step, it.count, it.weight});
            }
        }
        if (D == 0 || c.tempos.empty()) {
            p.tempos = c.tempos;
        } else {
            if (c.tempos.size() * k > kItemBudget)
                throw WalkError{"layered walk: too many tempo events in tick-shifted repetition"};
            for (uint64_t j = 0; j < k; ++j)
                for (const TempoEv& t : c.tempos)
                    p.tempos.push_back(t.frame == REL ? TempoEv{REL, t.tick + j * D, t.us} : t);
        }
        for (const KeyFx& f : c.rc_init) {
            KeyFx g = f;
            g.net = f.net * (int64_t)k;
            if (f.net < 0) g.d = f.d - (int64_t)(k - 1) * f.net;
            p.rc_init.push_back(g);
        }
        p.init_net_sum = c.init_net_sum * (int64_t)k;
    }
    return p;
}

void Composer::run_child(uint32_t child, uint64_t r) {
    uint64_t done = 0;
    std::vector<Shape> hist;   // start shapes of the recent copies (period detection)
    uint64_t iter = 0;
    while (done < r) {
        if ((++iter & 0x3FF) == 0) { W_.check_cancel(); W_.ping(); }
        const State st0 = state();
        EffPtr e = W_.get_effect(child, st0, known_R());
        apply(*e);
        ++done;
        if (done == r) break;
        // period 1: the copy maps the state onto itself
        if (e->start == cur_) {
            EffPtr e2 = W_.get_effect(child, state(), known_R());
            if (e2.get() == e.get()) {
                const uint64_t k = std::min(r - done, bound(*e));
                if (k >= 1) {
                    apply(power(W_, *e, k));
                    done += k;
                    ++W_.fast_runs;
                    hist.clear();
                    continue;
                }
            }
        }
        // period 2..8 (e.g. tiles whose length is not a multiple of the
        // event size shift the parse phase each copy)
        hist.push_back(st0.s);
        if (hist.size() > 8) hist.erase(hist.begin());
        int period = 0;
        for (int p = 2; p <= (int)hist.size(); ++p)
            if (hist[hist.size() - p] == cur_ && r - done >= (uint64_t)p) { period = p; break; }
        if (!period) continue;
        Composer tmp(W_, state(), cur_.mode == M_BODY ? nullptr : known_R());
        tmp.run_child(child, (uint64_t)period);
        Effect C = tmp.finish();
        if (!(C.start == C.end)) continue;
        // C must be valid here (generic: deficits vs our refcounts)
        if (cur_.mode == M_BODY && r_known_ && !Walk::deficits_ok(C, R_.data())) continue;
        if (!Walk::counters_ok(C, state())) continue;
        // validity of repeated application from the state after one C
        const uint64_t reps = (r - done) / (uint64_t)period;
        if (reps < 2) continue;
        uint64_t k = reps;
        if (C.rem_track && !C.rem_abs && C.rem_val) {
            if (rem_ <= C.rem_val) continue;
            k = std::min(k, rem_ / C.rem_val - (rem_ % C.rem_val == 0 ? 1 : 0));
        }
        if (C.ctr_track && !C.ctr_abs && C.ctr_val) {
            if (ctr_ <= C.ctr_val) continue;
            k = std::min(k, ctr_ / C.ctr_val - (ctr_ % C.ctr_val == 0 ? 1 : 0));
        }
        if (C.init_ended) {
            // copies 2..k start in the track C opened: same refcounts each time
            std::vector<int64_t> Re(kKeys, 0);
            for (const auto& kv : C.rc_end) Re[kv.first] = kv.second;
            if (!Walk::deficits_ok(C, Re.data())) k = std::min<uint64_t>(k, 1);
            if (C.rem_track && C.rem_abs && C.rem_val != C.rem0) k = std::min<uint64_t>(k, 1);
            if (C.ctr_track && C.ctr_abs && C.ctr_val != C.ctr0) k = std::min<uint64_t>(k, 1);
        } else if (r_known_) {
            for (const KeyFx& f : C.rc_init) {
                if (f.net >= 0) continue;
                if (R_[f.key] < f.d) { k = 0; break; }
                k = std::min(k, (uint64_t)((R_[f.key] - f.d) / (-f.net)) + 1);
            }
        }
        if (k < 1) continue;
        apply(power(W_, C, k));
        done += k * (uint64_t)period;
        ++W_.fast_runs;
        hist.clear();
    }
}

// ============================================================================
// Discovery
// ============================================================================

class Discovery {
public:
    Discovery(Walk& w, const ProgressCallbacks& cb, bool risky = false) : W_(w), cb_(cb), risky_(risky) {}

    struct Key { uint64_t h, n; bool operator==(const Key& o) const { return h == o.h && n == o.n; } };
    struct KeyHash { size_t operator()(const Key& k) const { return (size_t)(k.h ^ (k.n * 0x9E3779B97F4A7C15ull)); } };
    std::unordered_map<Key, uint32_t, KeyHash> index;
    std::vector<uint32_t> queue;
    uint64_t held_bytes = 0, decoded = 0, occurrences = 0;
    uint64_t truncated_members = 0, raw_tails = 0;
    uint64_t assumed_layers = 0;   // layers whose rest was ASSUMED (risky fast dedup)
    static constexpr uint64_t kMaxHeld = (uint64_t)6 << 30;   // owned compressed bytes of unique members

    // Intern one member; `owned`: copy the bytes (they live in a transient buffer).
    uint32_t intern(const uint8_t* p, uint64_t n, uint64_t unc, bool truncated, bool owned) {
        ++occurrences;
        const Key k{XXHash64::hash(p, (size_t)n, 0), n};
        auto it = index.find(k);
        if (it != index.end()) return it->second;
        Node nd;
        nd.hash = k.h; nd.clen = n; nd.unc = unc; nd.truncated = truncated;
        if (owned) {
            held_bytes += n;
            if (held_bytes > kMaxHeld)
                throw WalkError{"layered walk: unique compressed members exceed " + fmt_bytes(kMaxHeld)};
            nd.comp.assign(p, p + n);
        } else {
            nd.cptr = p;
        }
        W_.nodes.push_back(std::move(nd));
        const uint32_t id = (uint32_t)(W_.nodes.size() - 1);
        if (owned) W_.nodes[id].cptr = W_.nodes[id].comp.data();
        index.emplace(k, id);
        queue.push_back(id);
        return id;
    }

    static void push_kid(Node& parent, uint32_t id) {
        if (!parent.kids.empty() && parent.kids.back().first == id) parent.kids.back().second++;
        else parent.kids.push_back({id, 1});
    }

    // Forward cut of a decoded payload into xz members (validated footers).
    // `pull_block(const uint8_t*& p) -> size_t` yields decoded blocks (zero-copy).
    template <class PullBlock>
    void cut_payload(uint32_t parent, PullBlock pull_block, std::vector<uint8_t> prefix) {
        std::vector<uint8_t> buf = std::move(prefix);
        size_t head = 0, scan = 0;
        bool eof = false;
        const uint64_t unc_total = W_.nodes[parent].unc;
        uint64_t node_decoded = buf.size();
        // Recently seen members (length, node): the next member is usually one
        // of them (tiles repeat; BSOD cycles through 16), so its boundary is
        // checked directly and identity is an exact memcmp against the stored
        // copy -- no footer scan, no hashing of every byte.
        std::vector<std::pair<uint64_t, uint32_t>> recent;
        constexpr size_t kChunk = 4u << 20;
        auto avail = [&] { return buf.size() - head; };
        auto fill = [&]() -> bool {
            if (eof) return false;
            if (head > (64u << 20) && head * 2 > buf.size()) {
                buf.erase(buf.begin(), buf.begin() + (ptrdiff_t)head);
                scan = scan > head ? scan - head : 0;
                head = 0;
            }
            size_t got = 0;
            while (got < kChunk) {
                const uint8_t* bp = nullptr;
                const size_t sz = pull_block(bp);
                if (!sz) { eof = true; break; }
                buf.insert(buf.end(), bp, bp + sz);   // appends without zero-filling
                got += sz;
            }
            decoded += got;
            node_decoded += got;
            W_.ping("Layer discovery", unc_total ? (double)node_decoded / (double)unc_total : -1.0);
            W_.check_cancel();
            return got > 0;
        };
        auto remember = [&](uint64_t L, uint32_t id) {
            for (size_t i = 0; i < recent.size(); ++i)
                if (recent[i].first == L && recent[i].second == id) { recent.erase(recent.begin() + (ptrdiff_t)i); break; }
            recent.insert(recent.begin(), {L, id});
            if (recent.size() > 32) recent.pop_back();
        };
        // Risky fast dedup (opt-in, goal 8): the FIRST member that occurs
        // twice in a row is the layer's tile; everything before it is header.
        // If the rest of the layer (declared payload size minus the header)
        // is an exact multiple of the tile, it is ASSUMED to be that tile
        // repeated and the layer is not decoded further. Only that first
        // repeated tile is ever tried. Returns true when the assumption was
        // applied (the caller stops cutting this layer).
        bool risky_open = risky_ && unc_total > 0;
        uint32_t prev_id = UINT32_MAX;
        uint64_t prev_pos = 0, prev_len = 0;
        auto after_member = [&](uint32_t id, uint64_t pos, uint64_t L) -> bool {
            if (!risky_open) return false;
            if (!(id == prev_id && L == prev_len && pos == prev_pos + L)) {
                prev_id = id; prev_pos = pos; prev_len = L;
                return false;
            }
            risky_open = false;
            const uint64_t hdr = prev_pos;
            const uint64_t rest = unc_total > hdr ? unc_total - hdr : 0;
            std::ostringstream os;
            if (rest < 2 * L || rest % L != 0) {
                os << "  [layered] risky fast dedup: layer member " << parent << ": first repeated tile ("
                   << format_with_commas(L) << " bytes after " << format_with_commas(hdr)
                   << " header bytes) does not divide the remaining " << format_with_commas(rest)
                   << " bytes; layer decoded normally.\n";
                emit(cb_, os.str());
                return false;
            }
            const uint64_t m = rest / L;
            W_.nodes[parent].kids.back().second += m - 2;   // the two seen copies are already counted
            occurrences += m - 2;
            ++assumed_layers;
            os << "  WARNING: risky fast dedup ASSUMED layer member " << parent << " = "
               << format_with_commas(hdr) << " header bytes + a " << format_with_commas(L) << "-byte tile x "
               << format_with_commas(m) << " (" << fmt_bytes(rest - 2 * L)
               << " not decoded). The counts are wrong if that part of the layer holds other tiles.\n";
            emit(cb_, os.str(), true);
            return true;
        };
        for (;;) {
            if (avail() < 32 && !eof) { fill(); continue; }
            if (avail() == 0) break;
            const uint8_t* p = buf.data() + head;
            const size_t n = avail();
            if (n >= 4 && rd_le32(p) == 0) { head += 4; scan = 0; continue; }   // stream padding
            if (n < 6 || std::memcmp(p, kXzMagic, 6) != 0) {
                // Non-xz bytes after the members: a raw tail of the payload.
                while (fill()) {
                    if (avail() > ((uint64_t)1 << 30))
                        throw WalkError{"layered walk: raw tail after xz members larger than 1 GiB"};
                }
                Node raw;
                raw.kind = Node::RAW;
                raw.raw.assign(buf.data() + head, buf.data() + buf.size());
                raw.hash = XXHash64::hash(raw.raw.data(), raw.raw.size(), 1);
                raw.clen = raw.unc = raw.raw.size();
                W_.nodes.push_back(std::move(raw));
                push_kid(W_.nodes[parent], (uint32_t)(W_.nodes.size() - 1));
                ++raw_tails;
                break;
            }
            // Fast path: a recently seen member repeats here.
            bool found = false;
            for (size_t ri = 0; ri < recent.size(); ++ri) {
                const uint64_t L = recent[ri].first;
                const uint32_t rid = recent[ri].second;
                if (L > n) {
                    if (!eof && L <= ((uint64_t)1 << 31)) {
                        while (avail() < L && fill()) {}
                        found = true;   // re-evaluate with more data
                    }
                    break;
                }
                const Node& cand = W_.nodes[rid];
                if (cand.cptr && std::memcmp(p, cand.cptr, (size_t)L) == 0) {
                    ++occurrences;                 // byte-identical to a validated member
                    push_kid(W_.nodes[parent], rid);
                    if (ri) remember(L, rid);
                    if (after_member(rid, node_decoded - avail(), L)) return;
                    head += (size_t)L; scan = 0; found = true;
                    break;
                }
                uint64_t unc = 0;
                if (xz_stream_exact(p, (size_t)L, &unc)) {
                    const uint32_t id = intern(p, L, unc, false, true);
                    push_kid(W_.nodes[parent], id);
                    remember(L, id);
                    if (after_member(id, node_decoded - avail(), L)) return;
                    head += (size_t)L; scan = 0; found = true;
                    break;
                }
            }
            if (found) continue;
            // Footer scan: 'Y','Z' closing a footer whose index walks back to 0.
            size_t s = std::max<size_t>(scan, 30);
            while (s + 1 < n) {
                const uint8_t* y = (const uint8_t*)std::memchr(p + s, 'Y', n - 1 - s);
                if (!y) { s = n - 1; break; }
                const size_t yi = (size_t)(y - p);
                uint64_t unc = 0;
                if (p[yi + 1] == 'Z' && xz_stream_exact(p, yi + 2, &unc)) {
                    const uint64_t L = yi + 2;
                    const uint32_t id = intern(p, L, unc, false, true);
                    push_kid(W_.nodes[parent], id);
                    remember(L, id);
                    if (after_member(id, node_decoded - avail(), L)) return;
                    head += (size_t)L; scan = 0; found = true;
                    break;
                }
                s = yi + 1;
            }
            if (found) continue;
            scan = s;
            if (!eof) { fill(); continue; }
            // No footer before the payload ended: a truncated / damaged member.
            push_kid(W_.nodes[parent], intern(p, n, 0, true, true));
            ++truncated_members;
            head += n;
            break;
        }
    }

    // Decode each queued unique member once: classify leaf vs internal.
    void run() {
        while (!queue.empty()) {
            const uint32_t id = queue.back();
            queue.pop_back();
            Node& nd0 = W_.nodes[id];
            if (nd0.unc == 0 && !nd0.truncated) { nd0.kind = Node::LEAF; continue; }
            bool internal = false, derr = false;
            {
                XzDec dec(nd0.cptr, (size_t)nd0.clen);
                // Whole blocks only: mixing archive_read_data with
                // archive_read_data_block could drop a partly read block.
                std::vector<uint8_t> prefix;
                while (prefix.size() < 6) {
                    const uint8_t* bp = nullptr;
                    const size_t got = dec.pull_block(bp);
                    if (!got) break;
                    prefix.insert(prefix.end(), bp, bp + got);
                }
                internal = prefix.size() >= 6 && std::memcmp(prefix.data(), kXzMagic, 6) == 0;
                if (internal) {
                    W_.nodes[id].kind = Node::INTERNAL;
                    cut_payload(id, [&](const uint8_t*& bp) { return dec.pull_block(bp); }, std::move(prefix));
                }
                derr = dec.error();
            }
            if (internal) {
                Node& nd = W_.nodes[id];   // (vector may have grown)
                if (derr) { nd.decode_error = true; ++W_.decode_errors; }
                if (!nd.comp.empty()) { held_bytes -= nd.comp.size(); std::vector<uint8_t>().swap(nd.comp); }
                nd.cptr = nullptr;
                if (cb_.on_log) {
                    std::ostringstream os;
                    os << "  [layered] internal member " << id << ": " << nd.kids.size()
                       << " child run(s), " << W_.nodes.size() << " unique node(s) so far, "
                       << fmt_bytes(decoded) << " decoded\n";
                    W_.ping();
                    emit(cb_, os.str());
                }
            } else {
                W_.nodes[id].kind = Node::LEAF;
                if (derr) W_.nodes[id].decode_error = true;
            }
        }
    }

private:
    Walk& W_;
    const ProgressCallbacks& cb_;
    bool risky_ = false;
};

// Saturating flat-size computation over the grammar (for the log).
uint64_t flat_size(const Walk& W, uint32_t id, std::vector<uint64_t>& memo_sz) {
    if (memo_sz[id] != UINT64_MAX) return memo_sz[id];
    const Node& n = W.nodes[id];
    uint64_t s = 0;
    if (n.kind == Node::INTERNAL) {
        for (const auto& k : n.kids) {
            const uint64_t c = flat_size(W, k.first, memo_sz);
            const unsigned __int128 v = (unsigned __int128)c * k.second + s;
            s = v > UINT64_MAX ? UINT64_MAX : (uint64_t)v;
        }
    } else {
        s = n.kind == Node::RAW ? n.raw.size() : n.unc;
    }
    memo_sz[id] = s;
    return s;
}

} // namespace

// ============================================================================
// Entry point
// ============================================================================

int scan_layered(const std::string& filename, bool vel0_as_note_off, TickData& td,
                 std::vector<TempoChange>& tempo_raw, uint16_t& out_division,
                 const ProgressCallbacks& cb, bool risky) {
    BinaryReader reader(filename, 0, /*mmap=*/true);
    const uint8_t* img = reader.mmap_data();
    const uint64_t isz = reader.mmap_size();
    if (!img || isz < 32 || std::memcmp(img, kXzMagic, 6) != 0) return 0;

    std::vector<MemberRange> top;
    if (!xz_cut_backward(img, isz, top)) return 0;

    Walk W(cb, vel0_as_note_off);
    Discovery D(W, cb, risky);
    try {
        emit(cb, "  [layered] " + std::to_string(top.size()) + " top-level xz stream(s); discovering structure...\n");
        W.nodes.emplace_back();   // node 0: the file itself
        W.nodes[0].kind = Node::INTERNAL;
        for (const MemberRange& m : top) {
            const uint32_t id = D.intern(img + m.off, m.len, m.unc, false, false);
            Discovery::push_kid(W.nodes[0], id);
        }
        D.run();

        uint64_t internal = 0, leaves = 0, raws = 0, kidruns = 0;
        for (const Node& n : W.nodes) {
            if (n.kind == Node::INTERNAL) { ++internal; kidruns += n.kids.size(); }
            else if (n.kind == Node::LEAF) ++leaves;
            else if (n.kind == Node::RAW) ++raws;
        }
        std::vector<uint64_t> szmemo(W.nodes.size(), UINT64_MAX);
        const uint64_t flat = flat_size(W, 0, szmemo);
        {
            std::ostringstream os;
            os << "  [layered] structure: " << W.nodes.size() - 1 << " unique member(s) ("
               << internal - 1 << " internal, " << leaves << " leaf, " << raws << " raw tail), "
               << D.occurrences << " member occurrence(s) cut, " << kidruns << " child run(s), "
               << fmt_bytes(D.decoded) << " decoded; expanded stream "
               << (flat == UINT64_MAX ? std::string(">= 16 EiB") : fmt_bytes(flat) + " (" + format_with_commas(flat) + " bytes)")
               << "\n";
            emit(cb, os.str());
        }

        // ---- walk ----
        State s0;
        std::vector<int64_t> R0(kKeys, 0);
        EffPtr root = W.compute(0, s0, R0.data());
        const Effect& e = *root;

        // ---- end of stream: the standard scan's EOF semantics ----
        const Shape& fs = e.end;
        if (fs.mode == M_HDR) {
            emit(cb, e.end.k == 0 && !e.div_set ? "Error: not a MIDI file (missing MThd header).\n"
                                                : "Error: truncated MIDI header.\n", true);
            return -1;
        }
        if (fs.mode == M_LEN) { emit(cb, "Error: truncated chunk header.\n", true); return -1; }

        td.reset();
        uint64_t max_tick = e.has_abs_end ? e.max_abs_end : 0;
        uint64_t ntracks = e.ntracks;
        std::vector<int64_t> Rend(kKeys, 0);
        for (const auto& kv : e.rc_end) Rend[kv.first] = kv.second;
        std::vector<Item> items = e.items;
        if (fs.mode == M_BODY) {
            uint64_t t = e.tick_val;
            if (fs.sub == E_VLQ) t += fs.acc;   // truncated delta at EOF still advances
            max_tick = std::max(max_tick, t);
            ntracks++;
            if (e.rem_val > 0) td.desync_tracks++;
            if (W.close_held_) {
                int64_t s = 0;
                for (int64_t v : Rend) s += v;
                if (s) {
                    const uint32_t id = W.new_contrib();
                    W.contribs[id].add_cell(t, 0, -s, 0);
                    items.push_back({id, ABS, 0, 0, 1, 1});
                }
            }
        }

        // ---- materialize ----
        constexpr uint64_t kRunBudget = (uint64_t)64 << 20;
        uint64_t total_ons = 0, total_cc = 0;
        auto emit_run = [&](uint64_t start, uint64_t step, uint64_t count, uint64_t o, int64_t d, uint64_t c) {
            if (!o && !d && !c) return;
            if (count <= 8) {
                for (uint64_t j = 0; j < count; ++j) td.sparse.push_back({start + j * step, o, d, c});
            } else {
                if (td.runs.size() >= kRunBudget) throw WalkError{"layered walk: too many tick runs to materialize"};
                td.runs.push_back({start, step, count, o, d, c});
            }
        };
        for (const Item& it : items) {
            const Contrib& c = W.contribs[it.contrib];
            const uint64_t w = it.weight;
            total_ons += w * it.count * c.ons;
            total_cc += w * it.count * c.cc;
            for (const Run& r : c.runs) {
                const uint64_t o = r.ons * w, cc = r.cc * w;
                const int64_t d = r.delta * (int64_t)w;
                if (it.count == 1) emit_run(r.start + it.shift, r.step, r.count, o, d, cc);
                else if (r.count == 1) emit_run(r.start + it.shift, it.step, it.count, o, d, cc);
                else if (it.step == r.step * r.count) emit_run(r.start + it.shift, r.step, r.count * it.count, o, d, cc);
                else {
                    if (it.count > kRunBudget) throw WalkError{"layered walk: too many tick runs to materialize"};
                    for (uint64_t j = 0; j < it.count; ++j)
                        emit_run(r.start + it.shift + j * it.step, r.step, r.count, o, d, cc);
                }
            }
        }
        std::sort(td.sparse.begin(), td.sparse.end(),
                  [](const TickData::SparseCell& a, const TickData::SparseCell& b) { return a.tick < b.tick; });
        size_t w = 0;
        for (size_t i = 0; i < td.sparse.size(); ++i) {
            if (w && td.sparse[w - 1].tick == td.sparse[i].tick) {
                td.sparse[w - 1].ons += td.sparse[i].ons;
                td.sparse[w - 1].delta += td.sparse[i].delta;
                td.sparse[w - 1].cc += td.sparse[i].cc;
            } else {
                td.sparse[w++] = td.sparse[i];
            }
        }
        td.sparse.resize(w);
        if (td.sparse.empty() && td.runs.empty()) td.sparse.push_back({0, 0, 0, 0});   // keep event mode (no dense padding)
        td.total_ons = total_ons;
        td.total_cc = total_cc;
        td.max_tick = max_tick;
        td.ntracks = (size_t)ntracks;
        td.total_events_seen = e.events;
        tempo_raw.clear();
        for (const TempoEv& t : e.tempos)
            tempo_raw.push_back({t.tick, 0.0, t.us, 60000000.0 / t.us});
        out_division = e.div_set ? e.division : 480;

        if (e.data_oor)
            emit(cb, "  Warning: file contains note data bytes above 127 (invalid MIDI); "
                     "they are clamped to 127. Stats for those events may be approximate.\n", true);
        std::ostringstream os;
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - W.t0).count();
        os << "  [layered] walk: " << W.leaf_parses << " leaf parse(s) (" << fmt_bytes(W.leaf_bytes)
           << " parsed), " << W.memo_hits << " memo hit(s), " << W.fast_runs << " folded run(s), "
           << W.contribs.size() << " contribution(s), " << td.sparse.size() << " sparse cell(s) + "
           << td.runs.size() << " tick run(s), " << std::fixed << std::setprecision(2) << el << "s\n";
        os << "  Layer dedup: " << format_with_commas(total_ons) << " note-ons, "
           << format_with_commas(ntracks) << " track(s).\n";
        emit(cb, os.str());
        uint64_t trunc = 0, decerr = 0;
        for (const Node& n : W.nodes) { trunc += n.truncated; decerr += n.decode_error; }
        if (D.assumed_layers)
            emit(cb, "  WARNING: these counts rely on the risky fast-dedup assumption in "
                     + std::to_string(D.assumed_layers) + " layer(s) (see above); untick it for exact counts.\n", true);
        if (trunc || decerr || D.raw_tails) {
            std::ostringstream ws;
            ws << "  WARNING: " << decerr << " member(s) failed to decode, " << trunc
               << " member(s) have no valid xz footer, " << D.raw_tails
               << " raw tail(s): only the decodable bytes are counted (matches a flat decode of the same data).\n";
            emit(cb, ws.str(), true);
        }
        return 1;
    } catch (const NotMidi&) {
        emit(cb, "  [layered] decoded stream is not MIDI; using the standard scan.\n");
        return 0;
    } catch (const WalkError& we) {
        emit(cb, "Error: " + we.msg + "\n", true);
        return -1;
    }
}

} // namespace fmcg_stream
