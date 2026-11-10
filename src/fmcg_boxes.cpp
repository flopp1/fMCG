#include "fmcg_boxes.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace fmcg_boxes {

namespace {

struct Found {
    Node* node = nullptr;
    Node* parent = nullptr;
    int k = -1;          // index in parent->kids
};

// Depth-first search for leaf number `idx` (counted down as leaves pass).
bool find_leaf(Node& n, int& idx, Node* parent, int k, Found& f) {
    if (n.split == kLeaf) {
        if (idx-- == 0) { f.node = &n; f.parent = parent; f.k = k; return true; }
        return false;
    }
    for (size_t i = 0; i < n.kids.size(); ++i)
        if (find_leaf(n.kids[i], idx, &n, (int)i, f)) return true;
    return false;
}

Found find(Node& root, int leaf) {
    Found f;
    if (leaf < 0) return f;
    int idx = leaf;
    find_leaf(root, idx, nullptr, -1, f);
    return f;
}

void rects_of(const Node& n, const FRect& r, std::vector<FRect>& out) {
    if (n.split == kLeaf) { out.push_back(r); return; }
    double at = n.split == kColumns ? r.x0 : r.y0;
    const double len = n.split == kColumns ? r.x1 - r.x0 : r.y1 - r.y0;
    for (size_t i = 0; i < n.kids.size(); ++i) {
        const double end = i + 1 == n.kids.size() ? (n.split == kColumns ? r.x1 : r.y1) : at + n.weights[i] * len;
        FRect k = r;
        if (n.split == kColumns) { k.x0 = at; k.x1 = end; } else { k.y0 = at; k.y1 = end; }
        rects_of(n.kids[i], k, out);
        at = end;
    }
}

void dividers_of(const Node& n, const FRect& r, std::vector<int>& path, std::vector<Divider>& out) {
    if (n.split == kLeaf) return;
    const bool cols = n.split == kColumns;
    const double len = cols ? r.x1 - r.x0 : r.y1 - r.y0;
    std::vector<double> edge{cols ? r.x0 : r.y0};   // kid i spans [edge[i], edge[i+1]]
    for (size_t i = 0; i < n.kids.size(); ++i)
        edge.push_back(i + 1 == n.kids.size() ? (cols ? r.x1 : r.y1) : edge.back() + n.weights[i] * len);
    for (size_t i = 0; i + 1 < n.kids.size(); ++i) {
        Divider d;
        d.path = path;
        d.index = (int)i;
        d.dir = n.split;
        d.pos = edge[i + 1];
        d.lo = cols ? r.y0 : r.x0;
        d.hi = cols ? r.y1 : r.x1;
        d.min_pos = std::min(d.pos, edge[i] + kMinPart);
        d.max_pos = std::max(d.pos, edge[i + 2] - kMinPart);
        out.push_back(d);
    }
    for (size_t i = 0; i < n.kids.size(); ++i) {
        FRect k = r;
        if (cols) { k.x0 = edge[i]; k.x1 = edge[i + 1]; } else { k.y0 = edge[i]; k.y1 = edge[i + 1]; }
        path.push_back((int)i);
        dividers_of(n.kids[i], k, path, out);
        path.pop_back();
    }
}

void write(const Node& n, std::string& s) {
    if (n.split == kLeaf) { s += 'B'; return; }
    s += n.split == kColumns ? "C(" : "R(";
    for (size_t i = 0; i < n.kids.size(); ++i) {
        if (i) s += ',';
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.9g:", n.weights[i]);
        s += buf;
        write(n.kids[i], s);
    }
    s += ')';
}

bool read(const std::string& s, size_t& i, Node& n, int depth) {
    if (depth > 64 || i >= s.size()) return false;
    const char c = s[i];
    if (c == 'B') { ++i; n = Node(); return true; }
    if ((c != 'C' && c != 'R') || i + 1 >= s.size() || s[i + 1] != '(') return false;
    n = Node();
    n.split = c == 'C' ? kColumns : kRows;
    i += 2;
    for (;;) {
        const char* start = s.c_str() + i;
        char* end = nullptr;
        const double w = std::strtod(start, &end);
        if (end == start || !(w > 0.0) || !std::isfinite(w)) return false;
        i += (size_t)(end - start);
        if (i >= s.size() || s[i] != ':') return false;
        ++i;
        Node kid;
        if (!read(s, i, kid, depth + 1)) return false;
        n.weights.push_back(w);
        n.kids.push_back(std::move(kid));
        if (i >= s.size()) return false;
        if (s[i] == ',') { ++i; continue; }
        if (s[i] == ')') { ++i; break; }
        return false;
    }
    return !n.kids.empty() && n.kids.size() <= 256;
}

} // namespace

