// test_boxes -- the box tree (src/fmcg_boxes): structure strings, splits
// (with same-direction flattening), removal (space and text to the
// neighbour), dividers and dragging, rects tiling the frame, from_rect.
#include "src/fmcg_boxes.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace fmcg_boxes;

static int fails = 0;
static void check(const std::string& what, bool ok) {
    std::printf("%-66s [%s]\n", what.c_str(), ok ? "OK" : "FAIL");
    if (!ok) ++fails;
}

static bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

// The leaves tile [0,1]^2 exactly: areas sum to 1 and no two overlap.
static bool tiles(const Node& root) {
    const std::vector<FRect> r = leaf_rects(root);
    double area = 0;
    for (size_t i = 0; i < r.size(); ++i) {
        if (!(r[i].x1 > r[i].x0 && r[i].y1 > r[i].y0)) return false;
        area += (r[i].x1 - r[i].x0) * (r[i].y1 - r[i].y0);
        for (size_t j = i + 1; j < r.size(); ++j)
            if (r[i].x0 < r[j].x1 - 1e-12 && r[j].x0 < r[i].x1 - 1e-12 && r[i].y0 < r[j].y1 - 1e-12 &&
                r[j].y0 < r[i].y1 - 1e-12)
                return false;
    }
    return near(area, 1.0);
}

