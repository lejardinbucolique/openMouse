// openMouse — per-seat pointer rendering
// SPDX-License-Identifier: MIT
//
// Windows has exactly one hardware cursor. Every seat that does not currently
// own it gets its pointer drawn here instead: a layered, click-through,
// topmost window holding a 32bpp premultiplied-alpha bitmap of an arrow in the
// seat's colour.
#include "openmouse.h"
#include <cwchar>
#include <algorithm>
#include <cmath>

namespace om {
namespace {

// Classic arrow outline. The tip sits at (1,1) so the hotspot offset is (1,1).
constexpr POINT kArrow[] = {
    {1, 1}, {1, 27}, {7, 20}, {12, 31}, {16, 29}, {11, 19}, {19, 19}
};
constexpr int kArrowPts = (int)(sizeof(kArrow) / sizeof(kArrow[0]));
constexpr int kBmpW = 22;
constexpr int kBmpH = 34;

// Even-odd point-in-polygon. Exact and cheap at this size; avoids the GDI
// round trip and the ambiguity of distinguishing "drawn black" from
// "untouched transparent" in an ARGB buffer.
bool InsideArrow(double px, double py) {
    bool in = false;
    for (int i = 0, j = kArrowPts - 1; i < kArrowPts; j = i++) {
        const double xi = kArrow[i].x, yi = kArrow[i].y;
        const double xj = kArrow[j].x, yj = kArrow[j].y;
        if (((yi > py) != (yj > py)) &&
            (px < (xj - xi) * (py - yi) / (yj - yi) + xi))
            in = !in;
    }
    return in;
}

const wchar_t* kOverlayClass = L"openMouse.CursorOverlay";

void EnsureClass(HINSTANCE inst) {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance     = inst;
    wc.lpszClassName = kOverlayClass;
    wc.hCursor       = nullptr;
    RegisterClassExW(&wc);
    done = true;
}

} // namespace

// ---------------------------------------------------------------- cursor looks

namespace {

const wchar_t* const kStyleKeys[kStyleCount] = { L"classic", L"bright", L"inverse", L"big", L"glow" };

// What a set adds to the system pointer, which stays the model of every shape.
struct StyleSpec {
    bool     whiteBody;   // body white and edge in the mouse colour, instead of the reverse
    double   scale;
    int      rim;         // hard outer rim, in pixels at scale 1 (0: none)
    COLORREF rimColor;
    int      glow;        // soft glow in the mouse colour, in pixels (0: none)
};

StyleSpec SpecOf(int style) {
    switch (style) {
    case kStyleBright:  return { false, 1.0, 2, RGB(255, 255, 255), 0 };
    case kStyleInverse: return { true,  1.0, 2, RGB(20, 20, 20),    0 };
    case kStyleBig:     return { false, 1.5, 2, RGB(255, 255, 255), 0 };
    case kStyleGlow:    return { false, 1.0, 0, 0,                  5 };
    default:            return { false, 1.0, 0, 0,                  0 };
    }
}

// The model of a shape as two planes: lum is 255 on the body and 0 on the edge,
// alpha is the coverage. Straight (not premultiplied) like the system cursor.
struct Planes {
    int w = 0, h = 0, hotX = 0, hotY = 0;
    std::vector<BYTE> lum, alpha;
};

// A pointer drawn with a mask only (the text bar is one): one pixel of outline
// is added so that a thin line still has an edge to set it off, and a body to colour.
void AddOutline(Planes& p) {
    const int nw = p.w + 2, nh = p.h + 2;
    std::vector<BYTE> lum((size_t)nw * nh, 0), alpha((size_t)nw * nh, 0);
    for (int y = 0; y < nh; ++y)
        for (int x = 0; x < nw; ++x) {
            const int sx = x - 1, sy = y - 1;
            if (sx >= 0 && sy >= 0 && sx < p.w && sy < p.h && p.alpha[(size_t)sy * p.w + sx]) {
                lum[(size_t)y * nw + x]   = p.lum[(size_t)sy * p.w + sx];
                alpha[(size_t)y * nw + x] = 255;
                continue;
            }
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = sx + dx, ny = sy + dy;
                    if (nx >= 0 && ny >= 0 && nx < p.w && ny < p.h && p.alpha[(size_t)ny * p.w + nx])
                        alpha[(size_t)y * nw + x] = 255;
                }
        }
    p.w = nw; p.h = nh; p.hotX += 1; p.hotY += 1;
    p.lum.swap(lum); p.alpha.swap(alpha);
}

// Reads the real Windows pointer, so each shape has the native silhouette, the
// native anti-aliasing and the native hotspot instead of a hand-rasterised
// approximation. GetDIBits is used rather than DrawIconEx because GDI does not
// reliably carry the alpha channel through a blit, and the alpha is the whole
// point here. A pointer without alpha (colour with an AND mask, or monochrome
// like the text bar) is rebuilt from its masks.
bool PlanesFromSystemCursor(LPCWSTR id, Planes& out) {
    HCURSOR hc = (HCURSOR)LoadImageW(nullptr, id, IMAGE_CURSOR, 0, 0,
                                     LR_SHARED | LR_DEFAULTSIZE);
    if (!hc) return false;

    ICONINFO ii{};
    if (!GetIconInfo(hc, &ii)) return false;

    // Ownership: GetIconInfo hands us copies of the bitmaps, ours to delete.
    struct BmpGuard {
        HBITMAP a, b;
        ~BmpGuard() { if (a) DeleteObject(a); if (b) DeleteObject(b); }
    } guard{ ii.hbmMask, ii.hbmColor };

    // Monochrome: no colour bitmap, and the mask holds the AND plane over the XOR plane.
    const bool mono = (ii.hbmColor == nullptr);
    BITMAP bm{};
    if (!GetObjectW(mono ? ii.hbmMask : ii.hbmColor, sizeof(bm), &bm)) return false;
    const int w = bm.bmWidth, h = mono ? bm.bmHeight / 2 : bm.bmHeight;
    if (w <= 0 || h <= 0 || w > 256 || h > 256) return false;

    auto read = [&](HBITMAP hb, int rows, std::vector<BYTE>& px) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth       = w;
        bi.bmiHeader.biHeight      = -rows;  // top-down
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        px.assign((size_t)w * rows * 4, 0);
        HDC screen = GetDC(nullptr);
        const int got = GetDIBits(screen, hb, 0, rows, px.data(), &bi, DIB_RGB_COLORS);
        ReleaseDC(nullptr, screen);
        return got == rows;
    };