void normalize(Node& n) {
    if (n.split == kLeaf || n.kids.empty()) {
        n.split = kLeaf;
        n.kids.clear();
        n.weights.clear();
        return;
    }
    for (Node& k : n.kids) normalize(k);
    if (n.weights.size() != n.kids.size()) n.weights.assign(n.kids.size(), 1.0);
    // Same-direction kids join this split, their parts scaled by their share.
    std::vector<Node> kids;
    std::vector<double> w;
    for (size_t i = 0; i < n.kids.size(); ++i) {
        const double wi = n.weights[i] > 0.0 && std::isfinite(n.weights[i]) ? n.weights[i] : 1e-6;
        if (n.kids[i].split == n.split) {
            double sum = 0;
            for (double x : n.kids[i].weights) sum += x;
            for (size_t j = 0; j < n.kids[i].kids.size(); ++j) {
                w.push_back(wi * n.kids[i].weights[j] / (sum > 0 ? sum : 1.0));
                kids.push_back(std::move(n.kids[i].kids[j]));
            }
        } else {
            w.push_back(wi);
            kids.push_back(std::move(n.kids[i]));
        }
    }
    if (kids.size() == 1) {
        Node only = std::move(kids[0]);
        n = std::move(only);
        return;
    }
    double sum = 0;
    for (double x : w) sum += x;
    for (double& x : w) x /= sum;
    n.kids = std::move(kids);
    n.weights = std::move(w);
}

int leaf_count(const Node& n) {
    if (n.split == kLeaf) return 1;
    int c = 0;
    for (const Node& k : n.kids) c += leaf_count(k);
    return c;
}

Node* leaf_at(Node& root, int index) { return find(root, index).node; }
const Node* leaf_at(const Node& root, int index) { return find(const_cast<Node&>(root), index).node; }

std::vector<FRect> leaf_rects(const Node& root) {
    std::vector<FRect> out;
    rects_of(root, FRect{}, out);
    return out;
}

std::vector<fmcg_text::BoxPlace> place_list(const Node& root) {
    const std::vector<FRect> r = leaf_rects(root);
    std::vector<fmcg_text::BoxPlace> out;
    for (size_t i = 0; i < r.size(); ++i) {
        const Node* l = leaf_at(root, (int)i);
        fmcg_text::BoxPlace b;
        b.x0 = r[i].x0; b.y0 = r[i].y0; b.x1 = r[i].x1; b.y1 = r[i].y1;
        b.anchor = l->box.anchor;
        b.line_align = l->box.line_align;
        out.push_back(b);
    }
    return out;
}

int leaf_at_point(const Node& root, double x, double y) {
    if (x < 0 || y < 0 || x > 1 || y > 1) return -1;
    const std::vector<FRect> r = leaf_rects(root);
    for (size_t i = 0; i < r.size(); ++i)
        if (x >= r[i].x0 && x <= r[i].x1 && y >= r[i].y0 && y <= r[i].y1) return (int)i;
    return -1;
}

std::string structure_string(const Node& root) {
    std::string s;
    write(root, s);
    return s;
}

bool parse_structure(const std::string& s, Node& out) {
    std::string t;
    for (char c : s)
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') t += c;
    size_t i = 0;
    Node n;
    if (!read(t, i, n, 0) || i != t.size()) return false;
    normalize(n);
    out = std::move(n);
    return true;
}

bool split_leaf(Node& root, int leaf, int dir, int n) {
    if ((dir != kColumns && dir != kRows) || n < 2 || n > 16) return false;
    Found f = find(root, leaf);
    if (!f.node) return false;
    std::vector<Node> parts(n);
    parts[0].box = f.node->box;
    if (f.parent && f.parent->split == dir) {
        const double w = f.parent->weights[f.k];
        f.parent->kids.erase(f.parent->kids.begin() + f.k);
        f.parent->weights.erase(f.parent->weights.begin() + f.k);
        f.parent->kids.insert(f.parent->kids.begin() + f.k, parts.begin(), parts.end());
        f.parent->weights.insert(f.parent->weights.begin() + f.k, (size_t)n, w / n);
    } else {
        Node s;
        s.split = dir;
        s.kids = std::move(parts);
        s.weights.assign(n, 1.0 / n);
        *f.node = std::move(s);
    }
    normalize(root);
    return true;
}

