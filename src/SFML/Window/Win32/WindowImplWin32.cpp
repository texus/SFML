////////////////////////////////////////////////////////////
//
// SFML - Simple and Fast Multimedia Library
// Copyright (C) 2007-2022 Laurent Gomila (laurent@sfml-dev.org)
//
// This software is provided 'as-is', without any express or implied warranty.
// In no event will the authors be held liable for any damages arising from the use of this software.
//
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it freely,
// subject to the following restrictions:
//
// 1. The origin of this software must not be misrepresented;
//    you must not claim that you wrote the original software.
//    If you use this software in a product, an acknowledgment
//    in the product documentation would be appreciated but is not required.
//
// 2. Altered source versions must be plainly marked as such,
//    and must not be misrepresented as being the original software.
//
// 3. This notice may not be removed or altered from any source distribution.
//
////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////
// Headers
////////////////////////////////////////////////////////////
#include <SFML/System/Err.hpp>
#include <SFML/System/String.hpp>
#include <SFML/System/Utf.hpp>
#include <SFML/Window/JoystickImpl.hpp>
#include <SFML/Window/Win32/WindowImplWin32.hpp>
#include <SFML/Window/WindowStyle.hpp>

#include <cstring>
// dbt.h is lowercase here, as a cross-compile on linux with mingw-w64
// expects lowercase, and a native compile on windows, whether via msvc
// or mingw-w64 addresses files in a case insensitive manner.
#include <cassert>
#include <cmath>
#include <dbt.h>
#include <ostream>
#include <vector>

// MinGW lacks the definition of some Win32 constants
#ifndef XBUTTON1
#define XBUTTON1 0x0001
#endif
#ifndef XBUTTON2
#define XBUTTON2 0x0002
#endif
#ifndef WM_MOUSEHWHEEL
#define WM_MOUSEHWHEEL 0x020E
#endif
#ifndef MAPVK_VK_TO_VSC
#define MAPVK_VK_TO_VSC (0)
#endif

// DPI awareness events are only defined when WINVER is high enough
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif
#ifndef WM_GETDPISCALEDSIZE
#define WM_GETDPISCALEDSIZE 0x02E4
#endif

namespace
{
unsigned int               windowCount      = 0; // Windows owned by SFML
unsigned int               handleCount      = 0; // All window handles
const wchar_t*             className        = L"SFML_Window";
sf::priv::WindowImplWin32* fullscreenWindow = nullptr;

const GUID GUID_DEVINTERFACE_HID = {0x4d1e55b2, 0xf16f, 0x11cf, {0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}};
} // namespace