    std::vector<BYTE> src;
    bool anyAlpha = false;
    if (!mono) {
        if (!read(ii.hbmColor, h, src)) return false;
        for (size_t i = 3; i < src.size(); i += 4)
            if (src[i]) { anyAlpha = true; break; }
    }

    out.w = w; out.h = h;
    out.hotX = (int)ii.xHotspot;
    out.hotY = (int)ii.yHotspot;
    out.lum.assign((size_t)w * h, 0);
    out.alpha.assign((size_t)w * h, 0);

    if (anyAlpha) {
        for (size_t i = 0; i < (size_t)w * h; ++i) {
            const BYTE sb = src[i * 4], sg = src[i * 4 + 1], sr = src[i * 4 + 2];
            // Rec. 601 luminance: 1.0 on the arrow's white body, 0.0 on its outline.
            out.lum[i]   = (BYTE)((sr * 77 + sg * 150 + sb * 29) >> 8);
            out.alpha[i] = src[i * 4 + 3];
        }
        return true;
    }

    std::vector<BYTE> mask;
    if (!read(ii.hbmMask, mono ? 2 * h : h, mask)) return false;
    bool any = false;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = (size_t)y * w + x;
            const bool andBit = mask[i * 4] > 127;
            if (mono) {
                const bool xorBit = mask[((size_t)(y + h) * w + x) * 4] > 127;
                if (andBit && !xorBit) continue;               // transparent
                out.lum[i] = xorBit ? 255 : 0;                 // white or inverting: body; black: edge
            } else {
                if (andBit) continue;
                const BYTE sb = src[i * 4], sg = src[i * 4 + 1], sr = src[i * 4 + 2];
                out.lum[i] = (BYTE)((sr * 77 + sg * 150 + sb * 29) >> 8);
            }
            out.alpha[i] = 255;
            any = true;
        }
    if (!any) return false;
    if (mono) AddOutline(out);
    return true;
}

