#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <shlguid.h>
#include <exdisp.h>
#include <shldisp.h>
#include <servprov.h>
#include <gdiplus.h>
#include <commctrl.h>
#include <algorithm>
#include <string>
#include <vector>
#include <atomic>

namespace {
constexpr wchar_t AppName[] = L"Quick Preview";
constexpr wchar_t RunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr UINT TrayMessage = WM_APP + 1, OpenMessage = WM_APP + 2, CloseMessage = WM_APP + 3, NavigateMessage = WM_APP + 4;
HINSTANCE instance;
HWND owner = nullptr, preview = nullptr, body = nullptr, textBox = nullptr, source = nullptr;
std::atomic<HWND> previewHandle{nullptr}, sourceHandle{nullptr};
std::wstring currentPath;
Gdiplus::Image* image = nullptr;
ULONG_PTR graphicsToken = 0;
IPreviewHandler* previewHandler = nullptr;
IStream* previewStream = nullptr;
HHOOK keyboardHook = nullptr;
HANDLE hookThread = nullptr, hookReady = nullptr;
DWORD hookThreadId = 0;
HICON trayIcon = nullptr;
UINT taskbarCreated = 0;

template<class T> void Release(T*& p) { if (p) { p->Release(); p = nullptr; } }
std::wstring Name(const std::wstring& path) { const wchar_t* part = PathFindFileNameW(path.c_str()); return part ? part : path; }

bool IsEditing(HWND h) {
    GUITHREADINFO info = {}; info.cbSize = sizeof(info);
    if (!GetGUIThreadInfo(GetWindowThreadProcessId(h, nullptr), &info)) return true;
    wchar_t klass[128] = {};
    GetClassNameW(info.hwndFocus, klass, 128);
    return _wcsicmp(klass, L"Edit") == 0 || wcsstr(klass, L"RichEdit") != nullptr || info.hwndCaret != nullptr;
}
bool PreviewActive() {
    HWND p = previewHandle.load(), f = GetForegroundWindow(), e = sourceHandle.load();
    return p && (f == p || (f == e && !IsEditing(f)));
}

IWebBrowserApp* FindExplorer(HWND target) {
    IShellWindows* windows = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows)))) return nullptr;
    long count = 0; windows->get_Count(&count);
    IWebBrowserApp* result = nullptr;
    for (long i = 0; i < count; ++i) {
        VARIANT index; VariantInit(&index); index.vt = VT_I4; index.lVal = i;
        IDispatch* dispatch = nullptr; IWebBrowserApp* browser = nullptr;
        if (SUCCEEDED(windows->Item(index, &dispatch)) && dispatch && SUCCEEDED(dispatch->QueryInterface(IID_PPV_ARGS(&browser)))) {
            SHANDLE_PTR hwnd = 0;
            if (SUCCEEDED(browser->get_HWND(&hwnd)) && reinterpret_cast<HWND>(hwnd) == target) {
                HWND activeTab = FindWindowExW(target, nullptr, L"ShellTabWindowClass", nullptr);
                IServiceProvider* provider = nullptr; IShellBrowser* shellBrowser = nullptr; HWND browserWindow = nullptr;
                bool matches = !activeTab || (SUCCEEDED(browser->QueryInterface(IID_PPV_ARGS(&provider))) &&
                    SUCCEEDED(provider->QueryService(IID_IShellBrowser, IID_PPV_ARGS(&shellBrowser))) &&
                    SUCCEEDED(shellBrowser->GetWindow(&browserWindow)) && browserWindow == activeTab);
                Release(shellBrowser); Release(provider);
                if (matches) { result = browser; browser = nullptr; }
            }
        }
        Release(browser); Release(dispatch);
        if (result) break;
    }
    Release(windows);
    return result;
}
std::wstring Selection(IWebBrowserApp* browser) {
    std::wstring result;
    IDispatch* document = nullptr; IShellFolderViewDual* view = nullptr; FolderItems* items = nullptr; FolderItem* item = nullptr;
    if (browser && SUCCEEDED(browser->get_Document(&document)) && document &&
        SUCCEEDED(document->QueryInterface(IID_PPV_ARGS(&view))) && SUCCEEDED(view->SelectedItems(&items)) && items) {
        long count = 0; items->get_Count(&count);
        if (count == 1) {
            VARIANT index; VariantInit(&index); index.vt = VT_I4; index.lVal = 0;
            if (SUCCEEDED(items->Item(index, &item)) && item) { BSTR value = nullptr; if (SUCCEEDED(item->get_Path(&value)) && value) { result.assign(value, SysStringLen(value)); SysFreeString(value); } }
        }
    }
    Release(item); Release(items); Release(view); Release(document);
    if (!result.empty() && GetFileAttributesW(result.c_str()) == INVALID_FILE_ATTRIBUTES) result.clear();
    return result;
}
std::wstring Selection(HWND h) { IWebBrowserApp* b = FindExplorer(h); auto result = Selection(b); Release(b); return result; }

