#include <optional>
#include <mutex>

#include <wrl.h>

#include <d3d11.h>
#include <d3d12.h>
#include <utility/Scan.hpp>
#include <utility/Module.hpp>

#include <GraphicsMemory.h>

#include "d3d12/ComPtr.hpp"
#include "d3d12/CommandContext.hpp"
#include "d3d12/TextureContext.hpp"

#include "uevr/Plugin.hpp"

using namespace uevr;
HRESULT clear_d3d11_rt(ID3D11Device* device, ID3D11Texture2D* texture, const float* clear_color, std::optional<DXGI_FORMAT> format = std::nullopt) {
    // Create a temporary render target view
    // This is meant to be called infrequently so it's fine to create and destroy the view every time
    d3d12::ComPtr<ID3D11RenderTargetView> rtv{};

    if (format) {
        D3D11_RENDER_TARGET_VIEW_DESC rtv_desc{};
        rtv_desc.Format = *format;
        rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        rtv_desc.Texture2D.MipSlice = 0;
        if (auto result = device->CreateRenderTargetView(texture, &rtv_desc, &rtv); FAILED(result)) {
            return result;
        }
    } else {
        if (auto result = device->CreateRenderTargetView(texture, nullptr, &rtv); FAILED(result)) {
            format = DXGI_FORMAT_B8G8R8A8_UNORM;

            D3D11_RENDER_TARGET_VIEW_DESC rtv_desc{};
            rtv_desc.Format = *format;
            rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            rtv_desc.Texture2D.MipSlice = 0;
            if (auto result = device->CreateRenderTargetView(texture, &rtv_desc, &rtv); FAILED(result)) {
                return result;
            }
        }
    }

    // Clear the render target
    ID3D11DeviceContext* context{nullptr};
    device->GetImmediateContext(&context);
    context->ClearRenderTargetView(rtv.Get(), clear_color);

    return S_OK;
}

class FF7Plugin final : public uevr::Plugin {
public:
    struct IPooledRenderTargetImpl {
        void* vtable;
        struct {
            API::FRHITexture2D* texture;
            API::FRHITexture2D* srt_texture;
            void* uav;
        } data;
    };

    void on_initialize() override {
        // Find some horrible code that hardcodes a check against 1920
        // so we can find the GSystemResolution
        const auto game = utility::get_executable();
        const auto result = utility::scan(game, "81 3D ? ? ? ? 80 07 00 00");

        if (!result) {
            API::get()->log_error("Failed to find GSystemResolution");
            return;
        }

        const auto addr = utility::calculate_absolute(result.value() + 2, 8);
        m_system_resolution = (int32_t*)addr;

        API::get()->log_info("Found GSystemResolution at 0x%p", (void*)m_system_resolution);
    }