int remove_leaf(Node& root, int leaf) {
    Found f = find(root, leaf);
    if (!f.node || !f.parent) return -1;
    Node* p = f.parent;
    const int k = f.k;
    const int nb = k > 0 ? k - 1 : 1;                 // neighbour kid
    // The neighbour's leaf next to the removed box gets its text.
    Node* t = &p->kids[nb];
    while (t->split != kLeaf) t = k > 0 ? &t->kids.back() : &t->kids.front();
    const std::string& moved = f.node->box.text;
    if (!moved.empty()) {
        if (!t->box.text.empty() && t->box.text.back() != '\n') t->box.text += '\n';
        t->box.text += moved;
    }
    p->weights[nb] += p->weights[k];
    p->kids.erase(p->kids.begin() + k);
    p->weights.erase(p->weights.begin() + k);
    normalize(root);
    // In reading order the receiving leaf is right before the removed one
    // (k > 0), or takes its number (k == 0).
    return k > 0 ? leaf - 1 : leaf;
}

bool equalize_parent(Node& root, int leaf) {
    Found f = find(root, leaf);
    if (!f.node || !f.parent) return false;
    f.parent->weights.assign(f.parent->kids.size(), 1.0 / (double)f.parent->kids.size());
    return true;
}

std::vector<Divider> dividers(const Node& root) {
    std::vector<Divider> out;
    std::vector<int> path;
    dividers_of(root, FRect{}, path, out);
    return out;
}

bool move_divider(Node& root, const Divider& d, double pos) {
    Node* n = &root;
    FRect r;
    for (int k : d.path) {   // walk down, tracking the node's rect
        if (n->split == kLeaf || k < 0 || (size_t)k >= n->kids.size()) return false;
        const bool cols = n->split == kColumns;
        double at = cols ? r.x0 : r.y0;
        const double len = cols ? r.x1 - r.x0 : r.y1 - r.y0;
        for (int i = 0; i < k; ++i) at += n->weights[i] * len;
        const double end = at + n->weights[k] * len;
        if (cols) { r.x0 = at; r.x1 = end; } else { r.y0 = at; r.y1 = end; }
        n = &n->kids[k];
    }
    if (n->split != d.dir || d.index < 0 || (size_t)d.index + 1 >= n->kids.size()) return false;
    const bool cols = n->split == kColumns;
    const double base = cols ? r.x0 : r.y0, len = cols ? r.x1 - r.x0 : r.y1 - r.y0;
    if (!(len > 0)) return false;
    double start = base;
    for (int i = 0; i < d.index; ++i) start += n->weights[i] * len;
    const double end = start + (n->weights[d.index] + n->weights[d.index + 1]) * len;
    const double lo = std::min(start + kMinPart, (start + end) / 2), hi = std::max(end - kMinPart, (start + end) / 2);
    pos = std::max(lo, std::min(hi, pos));
    const double pair = n->weights[d.index] + n->weights[d.index + 1];
    n->weights[d.index] = (pos - start) / len;
    n->weights[d.index + 1] = pair - n->weights[d.index];
    return true;
}

Node from_rect(const FRect& rect, const BoxContent& content) {
    const double eps = 1e-9;
    auto clamp01 = [](double v) { return std::max(0.0, std::min(1.0, v)); };
    const double x0 = clamp01(rect.x0), x1 = clamp01(std::max(rect.x1, rect.x0)),
                 y0 = clamp01(rect.y0), y1 = clamp01(std::max(rect.y1, rect.y0));
    Node mid;
    mid.box = content;
    if (y1 - y0 > eps && (y0 > eps || y1 < 1 - eps)) {   // rows: above / the box / below
        Node rows;
        rows.split = kRows;
        if (y0 > eps) { rows.kids.emplace_back(); rows.weights.push_back(y0); }
        rows.kids.push_back(mid); rows.weights.push_back(y1 - y0);
        if (y1 < 1 - eps) { rows.kids.emplace_back(); rows.weights.push_back(1 - y1); }
        mid = std::move(rows);
    }
    Node root = mid;
    if (x1 - x0 > eps && (x0 > eps || x1 < 1 - eps)) {   // columns: left / the box / right
        Node cols;
        cols.split = kColumns;
        if (x0 > eps) { cols.kids.emplace_back(); cols.weights.push_back(x0); }
        cols.kids.push_back(mid); cols.weights.push_back(x1 - x0);
        if (x1 < 1 - eps) { cols.kids.emplace_back(); cols.weights.push_back(1 - x1); }
        root = std::move(cols);
    }
    normalize(root);
    return root;
}

} // namespace fmcg_boxes
