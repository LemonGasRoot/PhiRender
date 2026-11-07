// hud.cpp - render the Phigros-style HUD into a transparent upload texture.
#include "hud.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace hud {

#ifdef _WIN32
namespace {
std::wstring utf8Wide(const std::string& value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), (int)value.size(), nullptr, 0);
    if (count <= 0) return std::wstring(value.begin(), value.end());
    std::wstring out((size_t)count, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), (int)value.size(), out.data(), count);
    return out;
}

HFONT makeFont(const std::string& family, int px) {
    const std::wstring face = utf8Wide(family.empty() ? "Saira" : family);
    return CreateFontW(-px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, face.c_str());
}

void centeredText(HDC dc, const std::wstring& text, HFONT font, int left, int top, int right,
                  int bottom, UINT align) {
    if (text.empty() || !font) return;
    HGDIOBJ old = SelectObject(dc, font);
    RECT rect{left, top, right, bottom};
    DrawTextW(dc, text.c_str(), (int)text.size(), &rect,
              DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | align);
    SelectObject(dc, old);
}

void spacedText(HDC dc, const std::wstring& text, HFONT font, int left, int top, int right,
                int bottom, float gap, bool alignRight) {
    if (text.empty() || !font) return;
    HGDIOBJ old = SelectObject(dc, font);
    std::vector<SIZE> sizes(text.size());
    float totalWidth = 0.0f;
    int textHeight = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        GetTextExtentPoint32W(dc, &text[i], 1, &sizes[i]);
        totalWidth += sizes[i].cx;
        textHeight = std::max(textHeight, (int)sizes[i].cy);
    }
    if (text.size() > 1) totalWidth += gap * (float)(text.size() - 1);
    float x = alignRight ? (float)right - totalWidth
                         : (float)left + ((float)(right - left) - totalWidth) / 2.0f;
    const int y = top + std::max(0, (bottom - top - textHeight) / 2);
    for (size_t i = 0; i < text.size(); ++i) {
        TextOutW(dc, (int)std::lround(x), y, &text[i], 1);
        x += sizes[i].cx + gap;
    }
    SelectObject(dc, old);
}

void fillWhiteRect(HDC dc, int left, int top, int right, int bottom) {
    RECT rect{left, top, right, bottom};
    HBRUSH brush = (HBRUSH)GetStockObject(WHITE_BRUSH);
    FillRect(dc, &rect, brush);
}
} // namespace
#endif

Overlay::~Overlay() { destroy(); }

void Overlay::setFonts(const std::string& gameplayPath, const std::string& gameplayFamily,
                       const std::string& infoPath, const std::string& infoFamily) {
    gameplayPath_ = gameplayPath;
    gameplayFamily_ = gameplayFamily.empty() ? "Saira" : gameplayFamily;
    infoPath_ = infoPath;
    infoFamily_ = infoFamily.empty() ? "Saira" : infoFamily;
}

bool Overlay::resize(int width, int height, std::string* error) {
    destroy();
    if (width <= 0 || height <= 0) {
        if (error) *error = "invalid HUD size";
        return false;
    }
#ifdef _WIN32
    width_ = width;
    height_ = height;
    pixels_.resize((size_t)width_ * (size_t)height_ * 4u);

    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width_;
    bmi.bmiHeader.biHeight = -height_; // top-down rows
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!dc || !bitmap || !bits) {
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        width_ = height_ = 0;
        pixels_.clear();
        if (error) *error = "could not create the HUD text surface";
        return false;
    }
    dc_ = dc;
    bitmap_ = bitmap;
    dibBits_ = bits;
    previousBitmap_ = SelectObject(dc, bitmap);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));

    if (!gameplayPath_.empty()) {
        gameplayFontRegistered_ = AddFontResourceExW(utf8Wide(gameplayPath_).c_str(), FR_PRIVATE, nullptr) > 0;
        if (!gameplayFontRegistered_) {
            if (error) *error = "could not load gameplay font: " + gameplayPath_;
            destroy();
            return false;
        }
    }
    if (!infoPath_.empty()) {
        infoFontRegistered_ = AddFontResourceExW(utf8Wide(infoPath_).c_str(), FR_PRIVATE, nullptr) > 0;
        if (!infoFontRegistered_) {
            if (error) *error = "could not load song-info font: " + infoPath_;
            destroy();
            return false;
        }
    }

    const float scale = std::min((float)width_ / 1920.0f, (float)height_ / 1080.0f);
    // Match Phigros' compact AUTOPLAY label while keeping its combo and score
    // at the regular gameplay HUD size.
    gameplayFont_ = makeFont(gameplayFamily_, std::max(12, (int)std::lround(58.0f * scale)));
    comboFont_ = makeFont(gameplayFamily_, std::max(12, (int)std::lround(76.0f * scale)));
    labelFont_ = makeFont(gameplayFamily_, std::max(12, (int)std::lround(29.0f * scale)));
    scoreFont_ = makeFont(gameplayFamily_, std::max(12, (int)std::lround(61.0f * scale)));
    infoFont_ = makeFont(infoFamily_, std::max(12, (int)std::lround(39.0f * scale)));
    levelFont_ = makeFont(infoFamily_, std::max(12, (int)std::lround(39.0f * scale)));
    if (!gameplayFont_ || !comboFont_ || !labelFont_ || !scoreFont_ || !infoFont_ || !levelFont_) {
        if (error) *error = "could not create HUD fonts";
        destroy();
        return false;
    }
    return true;
