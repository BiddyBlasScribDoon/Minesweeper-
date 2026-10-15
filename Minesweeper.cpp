// ============================================================================
//  Minesweeper - a classic Minesweeper clone using the native Win32 API + GDI
//
//  Build (Visual Studio 2019/2022):
//    1. File > New > Project > "Windows Desktop Application" (C++), name it
//       Minesweeper. Delete the generated .cpp/.h/.rc files.
//    2. Add this file (Project > Add Existing Item).
//    3. Build > Build Solution (Release | x64 or x86). Done - one .exe.
//
//  Or from a "Developer Command Prompt for VS":
//    cl /EHsc /O2 /W3 /DUNICODE /D_UNICODE Minesweeper.cpp /link /SUBSYSTEM:WINDOWS
//
//  Controls:
//    Left click        reveal a square
//    Right click       flag / question mark / clear
//    Middle click, or  "chord": reveal all neighbours of a satisfied number
//    left+right click
//    Smiley / F2       new game
// ============================================================================

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>

#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <random>
#include <vector>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "/SUBSYSTEM:WINDOWS")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ----------------------------------------------------------------------------
//  Constants
// ----------------------------------------------------------------------------
static const wchar_t* APP_TITLE   = L"Minesweeper";
static const wchar_t* MAIN_CLASS  = L"Win32MinesweeperMain";
static const wchar_t* DLG_CLASS   = L"Win32MinesweeperCustom";
static const wchar_t* REG_PATH    = L"Software\\Win32Minesweeper";

enum {
    ID_NEW = 100, ID_BEGINNER, ID_INTERMEDIATE, ID_EXPERT, ID_CUSTOM,
    ID_MARKS, ID_BESTTIMES, ID_EXIT, ID_ABOUT
};
enum { IDC_HEIGHT = 201, IDC_WIDTH = 202, IDC_MINES = 203 };
static const UINT_PTR TIMER_ID = 1;

enum MarkType : unsigned char { M_NONE = 0, M_FLAG = 1, M_QUEST = 2 };
enum GameState { S_READY, S_PLAYING, S_WON, S_LOST };
enum Face { F_NORMAL, F_SURPRISE, F_WIN, F_LOSE };

struct Cell {
    bool          mine     = false;
    bool          revealed = false;
    unsigned char mark     = M_NONE;
    unsigned char adj      = 0;
};

// ----------------------------------------------------------------------------
//  Globals
// ----------------------------------------------------------------------------
static HINSTANCE g_hInst   = nullptr;
static HWND      g_hwnd    = nullptr;
static HMENU     g_gameMenu = nullptr;
static HFONT     g_fontNum = nullptr;

static int  g_dpi = 96;
static int  g_cell, g_margin, g_headerH, g_digW, g_digH, g_smiley;

static int  g_level = 0;               // 0 beginner, 1 intermediate, 2 expert, 3 custom
static int  g_cols = 9, g_rows = 9, g_mines = 10;
static int  g_custCols = 9, g_custRows = 9, g_custMines = 10;
static bool g_marks = true;            // enable '?' marks
static DWORD g_best[3] = { 999, 999, 999 };

static std::vector<Cell> g_cells;
static GameState g_state = S_READY;
static int  g_time = 0, g_flags = 0, g_revealed = 0, g_exploded = -1;

static bool g_lDown = false, g_rDown = false, g_mDown = false;
static bool g_chord = false, g_chordFired = false;
static bool g_smileyDown = false, g_smileyHot = false;
static int  g_pressX = -1, g_pressY = -1;

static std::mt19937 g_rng;

// ----------------------------------------------------------------------------
//  Small helpers
// ----------------------------------------------------------------------------
static int Sc(int v) { return MulDiv(v, g_dpi, 96); }

static void InitMetrics()
{
    HDC dc = GetDC(nullptr);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);

    g_cell    = Sc(24);
    g_margin  = Sc(10);
    g_headerH = Sc(46);
    g_digW    = Sc(20);
    g_digH    = Sc(34);
    g_smiley  = Sc(34);

    if (g_fontNum) DeleteObject(g_fontNum);
    g_fontNum = CreateFontW(-Sc(18), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Arial");
}

static void Fill(HDC dc, int l, int t, int r, int b, COLORREF c)
{
    RECT rc = { l, t, r, b };
    SetBkColor(dc, c);
    ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &rc, nullptr, 0, nullptr);
}

static void DrawBevel(HDC dc, RECT r, int thick, bool raised)
{
    COLORREF tl = raised ? RGB(255, 255, 255) : RGB(128, 128, 128);
    COLORREF br = raised ? RGB(128, 128, 128) : RGB(255, 255, 255);
    for (int i = 0; i < thick; ++i) {
        Fill(dc, r.left + i, r.top + i, r.right - i, r.top + i + 1, tl);
        Fill(dc, r.left + i, r.top + i, r.left + i + 1, r.bottom - i, tl);
        Fill(dc, r.left + i, r.bottom - i - 1, r.right - i, r.bottom - i, br);
        Fill(dc, r.right - i - 1, r.top + i, r.right - i, r.bottom - i, br);
    }
}