void ClearContent() {
    if (previewHandler) { previewHandler->Unload(); Release(previewHandler); }
    Release(previewStream);
    if (textBox) { DestroyWindow(textBox); textBox = nullptr; }
    delete image; image = nullptr;
}
bool StartupEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RunKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    bool enabled = RegQueryValueExW(key, L"QuickPreviewNative", nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(key); return enabled;
}
void ToggleStartup() {
    HKEY key = nullptr; LONG error = RegCreateKeyExW(HKEY_CURRENT_USER, RunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (error == ERROR_SUCCESS) {
        if (StartupEnabled()) { error = RegDeleteValueW(key, L"QuickPreviewNative"); }
        else {
            wchar_t path[32768] = {}; GetModuleFileNameW(nullptr, path, 32768);
            std::wstring command = L"\"" + std::wstring(path) + L"\"";
            error = RegSetValueExW(key, L"QuickPreviewNative", 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()), static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
        }
        RegCloseKey(key);
    }
    if (error != ERROR_SUCCESS) MessageBoxW(owner, L"Could not update the sign-in setting.", AppName, MB_ICONERROR);
}
void AddTray() {
    NOTIFYICONDATAW data = {}; data.cbSize = sizeof(data); data.hWnd = owner; data.uID = 1;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP; data.uCallbackMessage = TrayMessage;
    data.hIcon = trayIcon; wcscpy_s(data.szTip, AppName);
    if (!Shell_NotifyIconW(NIM_ADD, &data)) MessageBoxW(nullptr, L"Could not add the tray icon.", AppName, MB_ICONERROR);
}

void ResizeContent() {
    if (!preview || !body) return;
    RECT rect; GetClientRect(preview, &rect); int header = MulDiv(44, GetDpiForWindow(preview), 96);
    MoveWindow(body, 0, header, rect.right, std::max(0, static_cast<int>(rect.bottom) - header), TRUE);
    GetClientRect(body, &rect);
    if (textBox) MoveWindow(textBox, 0, 0, rect.right, rect.bottom, TRUE);
    if (previewHandler) previewHandler->SetRect(&rect);
}
void SizeWindow(const SIZE* dimensions, bool pdf, bool first) {
    HWND anchor = first && IsWindow(source) ? source : preview;
    MONITORINFO monitor = {}; monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromWindow(anchor, MONITOR_DEFAULTTONEAREST), &monitor);
    RECT work = monitor.rcWork, current = {}; GetWindowRect(preview, &current);
    int centerX = first ? (work.left + work.right) / 2 : (current.left + current.right) / 2;
    int centerY = first ? (work.top + work.bottom) / 2 : (current.top + current.bottom) / 2;
    int workWidth = work.right - work.left, workHeight = work.bottom - work.top;
    int maxWidth = workWidth * 9 / 10, maxHeight = workHeight * 9 / 10;
    int header = MulDiv(44, GetDpiForWindow(preview), 96);
    int width, height;
    if (dimensions && dimensions->cx > 0 && dimensions->cy > 0) {
        double factor = std::min(2.0, std::min(static_cast<double>(maxWidth) / dimensions->cx, static_cast<double>(maxHeight - header) / dimensions->cy));
        width = static_cast<int>(dimensions->cx * factor); height = static_cast<int>(dimensions->cy * factor) + header;
    } else if (pdf) { width = workWidth * 82 / 100; height = maxHeight; }
    else { width = workWidth * 65 / 100; height = workHeight * 72 / 100; }
    width = std::clamp(width, std::min(maxWidth, 400), maxWidth);
    height = std::clamp(height, std::min(maxHeight, 300), maxHeight);
    int left = std::clamp(centerX - width / 2, static_cast<int>(work.left), static_cast<int>(work.right) - width);
    int top = std::clamp(centerY - height / 2, static_cast<int>(work.top), static_cast<int>(work.bottom) - height);
    SetWindowPos(preview, HWND_TOPMOST, left, top, width, height, SWP_NOACTIVATE);
}
bool TextFile(const std::wstring& ext) {
    static const wchar_t* types[] = {L".txt", L".md", L".markdown", L".log", L".csv", L".tsv", L".json", L".xml", L".yaml", L".yml", L".ini", L".conf", L".config", L".html", L".htm", L".css", L".js", L".ts", L".py", L".cs", L".c", L".cpp", L".h", L".java", L".sql", L".ps1", L".bat", L".sh"};
    for (auto type : types) if (_wcsicmp(type, ext.c_str()) == 0) return true;
    return false;
}
bool ImageFile(const std::wstring& ext) {
    static const wchar_t* types[] = { L".png", L".jpg", L".jpeg", L".gif", L".bmp", L".ico", L".tif", L".tiff" };
    for (auto type : types) if (_wcsicmp(type, ext.c_str()) == 0) return true;
    return false;
}