#else
    (void)width;
    (void)height;
    if (error) *error = "HUD text rendering is currently supported on Windows only";
    return false;
#endif
}

bool Overlay::render(const Chart& chart, int combo, unsigned int score, std::string* error) {
#ifdef _WIN32
    if (!dc_ || !dibBits_ || width_ <= 0 || height_ <= 0) {
        if (error) *error = "HUD surface is not initialized";
        return false;
    }
    const size_t bytes = (size_t)width_ * (size_t)height_ * 4u;
    std::memset(dibBits_, 0, bytes);

    const float sx = (float)width_ / 1920.0f;
    const float sy = (float)height_ / 1080.0f;
    auto X = [sx](float v) { return (int)std::lround(v * sx); };
    auto Y = [sy](float v) { return (int)std::lround(v * sy); };

    // Pause glyph: two narrow white bars at the top-left.
    fillWhiteRect((HDC)dc_, X(36), Y(38), X(50), Y(84));
    fillWhiteRect((HDC)dc_, X(65), Y(38), X(79), Y(84));

    const std::wstring comboText = utf8Wide(std::to_string(std::max(0, combo)));
    spacedText((HDC)dc_, comboText, (HFONT)comboFont_, X(679), Y(8), X(1239), Y(82), 0.0f, false);
    spacedText((HDC)dc_, L"AUTOPLAY", (HFONT)labelFont_, X(680), Y(83), X(1240), Y(130), 0.5f, false);
    std::ostringstream scoreStream;
    scoreStream << std::setw(7) << std::setfill('0') << score;
    spacedText((HDC)dc_, utf8Wide(scoreStream.str()), (HFONT)scoreFont_, X(1460), Y(13), X(1891), Y(82), 0.17f, true);
    centeredText((HDC)dc_, utf8Wide(chart.songName), (HFONT)infoFont_, X(45), Y(992), X(917), Y(1068), DT_LEFT);
    centeredText((HDC)dc_, utf8Wide(chart.difficulty), (HFONT)levelFont_, X(1000), Y(992), X(1876), Y(1068), DT_RIGHT);

    // GDI writes coverage into the colour channels of the 32-bit DIB. Convert
    // that coverage to RGBA so the GPU's normal alpha blend composites cleanly.
    const unsigned char* src = (const unsigned char*)dibBits_;
    for (size_t i = 0; i < bytes; i += 4) {
        const unsigned char coverage = std::max(src[i], std::max(src[i + 1], src[i + 2]));
        pixels_[i] = pixels_[i + 1] = pixels_[i + 2] = 255;
        pixels_[i + 3] = coverage;
    }
    return true;
#else
    (void)chart;
    (void)combo;
    (void)score;
    if (error) *error = "HUD text rendering is currently supported on Windows only";
    return false;
#endif
}

void Overlay::destroy() {
#ifdef _WIN32
    if (dc_ && previousBitmap_) SelectObject((HDC)dc_, previousBitmap_);
    if (gameplayFont_) DeleteObject((HFONT)gameplayFont_);
    if (comboFont_) DeleteObject((HFONT)comboFont_);
    if (labelFont_) DeleteObject((HFONT)labelFont_);
    if (scoreFont_) DeleteObject((HFONT)scoreFont_);
    if (infoFont_) DeleteObject((HFONT)infoFont_);
    if (levelFont_) DeleteObject((HFONT)levelFont_);
    if (bitmap_) DeleteObject((HBITMAP)bitmap_);
    if (dc_) DeleteDC((HDC)dc_);
    if (gameplayFontRegistered_) RemoveFontResourceExW(utf8Wide(gameplayPath_).c_str(), FR_PRIVATE, nullptr);
    if (infoFontRegistered_) RemoveFontResourceExW(utf8Wide(infoPath_).c_str(), FR_PRIVATE, nullptr);
    dc_ = bitmap_ = previousBitmap_ = dibBits_ = nullptr;
    gameplayFont_ = comboFont_ = labelFont_ = scoreFont_ = infoFont_ = levelFont_ = nullptr;
    gameplayFontRegistered_ = infoFontRegistered_ = false;
#endif
    width_ = height_ = 0;
    pixels_.clear();
}

} // namespace hud
