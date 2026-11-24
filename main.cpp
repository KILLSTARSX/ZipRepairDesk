#include "repair_core.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <uxtheme.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kWindowClass[] = L"ZipRepairDeskWindow";
constexpr wchar_t kAboutClass[] = L"ZipRepairDeskAbout";
constexpr wchar_t kSettingsKey[] = L"Software\\KILL-STAR SX\\ZipRepairDesk";
constexpr wchar_t kThemeValue[] = L"Theme";
constexpr wchar_t kLanguageValue[] = L"Language";
constexpr wchar_t kPersonalizeKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
constexpr wchar_t kRepoUrl[] = L"https://github.com/KILLSTARSX/ZipRepairDesk";
constexpr wchar_t kRepoDisplay[] = L"github.com/KILLSTARSX/ZipRepairDesk";

constexpr int IDC_ADD = 1001;
constexpr int IDC_REMOVE = 1002;
constexpr int IDC_CLEAR = 1003;
constexpr int IDC_OUTPUT = 1004;
constexpr int IDC_BROWSE = 1005;
constexpr int IDC_TRIM = 1006;
constexpr int IDC_LIST = 1007;
constexpr int IDC_PROGRESS = 1008;
constexpr int IDC_STATUS = 1009;
constexpr int IDC_START = 1011;
constexpr int IDC_CANCEL = 1012;
constexpr int IDC_OPEN = 1013;
constexpr int IDC_THEME = 1014;
constexpr int IDC_LANGUAGE = 1015;
constexpr int IDC_ABOUT = 1016;

constexpr UINT WM_APP_UPDATE = WM_APP + 1;
constexpr UINT WM_APP_FINISHED = WM_APP + 2;

constexpr UINT_PTR kHoverTimer = 1;
constexpr UINT_PTR kToggleTimer = 2;

enum class UiLanguage { Chinese, English };

struct Palette {
    COLORREF window;
    COLORREF card;
    COLORREF text;
    COLORREF muted;
    COLORREF border;
    COLORREF accent;
    COLORREF accentPressed;
    COLORREF secondary;
    COLORREF success;
    COLORREF error;
    COLORREF trackOff;
    COLORREF disabledFill;
    COLORREF disabledInk;
};

const Palette kLightPalette{
    RGB(246, 248, 252), RGB(255, 255, 255), RGB(28, 35, 52), RGB(105, 114, 134),
    RGB(225, 229, 239), RGB(91, 87, 220), RGB(72, 68, 184), RGB(239, 241, 247),
    RGB(22, 163, 74), RGB(220, 38, 38), RGB(205, 211, 224), RGB(237, 239, 244),
    RGB(155, 161, 175)};

const Palette kDarkPalette{
    RGB(22, 24, 31), RGB(33, 36, 45), RGB(233, 237, 246), RGB(150, 157, 173),
    RGB(58, 63, 78), RGB(124, 120, 255), RGB(101, 97, 228), RGB(45, 49, 60),
    RGB(74, 222, 128), RGB(248, 113, 113), RGB(85, 91, 107), RGB(44, 47, 57),
    RGB(118, 124, 140)};

COLORREF blendColor(COLORREF from, COLORREF to, float amount) {
    if (amount <= 0.0f) return from;
    if (amount >= 1.0f) return to;
    const auto channel = [amount](int a, int b) {
        return static_cast<int>(static_cast<float>(a) + static_cast<float>(b - a) * amount + 0.5f);
    };
    return RGB(channel(GetRValue(from), GetRValue(to)),
               channel(GetGValue(from), GetGValue(to)),
               channel(GetBValue(from), GetBValue(to)));
}

struct HoverState {
    HWND window = nullptr;
    float value = 0.0f;
    bool hot = false;
    bool animating = false;
};

bool queryDword(HKEY root, const wchar_t* subkey, const wchar_t* name, DWORD& value) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    DWORD type = 0;
    DWORD data = 0;
    DWORD size = sizeof(data);
    const LONG status = RegQueryValueExW(key, name, nullptr, &type,
                                         reinterpret_cast<LPBYTE>(&data), &size);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_DWORD || size != sizeof(DWORD)) return false;
    value = data;
    return true;
}

void storeDword(const wchar_t* name, DWORD value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_WRITE, nullptr,
                        &key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value),
                   sizeof(value));
    RegCloseKey(key);
}

bool systemUsesLightTheme() {
    DWORD value = 0;
    if (!queryDword(HKEY_CURRENT_USER, kPersonalizeKey, L"AppsUseLightTheme", value)) return false;
    return value != 0;
}

bool systemLanguageIsChinese() {
    const LANGID language = GetUserDefaultUILanguage();
    return PRIMARYLANGID(language) == LANG_CHINESE;
}

std::wstring nativePath(const std::wstring& path) {
    if (path.rfind(LR"(\\?\)", 0) == 0) return path;
    if (path.rfind(LR"(\\)", 0) == 0) return LR"(\\?\UNC\)" + path.substr(2);
    if (path.size() >= 2 && path[1] == L':') return LR"(\\?\)" + path;
    return path;
}

std::wstring joinPath(const std::wstring& directory, const std::wstring& name) {
    if (directory.empty()) return name;
    if (directory.back() == L'\x5c' || directory.back() == L'/') return directory + name;
    return directory + L"\x5c" + name;
}

std::wstring fileName(const std::wstring& path) {
    const size_t separator = path.find_last_of(L"/\x5c");
    return separator == std::wstring::npos ? path : path.substr(separator + 1);
}

std::wstring fileNameOf(const std::wstring& path) {
    const std::wstring name = fileName(path);
    return name.empty() ? path : name;
}

std::wstring normalized(const std::wstring& path) {
    std::vector<wchar_t> buffer(32768, L'\0');
    const DWORD length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(buffer.size()),
                                          buffer.data(), nullptr);
    if (length && length < buffer.size()) return buffer.data();
    return path;
}

uint64_t fileSize(const std::wstring& path) {
    const std::wstring native = nativePath(path);
    HANDLE file = CreateFileW(native.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER size{};
    const bool ok = GetFileSizeEx(file, &size);
    CloseHandle(file);
    return ok ? static_cast<uint64_t>(size.QuadPart) : 0;
}

bool isRegularFile(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD attributes = GetFileAttributesW(nativePath(path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool supportedExtension(const wchar_t* path) {
    std::wstring extension = path;
    const size_t dot = extension.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    extension.erase(0, dot);
    CharLowerBuffW(extension.data(), static_cast<DWORD>(extension.size()));
    return extension == L".zip" || extension == L".mp4" || extension == L".rar" ||
           extension == L".7z";
}

void collectSupportedFiles(const std::wstring& directory, std::vector<std::wstring>& output) {
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(nativePath(joinPath(directory, L"*")).c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L"..")) continue;
        const std::wstring path = joinPath(directory, data.cFileName);
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                collectSupportedFiles(path, output);
            }
        } else if (supportedExtension(data.cFileName)) {
            output.push_back(path);
        }
    } while (FindNextFileW(find, &data));
    FindClose(find);
}

enum class UiState {
    Ready,
    NoConversion,
    NotArchive,
    Unsupported,
    Running,
    Success,
    Failure,
    Cancelled
};

const wchar_t* stateText(UiState state, UiLanguage language) {
    const bool english = language == UiLanguage::English;
    switch (state) {
        case UiState::Ready: return english ? L"Ready" : L"待处理";
        case UiState::NoConversion: return english ? L"Nothing to do" : L"无需转换";
        case UiState::NotArchive: return english ? L"Video" : L"视频文件";
        case UiState::Unsupported: return english ? L"Unsupported" : L"暂不支持";
        case UiState::Running: return english ? L"Working" : L"处理中";
        case UiState::Success: return english ? L"Done" : L"已完成";
        case UiState::Cancelled: return english ? L"Cancelled" : L"已取消";
        default: return english ? L"Failed" : L"失败";
    }
}

struct FileItem {
    std::wstring path;
    std::wstring output;
    std::wstring detail;
    ziprepair::Layout layout;
    uint64_t fileSize = 0;
    uint64_t workBytes = 0;
    uint64_t copiedBytes = 0;
    UiState state = UiState::Ready;
};

class App {
public:
    bool create(HINSTANCE instance, int showCommand) {
        instance_ = instance;
        loadSettings();
        refreshBrushes();
        InitializeCriticalSection(&lock_);
        lockInitialized_ = true;
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        comInitialized_ = SUCCEEDED(com);

        INITCOMMONCONTROLSEX common{sizeof(common), ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS |
                                                      ICC_STANDARD_CLASSES | ICC_LINK_CLASS};
        InitCommonControlsEx(&common);

        WNDCLASSEXW windowClass{sizeof(windowClass)};
        windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        windowClass.lpfnWndProc = &App::windowProc;
        windowClass.hInstance = instance_;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = kWindowClass;
        windowClass.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(1));
        windowClass.hIconSm = windowClass.hIcon;
        if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }

        WNDCLASSEXW aboutClass{sizeof(aboutClass)};
        aboutClass.style = CS_HREDRAW | CS_VREDRAW;
        aboutClass.lpfnWndProc = &App::aboutProc;
        aboutClass.hInstance = instance_;
        aboutClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        aboutClass.lpszClassName = kAboutClass;
        aboutClass.hIcon = windowClass.hIcon;
        aboutClass.hIconSm = windowClass.hIcon;
        if (!RegisterClassExW(&aboutClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }

        dpi_ = static_cast<int>(GetDpiForSystem());
        RECT rectangle{0, 0, scale(960), scale(720)};
        AdjustWindowRectEx(&rectangle, WS_OVERLAPPEDWINDOW, FALSE, 0);
        window_ = CreateWindowExW(0, kWindowClass, windowTitle(),
                                  WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                  CW_USEDEFAULT, CW_USEDEFAULT,
                                  rectangle.right - rectangle.left, rectangle.bottom - rectangle.top,
                                  nullptr, nullptr, instance_, this);
        if (!window_) return false;
        ShowWindow(window_, showCommand);
        UpdateWindow(window_);
        return true;
    }

    void addInitialFiles(const std::vector<std::wstring>& paths) { addFiles(paths); }

    ~App() {
        if (worker_) {
            InterlockedExchange(&cancel_, 1);
            WaitForSingleObject(worker_, INFINITE);
            CloseHandle(worker_);
        }
        if (lockInitialized_) DeleteCriticalSection(&lock_);
        if (comInitialized_) CoUninitialize();
        if (uiFont_) DeleteObject(uiFont_);
        if (titleFont_) DeleteObject(titleFont_);
        if (sectionFont_) DeleteObject(sectionFont_);
        if (smallFont_) DeleteObject(smallFont_);
        if (headerIcon_) DestroyIcon(headerIcon_);
        if (windowBrush_) DeleteObject(windowBrush_);
        if (cardBrush_) DeleteObject(cardBrush_);
    }