bool ReadHandlerId(const std::wstring& ext, CLSID* id) {
    const wchar_t suffix[] = L"\\shellex\\{8895b1c6-b41f-4c1c-a562-0d564250836f}";
    std::wstring key = ext + suffix;
    wchar_t text[256] = {}; DWORD bytes = sizeof(text);
    if (RegGetValueW(HKEY_CLASSES_ROOT, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, text, &bytes) != ERROR_SUCCESS) {
        wchar_t prog[256] = {}; bytes = sizeof(prog);
        if (RegGetValueW(HKEY_CLASSES_ROOT, ext.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, prog, &bytes) != ERROR_SUCCESS) return false;
        key = std::wstring(prog) + suffix; bytes = sizeof(text);
        if (RegGetValueW(HKEY_CLASSES_ROOT, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, text, &bytes) != ERROR_SUCCESS) return false;
    }
    return SUCCEEDED(CLSIDFromString(text, id));
}
bool LoadHandler(const std::wstring& path, const std::wstring& ext) {
    CLSID id; if (!ReadHandlerId(ext, &id)) return false;
    IUnknown* object = nullptr;
    if (FAILED(CoCreateInstance(id, nullptr, CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&object)))) return false;
    IInitializeWithFile* byFile = nullptr; IInitializeWithStream* byStream = nullptr;
    HRESULT hr = E_NOINTERFACE;
    if (SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&byFile)))) hr = byFile->Initialize(path.c_str(), STGM_READ);
    else if (SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&byStream)))) {
        hr = SHCreateStreamOnFileEx(path.c_str(), STGM_READ | STGM_SHARE_DENY_WRITE, 0, FALSE, nullptr, &previewStream);
        if (SUCCEEDED(hr)) hr = byStream->Initialize(previewStream, STGM_READ);
    }
    Release(byFile); Release(byStream);
    if (SUCCEEDED(hr)) hr = object->QueryInterface(IID_PPV_ARGS(&previewHandler));
    Release(object);
    if (SUCCEEDED(hr)) {
        RECT rect; GetClientRect(body, &rect);
        hr = previewHandler->SetWindow(body, &rect);
        if (SUCCEEDED(hr)) hr = previewHandler->DoPreview();
    }
    if (FAILED(hr)) { if (previewHandler) { previewHandler->Unload(); Release(previewHandler); } Release(previewStream); return false; }
    return true;
}
std::wstring ReadText(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return L"Unable to read this file.";
    std::vector<char> bytes(500000); DWORD count = 0;
    bool success = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) != FALSE;
    LARGE_INTEGER length = {}; GetFileSizeEx(file, &length); CloseHandle(file);
    if (!success) return L"Unable to read this file.";
    std::wstring content;
    if (count >= 2 && static_cast<unsigned char>(bytes[0]) == 0xff && static_cast<unsigned char>(bytes[1]) == 0xfe) {
        size_t characters = (count - 2) / sizeof(wchar_t); content.resize(characters);
        if (characters) memcpy(content.data(), bytes.data() + 2, characters * sizeof(wchar_t));
    } else {
        int skip = count >= 3 && static_cast<unsigned char>(bytes[0]) == 0xef && static_cast<unsigned char>(bytes[1]) == 0xbb && static_cast<unsigned char>(bytes[2]) == 0xbf ? 3 : 0;
        int amount = static_cast<int>(count) - skip;
        if (amount > 0) { int len = MultiByteToWideChar(CP_UTF8, 0, bytes.data() + skip, amount, nullptr, 0); if (len > 0) { content.resize(len); MultiByteToWideChar(CP_UTF8, 0, bytes.data() + skip, amount, content.data(), len); } }
    }
    if (length.QuadPart > count) content += L"\r\n\r\n[Preview truncated]";
    return content;
}
void ShowFile(const std::wstring& path, bool first) {
    if (path.empty() || (!first && path == currentPath)) return;
    ClearContent(); currentPath = path;
    std::wstring caption = Name(path) + L" — Quick Preview";
    SetWindowTextW(preview, caption.c_str()); SetWindowTextW(body, L"");
    std::wstring ext = PathFindExtensionW(path.c_str());
    DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        SizeWindow(nullptr, false, first); SetWindowTextW(body, L"Folder\n\nOpen this folder in File Explorer to view its contents.");
    } else if (attributes == INVALID_FILE_ATTRIBUTES) {
        SizeWindow(nullptr, false, first); SetWindowTextW(body, L"This file is no longer available.");
    } else if (ImageFile(ext)) {
        image = Gdiplus::Image::FromFile(path.c_str(), FALSE);
        if (image && image->GetLastStatus() == Gdiplus::Ok && image->GetWidth() && image->GetHeight()) {
            SIZE dimensions = {static_cast<LONG>(image->GetWidth()), static_cast<LONG>(image->GetHeight())}; SizeWindow(&dimensions, false, first);
        } else { delete image; image = nullptr; SizeWindow(nullptr, false, first); if (!LoadHandler(path, ext)) SetWindowTextW(body, L"Unable to preview this image."); }
    } else {
        SizeWindow(nullptr, _wcsicmp(ext.c_str(), L".pdf") == 0, first);
        if (TextFile(ext)) {
            auto content = ReadText(path); RECT rect; GetClientRect(body, &rect);
            textBox = CreateWindowExW(0, L"EDIT", content.c_str(), WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOHSCROLL | ES_AUTOVSCROLL,
                0, 0, rect.right, rect.bottom, body, nullptr, instance, nullptr);
            SendMessageW(textBox, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        } else if (!LoadHandler(path, ext)) {
            std::wstring message = L"No preview available\n\n" + Name(path) + L"\n\nInstall a Windows preview handler for this file type.";
            SetWindowTextW(body, message.c_str());
        }
    }
    ResizeContent(); InvalidateRect(body, nullptr, TRUE); InvalidateRect(preview, nullptr, TRUE);
}

