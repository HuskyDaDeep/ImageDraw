#include "exporter.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {

std::string num(float f) {
    char b[32];
    std::snprintf(b, sizeof b, "%.2f", f);
    return b;
}

// prefix=true  -> "M x y"   (SVG)
// prefix=false -> "x y m"   (PostScript / PDF)
std::string emit(const Shape& s, bool prefix, const char* M, const char* L, const char* C, const char* Z) {
    std::string r;
    auto put = [&](const char* op, std::initializer_list<float> v) {
        std::string nums;
        for (float f : v) nums += (nums.empty() ? "" : " ") + num(f);
        if (prefix) r += std::string(op) + (nums.empty() ? "" : " " + nums) + " ";
        else        r += (nums.empty() ? "" : nums + " ") + std::string(op) + "\n";
    };
    for (const Loop& l : s.loops) {
        put(M, {l.start.x, l.start.y});
        for (const Seg& g : l.segs) {
            if (g.line) put(L, {g.p.x, g.p.y});
            else        put(C, {g.c1.x, g.c1.y, g.c2.x, g.c2.y, g.p.x, g.p.y});
        }
        put(Z, {});
    }
    return r;
}

std::string hex(const Shape& s) {
    char b[8];
    std::snprintf(b, sizeof b, "#%02x%02x%02x", s.r, s.g, s.b);
    return b;
}

std::string rgb01(const Shape& s) {
    return num(s.r / 255.f) + " " + num(s.g / 255.f) + " " + num(s.b / 255.f);
}

} // namespace

std::string toSVG(const VectorImage& v) {
    std::ostringstream o;
    o << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << v.width << "\" height=\"" << v.height
      << "\" viewBox=\"0 0 " << v.width << " " << v.height << "\">\n";
    for (const Shape& s : v.shapes)
        o << "<path fill=\"" << hex(s) << "\" fill-rule=\"evenodd\" d=\"" << emit(s, true, "M", "L", "C", "Z")
          << "\"/>\n";
    o << "</svg>\n";
    return o.str();
}

std::string toEPS(const VectorImage& v) {
    std::ostringstream o;
    o << "%!PS-Adobe-3.0 EPSF-3.0\n%%BoundingBox: 0 0 " << v.width << " " << v.height
      << "\n%%EndComments\n0 " << v.height << " translate\n1 -1 scale\n";
    for (const Shape& s : v.shapes)
        o << rgb01(s) << " setrgbcolor\nnewpath\n"
          << emit(s, false, "moveto", "lineto", "curveto", "closepath") << "eofill\n";
    o << "showpage\n%%EOF\n";
    return o.str();
}

std::string toPDF(const VectorImage& v) {
    std::string content = "1 0 0 -1 0 " + std::to_string(v.height) + " cm\n";
    for (const Shape& s : v.shapes)
        content += rgb01(s) + " rg\n" + emit(s, false, "m", "l", "c", "h") + "f*\n";

    std::string out = "%PDF-1.4\n";
    std::vector<size_t> off;
    auto obj = [&](const std::string& body) {
        off.push_back(out.size());
        out += std::to_string(off.size()) + " 0 obj\n" + body + "\nendobj\n";
    };
    obj("<< /Type /Catalog /Pages 2 0 R >>");
    obj("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    obj("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " + std::to_string(v.width) + " " +
        std::to_string(v.height) + "] /Contents 4 0 R >>");
    obj("<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "\nendstream");

    size_t xr = out.size();
    out += "xref\n0 5\n0000000000 65535 f \n";
    for (size_t o : off) {
        char b[32];
        std::snprintf(b, sizeof b, "%010zu 00000 n \n", o);
        out += b;
    }
    out += "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n" + std::to_string(xr) + "\n%%EOF\n";
    return out;
}

bool writeFile(const std::string& path, const std::string& data) {
    std::ofstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) return false;
    f.write(data.data(), (std::streamsize)data.size());
    return bool(f);
}