private:
    bool english() const { return language_ == UiLanguage::English; }
    const wchar_t* tr(const wchar_t* chinese, const wchar_t* englishText) const {
        return english() ? englishText : chinese;
    }
    ziprepair::Language coreLanguage() const {
        return english() ? ziprepair::Language::English : ziprepair::Language::Chinese;
    }

    void loadSettings() {
        DWORD storedTheme = 0;
        if (queryDword(HKEY_CURRENT_USER, kSettingsKey, kThemeValue, storedTheme)) {
            light_ = storedTheme != 0;
        } else {
            light_ = systemUsesLightTheme();
            storeDword(kThemeValue, light_ ? 1 : 0);
        }
        DWORD storedLanguage = 0;
        if (queryDword(HKEY_CURRENT_USER, kSettingsKey, kLanguageValue, storedLanguage)) {
            language_ = storedLanguage ? UiLanguage::English : UiLanguage::Chinese;
        } else {
            language_ = systemLanguageIsChinese() ? UiLanguage::Chinese : UiLanguage::English;
            storeDword(kLanguageValue, english() ? 1 : 0);
        }
        palette_ = light_ ? kLightPalette : kDarkPalette;
    }

    void refreshBrushes() {
        if (windowBrush_) DeleteObject(windowBrush_);
        if (cardBrush_) DeleteObject(cardBrush_);
        windowBrush_ = CreateSolidBrush(palette_.window);
        cardBrush_ = CreateSolidBrush(palette_.card);
    }

    int scale(int value) const { return MulDiv(value, dpi_, 96); }

    HFONT font(int points, int weight) const {
        return CreateFontW(-MulDiv(points, dpi_, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                           L"Microsoft YaHei UI");
    }

    void recreateFonts() {
        if (uiFont_) DeleteObject(uiFont_);
        if (titleFont_) DeleteObject(titleFont_);
        if (sectionFont_) DeleteObject(sectionFont_);
        if (smallFont_) DeleteObject(smallFont_);
        uiFont_ = font(10, FW_NORMAL);
        titleFont_ = font(20, FW_SEMIBOLD);
        sectionFont_ = font(11, FW_SEMIBOLD);
        smallFont_ = font(9, FW_NORMAL);
    }

    const wchar_t* windowTitle() const {
        return tr(L"伪装压缩包修复器", L"Disguised Archive Repairer");
    }

    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        App* self = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<App*>(create->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return self ? self->handleMessage(message, wParam, lParam)
                    : DefWindowProcW(window, message, wParam, lParam);
    }

    static LRESULT CALLBACK headerProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                                       UINT_PTR, DWORD_PTR reference) {
        App* self = reinterpret_cast<App*>(reference);
        if (message == WM_ERASEBKGND) return 1;
        if (message == WM_PAINT) {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            RECT client{};
            GetClientRect(window, &client);
            FillRect(dc, &client, self->cardBrush_);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, self->palette_.muted);
            SelectObject(dc, self->uiFont_);
            const int count = Header_GetItemCount(window);
            for (int i = 0; i < count; ++i) {
                wchar_t text[256]{};
                HDITEMW item{};
                item.mask = HDI_TEXT;
                item.pszText = text;
                item.cchTextMax = 256;
                SendMessageW(window, HDM_GETITEMW, i, reinterpret_cast<LPARAM>(&item));
                RECT rc{};
                SendMessageW(window, HDM_GETITEMRECT, i, reinterpret_cast<LPARAM>(&rc));
                RECT label = rc;
                label.left += self->scale(10);
                label.right -= self->scale(4);
                DrawTextW(dc, text, -1, &label,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                RECT edge = rc;
                edge.left = edge.right - 1;
                HBRUSH border = CreateSolidBrush(self->palette_.border);
                FillRect(dc, &edge, border);
                DeleteObject(border);
            }
            RECT bottom{0, client.bottom - 1, client.right, client.bottom};
            HBRUSH border = CreateSolidBrush(self->palette_.border);
            FillRect(dc, &bottom, border);
            DeleteObject(border);
            EndPaint(window, &paint);
            return 0;
        }
        if (message == WM_NCDESTROY) RemoveWindowSubclass(window, &App::headerProc, 1);
        return DefSubclassProc(window, message, wParam, lParam);
    }


    static LRESULT CALLBACK outputEditProc(HWND window, UINT message, WPARAM wParam,
                                           LPARAM lParam, UINT_PTR, DWORD_PTR reference) {
        App* self = reinterpret_cast<App*>(reference);
        if (message == WM_PAINT && GetFocus() != window) {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            RECT client{};
            GetClientRect(window, &client);
            FillRect(dc, &client, self->cardBrush_);
            const int length = GetWindowTextLengthW(window);
            std::vector<wchar_t> buffer(static_cast<size_t>(length) + 1, L'\0');
            if (length > 0) GetWindowTextW(window, buffer.data(), length + 1);
            const wchar_t* text = length > 0 ? buffer.data() :
                self->tr(L"留空：与各源文件保存在同一目录",
                         L"Empty: save next to each source file");
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, length > 0 ? self->palette_.text : self->palette_.muted);
            SelectObject(dc, self->uiFont_);
            RECT label = client;
            label.left += self->scale(10);
            label.right -= self->scale(10);
            DrawTextW(dc, text, -1, &label,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                          (length > 0 ? DT_PATH_ELLIPSIS : DT_END_ELLIPSIS));
            EndPaint(window, &paint);
            return 0;
        }
        if (message == WM_SETFOCUS || message == WM_KILLFOCUS) {
            InvalidateRect(window, nullptr, TRUE);
        }
        if (message == WM_NCDESTROY) {
            RemoveWindowSubclass(window, &App::outputEditProc, 1);
        }
        return DefSubclassProc(window, message, wParam, lParam);
    }

    static LRESULT CALLBACK aboutProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        App* self = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<App*>(create->lpCreateParams);
            self->about_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return self ? self->handleAboutMessage(message, wParam, lParam)
                    : DefWindowProcW(window, message, wParam, lParam);
    }

    HWND makeControl(const wchar_t* className, const wchar_t* text, DWORD style,
                     int x, int y, int width, int height, int id = 0,
                     DWORD extendedStyle = 0) {
        HWND control = CreateWindowExW(extendedStyle, className, text,
                                       WS_CHILD | WS_VISIBLE | style,
                                       scale(x), scale(y), scale(width), scale(height),
                                       window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                       instance_, nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_), TRUE);
        return control;
    }

    HWND makeAboutControl(const wchar_t* className, const wchar_t* text, DWORD style,
                          int x, int y, int width, int height, int id = 0,
                          DWORD extendedStyle = 0) {
        HWND control = CreateWindowExW(extendedStyle, className, text,
                                       WS_CHILD | WS_VISIBLE | style,
                                       scale(x), scale(y), scale(width), scale(height),
                                       about_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                       instance_, nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_), TRUE);
        return control;
    }

    std::wstring themeChipText() const {
        if (english()) return light_ ? L"Light" : L"Dark";
        return light_ ? L"浅色" : L"深色";
    }

    const wchar_t* languageChipText() const {
        return english() ? L"中文" : L"EN";
    }

    void createControls() {
        dpi_ = static_cast<int>(GetDpiForWindow(window_));
        recreateFonts();
        headerIcon_ = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                                    scale(48), scale(48), 0));
        SendMessageW(window_, WM_SETICON, ICON_BIG,
                     reinterpret_cast<LPARAM>(LoadIconW(instance_, MAKEINTRESOURCEW(1))));
        SendMessageW(window_, WM_SETICON, ICON_SMALL,
                     reinterpret_cast<LPARAM>(LoadIconW(instance_, MAKEINTRESOURCEW(1))));

        addButton_ = makeControl(WC_BUTTONW, tr(L"＋ 添加文件", L"+ Add files"),
                                 BS_OWNERDRAW | WS_TABSTOP, 24, 98, 112, 36, IDC_ADD);
        removeButton_ = makeControl(WC_BUTTONW, tr(L"移除所选", L"Remove"),
                                    BS_OWNERDRAW | WS_TABSTOP, 146, 98, 102, 36, IDC_REMOVE);
        clearButton_ = makeControl(WC_BUTTONW, tr(L"清空列表", L"Clear"),
                                   BS_OWNERDRAW | WS_TABSTOP, 258, 98, 102, 36, IDC_CLEAR);
        summary_ = makeControl(WC_STATICW, tr(L"0 个文件", L"0 files"), SS_RIGHT | SS_CENTERIMAGE,
                               660, 98, 276, 36);

        outputLabel_ = makeControl(WC_STATICW, tr(L"保存到", L"Save to"), SS_LEFT | SS_CENTERIMAGE,
                                   42, 158, 58, 28);
        outputEdit_ = makeControl(WC_EDITW, L"", ES_AUTOHSCROLL | WS_TABSTOP,
                                  108, 158, 620, 30, IDC_OUTPUT);
        SetWindowSubclass(outputEdit_, &App::outputEditProc, 1,
                          reinterpret_cast<DWORD_PTR>(this));
        browseButton_ = makeControl(WC_BUTTONW, tr(L"浏览…", L"Browse…"),
                                    BS_OWNERDRAW | WS_TABSTOP, 828, 156, 88, 32, IDC_BROWSE);
        trimCheck_ = makeControl(WC_BUTTONW, tr(L"同时清理末尾的多余数据", L"Trim trailing data"),
                                 BS_OWNERDRAW | WS_TABSTOP, 42, 200, 340, 24, IDC_TRIM);

        list_ = makeControl(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SHOWSELALWAYS | WS_TABSTOP,
                            24, 282, 912, 290, IDC_LIST);
        SetWindowTheme(list_, light_ ? L"Explorer" : L"DarkMode_Explorer", nullptr);
        ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
                                                 LVS_EX_LABELTIP | LVS_EX_INFOTIP);
        ListView_SetBkColor(list_, palette_.card);
        ListView_SetTextBkColor(list_, palette_.card);
        ListView_SetTextColor(list_, palette_.text);
        listHeader_ = reinterpret_cast<HWND>(SendMessageW(list_, LVM_GETHEADER, 0, 0));
        if (listHeader_) {
            SetWindowSubclass(listHeader_, &App::headerProc, 1,
                              reinterpret_cast<DWORD_PTR>(this));
        }
        emptyText_ = makeControl(WC_STATICW,
            tr(L"把 ZIP、RAR、7Z 或伪装成 MP4 的文件拖到这里\r\n\r\n也可以点击上方“添加文件”，支持批量和整个文件夹",
               L"Drop ZIP, RAR, 7Z or MP4-disguised files here\r\n\r\nOr click “Add files” — batch and whole folders work"),
            SS_CENTER, 160, 360, 640, 90);
        SendMessageW(emptyText_, WM_SETFONT, reinterpret_cast<WPARAM>(sectionFont_), TRUE);

        status_ = makeControl(WC_STATICW, tr(L"准备就绪 · 源文件始终保持不变", L"Ready · source files are never modified"),
                              SS_LEFT | SS_CENTERIMAGE, 24, 594, 912, 24, IDC_STATUS);
        progress_ = makeControl(PROGRESS_CLASSW, L"", PBS_SMOOTH,
                                24, 624, 912, 6, IDC_PROGRESS);
        SendMessageW(progress_, PBM_SETRANGE, 0, MAKELPARAM(0, 1000));
        SendMessageW(progress_, PBM_SETBARCOLOR, 0, palette_.accent);
        SendMessageW(progress_, PBM_SETBKCOLOR, 0, palette_.border);
        ShowWindow(progress_, SW_HIDE);

        author_ = makeControl(WC_STATICW, L"KILL-STAR SX", SS_LEFT | SS_CENTERIMAGE,
                              24, 662, 200, 28);
        SendMessageW(author_, WM_SETFONT, reinterpret_cast<WPARAM>(smallFont_), TRUE);
        openButton_ = makeControl(WC_BUTTONW, tr(L"打开输出位置", L"Open output"),
                                  BS_OWNERDRAW | WS_TABSTOP, 458, 656, 132, 38, IDC_OPEN);
        cancelButton_ = makeControl(WC_BUTTONW, tr(L"取消", L"Cancel"),
                                    BS_OWNERDRAW | WS_TABSTOP, 602, 656, 84, 38, IDC_CANCEL);
        startButton_ = makeControl(WC_BUTTONW, tr(L"开始批量转换", L"Start batch"),
                                   BS_OWNERDRAW | WS_TABSTOP, 698, 656, 238, 38, IDC_START);

        themeChip_ = makeControl(WC_BUTTONW, themeChipText().c_str(), BS_OWNERDRAW | WS_TABSTOP,
                                 0, 28, 88, 34, IDC_THEME);
        languageChip_ = makeControl(WC_BUTTONW, languageChipText(), BS_OWNERDRAW | WS_TABSTOP,
                                    0, 28, 60, 34, IDC_LANGUAGE);
        aboutButton_ = makeControl(WC_BUTTONW, tr(L"关于", L"About"), BS_OWNERDRAW | WS_TABSTOP,
                                   0, 28, 72, 34, IDC_ABOUT);

        for (HWND control : {addButton_, removeButton_, clearButton_, browseButton_, trimCheck_,
                             openButton_, cancelButton_, startButton_, themeChip_, languageChip_,
                             aboutButton_}) {
            SetWindowSubclass(control, &App::hoverProc, 1, reinterpret_cast<DWORD_PTR>(this));
        }

        EnableWindow(cancelButton_, FALSE);
        EnableWindow(openButton_, FALSE);

        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.pszText = const_cast<LPWSTR>(tr(L"文件", L"File"));
        column.cx = scale(330);
        column.iSubItem = 0;
        ListView_InsertColumn(list_, 0, &column);
        column.pszText = const_cast<LPWSTR>(tr(L"大小", L"Size"));
        column.cx = scale(96);
        column.iSubItem = 1;
        ListView_InsertColumn(list_, 1, &column);
        column.pszText = const_cast<LPWSTR>(tr(L"状态", L"State"));
        column.cx = scale(86);
        column.iSubItem = 2;
        ListView_InsertColumn(list_, 2, &column);
        column.pszText = const_cast<LPWSTR>(tr(L"检测结果 / 输出", L"Result / Output"));
        column.cx = scale(400);
        column.iSubItem = 3;
        ListView_InsertColumn(list_, 3, &column);

        DragAcceptFiles(window_, TRUE);
        layoutControls();
        updateButtons();
    }

    void layoutControls() {
        if (!list_) return;
        RECT client{};
        GetClientRect(window_, &client);
        const int width = client.right;
        const int height = client.bottom;
        const int margin = scale(24);
        const int content = std::max(scale(300), width - margin * 2);

        MoveWindow(addButton_, margin, scale(98), scale(112), scale(36), TRUE);
        MoveWindow(removeButton_, margin + scale(122), scale(98), scale(102), scale(36), TRUE);
        MoveWindow(clearButton_, margin + scale(234), scale(98), scale(102), scale(36), TRUE);
        MoveWindow(summary_, width - margin - scale(220), scale(98), scale(220), scale(36), TRUE);

        const int aboutWidth = scale(72);
        const int languageWidth = scale(60);
        const int themeWidth = scale(88);
        const int chipGap = scale(8);
        const int chipsY = scale(28);
        int chipX = width - margin - aboutWidth;
        MoveWindow(aboutButton_, chipX, chipsY, aboutWidth, scale(34), TRUE);
        chipX -= languageWidth + chipGap;
        MoveWindow(languageChip_, chipX, chipsY, languageWidth, scale(34), TRUE);
        chipX -= themeWidth + chipGap;
        MoveWindow(themeChip_, chipX, chipsY, themeWidth, scale(34), TRUE);

        const int browseWidth = scale(88);
        const int browseX = width - margin - scale(18) - browseWidth;
        MoveWindow(browseButton_, browseX, scale(156), browseWidth, scale(32), TRUE);
        MoveWindow(outputLabel_, margin + scale(18), scale(158), scale(58), scale(28), TRUE);
        const int outputX = margin + scale(84);
        MoveWindow(outputEdit_, outputX, scale(158),
                   std::max(scale(100), browseX - outputX - scale(12)), scale(30), TRUE);
        MoveWindow(trimCheck_, margin + scale(18), scale(200), scale(340), scale(24), TRUE);

        const int listY = scale(282);
        const int listHeight = std::max(scale(100), height - listY - scale(142));
        MoveWindow(list_, margin, listY, content, listHeight, TRUE);
        MoveWindow(emptyText_, margin + scale(40), listY + listHeight / 2 - scale(45),
                   std::max(scale(160), content - scale(80)), scale(90), TRUE);

        const int statusY = height - scale(126);
        MoveWindow(status_, margin, statusY, content, scale(24), TRUE);
        MoveWindow(progress_, margin, statusY + scale(30), content, scale(6), TRUE);
        const int buttonY = height - scale(64);
        MoveWindow(startButton_, width - margin - scale(238), buttonY, scale(238), scale(38), TRUE);
        MoveWindow(cancelButton_, width - margin - scale(334), buttonY, scale(84), scale(38), TRUE);
        MoveWindow(openButton_, width - margin - scale(478), buttonY, scale(132), scale(38), TRUE);
        MoveWindow(author_, margin, buttonY + scale(5), scale(200), scale(28), TRUE);

        const int stateWidth = english() ? scale(126) : scale(92);
        ListView_SetColumnWidth(list_, 0,
                                std::max(scale(180), content - scale(96) - stateWidth - scale(400)));
        ListView_SetColumnWidth(list_, 1, scale(96));
        ListView_SetColumnWidth(list_, 2, stateWidth);
        ListView_SetColumnWidth(list_, 3, scale(400));
        InvalidateRect(window_, nullptr, FALSE);
    }

    void setStatus(const std::wstring& text) { SetWindowTextW(status_, text.c_str()); }

    void updateButtons() {
        EnableWindow(addButton_, !working_);
        EnableWindow(removeButton_, !working_ && ListView_GetSelectedCount(list_) > 0);
        EnableWindow(clearButton_, !working_ && !items_.empty());
        EnableWindow(outputEdit_, !working_);
        EnableWindow(browseButton_, !working_);
        EnableWindow(trimCheck_, !working_);
        EnableWindow(startButton_, !working_ && hasReadyItems());
        EnableWindow(cancelButton_, working_ && !InterlockedCompareExchange(&cancel_, 0, 0));
        EnableWindow(openButton_, !working_ && selectedOutputAvailable());
        ShowWindow(emptyText_, items_.empty() && !working_ ? SW_SHOW : SW_HIDE);
        std::wstring summary;
        if (items_.empty()) {
            summary = tr(L"0 个文件", L"0 files");
        } else if (english()) {
            summary = std::to_wstring(items_.size()) + L" files · " +
                      std::to_wstring(ListView_GetSelectedCount(list_)) + L" selected";
        } else {
            summary = std::to_wstring(items_.size()) + L" 个文件 · " +
                      std::to_wstring(ListView_GetSelectedCount(list_)) + L" 个已选";
        }
        SetWindowTextW(summary_, summary.c_str());
    }

    bool hasReadyItems() const {
        return std::any_of(items_.begin(), items_.end(), [](const FileItem& item) {
            return item.state == UiState::Ready || item.state == UiState::Failure ||
                   item.state == UiState::Cancelled;
        });
    }

    bool selectedOutputAvailable() const {
        const int selected = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
        if (selected < 0 || static_cast<size_t>(selected) >= items_.size()) return false;
        return isRegularFile(items_[static_cast<size_t>(selected)].output);
    }

    bool alreadyAdded(const std::wstring& path) const {
        return std::any_of(items_.begin(), items_.end(), [&](const FileItem& item) {
            return _wcsicmp(item.path.c_str(), path.c_str()) == 0;
        });
    }

    void classify(FileItem& item) {
        const ziprepair::Layout& layout = item.layout;
        if (!layout.error.empty()) {
            item.state = UiState::Failure;
            item.detail = layout.error;
            return;
        }
        if (ziprepair::formatIsVideo(layout.format)) {
            item.state = UiState::NotArchive;
            item.detail = ziprepair::describeLayout(layout, coreLanguage());
            return;
        }
        if (!layout.repairable()) {
            item.state = UiState::Unsupported;
            item.detail = ziprepair::describeLayout(layout, coreLanguage());
            return;
        }
        item.workBytes = layout.fileSize - layout.archiveStart;
        if (layout.archiveStart == 0) {
            item.state = UiState::NoConversion;
            if (layout.trailingBytes()) {
                item.detail = tr(L"结构正常 · 有多余尾部，可直接使用",
                                 L"Structure OK · trailing data present, usable as-is");
            } else {
                item.detail = tr(L"结构正常 · 无需处理", L"Structure OK · nothing to do");
            }
        } else {
            item.state = UiState::Ready;
            item.detail = ziprepair::describeLayout(layout, coreLanguage());
        }
    }

    void addFiles(const std::vector<std::wstring>& paths) {
        if (working_) return;
        size_t added = 0;
        for (const auto& input : paths) {
            const DWORD attributes = GetFileAttributesW(nativePath(input).c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) continue;
            if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
                std::vector<std::wstring> children;
                collectSupportedFiles(input, children);
                addFiles(children);
                continue;
            }
            const std::wstring path = normalized(input);
            if (alreadyAdded(path)) continue;

            FileItem item;
            item.path = path;
            item.fileSize = fileSize(path);
            item.layout = ziprepair::inspect(path, coreLanguage());
            classify(item);
            items_.push_back(std::move(item));
            ++added;
        }
        rebuildList();
        if (added) setStatus(tr(L"检测完成，点击“开始批量转换”", L"Inspected — click “Start batch”"));
        updateButtons();
    }

    void rebuildList() {
        ListView_DeleteAllItems(list_);
        for (size_t index = 0; index < items_.size(); ++index) {
            const FileItem& item = items_[index];
            LVITEMW row{};
            row.mask = LVIF_TEXT;
            row.iItem = static_cast<int>(index);
            const std::wstring label = fileNameOf(item.path);
            row.pszText = const_cast<LPWSTR>(label.c_str());
            const int inserted = ListView_InsertItem(list_, &row);
            ListView_SetItemText(list_, inserted, 1,
                const_cast<LPWSTR>(ziprepair::formatSize(item.fileSize).c_str()));
            ListView_SetItemText(list_, inserted, 2,
                const_cast<LPWSTR>(stateText(item.state, language_)));
            ListView_SetItemText(list_, inserted, 3, const_cast<LPWSTR>(item.detail.c_str()));
        }
        updateButtons();
    }

    void refreshItem(size_t index) {
        if (index >= items_.size()) return;
        std::wstring detail;
        std::wstring runningStatus;
        {
            EnterCriticalSection(&lock_);
            const FileItem& item = items_[index];
            detail = item.detail;
            uint64_t total = 0;
            uint64_t copied = 0;
            size_t runningIndex = items_.size();
            for (size_t i = 0; i < items_.size(); ++i) {
                total += items_[i].workBytes;
                copied += items_[i].copiedBytes;
                if (items_[i].state == UiState::Running) runningIndex = i;
            }
            if (runningIndex < items_.size()) {
                const FileItem& current = items_[runningIndex];
                runningStatus = tr(L"正在处理 ", L"Processing ") + fileName(current.path) + L" · " +
                                ziprepair::formatSize(current.copiedBytes) + L" / " +
                                ziprepair::formatSize(current.workBytes);
            }
            const int position = total
                ? static_cast<int>((static_cast<long double>(copied) * 1000.0L) /
                                   static_cast<long double>(total))
                : 0;
            SendMessageW(progress_, PBM_SETPOS, std::clamp(position, 0, 1000), 0);
            LeaveCriticalSection(&lock_);
        }
        ListView_SetItemText(list_, static_cast<int>(index), 2,
                             const_cast<LPWSTR>(stateText(items_[index].state, language_)));
        ListView_SetItemText(list_, static_cast<int>(index), 3,
                             const_cast<LPWSTR>(detail.c_str()));
        if (!runningStatus.empty()) setStatus(runningStatus);
        updateButtons();
    }

    void chooseFiles() {
        std::vector<wchar_t> buffer(65536, L'\0');
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = window_;
        dialog.lpstrFilter = english()
            ? L"Archives and disguised files (*.zip;*.mp4;*.rar;*.7z)\0*.zip;*.mp4;*.rar;*.7z\0All files (*.*)\0*.*\0\0"
            : L"压缩包和伪装文件 (*.zip;*.mp4;*.rar;*.7z)\0*.zip;*.mp4;*.rar;*.7z\0所有文件 (*.*)\0*.*\0\0";
        dialog.lpstrFile = buffer.data();
        dialog.nMaxFile = static_cast<DWORD>(buffer.size());
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_ALLOWMULTISELECT |
                       OFN_EXPLORER | OFN_DONTADDTORECENT;
        dialog.lpstrTitle = tr(L"选择要检测或修复的文件", L"Choose files to inspect or repair");
        if (!GetOpenFileNameW(&dialog)) return;
        std::vector<std::wstring> paths;
        const wchar_t* cursor = buffer.data();
        const std::wstring first = cursor;
        cursor += first.size() + 1;
        if (*cursor == L'\0') {
            paths.push_back(first);
        } else {
            while (*cursor) {
                paths.push_back(joinPath(first, cursor));
                cursor += wcslen(cursor) + 1;
            }
        }
        addFiles(paths);
    }

    void chooseOutputDirectory() {
        IFileDialog* dialog = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&dialog)))) return;
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        dialog->SetTitle(tr(L"选择统一输出目录", L"Choose output folder"));
        if (SUCCEEDED(dialog->Show(window_))) {
            IShellItem* result = nullptr;
            if (SUCCEEDED(dialog->GetResult(&result))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    SetWindowTextW(outputEdit_, path);
                    CoTaskMemFree(path);
                }
                result->Release();
            }
        }
        dialog->Release();
    }

    void removeSelected() {
        std::vector<int> selected;
        for (int index = ListView_GetNextItem(list_, -1, LVNI_SELECTED); index >= 0;
             index = ListView_GetNextItem(list_, index, LVNI_SELECTED)) {
            selected.push_back(index);
        }
        for (auto it = selected.rbegin(); it != selected.rend(); ++it) {
            if (*it >= 0 && static_cast<size_t>(*it) < items_.size()) {
                items_.erase(items_.begin() + *it);
            }
        }
        rebuildList();
        setStatus(items_.empty()
            ? tr(L"准备就绪 · 源文件始终保持不变", L"Ready · source files are never modified")
            : tr(L"已移除所选文件", L"Selected files removed"));
    }

    void clearItems() {
        items_.clear();
        ListView_DeleteAllItems(list_);
        SendMessageW(progress_, PBM_SETPOS, 0, 0);
        setStatus(tr(L"准备就绪 · 源文件始终保持不变", L"Ready · source files are never modified"));
        updateButtons();
    }

    void startBatch() {
        if (working_) return;
        std::vector<wchar_t> output(static_cast<size_t>(GetWindowTextLengthW(outputEdit_)) + 1, L'\0');
        GetWindowTextW(outputEdit_, output.data(), static_cast<int>(output.size()));
        ziprepair::Options options;
        options.outputDirectory = output.data();
        options.trimTail = trimEnabled_;
        options.language = coreLanguage();
        queue_.clear();
        EnterCriticalSection(&lock_);
        for (size_t i = 0; i < items_.size(); ++i) {
            FileItem& item = items_[i];
            if (item.state == UiState::Ready || item.state == UiState::Failure ||
                item.state == UiState::Cancelled) {
                item.state = UiState::Ready;
                item.copiedBytes = 0;
                item.detail = tr(L"等待处理", L"Queued");
                item.output.clear();
                if (item.workBytes == 0) item.workBytes = item.fileSize;
                queue_.push_back(i);
            } else if (item.state == UiState::NoConversion) {
                item.copiedBytes = item.workBytes;
            }
        }
        LeaveCriticalSection(&lock_);
        if (queue_.empty()) return;

        working_ = true;
        SendMessageW(progress_, PBM_SETPOS, 0, 0);
        ShowWindow(progress_, SW_SHOW);
        closeWhenDone_ = false;
        InterlockedExchange(&cancel_, 0);
        completed_ = succeeded_ = failed_ = 0;
        setStatus(tr(L"正在准备转换…", L"Preparing…"));
        rebuildList();
        worker_ = CreateThread(nullptr, 0, &App::workerEntry,
                               new WorkerArguments{this, options}, 0, nullptr);
        if (!worker_) {
            working_ = false;
            setStatus(tr(L"无法启动批量任务", L"Unable to start the batch"));
        }
        updateButtons();
    }

    struct WorkerArguments {
        App* app;
        ziprepair::Options options;
    };

    struct CallbackContext {
        App* app;
        size_t index;
        uint64_t lastPost = 0;
    };

    static DWORD WINAPI workerEntry(LPVOID parameter) {
        WorkerArguments* arguments = static_cast<WorkerArguments*>(parameter);
        arguments->app->worker(arguments->options);
        delete arguments;
        return 0;
    }

    static void progressCallback(void* context, uint64_t copied, uint64_t total) {
        auto* callback = static_cast<CallbackContext*>(context);
        App* app = callback->app;
        EnterCriticalSection(&app->lock_);
        callback->app->items_[callback->index].copiedBytes = copied;
        callback->app->items_[callback->index].workBytes = total;
        LeaveCriticalSection(&app->lock_);
        const uint64_t now = GetTickCount64();
        if (copied == total || now - callback->lastPost >= 100) {
            callback->lastPost = now;
            PostMessageW(app->window_, WM_APP_UPDATE, callback->index, 0);
        }
    }

    static bool cancelCallback(void* context) {
        auto* callback = static_cast<CallbackContext*>(context);
        return InterlockedCompareExchange(&callback->app->cancel_, 0, 0) != 0;
    }

    void worker(const ziprepair::Options& options) {
        for (const size_t index : queue_) {
            if (InterlockedCompareExchange(&cancel_, 0, 0)) break;
            std::wstring path;
            EnterCriticalSection(&lock_);
            items_[index].state = UiState::Running;
            items_[index].detail = tr(L"正在读取并写入新文件…", L"Reading and writing the new file…");
            path = items_[index].path;
            LeaveCriticalSection(&lock_);
            PostMessageW(window_, WM_APP_UPDATE, index, 0);

            CallbackContext callback{this, index, 0};
            const ziprepair::Result result = ziprepair::convert(
                path, options, &callback, &App::progressCallback, &App::cancelCallback);

            EnterCriticalSection(&lock_);
            FileItem& item = items_[index];
            item.output = result.output;
            item.detail = result.message;
            item.layout = result.layout;
            if (result.status == ziprepair::Status::Success) {
                item.state = UiState::Success;
                item.copiedBytes = item.workBytes;
                ++succeeded_;
            } else if (result.status == ziprepair::Status::NoConversion) {
                item.state = UiState::NoConversion;
            } else if (result.status == ziprepair::Status::Cancelled) {
                item.state = UiState::Cancelled;
            } else {
                item.state = UiState::Failure;
                ++failed_;
            }
            LeaveCriticalSection(&lock_);
            ++completed_;
            PostMessageW(window_, WM_APP_UPDATE, index, 0);
        }

        if (InterlockedCompareExchange(&cancel_, 0, 0)) {
            EnterCriticalSection(&lock_);
            for (const size_t index : queue_) {
                FileItem& item = items_[index];
                if (item.state == UiState::Ready || item.state == UiState::Running) {
                    item.state = UiState::Cancelled;
                    item.detail = tr(L"未处理", L"Not processed");
                }
            }
            LeaveCriticalSection(&lock_);
        }
        PostMessageW(window_, WM_APP_FINISHED, 0, 0);
    }

    void finishBatch() {
        if (worker_) {
            WaitForSingleObject(worker_, INFINITE);
            CloseHandle(worker_);
            worker_ = nullptr;
        }
        working_ = false;
        if (english()) {
            setStatus(L"Finished · succeeded " + std::to_wstring(succeeded_) + L" · failed " +
                      std::to_wstring(failed_) + L" · total " + std::to_wstring(completed_));
        } else {
            setStatus(L"处理结束 · 成功 " + std::to_wstring(succeeded_) + L" · 失败 " +
                      std::to_wstring(failed_) + L" · 共 " + std::to_wstring(completed_) + L" 个");
        }
        updateButtons();
        ShowWindow(progress_, SW_HIDE);
        if (closeWhenDone_) DestroyWindow(window_);
    }

    void openOutput() {
        const int selected = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
        std::wstring target;
        if (selected >= 0 && static_cast<size_t>(selected) < items_.size()) {
            target = items_[static_cast<size_t>(selected)].output;
        }
        if (isRegularFile(target)) {
            const std::wstring arguments = L"/select,\"" + target + L"\"";
            ShellExecuteW(window_, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL);
            return;
        }
        std::vector<wchar_t> output(static_cast<size_t>(GetWindowTextLengthW(outputEdit_)) + 1, L'\0');
        GetWindowTextW(outputEdit_, output.data(), static_cast<int>(output.size()));
        if (*output.data()) ShellExecuteW(window_, L"open", output.data(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    HoverState& hoverStateFor(HWND hwnd) {
        for (auto& state : hoverStates_) {
            if (state.window == hwnd) return state;
        }
        hoverStates_.push_back(HoverState{hwnd, 0.0f, false, false});
        return hoverStates_.back();
    }

    float hoverAmount(HWND hwnd) const {
        for (const auto& state : hoverStates_) {
            if (state.window == hwnd) return state.value;
        }
        return 0.0f;
    }

    void setHover(HWND hwnd, bool hot) {
        HoverState& state = hoverStateFor(hwnd);
        if (state.hot == hot) return;
        state.hot = hot;
        if (!state.animating) {
            state.animating = true;
            SetTimer(hwnd, kHoverTimer, 16, nullptr);
        }
    }

    bool stepHover(HWND hwnd) {
        HoverState& state = hoverStateFor(hwnd);
        const float target = state.hot ? 1.0f : 0.0f;
        if (state.value < target) state.value = std::min(target, state.value + 0.18f);
        if (state.value > target) state.value = std::max(target, state.value - 0.18f);
        InvalidateRect(hwnd, nullptr, FALSE);
        if (state.value == target) {
            state.animating = false;
            KillTimer(hwnd, kHoverTimer);
            return false;
        }
        return true;
    }

    void forgetHover(HWND hwnd) {
        for (size_t i = 0; i < hoverStates_.size(); ++i) {
            if (hoverStates_[i].window == hwnd) {
                KillTimer(hwnd, kHoverTimer);
                KillTimer(hwnd, kToggleTimer);
                hoverStates_.erase(hoverStates_.begin() + static_cast<ptrdiff_t>(i));
                return;
            }
        }
    }

    static LRESULT CALLBACK hoverProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                                      UINT_PTR, DWORD_PTR refData) {
        App* self = reinterpret_cast<App*>(refData);
        switch (message) {
            case WM_MOUSEMOVE:
                if (self && IsWindowEnabled(hwnd)) {
                    self->setHover(hwnd, true);
                    TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
                    TrackMouseEvent(&track);
                }
                break;
            case WM_MOUSELEAVE:
                if (self) self->setHover(hwnd, false);
                break;
            case WM_TIMER:
                if (self && wParam == kHoverTimer) {
                    self->stepHover(hwnd);
                    return 0;
                }
                if (self && wParam == kToggleTimer && hwnd == self->trimCheck_) {
                    self->stepToggle();
                    return 0;
                }
                break;
            case WM_NCDESTROY:
                if (self) self->forgetHover(hwnd);
                RemoveWindowSubclass(hwnd, &App::hoverProc, 1);
                break;
            default:
                break;
        }
        return DefSubclassProc(hwnd, message, wParam, lParam);
    }

    void toggleTrim() {
        trimEnabled_ = !trimEnabled_;
        if (trimCheck_) {
            SetTimer(trimCheck_, kToggleTimer, 16, nullptr);
            InvalidateRect(trimCheck_, nullptr, FALSE);
        }
    }

    void stepToggle() {
        if (!trimCheck_) return;
        const float target = trimEnabled_ ? 1.0f : 0.0f;
        if (togglePos_ < target) togglePos_ = std::min(target, togglePos_ + 0.2f);
        if (togglePos_ > target) togglePos_ = std::max(target, togglePos_ - 0.2f);
        InvalidateRect(trimCheck_, nullptr, FALSE);
        if (togglePos_ == target) KillTimer(trimCheck_, kToggleTimer);
    }

    bool edgeRunsElevated() const {
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return false;
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        bool elevated = false;
        if (Process32FirstW(snapshot, &entry)) {
            do {
                if (lstrcmpiW(entry.szExeFile, L"msedge.exe") != 0) continue;
                HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                             entry.th32ProcessID);
                if (!process) continue;
                HANDLE token = nullptr;
                if (OpenProcessToken(process, TOKEN_QUERY, &token)) {
                    TOKEN_ELEVATION state{};
                    DWORD size = sizeof(state);
                    if (GetTokenInformation(token, TokenElevation, &state, sizeof(state), &size) &&
                        state.TokenIsElevated) {
                        elevated = true;
                    }
                    CloseHandle(token);
                }
                CloseHandle(process);
                if (elevated) break;
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
        return elevated;
    }

    void copyTextToClipboard(const std::wstring& text) {
        if (!OpenClipboard(window_)) return;
        EmptyClipboard();
        const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (memory) {
            void* target = GlobalLock(memory);
            if (target) {
                std::memcpy(target, text.c_str(), bytes);
                GlobalUnlock(memory);
                if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
            } else {
                GlobalFree(memory);
            }
        }
        CloseClipboard();
    }

    void openExternalUrl(const wchar_t* url) {
        if (edgeRunsElevated()) {
            std::wstring message =
                tr(L"检测到以管理员身份运行的 Microsoft Edge。\n\n"
                   L"请先完全退出 Edge（包括托盘图标），再点一次链接。\n"
                   L"链接已复制，也可以直接粘贴到浏览器打开：\n\n",
                   L"Microsoft Edge is running as administrator.\n\n"
                   L"Fully quit Edge (including its tray icon), then click the link again.\n"
                   L"The link has been copied; you can also paste it into a browser:\n\n");
            message += url;
            copyTextToClipboard(url);
            MessageBoxW(about_ ? about_ : window_, message.c_str(),
                        tr(L"打开链接", L"Open link"), MB_OK | MB_ICONINFORMATION);
            return;
        }
        // Let the shell resolve the registered browser directly. Starting a new
        // explorer.exe process for a URL can fail with STATUS_DLL_INIT_FAILED.
        const HINSTANCE launched =
            ShellExecuteW(window_, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(launched) <= 32) {
            const std::wstring arguments = L"url.dll,FileProtocolHandler \"" + std::wstring(url) + L"\"";
            ShellExecuteW(window_, L"open", L"rundll32.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL);
        }
    }

    RECT aboutLinkHitRect() {
        RECT rect{scale(34), scale(282), scale(34), scale(282)};
        if (!about_) return rect;
        HDC dc = GetDC(about_);
        if (!dc) return rect;
        HGDIOBJ old = SelectObject(dc, uiFont_);
        SIZE size{};
        GetTextExtentPoint32W(dc, kRepoDisplay, lstrlenW(kRepoDisplay), &size);
        SelectObject(dc, old);
        ReleaseDC(about_, dc);
        rect.right = rect.left + size.cx;
        rect.bottom = rect.top + size.cy;
        InflateRect(&rect, scale(2), scale(2));
        return rect;
    }

    void drawAboutLink(HDC dc) {
        if (!about_) return;
        SetBkMode(dc, TRANSPARENT);
        HGDIOBJ oldFont = SelectObject(dc, uiFont_);
        SetTextColor(dc, palette_.accent);
        const int x = scale(34);
        const int y = scale(282);
        const int length = lstrlenW(kRepoDisplay);
        TextOutW(dc, x, y, kRepoDisplay, length);
        if (aboutLinkHover_) {
            SIZE size{};
            GetTextExtentPoint32W(dc, kRepoDisplay, length, &size);
            HPEN pen = CreatePen(PS_SOLID, 1, palette_.accent);
            HGDIOBJ oldPen = SelectObject(dc, pen);
            MoveToEx(dc, x, y + size.cy + scale(1), nullptr);
            LineTo(dc, x + size.cx, y + size.cy + scale(1));
            SelectObject(dc, oldPen);
            DeleteObject(pen);
        }
        SelectObject(dc, oldFont);
    }

    void drawButton(const DRAWITEMSTRUCT& draw) {
        FillRect(draw.hDC, const_cast<LPRECT>(&draw.rcItem), windowBrush_);
        const int id = static_cast<int>(draw.CtlID);
        wchar_t text[128]{};
        GetWindowTextW(draw.hwndItem, text, 128);
        const bool disabled = (draw.itemState & ODS_DISABLED) != 0;
        const bool pressed = (draw.itemState & ODS_SELECTED) != 0;
        const bool primary = id == IDC_START || id == IDOK;
        const float hot = hoverAmount(draw.hwndItem);
        COLORREF fill = primary ? palette_.accent : palette_.secondary;
        COLORREF ink = primary ? RGB(255, 255, 255) : palette_.text;
        if (!disabled && !pressed) {
            fill = primary ? blendColor(palette_.accent, palette_.accentPressed, hot * 0.55f)
                           : blendColor(palette_.secondary, palette_.border, hot);
        }
        if (!disabled && pressed) fill = primary ? palette_.accentPressed : palette_.border;
        if (disabled) {
            fill = palette_.disabledFill;
            ink = palette_.disabledInk;
        }
        HBRUSH brush = CreateSolidBrush(fill);
        HPEN pen = CreatePen(PS_SOLID, 1, primary ? fill : palette_.border);
        HGDIOBJ oldBrush = SelectObject(draw.hDC, brush);
        HGDIOBJ oldPen = SelectObject(draw.hDC, pen);
        RoundRect(draw.hDC, draw.rcItem.left, draw.rcItem.top, draw.rcItem.right,
                  draw.rcItem.bottom, scale(10), scale(10));
        SelectObject(draw.hDC, oldBrush);
        SelectObject(draw.hDC, oldPen);
        DeleteObject(brush);
        DeleteObject(pen);
        SetBkMode(draw.hDC, TRANSPARENT);
        SetTextColor(draw.hDC, ink);
        SelectObject(draw.hDC, uiFont_);
        DrawTextW(draw.hDC, text, -1, const_cast<LPRECT>(&draw.rcItem),
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if ((draw.itemState & ODS_FOCUS) && !disabled) {
            RECT focus = draw.rcItem;
            InflateRect(&focus, -scale(3), -scale(3));
            HPEN focusPen = CreatePen(PS_DOT, 1, primary ? RGB(255, 255, 255) : palette_.muted);
            HGDIOBJ old = SelectObject(draw.hDC, focusPen);
            SelectObject(draw.hDC, GetStockObject(HOLLOW_BRUSH));
            RoundRect(draw.hDC, focus.left, focus.top, focus.right, focus.bottom,
                      scale(7), scale(7));
            SelectObject(draw.hDC, old);
            DeleteObject(focusPen);
        }
    }

    void drawChip(const DRAWITEMSTRUCT& draw) {
        FillRect(draw.hDC, const_cast<LPRECT>(&draw.rcItem), windowBrush_);
        wchar_t text[64]{};
        GetWindowTextW(draw.hwndItem, text, 64);
        const bool disabled = (draw.itemState & ODS_DISABLED) != 0;
        const bool pressed = (draw.itemState & ODS_SELECTED) != 0;
        const float hot = hoverAmount(draw.hwndItem);
        COLORREF fill = pressed ? palette_.border
                                : blendColor(palette_.secondary, palette_.border, hot);
        COLORREF ink = palette_.text;
        if (disabled) {
            fill = palette_.disabledFill;
            ink = palette_.disabledInk;
        }
        HBRUSH brush = CreateSolidBrush(fill);
        HPEN pen = CreatePen(PS_SOLID, 1, palette_.border);
        HGDIOBJ oldBrush = SelectObject(draw.hDC, brush);
        HGDIOBJ oldPen = SelectObject(draw.hDC, pen);
        RoundRect(draw.hDC, draw.rcItem.left, draw.rcItem.top, draw.rcItem.right,
                  draw.rcItem.bottom, draw.rcItem.bottom - draw.rcItem.top,
                  draw.rcItem.bottom - draw.rcItem.top);
        SelectObject(draw.hDC, oldBrush);
        SelectObject(draw.hDC, oldPen);
        DeleteObject(brush);
        DeleteObject(pen);
        SetBkMode(draw.hDC, TRANSPARENT);
        SetTextColor(draw.hDC, ink);
        SelectObject(draw.hDC, uiFont_);
        DrawTextW(draw.hDC, text, -1, const_cast<LPRECT>(&draw.rcItem),
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    void drawToggle(const DRAWITEMSTRUCT& draw) {
        const bool enabled = (draw.itemState & ODS_DISABLED) == 0;
        const RECT rect = draw.rcItem;
        FillRect(draw.hDC, const_cast<LPRECT>(&draw.rcItem), cardBrush_);
        const int trackHeight = scale(20);
        const int trackWidth = scale(38);
        RECT track{rect.left, rect.top + (rect.bottom - rect.top - trackHeight) / 2,
                   rect.left + trackWidth, rect.top + (rect.bottom - rect.top + trackHeight) / 2};
        const COLORREF trackColor = enabled ? blendColor(palette_.trackOff, palette_.accent, togglePos_)
                                            : palette_.trackOff;
        HBRUSH brush = CreateSolidBrush(trackColor);
        HPEN pen = CreatePen(PS_SOLID, 1, trackColor);
        HGDIOBJ oldBrush = SelectObject(draw.hDC, brush);
        HGDIOBJ oldPen = SelectObject(draw.hDC, pen);
        RoundRect(draw.hDC, track.left, track.top, track.right, track.bottom,
                  trackHeight, trackHeight);
        SelectObject(draw.hDC, oldBrush);
        SelectObject(draw.hDC, oldPen);
        DeleteObject(brush);
        DeleteObject(pen);

        const int pad = scale(2);
        const int knobSize = trackHeight - pad * 2;
        const int travel = trackWidth - pad * 2 - knobSize;
        const int knobLeft = track.left + pad + static_cast<int>(static_cast<float>(travel) * togglePos_ + 0.5f);
        RECT knob{knobLeft, track.top + pad, knobLeft + knobSize, track.top + pad + knobSize};
        brush = CreateSolidBrush(RGB(255, 255, 255));
        pen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
        oldBrush = SelectObject(draw.hDC, brush);
        oldPen = SelectObject(draw.hDC, pen);
        Ellipse(draw.hDC, knob.left, knob.top, knob.right, knob.bottom);
        SelectObject(draw.hDC, oldBrush);
        SelectObject(draw.hDC, oldPen);
        DeleteObject(brush);
        DeleteObject(pen);

        SetBkMode(draw.hDC, TRANSPARENT);
        SetTextColor(draw.hDC, enabled ? palette_.text : palette_.muted);
        SelectObject(draw.hDC, uiFont_);
        RECT label{track.right + scale(10), rect.top, rect.right, rect.bottom};
        DrawTextW(draw.hDC, tr(L"同时清理末尾的多余数据", L"Trim trailing data"), -1, &label,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        if ((draw.itemState & ODS_FOCUS) && enabled) {
            RECT focus = label;
            InflateRect(&focus, -scale(2), -scale(2));
            DrawFocusRect(draw.hDC, &focus);
        }
    }

    void paint() {
        PAINTSTRUCT paint{};
        HDC screen = BeginPaint(window_, &paint);
        RECT client{};
        GetClientRect(window_, &client);
        const int width = client.right;
        const int height = client.bottom;
        HDC dc = CreateCompatibleDC(screen);
        HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
        HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
        FillRect(dc, &client, windowBrush_);

        const int margin = scale(24);
        if (headerIcon_) {
            DrawIconEx(dc, margin, scale(20), headerIcon_, scale(48), scale(48),
                       0, nullptr, DI_NORMAL);
        }
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, palette_.text);
        SelectObject(dc, titleFont_);
        RECT title{margin + scale(64), scale(20), width - margin - scale(260), scale(50)};
        DrawTextW(dc, windowTitle(), -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SetTextColor(dc, palette_.muted);
        SelectObject(dc, smallFont_);
        RECT subtitle{margin + scale(65), scale(53), width - margin - scale(240), scale(77)};
        DrawTextW(dc,
                  tr(L"识别伪装压缩包、切除视频前缀，恢复 ZIP / RAR / 7Z · 支持 ZIP64 与批量处理",
                     L"Detects disguised archives, strips video prefixes, restores ZIP / RAR / 7Z · ZIP64 and batch ready"),
                  -1, &subtitle, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        RECT card{margin, scale(148), width - margin, scale(238)};
        HBRUSH cardBrush = CreateSolidBrush(palette_.card);
        HPEN border = CreatePen(PS_SOLID, 1, palette_.border);
        SelectObject(dc, cardBrush);
        SelectObject(dc, border);
        RoundRect(dc, card.left, card.top, card.right, card.bottom, scale(16), scale(16));
        DeleteObject(cardBrush);
        DeleteObject(border);

        SetTextColor(dc, palette_.text);
        SelectObject(dc, sectionFont_);
        RECT listTitle{margin, scale(252), margin + scale(180), scale(276)};
        DrawTextW(dc, tr(L"文件列表", L"Files"), -1, &listTitle,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        RECT edit{};
        GetWindowRect(outputEdit_, &edit);
        MapWindowPoints(nullptr, window_, reinterpret_cast<POINT*>(&edit), 2);
        InflateRect(&edit, scale(7), scale(4));
        HBRUSH editBrush = CreateSolidBrush(palette_.card);
        border = CreatePen(PS_SOLID, 1, palette_.border);
        SelectObject(dc, editBrush);
        SelectObject(dc, border);
        RoundRect(dc, edit.left, edit.top, edit.right, edit.bottom, scale(8), scale(8));
        DeleteObject(editBrush);
        DeleteObject(border);

        BitBlt(screen, 0, 0, width, height, dc, 0, 0, SRCCOPY);
        SelectObject(dc, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(dc);
        EndPaint(window_, &paint);
    }

    void applyFrameTheme(HWND hwnd) {
        if (!hwnd) return;
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        if (!dwm) return;
        using SetAttribute = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
        const auto setAttribute =
            reinterpret_cast<SetAttribute>(GetProcAddress(dwm, "DwmSetWindowAttribute"));
        if (setAttribute) {
            const BOOL dark = light_ ? FALSE : TRUE;
            if (FAILED(setAttribute(hwnd, 20, &dark, sizeof(dark)))) {
                setAttribute(hwnd, 19, &dark, sizeof(dark));
            }
            SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                             SWP_FRAMECHANGED);
        }
        FreeLibrary(dwm);
    }

    void applyTheme() {
        palette_ = light_ ? kLightPalette : kDarkPalette;
        applyFrameTheme(window_);
        refreshBrushes();
        SendMessageW(progress_, PBM_SETBARCOLOR, 0, palette_.accent);
        SendMessageW(progress_, PBM_SETBKCOLOR, 0, palette_.border);
        ListView_SetBkColor(list_, palette_.card);
        ListView_SetTextBkColor(list_, palette_.card);
        ListView_SetTextColor(list_, palette_.text);
        SetWindowTheme(list_, light_ ? L"Explorer" : L"DarkMode_Explorer", nullptr);
        if (listHeader_) InvalidateRect(listHeader_, nullptr, TRUE);
        SetWindowTextW(themeChip_, themeChipText().c_str());
        EnumChildWindows(window_, [](HWND child, LPARAM) -> BOOL {
            InvalidateRect(child, nullptr, TRUE);
            return TRUE;
        }, 0);
        InvalidateRect(window_, nullptr, FALSE);
        if (about_) {
            applyFrameTheme(about_);
            EnumChildWindows(about_, [](HWND child, LPARAM) -> BOOL {
                InvalidateRect(child, nullptr, TRUE);
                return TRUE;
            }, 0);
            InvalidateRect(about_, nullptr, FALSE);
        }
    }

    void toggleTheme() {
        light_ = !light_;
        storeDword(kThemeValue, light_ ? 1 : 0);
        applyTheme();
    }

    void relocalizeItem(FileItem& item) {
        if (working_ && item.state == UiState::Running) return;
        if (item.state == UiState::Success) {
            item.detail = tr(L"已输出：", L"Output: ") + item.output;
            return;
        }
        if (item.state == UiState::Cancelled) {
            item.detail = tr(L"未处理", L"Not processed");
            return;
        }
        item.layout = ziprepair::inspect(item.path, coreLanguage());
        classify(item);
    }

    void applyLanguageChipText() {
        SetWindowTextW(languageChip_, languageChipText());
        SetWindowTextW(themeChip_, themeChipText().c_str());
        SetWindowTextW(aboutButton_, tr(L"关于", L"About"));
    }

    void applyLanguage() {
        SetWindowTextW(window_, windowTitle());
        SetWindowTextW(addButton_, tr(L"＋ 添加文件", L"+ Add files"));
        SetWindowTextW(removeButton_, tr(L"移除所选", L"Remove"));
        SetWindowTextW(clearButton_, tr(L"清空列表", L"Clear"));
        SetWindowTextW(outputLabel_, tr(L"保存到", L"Save to"));
        InvalidateRect(outputEdit_, nullptr, TRUE);
        SetWindowTextW(browseButton_, tr(L"浏览…", L"Browse…"));
        SetWindowTextW(trimCheck_, tr(L"同时清理末尾的多余数据", L"Trim trailing data"));
        SetWindowTextW(status_, tr(L"准备就绪 · 源文件始终保持不变",
                                  L"Ready · source files are never modified"));
        SetWindowTextW(openButton_, tr(L"打开输出位置", L"Open output"));
        SetWindowTextW(cancelButton_, tr(L"取消", L"Cancel"));
        SetWindowTextW(startButton_, tr(L"开始批量转换", L"Start batch"));
        SetWindowTextW(emptyText_,
            tr(L"把 ZIP、RAR、7Z 或伪装成 MP4 的文件拖到这里\r\n\r\n也可以点击上方“添加文件”，支持批量和整个文件夹",
               L"Drop ZIP, RAR, 7Z or MP4-disguised files here\r\n\r\nOr click “Add files” — batch and whole folders work"));
        applyLanguageChipText();
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT;
        column.pszText = const_cast<LPWSTR>(tr(L"文件", L"File"));
        ListView_SetColumn(list_, 0, &column);
        column.pszText = const_cast<LPWSTR>(tr(L"大小", L"Size"));
        ListView_SetColumn(list_, 1, &column);
        column.pszText = const_cast<LPWSTR>(tr(L"状态", L"State"));
        ListView_SetColumn(list_, 2, &column);
        column.pszText = const_cast<LPWSTR>(tr(L"检测结果 / 输出", L"Result / Output"));
        ListView_SetColumn(list_, 3, &column);
        if (listHeader_) InvalidateRect(listHeader_, nullptr, TRUE);
        for (size_t i = 0; i < items_.size(); ++i) relocalizeItem(items_[i]);
        rebuildList();
        layoutControls();
        updateButtons();
        if (about_) updateAboutTexts();
        InvalidateRect(window_, nullptr, FALSE);
    }

    void toggleLanguage() {
        language_ = english() ? UiLanguage::Chinese : UiLanguage::English;
        storeDword(kLanguageValue, english() ? 1 : 0);
        applyLanguage();
    }

    void createAboutControls() {
        SetWindowTextW(about_, tr(L"关于 · 伪装压缩包修复器", L"About · Disguised Archive Repairer"));
        aboutTitle_ = makeAboutControl(WC_STATICW, windowTitle(), SS_LEFT,
                                       32, 22, 544, 34);
        SendMessageW(aboutTitle_, WM_SETFONT, reinterpret_cast<WPARAM>(titleFont_), TRUE);
        aboutVersion_ = makeAboutControl(WC_STATICW, tr(L"版本 2.2 · Apache 2.0 许可",
                                                        L"Version 2.2 · Apache License 2.0"),
                                         SS_LEFT, 34, 60, 544, 22);
        aboutLogTitle_ = makeAboutControl(WC_STATICW, tr(L"更新日志", L"Changelog"), SS_LEFT,
                                          34, 96, 544, 24);
        SendMessageW(aboutLogTitle_, WM_SETFONT, reinterpret_cast<WPARAM>(sectionFont_), TRUE);
        aboutLog_ = makeAboutControl(WC_STATICW,
            tr(L"2.2  悬停与开关动画 · 链接悬停样式\n      以 Apache 2.0 许可开源发布\n2.1  深色 / 浅色主题 · 中文 / 英文界面\n      首次启动按系统语言与系统主题自动选择\n      新增关于窗口，可直接打开 GitHub\n2.0  RAR / 7Z 修复 · 视频文件识别 · 界面重做\n1.0  首个版本：修复 MP4 前缀 ZIP",
               L"2.2  Hover and toggle animations · link hover style\n      Released under the Apache License 2.0\n2.1  Dark / light theme · Chinese / English UI\n      First run follows system language and theme\n      About window with a direct GitHub link\n2.0  RAR / 7Z repair · video detection · redesigned UI\n1.0  First release: repair MP4-prefixed ZIP"),
            SS_LEFT, 34, 126, 544, 148);
        aboutLicense_ = makeAboutControl(WC_STATICW,
            tr(L"© 2026 KILL-STAR SX · 以 Apache 2.0 许可开源",
               L"© 2026 KILL-STAR SX · Open source under the Apache License 2.0"),
            SS_LEFT, 34, 316, 544, 22);
        SendMessageW(aboutLicense_, WM_SETFONT, reinterpret_cast<WPARAM>(smallFont_), TRUE);
        aboutClose_ = makeAboutControl(WC_BUTTONW, tr(L"关闭", L"Close"),
                                       BS_OWNERDRAW | WS_TABSTOP, 452, 348, 126, 36, IDOK);
        SetWindowSubclass(aboutClose_, &App::hoverProc, 1, reinterpret_cast<DWORD_PTR>(this));
    }

    void updateAboutTexts() {
        if (!about_) return;
        SetWindowTextW(about_, tr(L"关于 · 伪装压缩包修复器", L"About · Disguised Archive Repairer"));
        SetWindowTextW(aboutVersion_, tr(L"版本 2.2 · Apache 2.0 许可",
                                         L"Version 2.2 · Apache License 2.0"));
        SetWindowTextW(aboutLogTitle_, tr(L"更新日志", L"Changelog"));
        SetWindowTextW(aboutLog_,
            tr(L"2.2  悬停与开关动画 · 链接悬停样式\n      以 Apache 2.0 许可开源发布\n2.1  深色 / 浅色主题 · 中文 / 英文界面\n      首次启动按系统语言与系统主题自动选择\n      新增关于窗口，可直接打开 GitHub\n2.0  RAR / 7Z 修复 · 视频文件识别 · 界面重做\n1.0  首个版本：修复 MP4 前缀 ZIP",
               L"2.2  Hover and toggle animations · link hover style\n      Released under the Apache License 2.0\n2.1  Dark / light theme · Chinese / English UI\n      First run follows system language and theme\n      About window with a direct GitHub link\n2.0  RAR / 7Z repair · video detection · redesigned UI\n1.0  First release: repair MP4-prefixed ZIP"));
        SetWindowTextW(aboutTitle_, windowTitle());
        SetWindowTextW(aboutLicense_,
            tr(L"© 2026 KILL-STAR SX · 以 Apache 2.0 许可开源",
               L"© 2026 KILL-STAR SX · Open source under the Apache License 2.0"));
        SetWindowTextW(aboutClose_, tr(L"关闭", L"Close"));
        InvalidateRect(about_, nullptr, FALSE);
    }

    void showAbout() {
        if (about_) {
            SetForegroundWindow(about_);
            return;
        }
        RECT main{};
        GetWindowRect(window_, &main);
        const int width = scale(620);
        const int height = scale(420);
        const int x = main.left + ((main.right - main.left) - width) / 2;
        const int y = main.top + ((main.bottom - main.top) - height) / 2;
        about_ = CreateWindowExW(WS_EX_DLGMODALFRAME, kAboutClass,
                                 tr(L"关于 · 伪装压缩包修复器", L"About · Disguised Archive Repairer"),
                                 WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                                 x, y, width, height, window_, nullptr, instance_, this);
        if (!about_) return;
        applyFrameTheme(about_);
        ShowWindow(about_, SW_SHOWNORMAL);
        UpdateWindow(about_);
    }

    LRESULT handleAboutMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
            case WM_CREATE:
                createAboutControls();
                return 0;
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT: {
                PAINTSTRUCT paint{};
                HDC dc = BeginPaint(about_, &paint);
                RECT client{};
                GetClientRect(about_, &client);
                FillRect(dc, &client, windowBrush_);
                HPEN line = CreatePen(PS_SOLID, 1, palette_.border);
                HGDIOBJ old = SelectObject(dc, line);
                MoveToEx(dc, scale(32), scale(86), nullptr);
                LineTo(dc, client.right - scale(32), scale(86));
                SelectObject(dc, old);
                DeleteObject(line);
                drawAboutLink(dc);
                EndPaint(about_, &paint);
                return 0;
            }
            case WM_CTLCOLORSTATIC: {
                HDC dc = reinterpret_cast<HDC>(wParam);
                HWND control = reinterpret_cast<HWND>(lParam);
                SetBkMode(dc, TRANSPARENT);
                if (control == aboutVersion_ || control == aboutLicense_) {
                    SetTextColor(dc, palette_.muted);
                } else {
                    SetTextColor(dc, palette_.text);
                }
                return reinterpret_cast<LRESULT>(windowBrush_);
            }
            case WM_DRAWITEM: {
                const auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
                if (draw->CtlType == ODT_BUTTON) {
                    drawButton(*draw);
                    return TRUE;
                }
                break;
            }
            case WM_MOUSEMOVE: {
                const POINT point{static_cast<short>(LOWORD(lParam)),
                                  static_cast<short>(HIWORD(lParam))};
                RECT link = aboutLinkHitRect();
                const bool hot = PtInRect(&link, point) != FALSE;
                if (hot != aboutLinkHover_) {
                    aboutLinkHover_ = hot;
                    InvalidateRect(about_, &link, FALSE);
                }
                if (hot) {
                    TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, about_, 0};
                    TrackMouseEvent(&track);
                }
                break;
            }
            case WM_MOUSELEAVE:
                if (aboutLinkHover_) {
                    aboutLinkHover_ = false;
                    InvalidateRect(about_, nullptr, FALSE);
                }
                return 0;
            case WM_SETCURSOR:
                if (LOWORD(lParam) == HTCLIENT) {
                    POINT point{};
                    GetCursorPos(&point);
                    ScreenToClient(about_, &point);
                    RECT link = aboutLinkHitRect();
                    if (PtInRect(&link, point)) {
                        SetCursor(LoadCursorW(nullptr, IDC_HAND));
                        return TRUE;
                    }
                }
                break;
            case WM_LBUTTONUP: {
                POINT point{static_cast<short>(LOWORD(lParam)),
                            static_cast<short>(HIWORD(lParam))};
                RECT link = aboutLinkHitRect();
                if (PtInRect(&link, point)) {
                    openExternalUrl(kRepoUrl);
                    return 0;
                }
                break;
            }
            case WM_COMMAND:
                if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
                    DestroyWindow(about_);
                    return 0;
                }
                break;
            case WM_CLOSE:
                DestroyWindow(about_);
                return 0;
            case WM_DESTROY:
                aboutLinkHover_ = false;
                about_ = nullptr;
                return 0;
            default:
                break;
        }
        return DefWindowProcW(about_, message, wParam, lParam);
    }

    LRESULT handleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
            case WM_CREATE:
                createControls();
                return 0;
            case WM_SIZE:
                layoutControls();
                return 0;
            case WM_GETMINMAXINFO: {
                auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
                info->ptMinTrackSize.x = scale(800);
                info->ptMinTrackSize.y = scale(620);
                return 0;
            }
            case WM_DPICHANGED: {
                dpi_ = HIWORD(wParam);
                recreateFonts();
                EnumChildWindows(window_, [](HWND child, LPARAM font) -> BOOL {
                    SendMessageW(child, WM_SETFONT, font, TRUE);
                    return TRUE;
                }, reinterpret_cast<LPARAM>(uiFont_));
                SendMessageW(emptyText_, WM_SETFONT, reinterpret_cast<WPARAM>(sectionFont_), TRUE);
                SendMessageW(author_, WM_SETFONT, reinterpret_cast<WPARAM>(smallFont_), TRUE);
                const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
                SetWindowPos(window_, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left, suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                layoutControls();
                return 0;
            }
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT:
                paint();
                return 0;
            case WM_CTLCOLORSTATIC: {
                HDC dc = reinterpret_cast<HDC>(wParam);
                HWND control = reinterpret_cast<HWND>(lParam);
                SetBkMode(dc, TRANSPARENT);
                if (control == outputLabel_) {
                    SetTextColor(dc, palette_.text);
                    return reinterpret_cast<LRESULT>(cardBrush_);
                }
                if (control == emptyText_) {
                    SetTextColor(dc, palette_.muted);
                    return reinterpret_cast<LRESULT>(cardBrush_);
                }
                SetTextColor(dc, control == status_ ? palette_.text : palette_.muted);
                return reinterpret_cast<LRESULT>(windowBrush_);
            }
            case WM_CTLCOLOREDIT: {
                HDC dc = reinterpret_cast<HDC>(wParam);
                SetTextColor(dc, palette_.text);
                SetBkColor(dc, palette_.card);
                return reinterpret_cast<LRESULT>(cardBrush_);
            }
            case WM_CTLCOLORBTN: {
                HDC dc = reinterpret_cast<HDC>(wParam);
                SetBkMode(dc, TRANSPARENT);
                SetBkColor(dc, palette_.card);
                SetTextColor(dc, palette_.text);
                return reinterpret_cast<LRESULT>(cardBrush_);
            }
            case WM_DRAWITEM: {
                const auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
                if (draw->CtlID == IDC_TRIM) {
                    drawToggle(*draw);
                    return TRUE;
                }
                if (draw->CtlID == IDC_THEME || draw->CtlID == IDC_LANGUAGE ||
                    draw->CtlID == IDC_ABOUT) {
                    drawChip(*draw);
                    return TRUE;
                }
                if (draw->CtlType == ODT_BUTTON) {
                    drawButton(*draw);
                    return TRUE;
                }
                break;
            }
            case WM_COMMAND:
                if (LOWORD(wParam) == IDC_OUTPUT && HIWORD(wParam) == EN_CHANGE) {
                    InvalidateRect(window_, nullptr, FALSE);
                    return 0;
                }
                switch (LOWORD(wParam)) {
                    case IDC_ADD: chooseFiles(); return 0;
                    case IDC_REMOVE: removeSelected(); return 0;
                    case IDC_CLEAR: clearItems(); return 0;
                    case IDC_BROWSE: chooseOutputDirectory(); return 0;
                    case IDC_TRIM:
                        toggleTrim();
                        return 0;
                    case IDC_THEME: toggleTheme(); return 0;
                    case IDC_LANGUAGE: toggleLanguage(); return 0;
                    case IDC_ABOUT: showAbout(); return 0;
                    case IDC_START: startBatch(); return 0;
                    case IDC_CANCEL:
                        InterlockedExchange(&cancel_, 1);
                        setStatus(tr(L"正在取消当前文件，临时输出会被删除…",
                                     L"Cancelling — temporary output will be removed…"));
                        updateButtons();
                        return 0;
                    case IDC_OPEN: openOutput(); return 0;
                    default: break;
                }
                break;
            case WM_NOTIFY: {
                const auto* header = reinterpret_cast<LPNMHDR>(lParam);
                if (header->idFrom == IDC_LIST) {
                    if (header->code == LVN_GETINFOTIP) {
                        auto* tip = reinterpret_cast<LPNMLVGETINFOTIPW>(lParam);
                        if (tip->iItem >= 0 && static_cast<size_t>(tip->iItem) < items_.size()) {
                            lstrcpynW(tip->pszText, items_[tip->iItem].path.c_str(),
                                      tip->cchTextMax);
                        }
                        return 0;
                    }
                    if (header->code == LVN_ITEMCHANGED) updateButtons();
                    if (header->code == NM_CUSTOMDRAW) {
                        auto* draw = reinterpret_cast<LPNMLVCUSTOMDRAW>(lParam);
                        if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                        if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) return CDRF_NOTIFYSUBITEMDRAW;
                        if (draw->nmcd.dwDrawStage == CDDS_SUBITEM | CDDS_ITEMPREPAINT) {
                            const size_t index = draw->nmcd.dwItemSpec;
                            if (draw->iSubItem == 2 && index < items_.size()) {
                                draw->clrText = items_[index].state == UiState::Success ? palette_.success
                                    : items_[index].state == UiState::Failure ? palette_.error : palette_.text;
                            }
                            return CDRF_NEWFONT;
                        }
                    }
                }
                return 0;
            }
            case WM_DROPFILES: {
                HDROP drop = reinterpret_cast<HDROP>(wParam);
                const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
                std::vector<std::wstring> paths;
                for (UINT i = 0; i < count; ++i) {
                    const UINT length = DragQueryFileW(drop, i, nullptr, 0);
                    std::vector<wchar_t> path(length + 1, L'\0');
                    DragQueryFileW(drop, i, path.data(), length + 1);
                    paths.emplace_back(path.data());
                }
                DragFinish(drop);
                addFiles(paths);
                return 0;
            }
            case WM_APP_UPDATE:
                refreshItem(static_cast<size_t>(wParam));
                return 0;
            case WM_APP_FINISHED:
                finishBatch();
                return 0;
            case WM_CLOSE:
                if (working_) {
                    closeWhenDone_ = true;
                    InterlockedExchange(&cancel_, 1);
                    setStatus(tr(L"正在取消，完成后关闭窗口…", L"Cancelling — the window will close when done"));
                    updateButtons();
                } else {
                    DestroyWindow(window_);
                }
                return 0;
            case WM_DESTROY:
                DragAcceptFiles(window_, FALSE);
                if (about_) DestroyWindow(about_);
                PostQuitMessage(0);
                return 0;
            default:
                break;
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    }

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    HWND about_ = nullptr;
    int dpi_ = 96;
    bool comInitialized_ = false;
    bool lockInitialized_ = false;
    bool working_ = false;
    bool closeWhenDone_ = false;
    bool trimEnabled_ = true;
    bool light_ = false;
    UiLanguage language_ = UiLanguage::Chinese;
    Palette palette_ = kDarkPalette;
    HFONT uiFont_ = nullptr;
    HFONT titleFont_ = nullptr;
    HFONT sectionFont_ = nullptr;
    HFONT smallFont_ = nullptr;
    HICON headerIcon_ = nullptr;
    HBRUSH windowBrush_ = nullptr;
    HBRUSH cardBrush_ = nullptr;

    HWND addButton_ = nullptr;
    HWND removeButton_ = nullptr;
    HWND clearButton_ = nullptr;
    HWND summary_ = nullptr;
    HWND outputLabel_ = nullptr;
    HWND outputEdit_ = nullptr;
    HWND browseButton_ = nullptr;
    HWND trimCheck_ = nullptr;
    HWND list_ = nullptr;
    HWND listHeader_ = nullptr;
    HWND emptyText_ = nullptr;
    HWND progress_ = nullptr;
    HWND status_ = nullptr;
    HWND author_ = nullptr;
    HWND startButton_ = nullptr;
    HWND cancelButton_ = nullptr;
    HWND openButton_ = nullptr;
    HWND themeChip_ = nullptr;
    HWND languageChip_ = nullptr;
    HWND aboutButton_ = nullptr;

    HWND aboutTitle_ = nullptr;
    HWND aboutVersion_ = nullptr;
    HWND aboutLogTitle_ = nullptr;
    HWND aboutLog_ = nullptr;
    HWND aboutLicense_ = nullptr;
    HWND aboutClose_ = nullptr;
    bool aboutLinkHover_ = false;
    float togglePos_ = 1.0f;
    std::vector<HoverState> hoverStates_;

    CRITICAL_SECTION lock_{};
    HANDLE worker_ = nullptr;
    volatile LONG cancel_ = 0;
    std::vector<FileItem> items_;
    std::vector<size_t> queue_;
    size_t completed_ = 0;
    size_t succeeded_ = 0;
    size_t failed_ = 0;
};

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    App app;
    if (!app.create(instance, showCommand)) {
        MessageBoxW(nullptr, L"Window initialization failed", L"Zip Repair Desk", MB_ICONERROR);
        return 1;
    }

    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    std::vector<std::wstring> paths;
    for (int i = 1; i < count; ++i) paths.emplace_back(arguments[i]);
    if (arguments) LocalFree(arguments);
    if (!paths.empty()) app.addInitialFiles(paths);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
