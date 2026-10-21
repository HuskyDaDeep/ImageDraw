#include "vectorizer.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <thread>
#include <utility>

namespace {

// ---------- small helpers ----------
template <class F> void parallel(int T, F&& f) {
    std::vector<std::thread> th;
    for (int t = 1; t < T; t++) th.emplace_back(f, t);
    f(0);
    for (auto& x : th) x.join();
}
int hwThreads() { return (int)std::max(1u, std::thread::hardware_concurrency()); }

inline float dist(Pt a, Pt b) { return std::hypot(b.x - a.x, b.y - a.y); }
inline Pt toward(Pt from, Pt to, float d) {
    float l = dist(from, to);
    if (l < 1e-6f) return from;
    return {from.x + (to.x - from.x) / l * d, from.y + (to.y - from.y) / l * d};
}
inline Pt unitOr(Pt v, Pt fallback) {
    float l = std::hypot(v.x, v.y);
    return l < 1e-6f ? fallback : Pt{v.x / l, v.y / l};
}

// ---------- CIE Lab ----------
struct Lab { float L, a, b; };

inline float d2(const Lab& p, const Lab& q) {
    float x = p.L - q.L, y = p.a - q.a, z = p.b - q.b;
    return x * x + y * y + z * z;
}

Lab toLab(uint8_t r8, uint8_t g8, uint8_t b8) {
    struct LUT {
        float v[256];
        LUT() {
            for (int i = 0; i < 256; i++) {
                float c = i / 255.f;
                v[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
            }
        }
    };
    static const LUT lut;
    float R = lut.v[r8], G = lut.v[g8], B = lut.v[b8];
    float X = (0.4124564f * R + 0.3575761f * G + 0.1804375f * B) / 0.95047f;
    float Y = 0.2126729f * R + 0.7151522f * G + 0.0721750f * B;
    float Z = (0.0193339f * R + 0.1191920f * G + 0.9503041f * B) / 1.08883f;
    auto f = [](float t) { return t > 0.008856f ? std::cbrt(t) : 7.787f * t + 16.f / 116.f; };
    float fx = f(X), fy = f(Y), fz = f(Z);
    return {116.f * fy - 16.f, 500.f * (fx - fy), 200.f * (fy - fz)};
}

int nearest(const std::vector<Lab>& pal, const Lab& c) {
    int best = 0; float bd = 1e30f;
    for (int i = 0; i < (int)pal.size(); i++) {
        float d = d2(pal[i], c);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

// ---------- image preprocessing ----------
std::vector<uint8_t> boxResize(const uint8_t* src, int w, int h, int w2, int h2) {
    std::vector<uint8_t> out(size_t(w2) * h2 * 4);
    for (int y2 = 0; y2 < h2; y2++) {
        long long y0 = (long long)y2 * h / h2;
        long long y1 = std::max(y0 + 1, (long long)(y2 + 1) * h / h2);
        for (int x2 = 0; x2 < w2; x2++) {
            long long x0 = (long long)x2 * w / w2;
            long long x1 = std::max(x0 + 1, (long long)(x2 + 1) * w / w2);
            long long s[4] = {0, 0, 0, 0};
            for (long long y = y0; y < y1; y++)
                for (long long x = x0; x < x1; x++)
                    for (int c = 0; c < 4; c++) s[c] += src[(y * w + x) * 4 + c];
            long long cnt = (y1 - y0) * (x1 - x0);
            for (int c = 0; c < 4; c++) out[(size_t(y2) * w2 + x2) * 4 + c] = uint8_t(s[c] / cnt);
        }
    }
    return out;
}

void blur3x3(std::vector<uint8_t>& px, int w, int h) {
    std::vector<uint8_t> src = px;
    int T = std::min(hwThreads(), 8);
    parallel(T, [&](int t) {
        for (int y = h * t / T; y < h * (t + 1) / T; y++)
            for (int x = 0; x < w; x++) {
                int s[3] = {0, 0, 0}, n = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int xx = x + dx, yy = y + dy;
                        if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                        for (int c = 0; c < 3; c++) s[c] += src[(size_t(yy) * w + xx) * 4 + c];
                        n++;
                    }
                for (int c = 0; c < 3; c++) px[(size_t(y) * w + x) * 4 + c] = uint8_t(s[c] / n);
            }
    });
}

// Flood-fill from the border: pixels close to the top-left color become transparent.
void removeBackground(std::vector<uint8_t>& px, const std::vector<Lab>& lab, int w, int h) {
    const Lab ref = lab[0];
    const float thr2 = 10.f * 10.f;
    std::vector<char> seen(size_t(w) * h, 0);
    std::vector<int> st;
    auto push = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        int i = y * w + x;
        if (seen[i] || d2(lab[i], ref) >= thr2) return;
        seen[i] = 1; st.push_back(i);
    };
    for (int x = 0; x < w; x++) { push(x, 0); push(x, h - 1); }
    for (int y = 0; y < h; y++) { push(0, y); push(w - 1, y); }
    while (!st.empty()) {
        int i = st.back(); st.pop_back();
        int x = i % w, y = i / w;
        px[size_t(i) * 4 + 3] = 0;
        push(x + 1, y); push(x - 1, y); push(x, y + 1); push(x, y - 1);
    }
}

// ---------- palette (k-means in Lab) ----------
std::vector<Lab> buildPalette(const std::vector<Lab>& lab, const std::vector<uint8_t>& px, int n, int k,
                              const std::vector<uint8_t>* flat, int shading) {
    std::vector<Lab> s;
    int step = std::max(1, n / 30000);
    for (int i = 0; i < n; i += step)
        if (px[size_t(i) * 4 + 3] >= 128 && (!flat || (*flat)[i])) s.push_back(lab[i]);
    std::vector<Lab> pal;
    if (s.empty()) return pal;
    k = std::min<int>(k, (int)s.size());

    pal.push_back(s[0]);
    std::vector<float> best(s.size(), 1e30f);
    while ((int)pal.size() < k) {
        size_t far = 0; float fd = -1.f;
        for (size_t i = 0; i < s.size(); i++) {
            best[i] = std::min(best[i], d2(s[i], pal.back()));
            if (best[i] > fd) { fd = best[i]; far = i; }
        }
        pal.push_back(s[far]);
    }
    for (int it = 0; it < 8; it++) {
        std::vector<Lab> sum(k, Lab{0, 0, 0});
        std::vector<int> cnt(k, 0);
        for (const Lab& p : s) {
            int b = nearest(pal, p);
            sum[b].L += p.L; sum[b].a += p.a; sum[b].b += p.b; cnt[b]++;
        }
        for (int c = 0; c < k; c++)
            if (cnt[c]) pal[c] = {sum[c].L / cnt[c], sum[c].a / cnt[c], sum[c].b / cnt[c]};
    }

    // Adaptive shading tones: keep splitting the palette entry with the largest color error (a smooth
    // gradient or soft shadow spread over many pixels) until the extra budget is used or every
    // entry is visually uniform. Flat colors have ~0 error, so they never get extra tones.
    if (shading > 0) {
        const int total = std::min(64, k + (int)std::lround(k * 0.5 * std::min(shading, 4)));
        std::vector<int> asg(s.size());
        for (size_t i = 0; i < s.size(); i++) asg[i] = nearest(pal, s[i]);
        while ((int)pal.size() < total) {
            const int P = (int)pal.size();
            std::vector<double> sse(P, 0.0);
            std::vector<int> cn(P, 0);
            for (size_t i = 0; i < s.size(); i++) { sse[asg[i]] += d2(s[i], pal[asg[i]]); cn[asg[i]]++; }
            int bc = -1; double bs = 0;
            for (int c = 0; c < P; c++)
                if (cn[c] >= 8 && sse[c] > bs) { bs = sse[c]; bc = c; }
            if (bc < 0 || std::sqrt(bs / cn[bc]) < 2.5) break;           // already uniform enough

            std::vector<int> mem;
            for (size_t i = 0; i < s.size(); i++) if (asg[i] == bc) mem.push_back((int)i);
            double m[3] = {0, 0, 0};
            for (int i : mem) { m[0] += s[i].L; m[1] += s[i].a; m[2] += s[i].b; }
            for (double& x : m) x /= mem.size();
            double cov[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
            for (int i : mem) {
                double d[3] = {s[i].L - m[0], s[i].a - m[1], s[i].b - m[2]};
                for (int r = 0; r < 3; r++) for (int q = 0; q < 3; q++) cov[r][q] += d[r] * d[q];
            }
            double v[3] = {1.0, 0.3, 0.1};                                  // principal axis (power iteration)
            for (int it = 0; it < 12; it++) {
                double t[3];
                for (int r = 0; r < 3; r++) t[r] = cov[r][0] * v[0] + cov[r][1] * v[1] + cov[r][2] * v[2];
                double nrm = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
                if (nrm < 1e-12) break;
                for (int r = 0; r < 3; r++) v[r] = t[r] / nrm;
            }
            Lab ca{0, 0, 0}, cb{0, 0, 0}; int na = 0, nb = 0;
            for (int i : mem) {
                double pr = (s[i].L - m[0]) * v[0] + (s[i].a - m[1]) * v[1] + (s[i].b - m[2]) * v[2];
                Lab& t = pr < 0 ? ca : cb; (pr < 0 ? na : nb)++;
                t.L += s[i].L; t.a += s[i].a; t.b += s[i].b;
            }
            if (na == 0 || nb == 0) break;
            pal[bc] = {ca.L / na, ca.a / na, ca.b / na};
            pal.push_back({cb.L / nb, cb.a / nb, cb.b / nb});
            for (int r = 0; r < 3; r++) {                                   // refine the two halves (2-means)
                Lab sa{0, 0, 0}, sb{0, 0, 0}; int ka = 0, kb = 0;
                for (int i : mem) {
                    bool toA = d2(s[i], pal[bc]) <= d2(s[i], pal[P]);
                    asg[i] = toA ? bc : P;
                    Lab& t = toA ? sa : sb; (toA ? ka : kb)++;
                    t.L += s[i].L; t.a += s[i].a; t.b += s[i].b;
                }
                if (ka) pal[bc] = {sa.L / ka, sa.a / ka, sa.b / ka};
                if (kb) pal[P] = {sb.L / kb, sb.a / kb, sb.b / kb};
            }
        }
        for (int it = 0; it < 3; it++) {                                    // settle all entries
            std::vector<Lab> sum(pal.size(), Lab{0, 0, 0});
            std::vector<int> cn(pal.size(), 0);
            for (const Lab& p : s) {
                int b = nearest(pal, p);
                sum[b].L += p.L; sum[b].a += p.a; sum[b].b += p.b; cn[b]++;
            }
            for (size_t c = 0; c < pal.size(); c++)
                if (cn[c]) pal[c] = {sum[c].L / cn[c], sum[c].a / cn[c], sum[c].b / cn[c]};
        }
    }
    return pal;
}

// ---------- geometry ----------
float lineDist(Pt p, Pt a, Pt b) {
    float dx = b.x - a.x, dy = b.y - a.y;
    float l2 = dx * dx + dy * dy;
    if (l2 == 0.f) return dist(p, a);
    return std::fabs((p.x - a.x) * dy - (p.y - a.y) * dx) / std::sqrt(l2);
}

void rdp(const std::vector<Pt>& q, int lo, int hi, float tol, std::vector<char>& keep) {
    std::vector<std::pair<int, int>> st{{lo, hi}};
    while (!st.empty()) {
        auto [a, b] = st.back(); st.pop_back();
        float md = 0.f; int mi = -1;
        for (int i = a + 1; i < b; i++) {
            float d = lineDist(q[i], q[a], q[b]);
            if (d > md) { md = d; mi = i; }
        }
        if (mi >= 0 && md > tol) {
            keep[mi] = 1;
            st.push_back({a, mi});
            st.push_back({mi, b});
        }
    }
}

std::vector<Pt> simplifyClosed(const std::vector<Pt>& p, float tol) {
    int n = (int)p.size();
    if (n < 4) return p;
    int f = 0; float fd = -1.f;
    for (int i = 1; i < n; i++) {
        float d = dist(p[i], p[0]);
        if (d > fd) { fd = d; f = i; }
    }
    std::vector<Pt> q = p; q.push_back(p[0]);
    std::vector<char> keep(n + 1, 0);
    keep[0] = keep[f] = 1;
    rdp(q, 0, f, tol, keep);
    rdp(q, f, n, tol, keep);
    std::vector<Pt> out;
    for (int i = 0; i < n; i++) if (keep[i]) out.push_back(q[i]);
    return out;
}

// Sub-pixel contour -> final Loop (polygon or smooth curves).
bool buildLoop(const std::vector<Pt>& raw, double area, const Options& o, float sx, float sy, Loop& out) {
    if (raw.size() < 3) return false;

    // small shapes get a tighter tolerance so they keep their shape
    const float tol = std::clamp(0.12f * std::sqrt(float(area)), 0.15f, std::max(0.15f, o.tolerance));
    std::vector<Pt> pts = simplifyClosed(raw, tol);
    const int n = (int)pts.size();
    if (n < 3) return false;

    // corners (only needed for curves)
    std::vector<char> corner(n, 0);
    if (o.smooth) {
        auto turn = [&](int i) {
            Pt a = pts[(i + n - 1) % n], b = pts[i], c = pts[(i + 1) % n];
            float ux = b.x - a.x, uy = b.y - a.y, vx = c.x - b.x, vy = c.y - b.y;
            return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
        };
        std::vector<float> t(n);
        for (int i = 0; i < n; i++) t[i] = turn(i);
        for (int i = 0; i < n; i++) {
            if (std::fabs(t[i]) > 0.96f) corner[i] = 1;                 // > ~55 degrees
            int j = (i + 1) % n;
            if (dist(pts[i], pts[j]) < 1.5f && t[i] * t[j] > 0 && std::fabs(t[i] + t[j]) > 1.4f)
                corner[i] = corner[j] = 1;                             // a sharp corner split in two
        }
    }

    for (Pt& p : pts) { p.x *= sx; p.y *= sy; }

    out.start = pts[0];
    out.segs.clear();
    if (!o.smooth) {
        for (int i = 1; i < n; i++) out.segs.push_back({pts[i], pts[i], pts[i], true});
        return true;
    }
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        Pt pi = pts[i], pj = pts[j];
        if (corner[i] && corner[j]) { out.segs.push_back({pi, pj, pj, true}); continue; }
        float L = dist(pi, pj);
        Pt chord = {pj.x - pi.x, pj.y - pi.y};
        Pt dOut = corner[i] ? unitOr(chord, chord)
                            : unitOr({pj.x - pts[(i + n - 1) % n].x, pj.y - pts[(i + n - 1) % n].y}, chord);
        Pt dIn  = corner[j] ? unitOr(chord, chord)
                            : unitOr({pts[(j + 1) % n].x - pi.x, pts[(j + 1) % n].y - pi.y}, chord);
        Pt c1 = {pi.x + dOut.x * L / 3.f, pi.y + dOut.y * L / 3.f};
        Pt c2 = {pj.x - dIn.x * L / 3.f,  pj.y - dIn.y * L / 3.f};
        out.segs.push_back({c1, c2, pj, false});
    }
    return true;
}

// Separable grayscale dilation (max) or erosion (min) with a (2r+1)x(2r+1) window.
void morph(const std::vector<float>& src, std::vector<float>& dst, int w, int h, int r, bool useMax) {
    std::vector<float> tmp(src.size());
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float v = src[size_t(y) * w + x];
            for (int d = -r; d <= r; d++) {
                int xx = std::clamp(x + d, 0, w - 1);
                float s = src[size_t(y) * w + xx];
                v = useMax ? std::max(v, s) : std::min(v, s);
            }
            tmp[size_t(y) * w + x] = v;
        }
    dst.resize(src.size());
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float v = tmp[size_t(y) * w + x];
            for (int d = -r; d <= r; d++) {
                int yy = std::clamp(y + d, 0, h - 1);
                float s = tmp[size_t(yy) * w + x];
                v = useMax ? std::max(v, s) : std::min(v, s);
            }
            dst[size_t(y) * w + x] = v;
        }
}