// The hand-drawn arrow, for a machine where the system cursor cannot be read
// back with usable alpha.
void PlanesFromPolygon(Planes& out) {
    out.w = kBmpW; out.h = kBmpH; out.hotX = 1; out.hotY = 1;
    out.lum.assign((size_t)out.w * out.h, 0);
    out.alpha.assign((size_t)out.w * out.h, 0);
    std::vector<bool> inside((size_t)out.w * out.h, false);
    for (int y = 0; y < out.h; ++y)
        for (int x = 0; x < out.w; ++x)
            inside[(size_t)y * out.w + x] = InsideArrow(x + 0.5, y + 0.5);
    for (int y = 0; y < out.h; ++y)
        for (int x = 0; x < out.w; ++x) {
            if (!inside[(size_t)y * out.w + x]) continue;
            bool edge = false;
            for (int dy = -1; dy <= 1 && !edge; ++dy)
                for (int dx = -1; dx <= 1 && !edge; ++dx) {
                    const int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= out.w || ny >= out.h ||
                        !inside[(size_t)ny * out.w + nx])
                        edge = true;
                }
            out.alpha[(size_t)y * out.w + x] = 255;
            out.lum[(size_t)y * out.w + x]   = edge ? 0 : 255;
        }
}

LPCWSTR SystemCursorId(PointerShape shape) {
    switch (shape) {
    case PointerShape::SizeWE:      return IDC_SIZEWE;
    case PointerShape::SizeNS:      return IDC_SIZENS;
    case PointerShape::SizeNWSE:    return IDC_SIZENWSE;
    case PointerShape::SizeNESW:    return IDC_SIZENESW;
    case PointerShape::Hand:        return IDC_HAND;
    case PointerShape::IBeam:       return IDC_IBEAM;
    case PointerShape::No:          return IDC_NO;
    case PointerShape::Wait:        return IDC_WAIT;
    case PointerShape::AppStarting: return IDC_APPSTARTING;
    case PointerShape::Cross:       return IDC_CROSS;
    case PointerShape::SizeAll:     return IDC_SIZEALL;
    default:                        return IDC_ARROW;
    }
}

// Bilinear enlargement of one plane. The hotspot is scaled by the caller.
std::vector<BYTE> Enlarge(const std::vector<BYTE>& p, int w, int h, int nw, int nh) {
    std::vector<BYTE> o((size_t)nw * nh);
    for (int y = 0; y < nh; ++y) {
        const double fy = (y + 0.5) * h / nh - 0.5;
        int y0 = (int)std::floor(fy);
        const double ty = fy - y0;
        const int y1 = std::min(std::max(y0 + 1, 0), h - 1);
        y0 = std::min(std::max(y0, 0), h - 1);
        for (int x = 0; x < nw; ++x) {
            const double fx = (x + 0.5) * w / nw - 0.5;
            int x0 = (int)std::floor(fx);
            const double tx = fx - x0;
            const int x1 = std::min(std::max(x0 + 1, 0), w - 1);
            x0 = std::min(std::max(x0, 0), w - 1);
            const double top = p[(size_t)y0 * w + x0] * (1 - tx) + p[(size_t)y0 * w + x1] * tx;
            const double bot = p[(size_t)y1 * w + x0] * (1 - tx) + p[(size_t)y1 * w + x1] * tx;
            o[(size_t)y * nw + x] = (BYTE)std::lround(top * (1 - ty) + bot * ty);
        }
    }
    return o;
}

} // namespace

const wchar_t* CursorStyleKey(int style) {
    return kStyleKeys[style < 0 || style >= kStyleCount ? kStyleBright : style];
}

int CursorStyleFromKey(const std::wstring& key, int fallback) {
    for (int i = 0; i < kStyleCount; ++i)
        if (_wcsicmp(key.c_str(), kStyleKeys[i]) == 0) return i;
    return fallback;
}

