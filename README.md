<div align="center">

# ImageDraw

### Desktop application for converting raster images into vector graphics

[![C++](https://img.shields.io/badge/C%2B%2B-17-00599C?style=for-the-badge&logo=cplusplus&logoColor=white)](https://isocpp.org/)
[![CMake](https://img.shields.io/badge/CMake-Build-064F8C?style=for-the-badge&logo=cmake&logoColor=white)](https://cmake.org/)
[![OpenGL](https://img.shields.io/badge/OpenGL-Graphics-5586A4?style=for-the-badge&logo=opengl&logoColor=white)](https://www.opengl.org/)

ImageDraw is a native C++ desktop app designed to convert raster images into vector artwork, preview the result, and export it to common vector formats.

[Source code](https://github.com/HuskyDaDeep/Image-to-vector)

</div>

---

## About the app

ImageDraw explores desktop application development in C++, combining a graphical interface with image processing and vector output. The app is intended to let users load an image, adjust tracing options, preview the result, and export the generated artwork.

## Features

- Load a source image.
- Adjust conversion options such as color count, speck filtering, path simplification, and curve smoothing.
- Preview the vector result.
- Export to SVG, PDF, or EPS.
- Copy generated SVG markup.
- Keep conversion work off the main UI thread where supported by the implementation.

## Tech stack

- **C++17** — application and image-processing logic.
- **CMake** — configure and build the app.
- **GLFW** — window and input handling.
- **Dear ImGui** — graphical user interface.
- **OpenGL** — rendering.
- **stb_image** — image loading.
- **nativefiledialog-extended** — native file dialogs.

## Build from source

### Requirements

- C++17-compatible compiler.
- CMake 3.20 or newer.
- Git.
- OpenGL development libraries and platform-specific build tools.

### 1. Clone

```bash
git clone https://github.com/HuskyDaDeep/Image-to-vector.git
cd Image-to-vector
```

### 2. Configure

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```

### 3. Build

```bash
cmake --build build --config Release
```

CMake may download project dependencies during configuration, depending on the repository's `CMakeLists.txt` setup.

### 4. Run the app

The executable target is named `img2vec`. Its location depends on the platform and CMake generator. Common locations include `build/img2vec` on single-configuration generators or `build/Release/img2vec.exe` on Windows multi-configuration builds.

## Typical workflow

1. Build and launch ImageDraw.
2. Open an image.
3. Adjust the conversion settings.
4. Start vectorization.
5. Review the preview.
6. Export the result.

## Project structure

```text
Image-to-vector/
├── CMakeLists.txt
├── main.cpp
├── vectorizer.h
├── vectorizer.cpp
├── exporter.h
└── exporter.cpp
```

File names and structure may change as the project evolves.

## Project status

ImageDraw is a **buildable desktop application project**. This repository documents the source code and build process; it does not imply that a prebuilt installer or hosted browser version is available.

## Author

**Samuel Resende Campos**

- GitHub: [@HuskyDaDeep](https://github.com/HuskyDaDeep)
- Repository: [Image-to-vector](https://github.com/HuskyDaDeep/Image-to-vector)