// Hairlines are 1-2px wide and anti-aliased, so their darkest pixel is often a mid tone that gets
// quantized to the surrounding color. The black-hat of the luminance (closing - image) finds dark
// features narrower than the window; those pixels are darkened before quantization. (Brightening thin
// light features was tried and removed: it turned the gaps between close dark strokes white.)
#ifndef THIN_K
#define THIN_K 1.5f
#endif
void enhanceThinLines(std::vector<uint8_t>& px, int w, int h) {
    const size_t n = size_t(w) * h;
    std::vector<float> L(n), D, C;
    for (size_t i = 0; i < n; i++)
        L[i] = 0.299f * px[i * 4] + 0.587f * px[i * 4 + 1] + 0.114f * px[i * 4 + 2];
    morph(L, D, w, h, 2, true);
    morph(D, C, w, h, 2, false);                                  // closing: fills thin dark lines
    for (size_t i = 0; i < n; i++) {
        if (px[i * 4 + 3] < 128) continue;
        float bh = C[i] - L[i];
        if (bh <= 12.f) continue;
        float s = 1.f - std::min(0.9f, THIN_K * bh / std::max(C[i], 1.f));
        for (int c = 0; c < 3; c++) px[i * 4 + c] = uint8_t(px[i * 4 + c] * s + 0.5f);
    }
}