    void on_present() {
        std::scoped_lock _{m_present_mutex};

        const auto is_d3d11 = API::get()->param()->renderer->renderer_type == UEVR_RENDERER_D3D11;

        if (!is_d3d11) {
            init_d3d12();
        }

        ++m_frame_index;

        if (m_ui_tex_to_clear != nullptr) {
            auto native_resource = m_ui_tex_to_clear->get_native_resource();

            if (native_resource != nullptr) {
                if (is_d3d11) {
                    auto device = (ID3D11Device*)API::get()->param()->renderer->device;
                    float clear_color[4]{0.0f, 0.0f, 0.0f, 1.0f}; // why is the alpha channel 1.0f? it works though
                    if (FAILED(clear_d3d11_rt(device, (ID3D11Texture2D*)native_resource, clear_color))) {
                        API::get()->log_error("Failed to clear D3D11 render target");
                    }
                } else {
                    auto& command_context = m_d3d12_commands[m_frame_index % 3];
                    
                    command_context.wait(2000);

                    m_d3d12_ui_tex.setup((ID3D12Device*)API::get()->param()->renderer->device, (ID3D12Resource*)native_resource, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM);
                    const float clear_color[4]{0.0f, 0.0f, 0.0f, 1.0f};
                    command_context.clear_rtv(m_d3d12_ui_tex, clear_color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    command_context.execute((ID3D12CommandQueue*)API::get()->param()->renderer->command_queue);
                }

                m_ui_tex_to_clear = nullptr;
            }
        }

        if (!is_d3d11) {
            if (m_graphics_memory != nullptr) {
                auto command_queue = (ID3D12CommandQueue*)API::get()->param()->renderer->command_queue;

                m_graphics_memory->Commit(command_queue);
            }
        }
    }

    bool initialize_cvars() {
        if (m_cvars.initialized) {
            return true;
        }

        const auto console = API::get()->get_console_manager();

        if (console == nullptr) {
            return false;
        }

        m_cvars.r_InGameUI_FixedHeight = console->find_variable(L"r.InGameUI.FixedHeight");
        m_cvars.r_InGameUI_FixedWidth = console->find_variable(L"r.InGameUI.FixedWidth");
        m_cvars.initialized = m_cvars.r_InGameUI_FixedHeight != nullptr && 
                              m_cvars.r_InGameUI_FixedWidth != nullptr;

        return m_cvars.initialized;
    }

    void on_pre_viewport_client_draw(UEVR_UGameViewportClientHandle viewport_client, UEVR_FViewportHandle viewport, UEVR_FCanvasHandle) {
        if (!initialize_cvars()) {
            return;
        }

        const auto vr = API::get()->param()->vr;
        const auto is_hmd_active = vr->is_hmd_active();

        if (is_hmd_active) {
            const auto w = (int32_t)vr->get_ui_width();
            const auto h = (int32_t)vr->get_ui_height();

            if (w == 0 || h == 0) {
                return;
            }

            // TODO: Figure out why this crashes DX12
            if (m_cvars.r_InGameUI_FixedWidth->get_int() != w - 1) {
                m_cvars.r_InGameUI_FixedWidth->set(w - 1);
            }

            if (m_cvars.r_InGameUI_FixedHeight->get_int() != h - 1) {
                m_cvars.r_InGameUI_FixedHeight->set(h - 1);
            }

            if (m_system_resolution == nullptr) {
                return;
            }

            m_system_resolution[0] = vr->get_hmd_width() * 2;
            m_system_resolution[1] = vr->get_hmd_height();
        }
    }

    // Only used because this function gets called on the render thread
    void on_pre_slate_draw_window(UEVR_FSlateRHIRendererHandle renderer, UEVR_FViewportInfoHandle viewport_info) override {
        if (!initialize_cvars()) {
            return;
        }

        API::RenderTargetPoolHook::activate();

        auto rt = API::RenderTargetPoolHook::get_render_target(L"InGameUIRenderTarget");

        if (rt != nullptr) {
            replace_ingame_ui_render_target(rt);
        } else {
            m_last_engine_ui_tex = nullptr;
            m_last_engine_ui_srt = nullptr;
        }
    }

    void replace_ingame_ui_render_target(API::IPooledRenderTarget* rtb) {
        auto rt = (IPooledRenderTargetImpl*)rtb;
        const auto is_hmd_active = API::get()->param()->vr->is_hmd_active();

        if (m_last_engine_ui_tex != nullptr && !is_hmd_active) {
            // Restore the original render target if the HMD is not active
            if (rt->data.texture != nullptr && (rt->data.texture == API::StereoHook::get_ui_render_target() || rt->data.texture == m_last_ui_tex)) {
                rt->data.texture = m_last_engine_ui_tex;
                //rt->data.srt_texture = m_last_engine_ui_srt;
            }

            m_last_engine_ui_tex = nullptr;
            m_last_engine_ui_srt = nullptr;
            m_last_ui_tex = nullptr;
        }

        if (rt->data.texture == nullptr || !is_hmd_active) {
            m_last_engine_ui_tex = nullptr;
            m_last_ui_tex = nullptr;
            return;
        }

        const auto ui_render_target = API::StereoHook::get_ui_render_target();

        if (ui_render_target == nullptr) {
            if (rt->data.texture == m_last_ui_tex && m_last_engine_ui_tex != nullptr) {
                rt->data.texture = m_last_engine_ui_tex;
                //rt->data.srt_texture = m_last_engine_ui_srt;
            }

            m_last_ui_tex = nullptr;
            m_last_engine_ui_tex = nullptr;
            m_last_engine_ui_srt = nullptr;
            return;
        }

        if (rt->data.texture != ui_render_target) {
            if (rt->data.texture != m_last_ui_tex) {
                m_ui_tex_to_clear = rt->data.texture;
                m_last_engine_ui_tex = rt->data.texture;
                m_last_engine_ui_srt = rt->data.srt_texture;
            }
            
            rt->data.texture = ui_render_target;
            //rt->data.srt_texture = m_last_engine_ui_tex;
        }

        m_last_ui_tex = ui_render_target;
    }

private:
    std::recursive_mutex m_present_mutex{};

    API::FRHITexture2D* m_last_engine_ui_tex{nullptr}; // The engine's render target
    API::FRHITexture2D* m_last_engine_ui_srt{nullptr}; // The engine's render target

    API::FRHITexture2D* m_ui_tex_to_clear{nullptr};
    API::FRHITexture2D* m_last_ui_tex{nullptr}; // Our render target we made

    struct {
        bool initialized{false};
        API::IConsoleVariable* r_InGameUI_FixedWidth{nullptr};
        API::IConsoleVariable* r_InGameUI_FixedHeight{nullptr};
    } m_cvars{};

    int32_t* m_system_resolution{nullptr};

    std::unique_ptr<DirectX::DX12::GraphicsMemory> m_graphics_memory{};
    d3d12::CommandContext m_d3d12_commands[3]{};
    d3d12::TextureContext m_d3d12_ui_tex{};

    uint32_t m_frame_index{0};

    void init_d3d12() {
        m_d3d12_ui_tex.reset();

        if (m_graphics_memory == nullptr) {
            auto device = (ID3D12Device*)API::get()->param()->renderer->device;
            m_graphics_memory = std::make_unique<DirectX::DX12::GraphicsMemory>(device);
        }

        for (auto& command_context : m_d3d12_commands) {
            auto device = (ID3D12Device*)API::get()->param()->renderer->device;
            if (command_context.cmd_list.Get() == nullptr) {
                command_context.setup(device, L"FF7Plugin");
            }
        }
    }

    void on_device_reset() override {
        std::scoped_lock _{m_present_mutex};

        if (API::get()->param()->renderer->renderer_type == UEVR_RENDERER_D3D12) {
            for (auto& command_context : m_d3d12_commands) {
                command_context.reset();
            }

            m_graphics_memory.reset();
        }
    }
};


std::unique_ptr<FF7Plugin> g_plugin{new FF7Plugin()};