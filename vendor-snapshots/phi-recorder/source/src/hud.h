// hud.h - lightweight Windows text overlay for the rendered chart.
#pragma once

#include "chart.h"

#include <string>
#include <vector>

namespace hud {

class Overlay {
public:
    Overlay() = default;
    ~Overlay();

    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    bool resize(int width, int height, std::string* error);
    void setFonts(const std::string& gameplayPath, const std::string& gameplayFamily,
                  const std::string& infoPath, const std::string& infoFamily);
    bool render(const Chart& chart, int combo, unsigned int score, std::string* error);
    const std::vector<unsigned char>& pixels() const { return pixels_; }
    void destroy();

private:
    int width_ = 0;
    int height_ = 0;
    std::string gameplayPath_;
    std::string gameplayFamily_ = "Saira";
    std::string infoPath_;
    std::string infoFamily_ = "Saira";
    std::vector<unsigned char> pixels_;
#ifdef _WIN32
    void* dc_ = nullptr;
    void* bitmap_ = nullptr;
    void* previousBitmap_ = nullptr;
    void* dibBits_ = nullptr;
    void* gameplayFont_ = nullptr;
    void* comboFont_ = nullptr;
    void* labelFont_ = nullptr;
    void* scoreFont_ = nullptr;
    void* infoFont_ = nullptr;
    void* levelFont_ = nullptr;
    bool gameplayFontRegistered_ = false;
    bool infoFontRegistered_ = false;
#endif
};

} // namespace hud