// Per-worker buffers for the marching-squares tracer (grown on demand, sized to each layer's bounding box).
struct Buf {
    std::vector<float> f, tmp;          // smoothed layer mask (padded by 1 cell on each side)
    std::vector<int> a0, a1, touched;   // contour adjacency: every crossing edge has exactly 2 neighbors
    void ensure(size_t n) {
        if (f.size() < n) { f.resize(n); tmp.resize(n); }
        if (a0.size() < 2 * n) { a0.resize(2 * n, -1); a1.resize(2 * n, -1); }
    }
};

// Per-channel median on RGB: removes speckle / halftone dots smaller than the window, keeps edges.
void medianFilter(std::vector<uint8_t>& px, int w, int h, int r, int T) {
    const std::vector<uint8_t> src = px;
    parallel(T, [&](int t) {
        uint8_t v[3][49];
        for (int y = h * t / T; y < h * (t + 1) / T; y++)
            for (int x = 0; x < w; x++) {
                int m = 0;
                for (int dy = -r; dy <= r; dy++)
                    for (int dx = -r; dx <= r; dx++) {
                        int xx = std::clamp(x + dx, 0, w - 1), yy = std::clamp(y + dy, 0, h - 1);
                        const uint8_t* p = &src[(size_t(yy) * w + xx) * 4];
                        v[0][m] = p[0]; v[1][m] = p[1]; v[2][m] = p[2];
                        m++;
                    }
                for (int c = 0; c < 3; c++) {
                    std::nth_element(v[c], v[c] + m / 2, v[c] + m);
                    px[(size_t(y) * w + x) * 4 + c] = v[c][m / 2];
                }
            }
    });
}

