#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.capture.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgdx = winrt::Windows::Graphics::DirectX;
namespace wgd3d = winrt::Windows::Graphics::DirectX::Direct3D11;

using winrt::apartment_type;
using winrt::check_hresult;
using winrt::com_ptr;
using winrt::hresult_error;
using winrt::init_apartment;

namespace
{
constexpr wchar_t kWindowClass[] = L"SmoothMotionHostOutput";
constexpr UINT kQuitHotkey = 1;
constexpr UINT kToggleTopmostHotkey = 2;

HWND g_outputWindow = nullptr;
HWND g_sourceWindow = nullptr;
std::atomic<bool> g_running{true};
std::atomic<uint64_t> g_presentedFrames{0};
std::atomic<uint64_t> g_capturedFrames{0};

struct WindowInfo
{
    HWND hwnd{};
    std::wstring title;
};

std::wstring ToLower(std::wstring s)
{
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM param)
{
    auto* windows = reinterpret_cast<std::vector<WindowInfo>*>(param);
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;

    const int length = GetWindowTextLengthW(hwnd);
    if (length <= 0) return TRUE;

    std::wstring title(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(hwnd, title.data(), length + 1);
    title.resize(wcslen(title.c_str()));
    if (title.empty()) return TRUE;

    windows->push_back({hwnd, std::move(title)});
    return TRUE;
}

std::vector<WindowInfo> EnumerateWindows()
{
    std::vector<WindowInfo> result;
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&result));
    return result;
}

HWND PickSourceWindow(const std::wstring& titleSubstring)
{
    auto windows = EnumerateWindows();

    if (!titleSubstring.empty())
    {
        const auto needle = ToLower(titleSubstring);
        for (const auto& w : windows)
        {
            if (ToLower(w.title).find(needle) != std::wstring::npos)
                return w.hwnd;
        }
        std::wcerr << L"No visible top-level window matched: " << titleSubstring << L"\n";
        return nullptr;
    }

    std::wcout << L"\nVisible windows:\n";
    for (size_t i = 0; i < windows.size(); ++i)
        std::wcout << L"  [" << i << L"] " << windows[i].title << L"\n";

    std::wcout << L"\nSelect the game/window number: ";
    size_t index = static_cast<size_t>(-1);
    std::wcin >> index;
    if (index >= windows.size()) return nullptr;
    return windows[index].hwnd;
}

LRESULT CALLBACK OutputWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_HOTKEY:
        if (wParam == kQuitHotkey)
        {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        if (wParam == kToggleTopmostHotkey)
        {
            static bool topmost = true;
            topmost = !topmost;
            SetWindowPos(hwnd, topmost ? HWND_TOPMOST : HWND_NOTOPMOST,
                         0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            return 0;
        }
        break;
    case WM_CLOSE:
        g_running = false;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g_running = false;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

HWND CreateOutputWindow(HINSTANCE instance, HWND source, RECT& monitorRect)
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OutputWndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    HMONITOR monitor = MonitorFromWindow(source, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(monitor, &mi))
        throw std::runtime_error("GetMonitorInfo failed");
    monitorRect = mi.rcMonitor;

    const int width = monitorRect.right - monitorRect.left;
    const int height = monitorRect.bottom - monitorRect.top;

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT,
        kWindowClass,
        L"Smooth Motion Host",
        WS_POPUP,
        monitorRect.left,
        monitorRect.top,
        width,
        height,
        nullptr,
        nullptr,
        instance,
        nullptr);

    if (!hwnd) throw std::runtime_error("CreateWindowEx failed");

    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
    RegisterHotKey(hwnd, kQuitHotkey, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'Q');
    RegisterHotKey(hwnd, kToggleTopmostHotkey, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'T');
    return hwnd;
}

