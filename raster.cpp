#include "raster.h"
#include <algorithm>
#include <cmath>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace {

struct Edge { float x0, y0, x1, y1; };   // always y0 < y1

void addLine(std::vector<Edge>& e, Pt a, Pt b) {
    if (a.y == b.y) return;
    if (a.y < b.y) e.push_back({a.x, a.y, b.x, b.y});
    else           e.push_back({b.x, b.y, a.x, a.y});
}

Pt cubic(Pt p0, Pt c1, Pt c2, Pt p1, float t) {
    float u = 1 - t;
    float a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
    return {a * p0.x + b * c1.x + c * c2.x + d * p1.x, a * p0.y + b * c1.y + c * c2.y + d * p1.y};
}

void flatten(const Loop& l, float sx, float sy, std::vector<Edge>& e) {
    auto S = [&](Pt p) { return Pt{p.x * sx, p.y * sy}; };
    Pt start = S(l.start), cur = start;
    for (const Seg& s : l.segs) {
        Pt c1 = S(s.c1), c2 = S(s.c2), p = S(s.p);
        if (s.line) {
            addLine(e, cur, p);
        } else {
            float est = std::hypot(c1.x - cur.x, c1.y - cur.y) + std::hypot(c2.x - c1.x, c2.y - c1.y) +
                        std::hypot(p.x - c2.x, p.y - c2.y);
            int n = std::clamp(int(est / 3.f) + 2, 2, 48);
            Pt prev = cur;
            for (int k = 1; k <= n; k++) {
                Pt q = cubic(cur, c1, c2, p, float(k) / n);
                addLine(e, prev, q);
                prev = q;
            }
        }
        cur = p;
    }
    addLine(e, cur, start);
}

void fillShape(std::vector<Edge>& edges, const Shape& sh, int W, int H, std::vector<uint8_t>& dst) {
    if (edges.empty()) return;
    std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) { return a.y0 < b.y0; });
    float maxY = 0;
    for (const Edge& e : edges) maxY = std::max(maxY, e.y1);
    int yStart = std::max(0, (int)std::floor(edges.front().y0));
    int yEnd = std::min(H - 1, (int)std::floor(maxY));

    constexpr int SS = 4;
    const float w = 1.f / SS;
    std::vector<int> active;
    size_t nxt = 0;
    std::vector<float> part(W + 2), diff(W + 2), xs;

    for (int y = yStart; y <= yEnd; y++) {
        while (nxt < edges.size() && edges[nxt].y0 < y + 1) active.push_back((int)nxt++);
        active.erase(std::remove_if(active.begin(), active.end(), [&](int i) { return edges[i].y1 <= y; }),
                     active.end());
        if (active.empty()) continue;
        std::fill(part.begin(), part.end(), 0.f);
        std::fill(diff.begin(), diff.end(), 0.f);
        int lo = W, hi = -1;

        for (int s = 0; s < SS; s++) {
            float ys = y + (s + 0.5f) / SS;
            xs.clear();
            for (int i : active) {
                const Edge& e = edges[i];
                if (ys >= e.y0 && ys < e.y1) xs.push_back(e.x0 + (ys - e.y0) * (e.x1 - e.x0) / (e.y1 - e.y0));
            }
            std::sort(xs.begin(), xs.end());
            for (size_t k = 0; k + 1 < xs.size(); k += 2) {
                float xa = std::clamp(xs[k], 0.f, (float)W), xb = std::clamp(xs[k + 1], 0.f, (float)W);
                if (xb <= xa) continue;
                int ia = (int)xa, ib = (int)xb;
                if (ia == ib) {
                    part[ia] += (xb - xa) * w;
                } else {
                    part[ia] += (ia + 1 - xa) * w;
                    diff[ia + 1] += w;
                    diff[ib] -= w;
                    part[ib] += (xb - ib) * w;
                }
                lo = std::min(lo, ia);
                hi = std::max(hi, ib);
            }
        }
        if (hi < 0) continue;

        float run = 0.f;
        uint8_t* row = &dst[size_t(y) * W * 4];
        for (int x = lo; x <= std::min(hi, W - 1); x++) {
            run += diff[x];
            float a = std::min(1.f, part[x] + run);
            if (a <= 0.f) continue;
            uint8_t* d = row + size_t(x) * 4;
            float da = d[3] / 255.f;
            float oa = a + da * (1.f - a);
            const uint8_t col[3] = {sh.r, sh.g, sh.b};
            for (int c = 0; c < 3; c++)
                d[c] = uint8_t(std::min(255.f, (col[c] * a + d[c] * da * (1.f - a)) / oa + 0.5f));
            d[3] = uint8_t(std::min(255.f, oa * 255.f + 0.5f));
        }
    }
}

} // namespace

std::vector<uint8_t> renderRGBA(const VectorImage& v, int W, int H) {
    std::vector<uint8_t> dst(size_t(W) * H * 4, 0);
    if (v.width <= 0 || v.height <= 0 || W <= 0 || H <= 0) return dst;
    float sx = float(W) / v.width, sy = float(H) / v.height;
    std::vector<Edge> edges;
    for (const Shape& s : v.shapes) {
        edges.clear();
        for (const Loop& l : s.loops) flatten(l, sx, sy, edges);
        fillShape(edges, s, W, H, dst);
    }
    return dst;
}

std::string toPNG(const VectorImage& v, int W, int H) {
    std::vector<uint8_t> px = renderRGBA(v, W, H);
    std::string out;
    stbi_write_png_to_func([](void* ctx, void* data, int size) {
        static_cast<std::string*>(ctx)->append(static_cast<const char*>(data), size);
    }, &out, W, H, 4, px.data(), W * 4);
    return out;
}