// Replace a pixel's color with the most common color in its window when its own color is a minority.
void majorityFilter(std::vector<int>& idx, int P, int w, int h, int r, int T) {
    const std::vector<int> src = idx;
    parallel(T, [&](int t) {
        std::vector<int> cnt(P, 0);
        for (int y = h * t / T; y < h * (t + 1) / T; y++)
            for (int x = 0; x < w; x++) {
                int own = src[size_t(y) * w + x];
                if (own < 0) continue;
                for (int dy = -r; dy <= r; dy++)
                    for (int dx = -r; dx <= r; dx++) {
                        int xx = x + dx, yy = y + dy;
                        if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                        int c = src[size_t(yy) * w + xx];
                        if (c >= 0) cnt[c]++;
                    }
                int best = own, bc = cnt[own];
                for (int dy = -r; dy <= r; dy++)
                    for (int dx = -r; dx <= r; dx++) {
                        int xx = x + dx, yy = y + dy;
                        if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                        int c = src[size_t(yy) * w + xx];
                        if (c >= 0 && cnt[c] > bc) { bc = cnt[c]; best = c; }
                    }
                for (int dy = -r; dy <= r; dy++)
                    for (int dx = -r; dx <= r; dx++) {
                        int xx = x + dx, yy = y + dy;
                        if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                        int c = src[size_t(yy) * w + xx];
                        if (c >= 0) cnt[c] = 0;
                    }
                idx[size_t(y) * w + x] = best;
            }
    });
}

} // namespace