wgd3d::IDirect3DDevice MakeWinRTDevice(ID3D11Device* d3dDevice)
{
    com_ptr<IDXGIDevice> dxgiDevice;
    check_hresult(d3dDevice->QueryInterface(IID_PPV_ARGS(dxgiDevice.put())));

    com_ptr<::IInspectable> inspectable;
    check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put()));
    return inspectable.as<wgd3d::IDirect3DDevice>();
}

wgc::GraphicsCaptureItem CreateCaptureItemForWindow(HWND hwnd)
{
    // Follow Microsoft's Win32 + C++/WinRT interop pattern explicitly.
    // Using the ABI GUID avoids ambiguity between the native Windows namespace
    // from the interop headers and winrt::Windows.
    auto activationFactory = winrt::get_activation_factory<wgc::GraphicsCaptureItem>();
    auto interop = activationFactory.as<IGraphicsCaptureItemInterop>();

    wgc::GraphicsCaptureItem item{nullptr};
    check_hresult(interop->CreateForWindow(
        hwnd,
        winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
        reinterpret_cast<void**>(winrt::put_abi(item))));
    return item;
}

com_ptr<ID3D11Texture2D> GetTextureFromSurface(const wgd3d::IDirect3DSurface& surface)
{
    auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    com_ptr<ID3D11Texture2D> texture;
    check_hresult(access->GetInterface(IID_PPV_ARGS(texture.put())));
    return texture;
}

class Renderer
{
public:
    void Initialize(HWND hwnd, UINT outputWidth, UINT outputHeight, bool vsync)
    {
        _outputWidth = outputWidth;
        _outputHeight = outputHeight;
        _vsync = vsync;

        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0
        };
        D3D_FEATURE_LEVEL selected{};

