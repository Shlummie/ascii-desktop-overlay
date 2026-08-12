#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <shellapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mmsystem.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <string>
#include <vector>

#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Gdi32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "D3D11.lib")
#pragma comment(lib, "Dxgi.lib")
#pragma comment(lib, "Winmm.lib")

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace
{
using Microsoft::WRL::ComPtr;

constexpr wchar_t kWindowClassName[] = L"ASCIIVisionNativeOverlayWindow";
constexpr wchar_t kWindowTitle[] = L"ASCIIVision Native";
constexpr wchar_t kConfigFileName[] = L"asciivision_native.ini";
constexpr wchar_t kConfigSection[] = L"ASCIIVision";
constexpr wchar_t kAsciiChars[] = L" .'`^\",:;Il!i~+_-?][}{1)(|/tfjrxnuvczXYUJCLQ0OZmwqpdbkhao*#MW&8%B@$";

constexpr UINT_PTR kRenderTimerId = 1;
constexpr int kTrayIconId = 1;
constexpr UINT kTrayIconMessage = WM_APP + 1;
constexpr int kHotkeyToggle = 1;
constexpr int kHotkeyQuit = 2;

enum TrayCommand : UINT
{
    kTrayCommandToggleVisibility = 1001,
    kTrayCommandReloadConfig = 1002,
    kTrayCommandQuit = 1003,
};

enum class CaptureResult
{
    Rendered,
    Timeout,
    AccessLost,
    Fatal,
};

struct AppConfig
{
    int targetFps = 144;
    std::wstring fontName = L"Consolas";
    int fontHeight = 10;
    int densityPercent = 100;
    int colorBoostPercent = 120;
    bool invert = false;
    int primaryMonitorOnly = 1;
};

int ClampInt(int value, int minimum, int maximum)
{
    return std::max(minimum, std::min(value, maximum));
}

std::uint8_t ClampByte(int value)
{
    return static_cast<std::uint8_t>(ClampInt(value, 0, 255));
}

std::wstring ModuleDirectory()
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    std::wstring modulePath(path);
    const std::size_t separator = modulePath.find_last_of(L"\\/");
    if (separator == std::wstring::npos)
    {
        return L".";
    }
    return modulePath.substr(0, separator);
}

std::wstring JoinPath(const std::wstring& left, const wchar_t* right)
{
    std::wstring path = left;
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/')
    {
        path.push_back(L'\\');
    }
    path.append(right);
    return path;
}

std::wstring FormatHex(HRESULT hr)
{
    wchar_t buffer[32] = {};
    swprintf(buffer, static_cast<std::size_t>(std::size(buffer)), L"0x%08X", static_cast<unsigned int>(hr));
    return buffer;
}

std::wstring FormatHResult(HRESULT hr, const wchar_t* prefix)
{
    wchar_t* systemMessage = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        static_cast<DWORD>(hr),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&systemMessage),
        0,
        nullptr);

    std::wstring message(prefix);
    message += L"\n\nHRESULT ";
    message += FormatHex(hr);

    if (length != 0 && systemMessage != nullptr)
    {
        message += L"\n";
        message += systemMessage;
        LocalFree(systemMessage);
    }

    return message;
}

std::wstring FormatLastError(const wchar_t* prefix)
{
    return FormatHResult(HRESULT_FROM_WIN32(GetLastError()), prefix);
}