VectorImage vectorize(const uint8_t* rgba, int w0, int h0, const Options& o, Control* ctl) {
    VectorImage out;
    out.width = w0; out.height = h0;
    auto progress = [&](float f) { if (ctl) ctl->progress = f; };
    auto cancelled = [&] { return ctl && ctl->cancel.load(); };
    progress(0.f);

    // --- 1. optional downscale for speed ---
    int w = w0, h = h0;
    std::vector<uint8_t> px;
    int side = std::max(w0, h0);
    if (o.maxSide > 0 && side > o.maxSide) {
        double f = double(o.maxSide) / side;
        w = std::max(1, (int)std::lround(w0 * f));
        h = std::max(1, (int)std::lround(h0 * f));
        px = boxResize(rgba, w0, h0, w, h);
    } else {
        px.assign(rgba, rgba + size_t(w0) * h0 * 4);
    }
    const float sx = float(w0) / w, sy = float(h0) / h;
    const int n = w * h;
    const int T = std::min(hwThreads(), 8);

    if (o.denoise) blur3x3(px, w, h);
    if (o.texture > 0) medianFilter(px, w, h, o.texture == 1 ? 2 : 3, T);
    if (o.fineDetails && o.texture == 0 && !o.denoise) enhanceThinLines(px, w, h);
    const int minArea = o.texture > 0 ? std::max(o.minArea, o.texture == 1 ? 20 : 48) : o.minArea;
    if (cancelled()) return out;

    // --- 2. Lab conversion (parallel) ---
    std::vector<Lab> lab(n);
    parallel(T, [&](int t) {
        for (int i = int((long long)n * t / T); i < int((long long)n * (t + 1) / T); i++)
            lab[i] = toLab(px[size_t(i) * 4], px[size_t(i) * 4 + 1], px[size_t(i) * 4 + 2]);
    });
    if (o.removeBg) removeBackground(px, lab, w, h);
    progress(0.1f);

    // --- 3. palette + assignment ---
    // Flat pixels = low local color gradient. The palette is trained on them only, so the blended
    // colors of anti-aliased edges never become palette entries (no halos).
    std::vector<uint8_t> flat(n, 0);
    std::vector<float> gm2(n, 0.f);                       // squared local color gradient (Lab units)
    std::atomic<long long> flatCount{0}, validCount{0};
    parallel(T, [&](int t) {
        long long fc = 0, vc = 0;
        for (int y = h * t / T; y < h * (t + 1) / T; y++)
            for (int x = 0; x < w; x++) {
                int i = y * w + x;
                if (px[size_t(i) * 4 + 3] < 128) continue;
                vc++;
                float m = 0.f;
                const int nx[4] = {x - 1, x + 1, x, x}, ny[4] = {y, y, y - 1, y + 1};
                for (int k = 0; k < 4; k++) {
                    if (nx[k] < 0 || ny[k] < 0 || nx[k] >= w || ny[k] >= h) continue;
                    int j = ny[k] * w + nx[k];
                    if (px[size_t(j) * 4 + 3] < 128) continue;
                    m = std::max(m, d2(lab[i], lab[j]));
                }
                gm2[i] = m;
                if (m < 36.f) { flat[i] = 1; fc++; }
            }
        flatCount += fc; validCount += vc;
    });
    // photos / smooth gradients have few flat pixels: then train on everything
    const bool useFlat = flatCount.load() >= 0.15 * double(validCount.load()) && flatCount.load() > 0;

    std::vector<Lab> pal = buildPalette(lab, px, n, std::clamp(o.colors, 2, 64), useFlat ? &flat : nullptr, o.shading);
    if (pal.empty()) return out;
    progress(0.2f);

    std::vector<int> idx(n, -1);
    parallel(T, [&](int t) {
        for (int i = int((long long)n * t / T); i < int((long long)n * (t + 1) / T); i++) {
            if (px[size_t(i) * 4 + 3] < 128) continue;
            idx[i] = (!useFlat || flat[i]) ? nearest(pal, lab[i]) : -2;      // -2 = edge pixel, decided below
        }
    });
    if (useFlat) {
        // edge pixels choose among the colors of nearby flat pixels (never an in-between color)
        const std::vector<int> src = idx;
        parallel(T, [&](int t) {
            for (int y = h * t / T; y < h * (t + 1) / T; y++)
                for (int x = 0; x < w; x++) {
                    if (src[size_t(y) * w + x] != -2) continue;
                    const Lab& L0 = lab[size_t(y) * w + x];
                    // thin features (hairlines, small marks) have no flat pixels of their own: if the pixel
                    // is essentially a pure palette color, keep it; only blended colors use the local rule
                    int g = nearest(pal, L0);
                    if (gm2[size_t(y) * w + x] < 625.f) { idx[size_t(y) * w + x] = g; continue; }
                    if (d2(L0, pal[g]) < 64.f) { idx[size_t(y) * w + x] = g; continue; }
                    // colors of flat pixels nearby
                    int loc[8], nl = 0;
                    for (int dy = -4; dy <= 4; dy++)
                        for (int dx = -4; dx <= 4; dx++) {
                            int xx = x + dx, yy = y + dy;
                            if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                            int c = src[size_t(yy) * w + xx];
                            if (c < 0) continue;
                            bool seen = false;
                            for (int k = 0; k < nl; k++) seen |= (loc[k] == c);
                            if (!seen && nl < 8) loc[nl++] = c;
                        }
                    // a blend of two nearby colors = anti-aliasing: snap to the closest of them.
                    // Anything else is a real (thin) feature: use the closest palette color overall.
                    bool blend = false;
                    for (int a = 0; a < nl && !blend; a++)
                        for (int b2 = a; b2 < nl; b2++) {
                            const Lab &pa = pal[loc[a]], &pb = pal[loc[b2]];
                            float abx = pb.L - pa.L, aby = pb.a - pa.a, abz = pb.b - pa.b;
                            float acx = L0.L - pa.L, acy = L0.a - pa.a, acz = L0.b - pa.b;
                            float l2 = abx * abx + aby * aby + abz * abz;
                            float t = l2 > 1e-6f ? std::clamp((acx * abx + acy * aby + acz * abz) / l2, 0.f, 1.f) : 0.f;
                            float dx2 = acx - t * abx, dy2 = acy - t * aby, dz2 = acz - t * abz;
                            if (dx2 * dx2 + dy2 * dy2 + dz2 * dz2 < 14.f * 14.f) { blend = true; break; }
                        }
                    int best = g;
                    if (blend) {
                        float bd = 1e30f;
                        for (int k = 0; k < nl; k++) {
                            float d = d2(L0, pal[loc[k]]);
                            if (d < bd) { bd = d; best = loc[k]; }
                        }
                    }
                    idx[size_t(y) * w + x] = best;
                }
        });
    }
    lab.clear(); lab.shrink_to_fit();
    if (cancelled()) return out;

    if (o.texture > 0) majorityFilter(idx, (int)pal.size(), w, h, o.texture == 1 ? 2 : 3, T);

    // --- 3b. edge cleanup: a pixel whose color is a minority in its 3x3 neighborhood (1px slivers
    //         and isolated pixels created by anti-aliasing) takes the most common neighbor color ---
    for (int pass = 0; pass < std::clamp(o.cleanup, 0, 3); pass++) {
        const std::vector<int> src = idx;
        parallel(T, [&](int t) {
            for (int y = h * t / T; y < h * (t + 1) / T; y++)
                for (int x = 0; x < w; x++) {
                    int own = src[size_t(y) * w + x];
                    if (own < 0) continue;
                    int nb[8], m = 0, ownCount = 1;
                    for (int dy = -1; dy <= 1; dy++)
                        for (int dx = -1; dx <= 1; dx++) {
                            if (!dx && !dy) continue;
                            int xx = x + dx, yy = y + dy;
                            if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                            int c = src[size_t(yy) * w + xx];
                            if (c < 0) continue;
                            if (c == own) ownCount++; else nb[m++] = c;
                        }
                    if (ownCount > 3 || m == 0) continue;
                    if (o.fineDetails && ownCount >= 3) {
                        // a line of one color over one uniform surrounding is a real stroke, not an
                        // anti-aliasing sliver (a sliver has two different colors on its sides)
                        bool oneSurround = true;
                        for (int i = 1; i < m; i++) oneSurround &= (nb[i] == nb[0]);
                        if (oneSurround) continue;
                    }
                    int best = -1, bc = 0;
                    for (int i = 0; i < m; i++) {
                        int k = 0;
                        for (int j = 0; j < m; j++) k += (nb[j] == nb[i]);
                        if (k > bc) { bc = k; best = nb[i]; }
                    }
                    if (bc > ownCount) idx[size_t(y) * w + x] = best;
                }
        });
    }

    const int P = (int)pal.size();
    std::vector<long long> cnt(P, 0), sr(P, 0), sg(P, 0), sb(P, 0);
    for (int i = 0; i < n; i++) {
        int c = idx[i];
        if (c < 0) continue;
        cnt[c]++; sr[c] += px[size_t(i) * 4]; sg[c] += px[size_t(i) * 4 + 1]; sb[c] += px[size_t(i) * 4 + 2];
    }
    // Back-to-front order: biggest color first (it becomes the background layer).
    std::vector<int> order;
    for (int c = 0; c < P; c++) if (cnt[c] > 0) order.push_back(c);
    std::sort(order.begin(), order.end(), [&](int a, int b) { return cnt[a] > cnt[b]; });
    const int K = (int)order.size();
    std::vector<int> rankOf(P, -1);
    for (int r = 0; r < K; r++) rankOf[order[r]] = r;
    std::vector<int> rk(n, -1);
    for (int i = 0; i < n; i++) if (idx[i] >= 0) rk[i] = rankOf[idx[i]];
    idx.clear(); idx.shrink_to_fit();
    progress(0.3f);

    // --- 4. stacked layers: layer L = every pixel whose rank >= L (parallel over layers) ---
    // Layer L = pixels of rank L plus a HALO-wide band into higher-ranked neighbors. The band hides under
    // the higher layer but guarantees there is never a gap, and because layers no longer repeat each
    // other's boundaries, anti-aliased edges are never composited through several unrelated layers.
    constexpr int HALO = 2;
    struct Box { int x0, y0, x1, y1; };
    std::vector<Box> box(K, Box{w, h, -1, -1});
    long long transparentPx = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int r = rk[size_t(y) * w + x];
            if (r < 0) { transparentPx++; continue; }
            Box& b = box[r];
            b.x0 = std::min(b.x0, x); b.x1 = std::max(b.x1, x);
            b.y0 = std::min(b.y0, y); b.y1 = std::max(b.y1, y);
        }
    size_t maxCells = 1;
    for (int r = 0; r < K; r++) {
        Box& b = box[r];
        if (b.x1 < b.x0) continue;
        b.x0 = std::max(0, b.x0 - HALO); b.y0 = std::max(0, b.y0 - HALO);
        b.x1 = std::min(w - 1, b.x1 + HALO); b.y1 = std::min(h - 1, b.y1 + HALO);
        if (!(r == 0 && transparentPx == 0))
            maxCells = std::max(maxCells, size_t(b.x1 - b.x0 + 3) * size_t(b.y1 - b.y0 + 3));
    }

    std::vector<Shape> shapes(K);