static bool Active() { return g_state == S_READY || g_state == S_PLAYING; }
static bool InBounds(int x, int y) { return x >= 0 && y >= 0 && x < g_cols && y < g_rows; }
static void Redraw() { if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE); }

// ----------------------------------------------------------------------------
//  Geometry
// ----------------------------------------------------------------------------
static int ClientW() { return g_cols * g_cell + Sc(6) + 2 * g_margin; }
static int ClientH() { return g_margin + g_headerH + g_margin + g_rows * g_cell + Sc(6) + g_margin; }
static int GridX0()  { return g_margin + Sc(3); }
static int GridY0()  { return g_margin + g_headerH + g_margin + Sc(3); }

static RECT HeaderRect()
{
    RECT r = { g_margin, g_margin, ClientW() - g_margin, g_margin + g_headerH };
    return r;
}
static RECT GridOuterRect()
{
    int top = g_margin + g_headerH + g_margin;
    RECT r = { g_margin, top, ClientW() - g_margin, top + g_rows * g_cell + Sc(6) };
    return r;
}
static RECT CounterRect(bool left)
{
    RECT hr = HeaderRect();
    int w = 3 * g_digW + Sc(8), h = g_digH + Sc(6);
    int top = hr.top + (g_headerH - h) / 2;
    RECT r;
    r.top = top; r.bottom = top + h;
    if (left) { r.left = hr.left + Sc(6); r.right = r.left + w; }
    else      { r.right = hr.right - Sc(6); r.left = r.right - w; }
    return r;
}
static RECT SmileyRect()
{
    RECT hr = HeaderRect();
    int cx = ClientW() / 2;
    RECT r;
    r.left = cx - g_smiley / 2; r.right = r.left + g_smiley;
    r.top = hr.top + (g_headerH - g_smiley) / 2; r.bottom = r.top + g_smiley;
    return r;
}
static bool CellFromPoint(int px, int py, int& cx, int& cy)
{
    int x = px - GridX0(), y = py - GridY0();
    if (x < 0 || y < 0) return false;
    cx = x / g_cell; cy = y / g_cell;
    return InBounds(cx, cy);
}

// ----------------------------------------------------------------------------
//  Registry persistence
// ----------------------------------------------------------------------------
static DWORD RegGet(const wchar_t* name, DWORD def)
{
    HKEY k; DWORD v = def, sz = sizeof(v), type = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_PATH, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExW(k, name, nullptr, &type, (LPBYTE)&v, &sz) != ERROR_SUCCESS || type != REG_DWORD)
            v = def;
        RegCloseKey(k);
    }
    return v;
}
static void RegSet(const wchar_t* name, DWORD v)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_PATH, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
        RegCloseKey(k);
    }
}
static void ClampCustom(int& w, int& h, int& m)
{
    w = std::max(9, std::min(30, w));
    h = std::max(9, std::min(24, h));
    m = std::max(10, std::min((w - 1) * (h - 1), m));
}
static void LoadSettings()
{
    g_level = (int)std::min<DWORD>(3, RegGet(L"Level", 0));
    g_custCols  = (int)RegGet(L"Width", 9);
    g_custRows  = (int)RegGet(L"Height", 9);
    g_custMines = (int)RegGet(L"Mines", 10);
    ClampCustom(g_custCols, g_custRows, g_custMines);
    g_marks = RegGet(L"Marks", 1) != 0;
    g_best[0] = RegGet(L"BestBeginner", 999);
    g_best[1] = RegGet(L"BestIntermediate", 999);
    g_best[2] = RegGet(L"BestExpert", 999);
}
static void SaveSettings()
{
    RegSet(L"Level", (DWORD)g_level);
    RegSet(L"Width", (DWORD)g_custCols);
    RegSet(L"Height", (DWORD)g_custRows);
    RegSet(L"Mines", (DWORD)g_custMines);
    RegSet(L"Marks", g_marks ? 1 : 0);
}
static void SaveBest()
{
    RegSet(L"BestBeginner", g_best[0]);
    RegSet(L"BestIntermediate", g_best[1]);
    RegSet(L"BestExpert", g_best[2]);
}

// ----------------------------------------------------------------------------
//  Drawing
// ----------------------------------------------------------------------------
static void DrawDigit(HDC dc, int x, int y, int w, int h, int d)
{
    static const unsigned char masks[11] = {
        0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F, 0x40
    };
    const COLORREF on = RGB(255, 0, 0), off = RGB(70, 0, 0);
    int t = std::max(2, Sc(3));
    int half = h / 2;
    int gTop = y + half - t / 2, gBot = gTop + t;
    unsigned char m = masks[d];

    // a, b, c, d, e, f, g
    Fill(dc, x + t, y,         x + w - t, y + t,     (m & 0x01) ? on : off);
    Fill(dc, x + w - t, y + t, x + w,     gTop,      (m & 0x02) ? on : off);
    Fill(dc, x + w - t, gBot,  x + w,     y + h - t, (m & 0x04) ? on : off);
    Fill(dc, x + t, y + h - t, x + w - t, y + h,     (m & 0x08) ? on : off);
    Fill(dc, x, gBot,          x + t,     y + h - t, (m & 0x10) ? on : off);
    Fill(dc, x, y + t,         x + t,     gTop,      (m & 0x20) ? on : off);
    Fill(dc, x + t, gTop,      x + w - t, gBot,      (m & 0x40) ? on : off);
}