int main() {
    {
        Node root;
        check("one box: the whole frame", leaf_count(root) == 1 && structure_string(root) == "B" && tiles(root));
        root.box.text = "keep";
        check("split into 3 columns", split_leaf(root, 0, kColumns, 3) && leaf_count(root) == 3 && tiles(root));
        check("the first part keeps the content", leaf_at(root, 0)->box.text == "keep" && leaf_at(root, 1)->box.text.empty());
        const std::vector<FRect> r = leaf_rects(root);
        check("thirds", near(r[1].x0, 1.0 / 3) && near(r[2].x0, 2.0 / 3) && r[2].x1 == 1.0);
        split_leaf(root, 1, kColumns, 2);   // same direction: joins the split
        check("same-direction split flattens", root.split == kColumns && root.kids.size() == 4 &&
                                                 near(root.weights[1], 1.0 / 6) && tiles(root));
        split_leaf(root, 3, kRows, 2);      // other direction: nests
        check("cross-direction split nests", root.kids.size() == 4 && root.kids[3].split == kRows && leaf_count(root) == 5);
        check("bad splits refused", !split_leaf(root, 9, kRows, 2) && !split_leaf(root, 0, kRows, 1) &&
                                       !split_leaf(root, 0, 7, 2));
    }
    {   // structure strings
        Node a;
        split_leaf(a, 0, kRows, 2);
        split_leaf(a, 0, kColumns, 3);
        a.weights = {0.25, 0.75};
        const std::string s = structure_string(a);
        Node b;
        check("structure round-trips", parse_structure(s, b) && structure_string(b) == s && leaf_count(b) == 4);
        check("structure text", s == "R(0.25:C(0.333333333:B,0.333333333:B,0.333333333:B),0.75:B)");
        Node c;
        check("whitespace ok, weights normalised", parse_structure(" C( 1 : B , 3 : B ) ", c) && near(c.weights[0], 0.25));
        Node d;
        check("nested same direction flattens on parse", parse_structure("C(1:C(1:B,1:B),1:B)", d) &&
                                                          d.kids.size() == 3 && near(d.weights[0], 0.25));
        Node e;
        bool bad = true;
        for (const char* t : {"", "X", "C()", "C(1:B", "C(0:B,1:B)", "C(-1:B,1:B)", "C(1B)", "BB", "C(1:B,)", "C(nan:B,1:B)"})
            bad &= !parse_structure(t, e);
        check("malformed structures rejected", bad);
        Node f;
        check("single-kid split collapses", parse_structure("R(1:B)", f) && f.split == kLeaf);
    }
    {   // removal
        Node root;
        split_leaf(root, 0, kColumns, 3);
        leaf_at(root, 0)->box.text = "A";
        leaf_at(root, 1)->box.text = "B";
        leaf_at(root, 2)->box.text = "C\n";
        int to = remove_leaf(root, 1);
        check("remove middle: its text goes with it, the box before it is next",
              to == 0 && leaf_at(root, 0)->box.text == "A" && leaf_at(root, 1)->box.text == "C\n");
        check("remove middle: space goes to the box before it", near(leaf_rects(root)[0].x1, 2.0 / 3) && tiles(root));
        to = remove_leaf(root, 0);
        check("remove first: the next box takes its number and its space",
              to == 0 && leaf_count(root) == 1 && root.split == kLeaf && root.box.text == "C\n");
        check("the only box cannot be removed", remove_leaf(root, 0) == -1);
        Node n;   // removal next to a nested split
        split_leaf(n, 0, kColumns, 2);
        split_leaf(n, 0, kRows, 2);         // leaves: 0 top-left, 1 bottom-left, 2 right
        leaf_at(n, 0)->box.text = "TL";
        leaf_at(n, 1)->box.text = "BL";
        leaf_at(n, 2)->box.text = "R";
        to = remove_leaf(n, 2);
        check("remove after a nested split: the neighbouring leaf is reported, texts unchanged",
              to == 1 && leaf_at(n, 0)->box.text == "TL" && leaf_at(n, 1)->box.text == "BL" && n.split == kRows && tiles(n));
    }
    {   // insert a box beside the selected one
        bool ok = true;
        const char* names[4] = {"left", "above", "right", "below"};
        for (int side = 0; side < 4; ++side) {
            Node root;
            split_leaf(root, 0, kColumns, 2);     // two halves; box 1 = right half
            leaf_at(root, 1)->box.text = "keep";
            const FRect before = leaf_rects(root)[1];
            const int added = insert_box(root, 1, side);
            const std::vector<FRect> r = leaf_rects(root);
            int kept = -1;
            for (int i = 0; i < leaf_count(root); ++i)
                if (leaf_at(root, i)->box.text == "keep") kept = i;
            bool good = added >= 0 && kept >= 0 && added != kept && leaf_count(root) == 3 && tiles(root) &&
                        leaf_at(root, added)->box.text.empty();
            if (good) {
                const FRect& a = r[added];
                const FRect& k = r[kept];
                const double mx = (before.x0 + before.x1) / 2, my = (before.y0 + before.y1) / 2;
                if (side == 0) good = near(a.x0, before.x0) && near(a.x1, mx) && near(k.x0, mx) && near(k.y1, before.y1);
                if (side == 2) good = near(k.x1, mx) && near(a.x0, mx) && near(a.x1, before.x1);
                if (side == 1) good = near(a.y0, before.y0) && near(a.y1, my) && near(k.y0, my) && near(k.x0, before.x0);
                if (side == 3) good = near(k.y1, my) && near(a.y0, my) && near(a.y1, before.y1);
                good &= near(r[0].x1, 0.5);    // the other box is untouched
            }
            if (!good) std::printf("  insert %s failed\n", names[side]);
            ok &= good;
        }
        check("insert_box: an empty box on each side, in half the box's space", ok);
        Node one;
        one.box.text = "x";
        check("insert_box: left of the only box", insert_box(one, 0, 0) == 0 && leaf_at(one, 1)->box.text == "x" &&
                                                    near(leaf_rects(one)[0].x1, 0.5));
        check("insert_box: bad input", insert_box(one, 5, 0) == -1 && insert_box(one, 0, 4) == -1);
    }
    {   // dividers and dragging
        Node root;
        split_leaf(root, 0, kRows, 2);
        split_leaf(root, 0, kColumns, 3);
        std::vector<Divider> d = dividers(root);
        check("dividers: 1 row line + 2 column lines", d.size() == 3);
        const Divider* col = nullptr;
        for (const Divider& x : d)
            if (x.dir == kColumns && x.index == 0) col = &x;
        check("column divider spans the top row only", col && near(col->pos, 1.0 / 3) && near(col->lo, 0) && near(col->hi, 0.5));
        Divider c0 = *col;
        check("drag to 0.25", move_divider(root, c0, 0.25) && near(leaf_rects(root)[0].x1, 0.25) &&
                                near(leaf_rects(root)[1].x0, 0.25) && near(leaf_rects(root)[1].x1, 2.0 / 3) && tiles(root));
        move_divider(root, c0, -5);
        check("drag clamps to the minimum part", near(leaf_rects(root)[0].x1, kMinPart) && tiles(root));
        move_divider(root, c0, 0.9);
        check("drag clamps against the neighbour", near(leaf_rects(root)[1].x1 - leaf_rects(root)[0].x1, kMinPart) && tiles(root));
        Divider row;
        for (const Divider& x : dividers(root))
            if (x.dir == kRows) row = x;
        move_divider(root, row, 0.2);
        check("row divider moves the whole top strip", near(leaf_rects(root)[2].y1, 0.2) && near(leaf_rects(root)[3].y0, 0.2));
        check("equal sizes", equalize_parent(root, 1) && near(leaf_rects(root)[1].x0, 1.0 / 3));
        // Edges of box 1 (top middle): left / right are column lines, bottom
        // is the row line, top is the frame.
        Divider e;
        check("edge_divider: frame edge has none", !edge_divider(root, 1, 1, e));
        check("edge_divider: left edge", edge_divider(root, 1, 0, e) && e.dir == kColumns && e.index == 0);
        check("edge_divider: bottom edge = the row line", edge_divider(root, 1, 3, e) && e.dir == kRows);
        Divider right;
        check("edge_divider: typed right edge moves it", edge_divider(root, 1, 2, right) &&
                                                          move_divider(root, right, 0.7) &&
                                                          near(leaf_rects(root)[1].x1, 0.7) && near(leaf_rects(root)[2].x0, 0.7) && tiles(root));
        check("edge_divider: bad input", !edge_divider(root, 9, 0, e) && !edge_divider(root, 0, 4, e));
        // The bottom box spans the whole width: its top edge is the row line.
        check("edge_divider: wide box below", edge_divider(root, 3, 1, e) && e.dir == kRows && !edge_divider(root, 3, 0, e));
    }
    {   // hit testing, from_rect, place list
        Node root;
        split_leaf(root, 0, kColumns, 2);
        check("point -> box", leaf_at_point(root, 0.2, 0.5) == 0 && leaf_at_point(root, 0.8, 0.1) == 1 &&
                                leaf_at_point(root, 1.5, 0.5) == -1);
        BoxContent c{"X", 3, 2};
        Node r = from_rect({0.1, 0.4, 1.0, 1.0}, c);
        int with = -1;
        for (int i = 0; i < leaf_count(r); ++i)
            if (leaf_at(r, i)->box == c) with = i;
        const std::vector<FRect> rr = leaf_rects(r);
        check("from_rect: the content box has the rect", with >= 0 && near(rr[with].x0, 0.1) && near(rr[with].y0, 0.4) &&
                                                           rr[with].x1 == 1.0 && rr[with].y1 == 1.0 && tiles(r));
        Node whole = from_rect({0, 0, 1, 1}, c);
        check("from_rect: whole frame = one box", whole.split == kLeaf && whole.box == c);
        Node mid = from_rect({0.25, 0.25, 0.75, 0.5}, c);
        check("from_rect: inner rect gives a 3 x 3-style tiling", leaf_count(mid) == 5 && tiles(mid));
        const std::vector<fmcg_text::BoxPlace> pl = place_list(mid);
        bool found = false;
        for (const auto& b : pl)
            found |= near(b.x0, 0.25) && near(b.x1, 0.75) && near(b.y0, 0.25) && near(b.y1, 0.5) && b.anchor == 3 && b.line_align == 2;
        check("place_list carries rects, anchors, line alignment", found && pl.size() == 5);
    }
    {   // random edit sequences keep a valid tiling and a parseable structure
        std::mt19937 rng(1);
        bool ok = true;
        for (int run = 0; run < 200 && ok; ++run) {
            Node root;
            for (int step = 0; step < 30 && ok; ++step) {
                const int n = leaf_count(root);
                const int op = (int)(rng() % 5), leaf = (int)(rng() % n);
                if (op == 0) split_leaf(root, leaf, 1 + (int)(rng() % 2), 2 + (int)(rng() % 3));
                else if (op == 1 && n > 1) remove_leaf(root, leaf);
                else if (op == 2) {
                    const std::vector<Divider> d = dividers(root);
                    if (!d.empty()) move_divider(root, d[rng() % d.size()], (rng() % 1000) / 1000.0);
                } else if (op == 3) equalize_parent(root, leaf);
                else insert_box(root, leaf, (int)(rng() % 4));
                Node back;
                ok = tiles(root) && parse_structure(structure_string(root), back) && leaf_count(back) == leaf_count(root);
                if (!ok) std::printf("  run %d step %d: %s\n", run, step, structure_string(root).c_str());
            }
        }
        check("200 random edit sequences: always a valid tiling, round-trips", ok);
    }
    std::printf("\n%s (%d failures)\n", fails ? "FAILURES" : "ALL OK", fails);
    return fails ? 1 : 0;
}
