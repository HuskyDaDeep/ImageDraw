#define GL_SILENCE_DEPRECATION
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <nfd.hpp>

#define STBI_WINDOWS_UTF8
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <string>
#include <thread>

#include "exporter.h"
#include "raster.h"
#include "vectorizer.h"

struct Preset { const char* name; Options opt; };
static const Preset kPresets[] = {
    {"Logo",         {6,  4,  0.4f, true, false, false, 2048, 1, 0, 0}},
    {"Illustration", {12, 8,  0.6f, true, false, false, 2048, 1, 0, 1}},
    {"Photo",        {24, 16, 1.0f, true, true,  false, 2048, 2, 0, 2}},
};

struct App {
    std::string path, status;
    int imgW = 0, imgH = 0;
    std::vector<uint8_t> pixels;
    GLuint srcTex = 0, vecTex = 0;

    Options opt = kPresets[1].opt;
    int preset = 1;                       // 3 = custom
    VectorImage result;
    Control ctl;

    bool hasResult = false, gotoResult = false;
    std::thread worker;
    std::atomic<bool> busy{false}, done{false}, ok{false};

    float zoom = 1.f;
    bool compare = false;
    float split = 0.5f;
    int texW = 0, texH = 0;
    bool texDirty = true;
};

static std::string g_dropped;   // set by the GLFW drop callback (main thread only)
static void onDrop(GLFWwindow*, int n, const char** paths) { if (n > 0) g_dropped = paths[0]; }

static GLuint makeTexture(GLuint old, const uint8_t* rgba, int w, int h) {
    if (old) glDeleteTextures(1, &old);
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    return t;
}

static void showFitted(GLuint tex, int w, int h, ImVec2 avail) {
    if (!tex || avail.x < 1 || avail.y < 1) return;
    float s = std::min(avail.x / w, avail.y / h);
    ImVec2 size(w * s, h * s);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail.x - size.x) * 0.5f);
    ImGui::Image((ImTextureID)(intptr_t)tex, size);
}

// Plain download icon (arrow + tray), drawn with lines: no font or image needed.
static bool downloadIconButton(const char* id, const char* tip) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::Button(id, ImVec2(40, 40));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
    float cx = p.x + 20;
    dl->AddLine({cx, p.y + 9},  {cx, p.y + 23}, col, 2.f);
    dl->AddLine({cx - 7, p.y + 17}, {cx, p.y + 24}, col, 2.f);
    dl->AddLine({cx + 7, p.y + 17}, {cx, p.y + 24}, col, 2.f);
    dl->AddLine({p.x + 10, p.y + 30}, {p.x + 30, p.y + 30}, col, 2.f);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    return clicked;
}

static void loadImageFile(App& a, const std::string& path) {
    int w, h, c;
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &c, 4);
    if (!data) { a.status = "Could not open this image."; return; }
    a.pixels.assign(data, data + size_t(w) * h * 4);
    stbi_image_free(data);
    a.imgW = w; a.imgH = h; a.path = path;
    a.srcTex = makeTexture(a.srcTex, a.pixels.data(), w, h);
    a.hasResult = false;
    a.status.clear();
}

static void chooseImage(App& a) {
    NFD::UniquePath out;
    nfdfilteritem_t filter[1] = {{"Images", "png,jpg,jpeg,bmp,tga,gif"}};
    if (NFD::OpenDialog(out, filter, 1) == NFD_OKAY) loadImageFile(a, out.get());
}

static void startConvert(App& a) {
    if (a.worker.joinable()) a.worker.join();
    a.ctl.progress = 0.f;
    a.ctl.cancel = false;
    a.busy = true;
    a.worker = std::thread([&a] {
        VectorImage r = vectorize(a.pixels.data(), a.imgW, a.imgH, a.opt, &a.ctl);
        a.ok = !a.ctl.cancel.load();
        if (a.ok) a.result = std::move(r);
        a.done = true;
    });
}

static void saveAs(App& a, const char* ext, const char* defName, const std::function<std::string()>& make) {
    NFD::UniquePath out;
    nfdfilteritem_t filter[1] = {{"Export", ext}};
    if (NFD::SaveDialog(out, filter, 1, nullptr, defName) != NFD_OKAY) return;
    a.status = writeFile(out.get(), make()) ? std::string("Saved: ") + out.get() : "Could not save the file.";
}

static void ensureVectorTexture(App& a, int tw, int th) {
    if (!a.texDirty && tw == a.texW && th == a.texH) return;
    std::vector<uint8_t> px = renderRGBA(a.result, tw, th);
    a.vecTex = makeTexture(a.vecTex, px.data(), tw, th);
    a.texW = tw; a.texH = th; a.texDirty = false;
}