bool FileExists(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

void EnableBestDpiAwareness()
{
    using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);

    const HMODULE user32Module = GetModuleHandleW(L"user32.dll");
    const auto setProcessDpiAwarenessContext =
        reinterpret_cast<SetProcessDpiAwarenessContextFn>(
            GetProcAddress(user32Module, "SetProcessDpiAwarenessContext"));

    if (setProcessDpiAwarenessContext != nullptr &&
        setProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
    {
        return;
    }

    SetProcessDPIAware();
}

int TimerIntervalMs(int fps)
{
    return std::max(1, static_cast<int>(std::lround(1000.0 / static_cast<double>(ClampInt(fps, 5, 144)))));
}

bool RectsMatch(const RECT& left, const RECT& right)
{
    return left.left == right.left &&
        left.top == right.top &&
        left.right == right.right &&
        left.bottom == right.bottom;
}

class ASCIIVisionApp
{
public:
    explicit ASCIIVisionApp(HINSTANCE instance)
        : instance_(instance),
          configPath_(JoinPath(ModuleDirectory(), kConfigFileName))
    {
        grayToGlyph_.fill(0);
    }

    bool Initialize()
    {
        taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");
        EnableBestDpiAwareness();
        LoadConfig();

        if (!UpdatePrimaryMonitorRect())
        {
            ShowError(L"ASCIIVision Native", L"Failed to locate the primary monitor.");
            return false;
        }

        if (!BuildGlyphMasks())
        {
            ShowError(L"ASCIIVision Native", L"Failed to build ASCII glyph masks.");
            return false;
        }

        if (!RegisterWindowClass())
        {
            ShowError(L"ASCIIVision Native", FormatLastError(L"Failed to register the overlay window class."));
            return false;
        }

        if (!CreateOverlayWindow())
        {
            ShowError(L"ASCIIVision Native", FormatLastError(L"Failed to create the overlay window."));
            return false;
        }

        if (!ApplyCaptureExclusion())
        {
            ShowError(
                L"ASCIIVision Native",
                FormatLastError(L"Failed to exclude the overlay from screen capture. ASCIIVision requires WDA_EXCLUDEFROMCAPTURE to avoid recursive capture."));
            return false;
        }

        UpdateTrayIcon();
        RegisterHotkeys();

        if (!InitializeCapture())
        {
            ShowError(L"ASCIIVision Native", lastErrorMessage_);
            return false;
        }

        SetOverlayVisible(true);
        return true;
    }

    int Run()
    {
        MSG message = {};
        while (GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return static_cast<int>(message.wParam);
    }

private:
    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    UINT taskbarCreatedMessage_ = 0;
    NOTIFYICONDATAW trayIconData_ = {};
    bool trayIconAdded_ = false;
    bool overlayVisible_ = false;
    bool highResolutionTimerActive_ = false;

    AppConfig config_ = {};
    std::wstring configPath_;

    RECT primaryMonitorRect_ = {};
    HMONITOR primaryMonitor_ = nullptr;
    int monitorWidth_ = 0;
    int monitorHeight_ = 0;

    HFONT asciiFont_ = nullptr;
    int cellWidth_ = 0;
    int cellHeight_ = 0;
    std::array<std::uint8_t, 256> grayToGlyph_ = {};
    std::vector<std::uint8_t> glyphMasks_;
    std::vector<std::uint8_t> renderBuffer_;
    bool renderBufferValid_ = false;

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIOutputDuplication> duplication_;
    ComPtr<ID3D11Texture2D> stagingTexture_;
    DXGI_FORMAT captureFormat_ = DXGI_FORMAT_UNKNOWN;
    std::wstring lastErrorMessage_;

    void ShowError(const wchar_t* title, const std::wstring& message)
    {
        MessageBoxW(window_, message.c_str(), title, MB_ICONERROR | MB_OK);
    }

    void LoadConfig()
    {
        const bool existed = FileExists(configPath_);

        config_.targetFps = ClampInt(GetPrivateProfileIntW(kConfigSection, L"TargetFps", config_.targetFps, configPath_.c_str()), 5, 144);
        config_.fontHeight = ClampInt(GetPrivateProfileIntW(kConfigSection, L"FontHeight", config_.fontHeight, configPath_.c_str()), 6, 32);
        config_.densityPercent = ClampInt(GetPrivateProfileIntW(kConfigSection, L"DensityPercent", config_.densityPercent, configPath_.c_str()), 25, 300);
        config_.colorBoostPercent = ClampInt(GetPrivateProfileIntW(kConfigSection, L"ColorBoostPercent", config_.colorBoostPercent, configPath_.c_str()), 50, 200);
        config_.invert = GetPrivateProfileIntW(kConfigSection, L"Invert", config_.invert ? 1 : 0, configPath_.c_str()) != 0;
        config_.primaryMonitorOnly = GetPrivateProfileIntW(kConfigSection, L"PrimaryMonitorOnly", 1, configPath_.c_str());

        wchar_t fontName[LF_FACESIZE] = {};
        GetPrivateProfileStringW(kConfigSection, L"FontName", config_.fontName.c_str(), fontName, static_cast<DWORD>(std::size(fontName)), configPath_.c_str());
        if (fontName[0] != L'\0')
        {
            config_.fontName = fontName;
        }

        if (!existed)
        {
            SaveConfig();
        }
    }

    void SaveConfig()
    {
        WritePrivateProfileStringW(kConfigSection, L"TargetFps", std::to_wstring(config_.targetFps).c_str(), configPath_.c_str());
        WritePrivateProfileStringW(kConfigSection, L"FontName", config_.fontName.c_str(), configPath_.c_str());
        WritePrivateProfileStringW(kConfigSection, L"FontHeight", std::to_wstring(config_.fontHeight).c_str(), configPath_.c_str());
        WritePrivateProfileStringW(kConfigSection, L"DensityPercent", std::to_wstring(config_.densityPercent).c_str(), configPath_.c_str());
        WritePrivateProfileStringW(kConfigSection, L"ColorBoostPercent", std::to_wstring(config_.colorBoostPercent).c_str(), configPath_.c_str());
        WritePrivateProfileStringW(kConfigSection, L"Invert", config_.invert ? L"1" : L"0", configPath_.c_str());
        WritePrivateProfileStringW(kConfigSection, L"PrimaryMonitorOnly", L"1", configPath_.c_str());
    }

    bool RegisterWindowClass()
    {
        WNDCLASSEXW windowClass = {};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = StaticWindowProc;
        windowClass.hInstance = instance_;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        windowClass.lpszClassName = kWindowClassName;
        return RegisterClassExW(&windowClass) != 0;
    }

    bool CreateOverlayWindow()
    {
        window_ = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED,
            kWindowClassName,
            kWindowTitle,
            WS_POPUP,
            primaryMonitorRect_.left,
            primaryMonitorRect_.top,
            monitorWidth_,
            monitorHeight_,
            nullptr,
            nullptr,
            instance_,
            this);

        if (window_ == nullptr)
        {
            return false;
        }

        return SetLayeredWindowAttributes(window_, 0, 255, LWA_ALPHA) == TRUE;
    }

    bool ApplyCaptureExclusion()
    {
        if (window_ == nullptr)
        {
            return false;
        }

        return SetWindowDisplayAffinity(window_, WDA_EXCLUDEFROMCAPTURE) == TRUE;
    }

    bool UpdatePrimaryMonitorRect()
    {
        const POINT primaryPoint = {0, 0};
        primaryMonitor_ = MonitorFromPoint(primaryPoint, MONITOR_DEFAULTTOPRIMARY);
        if (primaryMonitor_ == nullptr)
        {
            return false;
        }

        MONITORINFO monitorInfo = {};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (!GetMonitorInfoW(primaryMonitor_, &monitorInfo))
        {
            return false;
        }

        primaryMonitorRect_ = monitorInfo.rcMonitor;
        monitorWidth_ = primaryMonitorRect_.right - primaryMonitorRect_.left;
        monitorHeight_ = primaryMonitorRect_.bottom - primaryMonitorRect_.top;
        return monitorWidth_ > 0 && monitorHeight_ > 0;
    }

    bool BuildGlyphMasks()
    {
        if (asciiFont_ != nullptr)
        {
            DeleteObject(asciiFont_);
            asciiFont_ = nullptr;
        }

        asciiFont_ = CreateFontW(
            -config_.fontHeight,
            0,
            0,
            0,
            FW_NORMAL,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_OUTLINE_PRECIS,
            CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY,
            FIXED_PITCH | FF_MODERN,
            config_.fontName.c_str());

        if (asciiFont_ == nullptr)
        {
            return false;
        }

        HDC screen = GetDC(nullptr);
        if (screen == nullptr)
        {
            return false;
        }

        HDC dc = CreateCompatibleDC(screen);
        ReleaseDC(nullptr, screen);
        if (dc == nullptr)
        {
            return false;
        }

        HGDIOBJ oldFont = SelectObject(dc, asciiFont_);
        SIZE sizeM = {};
        SIZE sizeMg = {};
        GetTextExtentPoint32W(dc, L"M", 1, &sizeM);
        GetTextExtentPoint32W(dc, L"Mg", 2, &sizeMg);
        cellWidth_ = std::max(1L, sizeM.cx);
        cellHeight_ = std::max(1L, sizeMg.cy + 2);

        BITMAPINFO info = {};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = cellWidth_;
        info.bmiHeader.biHeight = -cellHeight_;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bitmap == nullptr || bits == nullptr)
        {
            SelectObject(dc, oldFont);
            DeleteDC(dc);
            return false;
        }

        HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));

        const int glyphCount = static_cast<int>(std::wcslen(kAsciiChars));
        glyphMasks_.assign(
            static_cast<std::size_t>(glyphCount) *
                static_cast<std::size_t>(cellWidth_) *
                static_cast<std::size_t>(cellHeight_),
            0);

        const int paletteSize = std::max(0, glyphCount - 1);
        for (int value = 0; value < 256; ++value)
        {
            grayToGlyph_[static_cast<std::size_t>(value)] =
                static_cast<std::uint8_t>((value * paletteSize) / 255);
        }

        auto* pixelBytes = static_cast<std::uint8_t*>(bits);
        const std::size_t bitmapByteCount =
            static_cast<std::size_t>(cellWidth_) *
            static_cast<std::size_t>(cellHeight_) *
            4u;

        for (int glyphIndex = 0; glyphIndex < glyphCount; ++glyphIndex)
        {
            std::memset(pixelBytes, 0, bitmapByteCount);
            wchar_t glyph[2] = {kAsciiChars[glyphIndex], L'\0'};
            TextOutW(dc, 0, 0, glyph, 1);

            auto* glyphOut =
                glyphMasks_.data() +
                static_cast<std::size_t>(glyphIndex) *
                    static_cast<std::size_t>(cellWidth_) *
                    static_cast<std::size_t>(cellHeight_);

            for (int y = 0; y < cellHeight_; ++y)
            {
                for (int x = 0; x < cellWidth_; ++x)
                {
                    const std::size_t srcOffset =
                        (static_cast<std::size_t>(y) * static_cast<std::size_t>(cellWidth_) +
                         static_cast<std::size_t>(x)) *
                        4u;
                    const std::uint8_t alpha = std::max(
                        pixelBytes[srcOffset + 0u],
                        std::max(pixelBytes[srcOffset + 1u], pixelBytes[srcOffset + 2u]));
                    glyphOut[static_cast<std::size_t>(y) * static_cast<std::size_t>(cellWidth_) + static_cast<std::size_t>(x)] = alpha;
                }
            }
        }

        SelectObject(dc, oldBitmap);
        DeleteObject(bitmap);
        SelectObject(dc, oldFont);
        DeleteDC(dc);

        renderBufferValid_ = false;
        return true;
    }

    bool InitializeCapture()
    {
        ReleaseCaptureResources();

        ComPtr<IDXGIFactory1> factory;
        HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()));
        if (FAILED(hr))
        {
            lastErrorMessage_ = FormatHResult(hr, L"CreateDXGIFactory1 failed.");
            return false;
        }

        ComPtr<IDXGIAdapter1> selectedAdapter;
        ComPtr<IDXGIOutput> selectedOutput;
        DXGI_OUTPUT_DESC outputDescription = {};

        if (!FindPrimaryOutput(factory.Get(), selectedAdapter, selectedOutput, outputDescription))
        {
            lastErrorMessage_ = L"Failed to find a DXGI output for the primary monitor.";
            return false;
        }

        constexpr D3D_FEATURE_LEVEL featureLevels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
        };
        D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;

        hr = D3D11CreateDevice(
            selectedAdapter.Get(),
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            featureLevels,
            static_cast<UINT>(std::size(featureLevels)),
            D3D11_SDK_VERSION,
            device_.GetAddressOf(),
            &featureLevel,
            context_.GetAddressOf());
        if (FAILED(hr))
        {
            lastErrorMessage_ = FormatHResult(hr, L"D3D11CreateDevice failed.");
            return false;
        }

        ComPtr<IDXGIOutput1> output1;
        hr = selectedOutput.As(&output1);
        if (FAILED(hr))
        {
            lastErrorMessage_ = FormatHResult(hr, L"IDXGIOutput1 query failed.");
            return false;
        }

        hr = output1->DuplicateOutput(device_.Get(), duplication_.GetAddressOf());
        if (FAILED(hr))
        {
            lastErrorMessage_ = FormatHResult(hr, L"DuplicateOutput failed. ASCIIVision requires Desktop Duplication support for the primary monitor.");
            return false;
        }

        primaryMonitorRect_ = outputDescription.DesktopCoordinates;
        monitorWidth_ = primaryMonitorRect_.right - primaryMonitorRect_.left;
        monitorHeight_ = primaryMonitorRect_.bottom - primaryMonitorRect_.top;
        if (monitorWidth_ <= 0 || monitorHeight_ <= 0)
        {
            lastErrorMessage_ = L"The primary DXGI output reported an invalid monitor rectangle.";
            return false;
        }

        ResizeOverlayWindow();
        renderBufferValid_ = false;
        return true;
    }

    bool FindPrimaryOutput(
        IDXGIFactory1* factory,
        ComPtr<IDXGIAdapter1>& selectedAdapter,
        ComPtr<IDXGIOutput>& selectedOutput,
        DXGI_OUTPUT_DESC& selectedDescription)
    {
        if (factory == nullptr || primaryMonitor_ == nullptr)
        {
            return false;
        }

        for (UINT adapterIndex = 0;; ++adapterIndex)
        {
            ComPtr<IDXGIAdapter1> adapter;
            HRESULT hr = factory->EnumAdapters1(adapterIndex, adapter.GetAddressOf());
            if (hr == DXGI_ERROR_NOT_FOUND)
            {
                break;
            }
            if (FAILED(hr))
            {
                continue;
            }

            for (UINT outputIndex = 0;; ++outputIndex)
            {
                ComPtr<IDXGIOutput> output;
                hr = adapter->EnumOutputs(outputIndex, output.GetAddressOf());
                if (hr == DXGI_ERROR_NOT_FOUND)
                {
                    break;
                }
                if (FAILED(hr))
                {
                    continue;
                }

                DXGI_OUTPUT_DESC description = {};
                hr = output->GetDesc(&description);
                if (FAILED(hr) || !description.AttachedToDesktop)
                {
                    continue;
                }

                if (description.Monitor == primaryMonitor_ ||
                    RectsMatch(description.DesktopCoordinates, primaryMonitorRect_))
                {
                    selectedAdapter = adapter;
                    selectedOutput = output;
                    selectedDescription = description;
                    return true;
                }
            }
        }

        return false;
    }

    void ReleaseCaptureResources()
    {
        duplication_.Reset();
        stagingTexture_.Reset();
        context_.Reset();
        device_.Reset();
        captureFormat_ = DXGI_FORMAT_UNKNOWN;
    }

    CaptureResult CaptureAndRender()
    {
        if (duplication_ == nullptr || context_ == nullptr)
        {
            lastErrorMessage_ = L"Capture is not initialized.";
            return CaptureResult::Fatal;
        }

        DXGI_OUTDUPL_FRAME_INFO frameInfo = {};
        ComPtr<IDXGIResource> desktopResource;
        const UINT timeoutMs = static_cast<UINT>(std::min(16, TimerIntervalMs(config_.targetFps)));
        HRESULT hr = duplication_->AcquireNextFrame(timeoutMs, &frameInfo, desktopResource.GetAddressOf());

        if (hr == DXGI_ERROR_WAIT_TIMEOUT)
        {
            return CaptureResult::Timeout;
        }
        if (hr == DXGI_ERROR_ACCESS_LOST)
        {
            return CaptureResult::AccessLost;
        }
        if (FAILED(hr))
        {
            lastErrorMessage_ = FormatHResult(hr, L"AcquireNextFrame failed.");
            return CaptureResult::Fatal;
        }

        bool frameReleased = false;
        auto releaseFrame = [&]() {
            if (!frameReleased)
            {
                duplication_->ReleaseFrame();
                frameReleased = true;
            }
        };

        ComPtr<ID3D11Texture2D> desktopTexture;
        hr = desktopResource.As(&desktopTexture);
        if (FAILED(hr))
        {
            releaseFrame();
            lastErrorMessage_ = FormatHResult(hr, L"Failed to query the duplicated desktop texture.");
            return CaptureResult::Fatal;
        }

        D3D11_TEXTURE2D_DESC sourceDescription = {};
        desktopTexture->GetDesc(&sourceDescription);
        if (sourceDescription.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
        {
            releaseFrame();
            lastErrorMessage_ = L"Unsupported desktop duplication format. Expected DXGI_FORMAT_B8G8R8A8_UNORM.";
            return CaptureResult::Fatal;
        }

        if (!EnsureStagingTexture(sourceDescription))
        {
            releaseFrame();
            return CaptureResult::Fatal;
        }

        context_->CopyResource(stagingTexture_.Get(), desktopTexture.Get());
        releaseFrame();

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        hr = context_->Map(stagingTexture_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr))
        {
            lastErrorMessage_ = FormatHResult(hr, L"Failed to map the desktop staging texture.");
            return CaptureResult::Fatal;
        }

        RenderAsciiFrame(
            static_cast<const std::uint8_t*>(mapped.pData),
            mapped.RowPitch,
            static_cast<int>(sourceDescription.Width),
            static_cast<int>(sourceDescription.Height));

        context_->Unmap(stagingTexture_.Get(), 0);
        return CaptureResult::Rendered;
    }

    bool EnsureStagingTexture(const D3D11_TEXTURE2D_DESC& sourceDescription)
    {
        if (stagingTexture_ != nullptr &&
            sourceDescription.Format == captureFormat_)
        {
            D3D11_TEXTURE2D_DESC existingDescription = {};
            stagingTexture_->GetDesc(&existingDescription);
            if (existingDescription.Width == sourceDescription.Width &&
                existingDescription.Height == sourceDescription.Height)
            {
                return true;
            }
        }

        D3D11_TEXTURE2D_DESC stagingDescription = {};
        stagingDescription.Width = sourceDescription.Width;
        stagingDescription.Height = sourceDescription.Height;
        stagingDescription.MipLevels = 1;
        stagingDescription.ArraySize = 1;
        stagingDescription.Format = sourceDescription.Format;
        stagingDescription.SampleDesc.Count = 1;
        stagingDescription.SampleDesc.Quality = 0;
        stagingDescription.Usage = D3D11_USAGE_STAGING;
        stagingDescription.BindFlags = 0;
        stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDescription.MiscFlags = 0;

        stagingTexture_.Reset();
        const HRESULT hr = device_->CreateTexture2D(&stagingDescription, nullptr, stagingTexture_.GetAddressOf());
        if (FAILED(hr))
        {
            lastErrorMessage_ = FormatHResult(hr, L"Failed to create the desktop staging texture.");
            return false;
        }

        captureFormat_ = sourceDescription.Format;
        return true;
    }

    void RenderAsciiFrame(const std::uint8_t* sourceBytes, UINT sourcePitch, int sourceWidth, int sourceHeight)
    {
        if (sourceBytes == nullptr ||
            sourceWidth <= 0 ||
            sourceHeight <= 0 ||
            monitorWidth_ <= 0 ||
            monitorHeight_ <= 0 ||
            cellWidth_ <= 0 ||
            cellHeight_ <= 0 ||
            glyphMasks_.empty())
        {
            renderBufferValid_ = false;
            return;
        }

        const std::size_t outputByteCount =
            static_cast<std::size_t>(monitorWidth_) *
            static_cast<std::size_t>(monitorHeight_) *
            4u;
        renderBuffer_.assign(outputByteCount, 0);

        const int displayCellWidth = std::max(
            1,
            static_cast<int>(std::lround(static_cast<double>(cellWidth_) * 100.0 / static_cast<double>(config_.densityPercent))));
        const int displayCellHeight = std::max(
            1,
            static_cast<int>(std::lround(static_cast<double>(cellHeight_) * 100.0 / static_cast<double>(config_.densityPercent))));
        const int cols = std::max(1, (monitorWidth_ + displayCellWidth - 1) / displayCellWidth);
        const int rows = std::max(1, (monitorHeight_ + displayCellHeight - 1) / displayCellHeight);
        const double colorBoost = static_cast<double>(config_.colorBoostPercent) / 100.0;
        const int glyphCount = static_cast<int>(std::wcslen(kAsciiChars));

        for (int row = 0; row < rows; ++row)
        {
            const int destY = row * displayCellHeight;
            const int drawCellHeight = std::min(displayCellHeight, monitorHeight_ - destY);
            if (drawCellHeight <= 0)
            {
                continue;
            }

            int srcY0 = (row * sourceHeight) / rows;
            int srcY1 = ((row + 1) * sourceHeight) / rows;
            if (srcY1 <= srcY0)
            {
                srcY1 = std::min(sourceHeight, srcY0 + 1);
            }

            for (int col = 0; col < cols; ++col)
            {
                const int destX = col * displayCellWidth;
                const int drawCellWidth = std::min(displayCellWidth, monitorWidth_ - destX);
                if (drawCellWidth <= 0)
                {
                    continue;
                }

                int srcX0 = (col * sourceWidth) / cols;
                int srcX1 = ((col + 1) * sourceWidth) / cols;
                if (srcX1 <= srcX0)
                {
                    srcX1 = std::min(sourceWidth, srcX0 + 1);
                }

                std::uint64_t sumB = 0;
                std::uint64_t sumG = 0;
                std::uint64_t sumR = 0;
                std::uint64_t count = 0;

                for (int sourceY = srcY0; sourceY < srcY1; ++sourceY)
                {
                    const auto* sourceRow =
                        sourceBytes +
                        static_cast<std::size_t>(sourceY) *
                            static_cast<std::size_t>(sourcePitch);

                    for (int sourceX = srcX0; sourceX < srcX1; ++sourceX)
                    {
                        const auto* pixel =
                            sourceRow +
                            static_cast<std::size_t>(sourceX) *
                                4u;
                        sumB += pixel[0];
                        sumG += pixel[1];
                        sumR += pixel[2];
                        ++count;
                    }
                }

                if (count == 0)
                {
                    continue;
                }

                const int boostedB = ClampInt(static_cast<int>(std::round((static_cast<double>(sumB) / static_cast<double>(count)) * colorBoost)), 0, 255);
                const int boostedG = ClampInt(static_cast<int>(std::round((static_cast<double>(sumG) / static_cast<double>(count)) * colorBoost)), 0, 255);
                const int boostedR = ClampInt(static_cast<int>(std::round((static_cast<double>(sumR) / static_cast<double>(count)) * colorBoost)), 0, 255);
                const int gray = ((77 * boostedR) + (150 * boostedG) + (29 * boostedB)) >> 8;

                int glyphIndex = grayToGlyph_[static_cast<std::size_t>(ClampInt(gray, 0, 255))];
                if (config_.invert)
                {
                    glyphIndex = glyphCount - 1 - glyphIndex;
                }

                const auto* glyphMask =
                    glyphMasks_.data() +
                    static_cast<std::size_t>(glyphIndex) *
                        static_cast<std::size_t>(cellWidth_) *
                        static_cast<std::size_t>(cellHeight_);

                for (int glyphY = 0; glyphY < drawCellHeight; ++glyphY)
                {
                    const int maskY = std::min(
                        cellHeight_ - 1,
                        (glyphY * cellHeight_) / displayCellHeight);
                    const std::size_t outputRow =
                        static_cast<std::size_t>(destY + glyphY) *
                        static_cast<std::size_t>(monitorWidth_);
                    const std::size_t maskRow =
                        static_cast<std::size_t>(maskY) *
                        static_cast<std::size_t>(cellWidth_);

                    for (int glyphX = 0; glyphX < drawCellWidth; ++glyphX)
                    {
                        const int maskX = std::min(
                            cellWidth_ - 1,
                            (glyphX * cellWidth_) / displayCellWidth);
                        const std::uint8_t alpha = glyphMask[maskRow + static_cast<std::size_t>(maskX)];
                        if (alpha == 0)
                        {
                            continue;
                        }

                        const std::size_t outputOffset =
                            (outputRow +
                             static_cast<std::size_t>(destX + glyphX)) *
                            4u;
                        renderBuffer_[outputOffset + 0u] =
                            static_cast<std::uint8_t>((static_cast<unsigned int>(alpha) * static_cast<unsigned int>(boostedB) + 127u) / 255u);
                        renderBuffer_[outputOffset + 1u] =
                            static_cast<std::uint8_t>((static_cast<unsigned int>(alpha) * static_cast<unsigned int>(boostedG) + 127u) / 255u);
                        renderBuffer_[outputOffset + 2u] =
                            static_cast<std::uint8_t>((static_cast<unsigned int>(alpha) * static_cast<unsigned int>(boostedR) + 127u) / 255u);
                        renderBuffer_[outputOffset + 3u] = 0u;
                    }
                }
            }
        }

        renderBufferValid_ = true;
    }

    void Paint()
    {
        PAINTSTRUCT paint = {};
        HDC dc = BeginPaint(window_, &paint);

        RECT client = {};
        GetClientRect(window_, &client);
        FillRect(dc, &client, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

        if (renderBufferValid_ && !renderBuffer_.empty() && monitorWidth_ > 0 && monitorHeight_ > 0)
        {
            BITMAPINFO info = {};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = monitorWidth_;
            info.bmiHeader.biHeight = -monitorHeight_;
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;

            StretchDIBits(
                dc,
                0,
                0,
                client.right - client.left,
                client.bottom - client.top,
                0,
                0,
                monitorWidth_,
                monitorHeight_,
                renderBuffer_.data(),
                &info,
                DIB_RGB_COLORS,
                SRCCOPY);
        }

        EndPaint(window_, &paint);
    }

    void ResizeOverlayWindow()
    {
        if (window_ == nullptr)
        {
            return;
        }

        SetWindowPos(
            window_,
            HWND_TOPMOST,
            primaryMonitorRect_.left,
            primaryMonitorRect_.top,
            monitorWidth_,
            monitorHeight_,
            SWP_NOACTIVATE | (overlayVisible_ ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    }

    void EnableHighResolutionTimer()
    {
        if (!highResolutionTimerActive_ && timeBeginPeriod(1) == TIMERR_NOERROR)
        {
            highResolutionTimerActive_ = true;
        }
    }

    void DisableHighResolutionTimer()
    {
        if (highResolutionTimerActive_)
        {
            timeEndPeriod(1);
            highResolutionTimerActive_ = false;
        }
    }

    void StartRenderTimer()
    {
        if (window_ == nullptr)
        {
            return;
        }

        KillTimer(window_, kRenderTimerId);
        EnableHighResolutionTimer();
        SetTimer(window_, kRenderTimerId, static_cast<UINT>(TimerIntervalMs(config_.targetFps)), nullptr);
    }

    void StopRenderTimer()
    {
        if (window_ != nullptr)
        {
            KillTimer(window_, kRenderTimerId);
        }
        DisableHighResolutionTimer();
    }

    void SetOverlayVisible(bool visible)
    {
        overlayVisible_ = visible;

        if (window_ != nullptr)
        {
            if (visible)
            {
                ResizeOverlayWindow();
                ShowWindow(window_, SW_SHOWNA);
                StartRenderTimer();
            }
            else
            {
                StopRenderTimer();
                ShowWindow(window_, SW_HIDE);
            }
        }

        UpdateTrayIcon();
    }

    void ToggleOverlayVisible()
    {
        SetOverlayVisible(!overlayVisible_);
    }

    std::wstring BuildTrayToolTip() const
    {
        std::wstring tip = L"ASCIIVision Native - ";
        tip += overlayVisible_ ? L"visible" : L"hidden";
        tip += L" | ";
        tip += std::to_wstring(config_.targetFps);
        tip += L" fps | ";
        tip += config_.fontName;
        tip += L" ";
        tip += std::to_wstring(config_.fontHeight);
        tip += L"px";
        tip += L" | density ";
        tip += std::to_wstring(config_.densityPercent);
        tip += L"%";
        return tip;
    }

    void UpdateTrayIcon()
    {
        if (window_ == nullptr)
        {
            return;
        }

        trayIconData_.cbSize = sizeof(trayIconData_);
        trayIconData_.hWnd = window_;
        trayIconData_.uID = kTrayIconId;
        trayIconData_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        trayIconData_.uCallbackMessage = kTrayIconMessage;
        trayIconData_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);

        const std::wstring tip = BuildTrayToolTip();
        wcsncpy_s(trayIconData_.szTip, std::size(trayIconData_.szTip), tip.c_str(), _TRUNCATE);

        if (!trayIconAdded_)
        {
            trayIconAdded_ = Shell_NotifyIconW(NIM_ADD, &trayIconData_) == TRUE;
        }
        else
        {
            Shell_NotifyIconW(NIM_MODIFY, &trayIconData_);
        }
    }

    void RemoveTrayIcon()
    {
        if (trayIconAdded_)
        {
            Shell_NotifyIconW(NIM_DELETE, &trayIconData_);
            trayIconAdded_ = false;
        }
    }

    void ShowTrayMenu()
    {
        HMENU menu = CreatePopupMenu();
        if (menu == nullptr)
        {
            return;
        }

        AppendMenuW(
            menu,
            MF_STRING,
            kTrayCommandToggleVisibility,
            overlayVisible_ ? L"Hide ASCIIVision" : L"Show ASCIIVision");
        AppendMenuW(menu, MF_STRING, kTrayCommandReloadConfig, L"Reload Config");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kTrayCommandQuit, L"Quit");

        POINT cursor = {};
        GetCursorPos(&cursor);
        SetForegroundWindow(window_);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN, cursor.x, cursor.y, 0, window_, nullptr);
        PostMessageW(window_, WM_NULL, 0, 0);
        DestroyMenu(menu);
    }

    void RegisterHotkeys()
    {
        const BOOL toggleRegistered = RegisterHotKey(window_, kHotkeyToggle, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'A');
        const BOOL quitRegistered = RegisterHotKey(window_, kHotkeyQuit, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'Q');

        if (!toggleRegistered || !quitRegistered)
        {
            MessageBoxW(
                window_,
                L"One or more ASCIIVision hotkeys could not be registered. The tray menu will still work.",
                kWindowTitle,
                MB_ICONWARNING | MB_OK);
        }
    }

    void UnregisterHotkeys()
    {
        if (window_ != nullptr)
        {
            UnregisterHotKey(window_, kHotkeyToggle);
            UnregisterHotKey(window_, kHotkeyQuit);
        }
    }

    void ReloadConfig()
    {
        const bool wasVisible = overlayVisible_;
        StopRenderTimer();
        LoadConfig();
        SaveConfig();

        if (!BuildGlyphMasks())
        {
            ShowError(L"ASCIIVision Native", L"Failed to rebuild ASCII glyph masks after reloading config.");
        }

        const bool captureReady = InitializeCapture();
        if (!captureReady)
        {
            ShowError(L"ASCIIVision Native", lastErrorMessage_);
        }

        if (wasVisible && captureReady)
        {
            StartRenderTimer();
            ShowWindow(window_, SW_SHOWNA);
        }

        UpdateTrayIcon();
        InvalidateRect(window_, nullptr, FALSE);
    }

    void HandleDisplayChange()
    {
        const bool wasVisible = overlayVisible_;
        StopRenderTimer();

        const bool monitorReady = UpdatePrimaryMonitorRect();
        const bool captureReady = monitorReady && InitializeCapture();
        if (!captureReady)
        {
            ShowError(L"ASCIIVision Native", lastErrorMessage_.empty() ? L"Failed to recover after a display change." : lastErrorMessage_);
        }

        renderBufferValid_ = false;
        ResizeOverlayWindow();

        if (wasVisible && captureReady)
        {
            StartRenderTimer();
        }
    }

    void HandleRenderTimer()
    {
        if (!overlayVisible_)
        {
            return;
        }

        const CaptureResult result = CaptureAndRender();
        switch (result)
        {
        case CaptureResult::Rendered:
            InvalidateRect(window_, nullptr, FALSE);
            break;
        case CaptureResult::Timeout:
            break;
        case CaptureResult::AccessLost:
            if (!InitializeCapture())
            {
                StopRenderTimer();
                ShowError(L"ASCIIVision Native", lastErrorMessage_);
            }
            break;
        case CaptureResult::Fatal:
            StopRenderTimer();
            ShowError(L"ASCIIVision Native", lastErrorMessage_);
            break;
        }
    }

    void Destroy()
    {
        StopRenderTimer();
        RemoveTrayIcon();
        UnregisterHotkeys();
        ReleaseCaptureResources();
        if (asciiFont_ != nullptr)
        {
            DeleteObject(asciiFont_);
            asciiFont_ = nullptr;
        }
    }

    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == taskbarCreatedMessage_)
        {
            trayIconAdded_ = false;
            UpdateTrayIcon();
            return 0;
        }

        switch (message)
        {
        case WM_PAINT:
            Paint();
            return 0;

        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;

        case WM_NCHITTEST:
            return HTTRANSPARENT;

        case WM_ERASEBKGND:
            return 1;

        case WM_TIMER:
            if (wParam == kRenderTimerId)
            {
                HandleRenderTimer();
                return 0;
            }
            break;

        case WM_HOTKEY:
            if (wParam == kHotkeyToggle)
            {
                ToggleOverlayVisible();
                return 0;
            }
            if (wParam == kHotkeyQuit)
            {
                DestroyWindow(window_);
                return 0;
            }
            break;

        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
            case kTrayCommandToggleVisibility:
                ToggleOverlayVisible();
                return 0;
            case kTrayCommandReloadConfig:
                ReloadConfig();
                return 0;
            case kTrayCommandQuit:
                DestroyWindow(window_);
                return 0;
            }
            break;

        case kTrayIconMessage:
            if (lParam == WM_LBUTTONUP || lParam == WM_LBUTTONDBLCLK)
            {
                ToggleOverlayVisible();
                return 0;
            }
            if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU)
            {
                ShowTrayMenu();
                return 0;
            }
            break;

        case WM_DISPLAYCHANGE:
            HandleDisplayChange();
            return 0;

        case WM_CLOSE:
            DestroyWindow(window_);
            return 0;

        case WM_DESTROY:
            Destroy();
            PostQuitMessage(0);
            return 0;
        }

        return DefWindowProcW(window_, message, wParam, lParam);
    }

    static LRESULT CALLBACK StaticWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        ASCIIVisionApp* app = nullptr;

        if (message == WM_NCCREATE)
        {
            auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
            app = static_cast<ASCIIVisionApp*>(createStruct->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
            app->window_ = window;
        }
        else
        {
            app = reinterpret_cast<ASCIIVisionApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        }

        if (app != nullptr)
        {
            return app->HandleMessage(message, wParam, lParam);
        }

        return DefWindowProcW(window, message, wParam, lParam);
    }
};

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    ASCIIVisionApp app(instance);
    if (!app.Initialize())
    {
        return 1;
    }

    return app.Run();
}