void Navigate(int direction) {
    if (!preview || !IsWindow(source)) return;
    IWebBrowserApp* browser = FindExplorer(source); IServiceProvider* provider = nullptr;
    IShellBrowser* shellBrowser = nullptr; IShellView* shellView = nullptr; IFolderView* folder = nullptr;
    if (browser && Selection(browser) == currentPath && SUCCEEDED(browser->QueryInterface(IID_PPV_ARGS(&provider))) &&
        SUCCEEDED(provider->QueryService(IID_IShellBrowser, IID_PPV_ARGS(&shellBrowser))) &&
        SUCCEEDED(shellBrowser->QueryActiveShellView(&shellView)) && shellView && SUCCEEDED(shellView->QueryInterface(IID_PPV_ARGS(&folder)))) {
        int index = -1, count = 0;
        if (FAILED(folder->GetSelectionMarkedItem(&index)) || index < 0) folder->GetFocusedItem(&index);
        if (index >= 0 && SUCCEEDED(folder->ItemCount(SVGIO_ALLVIEW, &count))) {
            int next = index + direction;
            if (next >= 0 && next < count && SUCCEEDED(folder->SelectItem(next, SVSI_SELECT | SVSI_DESELECTOTHERS | SVSI_ENSUREVISIBLE | SVSI_FOCUSED | SVSI_SELECTIONMARK | SVSI_NOTAKEFOCUS))) {
                auto path = Selection(browser); if (!path.empty()) ShowFile(path, false);
            }
        }
    }
    Release(folder); Release(shellView); Release(shellBrowser); Release(provider); Release(browser);
}

