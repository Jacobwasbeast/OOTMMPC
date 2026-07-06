#pragma once

// Win32 + Direct3D 11 + Dear ImGui host for the launcher GUI. One window/swap chain: config stage
// = ImGui fills the client area; runtime stage = embedded game covers the left, ImGui sidebar right.

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d11.h>

#include <string>
#include <unordered_map>

struct ImDrawList;
struct ImVec2;

namespace ootmm::launcher {

// Accent palettes for the two games; "auto" follows the active game at runtime.
enum class Accent {
    OotGold,
    MmViolet,
};

// A REAL game icon decoded from the ports' o2r archives (see tools/gen_launcher_icons.py),
// uploaded as a D3D11 texture. `id` plugs straight into ImGui::Image.
struct IconTexture {
    void* id = nullptr; // ImTextureID (shader resource view)
    int width = 0;
    int height = 0;
};

class UiHost {
  public:
    bool Init(HWND window);
    void Shutdown();

    // Embedded game-icon lookup by generator name (e.g. "OcarinaOfTime"); nullptr when
    // the icon is unknown or texture creation failed.
    [[nodiscard]] const IconTexture* Icon(const std::string& name) const;

    // Forward window messages to ImGui; returns true when ImGui consumed the message.
    bool HandleMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    void OnResize(UINT width, UINT height);

    void BeginFrame();
    void EndFrame();

    void ApplyTheme(Accent accent, float scale);

    [[nodiscard]] bool Ready() const { return device_ != nullptr; }

  private:
    void CreateRenderTarget();
    void ReleaseRenderTarget();
    void CreateIconTextures();
    void ReleaseIconTextures();

    HWND window_ = nullptr;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    IDXGISwapChain* swapChain_ = nullptr;
    ID3D11RenderTargetView* renderTarget_ = nullptr;
    UINT pendingWidth_ = 0;
    UINT pendingHeight_ = 0;
    float appliedScale_ = 0.0f;
    int appliedAccent_ = -1;
    std::unordered_map<std::string, IconTexture> icons_;
};

// ---------------------------------------------------------------------------
// Decorative drawing (Zelda-styled, all procedural — no copyrighted art shipped)
// ---------------------------------------------------------------------------

// Golden Triforce emblem with a soft glow; glow01 in [0,1] drives the pulse.
void DrawTriforce(ImDrawList* draw, const ImVec2& center, float size, unsigned int color, float glow01);

// L-shaped gold corner brackets (the classic Zelda menu frame) around a rect.
void DrawCornerBrackets(ImDrawList* draw, const ImVec2& min, const ImVec2& max, unsigned int color, float length,
                        float thickness);

// Drifting fairy-dust motes + top glow + vignette over the given rect. Call once per
// frame with ImGui::GetTime(); the particle field is stateless (derived from time).
void DrawFairyDust(ImDrawList* draw, const ImVec2& min, const ImVec2& max, unsigned int accent, double time);

// Native Win32 open-file dialog. `filter` uses the Win32 double-NUL syntax, e.g.
// L"OoTMM seed (*.json)\0*.json\0All files (*.*)\0*.*\0". Empty result = cancelled.
[[nodiscard]] std::wstring OpenFileDialog(HWND owner, const wchar_t* title, const wchar_t* filter);

} // namespace ootmm::launcher

#endif // _WIN32
