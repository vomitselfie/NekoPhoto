// The Affinity reader's colours and vector shapes (affinity_read.h): colour records, curve nodes and the parametric
// shapes as paths. Ported from Patchy with the rest (affinity.cpp).
#include "affinity_read.h"
#include "nekophoto/colour.h"
#include "nekophoto/png.h"
#include "nekophoto/render.h"
#include "nekophoto/vectormask.h"
#include "lcms2.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <tuple>
#include <unordered_map>
#include <zlib.h>

#ifdef NEKOPHOTO_HAVE_ZSTD
#include <zstd.h>
#endif

namespace nekophoto::affinity_read {

// ------------------------------------------------------------------------------------------------ colours, paths

/// An Affinity colour class (RGBA, HSLA or CMYK floats in `_col`) as straight RGBA 0..1.
std::optional<std::array<float, 4>> readColor(const Class* color) {
    if (!color) return std::nullopt;
    const affinity::Field* f = color->field(tag("_col"));
    const auto* data = f ? std::get_if<std::vector<uint8_t>>(&f->value) : nullptr;
    if (!data || data->size() < 16) return std::nullopt;
    const auto component = [&](size_t i) {
        const uint32_t bits = uint32_t((*data)[i * 4]) | uint32_t((*data)[i * 4 + 1]) << 8 | uint32_t((*data)[i * 4 + 2]) << 16 | uint32_t((*data)[i * 4 + 3]) << 24;
        float v;
        std::memcpy(&v, &bits, 4);
        return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f;
    };
    std::array<float, 4> c{component(0), component(1), component(2), component(3)};
    if (color->type == tag("CMYK") && data->size() >= 20)
        return std::array<float, 4>{(1 - c[0]) * (1 - c[3]), (1 - c[1]) * (1 - c[3]), (1 - c[2]) * (1 - c[3]), component(4)};
    if (color->type == tag("HSLA")) {
        const float hue = c[0] - std::floor(c[0]), s = c[1], l = c[2];
        const float chroma = (1 - std::abs(2 * l - 1)) * s, seg = hue * 6, x = chroma * (1 - std::abs(std::fmod(seg, 2.0f) - 1));
        float r = 0, g = 0, b = 0;
        switch (int(seg)) {
        case 0: r = chroma; g = x; break;
        case 1: r = x; g = chroma; break;
        case 2: g = chroma; b = x; break;
        case 3: g = x; b = chroma; break;
        case 4: r = x; b = chroma; break;
        default: r = chroma; b = x; break;
        }
        const float m = l - chroma / 2;
        c = {std::clamp(r + m, 0.0f, 1.0f), std::clamp(g + m, 0.0f, 1.0f), std::clamp(b + m, 0.0f, 1.0f), c[3]};
    }
    return c;
}

VectorPath::Knot knot(double ax, double ay, double ix, double iy, double ox, double oy) { return {ix, iy, ax, ay, ox, oy}; }
VectorPath::Knot corner(double x, double y) { return knot(x, y, x, y, x, y); }

const Class* firstClass(const Class& node, uint32_t t) {
    if (const Class* c = node.child(t)) return c;
    if (const ClassList* l = node.list(t); l && !l->empty()) return l->front().get();
    return nullptr;
}

/// A linked symbol instance keeps its geometry on the defining sibling in its SLnk > ILOb ring.
const Class* symbolSiblingWith(const Class& node, uint32_t field) {
    const Class* link = node.child(tag("SLnk"));
    const ClassList* members = link ? link->list(tag("ILOb")) : nullptr;
    if (!members) return nullptr;
    for (const auto& m : *members) if (m && m.get() != &node && m->child(field)) return m.get();
    return nullptr;
}

/// A PCrv's poly-curve (Crvs > Data, or a compound's baked 'crvs'): per subpath a closed flag and 18-byte records
/// (x, y as f64, u16 flags: 1 corner anchor, 2 smooth anchor, 0x100 the previous anchor's out handle, 0x200 the
/// next anchor's in handle). All subpaths fill even-odd together.
std::optional<VectorPath> curvePath(const Class& node) {
    const Class* curves = node.child(tag("Crvs"));
    if (!curves) curves = node.child(tag("crvs"));
    if (!curves) if (const Class* s = symbolSiblingWith(node, tag("Crvs"))) curves = s->child(tag("Crvs"));
    const Class* data = curves ? curves->child(tag("Data")) : nullptr;
    if (!data) return std::nullopt;
    const auto f64 = [](const uint8_t* b) { uint64_t bits = 0; for (int i = 7; i >= 0; i--) bits = bits << 8 | b[i]; double v; std::memcpy(&v, &bits, 8); return v; };
    VectorPath path;
    bool closed = true;
    for (const auto& field : data->fields) {
        if (const auto* flag = std::get_if<bool>(&field.value)) { closed = *flag; continue; }
        const auto* records = std::get_if<affinity::CurveArray>(&field.value);
        if (!records || records->recordSize < 18) continue;
        VectorPath::Subpath sub;
        sub.closed = closed;
        bool pendingIn = false;
        double inX = 0, inY = 0;
        for (size_t at = 0; at + records->recordSize <= records->bytes.size(); at += records->recordSize) {
            const uint8_t* b = records->bytes.data() + at;
            const double x = f64(b), y = f64(b + 8);
            if (!std::isfinite(x) || !std::isfinite(y)) continue;
            const uint16_t flags = uint16_t(b[16] | b[17] << 8);
            if (flags & 0x0003u) {
                auto k = corner(x, y);
                if (pendingIn) { k.inX = inX; k.inY = inY; pendingIn = false; }
                sub.knots.push_back(k);
            } else if (flags & 0x0100u) {
                if (!sub.knots.empty()) { sub.knots.back().outX = x; sub.knots.back().outY = y; }
            } else if (flags & 0x0200u) {
                pendingIn = true; inX = x; inY = y;
            }
        }
        if (sub.closed && sub.knots.size() >= 2) {
            const auto& first = sub.knots.front();
            const auto& last = sub.knots.back();
            if (std::abs(first.x - last.x) < 1e-4 && std::abs(first.y - last.y) < 1e-4) {
                sub.knots.front().inX = last.inX;
                sub.knots.front().inY = last.inY;
                sub.knots.pop_back();
            }
        }
        if (sub.knots.size() >= 2) path.subpaths.push_back(std::move(sub));
        closed = true;
    }
    if (path.subpaths.empty()) return std::nullopt;
    return path;
}

constexpr double quarterArc = 0.5522847498307936;
enum { cornerRound = 0, cornerStraight = 1, cornerRoundInverse = 2, cornerCutout = 3, cornerNone = 4 };

// The parametric shapes below are Patchy's parametric_shape_path, kept close to its text (and its anchor type) so
// updates stay a comparison: an anchor with its in and out handles, and a subpath of them.
struct PA { double anchor_x, anchor_y, in_x, in_y, out_x, out_y; bool smooth; };
struct Sub { std::vector<PA> anchors; bool closed = true; };

VectorPath toPath(const std::vector<Sub>& subs) {
    VectorPath path;
    for (const Sub& s : subs) {
        VectorPath::Subpath out;
        out.closed = s.closed;
        for (const PA& a : s.anchors) out.knots.push_back(knot(a.anchor_x, a.anchor_y, a.in_x, a.in_y, a.out_x, a.out_y));
        path.subpaths.push_back(std::move(out));
    }
    return path;
}

int64_t enumId(const Class& cls, uint32_t t, int64_t fallback) {
    const affinity::Field* f = cls.field(t);
    if (const auto* e = f ? std::get_if<affinity::Enum>(&f->value) : nullptr) return e->id;
    return fallback;
}

// One rectangle corner, walking clockwise: `enter` is the direction along the incoming edge, `exit` the outgoing,
// `radius` the corner size (0: sharp), `type` the wire corner type.
void appendRectCornerPA(Sub& subpath, double px, double py, double enter_x, double enter_y, double exit_x, double exit_y, double radius, int type) {
    const auto add = [&](double x, double y, bool smooth) -> PA& { subpath.anchors.push_back(PA{x, y, x, y, x, y, smooth}); return subpath.anchors.back(); };
    if (radius <= 0.0 || type == cornerNone) { add(px, py, false); return; }
    const double in_x = px - enter_x * radius, in_y = py - enter_y * radius, out_x = px + exit_x * radius, out_y = py + exit_y * radius;
    const double k = quarterArc * radius;
    switch (type) {
    case cornerRound: {
        PA& a = add(in_x, in_y, true); a.out_x = in_x + enter_x * k; a.out_y = in_y + enter_y * k;
        PA& b = add(out_x, out_y, true); b.in_x = out_x - exit_x * k; b.in_y = out_y - exit_y * k;
        break;
    }
    case cornerRoundInverse: {
        PA& a = add(in_x, in_y, false); a.out_x = in_x + exit_x * k; a.out_y = in_y + exit_y * k;
        PA& b = add(out_x, out_y, false); b.in_x = out_x - enter_x * k; b.in_y = out_y - enter_y * k;
        break;
    }
    case cornerCutout:
        add(in_x, in_y, false);
        add(in_x + out_x - px, in_y + out_y - py, false);
        add(out_x, out_y, false);
        break;
    default:
        add(in_x, in_y, false);
        add(out_x, out_y, false);
        break;
    }
}

// Wire semantics pinned by Patchy's one-toggle probes and its shape-curves sweep: the record's class tag is the
// kind; ShpB [x0, y0, x1, y1] is the local box; radial kinds work in the box-inscribed ellipse's unit space.
std::optional<VectorPath> shapePath(const Class& node, std::string* why) {
  // A linked symbol instance keeps its geometry (Shpe record AND local box)
  // on the defining sibling; the instance's own transform still places it.
  const Class* geometry_node = &node;
  if (node.child(tag("Shpe")) == nullptr) {
    if (const Class* sibling = symbolSiblingWith(node, tag("Shpe"))) {
      geometry_node = sibling;
    }
  }
  const Class* shape = geometry_node->child(tag("Shpe"));
  if (shape == nullptr) {
    *why = "has no shape record";
    return std::nullopt;
  }
  const auto box = geometry_node->vector(tag("ShpB"));
  if (box.size() != 4 || !(box[2] > box[0]) || !(box[3] > box[1])) {
    *why = "has a degenerate shape box";
    return std::nullopt;
  }
  const double x0 = box[0];
  const double y0 = box[1];
  const double x1 = box[2];
  const double y1 = box[3];
  const double w = x1 - x0;
  const double h = y1 - y0;

  std::vector<Sub> subs;
  Sub subpath;
  subpath.closed = true;

  // Shared helpers for the long-tail kinds: unit space is the box-inscribed
  // ellipse's coordinate system (centre (0,0), radius 1, y down); mapping to
  // the document is an anisotropic scale, which keeps beziers beziers.
  constexpr double kPiSh = 3.14159265358979323846;
  constexpr double kCircleK = 0.5522847498307934;
  const double ucx = (x0 + x1) / 2.0;
  const double ucy = (y0 + y1) / 2.0;
  const double hw = w / 2.0;
  const double hh = h / 2.0;
  const auto unit_x = [&](double ux) { return ucx + hw * ux; };
  const auto unit_y = [&](double uy) { return ucy + hh * uy; };
  const auto push_corner = [](Sub& sp, double px, double py) {
    sp.anchors.push_back(PA{px, py, px, py, px, py, false});
  };
  const auto push_corner_unit = [&](Sub& sp, double ux, double uy) {
    push_corner(sp, unit_x(ux), unit_y(uy));
  };
  // Append a circular arc (unit space: centre (ax,ay), radius r) from angle
  // a0 to a1, either direction, split into <=90-degree bezier segments. When
  // the subpath already ends at the arc's start point only its out handle is
  // set (so lines and arcs chain); interior anchors are smooth, the arc's
  // endpoints stay corners for the caller to join.
  const auto append_unit_arc = [&](Sub& sp, double ax, double ay, double r, double a0,
                                   double a1) {
    const int segments =
        std::max(1, static_cast<int>(std::ceil(std::abs(a1 - a0) / (kPiSh / 3.0) - 1e-9)));
    const double step = (a1 - a0) / segments;
    const double k = 4.0 / 3.0 * std::tan(step / 4.0) * r;
    for (int i = 0; i <= segments; ++i) {
      const double ang = a0 + step * static_cast<double>(i);
      const double px = unit_x(ax + r * std::cos(ang));
      const double py = unit_y(ay + r * std::sin(ang));
      const double tx = -std::sin(ang) * k * hw;
      const double ty = std::cos(ang) * k * hh;
      if (i == 0) {
        if (!sp.anchors.empty() && std::abs(sp.anchors.back().anchor_x - px) < 1e-6 &&
            std::abs(sp.anchors.back().anchor_y - py) < 1e-6) {
          sp.anchors.back().out_x = px + tx;
          sp.anchors.back().out_y = py + ty;
        } else {
          sp.anchors.push_back(PA{px, py, px, py, px + tx, py + ty, false});
        }
        continue;
      }
      PA anchor{px, py, px - tx, py - ty, px, py, i < segments};
      if (i < segments) {
        anchor.out_x = px + tx;
        anchor.out_y = py + ty;
      }
      sp.anchors.push_back(anchor);
    }
  };
  // A closed subpath built from chained arcs ends where it began; fold the
  // duplicate final anchor's incoming handle onto the first anchor.
  const auto close_fold = [](Sub& sp) {
    if (sp.anchors.size() >= 2) {
      const auto& first = sp.anchors.front();
      const auto& last = sp.anchors.back();
      if (std::abs(first.anchor_x - last.anchor_x) < 1e-6 &&
          std::abs(first.anchor_y - last.anchor_y) < 1e-6) {
        sp.anchors.front().in_x = last.in_x;
        sp.anchors.front().in_y = last.in_y;
        sp.anchors.pop_back();
      }
    }
  };

  if (shape->type == tag("ShNR")) {
    auto radii = shape->vector(tag("ShCR"));
    radii.resize(4, 0.0);
    auto types_raw = shape->vector(tag("CTyp"));
    types_raw.resize(4, static_cast<double>(cornerNone));
    std::array<int, 4> types{};
    for (std::size_t i = 0; i < 4; ++i) {
      types[i] = static_cast<int>(types_raw[i]);
    }
    if (shape->boolean(tag("Lock"), true)) {
      // Single-radius mode: corner 0 defines every corner.
      radii[1] = radii[2] = radii[3] = radii[0];
      types[1] = types[2] = types[3] = types[0];
    }
    const bool absolute = shape->boolean(tag("AbSz"), false);
    const double reference = std::min(w, h);
    std::array<double, 4> r{};  // TL, TR, BR, BL in local pixels
    for (std::size_t i = 0; i < 4; ++i) {
      const double value = absolute ? radii[i] : radii[i] * reference;
      r[i] = (types[i] == cornerNone) ? 0.0 : std::max(0.0, value);
    }
    // Adjacent radii cannot exceed their shared edge (the CSS overlap rule;
    // Affinity's exact overflow behavior is unprobed).
    double scale = 1.0;
    const auto limit = [&](double a, double b, double edge) {
      if (a + b > edge && a + b > 0.0) {
        scale = std::min(scale, edge / (a + b));
      }
    };
    limit(r[0], r[1], w);
    limit(r[1], r[2], h);
    limit(r[2], r[3], w);
    limit(r[3], r[0], h);
    for (double& value : r) {
      value *= scale;
    }
    appendRectCornerPA(subpath, x0, y0, 0.0, -1.0, 1.0, 0.0, r[0], types[0]);
    appendRectCornerPA(subpath, x1, y0, 1.0, 0.0, 0.0, 1.0, r[1], types[1]);
    appendRectCornerPA(subpath, x1, y1, 0.0, 1.0, -1.0, 0.0, r[2], types[2]);
    appendRectCornerPA(subpath, x0, y1, -1.0, 0.0, 0.0, -1.0, r[3], types[3]);
  } else if (shape->type == tag("ShpE")) {
    const double cx = (x0 + x1) / 2.0;
    const double cy = (y0 + y1) / 2.0;
    const double kx = quarterArc * (w / 2.0);
    const double ky = quarterArc * (h / 2.0);
    const auto add = [&](double ax, double ay, double ix, double iy, double ox, double oy) {
      subpath.anchors.push_back(PA{ax, ay, ix, iy, ox, oy, true});
    };
    add(cx, y0, cx - kx, y0, cx + kx, y0);
    add(x1, cy, x1, cy - ky, x1, cy + ky);
    add(cx, y1, cx + kx, y1, cx - kx, y1);
    add(x0, cy, x0, cy + ky, x0, cy - ky);
  } else if (shape->type == tag("ShPy")) {
    for (const auto& field : shape->fields) {
      if (field.tag != tag("Side") && field.tag != tag("Smth") &&
          field.tag != tag("Curv")) {
        *why = "is a non-regular Affinity polygon";
        return std::nullopt;
      }
    }
    const auto sides = std::clamp<std::int64_t>(shape->integer(tag("Side"), 5), 3, 256);
    const bool smooth = shape->boolean(tag("Smth"), false);
    const double cx = (x0 + x1) / 2.0;
    const double cy = (y0 + y1) / 2.0;
    constexpr double kPi = 3.14159265358979323846;
    // Smoothed polygons replace every corner with a symmetric smooth anchor
    // whose tangent length scales with Curv: the shp-polygon6-smooth probe
    // pins Smth=true/Curv=0 as rendering EXACTLY like the plain polygon, so
    // Curv is the rounding amount. Curv=1 is mapped to the circle through
    // the vertices ((4/3)tan(pi/2n) in unit-circle space) - plausible but
    // unpinned, no Curv>0 ground truth exists yet.
    const double curv = std::clamp(shape->number(tag("Curv"), 0.0), 0.0, 1.0);
    const double tangent =
        smooth ? curv * (4.0 / 3.0) * std::tan(kPi / (2.0 * static_cast<double>(sides))) : 0.0;
    for (std::int64_t i = 0; i < sides; ++i) {
      const double angle = -kPi / 2.0 + 2.0 * kPi * static_cast<double>(i) /
                                            static_cast<double>(sides);
      const double ux = std::cos(angle);
      const double uy = std::sin(angle);
      const double x = cx + (w / 2.0) * ux;
      const double y = cy + (h / 2.0) * uy;
      if (!smooth) {
        subpath.anchors.push_back(PA{x, y, x, y, x, y, false});
        continue;
      }
      // The tangent runs perpendicular to the radius (unit space), scaled per
      // axis by the box's half sizes.
      const double tx = -uy * tangent * (w / 2.0);
      const double ty = ux * tangent * (h / 2.0);
      subpath.anchors.push_back(PA{x, y, x - tx, y - ty, x + tx, y + ty, true});
    }
  } else if (shape->type == tag("ShSt")) {
    // Star: Pnts outer vertices on the box-inscribed ellipse (first vertex
    // up, like ShPy), alternating with inner vertices at the IRad fraction,
    // offset by half a step. CrvL/CrvR only take effect when Lgcy
    // (curvedEdges) is true - the shape-curves sweep pins both a plain-star
    // conversion for Lgcy alone or CrvL/CrvR alone, and tangential tip
    // handles for the combination (star-lgcy-crv, CrvL=CrvR=0.4 -> unit
    // handle 0.18665). Circle-rounded stars (CrcI/CrcO) keep the
    // placeholder until their tangent construction is pinned.
    if (shape->number(tag("CrcI"), 0.0) > 1e-6 ||
        shape->number(tag("CrcO"), 0.0) > 1e-6) {
      *why = "is a circle-rounded Affinity star";
      return std::nullopt;
    }
    const auto points = std::clamp<std::int64_t>(shape->integer(tag("Pnts"), 5), 3, 256);
    const double inner = std::clamp(shape->number(tag("IRad"), 0.5), 0.0, 1.0);
    const bool curved = shape->boolean(tag("Lgcy"), false);
    const double curve_left =
        curved ? std::clamp(shape->number(tag("CrvL"), 0.0), 0.0, 1.0) : 0.0;
    const double curve_right =
        curved ? std::clamp(shape->number(tag("CrvR"), 0.0), 0.0, 1.0) : 0.0;
    // The pinned 0.4 -> 0.18665 sample gives the per-unit-curve handle scale
    // at five points; scale other point counts by their half-step tangent.
    const double handle_scale = 0.46662 * std::tan(kPiSh / static_cast<double>(points)) /
                                std::tan(kPiSh / 5.0);
    const double cx = (x0 + x1) / 2.0;
    const double cy = (y0 + y1) / 2.0;
    constexpr double kPi = 3.14159265358979323846;
    for (std::int64_t i = 0; i < points; ++i) {
      const double outer_angle = -kPi / 2.0 + 2.0 * kPi * static_cast<double>(i) /
                                                  static_cast<double>(points);
      const double inner_angle = outer_angle + kPi / static_cast<double>(points);
      const double ox = cx + (w / 2.0) * std::cos(outer_angle);
      const double oy = cy + (h / 2.0) * std::sin(outer_angle);
      if (curve_left > 0.0 || curve_right > 0.0) {
        // Tangential handles round the tip; CrvL feeds the incoming side.
        const double tx = -std::sin(outer_angle) * hw;
        const double ty = std::cos(outer_angle) * hh;
        subpath.anchors.push_back(PA{
            ox, oy, ox - tx * curve_left * handle_scale, oy - ty * curve_left * handle_scale,
            ox + tx * curve_right * handle_scale, oy + ty * curve_right * handle_scale, false});
      } else {
        subpath.anchors.push_back(PA{ox, oy, ox, oy, ox, oy, false});
      }
      const double ix = cx + inner * (w / 2.0) * std::cos(inner_angle);
      const double iy = cy + inner * (h / 2.0) * std::sin(inner_angle);
      subpath.anchors.push_back(PA{ix, iy, ix, iy, ix, iy, false});
    }
  } else if (shape->type == tag("ShpD")) {
    // Diamond: side vertices sit Pos of the way up the box (default 0.5).
    const double pos = std::clamp(shape->number(tag("Pos "), 0.5), 0.0, 1.0);
    const double side_y = y1 - pos * h;
    push_corner(subpath, ucx, y0);
    push_corner(subpath, x1, side_y);
    push_corner(subpath, ucx, y1);
    push_corner(subpath, x0, side_y);
  } else if (shape->type == tag("ShTz")) {
    // Trapezoid: the top edge runs from PosL to PosR across the box.
    const double pos_l = std::clamp(shape->number(tag("PosL"), 0.25), 0.0, 1.0);
    const double pos_r = std::clamp(shape->number(tag("PosR"), 0.75), 0.0, 1.0);
    push_corner(subpath, x1, y1);
    push_corner(subpath, x0, y1);
    push_corner(subpath, x0 + pos_l * w, y0);
    push_corner(subpath, x0 + pos_r * w, y0);
  } else if (shape->type == tag("ShHt")) {
    // Heart: a fixed six-anchor template in box fractions; only the top
    // cleft anchor moves - its y is Sprd of the way down the box.
    const double spread = std::clamp(shape->number(tag("Sprd"), 0.2), 0.0, 1.0);
    struct HeartAnchor {
      double ax, ay, inx, iny, outx, outy;
      bool smooth;
    };
    const HeartAnchor kHeart[6] = {
        {0.5, 0.0, 0.3947370, 0.0, 0.6052630, 0.0, false},
        {0.9210530, 0.1, 0.8157890, 0.0, 1.0263160, 0.2, true},
        {0.9210530, 0.6, 1.0263160, 0.4, 0.8473680, 0.75, true},
        {0.5, 1.0, 0.6578950, 0.9, 0.3421050, 0.9, false},
        {0.0789474, 0.6, 0.1526320, 0.75, -0.0263158, 0.4, true},
        {0.0789474, 0.1, -0.0263158, 0.2, 0.1842110, 0.0, true},
    };
    for (const auto& a : kHeart) {
      const double ay = a.ax == 0.5 && a.ay == 0.0 ? spread : a.ay;
      subpath.anchors.push_back(PA{x0 + a.ax * w, y0 + ay * h, x0 + a.inx * w,
                                           y0 + a.iny * h, x0 + a.outx * w, y0 + a.outy * h,
                                           a.smooth});
    }
  } else if (shape->type == tag("ShTr")) {
    // Tear: an ellipse whose top anchor collapses into a corner at Tail
    // across the top edge; Curv scales the upper side handles (default 0.3),
    // Bend curls the tail tip (handle template pinned at Tail 0.5). The Fixd
    // and Ball fields do not alter 3.x geometry (sweep-pinned) and are
    // ignored.
    const double tail = std::clamp(shape->number(tag("Tail"), 0.5), 0.0, 1.0);
    const double curve = std::clamp(shape->number(tag("Curv"), 0.3), 0.0, 1.0);
    const double bend = std::clamp(shape->number(tag("Bend"), 0.0), -1.0, 1.0);
    const double tip_x = x0 + tail * w;
    // The bottom bulb is a half-ellipse whose vertical radius caps at half
    // the WIDTH (the tall-box probe pins side anchors at y1 - w/2 there);
    // the side anchors' upper handles are Curv of the way back up to the top.
    const double bulb = std::min(hw, hh);
    const double side_y = y1 - bulb;
    subpath.anchors.push_back(PA{tip_x, y0, tip_x + bend * w * 0.1,
                                         y0 + bend * h * 0.25, tip_x + bend * w * 0.25, y0,
                                         false});
    subpath.anchors.push_back(PA{x1, side_y, x1, side_y - curve * (side_y - y0), x1,
                                         side_y + kCircleK * bulb, true});
    subpath.anchors.push_back(PA{ucx, y1, ucx + kCircleK * hw, y1, ucx - kCircleK * hw,
                                         y1, true});
    subpath.anchors.push_back(PA{x0, side_y, x0, side_y + kCircleK * bulb, x0,
                                         side_y - curve * (side_y - y0), true});
  } else if (shape->type == tag("ShDA")) {
    // Arrow: a straight-line polygon. Head length is LPr1/RPr1 of the BOX
    // HEIGHT (wide-box pinned), the shaft is Thck of the height, and
    // LPr2/RPr2 offset the shaft junction from the barb base. Only end
    // styles 0 (flat) and 1 (the plain arrowhead) are modeled.
    const auto left_style = enumId(*shape, tag("LSty"), 1);
    const auto right_style = enumId(*shape, tag("RSty"), 1);
    if (left_style > 1 || right_style > 1) {
      *why = "uses an Affinity arrow end style NekoPhoto does not read yet";
      return std::nullopt;
    }
    const double thick = std::clamp(shape->number(tag("Thck"), 0.35), 0.0, 1.0);
    const double shaft_top = ucy - thick * hh;
    const double shaft_bottom = ucy + thick * hh;
    double left_len = std::max(0.0, shape->number(tag("LPr1"), 0.5)) * h;
    const double left_inner = shape->number(tag("LPr2"), 0.0) * h;
    double right_len = std::max(0.0, shape->number(tag("RPr1"), 0.5)) * h;
    const double right_inner = shape->number(tag("RPr2"), 0.0) * h;
    // Heads that would overlap scale down proportionally to meet (the
    // tall-box probe: two 30px heads in a 44px box become 22px each).
    const double head_total = (left_style == 1 ? left_len : 0.0) +
                              (right_style == 1 ? right_len : 0.0);
    if (head_total > w && head_total > 0.0) {
      left_len *= w / head_total;
      right_len *= w / head_total;
    }
    const double barb_l = x0 + left_len;
    const double shaft_l = left_style == 1 ? barb_l + left_inner : x0;
    const double barb_r = x1 - right_len;
    const double shaft_r = right_style == 1 ? barb_r - right_inner : x1;
    if (left_style == 1) {
      push_corner(subpath, shaft_l, shaft_bottom);
      push_corner(subpath, barb_l, y1);
      push_corner(subpath, x0, ucy);
      push_corner(subpath, barb_l, y0);
      push_corner(subpath, shaft_l, shaft_top);
    } else {
      push_corner(subpath, x0, shaft_bottom);
      push_corner(subpath, x0, shaft_top);
    }
    if (right_style == 1) {
      push_corner(subpath, shaft_r, shaft_top);
      push_corner(subpath, barb_r, y0);
      push_corner(subpath, x1, ucy);
      push_corner(subpath, barb_r, y1);
      push_corner(subpath, shaft_r, shaft_bottom);
    } else {
      push_corner(subpath, x1, shaft_top);
      push_corner(subpath, x1, shaft_bottom);
    }
  } else if (shape->type == tag("ShPi")) {
    // Pie: the wire angles are radians in y-UP math orientation (AngS
    // default pi/2 = top, AngE default 0 = right); the filled sector runs
    // from AngE to AngS clockwise on screen. IRad > 0 cuts an annulus.
    const double phi_s = -shape->number(tag("AngS"), kPiSh / 2.0);
    double phi_e = -shape->number(tag("AngE"), 0.0);
    const double inner = std::clamp(shape->number(tag("IRad"), 0.0), 0.0, 1.0);
    double sweep = phi_s - phi_e;
    sweep -= std::floor(sweep / (2.0 * kPiSh)) * 2.0 * kPiSh;  // wrap into (0, 2*pi]
    if (sweep < 1e-9) {
      sweep = 2.0 * kPiSh;
    }
    const double phi_end = phi_e + sweep;  // == phi_s modulo 2*pi
    if (inner <= 1e-6) {
      push_corner_unit(subpath, std::cos(phi_end), std::sin(phi_end));
      push_corner_unit(subpath, 0.0, 0.0);
      append_unit_arc(subpath, 0.0, 0.0, 1.0, phi_e, phi_end);
    } else {
      push_corner_unit(subpath, std::cos(phi_end), std::sin(phi_end));
      append_unit_arc(subpath, 0.0, 0.0, inner, phi_end, phi_e);
      append_unit_arc(subpath, 0.0, 0.0, 1.0, phi_e, phi_end);
    }
    close_fold(subpath);
  } else if (shape->type == tag("ShSg")) {
    // Segment: the box ellipse clipped to the band between two chords
    // perpendicular to the Angl direction (unit space): Pos0/Pos1 map to
    // signed offsets 2*Pos-1 along the direction.
    const double angle = shape->number(tag("Angl"), kPiSh / 2.0);
    double c0 = std::clamp(2.0 * shape->number(tag("Pos0"), 0.25) - 1.0, -1.0, 1.0);
    double c1 = std::clamp(2.0 * shape->number(tag("Pos1"), 1.0) - 1.0, -1.0, 1.0);
    if (c0 > c1) {
      std::swap(c0, c1);
    }
    const double alpha0 = std::acos(std::clamp(c0, -1.0, 1.0));
    const double alpha1 = std::acos(std::clamp(c1, -1.0, 1.0));
    if (alpha0 - alpha1 < 1e-6) {
      *why = "has a degenerate shape outline";
      return std::nullopt;
    }
    const double delta = -angle;  // screen angle of the cut direction
    push_corner_unit(subpath, std::cos(delta + alpha0), std::sin(delta + alpha0));
    push_corner_unit(subpath, std::cos(delta - alpha0), std::sin(delta - alpha0));
    if (alpha1 > 1e-6) {
      append_unit_arc(subpath, 0.0, 0.0, 1.0, delta - alpha0, delta - alpha1);
      push_corner_unit(subpath, std::cos(delta + alpha1), std::sin(delta + alpha1));
      append_unit_arc(subpath, 0.0, 0.0, 1.0, delta + alpha1, delta + alpha0);
    } else {
      append_unit_arc(subpath, 0.0, 0.0, 1.0, delta - alpha0, delta + alpha0);
    }
    close_fold(subpath);
  } else if (shape->type == tag("ShCr")) {
    // Crescent: the region between two boundary curves (unit space), each
    // from (0,-1) through (Arc, 0) to (0,1) - ArcL/ArcR are the signed bulge
    // fractions (the JS setters negate leftArc on the wire; the wire values
    // place both boundaries directly). Affinity's boundary is two cubics
    // whose handles the shape-curves sweep pins exactly across four bulges:
    // the endpoint handle is (K*m, (1-|m|)/3) and the mid handle is vertical
    // with length K*|m| + (1-|m|)/3 - the ellipse half at |m|=1, the
    // straight line at m=0, blended linearly between.
    const double arc_l = std::clamp(shape->number(tag("ArcL"), -1.0), -1.0, 1.0);
    const double arc_r = std::clamp(shape->number(tag("ArcR"), -0.3), -1.0, 1.0);
    if (std::abs(arc_r - arc_l) < 1e-6) {
      *why = "has a degenerate shape outline";
      return std::nullopt;
    }
    // Emits the boundary for bulge m from (0,-1) to (0,1) when down is true,
    // or the reverse; the subpath's last anchor is the start point.
    const auto boundary = [&](double m, bool down) {
      const double s = down ? 1.0 : -1.0;
      if (std::abs(m) < 1e-6) {
        push_corner_unit(subpath, 0.0, s);
        return;
      }
      const double end_off = (1.0 - std::abs(m)) / 3.0;
      const double mid_len = kCircleK * std::abs(m) + end_off;
      auto& start = subpath.anchors.back();
      start.out_x = unit_x(kCircleK * m);
      start.out_y = unit_y(-s + s * end_off);
      subpath.anchors.push_back(PA{unit_x(m), unit_y(0.0), unit_x(m),
                                           unit_y(-s * mid_len), unit_x(m), unit_y(s * mid_len),
                                           true});
      subpath.anchors.push_back(PA{unit_x(0.0), unit_y(s), unit_x(kCircleK * m),
                                           unit_y(s - s * end_off), unit_x(0.0), unit_y(s),
                                           false});
    };
    push_corner_unit(subpath, 0.0, -1.0);
    boundary(arc_r, true);
    boundary(arc_l, false);
    close_fold(subpath);
  } else if (shape->type == tag("ShDS")) {
    // Double star: 4*Pnts straight-line vertices at uniform angle steps from
    // the top, radii cycling [1, IRad, PRad, IRad].
    const auto points = std::clamp<std::int64_t>(shape->integer(tag("Pnts"), 5), 2, 128);
    const double inner = std::clamp(shape->number(tag("IRad"), 0.525731), 0.0, 1.0);
    const double point_r = std::clamp(shape->number(tag("PRad"), 0.809017), 0.0, 1.0);
    const double radii[4] = {1.0, inner, point_r, inner};
    const std::int64_t count = points * 4;
    for (std::int64_t i = 0; i < count; ++i) {
      const double ang = -kPiSh / 2.0 +
                         2.0 * kPiSh * static_cast<double>(i) / static_cast<double>(count);
      const double r = radii[i % 4];
      push_corner_unit(subpath, r * std::cos(ang), r * std::sin(ang));
    }
  } else if (shape->type == tag("ShSS")) {
    // Square star: Side arms whose tips are flattened; per arm the two outer
    // corners sit at (cos h, +/- (1-COut)*sin h) in tip-local unit space
    // (h = pi/Side), with the inner corner between arms at radius 1-COut.
    const auto sides = std::clamp<std::int64_t>(shape->integer(tag("Side"), 5), 3, 128);
    const double cutout = std::clamp(shape->number(tag("COut"), 0.5), 0.0, 1.0);
    const double inner_r = 1.0 - cutout;
    const double half = kPiSh / static_cast<double>(sides);
    for (std::int64_t k = 0; k < sides; ++k) {
      const double tip = -kPiSh / 2.0 + half +
                         2.0 * kPiSh * static_cast<double>(k) / static_cast<double>(sides);
      const double ct = std::cos(tip);
      const double st = std::sin(tip);
      const double px = std::cos(half);
      const double py = inner_r * std::sin(half);
      push_corner_unit(subpath, ct * px - st * -py, st * px + ct * -py);
      push_corner_unit(subpath, ct * px - st * py, st * px + ct * py);
      push_corner_unit(subpath, inner_r * std::cos(tip + half), inner_r * std::sin(tip + half));
    }
  } else if (shape->type == tag("ShCg")) {
    // Cog: alternating tooth-top arcs (radius 1, width TtSz of the tooth
    // period) and root arcs (radius IRad, width NtSz), joined by straight
    // flanks; Curv bows the flanks (handles at 2/3*Curv along the chord,
    // approximate); Hole cuts a centre ellipse.
    const auto teeth = std::clamp<std::int64_t>(shape->integer(tag("Teth"), 12), 3, 256);
    const double root_r = std::clamp(shape->number(tag("IRad"), 0.85), 0.0, 1.0);
    const double hole = std::clamp(shape->number(tag("Hole"), 0.2), 0.0, 1.0);
    const double tooth_size = std::clamp(shape->number(tag("TtSz"), 0.37), 0.0, 1.0);
    const double notch_size = std::clamp(shape->number(tag("NtSz"), 0.42), 0.0, 1.0);
    const double curve = std::clamp(shape->number(tag("Curv"), 0.0), 0.0, 1.0);
    const double period = 2.0 * kPiSh / static_cast<double>(teeth);
    const double half_top = tooth_size * period / 2.0;
    const double half_notch = notch_size * period / 2.0;
    for (std::int64_t k = 0; k < teeth; ++k) {
      const double centre = -kPiSh / 2.0 + period * static_cast<double>(k);
      append_unit_arc(subpath, 0.0, 0.0, 1.0, centre - half_top, centre + half_top);
      append_unit_arc(subpath, 0.0, 0.0, root_r, centre + period / 2.0 - half_notch,
                      centre + period / 2.0 + half_notch);
    }
    close_fold(subpath);
    if (curve > 0.0) {
      // Bow every straight flank: a flank runs between an arc-end anchor
      // (degenerate out handle) and the next arc-start anchor.
      const std::size_t n = subpath.anchors.size();
      for (std::size_t i = 0; i < n; ++i) {
        auto& a = subpath.anchors[i];
        auto& b = subpath.anchors[(i + 1) % n];
        const bool a_flat = a.out_x == a.anchor_x && a.out_y == a.anchor_y;
        const bool b_flat = b.in_x == b.anchor_x && b.in_y == b.anchor_y;
        if (a_flat && b_flat) {
          const double dx = (b.anchor_x - a.anchor_x) * (2.0 / 3.0) * curve;
          const double dy = (b.anchor_y - a.anchor_y) * (2.0 / 3.0) * curve;
          a.out_x = a.anchor_x + dx;
          a.out_y = a.anchor_y + dy;
          b.in_x = b.anchor_x - dx;
          b.in_y = b.anchor_y - dy;
        }
      }
    }
    if (subpath.anchors.size() < 2) {
      *why = "has a degenerate shape outline";
      return std::nullopt;
    }
    subs.push_back(std::move(subpath));
    if (hole > 1e-6) {
      Sub hole_path;
      hole_path.closed = true;
      append_unit_arc(hole_path, 0.0, 0.0, hole, -kPiSh / 2.0, 3.0 * kPiSh / 2.0);
      close_fold(hole_path);
      subs.push_back(std::move(hole_path));
    }
    return toPath(subs);
  } else if (shape->type == tag("ShCl")) {
    // Cloud: Bubl bubbles; notches at radius IRad between bumps at radius 1.
    // Each bubble half is a quarter-ellipse in the bump-rotated frame with
    // radial semi-axis 1 - IRad*cos(P/2) and tangential semi-axis
    // IRad*sin(P/2) (wide- and default-probe pinned).
    const auto bubbles = std::clamp<std::int64_t>(shape->integer(tag("Bubl"), 12), 3, 256);
    const double inner = std::clamp(shape->number(tag("IRad"), 0.8164966), 0.01, 1.0);
    const double period = 2.0 * kPiSh / static_cast<double>(bubbles);
    const double radial = std::max(0.0, 1.0 - inner * std::cos(period / 2.0));
    const double tangential = inner * std::sin(period / 2.0);
    for (std::int64_t k = 0; k < bubbles; ++k) {
      const double bump = -kPiSh / 2.0 + period * static_cast<double>(k);
      const double dirx = std::cos(bump);
      const double diry = std::sin(bump);
      const double tanx = -std::sin(bump);
      const double tany = std::cos(bump);
      const double n1x = inner * std::cos(bump - period / 2.0);
      const double n1y = inner * std::sin(bump - period / 2.0);
      const double n2x = inner * std::cos(bump + period / 2.0);
      const double n2y = inner * std::sin(bump + period / 2.0);
      const auto push_or_patch = [&](double ux, double uy, double outx, double outy) {
        const double px = unit_x(ux);
        const double py = unit_y(uy);
        if (!subpath.anchors.empty() && std::abs(subpath.anchors.back().anchor_x - px) < 1e-6 &&
            std::abs(subpath.anchors.back().anchor_y - py) < 1e-6) {
          subpath.anchors.back().out_x = unit_x(outx);
          subpath.anchors.back().out_y = unit_y(outy);
        } else {
          subpath.anchors.push_back(PA{px, py, px, py, unit_x(outx), unit_y(outy),
                                               false});
        }
      };
      push_or_patch(n1x, n1y, n1x + kCircleK * radial * dirx, n1y + kCircleK * radial * diry);
      subpath.anchors.push_back(PA{
          unit_x(dirx), unit_y(diry), unit_x(dirx - kCircleK * tangential * tanx),
          unit_y(diry - kCircleK * tangential * tany),
          unit_x(dirx + kCircleK * tangential * tanx),
          unit_y(diry + kCircleK * tangential * tany), true});
      subpath.anchors.push_back(PA{unit_x(n2x), unit_y(n2y),
                                           unit_x(n2x + kCircleK * radial * dirx),
                                           unit_y(n2y + kCircleK * radial * diry), unit_x(n2x),
                                           unit_y(n2y), false});
    }
    close_fold(subpath);
  } else if (shape->type == tag("ShpT")) {
    // Triangle: apex at the "Pos " fraction across the top edge (default
    // centered), base along the bottom of the box.
    for (const auto& field : shape->fields) {
      if (field.tag != tag("Pos ")) {
        *why = "is an Affinity triangle variant NekoPhoto does not read yet";
        return std::nullopt;
      }
    }
    const double pos = std::clamp(shape->number(tag("Pos "), 0.5), 0.0, 1.0);
    const double ax = x0 + pos * w;
    subpath.anchors.push_back(PA{ax, y0, ax, y0, ax, y0, false});
    subpath.anchors.push_back(PA{x1, y1, x1, y1, x1, y1, false});
    subpath.anchors.push_back(PA{x0, y1, x0, y1, x0, y1, false});
  } else {
    *why = "is an Affinity shape kind NekoPhoto does not read yet";
    return std::nullopt;
  }

  if (subpath.anchors.size() < 2) {
    *why = "has a degenerate shape outline";
    return std::nullopt;
  }
  subs.push_back(std::move(subpath));
  return toPath(subs);
}

void transformPath(VectorPath& path, const Affine6& m) {
    for (auto& s : path.subpaths)
        for (auto& k : s.knots) {
            const Point a = apply(m, k.x, k.y), i = apply(m, k.inX, k.inY), o = apply(m, k.outX, k.outY);
            k = {i.x, i.y, a.x, a.y, o.x, o.y};
        }
}

bool isArtboard(const Class& node) { return node.type == tag("ShpN") && (node.boolean(tag("ABEn"), false) || node.child(tag("phrp"))); }

/// Adjustments (the *RA family) and live filters (FlRN): their bitmap is a mask, not content.
bool isAdjustmentOrFilter(uint32_t type) { return (type & 0xFFFFu) == (uint32_t('R') << 8 | 'A') || type == tag("FlRN"); }

} // namespace nekophoto::affinity_read