LRESULT CALLBACK KeyboardProc(int code, WPARAM message, LPARAM data) {
    if (code >= 0) {
        const auto& key = *reinterpret_cast<KBDLLHOOKSTRUCT*>(data);
        bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
        bool up = message == WM_KEYUP || message == WM_SYSKEYUP;
        static bool spaceDown = false, escapeDown = false, arrowDown[4] = {};
        if (key.vkCode == VK_SPACE) {
            if (up && spaceDown) { spaceDown = false; return 1; }
            if (down && spaceDown) return 1;
            if (down && !(GetAsyncKeyState(VK_CONTROL) & 0x8000) && !(GetAsyncKeyState(VK_MENU) & 0x8000) && !(GetAsyncKeyState(VK_SHIFT) & 0x8000)) {
                if (PreviewActive()) { spaceDown = true; PostMessageW(owner, CloseMessage, 0, 0); return 1; }
                HWND foreground = GetForegroundWindow(); wchar_t klass[128] = {};
                GetClassNameW(foreground, klass, 128);
                if (_wcsicmp(klass, L"CabinetWClass") == 0 && !IsEditing(foreground)) {
                    spaceDown = true; PostMessageW(owner, OpenMessage, reinterpret_cast<WPARAM>(foreground), 0); return 1;
                }
            }
        }
        if (key.vkCode == VK_ESCAPE) {
            if (up && escapeDown) { escapeDown = false; return 1; }
            if (down && escapeDown) return 1;
            if (down && PreviewActive()) { escapeDown = true; PostMessageW(owner, CloseMessage, 0, 0); return 1; }
        }
        if (key.vkCode >= VK_LEFT && key.vkCode <= VK_DOWN) {
            auto index = key.vkCode - VK_LEFT;
            if (up && arrowDown[index]) { arrowDown[index] = false; return 1; }
            if (down && previewHandle.load() && GetForegroundWindow() == previewHandle.load() &&
                !(GetAsyncKeyState(VK_CONTROL) & 0x8000) && !(GetAsyncKeyState(VK_MENU) & 0x8000) && !(GetAsyncKeyState(VK_SHIFT) & 0x8000)) {
                arrowDown[index] = true;
                PostMessageW(owner, NavigateMessage, (key.vkCode == VK_LEFT || key.vkCode == VK_UP) ? static_cast<WPARAM>(-1) : 1, 0);
                return 1;
            }
        }
    }
    return CallNextHookEx(keyboardHook, code, message, data);
}
DWORD WINAPI RunHook(void*) {
    hookThreadId = GetCurrentThreadId();
    keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, instance, 0);
    MSG message; PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
    SetEvent(hookReady);
    if (!keyboardHook) return 1;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    UnhookWindowsHookEx(keyboardHook); keyboardHook = nullptr; return 0;
}

