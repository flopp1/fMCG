// fmcg_boxes -- the overlay's boxes: the frame is split into rows and columns
// (a tree of splits), each leaf box holds its own layout text and says where
// its text block sits inside it. Boxes never overlap and always tile the
// whole frame; sizes are fractions, so a layout keeps its proportions at any
// resolution. The renderer only sees the flattened list (place_list).
#pragma once
#include "fmcg_text.h"

#include <string>
#include <vector>

namespace fmcg_boxes {

// What a box shows and how.
struct BoxContent {
    std::string text;               // layout text: one overlay line per non-empty line
    int anchor = 7;                 // numpad point of the box the text block sits at
    int line_align = 0;             // 0 = follow the anchor's side, 1 left, 2 centre, 3 right
    bool operator==(const BoxContent& o) const {
        return text == o.text && anchor == o.anchor && line_align == o.line_align;
    }
};

enum Split { kLeaf = 0, kColumns = 1, kRows = 2 };   // columns: kids side by side

struct Node {
    int split = kLeaf;
    BoxContent box;                 // leaf only
    std::vector<double> weights;    // split only: each kid's share of this node (sum 1)
    std::vector<Node> kids;
    bool operator==(const Node& o) const {
        return split == o.split && box == o.box && weights == o.weights && kids == o.kids;
    }
    bool operator!=(const Node& o) const { return !(*this == o); }
};

struct FRect { double x0 = 0, y0 = 0, x1 = 1, y1 = 1; };   // fractions of the frame

// Leaves in reading order (depth first); a leaf's index is its box number.
int leaf_count(const Node& root);
Node* leaf_at(Node& root, int index);
const Node* leaf_at(const Node& root, int index);
std::vector<FRect> leaf_rects(const Node& root);
// The boxes as the renderer takes them (Placement::boxes), in leaf order.
std::vector<fmcg_text::BoxPlace> place_list(const Node& root);
// Box index at a point (fractions), -1 outside the frame.
int leaf_at_point(const Node& root, double x, double y);

// Structure only, e.g. "C(0.5:B,0.5:R(0.25:B,0.75:B))" (B = box, C = columns,
// R = rows, weights before ':'). Leaves of a parsed tree keep default content.
std::string structure_string(const Node& root);
bool parse_structure(const std::string& s, Node& out);

// Splits box `leaf` into n equal parts side by side (kColumns) or stacked
// (kRows). The first part keeps the content, the others start empty. Inside a
// split of the same direction the parts join it (no extra nesting).
bool split_leaf(Node& root, int leaf, int dir, int n);
// Adds an empty box beside box `leaf` (side: 0 left, 1 above, 2 right,
// 3 below), taking half of that box's space; the box keeps its content.
// Returns the new box's index, -1 on bad input.
int insert_box(Node& root, int leaf, int side);
// Removes box `leaf` and its content: its space goes to the neighbour
// before it (after it when it is the first). Returns the index of the
// neighbouring box next to it afterwards, -1 if it is the only box.
int remove_leaf(Node& root, int leaf);
// Gives every part of the split that directly holds `leaf` the same size.
bool equalize_parent(Node& root, int leaf);

// A movable line between two neighbouring parts of a split.
struct Divider {
    std::vector<int> path;          // kid indices from the root to the split node
    int index = 0;                  // between kids index and index + 1
    int dir = kColumns;             // kColumns: a vertical line at x = pos
    double pos = 0;                 // fraction of the frame (x or y)
    double lo = 0, hi = 1;          // the line's extent along the other axis
    double min_pos = 0, max_pos = 1;   // how far it can move
};
std::vector<Divider> dividers(const Node& root);
// The divider on one edge of box `leaf` (side: 0 left, 1 top, 2 right,
// 3 bottom); false on a frame edge.
bool edge_divider(const Node& root, int leaf, int side, Divider& out);
// Moves a divider to `pos` (clamped to its range); the two parts next to it
// change size, everything else stays.
bool move_divider(Node& root, const Divider& d, double pos);
// Smallest part a divider drag leaves, as a fraction of the frame.
constexpr double kMinPart = 0.02;

// A tree whose box `rect` holds `content` (empty boxes around it).
Node from_rect(const FRect& rect, const BoxContent& content);

// Flattens same-direction nesting, drops single-kid splits, renormalises
// weights. Every edit calls it; parse_structure too.
void normalize(Node& root);

} // namespace fmcg_boxes