        check_hresult(D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            flags,
            levels,
            ARRAYSIZE(levels),
            D3D11_SDK_VERSION,
            _device.put(),
            &selected,
            _context.put()));

        com_ptr<IDXGIDevice> dxgiDevice;
        check_hresult(_device->QueryInterface(IID_PPV_ARGS(dxgiDevice.put())));
        com_ptr<IDXGIAdapter> adapter;
        check_hresult(dxgiDevice->GetAdapter(adapter.put()));
        com_ptr<IDXGIFactory2> factory;
        check_hresult(adapter->GetParent(IID_PPV_ARGS(factory.put())));

        BOOL tearing = FALSE;
        com_ptr<IDXGIFactory5> factory5;
        if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(factory5.put()))))
        {
            if (FAILED(factory5->CheckFeatureSupport(
                    DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                    &tearing,
                    sizeof(tearing))))
                tearing = FALSE;
        }
        _allowTearing = tearing == TRUE;

        DXGI_SWAP_CHAIN_DESC1 sc{};
        sc.Width = outputWidth;
        sc.Height = outputHeight;
        sc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sc.Stereo = FALSE;
        sc.SampleDesc.Count = 1;
        sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sc.BufferCount = 2;
        sc.Scaling = DXGI_SCALING_STRETCH;
        sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        sc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        if (_allowTearing)
            sc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

        check_hresult(factory->CreateSwapChainForHwnd(
            _device.get(), hwnd, &sc, nullptr, nullptr, _swapChain.put()));
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

        com_ptr<IDXGISwapChain2> swapChain2;
        if (SUCCEEDED(_swapChain->QueryInterface(IID_PPV_ARGS(swapChain2.put()))))
            swapChain2->SetMaximumFrameLatency(1);

        check_hresult(_swapChain->GetBuffer(0, IID_PPV_ARGS(_backBuffer.put())));
        check_hresult(_device->CreateRenderTargetView(_backBuffer.get(), nullptr, _rtv.put()));

        BuildShaders();
        _winrtDevice = MakeWinRTDevice(_device.get());
    }

    ID3D11Device* Device() const { return _device.get(); }
    ID3D11DeviceContext* Context() const { return _context.get(); }
    const wgd3d::IDirect3DDevice& WinRTDevice() const { return _winrtDevice; }

    void Render(ID3D11Texture2D* source, UINT sourceWidth, UINT sourceHeight)
    {
        std::scoped_lock lock(_renderMutex);

        D3D11_TEXTURE2D_DESC src{};
        source->GetDesc(&src);

        D3D11_TEXTURE2D_DESC dst{};
        _backBuffer->GetDesc(&dst);

        // Fast path: no shader at all when source and output match exactly.
        if (src.Width == dst.Width && src.Height == dst.Height && src.Format == dst.Format)
        {
            _context->CopyResource(_backBuffer.get(), source);
        }
        else
        {
            EnsureIntermediate(src);
            _context->CopyResource(_intermediate.get(), source);

            const float clear[4] = {0.f, 0.f, 0.f, 1.f};
            _context->ClearRenderTargetView(_rtv.get(), clear);

            // Preserve aspect ratio. This normally becomes a full-screen viewport for 16:9 -> 16:9.
            const float srcAspect = static_cast<float>(sourceWidth) / static_cast<float>(sourceHeight);
            const float dstAspect = static_cast<float>(_outputWidth) / static_cast<float>(_outputHeight);

            D3D11_VIEWPORT vp{};
            if (srcAspect > dstAspect)
            {
                vp.Width = static_cast<float>(_outputWidth);
                vp.Height = vp.Width / srcAspect;
                vp.TopLeftY = (static_cast<float>(_outputHeight) - vp.Height) * 0.5f;
            }
            else
            {
                vp.Height = static_cast<float>(_outputHeight);
                vp.Width = vp.Height * srcAspect;
                vp.TopLeftX = (static_cast<float>(_outputWidth) - vp.Width) * 0.5f;
            }
            vp.MinDepth = 0.f;
            vp.MaxDepth = 1.f;

            ID3D11RenderTargetView* rtv = _rtv.get();
            ID3D11ShaderResourceView* srv = _intermediateSrv.get();
            ID3D11SamplerState* sampler = _pointSampler.get();

            _context->OMSetRenderTargets(1, &rtv, nullptr);
            _context->RSSetViewports(1, &vp);
            _context->IASetInputLayout(nullptr);
            _context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            _context->VSSetShader(_vs.get(), nullptr, 0);
            _context->PSSetShader(_ps.get(), nullptr, 0);
            _context->PSSetShaderResources(0, 1, &srv);
            _context->PSSetSamplers(0, 1, &sampler);
            _context->Draw(3, 0);

            ID3D11ShaderResourceView* nullSrv = nullptr;
            _context->PSSetShaderResources(0, 1, &nullSrv);
        }

        UINT syncInterval = _vsync ? 1u : 0u;
        UINT presentFlags = (!_vsync && _allowTearing) ? DXGI_PRESENT_ALLOW_TEARING : 0u;
        HRESULT hr = _swapChain->Present(syncInterval, presentFlags);
        if (hr == DXGI_STATUS_OCCLUDED)
            return;
        check_hresult(hr);
        ++g_presentedFrames;
    }

private:
    void EnsureIntermediate(const D3D11_TEXTURE2D_DESC& src)
    {
        if (_intermediate)
        {
            D3D11_TEXTURE2D_DESC old{};
            _intermediate->GetDesc(&old);
            if (old.Width == src.Width && old.Height == src.Height && old.Format == src.Format)
                return;
        }

        _intermediate = nullptr;
        _intermediateSrv = nullptr;

        D3D11_TEXTURE2D_DESC td{};
        td.Width = src.Width;
        td.Height = src.Height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = src.Format;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        check_hresult(_device->CreateTexture2D(&td, nullptr, _intermediate.put()));
        check_hresult(_device->CreateShaderResourceView(_intermediate.get(), nullptr, _intermediateSrv.put()));
    }

    void BuildShaders()
    {
        static constexpr char kVs[] = R"(
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VSOut main(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    return o;
}
)";

        static constexpr char kPs[] = R"(