LRESULT CALLBACK BodyProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_PAINT) {
        PAINTSTRUCT paint; HDC dc = BeginPaint(window, &paint); RECT rect; GetClientRect(window, &rect);
        HBRUSH background = CreateSolidBrush(RGB(28, 30, 34)); FillRect(dc, &rect, background); DeleteObject(background);
        if (image) {
            Gdiplus::Graphics graphics(dc); graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            UINT width = image->GetWidth(), height = image->GetHeight();
            if (width && height) { double scale = std::min(static_cast<double>(rect.right) / width, static_cast<double>(rect.bottom) / height);
                int scaledWidth = static_cast<int>(width * scale), scaledHeight = static_cast<int>(height * scale);
                graphics.DrawImage(image, (rect.right - scaledWidth) / 2, (rect.bottom - scaledHeight) / 2, scaledWidth, scaledHeight); }
        } else if (!previewHandler && !textBox) {
            wchar_t messageText[2048] = {}; GetWindowTextW(window, messageText, 2048);
            SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(220, 220, 220));
            SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT)); DrawTextW(dc, messageText, -1, &rect, DT_CENTER | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX);
        }
        EndPaint(window, &paint); return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
LRESULT CALLBACK PreviewProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            body = CreateWindowExW(0, L"QuickPreviewBody", L"", WS_CHILD | WS_VISIBLE, 0, 0, 1, 1, window, nullptr, instance, nullptr);
            return 0;
        case WM_SIZE: ResizeContent(); return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint; HDC dc = BeginPaint(window, &paint); RECT rect; GetClientRect(window, &rect);
            rect.bottom = MulDiv(44, GetDpiForWindow(window), 96);
            HBRUSH bg = CreateSolidBrush(RGB(39, 42, 47)); FillRect(dc, &rect, bg); DeleteObject(bg);
            SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255,255,255)); SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
            RECT titleRect = rect; titleRect.left += 14; titleRect.right -= 48;
            std::wstring title = Name(currentPath); DrawTextW(dc, title.c_str(), -1, &titleRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            rect.left = rect.right - 48; DrawTextW(dc, L"×", -1, &rect, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
            EndPaint(window, &paint); return 0;
        }
        case WM_LBUTTONDOWN: {
            RECT rect; GetClientRect(window, &rect);
            if (GET_Y_LPARAM(lParam) < MulDiv(44, GetDpiForWindow(window), 96)) {
                if (GET_X_LPARAM(lParam) >= rect.right - 48) DestroyWindow(window);
                else { ReleaseCapture(); SendMessageW(window, WM_NCLBUTTONDOWN, HTCAPTION, 0); }
                return 0;
            }
            break;
        }
        case WM_DESTROY: ClearContent(); previewHandle.store(nullptr); preview = nullptr; body = nullptr; currentPath.clear(); return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
void OpenPreview(HWND explorer) {
    std::wstring path = Selection(explorer); if (path.empty()) return;
    source = explorer; sourceHandle.store(explorer);
    if (preview) DestroyWindow(preview);
    preview = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"QuickPreviewWindow", AppName,
        WS_POPUP, 0, 0, 920, 680, nullptr, nullptr, instance, nullptr);
    if (!preview) return;
    previewHandle.store(preview); ShowFile(path, true); ShowWindow(preview, SW_SHOW); SetForegroundWindow(preview);
}
void TrayMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (StartupEnabled() ? MF_CHECKED : 0), 1, L"Start at sign-in");
    AppendMenuW(menu, MF_STRING, 2, L"How to use"); AppendMenuW(menu, MF_SEPARATOR, 0, nullptr); AppendMenuW(menu, MF_STRING, 3, L"Exit");
    POINT point; GetCursorPos(&point); SetForegroundWindow(owner);
    int choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, owner, nullptr);
    DestroyMenu(menu);
    if (choice == 1) ToggleStartup();
    if (choice == 2) MessageBoxW(owner, L"Select a file in File Explorer and press Space. Use the arrow keys to move between files. Press Space or Esc to close.\n\nRight-click the tray icon for options.", AppName, MB_OK | MB_ICONINFORMATION);
    if (choice == 3) DestroyWindow(owner);
}
LRESULT CALLBACK OwnerProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == taskbarCreated) { AddTray(); return 0; }
    switch (message) {
        case WM_CREATE: owner = window; SetTimer(window, 1, 200, nullptr); AddTray(); return 0;
        case OpenMessage: OpenPreview(reinterpret_cast<HWND>(wParam)); return 0;
        case CloseMessage: if (preview) DestroyWindow(preview); return 0;
        case NavigateMessage: Navigate(static_cast<int>(wParam)); return 0;
        case WM_TIMER: if (preview && GetForegroundWindow() == source && IsWindow(source)) { auto path = Selection(source); if (!path.empty()) ShowFile(path, false); } return 0;
        case TrayMessage: if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) TrayMenu(); return 0;
        case WM_DESTROY: {
            KillTimer(window, 1); if (preview) DestroyWindow(preview);
            NOTIFYICONDATAW data = {}; data.cbSize = sizeof(data); data.hWnd = window; data.uID = 1; Shell_NotifyIconW(NIM_DELETE, &data);
            if (hookThreadId) PostThreadMessageW(hookThreadId, WM_QUIT, 0, 0);
            if (hookThread) { WaitForSingleObject(hookThread, 2000); CloseHandle(hookThread); hookThread = nullptr; }
            PostQuitMessage(0); return 0;
        }
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\QuickPreviewNativeSingleton");
    if (GetLastError() == ERROR_ALREADY_EXISTS) { if (mutex) CloseHandle(mutex); return 0; }
    instance = hInstance; SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) { if (mutex) CloseHandle(mutex); return 1; }
    Gdiplus::GdiplusStartupInput input; Gdiplus::GdiplusStartup(&graphicsToken, &input, nullptr);
    taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated"); trayIcon = LoadIconW(nullptr, IDI_APPLICATION);
    WNDCLASSW klass = {}; klass.hInstance = instance; klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    klass.lpszClassName = L"QuickPreviewOwner"; klass.lpfnWndProc = OwnerProc; RegisterClassW(&klass);
    klass.lpszClassName = L"QuickPreviewWindow"; klass.lpfnWndProc = PreviewProc; RegisterClassW(&klass);
    klass.lpszClassName = L"QuickPreviewBody"; klass.lpfnWndProc = BodyProc; RegisterClassW(&klass);
    owner = CreateWindowExW(0, L"QuickPreviewOwner", AppName, 0, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (!owner) { Gdiplus::GdiplusShutdown(graphicsToken); CoUninitialize(); if (mutex) CloseHandle(mutex); return 1; }
    hookReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    hookThread = CreateThread(nullptr, 0, RunHook, nullptr, 0, nullptr);
    if (!hookReady || !hookThread || WaitForSingleObject(hookReady, 5000) != WAIT_OBJECT_0 || !keyboardHook)
        MessageBoxW(owner, L"Could not enable the keyboard shortcut. Restart Quick Preview.", AppName, MB_ICONERROR);
    if (hookReady) CloseHandle(hookReady);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    Gdiplus::GdiplusShutdown(graphicsToken); CoUninitialize(); if (mutex) CloseHandle(mutex); return 0;
}

