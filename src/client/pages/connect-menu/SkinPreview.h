#pragma once

#include <d3d9.h>
#include <algorithm>

#include "common/lib/log/Log.h"
#include "pages/connect-menu/SkinsAtlasData.h"
#include "pages/connect-menu/SkinNames.h"
#include "shared/game/Game.h"
#include "shared/ui/renderer/Renderer.h"

// Define STB_IMAGE_STATIC to avoid multiple definition issues
#define STBI_ONLY_JPEG
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "common/lib/stb/stb_image.h"

namespace coop::ui {

class SkinPreview {
public:
    static SkinPreview& Instance() {
        static SkinPreview s;
        return s;
    }

    void Invalidate() {
        if (m_tex) {
            m_tex->Release();
            m_tex = nullptr;
        }
    }

    bool EnsureLoaded() {
        if (m_tex) return true;
        auto* dev = (IDirect3DDevice9*)game::D3DDevice();
        if (!dev) return false;

        int w = 0, h = 0, channels = 0;
        unsigned char* data = stbi_load_from_memory(kSkinsAtlasJpg, (int)kSkinsAtlasJpgSize, &w, &h, &channels, 4);
        if (!data || w <= 0 || h <= 0) {
            LOGW("SkinPreview: failed to decode skins atlas JPEG");
            if (data) stbi_image_free(data);
            return false;
        }

        IDirect3DTexture9* tex = nullptr;
        if (FAILED(dev->CreateTexture((UINT)w, (UINT)h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr))) {
            LOGW("SkinPreview: CreateTexture failed");
            stbi_image_free(data);
            return false;
        }

        D3DLOCKED_RECT lr{};
        if (SUCCEEDED(tex->LockRect(0, &lr, nullptr, 0))) {
            for (int r = 0; r < h; ++r) {
                const uint8_t* srcRow = data + r * w * 4;
                uint8_t* dstRow = (uint8_t*)lr.pBits + r * lr.Pitch;
                for (int c = 0; c < w; ++c) {
                    uint8_t R = srcRow[c * 4 + 0];
                    uint8_t G = srcRow[c * 4 + 1];
                    uint8_t B = srcRow[c * 4 + 2];
                    // D3DFMT_A8R8G8B8: BGRA
                    dstRow[c * 4 + 0] = B;
                    dstRow[c * 4 + 1] = G;
                    dstRow[c * 4 + 2] = R;
                    dstRow[c * 4 + 3] = 0xFF;
                }
            }
            tex->UnlockRect(0);
        }

        stbi_image_free(data);
        m_tex = tex;
        m_width = w;
        m_height = h;
        LOGI("SkinPreview: loaded atlas texture (%dx%d)", w, h);
        return true;
    }

    void Draw(float x, float y, float w, float h, int skinId) {
        if (skinId < 0 || skinId > 311) return;
        if (!EnsureLoaded()) return;

        auto& g = Gfx();
        // 16 columns x 20 rows
        int col = skinId % 16;
        int row = skinId / 16;
        float u0 = (float)col / 16.0f;
        float u1 = (float)(col + 1) / 16.0f;
        float v0 = (float)row / 20.0f;
        float v1 = (float)(row + 1) / 20.0f;

        g.Rect(x - 2, y - 2, w + 4, h + 4, 0xFF000000);
        g.RectOutline(x - 2, y - 2, w + 4, h + 4, 0xFF4A5A6B, 1.0f);
        g.TexturedQuad(m_tex, x, y, w, h, u0, v0, u1, v1, 0xFFFFFFFF);
    }

private:
    SkinPreview() = default;
    IDirect3DTexture9* m_tex = nullptr;
    int m_width = 0;
    int m_height = 0;
};

inline void DrawSkinThumbnail(float x, float y, float w, float h, int skinId) {
    SkinPreview::Instance().Draw(x, y, w, h, skinId);
}

}  // namespace coop::ui