// Draws one shape of a set. The model is the system pointer (lum + alpha); the
// body is the mouse colour, the edge black, then the set adds its rim or glow
// outside the silhouette so the pointer shows on any background.
bool RenderPointer(PointerShape shape, COLORREF color, int style, PointerImage& out) {
    Planes m;
    if (!PlanesFromSystemCursor(SystemCursorId(shape), m)) {
        if (shape != PointerShape::Arrow) return false;
        PlanesFromPolygon(m);
    }
    const StyleSpec sp = SpecOf(style);

    if (sp.scale != 1.0) {
        const int nw = (int)std::lround(m.w * sp.scale), nh = (int)std::lround(m.h * sp.scale);
        m.lum   = Enlarge(m.lum,   m.w, m.h, nw, nh);
        m.alpha = Enlarge(m.alpha, m.w, m.h, nw, nh);
        m.hotX = (int)std::lround(m.hotX * sp.scale);
        m.hotY = (int)std::lround(m.hotY * sp.scale);
        m.w = nw; m.h = nh;
    }

    const int rim  = sp.rim ? (int)std::lround(sp.rim * sp.scale) : 0;
    const int halo = std::max(rim, sp.glow);       // how far outside the silhouette we draw
    const int pad  = halo ? halo + 1 : 0;
    const int W = m.w + 2 * pad, H = m.h + 2 * pad;

    // Black as a colour inverts the roles (black body, white edge), as the
    // Windows "black" scheme does; white is the stock arrow.
    const COLORREF bodyC = sp.whiteBody ? RGB(255, 255, 255) : color;
    COLORREF edgeC = sp.whiteBody ? color : RGB(0, 0, 0);
    if (!sp.whiteBody && color == RGB(0, 0, 0)) edgeC = RGB(255, 255, 255);
    const COLORREF haloC = sp.glow ? color : sp.rimColor;

    out.w = W; out.h = H;
    out.hotX = m.hotX + pad;
    out.hotY = m.hotY + pad;
    out.bgra.assign((size_t)W * H * 4, 0);

    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const int sx = x - pad, sy = y - pad;
            int sa = 0, lum = 0;
            if (sx >= 0 && sy >= 0 && sx < m.w && sy < m.h) {
                sa  = m.alpha[(size_t)sy * m.w + sx];
                lum = m.lum[(size_t)sy * m.w + sx];
            }

            // Outer rim or glow: the strongest neighbouring coverage, weighted by distance.
            double ha = 0;
            if (halo && sa < 255) {
                for (int dy = -halo - 1; dy <= halo + 1; ++dy) {
                    const int ny = sy + dy;
                    if (ny < 0 || ny >= m.h) continue;
                    for (int dx = -halo - 1; dx <= halo + 1; ++dx) {
                        const int nx = sx + dx;
                        if (nx < 0 || nx >= m.w) continue;
                        const int na = m.alpha[(size_t)ny * m.w + nx];
                        if (!na) continue;
                        const double d = std::sqrt((double)(dx * dx + dy * dy));
                        double f;
                        if (sp.glow) { f = 1.0 - d / (sp.glow + 1); f = f > 0 ? f * f * 0.9 : 0; }
                        else         { f = rim + 0.5 - d; f = f < 0 ? 0 : (f > 1 ? 1 : f); }
                        ha = std::max(ha, f * na / 255.0);
                    }
                }
            }

            BYTE* p = out.bgra.data() + ((size_t)y * W + x) * 4;
            // Shape colour: edge to body by luminance; premultiplied by coverage.
            const int r = (GetRValue(edgeC) * (255 - lum) + GetRValue(bodyC) * lum) / 255;
            const int g = (GetGValue(edgeC) * (255 - lum) + GetGValue(bodyC) * lum) / 255;
            const int b = (GetBValue(edgeC) * (255 - lum) + GetBValue(bodyC) * lum) / 255;
            const double under = ha * (255 - sa) / 255.0;    // halo showing through the shape's transparency
            p[0] = (BYTE)std::lround(b * sa / 255.0 + GetBValue(haloC) * under);
            p[1] = (BYTE)std::lround(g * sa / 255.0 + GetGValue(haloC) * under);
            p[2] = (BYTE)std::lround(r * sa / 255.0 + GetRValue(haloC) * under);
            p[3] = (BYTE)std::lround(sa + 255.0 * under);
        }
    }
    return true;
}

