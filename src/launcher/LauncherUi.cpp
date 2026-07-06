#ifdef _WIN32

#include "LauncherUi.hpp"

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

#include <commdlg.h>

#include <cmath>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace ootmm::launcher {

namespace {

struct EmbeddedIcon {
    const char* name;
    int width;
    int height;
    const unsigned char* pixels; // RGBA8
};

#include "LauncherIcons.inc"

} // namespace

bool UiHost::Init(HWND window) {
    window_ = window;

    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 2;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferDesc.RefreshRate.Numerator = 60;
    desc.BufferDesc.RefreshRate.Denominator = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = window;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    // Blt-model presentation layers with the embedded game child HWND; flip-model does not.
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL createdLevel{};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                               D3D11_SDK_VERSION, &desc, &swapChain_, &device_, &createdLevel,
                                               &context_);
    if (FAILED(hr)) {
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                           &desc, &swapChain_, &device_, &createdLevel, &context_);
    }
    if (FAILED(hr)) {
        return false;
    }
    CreateRenderTarget();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // window layout is fixed; don't scatter imgui.ini files
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(device_, context_);
    CreateIconTextures();
    return true;
}

void UiHost::CreateIconTextures() {
    for (const EmbeddedIcon& icon : kEmbeddedIcons) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(icon.width);
        desc.Height = static_cast<UINT>(icon.height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA data{};
        data.pSysMem = icon.pixels;
        data.SysMemPitch = static_cast<UINT>(icon.width * 4);

        ID3D11Texture2D* texture = nullptr;
        if (FAILED(device_->CreateTexture2D(&desc, &data, &texture)) || texture == nullptr) {
            continue;
        }
        ID3D11ShaderResourceView* view = nullptr;
        const HRESULT hr = device_->CreateShaderResourceView(texture, nullptr, &view);
        texture->Release();
        if (FAILED(hr) || view == nullptr) {
            continue;
        }
        icons_[icon.name] = IconTexture{ view, icon.width, icon.height };
    }
}

void UiHost::ReleaseIconTextures() {
    for (auto& [name, icon] : icons_) {
        if (icon.id != nullptr) {
            static_cast<ID3D11ShaderResourceView*>(icon.id)->Release();
        }
    }
    icons_.clear();
}

const IconTexture* UiHost::Icon(const std::string& name) const {
    const auto it = icons_.find(name);
    return it != icons_.end() ? &it->second : nullptr;
}

void UiHost::Shutdown() {
    if (device_ == nullptr) {
        return;
    }
    ReleaseIconTextures();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    ReleaseRenderTarget();
    if (swapChain_ != nullptr) {
        swapChain_->Release();
        swapChain_ = nullptr;
    }
    if (context_ != nullptr) {
        context_->Release();
        context_ = nullptr;
    }
    if (device_ != nullptr) {
        device_->Release();
        device_ = nullptr;
    }
}

bool UiHost::HandleMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (device_ == nullptr) {
        return false;
    }
    return ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam) != 0;
}

void UiHost::OnResize(UINT width, UINT height) {
    // Defer the buffer resize to the next BeginFrame so it never races a mid-record frame.
    pendingWidth_ = width;
    pendingHeight_ = height;
}