static void DrawCounter(HDC dc, RECT cr, int value)
{
    Fill(dc, cr.left, cr.top, cr.right, cr.bottom, RGB(0, 0, 0));
    DrawBevel(dc, cr, Sc(2), false);
    int d[3];
    if (value < 0) {
        int v = std::min(99, -value);
        d[0] = 10; d[1] = v / 10; d[2] = v % 10;
    } else {
        int v = std::min(999, value);
        d[0] = v / 100; d[1] = (v / 10) % 10; d[2] = v % 10;
    }
    for (int i = 0; i < 3; ++i)
        DrawDigit(dc, cr.left + Sc(4) + i * g_digW, cr.top + Sc(3), g_digW - Sc(3), g_digH, d[i]);
}

static void DrawSmiley(HDC dc, RECT s, Face face)
{
    bool down = g_smileyDown && g_smileyHot;
    int w = s.right - s.left;
    Fill(dc, s.left, s.top, s.right, s.bottom, RGB(192, 192, 192));
    if (down) {
        DrawBevel(dc, s, std::max(1, Sc(1)), false);
    } else {
        DrawBevel(dc, s, Sc(2), true);
    }
    int off = down ? Sc(1) : 0;
    int cx = (s.left + s.right) / 2 + off, cy = (s.top + s.bottom) / 2 + off;
    int r = w * 38 / 100;
    int pw = std::max(1, w / 16);

    HBRUSH yellow = CreateSolidBrush(RGB(255, 255, 0));
    HPEN   pen    = CreatePen(PS_SOLID, pw, RGB(0, 0, 0));
    HGDIOBJ oldB  = SelectObject(dc, yellow);
    HGDIOBJ oldP  = SelectObject(dc, pen);

    Ellipse(dc, cx - r, cy - r, cx + r + 1, cy + r + 1);

    HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
    int ex = r * 35 / 100, ey = cy - r * 25 / 100;
    int er = std::max(2, r / 8);

    auto smile = [&]() {
        int rm = r * 50 / 100, mc = cy + r / 10;
        Arc(dc, cx - rm, mc - rm, cx + rm, mc + rm, cx - rm, mc, cx + rm, mc);
    };

    switch (face) {
    case F_NORMAL:
    case F_SURPRISE:
        SelectObject(dc, black);
        Ellipse(dc, cx - ex - er, ey - er, cx - ex + er + 1, ey + er + 1);
        Ellipse(dc, cx + ex - er, ey - er, cx + ex + er + 1, ey + er + 1);
        if (face == F_NORMAL) smile();
        else Ellipse(dc, cx - r / 6, cy + r * 25 / 100, cx + r / 6 + 1, cy + r * 60 / 100);
        break;
    case F_WIN:
        SelectObject(dc, black);
        Rectangle(dc, cx - r * 65 / 100, cy - r * 40 / 100, cx - r * 5 / 100, cy - r * 5 / 100);
        Rectangle(dc, cx + r * 5 / 100,  cy - r * 40 / 100, cx + r * 65 / 100, cy - r * 5 / 100);
        MoveToEx(dc, cx - r * 5 / 100, cy - r * 30 / 100, nullptr);
        LineTo(dc, cx + r * 5 / 100, cy - r * 30 / 100);
        smile();
        break;
    case F_LOSE: {
        int e = r * 15 / 100;
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            int px = cx + sgn * ex;
            MoveToEx(dc, px - e, ey - e, nullptr); LineTo(dc, px + e, ey + e);
            MoveToEx(dc, px - e, ey + e, nullptr); LineTo(dc, px + e, ey - e);
        }
        int rm = r * 35 / 100, mc = cy + r * 70 / 100;
        Arc(dc, cx - rm, mc - rm, cx + rm, mc + rm, cx + rm, mc, cx - rm, mc);
        break;
    }
    }

    SelectObject(dc, oldB);
    SelectObject(dc, oldP);
    DeleteObject(yellow);
    DeleteObject(pen);
}

static void DrawMine(HDC dc, RECT r)
{
    int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    int R = Sc(6);
    HPEN pen = CreatePen(PS_SOLID, std::max(2, Sc(2)), RGB(0, 0, 0));
    HGDIOBJ oldP = SelectObject(dc, pen);
    HGDIOBJ oldB = SelectObject(dc, GetStockObject(BLACK_BRUSH));
    int L = R + Sc(3);
    MoveToEx(dc, cx - L, cy, nullptr); LineTo(dc, cx + L + 1, cy);
    MoveToEx(dc, cx, cy - L, nullptr); LineTo(dc, cx, cy + L + 1);
    int D = L * 7 / 10;
    MoveToEx(dc, cx - D, cy - D, nullptr); LineTo(dc, cx + D + 1, cy + D + 1);
    MoveToEx(dc, cx - D, cy + D, nullptr); LineTo(dc, cx + D + 1, cy - D - 1);
    Ellipse(dc, cx - R, cy - R, cx + R + 1, cy + R + 1);
    Fill(dc, cx - Sc(3), cy - Sc(3), cx - Sc(3) + std::max(2, Sc(2)), cy - Sc(3) + std::max(2, Sc(2)), RGB(255, 255, 255));
    SelectObject(dc, oldP);
    SelectObject(dc, oldB);
    DeleteObject(pen);
}