int main() {
    if (!glfwInit()) return 1;
    GLFWwindow* win = glfwCreateWindow(1100, 760, "Image to Vector", nullptr, nullptr);
    if (!win) return 1;
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);
    glfwSetDropCallback(win, onDrop);

    NFD::Guard nfdGuard;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsLight();
    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init("#version 130");   // macOS: "#version 150"

    App app;
    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();

        if (!g_dropped.empty()) {
            if (!app.busy) loadImageFile(app, g_dropped);
            g_dropped.clear();
        }
        if (app.done.exchange(false)) {
            app.worker.join();
            app.busy = false;
            if (app.ok) {
                app.hasResult = true;
                app.gotoResult = true;
                app.texDirty = true;
                app.zoom = 1.f;
                app.status = "Done.";
            } else {
                app.status = "Cancelled.";
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        ImGuiIO& io = ImGui::GetIO();

        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings);

        if (ImGui::BeginTabBar("tabs")) {
            // ---------------- Tab 1: choose image ----------------
            if (ImGui::BeginTabItem("1. Choose image")) {
                ImGui::BeginChild("controls", ImVec2(300, 0), true);
                ImGui::BeginDisabled(app.busy);
                if (ImGui::Button("Select image...", ImVec2(-1, 32))) chooseImage(app);
                ImGui::TextDisabled("or drop an image on the window");
                ImGui::TextWrapped("%s", app.path.empty() ? "No image selected." : app.path.c_str());
                if (app.imgW) ImGui::Text("Size: %d x %d px", app.imgW, app.imgH);
                ImGui::Separator();

                ImGui::Text("Preset");
                static const char* names[] = {"Logo", "Illustration", "Photo", "Custom"};
                if (ImGui::Combo("##preset", &app.preset, names, 4) && app.preset < 3)
                    app.opt = kPresets[app.preset].opt;

                bool changed = false;
                changed |= ImGui::SliderInt("Colors", &app.opt.colors, 2, 32);
                changed |= ImGui::SliderInt("Ignore specks", &app.opt.minArea, 0, 200);
                changed |= ImGui::SliderFloat("Simplify", &app.opt.tolerance, 0.1f, 5.0f, "%.1f");
                changed |= ImGui::SliderInt("Clean edges", &app.opt.cleanup, 0, 3);
                changed |= ImGui::SliderInt("Shading detail", &app.opt.shading, 0, 4);
                static const char* texNames[] = {"Off", "Light", "Strong"};
                changed |= ImGui::Combo("Texture", &app.opt.texture, texNames, 3);
                changed |= ImGui::SliderInt("Max size (px)", &app.opt.maxSide, 512, 4096);
                changed |= ImGui::Checkbox("Smooth curves", &app.opt.smooth);
                changed |= ImGui::Checkbox("Keep fine details", &app.opt.fineDetails);
                changed |= ImGui::Checkbox("Reduce noise", &app.opt.denoise);
                changed |= ImGui::Checkbox("Remove background", &app.opt.removeBg);
                if (changed) app.preset = 3;
                ImGui::EndDisabled();
                ImGui::Separator();

                if (app.busy) {
                    ImGui::ProgressBar(app.ctl.progress.load(), ImVec2(-1, 24));
                    if (ImGui::Button("Cancel", ImVec2(-1, 30))) app.ctl.cancel = true;
                } else {
                    ImGui::BeginDisabled(app.pixels.empty());
                    if (ImGui::Button("Convert to vector", ImVec2(-1, 36))) startConvert(app);
                    ImGui::EndDisabled();
                }
                if (!app.status.empty()) ImGui::TextWrapped("%s", app.status.c_str());
                ImGui::EndChild();

                ImGui::SameLine();
                ImGui::BeginChild("source", ImVec2(0, 0), true);
                if (app.srcTex) showFitted(app.srcTex, app.imgW, app.imgH, ImGui::GetContentRegionAvail());
                else ImGui::TextUnformatted("Your image will appear here.");
                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            // ---------------- Tab 2: result ----------------
            ImGuiTabItemFlags flags = app.gotoResult ? ImGuiTabItemFlags_SetSelected : 0;
            app.gotoResult = false;
            if (ImGui::BeginTabItem("2. Result", nullptr, flags)) {
                if (!app.hasResult) {
                    ImGui::TextUnformatted("No result yet. Choose an image and press \"Convert to vector\".");
                } else {
                    // toolbar
                    if (ImGui::Button("Zoom -")) app.zoom = std::max(1.f, app.zoom / 1.5f);
                    ImGui::SameLine();
                    if (ImGui::Button("Zoom +")) app.zoom = std::min(16.f, app.zoom * 1.5f);
                    ImGui::SameLine();
                    if (ImGui::Button("Fit")) app.zoom = 1.f;
                    ImGui::SameLine();
                    ImGui::Text("%d%%", int(app.zoom * 100));
                    ImGui::SameLine();
                    ImGui::Checkbox("Compare with original", &app.compare);
                    if (app.compare) {
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(220);
                        ImGui::SliderFloat("##split", &app.split, 0.f, 1.f, "");
                    }

                    // preview (real render of the vector)
                    ImGui::BeginChild("preview", ImVec2(0, -56), true, ImGuiWindowFlags_HorizontalScrollbar);
                    ImVec2 avail = ImGui::GetContentRegionAvail();
                    avail.x -= 4; avail.y -= 4;
                    const float rw = (float)app.result.width, rh = (float)app.result.height;
                    float fit = std::min(avail.x / rw, avail.y / rh);
                    ImVec2 disp(rw * fit * app.zoom, rh * fit * app.zoom);
                    if (disp.x < 8 || disp.y < 8) disp = ImVec2(8, 8);

                    float k = std::min(1.f, 3000.f / std::max(disp.x, disp.y));
                    ensureVectorTexture(app, std::max(16, int(disp.x * k)), std::max(16, int(disp.y * k)));

                    if (disp.x < avail.x) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail.x - disp.x) * 0.5f);
                    if (disp.y < avail.y) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (avail.y - disp.y) * 0.5f);
                    ImGui::Image((ImTextureID)(intptr_t)app.vecTex, disp);
                    ImVec2 a0 = ImGui::GetItemRectMin(), a1 = ImGui::GetItemRectMax();

                    if (app.compare && app.srcTex) {
                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        float sx = a0.x + (a1.x - a0.x) * app.split;
                        dl->PushClipRect(a0, ImVec2(sx, a1.y), true);
                        dl->AddImage((ImTextureID)(intptr_t)app.srcTex, a0, a1);
                        dl->PopClipRect();
                        dl->AddLine(ImVec2(sx, a0.y), ImVec2(sx, a1.y), IM_COL32(0, 0, 0, 255), 2.f);
                        dl->AddText(ImVec2(a0.x + 6, a0.y + 4), IM_COL32(0, 0, 0, 255), "Original");
                        dl->AddText(ImVec2(a1.x - 48, a0.y + 4), IM_COL32(0, 0, 0, 255), "Vector");
                    }
                    if (ImGui::IsWindowHovered()) {
                        if (io.KeyCtrl && io.MouseWheel != 0.f)
                            app.zoom = std::clamp(app.zoom * std::pow(1.1f, io.MouseWheel), 1.f, 16.f);
                        if (app.zoom > 1.f && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.f)) {
                            ImGui::SetScrollX(ImGui::GetScrollX() - io.MouseDelta.x);
                            ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseDelta.y);
                        }
                    }
                    ImGui::EndChild();

                    // bottom bar
                    size_t loops = 0;
                    for (auto& s : app.result.shapes) loops += s.loops.size();
                    ImGui::AlignTextToFramePadding();
                    ImGui::Text("%zu layers, %zu paths", app.result.shapes.size(), loops);
                    if (!app.status.empty()) {
                        ImGui::SameLine(); ImGui::TextUnformatted("|");
                        ImGui::SameLine(); ImGui::TextUnformatted(app.status.c_str());
                    }

                    ImGui::SameLine(ImGui::GetWindowWidth() - 60);
                    if (downloadIconButton("##export", "Download / Share")) ImGui::OpenPopup("export_menu");
                    if (ImGui::BeginPopup("export_menu")) {
                        const int W = app.result.width, H = app.result.height;
                        if (ImGui::MenuItem("Save as SVG..."))
                            saveAs(app, "svg", "vector.svg", [&] { return toSVG(app.result); });
                        if (ImGui::MenuItem("Save as PDF..."))
                            saveAs(app, "pdf", "vector.pdf", [&] { return toPDF(app.result); });
                        if (ImGui::MenuItem("Save as EPS..."))
                            saveAs(app, "eps", "vector.eps", [&] { return toEPS(app.result); });
                        if (ImGui::MenuItem("Save as PNG (original size)..."))
                            saveAs(app, "png", "image.png", [&] { return toPNG(app.result, W, H); });
                        if (ImGui::MenuItem("Save as PNG (2x)..."))
                            saveAs(app, "png", "image@2x.png", [&] { return toPNG(app.result, W * 2, H * 2); });
                        ImGui::Separator();
                        if (ImGui::MenuItem("Copy SVG code to clipboard")) {
                            ImGui::SetClipboardText(toSVG(app.result).c_str());
                            app.status = "SVG copied to clipboard.";
                        }
                        ImGui::EndPopup();
                    }
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::End();

        ImGui::Render();
        int fw, fh;
        glfwGetFramebufferSize(win, &fw, &fh);
        glViewport(0, 0, fw, fh);
        glClearColor(1, 1, 1, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(win);
    }

    app.ctl.cancel = true;
    if (app.worker.joinable()) app.worker.join();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