void UiHost::BeginFrame() {
    if (pendingWidth_ != 0 && pendingHeight_ != 0 && swapChain_ != nullptr) {
        ReleaseRenderTarget();
        swapChain_->ResizeBuffers(0, pendingWidth_, pendingHeight_, DXGI_FORMAT_UNKNOWN, 0);
        CreateRenderTarget();
        pendingWidth_ = 0;
        pendingHeight_ = 0;
    }
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

void UiHost::EndFrame() {
    ImGui::Render();
    const float clear[4] = { 0.055f, 0.045f, 0.035f, 1.0f };
    context_->OMSetRenderTargets(1, &renderTarget_, nullptr);
    context_->ClearRenderTargetView(renderTarget_, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    swapChain_->Present(1, 0);
}

void UiHost::CreateRenderTarget() {
    ID3D11Texture2D* backBuffer = nullptr;
    if (SUCCEEDED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) && backBuffer != nullptr) {
        device_->CreateRenderTargetView(backBuffer, nullptr, &renderTarget_);
        backBuffer->Release();
    }
}

void UiHost::ReleaseRenderTarget() {
    if (renderTarget_ != nullptr) {
        renderTarget_->Release();
        renderTarget_ = nullptr;
    }
}

void UiHost::ApplyTheme(Accent accent, float scale) {
    const int accentIndex = accent == Accent::OotGold ? 0 : 1;
    if (appliedScale_ == scale && appliedAccent_ == accentIndex) {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    if (appliedScale_ != scale) {
        io.Fonts->Clear();
        ImFontConfig fontConfig;
        fontConfig.OversampleH = 2;
        const float baseSize = 17.0f * scale;
        ImFont* body = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", baseSize, &fontConfig);
        if (body == nullptr) {
            io.Fonts->AddFontDefault();
        }
        // Second font: larger semi-bold for headers (index 1 when available).
        ImFont* header =
            io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisb.ttf", baseSize * 1.45f, &fontConfig);
        if (header == nullptr) {
            io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", baseSize * 1.45f, &fontConfig);
        }
        ImGui_ImplDX11_InvalidateDeviceObjects();
        ImGui_ImplDX11_CreateDeviceObjects();
    }

    // Dark parchment-and-metal palette; accent (gold for OoT, violet for MM) on interactive parts.
    const bool gold = accent == Accent::OotGold;
    const ImVec4 accentCol = gold ? ImVec4(0.79f, 0.64f, 0.24f, 1.0f) : ImVec4(0.58f, 0.44f, 0.86f, 1.0f);
    const ImVec4 accentDim = gold ? ImVec4(0.48f, 0.38f, 0.14f, 1.0f) : ImVec4(0.34f, 0.25f, 0.55f, 1.0f);
    const ImVec4 accentHot = gold ? ImVec4(0.93f, 0.78f, 0.34f, 1.0f) : ImVec4(0.71f, 0.57f, 0.98f, 1.0f);

    ImGuiStyle style; // fresh defaults, then restyle (avoids compounding scale)
    style.WindowRounding = 8.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 5.0f;
    style.PopupRounding = 6.0f;
    style.GrabRounding = 5.0f;
    style.TabRounding = 5.0f;
    style.ScrollbarRounding = 8.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.WindowPadding = ImVec2(16, 14);
    style.FramePadding = ImVec2(10, 6);
    style.ItemSpacing = ImVec2(10, 8);
    style.ScrollbarSize = 12.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.078f, 0.066f, 0.051f, 1.0f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.104f, 0.088f, 0.066f, 1.0f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.085f, 0.064f, 0.98f);
    colors[ImGuiCol_Border] = ImVec4(accentDim.x, accentDim.y, accentDim.z, 0.45f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.135f, 0.10f, 1.0f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.21f, 0.175f, 0.125f, 1.0f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.25f, 0.21f, 0.15f, 1.0f);
    colors[ImGuiCol_TitleBg] = colors[ImGuiCol_WindowBg];
    colors[ImGuiCol_TitleBgActive] = colors[ImGuiCol_WindowBg];
    colors[ImGuiCol_Text] = ImVec4(0.92f, 0.89f, 0.82f, 1.0f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.52f, 0.46f, 1.0f);
    colors[ImGuiCol_Button] = ImVec4(accentDim.x, accentDim.y, accentDim.z, 0.85f);
    colors[ImGuiCol_ButtonHovered] = accentCol;
    colors[ImGuiCol_ButtonActive] = accentHot;
    colors[ImGuiCol_Header] = ImVec4(accentDim.x, accentDim.y, accentDim.z, 0.55f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(accentCol.x, accentCol.y, accentCol.z, 0.65f);
    colors[ImGuiCol_HeaderActive] = ImVec4(accentCol.x, accentCol.y, accentCol.z, 0.80f);
    colors[ImGuiCol_CheckMark] = accentHot;
    colors[ImGuiCol_SliderGrab] = accentCol;
    colors[ImGuiCol_SliderGrabActive] = accentHot;
    colors[ImGuiCol_Tab] = ImVec4(0.14f, 0.12f, 0.09f, 1.0f);
    colors[ImGuiCol_TabHovered] = ImVec4(accentCol.x, accentCol.y, accentCol.z, 0.70f);
    colors[ImGuiCol_TabSelected] = ImVec4(accentDim.x, accentDim.y, accentDim.z, 1.0f);
    colors[ImGuiCol_TabDimmed] = colors[ImGuiCol_Tab];
    colors[ImGuiCol_TabDimmedSelected] = colors[ImGuiCol_TabSelected];
    colors[ImGuiCol_SeparatorHovered] = accentCol;
    colors[ImGuiCol_SeparatorActive] = accentHot;
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.06f, 0.05f, 0.04f, 1.0f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(accentDim.x, accentDim.y, accentDim.z, 0.7f);
    colors[ImGuiCol_ScrollbarGrabHovered] = accentCol;
    colors[ImGuiCol_ScrollbarGrabActive] = accentHot;
    colors[ImGuiCol_TableHeaderBg] = ImVec4(0.13f, 0.11f, 0.085f, 1.0f);
    colors[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 0.95f, 0.80f, 0.03f);
    colors[ImGuiCol_TableBorderLight] = ImVec4(accentDim.x, accentDim.y, accentDim.z, 0.25f);
    colors[ImGuiCol_TableBorderStrong] = ImVec4(accentDim.x, accentDim.y, accentDim.z, 0.45f);
    colors[ImGuiCol_NavCursor] = accentHot;
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.6f);

    ImGui::GetStyle() = style;
    appliedScale_ = scale;
    appliedAccent_ = accentIndex;
}

// ---------------------------------------------------------------------------
// Decorative drawing
// ---------------------------------------------------------------------------

namespace {

// Deterministic per-particle pseudo-random in [0,1) — stateless across frames.
float Hash01(int n) {
    n = (n << 13) ^ n;
    n = n * (n * n * 15731 + 789221) + 1376312589;
    return static_cast<float>(n & 0x7fffffff) / 2147483647.0f;
}

ImU32 WithAlpha(unsigned int color, float alpha01) {
    const ImU32 a = static_cast<ImU32>(alpha01 * 255.0f) << IM_COL32_A_SHIFT;
    return (color & ~IM_COL32_A_MASK) | a;
}

} // namespace

void DrawTriforce(ImDrawList* draw, const ImVec2& center, float size, unsigned int color, float glow01) {
    const float h = size * 0.866f; // equilateral triangle height
    const auto tri = [&](ImVec2 apex, float s, ImU32 col) {
        draw->AddTriangleFilled(apex, ImVec2(apex.x - s * 0.5f, apex.y + s * 0.866f),
                                ImVec2(apex.x + s * 0.5f, apex.y + s * 0.866f), col);
    };
    // Soft pulsing glow: enlarged, translucent copies underneath.
    const float glowAlpha = 0.10f + 0.10f * glow01;
    for (int layer = 3; layer >= 1; --layer) {
        const float spread = size * 0.06f * static_cast<float>(layer);
        tri(ImVec2(center.x, center.y - h - spread * 0.9f), size + spread * 2.0f,
            WithAlpha(color, glowAlpha / static_cast<float>(layer)));
    }
    // The three triforce pieces.
    tri(ImVec2(center.x, center.y - h), size, color);                                    // top
    tri(ImVec2(center.x - size * 0.5f, center.y), size, color);                          // bottom left
    tri(ImVec2(center.x + size * 0.5f, center.y), size, color);                          // bottom right
}

void DrawCornerBrackets(ImDrawList* draw, const ImVec2& min, const ImVec2& max, unsigned int color, float length,
                        float thickness) {
    const auto corner = [&](ImVec2 at, float dx, float dy) {
        draw->AddLine(at, ImVec2(at.x + dx * length, at.y), color, thickness);
        draw->AddLine(at, ImVec2(at.x, at.y + dy * length), color, thickness);
    };
    corner(ImVec2(min.x, min.y), 1.0f, 1.0f);
    corner(ImVec2(max.x, min.y), -1.0f, 1.0f);
    corner(ImVec2(min.x, max.y), 1.0f, -1.0f);
    corner(ImVec2(max.x, max.y), -1.0f, -1.0f);
}

void DrawFairyDust(ImDrawList* draw, const ImVec2& min, const ImVec2& max, unsigned int accent, double time) {
    const float width = max.x - min.x;
    const float height = max.y - min.y;
    if (width <= 0 || height <= 0) {
        return;
    }

    // Top sky glow: a few stacked translucent bands reading as a soft gradient.
    for (int band = 0; band < 6; ++band) {
        const float t0 = static_cast<float>(band) / 6.0f;
        const float t1 = static_cast<float>(band + 1) / 6.0f;
        const float alpha = 0.045f * (1.0f - t0);
        draw->AddRectFilled(ImVec2(min.x, min.y + height * 0.35f * t0), ImVec2(max.x, min.y + height * 0.35f * t1),
                            WithAlpha(accent, alpha));
    }

    // Drifting motes: position fully derived from (index, time) so no state is kept.
    constexpr int kMotes = 56;
    for (int i = 0; i < kMotes; ++i) {
        const float seedX = Hash01(i * 7 + 1);
        const float speed = 8.0f + 26.0f * Hash01(i * 7 + 2);
        const float sway = 10.0f + 26.0f * Hash01(i * 7 + 3);
        const float phase = Hash01(i * 7 + 4) * 6.28318f;
        const float radius = 1.0f + 2.2f * Hash01(i * 7 + 5);

        const float travel = static_cast<float>(time) * speed + Hash01(i * 7 + 6) * height * 4.0f;
        const float y = max.y - std::fmod(travel, height + 40.0f) + 20.0f;
        const float x = min.x + seedX * width + std::sin(static_cast<float>(time) * 0.7f + phase) * sway;
        const float twinkle = 0.5f + 0.5f * std::sin(static_cast<float>(time) * 2.1f + phase * 3.0f);
        const float alpha = (0.05f + 0.20f * twinkle) * (0.4f + 0.6f * Hash01(i * 7 + 5));

        draw->AddCircleFilled(ImVec2(x, y), radius * 2.6f, WithAlpha(accent, alpha * 0.25f));
        draw->AddCircleFilled(ImVec2(x, y), radius, WithAlpha(accent, alpha));
    }

    // Vignette: darkened corners pull focus to the center panels.
    const ImU32 dark = IM_COL32(0, 0, 0, 110);
    const ImU32 clear = IM_COL32(0, 0, 0, 0);
    const float edge = height * 0.28f;
    draw->AddRectFilledMultiColor(ImVec2(min.x, max.y - edge), max, clear, clear, dark, dark);
    draw->AddRectFilledMultiColor(min, ImVec2(max.x, min.y + edge * 0.5f), dark, dark, clear, clear);
}

std::wstring OpenFileDialog(HWND owner, const wchar_t* title, const wchar_t* filter) {
    wchar_t buffer[MAX_PATH] = { 0 };
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = filter;
    ofn.nFilterIndex = 1;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn) == 0) {
        return {};
    }
    return buffer;
}

} // namespace ootmm::launcher

#endif // _WIN32
