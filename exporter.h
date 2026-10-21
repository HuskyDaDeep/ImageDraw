#pragma once
#include <string>
#include "vectorizer.h"

std::string toSVG(const VectorImage& v);
std::string toPDF(const VectorImage& v);
std::string toEPS(const VectorImage& v);
bool writeFile(const std::string& path, const std::string& data);