Texture2D sourceTex : register(t0);
SamplerState pointSampler : register(s0);
float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET
{
    return sourceTex.Sample(pointSampler, uv);
}
)";

        com_ptr<ID3DBlob> vsBlob;
        com_ptr<ID3DBlob> psBlob;
        com_ptr<ID3DBlob> errors;

        check_hresult(D3DCompile(kVs, sizeof(kVs) - 1, nullptr, nullptr, nullptr,
                                 "main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                 vsBlob.put(), errors.put()));
        errors = nullptr;
        check_hresult(D3DCompile(kPs, sizeof(kPs) - 1, nullptr, nullptr, nullptr,
                                 "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                 psBlob.put(), errors.put()));

        check_hresult(_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                                   nullptr, _vs.put()));
        check_hresult(_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
                                                  nullptr, _ps.put()));

        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        check_hresult(_device->CreateSamplerState(&sd, _pointSampler.put()));
    }

    com_ptr<ID3D11Device> _device;
    com_ptr<ID3D11DeviceContext> _context;
    com_ptr<IDXGISwapChain1> _swapChain;
    com_ptr<ID3D11Texture2D> _backBuffer;
    com_ptr<ID3D11RenderTargetView> _rtv;
    com_ptr<ID3D11VertexShader> _vs;
    com_ptr<ID3D11PixelShader> _ps;
    com_ptr<ID3D11SamplerState> _pointSampler;
    com_ptr<ID3D11Texture2D> _intermediate;
    com_ptr<ID3D11ShaderResourceView> _intermediateSrv;
    wgd3d::IDirect3DDevice _winrtDevice{nullptr};
    UINT _outputWidth{};
    UINT _outputHeight{};
    bool _allowTearing{};
    bool _vsync{};
    std::mutex _renderMutex;
};

