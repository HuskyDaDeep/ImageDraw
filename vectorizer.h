#pragma once
#include <atomic>
#include <cstdint>
#include <vector>

struct Pt { float x, y; };

// One closed outline = a start point + a chain of segments (line or cubic Bezier).
struct Seg  { Pt c1, c2, p; bool line; };
struct Loop { Pt start; std::vector<Seg> segs; };

struct Shape {
    uint8_t r, g, b;
    std::vector<Loop> loops;              // outer contours and holes (even-odd fill)
};

struct Options {
    int   colors    = 12;     // palette size
    int   minArea   = 8;      // ignore specks smaller than this (px^2)
    float tolerance = 1.0f;   // path simplification (px)
    bool  smooth    = true;   // fit curves instead of straight polygons
    bool  denoise   = false;  // light blur before color quantization
    bool  removeBg  = false;  // make the background (border-connected color) transparent
    int   maxSide   = 2048;   // larger images are processed at this size, output keeps original size
    int   cleanup   = 1;      // edge cleanup passes (0 = off, max 3): removes 1px slivers from anti-aliasing
    int   texture   = 0;      // 0 = off, 1 = light, 2 = strong: simplify grunge / halftone / speckle textures
    int   shading   = 0;      // 0..4: extra tones added where colors vary smoothly (0 = off); stops when uniform
    bool  fineDetails = true; // keep hairlines, thin strokes and small marks (otherwise they are treated as noise)
};

// Shared with the UI thread.
struct Control {
    std::atomic<float> progress{0.f};
    std::atomic<bool>  cancel{false};
};

struct VectorImage {
    int width = 0, height = 0;            // original image size (SVG units)
    std::vector<Shape> shapes;            // drawn in order, back to front
};

VectorImage vectorize(const uint8_t* rgba, int w, int h, const Options& opt, Control* ctl = nullptr);