static void DrawFlag(HDC dc, RECT r)
{
    int cx = (r.left + r.right) / 2, t = r.top;
    Fill(dc, cx - Sc(7), t + Sc(17), cx + Sc(7), t + Sc(19), RGB(0, 0, 0));
    Fill(dc, cx - Sc(4), t + Sc(14), cx + Sc(4), t + Sc(17), RGB(0, 0, 0));
    Fill(dc, cx - Sc(1), t + Sc(5),  cx + Sc(1), t + Sc(15), RGB(0, 0, 0));
    HBRUSH red = CreateSolidBrush(RGB(255, 0, 0));
    HGDIOBJ oldB = SelectObject(dc, red);
    HGDIOBJ oldP = SelectObject(dc, GetStockObject(NULL_PEN));
    POINT pts[3] = { { cx - Sc(1), t + Sc(5) }, { cx - Sc(1), t + Sc(12) }, { cx - Sc(9), t + Sc(8) } };
    Polygon(dc, pts, 3);
    SelectObject(dc, oldB);
    SelectObject(dc, oldP);
    DeleteObject(red);
}

static void DrawRevealedBg(HDC dc, RECT r, COLORREF bg)
{
    Fill(dc, r.left, r.top, r.right, r.bottom, bg);
    int th = std::max(1, Sc(1));
    Fill(dc, r.left, r.top, r.right, r.top + th, RGB(128, 128, 128));
    Fill(dc, r.left, r.top, r.left + th, r.bottom, RGB(128, 128, 128));
}

static bool IsPressed(int x, int y)
{
    if (g_pressX < 0 || !Active()) return false;
    if (g_chord) return std::abs(x - g_pressX) <= 1 && std::abs(y - g_pressY) <= 1;
    return x == g_pressX && y == g_pressY;
}