void PrintUsage()
{
    std::wcout
        << L"SmoothMotionHost v0.3\n"
        << L"Usage:\n"
        << L"  SmoothMotionHost.exe                    (interactive window picker)\n"
        << L"  SmoothMotionHost.exe --title \"Sekiro\"\n"
        << L"  SmoothMotionHost.exe --title \"Sekiro\" --vsync\n\n"
        << L"Hotkeys:\n"
        << L"  Ctrl+Shift+Q   Quit\n"
        << L"  Ctrl+Shift+T   Toggle always-on-top\n";
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    try
    {
        init_apartment(apartment_type::multi_threaded);

        if (!wgc::GraphicsCaptureSession::IsSupported())
        {
            std::wcerr << L"Windows Graphics Capture is not supported on this system.\n";
            return 2;
        }

        std::wstring title;
        bool vsync = false;
        for (int i = 1; i < argc; ++i)
        {
            std::wstring arg = argv[i];
            if (arg == L"--title" && i + 1 < argc)
                title = argv[++i];
            else if (arg == L"--vsync")
                vsync = true;
            else if (arg == L"--help" || arg == L"-h" || arg == L"/?")
            {
                PrintUsage();
                return 0;
            }
        }

        PrintUsage();
        HWND source = PickSourceWindow(title);
        if (!source || !IsWindow(source))
        {
            std::wcerr << L"No valid source window selected.\n";
            return 3;
        }
        g_sourceWindow = source;

        wchar_t sourceTitle[512]{};
        GetWindowTextW(source, sourceTitle, ARRAYSIZE(sourceTitle));
        std::wcout << L"\nCapturing: " << sourceTitle << L"\n";

        RECT monitorRect{};
        g_outputWindow = CreateOutputWindow(GetModuleHandleW(nullptr), source, monitorRect);
        const UINT outputWidth = static_cast<UINT>(monitorRect.right - monitorRect.left);
        const UINT outputHeight = static_cast<UINT>(monitorRect.bottom - monitorRect.top);

        Renderer renderer;
        renderer.Initialize(g_outputWindow, outputWidth, outputHeight, vsync);

        auto item = CreateCaptureItemForWindow(source);
        auto captureSize = item.Size();
        if (captureSize.Width <= 0 || captureSize.Height <= 0)
            throw std::runtime_error("Capture item has invalid size");

        auto framePool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
            renderer.WinRTDevice(),
            wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            2,
            captureSize);

        auto session = framePool.CreateCaptureSession(item);
        // Windows 10 2004+ / Windows 11. Keeping the cursor out avoids needless work.
        session.IsCursorCaptureEnabled(false);

        // Keep v0.3 deliberately simple: do not recreate the frame pool while capturing.
        // If the source window changes size, restart the host. This avoids an event-callback
        // projection/compiler issue in the first prototype and keeps the SM86 test path minimal.
        auto frameToken = framePool.FrameArrived([
            &renderer](wgc::Direct3D11CaptureFramePool const& sender, winrt::IInspectable const&) noexcept
        {
            try
            {
                if (!g_running) return;

                auto frame = sender.TryGetNextFrame();
                if (!frame) return;
                ++g_capturedFrames;

                auto size = frame.ContentSize();
                if (size.Width <= 0 || size.Height <= 0) return;

                auto texture = GetTextureFromSurface(frame.Surface());
                renderer.Render(texture.get(), static_cast<UINT>(size.Width), static_cast<UINT>(size.Height));
            }
            catch (const hresult_error& e)
            {
                std::wcerr << L"Capture/render error: 0x" << std::hex << static_cast<uint32_t>(e.code().value)
                           << L" " << e.message().c_str() << std::dec << L"\n";
            }
            catch (...)
            {
                std::wcerr << L"Capture/render error.\n";
            }
        });

        auto closedToken = item.Closed([](wgc::GraphicsCaptureItem const&, winrt::IInspectable const&) noexcept
        {
            if (g_outputWindow) PostMessageW(g_outputWindow, WM_CLOSE, 0, 0);
        });

        session.StartCapture();
        std::wcout << L"Output: " << outputWidth << L"x" << outputHeight
                   << (vsync ? L" | Present VSync ON\n" : L" | Present VSync OFF\n");
        std::wcout << L"The source game should remain focused. Press Ctrl+Shift+Q to exit.\n\n";

        auto last = std::chrono::steady_clock::now();
        uint64_t lastCaptured = 0;
        uint64_t lastPresented = 0;

        MSG msg{};
        while (g_running)
        {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                if (msg.message == WM_QUIT)
                {
                    g_running = false;
                    break;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }

            auto now = std::chrono::steady_clock::now();
            const double seconds = std::chrono::duration<double>(now - last).count();
            if (seconds >= 2.0)
            {
                const auto captured = g_capturedFrames.load();
                const auto presented = g_presentedFrames.load();
                const double captureFps = static_cast<double>(captured - lastCaptured) / seconds;
                const double presentFps = static_cast<double>(presented - lastPresented) / seconds;
                std::wcout << L"capture " << static_cast<int>(captureFps + 0.5)
                           << L" fps | present " << static_cast<int>(presentFps + 0.5) << L" fps\n";
                lastCaptured = captured;
                lastPresented = presented;
                last = now;
            }

            Sleep(1);
        }

        item.Closed(closedToken);
        framePool.FrameArrived(frameToken);
        session.Close();
        framePool.Close();
        UnregisterHotKey(g_outputWindow, kQuitHotkey);
        UnregisterHotKey(g_outputWindow, kToggleTopmostHotkey);
        return 0;
    }
    catch (const hresult_error& e)
    {
        std::wcerr << L"Fatal WinRT error: 0x" << std::hex << static_cast<uint32_t>(e.code().value)
                   << L" " << e.message().c_str() << std::dec << L"\n";
        return 10;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 11;
    }
}
