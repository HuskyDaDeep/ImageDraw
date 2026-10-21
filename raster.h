#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "vectorizer.h"

// Software renderer (anti-aliased, even-odd fill) used for the preview and PNG export.
std::vector<uint8_t> renderRGBA(const VectorImage& v, int W, int H);
std::string toPNG(const VectorImage& v, int W, int H);