#ifndef ISO_FINE
#define ISO_FINE 0.5f
#endif
    const float iso = o.fineDetails ? ISO_FINE : 0.5f;     // contour level of the smoothed layer mask
    std::atomic<int> next{0}, finished{0};
    // memory budget: ~24 bytes per cell per worker
    const size_t budget = size_t(700) << 20;
    int LT = (int)std::clamp<size_t>(budget / (maxCells * 24), 1, 4);
    LT = std::max(1, std::min({LT, T, K}));

    parallel(LT, [&](int) {
        Buf b;
        for (;;) {
            int L = next++;
            if (L >= K || cancelled()) break;

            Shape& shape = shapes[L];
            int c = order[L];
            shape.r = uint8_t(sr[c] / cnt[c]);
            shape.g = uint8_t(sg[c] / cnt[c]);
            shape.b = uint8_t(sb[c] / cnt[c]);

            const Box bx = box[L];
            if (L == 0 && transparentPx == 0) {
                // the background layer covers the whole image: no tracing needed
                Loop lp;
                lp.start = {0.f, 0.f};
                lp.segs.push_back({{float(w0), 0.f}, {float(w0), 0.f}, {float(w0), 0.f}, true});
                lp.segs.push_back({{float(w0), float(h0)}, {float(w0), float(h0)}, {float(w0), float(h0)}, true});
                lp.segs.push_back({{0.f, float(h0)}, {0.f, float(h0)}, {0.f, float(h0)}, true});
                shape.loops.push_back(std::move(lp));
                progress(0.3f + 0.7f * (++finished) / K);
                continue;
            }
            if (bx.x1 < bx.x0) { ++finished; continue; }

            const int GW = bx.x1 - bx.x0 + 3, GH = bx.y1 - bx.y0 + 3;   // padded by 1 cell on each side
            b.ensure(size_t(GW) * GH);
            float* f = b.f.data();
            float* tmp = b.tmp.data();

            // layer mask = own pixels + HALO-wide band of higher-ranked neighbors, then smoothed ([1 2 1]/4)
            auto rankAt = [&](int gx, int gy) {
                int x = bx.x0 + gx - 1, y = bx.y0 + gy - 1;
                return (x >= 0 && y >= 0 && x < w && y < h) ? rk[size_t(y) * w + x] : -1;
            };
            for (int gy = 0; gy < GH; gy++)
                for (int gx = 0; gx < GW; gx++) f[size_t(gy) * GW + gx] = rankAt(gx, gy) == L ? 1.f : 0.f;
            for (int gy = 0; gy < GH; gy++)                                  // dilate in x: f -> tmp
                for (int gx = 0; gx < GW; gx++) {
                    float mx = 0.f;
                    for (int d = -HALO; d <= HALO; d++) {
                        int xx = gx + d;
                        if (xx >= 0 && xx < GW) mx = std::max(mx, f[size_t(gy) * GW + xx]);
                    }
                    tmp[size_t(gy) * GW + gx] = mx;
                }
            for (int gy = 0; gy < GH; gy++)                                  // dilate in y: tmp -> f
                for (int gx = 0; gx < GW; gx++) {
                    float mx = 0.f;
                    for (int d = -HALO; d <= HALO; d++) {
                        int yy = gy + d;
                        if (yy >= 0 && yy < GH) mx = std::max(mx, tmp[size_t(yy) * GW + gx]);
                    }
                    f[size_t(gy) * GW + gx] = mx;
                }
            for (int gy = 0; gy < GH; gy++)
                for (int gx = 0; gx < GW; gx++) {
                    int r = rankAt(gx, gy);
                    float& v = f[size_t(gy) * GW + gx];
                    v = (r == L || (r > L && v > 0.f)) ? 1.f : 0.f;
                }
            for (int gy = 0; gy < GH; gy++) {
                const float* r = f + size_t(gy) * GW;
                float* t = tmp + size_t(gy) * GW;
                t[0] = (2.f * r[0] + r[1]) * 0.25f;
                for (int gx = 1; gx < GW - 1; gx++) t[gx] = (r[gx - 1] + 2.f * r[gx] + r[gx + 1]) * 0.25f;
                t[GW - 1] = (r[GW - 2] + 2.f * r[GW - 1]) * 0.25f;
            }
            for (int gy = 0; gy < GH; gy++) {
                const float* up = tmp + size_t(gy > 0 ? gy - 1 : 0) * GW;
                const float* mid = tmp + size_t(gy) * GW;
                const float* dn = tmp + size_t(gy < GH - 1 ? gy + 1 : GH - 1) * GW;
                float* o2 = f + size_t(gy) * GW;
                float wu = gy > 0 ? 1.f : 0.f, wd = gy < GH - 1 ? 1.f : 0.f;
                for (int gx = 0; gx < GW; gx++) o2[gx] = (wu * up[gx] + 2.f * mid[gx] + wd * dn[gx]) * 0.25f;
            }

            // marching squares at iso = 0.5, linking segments through shared cell edges
            auto link = [&](int p, int q) {
                if (b.a0[p] < 0) b.a0[p] = q; else b.a1[p] = q;
                if (b.a0[q] < 0) b.a0[q] = p; else b.a1[q] = p;
                b.touched.push_back(p);
            };
            b.touched.clear();
            for (int cy = 0; cy < GH - 1; cy++)
                for (int cx = 0; cx < GW - 1; cx++) {
                    const int c00 = cy * GW + cx;
                    float va = f[c00], vb = f[c00 + 1], vc = f[c00 + GW + 1], vd = f[c00 + GW];
                    int cs = (va >= iso) | ((vb >= iso) << 1) | ((vc >= iso) << 2) | ((vd >= iso) << 3);
                    if (cs == 0 || cs == 15) continue;
                    const int eT = 2 * c00, eL = 2 * c00 + 1, eR = 2 * (c00 + 1) + 1, eB = 2 * (c00 + GW);
                    bool center = (va + vb + vc + vd) * 0.25f >= iso;
                    switch (cs) {
                        case 1: case 14: link(eL, eT); break;
                        case 2: case 13: link(eT, eR); break;
                        case 3: case 12: link(eL, eR); break;
                        case 4: case 11: link(eR, eB); break;
                        case 6: case 9:  link(eT, eB); break;
                        case 7: case 8:  link(eL, eB); break;
                        case 5:  if (center) { link(eT, eR); link(eL, eB); } else { link(eL, eT); link(eR, eB); } break;
                        case 10: if (center) { link(eL, eT); link(eR, eB); } else { link(eT, eR); link(eL, eB); } break;
                    }
                }

            auto pos = [&](int id) -> Pt {
                int cell = id >> 1, gx = cell % GW, gy = cell / GW;
                float v0 = f[cell];
                if (id & 1) {
                    float t = (iso - v0) / (f[cell + GW] - v0);
                    return {gx + bx.x0 - 0.5f, gy + bx.y0 + t - 0.5f};
                }
                float t = (iso - v0) / (f[cell + 1] - v0);
                return {gx + bx.x0 + t - 0.5f, gy + bx.y0 - 0.5f};
            };

            for (int s : b.touched) {
                if (b.a0[s] < 0) continue;                       // already part of a traced loop
                std::vector<Pt> poly;
                int prev = -1, cur = s;
                for (;;) {
                    poly.push_back(pos(cur));
                    int n0 = b.a0[cur], n1 = b.a1[cur];
                    b.a0[cur] = b.a1[cur] = -1;
                    int nx = (n0 != prev) ? n0 : n1;
                    if (nx < 0 || nx == s) break;
                    prev = cur; cur = nx;
                }
                double area = 0;
                for (size_t i = 0; i < poly.size(); i++) {
                    const Pt& p = poly[i]; const Pt& q = poly[(i + 1) % poly.size()];
                    area += double(p.x) * q.y - double(q.x) * p.y;
                }
                area = std::fabs(area) * 0.5;
                bool keep = area >= minArea;
                if (!keep && o.fineDetails && area >= 0.3 * minArea) {
                    double per = 0;
                    for (size_t i = 0; i < poly.size(); i++) {
                        const Pt& p = poly[i]; const Pt& q = poly[(i + 1) % poly.size()];
                        per += std::hypot(double(q.x) - p.x, double(q.y) - p.y);
                    }
                    keep = per >= 10.0 && per * per > 36.0 * area;       // clearly elongated: a stroke, not dust
                }
                if (!keep) continue;
                Loop lp;
                if (buildLoop(poly, area, o, sx, sy, lp)) shape.loops.push_back(std::move(lp));
            }
            int fin = ++finished;
            progress(0.3f + 0.7f * fin / K);
        }
    });

    if (cancelled()) return out;
    for (Shape& s : shapes) if (!s.loops.empty()) out.shapes.push_back(std::move(s));
    progress(1.f);
    return out;
}