static void DrawCell(HDC dc, int x, int y)
{
    static const COLORREF numColors[9] = {
        0, RGB(0, 0, 255), RGB(0, 128, 0), RGB(255, 0, 0), RGB(0, 0, 128),
        RGB(128, 0, 0), RGB(0, 128, 128), RGB(0, 0, 0), RGB(128, 128, 128)
    };
    int idx = y * g_cols + x;
    const Cell& c = g_cells[idx];
    int l = GridX0() + x * g_cell, t = GridY0() + y * g_cell;
    RECT r = { l, t, l + g_cell, t + g_cell };
    bool lost = (g_state == S_LOST);

    if (c.revealed) {
        DrawRevealedBg(dc, r, RGB(192, 192, 192));
        if (c.adj > 0) {
            wchar_t ch[2] = { (wchar_t)(L'0' + c.adj), 0 };
            SetTextColor(dc, numColors[c.adj]);
            DrawTextW(dc, ch, 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
    } else if (lost && c.mine && c.mark != M_FLAG) {
        DrawRevealedBg(dc, r, idx == g_exploded ? RGB(255, 0, 0) : RGB(192, 192, 192));
        DrawMine(dc, r);
    } else if (lost && !c.mine && c.mark == M_FLAG) {
        DrawRevealedBg(dc, r, RGB(192, 192, 192));
        DrawMine(dc, r);
        HPEN pen = CreatePen(PS_SOLID, std::max(2, Sc(2)), RGB(255, 0, 0));
        HGDIOBJ oldP = SelectObject(dc, pen);
        int p = Sc(4);
        MoveToEx(dc, r.left + p, r.top + p, nullptr); LineTo(dc, r.right - p, r.bottom - p);
        MoveToEx(dc, r.left + p, r.bottom - p - 1, nullptr); LineTo(dc, r.right - p, r.top + p - 1);
        SelectObject(dc, oldP);
        DeleteObject(pen);
    } else if (IsPressed(x, y) && c.mark != M_FLAG) {
        DrawRevealedBg(dc, r, RGB(192, 192, 192));
    } else {
        Fill(dc, r.left, r.top, r.right, r.bottom, RGB(192, 192, 192));
        DrawBevel(dc, r, Sc(3), true);
        if (c.mark == M_FLAG) {
            DrawFlag(dc, r);
        } else if (c.mark == M_QUEST) {
            SetTextColor(dc, RGB(0, 0, 0));
            DrawTextW(dc, L"?", 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
    }
}

static void Paint(HDC dc, int W, int H)
{
    Fill(dc, 0, 0, W, H, RGB(192, 192, 192));
    RECT all = { 0, 0, ClientW(), ClientH() };
    DrawBevel(dc, all, Sc(3), true);

    RECT hr = HeaderRect();
    DrawBevel(dc, hr, Sc(2), false);

    DrawCounter(dc, CounterRect(true), g_mines - g_flags);
    DrawCounter(dc, CounterRect(false), g_time);

    Face face = F_NORMAL;
    if (g_state == S_WON) face = F_WIN;
    else if (g_state == S_LOST) face = F_LOSE;
    else if (g_pressX >= 0 && !g_smileyDown) face = F_SURPRISE;
    DrawSmiley(dc, SmileyRect(), face);

    DrawBevel(dc, GridOuterRect(), Sc(3), false);

    HGDIOBJ oldF = SelectObject(dc, g_fontNum);
    int oldBk = SetBkMode(dc, TRANSPARENT);
    for (int y = 0; y < g_rows; ++y)
        for (int x = 0; x < g_cols; ++x)
            DrawCell(dc, x, y);
    SetBkMode(dc, oldBk);
    SelectObject(dc, oldF);
}

// ----------------------------------------------------------------------------
//  Game logic
// ----------------------------------------------------------------------------
static void ResetGame()
{
    if (g_hwnd) KillTimer(g_hwnd, TIMER_ID);
    g_cells.assign((size_t)g_cols * g_rows, Cell());
    g_state = S_READY;
    g_time = 0; g_flags = 0; g_revealed = 0; g_exploded = -1;
    g_pressX = g_pressY = -1;
    Redraw();
}

static void PlaceMines(int sx, int sy)
{
    int total = g_cols * g_rows;
    bool wide = (total - 9) >= g_mines;
    std::vector<int> cand;
    cand.reserve(total);
    for (int y = 0; y < g_rows; ++y)
        for (int x = 0; x < g_cols; ++x) {
            bool excluded = wide ? (std::abs(x - sx) <= 1 && std::abs(y - sy) <= 1)
                                 : (x == sx && y == sy);
            if (!excluded) cand.push_back(y * g_cols + x);
        }
    std::shuffle(cand.begin(), cand.end(), g_rng);
    for (int i = 0; i < g_mines && i < (int)cand.size(); ++i)
        g_cells[cand[i]].mine = true;

    for (int y = 0; y < g_rows; ++y)
        for (int x = 0; x < g_cols; ++x) {
            int n = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    if (!dx && !dy) continue;
                    int nx = x + dx, ny = y + dy;
                    if (InBounds(nx, ny) && g_cells[ny * g_cols + nx].mine) ++n;
                }
            g_cells[y * g_cols + x].adj = (unsigned char)n;
        }
}

static void Win()
{
    g_state = S_WON;
    KillTimer(g_hwnd, TIMER_ID);
    for (auto& c : g_cells)
        if (c.mine) c.mark = M_FLAG;
    g_flags = g_mines;
    Redraw();
    UpdateWindow(g_hwnd);

    if (g_level < 3 && (DWORD)g_time < g_best[g_level]) {
        g_best[g_level] = (DWORD)g_time;
        SaveBest();
        static const wchar_t* names[3] = { L"Beginner", L"Intermediate", L"Expert" };
        wchar_t msg[160];
        swprintf_s(msg, L"You have the fastest time for the %s level: %d seconds!", names[g_level], g_time);
        MessageBoxW(g_hwnd, msg, APP_TITLE, MB_OK | MB_ICONINFORMATION);
    }
}

static void Lose(int idx)
{
    g_state = S_LOST;
    g_exploded = idx;
    KillTimer(g_hwnd, TIMER_ID);
    Redraw();
}

static void OpenCell(int x, int y)
{
    if (!InBounds(x, y)) return;
    Cell& c0 = g_cells[y * g_cols + x];
    if (c0.revealed || c0.mark == M_FLAG) return;

    if (g_state == S_READY) {
        PlaceMines(x, y);
        g_state = S_PLAYING;
        g_time = 0;
        SetTimer(g_hwnd, TIMER_ID, 1000, nullptr);
    }
    if (g_cells[y * g_cols + x].mine) {
        Lose(y * g_cols + x);
        return;
    }

    std::vector<int> stack;
    stack.push_back(y * g_cols + x);
    while (!stack.empty()) {
        int idx = stack.back(); stack.pop_back();
        Cell& c = g_cells[idx];
        if (c.revealed || c.mark == M_FLAG || c.mine) continue;
        c.revealed = true;
        c.mark = M_NONE;
        ++g_revealed;
        if (c.adj == 0) {
            int cx = idx % g_cols, cy = idx / g_cols;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    int nx = cx + dx, ny = cy + dy;
                    if (!InBounds(nx, ny)) continue;
                    const Cell& n = g_cells[ny * g_cols + nx];
                    if (!n.revealed && n.mark != M_FLAG) stack.push_back(ny * g_cols + nx);
                }
        }
    }
    Redraw();
    if (g_state == S_PLAYING && g_revealed == g_cols * g_rows - g_mines)
        Win();
}

static void ToggleMark(int x, int y)
{
    if (!InBounds(x, y)) return;
    Cell& c = g_cells[y * g_cols + x];
    if (c.revealed) return;
    switch (c.mark) {
    case M_NONE:  c.mark = M_FLAG; ++g_flags; break;
    case M_FLAG:  --g_flags; c.mark = g_marks ? M_QUEST : M_NONE; break;
    default:      c.mark = M_NONE; break;
    }
    Redraw();
}

static void Chord(int x, int y)
{
    if (!InBounds(x, y)) return;
    const Cell& c = g_cells[y * g_cols + x];
    if (!c.revealed || c.adj == 0) return;
    int flags = 0;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            int nx = x + dx, ny = y + dy;
            if (InBounds(nx, ny) && g_cells[ny * g_cols + nx].mark == M_FLAG) ++flags;
        }
    if (flags != c.adj) return;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            if (g_state != S_PLAYING) return;
            int nx = x + dx, ny = y + dy;
            if (InBounds(nx, ny)) OpenCell(nx, ny);
        }
}