namespace sf
{
namespace priv
{
////////////////////////////////////////////////////////////
WindowImplWin32::WindowImplWin32(WindowHandle handle) :
m_handle(handle),
m_callback(0),
m_cursorVisible(true), // might need to call GetCursorInfo
m_lastCursor(LoadCursor(nullptr, IDC_ARROW)),
m_icon(nullptr),
m_keyRepeatEnabled(true),
m_lastSize(0, 0),
m_resizing(false),
m_surrogate(0),
m_mouseInside(false),
m_fullscreen(false),
m_cursorGrabbed(false),
m_scaleWithDpi(false),
m_dpiScale(1),
m_user32Dll(nullptr),
m_shCoreDll(nullptr),
m_adjustWindowRectExForDpiFunc(nullptr),
m_getDpiForWindowFunc(nullptr),
m_getDpiForMonitorFunc(nullptr)
{
    // Set that this process is DPI aware and can handle DPI scaling
    setProcessDpiAware();

    if (m_handle)
    {
        // If we're the first window handle, we only need to poll for joysticks when WM_DEVICECHANGE message is received
        if (handleCount == 0)
            JoystickImpl::setLazyUpdates(true);

        ++handleCount;

        // We change the event procedure of the control (it is important to save the old one)
        SetWindowLongPtrW(m_handle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        m_callback = SetWindowLongPtrW(m_handle, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WindowImplWin32::globalOnEvent));
    }
}


////////////////////////////////////////////////////////////
WindowImplWin32::WindowImplWin32(VideoMode mode, const String& title, std::uint32_t style, const ContextSettings& /*settings*/) :
m_handle(nullptr),
m_callback(0),
m_cursorVisible(true), // might need to call GetCursorInfo
m_lastCursor(LoadCursor(nullptr, IDC_ARROW)),
m_icon(nullptr),
m_keyRepeatEnabled(true),
m_lastSize(mode.size),
m_resizing(false),
m_surrogate(0),
m_mouseInside(false),
m_fullscreen((style & Style::Fullscreen) != 0),
m_cursorGrabbed(m_fullscreen),
m_scaleWithDpi(mode.scaleWithDpi),
m_dpiScale(1),
m_user32Dll(nullptr),
m_shCoreDll(nullptr),
m_adjustWindowRectExForDpiFunc(nullptr),
m_getDpiForWindowFunc(nullptr),
m_getDpiForMonitorFunc(nullptr)
{
    // Set that this process is DPI aware and can handle DPI scaling
    setProcessDpiAware();

    // Register the window class at first call
    if (windowCount == 0)
        registerWindowClass();

    // Compute position and size
    HDC screenDC = GetDC(nullptr);
    int left     = (GetDeviceCaps(screenDC, HORZRES) - static_cast<int>(mode.size.x)) / 2;
    int top      = (GetDeviceCaps(screenDC, VERTRES) - static_cast<int>(mode.size.y)) / 2;
    int width    = static_cast<int>(mode.size.x);
    int height   = static_cast<int>(mode.size.y);
    ReleaseDC(nullptr, screenDC);

    // Choose the window style according to the Style parameter
    DWORD win32Style = WS_VISIBLE;
    if (style == Style::None)
    {
        win32Style |= WS_POPUP;
    }
    else
    {
        if (style & Style::Titlebar)
            win32Style |= WS_CAPTION | WS_MINIMIZEBOX;
        if (style & Style::Resize)
            win32Style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
        if (style & Style::Close)
            win32Style |= WS_SYSMENU;
    }

    Vector2u scaledSize = mode.size;
    if (!m_fullscreen)
    {
        // Calculate the correct window size when High DPI support is is enabled
        UINT dpi = 96;
        if (m_scaleWithDpi)
        {
            RECT unscaledWindowRect;
            unscaledWindowRect.left   = left;
            unscaledWindowRect.top    = top;
            unscaledWindowRect.right  = left + width;
            unscaledWindowRect.bottom = top + height;

            if (m_getDpiForMonitorFunc && ((m_dpiAwareness == DpiAwareness::PerMonitorAwareV1) ||
                                           (m_dpiAwareness == DpiAwareness::PerMonitorAwareV2)))
            {
                // We first need to figure out on which monitor the window will appear,
                // as each monitor can have its own scaling.
                const HMONITOR monitor = MonitorFromRect(&unscaledWindowRect, MONITOR_DEFAULTTONEAREST);

                UINT dpiX;
                UINT dpiY;
                if (m_getDpiForMonitorFunc(monitor, MDTEffectiveDpi, &dpiX, &dpiY) == S_OK)
                    dpi = dpiY; // dpiX and dpiY are always identical
                else
                    dpi = GetSystemDPI();
            }
            else if (m_dpiAwareness == DpiAwareness::SystemAware)
            {
                dpi = GetSystemDPI();
            }

            m_dpiScale             = static_cast<float>(dpi) / 96.f;
            const int scaledWidth  = static_cast<int>(std::round(width * m_dpiScale));
            const int scaledHeight = static_cast<int>(std::round(height * m_dpiScale));

            left -= (scaledWidth - width) / 2;
            top -= (scaledHeight - height) / 2;
            width  = scaledWidth;
            height = scaledHeight;
        }

        scaledSize.x = static_cast<unsigned int>(width);
        scaledSize.y = static_cast<unsigned int>(height);
        m_lastSize   = scaledSize;

        // In windowed mode, adjust width and height so that window will have the requested client area.
        // When DPI awareness is set to Per Monitor V2 then we also need to take the scaling of the title bar
        // into account. Simply calling AdjustWindowRect actually appears to be enough even when using
        // Per Monitor V2, but this is probably only true because the window is placed on the main monitor.
        RECT rectangle = {0, 0, width, height};
        if ((m_dpiAwareness == DpiAwareness::PerMonitorAwareV2) && m_adjustWindowRectExForDpiFunc)
            m_adjustWindowRectExForDpiFunc(&rectangle, win32Style, false, 0, dpi);
        else
            AdjustWindowRect(&rectangle, win32Style, false);

        width  = rectangle.right - rectangle.left;
        height = rectangle.bottom - rectangle.top;
    }

    // Create the window
    m_handle = CreateWindowW(className,
                             title.toWideString().c_str(),
                             win32Style,
                             left,
                             top,
                             width,
                             height,
                             nullptr,
                             nullptr,
                             GetModuleHandle(nullptr),
                             this);

    // Register to receive device interface change notifications (used for joystick connection handling)
    DEV_BROADCAST_DEVICEINTERFACE deviceInterface =
        {sizeof(DEV_BROADCAST_DEVICEINTERFACE), DBT_DEVTYP_DEVICEINTERFACE, 0, GUID_DEVINTERFACE_HID, {0}};
    RegisterDeviceNotification(m_handle, &deviceInterface, DEVICE_NOTIFY_WINDOW_HANDLE);

    // If we're the first window handle, we only need to poll for joysticks when WM_DEVICECHANGE message is received
    if (m_handle)
    {
        if (handleCount == 0)
            JoystickImpl::setLazyUpdates(true);

        ++handleCount;
    }

    // By default, the OS limits the size of the window the the desktop size,
    // we have to resize it after creation to apply the real size
    setSize(scaledSize);

    // Switch to fullscreen if requested
    if (m_fullscreen)
        switchToFullscreen(mode);

    // Increment window count
    ++windowCount;
}


////////////////////////////////////////////////////////////
WindowImplWin32::~WindowImplWin32()
{
    // TODO should we restore the cursor shape and visibility?

    // Destroy the custom icon, if any
    if (m_icon)
        DestroyIcon(m_icon);

    // If it's the last window handle we have to poll for joysticks again
    if (m_handle)
    {
        --handleCount;

        if (handleCount == 0)
            JoystickImpl::setLazyUpdates(false);
    }

    if (!m_callback)
    {
        // Destroy the window
        if (m_handle)
            DestroyWindow(m_handle);

        // Decrement the window count
        --windowCount;

        // Unregister window class if we were the last window
        if (windowCount == 0)
            UnregisterClassW(className, GetModuleHandleW(nullptr));
    }
    else
    {
        // The window is external: remove the hook on its message callback
        SetWindowLongPtrW(m_handle, GWLP_WNDPROC, m_callback);
    }

    if (m_shCoreDll)
        FreeLibrary(m_shCoreDll);
    if (m_user32Dll)
        FreeLibrary(m_user32Dll);
}


////////////////////////////////////////////////////////////
WindowHandle WindowImplWin32::getSystemHandle() const
{
    return m_handle;
}


////////////////////////////////////////////////////////////
void WindowImplWin32::processEvents()
{
    // We process the window events only if we own it
    if (!m_callback)
    {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
}


////////////////////////////////////////////////////////////
Vector2i WindowImplWin32::getPosition() const
{
    RECT rect;
    GetWindowRect(m_handle, &rect);

    return Vector2i(rect.left, rect.top);
}


////////////////////////////////////////////////////////////
void WindowImplWin32::setPosition(const Vector2i& position)
{
    SetWindowPos(m_handle, nullptr, position.x, position.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

    if (m_cursorGrabbed)
        grabCursor(true);
}


////////////////////////////////////////////////////////////
Vector2u WindowImplWin32::getSize() const
{
    RECT rect;
    GetClientRect(m_handle, &rect);

    return Vector2u(static_cast<unsigned int>(rect.right - rect.left), static_cast<unsigned int>(rect.bottom - rect.top));
}


////////////////////////////////////////////////////////////
void WindowImplWin32::setSize(const Vector2u& size)
{
    // SetWindowPos wants the total size of the window (including title bar and borders),
    // so we have to compute it
    RECT rectangle = {0, 0, static_cast<long>(size.x), static_cast<long>(size.y)};
    AdjustWindowRect(&rectangle, static_cast<DWORD>(GetWindowLongPtr(m_handle, GWL_STYLE)), false);
    int width  = rectangle.right - rectangle.left;
    int height = rectangle.bottom - rectangle.top;

    SetWindowPos(m_handle, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER);
}


////////////////////////////////////////////////////////////
void WindowImplWin32::setTitle(const String& title)
{
    SetWindowTextW(m_handle, title.toWideString().c_str());
}


////////////////////////////////////////////////////////////
void WindowImplWin32::setIcon(const Vector2u& size, const std::uint8_t* pixels)
{
    // First destroy the previous one
    if (m_icon)
        DestroyIcon(m_icon);

    // Windows wants BGRA pixels: swap red and blue channels
    std::vector<std::uint8_t> iconPixels(size.x * size.y * 4);
    for (std::size_t i = 0; i < iconPixels.size() / 4; ++i)
    {
        iconPixels[i * 4 + 0] = pixels[i * 4 + 2];
        iconPixels[i * 4 + 1] = pixels[i * 4 + 1];
        iconPixels[i * 4 + 2] = pixels[i * 4 + 0];
        iconPixels[i * 4 + 3] = pixels[i * 4 + 3];
    }

    // Create the icon from the pixel array
    m_icon = CreateIcon(GetModuleHandleW(nullptr),
                        static_cast<int>(size.x),
                        static_cast<int>(size.y),
                        1,
                        32,
                        nullptr,
                        iconPixels.data());

    // Set it as both big and small icon of the window
    if (m_icon)
    {
        SendMessageW(m_handle, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(m_icon));
        SendMessageW(m_handle, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(m_icon));
    }
    else
    {
        err() << "Failed to set the window's icon" << std::endl;
    }
}


////////////////////////////////////////////////////////////
void WindowImplWin32::setVisible(bool visible)
{
    ShowWindow(m_handle, visible ? SW_SHOW : SW_HIDE);
}


////////////////////////////////////////////////////////////
void WindowImplWin32::setMouseCursorVisible(bool visible)
{
    m_cursorVisible = visible;
    SetCursor(m_cursorVisible ? m_lastCursor : nullptr);
}


////////////////////////////////////////////////////////////
void WindowImplWin32::setMouseCursorGrabbed(bool grabbed)
{
    m_cursorGrabbed = grabbed;
    grabCursor(m_cursorGrabbed);
}


////////////////////////////////////////////////////////////
void WindowImplWin32::setMouseCursor(const CursorImpl& cursor)
{
    m_lastCursor = static_cast<HCURSOR>(cursor.m_cursor);
    SetCursor(m_cursorVisible ? m_lastCursor : nullptr);
}


////////////////////////////////////////////////////////////
void WindowImplWin32::setKeyRepeatEnabled(bool enabled)
{
    m_keyRepeatEnabled = enabled;
}


////////////////////////////////////////////////////////////
void WindowImplWin32::requestFocus()
{
    // Allow focus stealing only within the same process; compare PIDs of current and foreground window
    DWORD thisPid, foregroundPid;
    GetWindowThreadProcessId(m_handle, &thisPid);
    GetWindowThreadProcessId(GetForegroundWindow(), &foregroundPid);

    if (thisPid == foregroundPid)
    {
        // The window requesting focus belongs to the same process as the current window: steal focus
        SetForegroundWindow(m_handle);
    }
    else
    {
        // Different process: don't steal focus, but create a taskbar notification ("flash")
        FLASHWINFO info;
        info.cbSize    = sizeof(info);
        info.hwnd      = m_handle;
        info.dwFlags   = FLASHW_TRAY;
        info.dwTimeout = 0;
        info.uCount    = 3;

        FlashWindowEx(&info);
    }
}


////////////////////////////////////////////////////////////
bool WindowImplWin32::hasFocus() const
{
    return m_handle == GetForegroundWindow();
}


////////////////////////////////////////////////////////////
float WindowImplWin32::getDpiScale() const
{
    return m_dpiScale;
}


////////////////////////////////////////////////////////////
void WindowImplWin32::setProcessDpiAware()
{
    assert(!m_user32Dll && !m_shCoreDll);
    m_user32Dll = LoadLibrary(L"user32.dll");
    m_shCoreDll = LoadLibrary(L"Shcore.dll");

    // Load some helper function which we may need later for DPI calculations
    if (m_user32Dll)
    {
        // AdjustWindowRectExForDpi is used for calculations when using Per Monitor V2.
        // GetDpiForWindow is not a necessity, we fall back to GetDpiForMonitor when unavailable.
        // These functions are available in Windows 10 version 1607 or newer.
        m_adjustWindowRectExForDpiFunc = reinterpret_cast<AdjustWindowRectExForDpiFuncType>(
            reinterpret_cast<void*>(GetProcAddress(m_user32Dll, "AdjustWindowRectExForDpi")));
        m_getDpiForWindowFunc = reinterpret_cast<GetDpiForWindowFuncType>(
            reinterpret_cast<void*>(GetProcAddress(m_user32Dll, "GetDpiForWindow")));
    }
    if (m_shCoreDll)
    {
        // We need GetDpiForMonitor to figure out the correct size of the window when
        // creating it with HighDPI support or when GetDpiForWindow is unavailable.
        // GetDpiForMonitor is available in Windows 8.1 or newer.
        m_getDpiForMonitorFunc = reinterpret_cast<GetDpiForMonitorFuncType>(
            reinterpret_cast<void*>(GetProcAddress(m_shCoreDll, "GetDpiForMonitor")));
    }

    // Try enabling Per Monitor V2 DPI awareness using SetProcessDpiAwarenessContext first.
    // SetProcessDpiAwarenessContext is only supported on Windows 10 version 1703 or newer.
    if (m_user32Dll)
    {
        // The function parameter actually has type DPI_AWARENESS_CONTEXT instead of void*,
        // but DPI_AWARENESS_CONTEXT is declared as a kind of HANDLE and the value passed
        // to the function is actually an integer. So the exact pointer type is not important.
        using SetProcessDpiAwarenessContextFuncType = BOOL(WINAPI*)(void*);
        auto setProcessDpiAwarenessContextFunc      = reinterpret_cast<SetProcessDpiAwarenessContextFuncType>(
            reinterpret_cast<void*>(GetProcAddress(m_user32Dll, "SetProcessDpiAwarenessContext")));

        // Only set the DPI awareness to Per Monitor V2 if our helper function that are needed
        // later were also properly loaded. We would need to know in globalOnEvent whether these
        // helper functions exist, but they are not available there. By only allowing PerMonitorAwareV2
        // to be used when the helper functions are found, we can simply check if we have V2 in globalOnEvent.
        // Technically loading the helper functions should never fail when SetProcessDpiAwarenessContext exists.
        if (setProcessDpiAwarenessContextFunc && m_adjustWindowRectExForDpiFunc && m_getDpiForWindowFunc)
        {
            enum class DpiAwarenessContext
            {
                Unaware           = -1,
                SystemAware       = -2,
                PerMonitorAware   = -3,
                PerMonitorAwareV2 = -4,
                UnawareGdiScaled  = -5
            };

            // When High-DPI scaling is not enabled and the window should keep a constant size across monitors,
            // then we won't bother with enabling Per Monitor V2 DPI awareness. Enabling V2 would automatically
            // change the title bar height which forces us to manually recalculate the window size when
            // receiving the WM_GETDPISCALEDSIZE event. Occationally this event didn't trigger (tested on Windows 21H2),
            // which caused the window size to suddenly be scaled according to the DPI instead of remaining constant.
            // Per Monitor V1 doesn't have this issue, and it makes sense to not scale the title when not scaling the window.
            // With High-DPI scaling we will use V2, but missing the WM_GETDPISCALEDSIZE event would result in
            // the window size changing by just a few pixels.
            const auto dpiMode = m_scaleWithDpi ? DpiAwarenessContext::PerMonitorAwareV2
                                                : DpiAwarenessContext::PerMonitorAware;

            if (!setProcessDpiAwarenessContextFunc(reinterpret_cast<void*>(static_cast<std::ptrdiff_t>(dpiMode))))
                sf::err() << "Failed to set process DPI awareness with SetProcessDpiAwarenessContext" << std::endl;
            else
            {
                if (m_scaleWithDpi)
                    m_dpiAwareness = DpiAwareness::PerMonitorAwareV2;
                else
                {
                    m_dpiAwareness = DpiAwareness::PerMonitorAwareV1;

                    // If we aren't using PerMonitorAwareV2 then we don't need to keep the User32.dll available the whole time
                    FreeLibrary(m_user32Dll);
                    m_user32Dll = nullptr;
                }
                return;
            }
        }

        // If SetProcessDpiAwarenessContext doesn't exist then we don't need to keep the user32.dll available the whole time
        m_adjustWindowRectExForDpiFunc = nullptr;
        m_getDpiForWindowFunc          = nullptr;

        FreeLibrary(m_user32Dll);
        m_user32Dll = nullptr;
    }

    // Try enabling Per Monitor DPI awareness using SetProcessDpiAwareness
    // if SetProcessDpiAwarenessContext is not available on this system.
    // SetProcessDpiAwareness is only supported on Windows 8.1 and newer.
    if (m_shCoreDll)
    {
        enum ProcessDpiAwareness
        {
            ProcessDpiUnaware         = 0,
            ProcessSystemDpiAware     = 1,
            ProcessPerMonitorDpiAware = 2
        };

        using SetProcessDpiAwarenessFuncType = HRESULT(WINAPI*)(ProcessDpiAwareness);
        auto setProcessDpiAwarenessFunc      = reinterpret_cast<SetProcessDpiAwarenessFuncType>(
            reinterpret_cast<void*>(GetProcAddress(m_shCoreDll, "SetProcessDpiAwareness")));

        if (setProcessDpiAwarenessFunc)
        {
            // We only check for E_INVALIDARG because we would get
            // E_ACCESSDENIED if the DPI was already set previously
            // and S_OK means the call was successful
            if (setProcessDpiAwarenessFunc(ProcessPerMonitorDpiAware) == E_INVALIDARG)
            {
                sf::err() << "Failed to set process DPI awareness with SetProcessDpiAwareness" << std::endl;
            }
            else
            {
                m_dpiAwareness = DpiAwareness::PerMonitorAwareV1;
                return;
            }
        }

        // If SetProcessDpiAwareness doesn't exist then we don't need to keep the shcore.dll available the whole time
        m_getDpiForMonitorFunc = nullptr;
        FreeLibrary(m_shCoreDll);
        m_shCoreDll = nullptr;
    }

    // Fall back to enabling System DPI awareness using SetProcessDPIAware
    // if SetProcessDpiAwareness is not available on this system.
    // SetProcessDPIAware is only supported on Windows Vista and newer.
    HINSTANCE user32Dll = LoadLibrary(L"user32.dll");

    if (user32Dll)
    {
        using SetProcessDPIAwareFuncType = BOOL(WINAPI*)();
        auto setProcessDPIAwareFunc      = reinterpret_cast<SetProcessDPIAwareFuncType>(
            reinterpret_cast<void*>(GetProcAddress(user32Dll, "SetProcessDPIAware")));

        if (setProcessDPIAwareFunc)
        {
            if (!setProcessDPIAwareFunc())
                sf::err() << "Failed to set process DPI awareness with SetProcessDPIAware" << std::endl;
            else
            {
                m_dpiAwareness = DpiAwareness::SystemAware;
                FreeLibrary(user32Dll);
                return;
            }
        }

        FreeLibrary(user32Dll);
    }

    m_dpiAwareness = DpiAwareness::Unaware;
}


////////////////////////////////////////////////////////////
UINT WindowImplWin32::GetWindowDPI() const
{
    // Use GetDpiForWindow on Windows 10 version 1607 or newer
    if (m_getDpiForWindowFunc)
    {
        const UINT dpi = m_getDpiForWindowFunc(m_handle);
        if (dpi != 0)
            return dpi;
    }

    // Use GetDpiForMonitor on Windows 8.1 or newer
    if (m_getDpiForMonitorFunc)
    {
        UINT           dpiX;
        UINT           dpiY;
        const HMONITOR monitor = MonitorFromWindow(m_handle, MONITOR_DEFAULTTONEAREST);
        if (m_getDpiForMonitorFunc(monitor, MDTEffectiveDpi, &dpiX, &dpiY) == S_OK)
            return dpiY; // dpiX and dpiY are always identical
    }

    // When the DPI of the monitor can't be found then fall back to system DPI
    return GetSystemDPI();
}


////////////////////////////////////////////////////////////
UINT WindowImplWin32::GetSystemDPI() const
{
    const HDC hdc = GetDC(nullptr);
    if (hdc)
    {
        int dpi = GetDeviceCaps(hdc, LOGPIXELSY);
        ReleaseDC(NULL, hdc);
        if (dpi > 0)
            return static_cast<UINT>(dpi);
    }

    // Return the DPI that corresponds to 100% scaling in the unlikely event that GetDC fails
    return 96;
}


////////////////////////////////////////////////////////////
void WindowImplWin32::registerWindowClass()
{
    WNDCLASSW windowClass;
    windowClass.style         = 0;
    windowClass.lpfnWndProc   = &WindowImplWin32::globalOnEvent;
    windowClass.cbClsExtra    = 0;
    windowClass.cbWndExtra    = 0;
    windowClass.hInstance     = GetModuleHandleW(nullptr);
    windowClass.hIcon         = nullptr;
    windowClass.hCursor       = nullptr;
    windowClass.hbrBackground = nullptr;
    windowClass.lpszMenuName  = nullptr;
    windowClass.lpszClassName = className;
    RegisterClassW(&windowClass);
}


////////////////////////////////////////////////////////////
void WindowImplWin32::switchToFullscreen(const VideoMode& mode)
{
    DEVMODE devMode;
    devMode.dmSize       = sizeof(devMode);
    devMode.dmPelsWidth  = mode.size.x;
    devMode.dmPelsHeight = mode.size.y;
    devMode.dmBitsPerPel = mode.bitsPerPixel;
    devMode.dmFields     = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;

    // Apply fullscreen mode
    if (ChangeDisplaySettingsW(&devMode, CDS_FULLSCREEN) != DISP_CHANGE_SUCCESSFUL)
    {
        err() << "Failed to change display mode for fullscreen" << std::endl;
        return;
    }

    // Make the window flags compatible with fullscreen mode
    SetWindowLongPtr(m_handle,
                     GWL_STYLE,
                     static_cast<LONG_PTR>(WS_POPUP) | static_cast<LONG_PTR>(WS_CLIPCHILDREN) |
                         static_cast<LONG_PTR>(WS_CLIPSIBLINGS));
    SetWindowLongPtr(m_handle, GWL_EXSTYLE, WS_EX_APPWINDOW);

    // Resize the window so that it fits the entire screen
    SetWindowPos(m_handle, HWND_TOP, 0, 0, static_cast<int>(mode.size.x), static_cast<int>(mode.size.y), SWP_FRAMECHANGED);
    ShowWindow(m_handle, SW_SHOW);

    // Set "this" as the current fullscreen window
    fullscreenWindow = this;
}


////////////////////////////////////////////////////////////
void WindowImplWin32::cleanup()
{
    // Restore the previous video mode (in case we were running in fullscreen)
    if (fullscreenWindow == this)
    {
        ChangeDisplaySettingsW(nullptr, 0);
        fullscreenWindow = nullptr;
    }

    // Unhide the mouse cursor (in case it was hidden)
    setMouseCursorVisible(true);

    // No longer track the cursor
    setTracking(false);

    // No longer capture the cursor
    ReleaseCapture();
}


////////////////////////////////////////////////////////////
void WindowImplWin32::setTracking(bool track)
{
    TRACKMOUSEEVENT mouseEvent;
    mouseEvent.cbSize      = sizeof(TRACKMOUSEEVENT);
    mouseEvent.dwFlags     = track ? TME_LEAVE : TME_CANCEL;
    mouseEvent.hwndTrack   = m_handle;
    mouseEvent.dwHoverTime = HOVER_DEFAULT;
    TrackMouseEvent(&mouseEvent);
}


////////////////////////////////////////////////////////////
void WindowImplWin32::grabCursor(bool grabbed)
{
    if (grabbed)
    {
        RECT rect;
        GetClientRect(m_handle, &rect);
        MapWindowPoints(m_handle, nullptr, reinterpret_cast<LPPOINT>(&rect), 2);
        ClipCursor(&rect);
    }
    else
    {
        ClipCursor(nullptr);
    }
}


////////////////////////////////////////////////////////////
void WindowImplWin32::processEvent(UINT message, WPARAM wParam, LPARAM lParam)
{
    // Don't process any message until window is created
    if (m_handle == nullptr)
        return;

    switch (message)
    {
        // Destroy event
        case WM_DESTROY:
        {
            // Here we must cleanup resources !
            cleanup();
            break;
        }

        // Set cursor event
        case WM_SETCURSOR:
        {
            // The mouse has moved, if the cursor is in our window we must refresh the cursor
            if (LOWORD(lParam) == HTCLIENT)
            {
                SetCursor(m_cursorVisible ? m_lastCursor : nullptr);
            }

            break;
        }

        // Close event
        case WM_CLOSE:
        {
            Event event;
            event.type = Event::Closed;
            pushEvent(event);
            break;
        }

        // Resize event
        case WM_SIZE:
        {
            // Consider only events triggered by a maximize or a un-maximize
            if (wParam != SIZE_MINIMIZED && !m_resizing && m_lastSize != getSize())
            {
                // Update the last handled size
                m_lastSize = getSize();

                // Push a resize event
                Event event;
                event.type        = Event::Resized;
                event.size.width  = m_lastSize.x;
                event.size.height = m_lastSize.y;
                pushEvent(event);

                // Restore/update cursor grabbing
                grabCursor(m_cursorGrabbed);
            }
            break;
        }

        // Start resizing
        case WM_ENTERSIZEMOVE:
        {
            m_resizing = true;
            grabCursor(false);
            break;
        }

        // Stop resizing
        case WM_EXITSIZEMOVE:
        {
            m_resizing = false;

            // Ignore cases where the window has only been moved
            if (m_lastSize != getSize())
            {
                // Update the last handled size
                m_lastSize = getSize();

                // Push a resize event
                Event event;
                event.type        = Event::Resized;
                event.size.width  = m_lastSize.x;
                event.size.height = m_lastSize.y;
                pushEvent(event);
            }

            // Restore/update cursor grabbing
            grabCursor(m_cursorGrabbed);
            break;
        }

        // The system request the min/max window size and position
        case WM_GETMINMAXINFO:
        {
            // We override the returned information to remove the default limit
            // (the OS doesn't allow windows bigger than the desktop by default)
            auto* info             = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMaxTrackSize.x = 50000;
            info->ptMaxTrackSize.y = 50000;
            break;
        }

        // Gain focus event
        case WM_SETFOCUS:
        {
            // Restore cursor grabbing
            grabCursor(m_cursorGrabbed);

            Event event;
            event.type = Event::GainedFocus;
            pushEvent(event);
            break;
        }

        // Lost focus event
        case WM_KILLFOCUS:
        {
            // Ungrab the cursor
            grabCursor(false);

            Event event;
            event.type = Event::LostFocus;
            pushEvent(event);
            break;
        }

        // Text event
        case WM_CHAR:
        {
            if (m_keyRepeatEnabled || ((lParam & (1 << 30)) == 0))
            {
                // Get the code of the typed character
                auto character = static_cast<std::uint32_t>(wParam);

                // Check if it is the first part of a surrogate pair, or a regular character
                if ((character >= 0xD800) && (character <= 0xDBFF))
                {
                    // First part of a surrogate pair: store it and wait for the second one
                    m_surrogate = static_cast<std::uint16_t>(character);
                }
                else
                {
                    // Check if it is the second part of a surrogate pair, or a regular character
                    if ((character >= 0xDC00) && (character <= 0xDFFF))
                    {
                        // Convert the UTF-16 surrogate pair to a single UTF-32 value
                        std::uint16_t utf16[] = {m_surrogate, static_cast<std::uint16_t>(character)};
                        sf::Utf16::toUtf32(utf16, utf16 + 2, &character);
                        m_surrogate = 0;
                    }

                    // Send a TextEntered event
                    Event event;
                    event.type         = Event::TextEntered;
                    event.text.unicode = character;
                    pushEvent(event);
                }
            }
            break;
        }

        // Keydown event
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        {
            if (m_keyRepeatEnabled || ((HIWORD(lParam) & KF_REPEAT) == 0))
            {
                Event event;
                event.type        = Event::KeyPressed;
                event.key.alt     = HIWORD(GetKeyState(VK_MENU)) != 0;
                event.key.control = HIWORD(GetKeyState(VK_CONTROL)) != 0;
                event.key.shift   = HIWORD(GetKeyState(VK_SHIFT)) != 0;
                event.key.system  = HIWORD(GetKeyState(VK_LWIN)) || HIWORD(GetKeyState(VK_RWIN));
                event.key.code    = virtualKeyCodeToSF(wParam, lParam);
                pushEvent(event);
            }
            break;
        }

        // Keyup event
        case WM_KEYUP:
        case WM_SYSKEYUP:
        {
            Event event;
            event.type        = Event::KeyReleased;
            event.key.alt     = HIWORD(GetKeyState(VK_MENU)) != 0;
            event.key.control = HIWORD(GetKeyState(VK_CONTROL)) != 0;
            event.key.shift   = HIWORD(GetKeyState(VK_SHIFT)) != 0;
            event.key.system  = HIWORD(GetKeyState(VK_LWIN)) || HIWORD(GetKeyState(VK_RWIN));
            event.key.code    = virtualKeyCodeToSF(wParam, lParam);
            pushEvent(event);
            break;
        }

        // Vertical mouse wheel event
        case WM_MOUSEWHEEL:
        {
            // Mouse position is in screen coordinates, convert it to window coordinates
            POINT position;
            position.x = static_cast<std::int16_t>(LOWORD(lParam));
            position.y = static_cast<std::int16_t>(HIWORD(lParam));
            ScreenToClient(m_handle, &position);

            auto delta = static_cast<std::int16_t>(HIWORD(wParam));

            Event event;

            event.type                   = Event::MouseWheelScrolled;
            event.mouseWheelScroll.wheel = Mouse::VerticalWheel;
            event.mouseWheelScroll.delta = static_cast<float>(delta) / 120.f;
            event.mouseWheelScroll.x     = position.x;
            event.mouseWheelScroll.y     = position.y;
            pushEvent(event);
            break;
        }

        // Horizontal mouse wheel event
        case WM_MOUSEHWHEEL:
        {
            // Mouse position is in screen coordinates, convert it to window coordinates
            POINT position;
            position.x = static_cast<std::int16_t>(LOWORD(lParam));
            position.y = static_cast<std::int16_t>(HIWORD(lParam));
            ScreenToClient(m_handle, &position);

            auto delta = static_cast<std::int16_t>(HIWORD(wParam));

            Event event;
            event.type                   = Event::MouseWheelScrolled;
            event.mouseWheelScroll.wheel = Mouse::HorizontalWheel;
            event.mouseWheelScroll.delta = -static_cast<float>(delta) / 120.f;
            event.mouseWheelScroll.x     = position.x;
            event.mouseWheelScroll.y     = position.y;
            pushEvent(event);
            break;
        }

        // Mouse left button down event
        case WM_LBUTTONDOWN:
        {
            Event event;
            event.type               = Event::MouseButtonPressed;
            event.mouseButton.button = Mouse::Left;
            event.mouseButton.x      = static_cast<std::int16_t>(LOWORD(lParam));
            event.mouseButton.y      = static_cast<std::int16_t>(HIWORD(lParam));
            pushEvent(event);
            break;
        }

        // Mouse left button up event
        case WM_LBUTTONUP:
        {
            Event event;
            event.type               = Event::MouseButtonReleased;
            event.mouseButton.button = Mouse::Left;
            event.mouseButton.x      = static_cast<std::int16_t>(LOWORD(lParam));
            event.mouseButton.y      = static_cast<std::int16_t>(HIWORD(lParam));
            pushEvent(event);
            break;
        }

        // Mouse right button down event
        case WM_RBUTTONDOWN:
        {
            Event event;
            event.type               = Event::MouseButtonPressed;
            event.mouseButton.button = Mouse::Right;
            event.mouseButton.x      = static_cast<std::int16_t>(LOWORD(lParam));
            event.mouseButton.y      = static_cast<std::int16_t>(HIWORD(lParam));
            pushEvent(event);
            break;
        }

        // Mouse right button up event
        case WM_RBUTTONUP:
        {
            Event event;
            event.type               = Event::MouseButtonReleased;
            event.mouseButton.button = Mouse::Right;
            event.mouseButton.x      = static_cast<std::int16_t>(LOWORD(lParam));
            event.mouseButton.y      = static_cast<std::int16_t>(HIWORD(lParam));
            pushEvent(event);
            break;
        }

        // Mouse wheel button down event
        case WM_MBUTTONDOWN:
        {
            Event event;
            event.type               = Event::MouseButtonPressed;
            event.mouseButton.button = Mouse::Middle;
            event.mouseButton.x      = static_cast<std::int16_t>(LOWORD(lParam));
            event.mouseButton.y      = static_cast<std::int16_t>(HIWORD(lParam));
            pushEvent(event);
            break;
        }

        // Mouse wheel button up event
        case WM_MBUTTONUP:
        {
            Event event;
            event.type               = Event::MouseButtonReleased;
            event.mouseButton.button = Mouse::Middle;
            event.mouseButton.x      = static_cast<std::int16_t>(LOWORD(lParam));
            event.mouseButton.y      = static_cast<std::int16_t>(HIWORD(lParam));
            pushEvent(event);
            break;
        }

        // Mouse X button down event
        case WM_XBUTTONDOWN:
        {
            Event event;
            event.type               = Event::MouseButtonPressed;
            event.mouseButton.button = HIWORD(wParam) == XBUTTON1 ? Mouse::XButton1 : Mouse::XButton2;
            event.mouseButton.x      = static_cast<std::int16_t>(LOWORD(lParam));
            event.mouseButton.y      = static_cast<std::int16_t>(HIWORD(lParam));
            pushEvent(event);
            break;
        }

        // Mouse X button up event
        case WM_XBUTTONUP:
        {
            Event event;
            event.type               = Event::MouseButtonReleased;
            event.mouseButton.button = HIWORD(wParam) == XBUTTON1 ? Mouse::XButton1 : Mouse::XButton2;
            event.mouseButton.x      = static_cast<std::int16_t>(LOWORD(lParam));
            event.mouseButton.y      = static_cast<std::int16_t>(HIWORD(lParam));
            pushEvent(event);
            break;
        }

        // Mouse leave event
        case WM_MOUSELEAVE:
        {
            // Avoid this firing a second time in case the cursor is dragged outside
            if (m_mouseInside)
            {
                m_mouseInside = false;

                // Generate a MouseLeft event
                Event event;
                event.type = Event::MouseLeft;
                pushEvent(event);
            }
            break;
        }

        // Mouse move event
        case WM_MOUSEMOVE:
        {
            // Extract the mouse local coordinates
            int x = static_cast<std::int16_t>(LOWORD(lParam));
            int y = static_cast<std::int16_t>(HIWORD(lParam));

            // Get the client area of the window
            RECT area;
            GetClientRect(m_handle, &area);

            // Capture the mouse in case the user wants to drag it outside
            if ((wParam & (MK_LBUTTON | MK_MBUTTON | MK_RBUTTON | MK_XBUTTON1 | MK_XBUTTON2)) == 0)
            {
                // Only release the capture if we really have it
                if (GetCapture() == m_handle)
                    ReleaseCapture();
            }
            else if (GetCapture() != m_handle)
            {
                // Set the capture to continue receiving mouse events
                SetCapture(m_handle);
            }

            // If the cursor is outside the client area...
            if ((x < area.left) || (x > area.right) || (y < area.top) || (y > area.bottom))
            {
                // and it used to be inside, the mouse left it.
                if (m_mouseInside)
                {
                    m_mouseInside = false;

                    // No longer care for the mouse leaving the window
                    setTracking(false);

                    // Generate a MouseLeft event
                    Event event;
                    event.type = Event::MouseLeft;
                    pushEvent(event);
                }
            }
            else
            {
                // and vice-versa
                if (!m_mouseInside)
                {
                    m_mouseInside = true;

                    // Look for the mouse leaving the window
                    setTracking(true);

                    // Generate a MouseEntered event
                    Event event;
                    event.type = Event::MouseEntered;
                    pushEvent(event);
                }
            }

            // Generate a MouseMove event
            Event event;
            event.type        = Event::MouseMoved;
            event.mouseMove.x = x;
            event.mouseMove.y = y;
            pushEvent(event);
            break;
        }

        // Hardware configuration change event
        case WM_DEVICECHANGE:
        {
            // Some sort of device change has happened, update joystick connections
            if ((wParam == DBT_DEVICEARRIVAL) || (wParam == DBT_DEVICEREMOVECOMPLETE))
            {
                // Some sort of device change has happened, update joystick connections if it is a device interface
                auto* deviceBroadcastHeader = reinterpret_cast<DEV_BROADCAST_HDR*>(lParam);

                if (deviceBroadcastHeader && (deviceBroadcastHeader->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE))
                    JoystickImpl::updateConnections();
            }

            break;
        }

        // Window dimension change event (Per Monitor v2 DPI awareness, Windows 10 version 1703 or newer)
        case WM_GETDPISCALEDSIZE:
        {
            // If we used Per Monitor V2 as DPI awareness mode then the non-client area portions
            // of the window (e.g. the title bar) will automatically be scaled.
            // This causes the ratio of the client area to change when switching monitor,
            // as Windows scales the entire window (including decorations) linearly instead of
            // just scaling the client area linearly.
            // So we manually calculate the window size to scale the contents properly.
            if ((m_dpiAwareness == DpiAwareness::PerMonitorAwareV2) && m_adjustWindowRectExForDpiFunc &&
                m_getDpiForWindowFunc)
            {
                const DWORD style      = GetWindowLong(m_handle, GWL_STYLE);
                const DWORD exStyle    = m_fullscreen ? WS_EX_APPWINDOW : 0;
                const BOOL  menu       = (GetMenu(m_handle) != NULL);
                const UINT  prevDPI    = m_getDpiForWindowFunc(m_handle);
                const UINT  nextDPI    = static_cast<UINT>(wParam);
                SIZE*       windowSize = reinterpret_cast<SIZE*>(lParam);

                // Subtract the window decoration from the window size,
                // using the old DPI as the window hasn't been rescaled yet.
                RECT source = {0};
                m_adjustWindowRectExForDpiFunc(&source, style, FALSE, exStyle, prevDPI);
                windowSize->cx -= -source.left + source.right;
                windowSize->cy -= -source.top + source.bottom;

                // We now have the size of the client area, rescale it to the new DPI
                if (m_scaleWithDpi)
                {
                    const float dpiScale = static_cast<float>(nextDPI) / static_cast<float>(prevDPI);
                    windowSize->cx       = static_cast<LONG>(std::round(windowSize->cx * dpiScale));
                    windowSize->cy       = static_cast<LONG>(std::round(windowSize->cy * dpiScale));
                }

                // Add the window decoration that will be present with the new DPI
                // to get the total window size.
                RECT target = {0};
                m_adjustWindowRectExForDpiFunc(&target, style, menu, exStyle, nextDPI);
                windowSize->cx += -target.left + target.right;
                windowSize->cy += -target.top + target.bottom;
            }

            break;
        }

        // DPI change event (Windows 8.1 or newer)
        case WM_DPICHANGED:
        {
            const float prevDpiScale = m_dpiScale;
            m_dpiScale               = static_cast<float>(LOWORD(wParam)) / 96.f;

            // Changing the DPI of a monitor containing a fullscreen window was not tested,
            // we currently just ignore the event in such a case.
            if (m_fullscreen)
                break;

            // When using PerMonitorAwareV2, we should have already received a WM_GETDPISCALEDSIZE event
            // that calculated the new window size, so we don't need any calculations here.
            // Note that during tested on Windows 21H2 it occationally happened that the WM_GETDPISCALEDSIZE
            // wasn't received and the code below scales the window linearly based on its total size
            // including decorations, as opposed to keeping the aspect ratio of the client area.
            // Apparently WM_DPICHANGED can also be received by a call to SetWindowPos in SFML, so we
            // would need to ignore this event in such case, but this did not occur yet during testing.
            if (m_dpiAwareness == DpiAwareness::PerMonitorAwareV2)
            {
                // Resize the window to its new size
                const RECT* suggestedRect = (RECT*)lParam;
                SetWindowPos(m_handle,
                             nullptr,
                             suggestedRect->left,
                             suggestedRect->top,
                             suggestedRect->right - suggestedRect->left,
                             suggestedRect->bottom - suggestedRect->top,
                             SWP_NOACTIVATE | SWP_NOZORDER);
            }
            else if (m_scaleWithDpi)
            {
                // When using PerMonitorAwareV1 or SystemAware scaling, scale the window if the
                // user wanted high DPI support.
                const DWORD style    = GetWindowLong(m_handle, GWL_STYLE);
                const BOOL  menu     = (GetMenu(m_handle) != NULL);
                const float dpiScale = m_dpiScale / prevDpiScale;

                RECT target   = {0};
                target.right  = static_cast<LONG>(std::round(m_lastSize.x * dpiScale));
                target.bottom = static_cast<LONG>(std::round(m_lastSize.y * dpiScale));
                AdjustWindowRectEx(&target, style, menu, 0);

                const Vector2i windowSize    = {target.right - target.left, target.bottom - target.top};
                const RECT*    suggestedRect = (RECT*)lParam;
                SetWindowPos(m_handle,
                             nullptr,
                             suggestedRect->left,
                             suggestedRect->top,
                             windowSize.x,
                             windowSize.y,
                             SWP_NOACTIVATE | SWP_NOZORDER);
            }

            break;
        }
    }
}


////////////////////////////////////////////////////////////
Keyboard::Key WindowImplWin32::virtualKeyCodeToSF(WPARAM key, LPARAM flags)
{
    // clang-format off
    switch (key)
    {
        // Check the scancode to distinguish between left and right shift
        case VK_SHIFT:
        {
            static UINT lShift = MapVirtualKeyW(VK_LSHIFT, MAPVK_VK_TO_VSC);
            UINT scancode = static_cast<UINT>((flags & (0xFF << 16)) >> 16);
            return scancode == lShift ? Keyboard::LShift : Keyboard::RShift;
        }

        // Check the "extended" flag to distinguish between left and right alt
        case VK_MENU : return (HIWORD(flags) & KF_EXTENDED) ? Keyboard::RAlt : Keyboard::LAlt;

        // Check the "extended" flag to distinguish between left and right control
        case VK_CONTROL : return (HIWORD(flags) & KF_EXTENDED) ? Keyboard::RControl : Keyboard::LControl;

        // Other keys are reported properly
        case VK_LWIN:       return Keyboard::LSystem;
        case VK_RWIN:       return Keyboard::RSystem;
        case VK_APPS:       return Keyboard::Menu;
        case VK_OEM_1:      return Keyboard::Semicolon;
        case VK_OEM_2:      return Keyboard::Slash;
        case VK_OEM_PLUS:   return Keyboard::Equal;
        case VK_OEM_MINUS:  return Keyboard::Hyphen;
        case VK_OEM_4:      return Keyboard::LBracket;
        case VK_OEM_6:      return Keyboard::RBracket;
        case VK_OEM_COMMA:  return Keyboard::Comma;
        case VK_OEM_PERIOD: return Keyboard::Period;
        case VK_OEM_7:      return Keyboard::Quote;
        case VK_OEM_5:      return Keyboard::Backslash;
        case VK_OEM_3:      return Keyboard::Tilde;
        case VK_ESCAPE:     return Keyboard::Escape;
        case VK_SPACE:      return Keyboard::Space;
        case VK_RETURN:     return Keyboard::Enter;
        case VK_BACK:       return Keyboard::Backspace;
        case VK_TAB:        return Keyboard::Tab;
        case VK_PRIOR:      return Keyboard::PageUp;
        case VK_NEXT:       return Keyboard::PageDown;
        case VK_END:        return Keyboard::End;
        case VK_HOME:       return Keyboard::Home;
        case VK_INSERT:     return Keyboard::Insert;
        case VK_DELETE:     return Keyboard::Delete;
        case VK_ADD:        return Keyboard::Add;
        case VK_SUBTRACT:   return Keyboard::Subtract;
        case VK_MULTIPLY:   return Keyboard::Multiply;
        case VK_DIVIDE:     return Keyboard::Divide;
        case VK_PAUSE:      return Keyboard::Pause;
        case VK_F1:         return Keyboard::F1;
        case VK_F2:         return Keyboard::F2;
        case VK_F3:         return Keyboard::F3;
        case VK_F4:         return Keyboard::F4;
        case VK_F5:         return Keyboard::F5;
        case VK_F6:         return Keyboard::F6;
        case VK_F7:         return Keyboard::F7;
        case VK_F8:         return Keyboard::F8;
        case VK_F9:         return Keyboard::F9;
        case VK_F10:        return Keyboard::F10;
        case VK_F11:        return Keyboard::F11;
        case VK_F12:        return Keyboard::F12;
        case VK_F13:        return Keyboard::F13;
        case VK_F14:        return Keyboard::F14;
        case VK_F15:        return Keyboard::F15;
        case VK_LEFT:       return Keyboard::Left;
        case VK_RIGHT:      return Keyboard::Right;
        case VK_UP:         return Keyboard::Up;
        case VK_DOWN:       return Keyboard::Down;
        case VK_NUMPAD0:    return Keyboard::Numpad0;
        case VK_NUMPAD1:    return Keyboard::Numpad1;
        case VK_NUMPAD2:    return Keyboard::Numpad2;
        case VK_NUMPAD3:    return Keyboard::Numpad3;
        case VK_NUMPAD4:    return Keyboard::Numpad4;
        case VK_NUMPAD5:    return Keyboard::Numpad5;
        case VK_NUMPAD6:    return Keyboard::Numpad6;
        case VK_NUMPAD7:    return Keyboard::Numpad7;
        case VK_NUMPAD8:    return Keyboard::Numpad8;
        case VK_NUMPAD9:    return Keyboard::Numpad9;
        case 'A':           return Keyboard::A;
        case 'Z':           return Keyboard::Z;
        case 'E':           return Keyboard::E;
        case 'R':           return Keyboard::R;
        case 'T':           return Keyboard::T;
        case 'Y':           return Keyboard::Y;
        case 'U':           return Keyboard::U;
        case 'I':           return Keyboard::I;
        case 'O':           return Keyboard::O;
        case 'P':           return Keyboard::P;
        case 'Q':           return Keyboard::Q;
        case 'S':           return Keyboard::S;
        case 'D':           return Keyboard::D;
        case 'F':           return Keyboard::F;
        case 'G':           return Keyboard::G;
        case 'H':           return Keyboard::H;
        case 'J':           return Keyboard::J;
        case 'K':           return Keyboard::K;
        case 'L':           return Keyboard::L;
        case 'M':           return Keyboard::M;
        case 'W':           return Keyboard::W;
        case 'X':           return Keyboard::X;
        case 'C':           return Keyboard::C;
        case 'V':           return Keyboard::V;
        case 'B':           return Keyboard::B;
        case 'N':           return Keyboard::N;
        case '0':           return Keyboard::Num0;
        case '1':           return Keyboard::Num1;
        case '2':           return Keyboard::Num2;
        case '3':           return Keyboard::Num3;
        case '4':           return Keyboard::Num4;
        case '5':           return Keyboard::Num5;
        case '6':           return Keyboard::Num6;
        case '7':           return Keyboard::Num7;
        case '8':           return Keyboard::Num8;
        case '9':           return Keyboard::Num9;
    }
    // clang-format on

    return Keyboard::Unknown;
}


////////////////////////////////////////////////////////////
LRESULT CALLBACK WindowImplWin32::globalOnEvent(HWND handle, UINT message, WPARAM wParam, LPARAM lParam)
{
    // Associate handle and Window instance when the creation message is received
    if (message == WM_CREATE)
    {
        // Get WindowImplWin32 instance (it was passed as the last argument of CreateWindow)
        auto window = reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCT*>(lParam)->lpCreateParams);

        // Set as the "user data" parameter of the window
        SetWindowLongPtrW(handle, GWLP_USERDATA, window);
    }

    // Get the WindowImpl instance corresponding to the window handle
    WindowImplWin32* window = handle ? reinterpret_cast<WindowImplWin32*>(GetWindowLongPtr(handle, GWLP_USERDATA)) : nullptr;

    // Forward the event to the appropriate function
    if (window)
    {
        window->processEvent(message, wParam, lParam);

        // Don't forward the WM_GETDPISCALEDSIZE message when we reacted by changing the window size
        if ((message == WM_GETDPISCALEDSIZE) && (m_dpiAwareness == DpiAwareness::PerMonitorAwareV2))
            return 1;

        if (window->m_callback)
            return CallWindowProcW(reinterpret_cast<WNDPROC>(window->m_callback), handle, message, wParam, lParam);
    }

    // We don't forward the WM_CLOSE message to prevent the OS from automatically destroying the window
    if (message == WM_CLOSE)
        return 0;

    // Don't forward the menu system command, so that pressing ALT or F10 doesn't steal the focus
    if ((message == WM_SYSCOMMAND) && (wParam == SC_KEYMENU))
        return 0;

    return DefWindowProcW(handle, message, wParam, lParam);
}

////////////////////////////////////////////////////////////
// Static member data
////////////////////////////////////////////////////////////
WindowImplWin32::DpiAwareness WindowImplWin32::m_dpiAwareness = DpiAwareness::Unaware;

} // namespace priv

} // namespace sf