bool CursorOverlay::Create(HINSTANCE inst, int seatIndex, COLORREF color, int style) {
    EnsureClass(inst);
    w_ = kBmpW;
    h_ = kBmpH;

    // Title carries the seat index so overlays are identifiable in Spy++ or
    // window-enumeration tools during debugging.
    wchar_t title[32];
    swprintf(title, 32, L"openMouse seat %d", seatIndex);

    hwnd_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        kOverlayClass, title, WS_POPUP,
        0, 0, w_, h_,
        nullptr, nullptr, inst, nullptr);
    if (!hwnd_) return false;

    color_ = color;
    style_ = style;
    Present(PointerShape::Arrow);
    return true;
}

void CursorOverlay::SetLook(COLORREF color, int style) {
    if (color == color_ && style == style_) return;
    color_ = color;
    style_ = style;
    if (!hwnd_) return;
    // Every shape is drawn again, on demand: forget the ones in the cache.
    bitmap_ = nullptr;
    for (ShapeBmp& sb : shapes_) { if (sb.bmp) DeleteObject(sb.bmp); sb = ShapeBmp(); }
    const PointerShape was = shape_;
    shape_ = PointerShape::Count;       // so that the redraw is not skipped
    Present(was);
}

// Builds (once) and shows the bitmap of a shape. Every shape comes from the
// real system cursor, so the resize arrows have the native look and hotspot —
// the size cursors' hotspot is their centre, not a corner, which is why the
// position is always placed through hotX_/hotY_ of the shape on display.
void CursorOverlay::Present(PointerShape shape) {
    ShapeBmp* sb = &shapes_[(int)shape];
    if (!sb->bmp) {
        PointerImage img;
        if (!RenderPointer(shape, color_, style_, img) || img.w <= 0 || img.h <= 0 ||
            img.bgra.size() != (size_t)img.w * img.h * 4) {
            // No usable cursor: stay on the arrow rather than show nothing.
            if (shape != PointerShape::Arrow) Present(PointerShape::Arrow);
            return;
        }

        BITMAPINFO bi{};
        bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth       = img.w;
        bi.bmiHeader.biHeight      = -img.h;       // top-down
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HDC screen = GetDC(nullptr);
        HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        ReleaseDC(nullptr, screen);
        if (!bmp || !bits) {
            if (bmp) DeleteObject(bmp);
            if (shape != PointerShape::Arrow) Present(PointerShape::Arrow);
            return;
        }
        memcpy(bits, img.bgra.data(), img.bgra.size());
        sb->bmp = bmp; sb->w = img.w; sb->h = img.h; sb->hotX = img.hotX; sb->hotY = img.hotY;
    }

    w_ = sb->w; h_ = sb->h; hotX_ = sb->hotX; hotY_ = sb->hotY;
    bitmap_ = sb->bmp;
    shape_  = shape;

    // The window was created at the fallback size; the system cursor may be
    // larger, so resize before the layered update or the arrow is clipped.
    SetWindowPos(hwnd_, nullptr, 0, 0, w_, h_, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HGDIOBJ old = SelectObject(mem, bitmap_);

    POINT  dst{at_.x - hotX_, at_.y - hotY_};
    POINT  src{0, 0};
    SIZE   size{w_, h_};
    BLENDFUNCTION bf{};
    bf.BlendOp             = AC_SRC_OVER;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat         = AC_SRC_ALPHA;

    UpdateLayeredWindow(hwnd_, screen, &dst, &size, mem, &src, 0, &bf, ULW_ALPHA);

    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

void CursorOverlay::SetShape(PointerShape shape) {
    if (!hwnd_ || shape == shape_) return;
    Present(shape);
}

void CursorOverlay::MoveTo(POINT ptVirtual) {
    if (!hwnd_) return;
    at_ = ptVirtual;
    // Offset by the real hotspot so the tip lands on the logical position.
    // UpdateLayeredWindow with only a destination point moves the layered
    // window without touching its content or z-order, which is far cheaper
    // than SetWindowPos(HWND_TOPMOST) on every move (26 to 54 ms measured).
    POINT dst{ptVirtual.x - hotX_, ptVirtual.y - hotY_};
    UpdateLayeredWindow(hwnd_, nullptr, &dst, nullptr, nullptr, nullptr, 0, nullptr, 0);
    // The z-order is reasserted from the timer (ReassertTopmost), not here:
    // even once a second, SetWindowPos on the move path showed as 50 to
    // 100 ms hitches.
}

bool CursorOverlay::ReassertTopmost(wchar_t* who, int whoLen) {
    if (!hwnd_ || !visible_) return false;
    // Walking up the z-order is cheap; SetWindowPos is not. Only pay for it
    // when a visible topmost window of another process sits above the arrow.
    // The other arrows must not count: they are all topmost, so each reassert
    // put the other one underneath and they swapped every second, 50 to
    // 100 ms each. Our other windows do count, though: the tray icon's menu
    // belongs to this process and must have the arrows drawn over it.
    HWND cover = nullptr;
    for (HWND w = GetWindow(hwnd_, GW_HWNDPREV); w; w = GetWindow(w, GW_HWNDPREV)) {
        if (!IsWindowVisible(w)) continue;
        if (!(GetWindowLongW(w, GWL_EXSTYLE) & WS_EX_TOPMOST)) continue;
        wchar_t cls[64] = {};
        GetClassNameW(w, cls, 63);
        if (!wcscmp(cls, kOverlayClass)) continue;
        // The shell's band sits above every topmost window: nothing reasserted under it can show.
        if (IsShellWindowClass(cls)) continue;
        cover = w;
        break;
    }
    if (!cover) return false;
    if (who && whoLen > 0) {
        who[0] = 0;
        GetClassNameW(cover, who, whoLen);
    }
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW);
    return true;
}

void CursorOverlay::Show(bool visible) {
    if (!hwnd_ || visible == visible_) return;
    ShowWindow(hwnd_, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
    visible_ = visible;
}

void CursorOverlay::Destroy() {
    bitmap_ = nullptr;
    for (ShapeBmp& sb : shapes_) { if (sb.bmp) DeleteObject(sb.bmp); sb = ShapeBmp(); }
    shape_ = PointerShape::Arrow;
    if (hwnd_)   { DestroyWindow(hwnd_);  hwnd_ = nullptr; }
    visible_ = false;
}

// ---------------------------------------------------------------- utilities

RECT VirtualScreenRect() {
    RECT r;
    r.left   = GetSystemMetrics(SM_XVIRTUALSCREEN);
    r.top    = GetSystemMetrics(SM_YVIRTUALSCREEN);
    r.right  = r.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    r.bottom = r.top  + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return r;
}

void ClampToVirtualScreen(POINT& p) {
    ClampToRect(p, VirtualScreenRect());
}

void ClampToRect(POINT& p, const RECT& r) {
    if (p.x < r.left)       p.x = r.left;
    if (p.y < r.top)        p.y = r.top;
    if (p.x > r.right  - 1) p.x = r.right  - 1;
    if (p.y > r.bottom - 1) p.y = r.bottom - 1;
}

namespace {
BOOL CALLBACK AddScreen(HMONITOR mon, HDC, LPRECT, LPARAM out) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi))
        ((std::vector<ScreenInfo>*)out)->push_back(
            { mi.szDevice, mi.rcMonitor, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0 });
    return TRUE;
}
} // namespace

std::vector<ScreenInfo> ListScreens() {
    std::vector<ScreenInfo> v;
    EnumDisplayMonitors(nullptr, nullptr, AddScreen, (LPARAM)&v);
    std::sort(v.begin(), v.end(), [](const ScreenInfo& a, const ScreenInfo& b) {
        return a.rc.left != b.rc.left ? a.rc.left < b.rc.left : a.rc.top < b.rc.top; });
    return v;
}

bool IsShellWindowClass(const wchar_t* cls) {
    return !wcscmp(cls, L"Shell_TrayWnd") ||
           !wcscmp(cls, L"Shell_SecondaryTrayWnd") ||
           !wcscmp(cls, L"NotifyIconOverflowWindow") ||
           !wcscmp(cls, L"Windows.UI.Core.CoreWindow") ||          // Start, search
           !wcscmp(cls, L"XamlExplorerHostIslandWindow") ||        // Win11 taskbar
           !wcscmp(cls, L"TopLevelWindowForOverflowXamlIsland") ||
           !wcscmp(cls, L"Xaml_WindowedPopupClass");
}

} // namespace om