// ----------------------------------------------------------------------------
//  Window sizing / menu state
// ----------------------------------------------------------------------------
static void ResizeWindow()
{
    DWORD style = (DWORD)GetWindowLongPtr(g_hwnd, GWL_STYLE);
    DWORD ex    = (DWORD)GetWindowLongPtr(g_hwnd, GWL_EXSTYLE);
    RECT rc = { 0, 0, ClientW(), ClientH() };
    AdjustWindowRectEx(&rc, style, TRUE, ex);
    SetWindowPos(g_hwnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    // keep the window on screen
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    RECT wr; GetWindowRect(g_hwnd, &wr);
    int nx = wr.left, ny = wr.top;
    if (wr.right  > mi.rcWork.right)  nx -= wr.right - mi.rcWork.right;
    if (wr.bottom > mi.rcWork.bottom) ny -= wr.bottom - mi.rcWork.bottom;
    nx = std::max(nx, (int)mi.rcWork.left);
    ny = std::max(ny, (int)mi.rcWork.top);
    if (nx != wr.left || ny != wr.top)
        SetWindowPos(g_hwnd, nullptr, nx, ny, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void UpdateMenuChecks()
{
    CheckMenuRadioItem(g_gameMenu, ID_BEGINNER, ID_CUSTOM, ID_BEGINNER + g_level, MF_BYCOMMAND);
    CheckMenuItem(g_gameMenu, ID_MARKS, MF_BYCOMMAND | (g_marks ? MF_CHECKED : MF_UNCHECKED));
}

static void ApplyLevel()
{
    switch (g_level) {
    case 0:  g_cols = 9;  g_rows = 9;  g_mines = 10; break;
    case 1:  g_cols = 16; g_rows = 16; g_mines = 40; break;
    case 2:  g_cols = 30; g_rows = 16; g_mines = 99; break;
    default: g_cols = g_custCols; g_rows = g_custRows; g_mines = g_custMines; break;
    }
    ResetGame();
    ResizeWindow();
    UpdateMenuChecks();
    SaveSettings();
}

// ----------------------------------------------------------------------------
//  Custom field dialog (built entirely in code - no .rc needed)
// ----------------------------------------------------------------------------
struct CustomDlg {
    int  w, h, m;
    bool ok, done;
};

static LRESULT CALLBACK CustomProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    CustomDlg* d = (CustomDlg*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
        return DefWindowProcW(hwnd, msg, wp, lp);

    case WM_CREATE: {
        HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        auto mk = [&](const wchar_t* cls, const wchar_t* txt, DWORD style, DWORD ex,
                      int x, int y, int w, int h, int id) {
            HWND c = CreateWindowExW(ex, cls, txt, WS_CHILD | WS_VISIBLE | style,
                                     Sc(x), Sc(y), Sc(w), Sc(h), hwnd, (HMENU)(INT_PTR)id, g_hInst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
            return c;
        };
        mk(L"STATIC", L"&Height (9-24):", 0, 0, 14, 19, 110, 18, -1);
        mk(L"STATIC", L"&Width (9-30):",  0, 0, 14, 49, 110, 18, -1);
        mk(L"STATIC", L"&Mines:",         0, 0, 14, 79, 110, 18, -1);
        DWORD es = WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL;
        mk(L"EDIT", L"", es, WS_EX_CLIENTEDGE, 130, 16, 66, 22, IDC_HEIGHT);
        mk(L"EDIT", L"", es, WS_EX_CLIENTEDGE, 130, 46, 66, 22, IDC_WIDTH);
        mk(L"EDIT", L"", es, WS_EX_CLIENTEDGE, 130, 76, 66, 22, IDC_MINES);
        mk(L"BUTTON", L"OK",     WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 26,  114, 76, 26, IDOK);
        mk(L"BUTTON", L"Cancel", WS_TABSTOP | BS_PUSHBUTTON,    0, 114, 114, 76, 26, IDCANCEL);
        SetDlgItemInt(hwnd, IDC_HEIGHT, d->h, FALSE);
        SetDlgItemInt(hwnd, IDC_WIDTH,  d->w, FALSE);
        SetDlgItemInt(hwnd, IDC_MINES,  d->m, FALSE);
        return 0;
    }
    case DM_GETDEFID:
        return MAKELRESULT(IDOK, DC_HASDEFID);

    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            int h = (int)GetDlgItemInt(hwnd, IDC_HEIGHT, nullptr, FALSE);
            int w = (int)GetDlgItemInt(hwnd, IDC_WIDTH,  nullptr, FALSE);
            int m = (int)GetDlgItemInt(hwnd, IDC_MINES,  nullptr, FALSE);
            ClampCustom(w, h, m);
            d->w = w; d->h = h; d->m = m; d->ok = true;
            DestroyWindow(hwnd);
        } else if (LOWORD(wp) == IDCANCEL) {
            DestroyWindow(hwnd);
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (d) d->done = true;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static bool ShowCustomDialog(HWND owner, int& w, int& h, int& m)
{
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc   = CustomProc;
        wc.hInstance     = g_hInst;
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = DLG_CLASS;
        RegisterClassExW(&wc);
        registered = true;
    }

    CustomDlg d = { w, h, m, false, false };
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    DWORD ex = WS_EX_DLGMODALFRAME;
    RECT rc = { 0, 0, Sc(220), Sc(160) };
    AdjustWindowRectEx(&rc, style, FALSE, ex);
    int dw = rc.right - rc.left, dh = rc.bottom - rc.top;

    RECT or_; GetWindowRect(owner, &or_);
    int x = or_.left + ((or_.right - or_.left) - dw) / 2;
    int y = or_.top + ((or_.bottom - or_.top) - dh) / 2;

    EnableWindow(owner, FALSE);
    HWND dlg = CreateWindowExW(ex, DLG_CLASS, L"Custom Field", style, x, y, dw, dh,
                               owner, nullptr, g_hInst, &d);
    if (!dlg) { EnableWindow(owner, TRUE); return false; }
    ShowWindow(dlg, SW_SHOW);
    HWND first = GetDlgItem(dlg, IDC_HEIGHT);
    SetFocus(first);
    SendMessageW(first, EM_SETSEL, 0, -1);

    MSG msg;
    while (!d.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (!d.done) {                // WM_QUIT arrived while the dialog was open
        DestroyWindow(dlg);
        PostQuitMessage(0);
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);

    if (d.ok) { w = d.w; h = d.h; m = d.m; }
    return d.ok;
}

// ----------------------------------------------------------------------------
//  Mouse handling
// ----------------------------------------------------------------------------
static void UpdatePress(int x, int y)
{
    if (g_smileyDown) {
        POINT p = { x, y };
        RECT sr = SmileyRect();
        bool hot = PtInRect(&sr, p) != 0;
        if (hot != g_smileyHot) { g_smileyHot = hot; Redraw(); }
        return;
    }
    int nx = -1, ny = -1;
    if ((g_lDown || g_mDown || g_chord) && !g_chordFired && Active()) {
        int cx, cy;
        if (CellFromPoint(x, y, cx, cy)) { nx = cx; ny = cy; }
    }
    if (nx != g_pressX || ny != g_pressY) { g_pressX = nx; g_pressY = ny; Redraw(); }
}

static void OnButtonDown(HWND h, int which, int x, int y)
{
    if (which == 0) g_lDown = true;
    else if (which == 1) g_rDown = true;
    else g_mDown = true;
    SetCapture(h);

    POINT p = { x, y };
    RECT sr = SmileyRect();
    if (which == 0 && !g_rDown && PtInRect(&sr, p)) {
        g_smileyDown = true; g_smileyHot = true;
        Redraw();
        return;
    }
    if (g_smileyDown) return;

    if (g_mDown || (g_lDown && g_rDown)) g_chord = true;
    UpdatePress(x, y);
}

static void OnButtonUp(int which, int x, int y)
{
    bool* flag = (which == 0) ? &g_lDown : (which == 1) ? &g_rDown : &g_mDown;
    if (!*flag) return;
    *flag = false;

    POINT p = { x, y };
    if (g_smileyDown) {
        if (which == 0) {
            g_smileyDown = false;
            RECT sr = SmileyRect();
            if (PtInRect(&sr, p)) ResetGame();
        }
    } else {
        int cx, cy;
        bool hit = CellFromPoint(x, y, cx, cy);
        if (g_chord) {
            if (!g_chordFired) {
                g_chordFired = true;
                g_pressX = g_pressY = -1;
                if (hit && Active()) Chord(cx, cy);
            }
        } else if (hit && Active()) {
            g_pressX = g_pressY = -1;
            if (which == 0) OpenCell(cx, cy);
            else if (which == 1) ToggleMark(cx, cy);
        }
    }

    if (!g_lDown && !g_rDown && !g_mDown) {
        g_chord = g_chordFired = g_smileyDown = g_smileyHot = false;
        g_pressX = g_pressY = -1;
        if (GetCapture() == g_hwnd) ReleaseCapture();
    }
    Redraw();
}

// ----------------------------------------------------------------------------
//  Main window procedure
// ----------------------------------------------------------------------------
static void ShowBestTimes()
{
    wchar_t buf[256], a[32], b[32], c[32];
    auto fmt = [](wchar_t* out, size_t n, DWORD t) {
        if (t >= 999) wcscpy_s(out, n, L"--");
        else swprintf_s(out, n, L"%lu seconds", (unsigned long)t);
    };
    fmt(a, 32, g_best[0]); fmt(b, 32, g_best[1]); fmt(c, 32, g_best[2]);
    swprintf_s(buf, L"Fastest times\n\nBeginner:\t\t%s\nIntermediate:\t%s\nExpert:\t\t%s", a, b, c);
    MessageBoxW(g_hwnd, buf, L"Best Times", MB_OK | MB_ICONINFORMATION);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_NEW:          ResetGame(); break;
        case ID_BEGINNER:     g_level = 0; ApplyLevel(); break;
        case ID_INTERMEDIATE: g_level = 1; ApplyLevel(); break;
        case ID_EXPERT:       g_level = 2; ApplyLevel(); break;
        case ID_CUSTOM: {
            int w = g_custCols, h = g_custRows, m = g_custMines;
            if (ShowCustomDialog(hwnd, w, h, m)) {
                g_custCols = w; g_custRows = h; g_custMines = m;
                g_level = 3;
                ApplyLevel();
            }
            break;
        }
        case ID_MARKS:
            g_marks = !g_marks;
            if (!g_marks)
                for (auto& c : g_cells) if (c.mark == M_QUEST) c.mark = M_NONE;
            UpdateMenuChecks(); SaveSettings(); Redraw();
            break;
        case ID_BESTTIMES: ShowBestTimes(); break;
        case ID_EXIT:      DestroyWindow(hwnd); break;
        case ID_ABOUT:
            MessageBoxW(hwnd,
                L"Minesweeper\nA Win32 / GDI implementation.\n\n"
                L"Left click: reveal\nRight click: flag / ? / clear\n"
                L"Middle click or left+right: chord\nF2: new game",
                L"About Minesweeper", MB_OK | MB_ICONINFORMATION);
            break;
        }
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_F2) ResetGame();
        return 0;

    case WM_LBUTTONDOWN: OnButtonDown(hwnd, 0, GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
    case WM_RBUTTONDOWN: OnButtonDown(hwnd, 1, GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
    case WM_MBUTTONDOWN: OnButtonDown(hwnd, 2, GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
    case WM_LBUTTONUP:   OnButtonUp(0, GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
    case WM_RBUTTONUP:   OnButtonUp(1, GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
    case WM_MBUTTONUP:   OnButtonUp(2, GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;

    case WM_MOUSEMOVE:
        if (g_lDown || g_rDown || g_mDown) UpdatePress(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;

    case WM_CAPTURECHANGED:
    case WM_CANCELMODE:
        g_lDown = g_rDown = g_mDown = false;
        g_chord = g_chordFired = g_smileyDown = g_smileyHot = false;
        g_pressX = g_pressY = -1;
        Redraw();
        return 0;

    case WM_TIMER:
        if (wp == TIMER_ID && g_state == S_PLAYING && g_time < 999) {
            ++g_time;
            RECT cr = CounterRect(false);
            InvalidateRect(hwnd, &cr, FALSE);
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        int W = rc.right, H = rc.bottom;
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, W, H);
        HGDIOBJ old = SelectObject(mem, bmp);
        Paint(mem, W, H);
        BitBlt(hdc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_ID);
        if (g_fontNum) { DeleteObject(g_fontNum); g_fontNum = nullptr; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ----------------------------------------------------------------------------
//  Entry point
// ----------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow)
{
    g_hInst = hInst;
    SetProcessDPIAware();
    InitCommonControls();
    InitMetrics();

    std::random_device rd;
    g_rng.seed(rd() ^ (unsigned)GetTickCount64());
    LoadSettings();

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hIcon         = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm       = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = MAIN_CLASS;
    if (!RegisterClassExW(&wc)) return 1;

    // Menu bar
    HMENU bar = CreateMenu();
    g_gameMenu = CreatePopupMenu();
    AppendMenuW(g_gameMenu, MF_STRING, ID_NEW, L"&New\tF2");
    AppendMenuW(g_gameMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(g_gameMenu, MF_STRING, ID_BEGINNER, L"&Beginner");
    AppendMenuW(g_gameMenu, MF_STRING, ID_INTERMEDIATE, L"&Intermediate");
    AppendMenuW(g_gameMenu, MF_STRING, ID_EXPERT, L"&Expert");
    AppendMenuW(g_gameMenu, MF_STRING, ID_CUSTOM, L"&Custom...");
    AppendMenuW(g_gameMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(g_gameMenu, MF_STRING, ID_MARKS, L"&Marks (?)");
    AppendMenuW(g_gameMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(g_gameMenu, MF_STRING, ID_BESTTIMES, L"Best &Times...");
    AppendMenuW(g_gameMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(g_gameMenu, MF_STRING, ID_EXIT, L"E&xit");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)g_gameMenu, L"&Game");

    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, ID_ABOUT, L"&About Minesweeper...");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)help, L"&Help");

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    g_hwnd = CreateWindowExW(0, MAIN_CLASS, APP_TITLE, style,
                             CW_USEDEFAULT, CW_USEDEFAULT, 300, 400,
                             nullptr, bar, hInst, nullptr);
    if (!g_hwnd) return 1;

    ApplyLevel();          // sets board size, resizes the window, checks menu items
    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
