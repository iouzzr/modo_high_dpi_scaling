// Copyright 2026 iouzzr
// SPDX-License-Identifier: Apache-2.0

// high_dpi_scaling.cpp - High-DPI 3D Viewport & 2D UI (OPALDisp) Patch DLL for Foundry Modo (16.x & 17.x)
// Hooks QOpenGLFramebufferObject, AGLViewport / AGLScissor, and QImage / QPaintDevice / QPainter / QFont
// across Modo 16 (front3000.dll + frame3000.dll + GnzUI.dll + Qt5Gui.dll) and Modo 17 (modo.exe + Qt6Gui.dll + Qt6OpenGL.dll)
// to render both 3D viewports and 2D UI panels/text at native High-DPI physical resolution.
// Reference RVAs below correspond to Modo 16.1v9 / 17.1v1; inline patches fall back to scanning .text via FindCodePattern.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <intrin.h>

#pragma intrinsic(_ReturnAddress)

// Headerless POD ABI mirrors for Qt value types:
// - QSizeC (2 ints) matches QSize and QPoint (reused in FnDrawPoints).
// - QRectC (4 ints) matches QRect (closed interval: x2 = x + w - 1, y2 = y + h - 1)
//   and QLine (x1, y1, x2, y2, reused in FnDrawLines).
struct QSizeC {
    int w;
    int h;
};

struct QRectC {
    int x1;
    int y1;
    int x2;
    int y2;
};

// 3D OpenGL Viewport function pointer types
typedef void* (*FnFboCtor)(void* this_ptr, int w, int h, const void* fmt_ptr);
typedef void  (*FnFboDtor)(void* this_ptr);
typedef int   (*FnDim)(const void* this_ptr);
typedef void  (*FnFboBlit)(void* target, const QRectC* targetRect, void* source, const QRectC* sourceRect, uint32_t buffers, uint32_t filter);
typedef void  (*FnRect4)(int x, int y, int w, int h);

// 2D UI (OPALDisp / QImage / QPaintDevice / QPainter / QFont) function pointer types
// Note: In MSVC x64 ABI, non-trivial return types (QSize in FnQImageSize, QImage in FnQImageCopy)
// pass a hidden caller-allocated return buffer pointer in RDX (2nd parameter, after `this` in RCX).
typedef void*          (*FnQImageCtorSize)(void* this_ptr, const QSizeC* sz, int fmt);
typedef void*          (*FnQImageCtorWH)(void* this_ptr, int w, int h, int fmt);
typedef void           (*FnQImageDtor)(void* this_ptr);
typedef uint8_t*       (*FnQImageScanLine)(void* this_ptr, int row);
typedef QSizeC*        (*FnQImageSize)(const void* this_ptr, QSizeC* out_sz);
typedef void*          (*FnQImageCopy)(const void* this_ptr, void* out_img, int x, int y, int w, int h);
typedef uint32_t       (*FnQImagePixel)(const void* this_ptr, int x, int y);
typedef void           (*FnDrawImageRectRect)(void* painter, const QRectC* tr, const void* img, const QRectC* sr, int flags);
typedef void           (*FnDrawImageXY)(void* painter, int x, int y, const void* img, int sx, int sy, int sw, int sh, int flags);
typedef void           (*FnDrawRects)(void* painter, const QRectC* rects, int count);
typedef void           (*FnDrawLines)(void* painter, const QRectC* lines, int count);
typedef void           (*FnDrawPoints)(void* painter, const QSizeC* points, int count);
typedef void           (*FnDrawLine4)(void* painter, int x1, int y1, int x2, int y2);
typedef void           (*FnDrawPoint2)(void* painter, int x, int y);
typedef void           (*FnFillRectBrush)(void* painter, const QRectC* rect, const void* brush);
typedef const void*    (*FnPainterBrush)(const void* painter);
typedef void           (*FnPainterSetWindow)(void* painter, int x, int y, int w, int h);
typedef void*          (*FnPainterDevice)(const void* painter);
typedef void           (*FnFontSetStyleStrategy)(void* font_ptr, int strategy);
typedef void           (*FnFontSetPointSize)(void* font_ptr, int pt);
typedef void           (*FnFontSetPointSizeF)(void* font_ptr, double pt);
typedef double         (*FnFontPointSizeF)(const void* font_ptr);
typedef void           (*FnSetDpr)(void* this_ptr, double dpr);
typedef double         (*FnGetDpr)(const void* this_ptr);

static FnFboCtor              g_orig_fbo_ctor        = nullptr;
static FnFboDtor              g_orig_fbo_dtor        = nullptr;
static FnDim                  g_orig_fbo_width       = nullptr;
static FnDim                  g_orig_fbo_height      = nullptr;
static FnFboBlit              g_orig_fbo_blit        = nullptr;
static FnRect4                g_orig_viewport        = nullptr;
static FnRect4                g_orig_scissor         = nullptr;
static void***                g_agl_table            = nullptr;

static FnQImageCtorSize       g_orig_qimg_ctor_size  = nullptr;
static FnQImageCtorWH         g_orig_qimg_ctor_wh    = nullptr;
static FnQImageDtor           g_qimg_dtor            = nullptr;
static FnQImageScanLine       g_qimg_scanline        = nullptr;
static FnQImageSize           g_orig_qimg_size       = nullptr;
static FnDim                  g_orig_qimg_width      = nullptr;
static FnDim                  g_orig_qimg_height     = nullptr;
static FnDim                  g_orig_qpd_width       = nullptr;
static FnDim                  g_orig_qpd_height      = nullptr;
static FnQImageCopy           g_orig_qimg_copy       = nullptr;
static FnQImagePixel          g_orig_qimg_pixel      = nullptr;
static FnDrawImageRectRect    g_orig_draw_image_rr   = nullptr;
static FnDrawImageXY          g_orig_draw_image_xy   = nullptr;
static FnDrawRects            g_orig_draw_rects      = nullptr;
static FnDrawLines            g_orig_draw_lines      = nullptr;
static FnDrawPoints           g_orig_draw_points     = nullptr;
static FnDrawLine4            g_orig_draw_line4      = nullptr;
static FnDrawPoint2           g_orig_draw_point2     = nullptr;
static FnFillRectBrush        g_painter_fill_rect    = nullptr;
static FnPainterBrush         g_painter_brush        = nullptr;
static FnPainterSetWindow     g_orig_set_window      = nullptr;
static FnPainterDevice        g_painter_device       = nullptr;
static FnFontSetStyleStrategy g_orig_font_style      = nullptr;
static FnFontSetPointSize     g_orig_font_pt_size    = nullptr;
static FnFontSetPointSizeF    g_qfont_set_pt_f       = nullptr;
static FnFontPointSizeF       g_qfont_get_pt_f       = nullptr;

static FnSetDpr               g_qimg_set_dpr         = nullptr;
static FnGetDpr               g_qimg_get_dpr         = nullptr;
static FnGetDpr               g_qpd_get_dpr          = nullptr;

static const uint8_t*         g_front_text_begin     = nullptr;
static const uint8_t*         g_front_text_end       = nullptr;
static const uint8_t*         g_ret_rot_ccw          = nullptr;
static const uint8_t*         g_rot_ccw_end          = nullptr;
static const uint8_t*         g_ret_rot_cw           = nullptr;
static const uint8_t*         g_rot_cw_end           = nullptr;
static double                 g_scale                = 1.0;
static DWORD                  g_main_tid             = 0;
static bool                   g_hooked               = false;

// High-DPI buffer for rotated vertical tab strips (ORastCreateImage -> 90 deg rotation -> DispDrawImage)
static uint32_t*              g_rot_buf              = nullptr;
static size_t                 g_rot_cap              = 0;
static int                    g_rot_lw               = 0;
static int                    g_rot_lh               = 0;
static int                    g_rot_pw               = 0;
static int                    g_rot_ph               = 0;
static double                 g_rot_dpr              = 1.0;
static uint64_t               g_rot_tick             = 0;
static const uint8_t*         g_rot_expected_site    = nullptr;
static const uint8_t*         g_rot_expected_end     = nullptr;
static bool                   g_rot_valid            = false;

struct FboRecord {
    const void* ptr;
    int logical_w;
    int logical_h;
    int physical_w;
    int physical_h;
};

static constexpr uint32_t kMaxTrackedFbos = 256;
static FboRecord g_fbo_table[kMaxTrackedFbos];
static uint32_t  g_fbo_next = 0;
static SRWLOCK   g_fbo_lock = SRWLOCK_INIT;

static bool IsInFrontText(const uint8_t* addr, size_t pre_bytes = 0, size_t post_bytes = 16) {
    if (!addr || !g_front_text_begin || !g_front_text_end) return false;
    auto u_addr  = reinterpret_cast<uintptr_t>(addr);
    auto u_begin = reinterpret_cast<uintptr_t>(g_front_text_begin);
    auto u_end   = reinterpret_cast<uintptr_t>(g_front_text_end);
    if (u_addr < u_begin + pre_bytes) return false;
    if (u_addr > u_end || u_end - u_addr < post_bytes) return false;
    return true;
}

static bool MatchPattern(const uint8_t* p, const uint8_t* pat, const char* mask, size_t len) {
    for (size_t j = 0; j < len; ++j) {
        if (mask && mask[j] != 'x') continue;
        if (p[j] != pat[j]) return false;
    }
    return true;
}

template <size_t N>
static bool MatchReturnBytes(const void* ret_addr, const uint8_t (&pat)[N]) {
    const auto* ret = static_cast<const uint8_t*>(ret_addr);
    return IsInFrontText(ret, 0, N) && MatchPattern(ret, pat, nullptr, N);
}

template <size_t N, size_t M>
static bool MatchReturnPattern(const void* ret_addr, const uint8_t (&pat)[N], const char (&mask)[M]) {
    static_assert(M == N + 1, "Mask length must match pattern length");
    const auto* ret = static_cast<const uint8_t*>(ret_addr);
    return IsInFrontText(ret, 0, N) && MatchPattern(ret, pat, mask, N);
}

static void ResetRotState() {
    g_rot_valid         = false;
    g_rot_expected_site = nullptr;
    g_rot_expected_end  = nullptr;
}

static int ScaleByDpr(int64_t v, double dpr) {
    if (dpr <= 1.001) {
        if (v > 2147483647LL) return 2147483647;
        if (v < -2147483647LL - 1LL) return -2147483647 - 1;
        return static_cast<int>(v);
    }
    double scaled = (v >= 0)
        ? (static_cast<double>(v) * dpr + 0.5)
        : (static_cast<double>(v) * dpr - 0.5);
    if (scaled >= 2147483647.0) return 2147483647;
    if (scaled <= -2147483648.0) return -2147483647 - 1;
    return static_cast<int>(scaled);
}

static int UnscaleByDpr(int v, double dpr) {
    if (dpr <= 1.001) return v;
    if (v >= 0) return static_cast<int>(v / dpr + 0.5);
    return static_cast<int>(v / dpr - 0.5);
}

static int UnscaleDimByDpr(int p, double dpr) {
    if (p <= 0 || dpr <= 1.001) return p;
    int l = UnscaleByDpr(p, dpr);
    return (l > 0) ? l : 1;
}

static int ScaleInt(int64_t v) {
    return ScaleByDpr(v, g_scale);
}

static double GetImageDpr(const void* img) {
    if (!img || !g_qimg_get_dpr) return 1.0;
    return g_qimg_get_dpr(img);
}

static double GetPaintDeviceDpr(const void* dev) {
    if (!dev || !g_qpd_get_dpr) return 1.0;
    return g_qpd_get_dpr(dev);
}

static void RecordFbo(const void* ptr, int lw, int lh, int pw, int ph) {
    if (!ptr) return;
    AcquireSRWLockExclusive(&g_fbo_lock);
    for (uint32_t i = 0; i < kMaxTrackedFbos; ++i) {
        if (g_fbo_table[i].ptr == ptr) {
            g_fbo_table[i].logical_w  = lw;
            g_fbo_table[i].logical_h  = lh;
            g_fbo_table[i].physical_w = pw;
            g_fbo_table[i].physical_h = ph;
            ReleaseSRWLockExclusive(&g_fbo_lock);
            return;
        }
    }
    for (uint32_t i = 0; i < kMaxTrackedFbos; ++i) {
        if (g_fbo_table[i].ptr == nullptr) {
            g_fbo_table[i] = { ptr, lw, lh, pw, ph };
            ReleaseSRWLockExclusive(&g_fbo_lock);
            return;
        }
    }
    uint32_t idx = (g_fbo_next++) & (kMaxTrackedFbos - 1);
    g_fbo_table[idx] = { ptr, lw, lh, pw, ph };
    ReleaseSRWLockExclusive(&g_fbo_lock);
}

static void EraseFbo(const void* ptr) {
    if (!ptr) return;
    AcquireSRWLockExclusive(&g_fbo_lock);
    for (uint32_t i = 0; i < kMaxTrackedFbos; ++i) {
        if (g_fbo_table[i].ptr == ptr) {
            g_fbo_table[i] = { nullptr, 0, 0, 0, 0 };
        }
    }
    ReleaseSRWLockExclusive(&g_fbo_lock);
}

static bool LookupFbo(const void* ptr, FboRecord* out_rec = nullptr) {
    if (!ptr) return false;
    AcquireSRWLockShared(&g_fbo_lock);
    for (uint32_t i = 0; i < kMaxTrackedFbos; ++i) {
        if (g_fbo_table[i].ptr == ptr) {
            if (out_rec) *out_rec = g_fbo_table[i];
            ReleaseSRWLockShared(&g_fbo_lock);
            return true;
        }
    }
    ReleaseSRWLockShared(&g_fbo_lock);
    return false;
}

// ============================================================================
// 1. 3D OpenGL Viewport Hooks
//    Modo creates 3D viewport FBOs at logical widget size, queries FBO
//    width()/height() for logical layout/camera checks, and issues AGLViewport,
//    AGLScissor, and blitFramebuffer in logical coordinates. We allocate the
//    FBO at physical High-DPI size, return the recorded logical size from
//    width()/height(), and scale viewport/scissor/blit rects to physical pixels.
//    Untracked FBOs (e.g. Modo 17 QQuickFramebufferObject / USD Hydra created
//    via QSize overload) are left untouched at their native physical size.
// ============================================================================

static void* HookFboCtor(void* this_ptr, int w, int h, const void* fmt_ptr) {
    if (g_scale > 1.001 && w > 0 && h > 0) {
        int pw = ScaleInt(w);
        int ph = ScaleInt(h);
        void* res = g_orig_fbo_ctor(this_ptr, pw, ph, fmt_ptr);
        RecordFbo(this_ptr, w, h, pw, ph);
        return res;
    }
    return g_orig_fbo_ctor(this_ptr, w, h, fmt_ptr);
}

static void HookFboDtor(void* this_ptr) {
    EraseFbo(this_ptr);
    g_orig_fbo_dtor(this_ptr);
}

static int HookFboWidth(const void* this_ptr) {
    int pw = g_orig_fbo_width(this_ptr);
    FboRecord rec = {};
    if (g_scale > 1.001 && LookupFbo(this_ptr, &rec) && rec.physical_w == pw) {
        return rec.logical_w;
    }
    return pw;
}

static int HookFboHeight(const void* this_ptr) {
    int ph = g_orig_fbo_height(this_ptr);
    FboRecord rec = {};
    if (g_scale > 1.001 && LookupFbo(this_ptr, &rec) && rec.physical_h == ph) {
        return rec.logical_h;
    }
    return ph;
}

static void HookFboBlit(void* target, const QRectC* tr, void* source, const QRectC* sr, uint32_t buffers, uint32_t filter) {
    if (g_scale <= 1.001 || !tr || !sr || (!LookupFbo(source) && !LookupFbo(target))) {
        g_orig_fbo_blit(target, tr, source, sr, buffers, filter);
        return;
    }
    int64_t tw = static_cast<int64_t>(tr->x2) - static_cast<int64_t>(tr->x1) + 1;
    int64_t th = static_cast<int64_t>(tr->y2) - static_cast<int64_t>(tr->y1) + 1;
    int64_t sw = static_cast<int64_t>(sr->x2) - static_cast<int64_t>(sr->x1) + 1;
    int64_t sh = static_cast<int64_t>(sr->y2) - static_cast<int64_t>(sr->y1) + 1;
    if (tw <= 0 || th <= 0 || sw <= 0 || sh <= 0) {
        g_orig_fbo_blit(target, tr, source, sr, buffers, filter);
        return;
    }
    int tx1 = ScaleInt(tr->x1);
    int ty1 = ScaleInt(tr->y1);
    int tx2 = ScaleInt(static_cast<int64_t>(tr->x2) + 1) - 1;
    int ty2 = ScaleInt(static_cast<int64_t>(tr->y2) + 1) - 1;
    int sx1 = ScaleInt(sr->x1);
    int sy1 = ScaleInt(sr->y1);
    int sx2 = ScaleInt(static_cast<int64_t>(sr->x2) + 1) - 1;
    int sy2 = ScaleInt(static_cast<int64_t>(sr->y2) + 1) - 1;
    QRectC scaled_t = { tx1, ty1, tx2, ty2 };
    QRectC scaled_s = { sx1, sy1, sx2, sy2 };
    g_orig_fbo_blit(target, &scaled_t, source, &scaled_s, buffers, filter);
}

static void ScaleGLBox(int x, int y, int w, int h, int* ox, int* oy, int* ow, int* oh) {
    if (g_scale <= 1.001) {
        *ox = x;
        *oy = y;
        *ow = w;
        *oh = h;
        return;
    }
    int px1 = ScaleInt(x);
    int py1 = ScaleInt(y);
    *ox = px1;
    *oy = py1;
    *ow = (w > 0) ? (ScaleInt(static_cast<int64_t>(x) + w) - px1) : ScaleInt(w);
    *oh = (h > 0) ? (ScaleInt(static_cast<int64_t>(y) + h) - py1) : ScaleInt(h);
}

static void HookViewport(int x, int y, int w, int h) {
    int px = 0, py = 0, pw = 0, ph = 0;
    ScaleGLBox(x, y, w, h, &px, &py, &pw, &ph);
    if (g_orig_viewport) {
        g_orig_viewport(px, py, pw, ph);
    } else if (g_agl_table && *g_agl_table) {
        reinterpret_cast<FnRect4>((*g_agl_table)[0xF0 / 8])(px, py, pw, ph);
    }
}

static void HookScissor(int x, int y, int w, int h) {
    int px = 0, py = 0, pw = 0, ph = 0;
    ScaleGLBox(x, y, w, h, &px, &py, &pw, &ph);
    if (g_orig_scissor) {
        g_orig_scissor(px, py, pw, ph);
    } else if (g_agl_table && *g_agl_table) {
        reinterpret_cast<FnRect4>((*g_agl_table)[0x460 / 8])(px, py, pw, ph);
    }
}

// Modo 16's WPaneGLGetPixelRatio (body at front3000.dll+0x1B8CB0; export thunk at +0x1AC800)
// calls Qt5's integer QPaintDevice::devicePixelRatio(), truncating 1.25/1.5 to 1.0 before
// returning double (whereas Modo 17 returns double directly). Return exact fractional DPR.
static double HookWPaneGLGetPixelRatio(const uint8_t* pane) {
    if (pane && g_qpd_get_dpr) {
        const uint8_t* sub = *reinterpret_cast<const uint8_t* const*>(pane + 0xD8);
        if (sub) {
            const uint8_t* info = *reinterpret_cast<const uint8_t* const*>(sub + 0x10);
            if (info && *reinterpret_cast<const int*>(info + 4) != 0) {
                const uint8_t* glw = *reinterpret_cast<const uint8_t* const*>(sub + 0x18);
                if (glw) {
                    return g_qpd_get_dpr(glw + 0x10);
                }
            }
        }
    }
    return g_scale;
}

// ============================================================================
// 2. 2D UI (OPALDisp / QImage / QPaintDevice / QPainter / QFont) Hooks
//    Modo's 2D panels, forms, trees, and buttons are rendered by OPAL into an
//    offscreen QImage buffer (LUXQt::LUXView::m_image at [widget+0x48] in Modo 16,
//    [widget+0x40] in Modo 17 where Qt6 QWidget is 8 bytes smaller) and then
//    blitted onto the QWidget in LUXQt::LUXView::paintEvent. By default, Modo
//    allocates these offscreen QImages at 1x logical pixel size with DPR=1.0.
// ============================================================================

// Check QPaintDevice::devType() (vtable[1]) == QInternal::Image (3)
static bool IsQImageDevice(const void* this_ptr) {
    if (!this_ptr) return false;
    void* const* vtable = *reinterpret_cast<void* const* const*>(this_ptr);
    if (!vtable || !vtable[1]) return false;
    typedef int (*FnDevType)(const void*);
    return reinterpret_cast<FnDevType>(vtable[1])(this_ptr) == 3;
}

static bool IsHighDpiPainter(const void* painter) {
    if (!painter || !g_painter_device) return false;
    const void* dev = g_painter_device(painter);
    return dev && IsQImageDevice(dev) && GetPaintDeviceDpr(dev) > 1.001;
}

// LUXQt::LUXView::updateImage (call at Modo 16 +0x172077, Modo 17 +0xD703EA) constructs its
// offscreen ARGB32 (fmt == 5) backing QImage via QImage(const QSize&, Format).
// Verify both Format_ARGB32 and the exact return site so non-LUXView callers stay 1:1.
static void* HookQImageCtorSize(void* this_ptr, const QSizeC* sz, int fmt) {
    // Return site in LUXQt::LUXView::updateImage: nop; mov rcx, rdi; call QWidget::palette
    static constexpr uint8_t kSigUpdateImage[] = { 0x90, 0x48, 0x8B, 0xCF, 0xFF, 0x15 };
    if (g_scale > 1.001 && fmt == 5 && sz && sz->w > 0 && sz->h > 0 && g_qimg_set_dpr &&
        MatchReturnBytes(_ReturnAddress(), kSigUpdateImage)) {
        QSizeC scaled_sz = { ScaleInt(sz->w), ScaleInt(sz->h) };
        void* res = g_orig_qimg_ctor_size(this_ptr, &scaled_sz, fmt);
        g_qimg_set_dpr(this_ptr, g_scale);
        return res;
    }
    return g_orig_qimg_ctor_size(this_ptr, sz, fmt);
}

// OPALDisp's internal QtRasterDisplay / ORast create callback (call at Modo 16 +0x1B60D0,
// Modo 17 +0xD9ED10; distinct from the ORastCreate wrapper exported at Modo 16 +0x5D40,
// Modo 17 +0xAAB290) allocates temporary offscreen ARGB32 (fmt == 5) QImages via
// QImage(int w, int h, Format) for text/tabs. Only scale calls from this callback so raw
// icon/texture assets stay 1:1.
static void* HookQImageCtorWH(void* this_ptr, int w, int h, int fmt) {
    // Return site after QImage ctor: lea rax, [rip+disp32]; mov [rdi], rax (local QImage vftable)
    static constexpr uint8_t kSigORastCreate[] = { 0x48, 0x8D, 0x05, 0, 0, 0, 0, 0x48, 0x89, 0x07 };
    if (g_scale > 1.001 && fmt == 5 && w > 0 && h > 0 && g_qimg_set_dpr &&
        MatchReturnPattern(_ReturnAddress(), kSigORastCreate, "xxx????xxx")) {
        void* res = g_orig_qimg_ctor_wh(this_ptr, ScaleInt(w), ScaleInt(h), fmt);
        g_qimg_set_dpr(this_ptr, g_scale);
        return res;
    }
    return g_orig_qimg_ctor_wh(this_ptr, w, h, fmt);
}

// Report logical dimensions back to Modo for High-DPI QImages (dpr > 1.0) using
// each QImage's own devicePixelRatio() so LUXQt::LUXView::updateImage, OPAL layout math,
// and QOpenGLWidget::grabFramebuffer / QPixmap::toImage consumers operate cleanly.
static QSizeC* HookQImageSize(const void* this_ptr, QSizeC* out_sz) {
    QSizeC* res = g_orig_qimg_size(this_ptr, out_sz);
    if (g_scale > 1.001 && out_sz) {
        double dpr = GetImageDpr(this_ptr);
        if (dpr > 1.001) {
            out_sz->w = UnscaleDimByDpr(out_sz->w, dpr);
            out_sz->h = UnscaleDimByDpr(out_sz->h, dpr);
        }
    }
    return res;
}

static int HookQImageWidth(const void* this_ptr) {
    int pw = g_orig_qimg_width(this_ptr);
    if (g_scale > 1.001) {
        double dpr = GetImageDpr(this_ptr);
        if (dpr > 1.001) {
            return UnscaleDimByDpr(pw, dpr);
        }
    }
    return pw;
}

// Modo renders vertical tab strips by first drawing horizontal text into an
// ORast QImage, converting it into a logical-sized ILxImage via ORastCreateImage ->
// QImageToILxImage (Modo 16 +0x1A29E0, Modo 17 +0xD9A830), rotating the ILxImage 90 degrees
// on the CPU (CCW or CW), and blitting via DispDrawImage -> OPALDisp::BlitImage -> QPainter::drawImage.
// Pre-rotate the physical High-DPI QImage into g_rot_buf here and substitute it in HookDrawImageXY.
static int HookQImageHeight(const void* this_ptr) {
    int ph = g_orig_qimg_height(this_ptr);
    if (g_scale <= 1.001) return ph;

    // Return site at start of QImageToILxImage: mov rcx, rsi; mov ebx, eax; call QImage::width
    static constexpr uint8_t kSigQImageToILx[] = { 0x48, 0x8B, 0xCE, 0x8B, 0xD8, 0xFF, 0x15 };
    const bool is_q2i_main = MatchReturnBytes(_ReturnAddress(), kSigQImageToILx) &&
                             GetCurrentThreadId() == g_main_tid;
    if (is_q2i_main) {
        // Always clear any unconsumed rotation buffer from a prior clipped-out tab.
        ResetRotState();
    }

    double dpr = GetImageDpr(this_ptr);
    if (dpr > 1.001) {
        int lh = UnscaleDimByDpr(ph, dpr);
        if (is_q2i_main && g_orig_qimg_width && g_orig_qimg_pixel) {
            void* frames[12] = {};
            USHORT num_frames = CaptureStackBackTrace(0, 12, frames, nullptr);
            int rot_mode = 0; // 1 = 90 deg CCW, 2 = 90 deg CW
            for (USHORT i = 0; i < num_frames; ++i) {
                const uint8_t* caller_ret = reinterpret_cast<const uint8_t*>(frames[i]);
                if (g_ret_rot_ccw && caller_ret == g_ret_rot_ccw) {
                    rot_mode = 1;
                    break;
                }
                if (g_ret_rot_cw && caller_ret == g_ret_rot_cw) {
                    rot_mode = 2;
                    break;
                }
            }
            if (rot_mode != 0) {
                int pw = g_orig_qimg_width(this_ptr);
                int lw = UnscaleDimByDpr(pw, dpr);
                if (pw > 0 && ph > 0 && lw > 0 && lh > 0) {
                    size_t needed = static_cast<size_t>(pw) * static_cast<size_t>(ph);
                    if (needed > g_rot_cap) {
                        if (g_rot_buf) VirtualFree(g_rot_buf, 0, MEM_RELEASE);
                        g_rot_buf = reinterpret_cast<uint32_t*>(
                            VirtualAlloc(nullptr, needed * sizeof(uint32_t), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
                        g_rot_cap = g_rot_buf ? needed : 0;
                    }
                    if (g_rot_buf) {
                        for (int py = 0; py < ph; ++py) {
                            for (int px = 0; px < pw; ++px) {
                                // Force opaque alpha (0xFF) so Format_ARGB32 tab pixels blit without alpha fade.
                                uint32_t pix = g_orig_qimg_pixel(this_ptr, px, py) | 0xFF000000u;
                                if (rot_mode == 1) {
                                    g_rot_buf[static_cast<size_t>(pw - 1 - px) * ph + py] = pix;
                                } else {
                                    g_rot_buf[static_cast<size_t>(px) * ph + (ph - 1 - py)] = pix;
                                }
                            }
                        }
                        g_rot_lw            = lh;
                        g_rot_lh            = lw;
                        g_rot_pw            = ph;
                        g_rot_ph            = pw;
                        g_rot_dpr           = dpr;
                        g_rot_tick          = GetTickCount64();
                        g_rot_expected_site = (rot_mode == 1) ? g_ret_rot_ccw : g_ret_rot_cw;
                        g_rot_expected_end  = (rot_mode == 1) ? g_rot_ccw_end : g_rot_cw_end;
                        g_rot_valid         = true;
                    }
                }
            }
        }
        return lh;
    }
    return ph;
}

static int HookQPaintDeviceWidth(const void* this_ptr) {
    int pw = g_orig_qpd_width(this_ptr);
    if (g_scale > 1.001 && IsQImageDevice(this_ptr)) {
        double dpr = GetPaintDeviceDpr(this_ptr);
        if (dpr > 1.001) {
            return UnscaleDimByDpr(pw, dpr);
        }
    }
    return pw;
}

static int HookQPaintDeviceHeight(const void* this_ptr) {
    int ph = g_orig_qpd_height(this_ptr);
    if (g_scale > 1.001 && IsQImageDevice(this_ptr)) {
        double dpr = GetPaintDeviceDpr(this_ptr);
        if (dpr > 1.001) {
            return UnscaleDimByDpr(ph, dpr);
        }
    }
    return ph;
}

// QImage::copy and QImage::pixel operate in physical image coordinates in Qt,
// whereas Modo passes logical coordinates when cropping/sampling its offscreen QImage.
static void* HookQImageCopy(const void* this_ptr, void* out_img, int x, int y, int w, int h) {
    if (g_scale > 1.001 && g_qimg_set_dpr) {
        double dpr = GetImageDpr(this_ptr);
        if (dpr > 1.001) {
            int sx = ScaleByDpr(x, dpr);
            int sy = ScaleByDpr(y, dpr);
            int sw = (w > 0) ? (ScaleByDpr(static_cast<int64_t>(x) + w, dpr) - sx) : w;
            int sh = (h > 0) ? (ScaleByDpr(static_cast<int64_t>(y) + h, dpr) - sy) : h;
            void* res = g_orig_qimg_copy(this_ptr, out_img, sx, sy, sw, sh);
            if (out_img) {
                g_qimg_set_dpr(out_img, dpr);
            }
            return res;
        }
    }
    return g_orig_qimg_copy(this_ptr, out_img, x, y, w, h);
}

static uint32_t HookQImagePixel(const void* this_ptr, int x, int y) {
    if (g_scale > 1.001) {
        double dpr = GetImageDpr(this_ptr);
        if (dpr > 1.001) {
            int px = ScaleByDpr(x, dpr);
            int py = ScaleByDpr(y, dpr);
            if (g_orig_qimg_width && g_orig_qimg_height) {
                int pw = g_orig_qimg_width(this_ptr);
                int ph = g_orig_qimg_height(this_ptr);
                if (pw > 0 && ph > 0) {
                    if (px < 0) px = 0;
                    else if (px >= pw) px = pw - 1;
                    if (py < 0) py = 0;
                    else if (py >= ph) py = ph - 1;
                }
            }
            return g_orig_qimg_pixel(this_ptr, px, py);
        }
    }
    return g_orig_qimg_pixel(this_ptr, x, y);
}

// In both Qt5 and Qt6, QPainter::drawImage(targetRect, image, sourceRect) interprets
// targetRect in logical painter coordinates but sourceRect in physical QImage pixels
// even when image.devicePixelRatio() > 1.0. Scale half-open source endpoints by DPR
// so adjacent dirty bands at fractional 125% / 150% partition physical pixels 1-to-1.
static void HookDrawImageRectRect(void* painter, const QRectC* tr, const void* img, const QRectC* sr, int flags) {
    if (g_scale > 1.001 && sr && img) {
        double dpr = GetImageDpr(img);
        if (dpr > 1.001) {
            int64_t sw = static_cast<int64_t>(sr->x2) - static_cast<int64_t>(sr->x1) + 1;
            int64_t sh = static_cast<int64_t>(sr->y2) - static_cast<int64_t>(sr->y1) + 1;
            if (sw > 0 && sh > 0) {
                int sx1 = ScaleByDpr(sr->x1, dpr);
                int sy1 = ScaleByDpr(sr->y1, dpr);
                int sx2 = ScaleByDpr(static_cast<int64_t>(sr->x2) + 1, dpr);
                int sy2 = ScaleByDpr(static_cast<int64_t>(sr->y2) + 1, dpr);
                if (sx2 > sx1 && sy2 > sy1) {
                    QRectC scaled_s = { sx1, sy1, sx2 - 1, sy2 - 1 };
                    g_orig_draw_image_rr(painter, tr, img, &scaled_s, flags);
                    return;
                }
            }
        }
    }
    g_orig_draw_image_rr(painter, tr, img, sr, flags);
}

static bool HasActiveRotCallerFrame(const uint8_t* expected_site, const uint8_t* expected_end) {
    if (!expected_site || !expected_end || expected_end <= expected_site) return false;
    void* frames[12] = {};
    USHORT num_frames = CaptureStackBackTrace(0, 12, frames, nullptr);
    for (USHORT i = 0; i < num_frames; ++i) {
        const uint8_t* caller_ret = reinterpret_cast<const uint8_t*>(frames[i]);
        if (caller_ret > expected_site && caller_ret < expected_end) {
            return true;
        }
    }
    return false;
}

// Intercept OPALDisp::BlitImage (call at Modo 16 +0x1B59EE, Modo 17 +0xD9E71C) for rotated
// vertical tab strips (captured in HookQImageHeight), verifying that an active stack
// frame still lies inside the originating vertical tab function while normalizing
// Qt sub-rects and clearing g_rot_valid so clipped tab buffers never leak onto 1x icons.
static void HookDrawImageXY(void* painter, int x, int y, const void* img, int sx, int sy, int sw, int sh, int flags) {
    // Return site in OPALDisp::BlitImage: mov [rsp+20h], edi; mov r9d, esi; mov r8d, ebp
    static constexpr uint8_t kSigOpalBlitImage[] = {
        0x89, 0x7C, 0x24, 0x20, 0x44, 0x8B, 0xCE, 0x44, 0x8B, 0xC5
    };
    if (g_rot_valid && g_scale > 1.001 && GetCurrentThreadId() == g_main_tid &&
        MatchReturnBytes(_ReturnAddress(), kSigOpalBlitImage)) {
        const uint8_t* expected_site = g_rot_expected_site;
        const uint8_t* expected_end  = g_rot_expected_end;
        ResetRotState();

        if ((GetTickCount64() - g_rot_tick <= 250) &&
            HasActiveRotCallerFrame(expected_site, expected_end) &&
            IsHighDpiPainter(painter) && img && GetImageDpr(img) <= 1.001 &&
            g_orig_qimg_width && g_orig_qimg_height &&
            g_orig_qimg_width(img) == g_rot_lw && g_orig_qimg_height(img) == g_rot_lh &&
            g_orig_qimg_ctor_wh && g_qimg_dtor && g_qimg_scanline && g_qimg_set_dpr && g_orig_draw_image_rr) {
            int64_t nx  = x;
            int64_t ny  = y;
            int64_t nsx = sx;
            int64_t nsy = sy;
            int64_t nsw = (sw < 0) ? (static_cast<int64_t>(g_rot_lw) - nsx) : sw;
            int64_t nsh = (sh < 0) ? (static_cast<int64_t>(g_rot_lh) - nsy) : sh;
            if (nsx < 0) {
                nx  -= nsx;
                nsw += nsx;
                nsx  = 0;
            }
            if (nsy < 0) {
                ny  -= nsy;
                nsh += nsy;
                nsy  = 0;
            }
            if (nsx + nsw > g_rot_lw) nsw = static_cast<int64_t>(g_rot_lw) - nsx;
            if (nsy + nsh > g_rot_lh) nsh = static_cast<int64_t>(g_rot_lh) - nsy;

            if (nsw > 0 && nsh > 0) {
                int psx1 = ScaleByDpr(nsx, g_rot_dpr);
                int psy1 = ScaleByDpr(nsy, g_rot_dpr);
                int psx2 = ScaleByDpr(nsx + nsw, g_rot_dpr);
                int psy2 = ScaleByDpr(nsy + nsh, g_rot_dpr);
                if (psx1 < 0) psx1 = 0;
                if (psy1 < 0) psy1 = 0;
                if (psx2 > g_rot_pw) psx2 = g_rot_pw;
                if (psy2 > g_rot_ph) psy2 = g_rot_ph;
                if (psx2 > psx1 && psy2 > psy1) {
                    // Headerless in-place QImage stack buffer (sizeof(QImage) is 32B in Qt5, 24B in Qt6).
                    alignas(16) uint8_t tmp_qimg[64];
                    g_orig_qimg_ctor_wh(tmp_qimg, g_rot_pw, g_rot_ph, 5);
                    g_qimg_set_dpr(tmp_qimg, g_rot_dpr);
                    for (int ry = 0; ry < g_rot_ph; ++ry) {
                        uint32_t* dst = reinterpret_cast<uint32_t*>(g_qimg_scanline(tmp_qimg, ry));
                        const uint32_t* src = g_rot_buf + static_cast<size_t>(ry) * g_rot_pw;
                        if (dst) {
                            for (int rx = 0; rx < g_rot_pw; ++rx) {
                                dst[rx] = src[rx];
                            }
                        }
                    }
                    QRectC tr = {
                        static_cast<int>(nx),
                        static_cast<int>(ny),
                        static_cast<int>(nx + nsw - 1),
                        static_cast<int>(ny + nsh - 1)
                    };
                    QRectC sr = { psx1, psy1, psx2 - 1, psy2 - 1 };
                    g_orig_draw_image_rr(painter, &tr, tmp_qimg, &sr, flags);
                    g_qimg_dtor(tmp_qimg);
                    return;
                }
            }
        }
    }
    g_orig_draw_image_xy(painter, x, y, img, sx, sy, sw, sh, flags);
}

// OPALDisp sets both QBrush(color) and QPainter::setPen(const QColor&) -- which in Qt5/Qt6
// constructs a non-cosmetic pen of logical width 1.0 (cosmetic = false) -- across its 2D
// primitives (FillRect, DrawLine, DrawPoint). Under fractional DPR (125% / 150%), a 1.0 * dpr
// pen stroke does not partition physical pixels into the exact half-open interval
// [round(k * dpr), round((k + 1) * dpr)) that QPainter::fillRect uses, causing 1px seams/overlaps.
// Convert these primitives to QPainter::fillRect ONLY when called from OPALDisp onto a High-DPI QImage.

// Return sites in OPALDisp::FillRect, OPALDisp::DrawLine (Modo 16 & 17), and OPALDisp::DrawPoint (Modo 16 & 17)
static constexpr uint8_t kSigOpalFillRect[]  = { 0x48, 0x8D, 0x4C, 0x24, 0x38, 0xFF, 0x15 };
static constexpr uint8_t kSigOpalDrawLine[]  = { 0x48, 0x8B, 0x4B, 0x18, 0xFF, 0x15 };
static constexpr uint8_t kSigOpalDrawPoint[] = { 0xC7, 0x44, 0x24, 0x20, 0x01, 0x00, 0x00, 0x00 };

// OPALDisp::FillRect subtracts 1 from width/height before drawRects to compensate for the 1.0px
// logical pen stroke expansion. Restore +1 (x2 + 1, y2 + 1) when forwarding to fillRect.
static void HookDrawRects(void* painter, const QRectC* rects, int count) {
    if (g_scale > 1.001 && rects && count == 1 && g_painter_fill_rect && g_painter_brush &&
        MatchReturnBytes(_ReturnAddress(), kSigOpalFillRect) && IsHighDpiPainter(painter)) {
        QRectC full_rect = { rects[0].x1, rects[0].y1, rects[0].x2 + 1, rects[0].y2 + 1 };
        g_painter_fill_rect(painter, &full_rect, g_painter_brush(painter));
        return;
    }
    g_orig_draw_rects(painter, rects, count);
}

// Convert OPALDisp::DrawLine's axis-aligned 1.0px logical pen lines to 1-logical-cell fillRects
// to prevent 1px unpainted gaps at fractional scales. Both drawLines(const QLine*, int) (used by
// Qt5's inline drawLine header in Modo 16) and drawLine(int,int,int,int) (Modo 17 / Qt6) are hooked.
static bool TryFillAxisLine(void* painter, int x1, int y1, int x2, int y2) {
    if (y1 == y2) {
        int lx1 = (x1 <= x2) ? x1 : x2;
        int lx2 = (x1 <= x2) ? x2 : x1;
        QRectC full_rect = { lx1, y1, lx2, y1 };
        g_painter_fill_rect(painter, &full_rect, g_painter_brush(painter));
        return true;
    }
    if (x1 == x2) {
        int ly1 = (y1 <= y2) ? y1 : y2;
        int ly2 = (y1 <= y2) ? y2 : y1;
        QRectC full_rect = { x1, ly1, x1, ly2 };
        g_painter_fill_rect(painter, &full_rect, g_painter_brush(painter));
        return true;
    }
    return false;
}

static void HookDrawLines(void* painter, const QRectC* lines, int count) {
    if (g_scale > 1.001 && lines && count == 1 && g_painter_fill_rect && g_painter_brush &&
        MatchReturnBytes(_ReturnAddress(), kSigOpalDrawLine) && IsHighDpiPainter(painter) &&
        TryFillAxisLine(painter, lines[0].x1, lines[0].y1, lines[0].x2, lines[0].y2)) {
        return;
    }
    g_orig_draw_lines(painter, lines, count);
}

static void HookDrawLine4(void* painter, int x1, int y1, int x2, int y2) {
    if (g_scale > 1.001 && g_painter_fill_rect && g_painter_brush &&
        MatchReturnBytes(_ReturnAddress(), kSigOpalDrawLine) && IsHighDpiPainter(painter) &&
        TryFillAxisLine(painter, x1, y1, x2, y2)) {
        return;
    }
    g_orig_draw_line4(painter, x1, y1, x2, y2);
}

// OPALDisp::DrawPoint uses its 1.0px logical pen to erase square FillRect corners and draw
// checkmarks/arrows. Convert to a 1x1 logical cell fillRect across both drawPoints (Modo 16 / Qt5
// inline header) and drawPoint (Modo 17 / Qt6 direct export).
static void HookDrawPoints(void* painter, const QSizeC* points, int count) {
    if (g_scale > 1.001 && points && count == 1 && g_painter_fill_rect && g_painter_brush &&
        MatchReturnBytes(_ReturnAddress(), kSigOpalDrawPoint) && IsHighDpiPainter(painter)) {
        QRectC full_rect = { points[0].w, points[0].h, points[0].w, points[0].h };
        g_painter_fill_rect(painter, &full_rect, g_painter_brush(painter));
        return;
    }
    g_orig_draw_points(painter, points, count);
}

static void HookDrawPoint2(void* painter, int x, int y) {
    if (g_scale > 1.001 && g_painter_fill_rect && g_painter_brush &&
        MatchReturnBytes(_ReturnAddress(), kSigOpalDrawPoint) && IsHighDpiPainter(painter)) {
        QRectC full_rect = { x, y, x, y };
        g_painter_fill_rect(painter, &full_rect, g_painter_brush(painter));
        return;
    }
    g_orig_draw_point2(painter, x, y);
}

// Modo calls QPainter::setWindow(logical_x, logical_y, logical_w, logical_h) on OPAL
// QImage painters. In Qt5/Qt6, QPainter::begin(QImage) initializes viewport (vw, vh)
// to QImage::metric(PdmWidth/PdmHeight) == physical dimensions and post-multiplies by
// fromScale(dpr, dpr), so viewTransform computes ((p - wx) * (vw / ww)) * dpr.
// Keeping (x, y) logical while scaling (w, h) by the device's DPR keeps vw / ww == 1.0
// and maps pane origin (x, y) cleanly to physical pixel (0, 0).
static void HookPainterSetWindow(void* painter, int x, int y, int w, int h) {
    if (g_scale > 1.001 && painter && g_painter_device) {
        void* dev = g_painter_device(painter);
        if (dev && IsQImageDevice(dev)) {
            double dpr = GetPaintDeviceDpr(dev);
            if (dpr > 1.001) {
                g_orig_set_window(painter, x, y, ScaleByDpr(w, dpr), ScaleByDpr(h, dpr));
                return;
            }
        }
    }
    g_orig_set_window(painter, x, y, w, h);
}

// OPALDisp::DrawText (call at Modo 16 +0x1B579E, Modo 17 +0xD9E50E) sets QFont::NoAntialias
// (0x100) on 2D UI fonts, producing jagged glyphs at High-DPI. Verify the exact
// OPALDisp::DrawText return site before upgrading 0x100 to QFont::PreferAntialias
// (0x80) so 3D OpenGL 1-bit Format_Mono bitmap fonts in WPaneGLDrawTextW and
// CreateGLFont keep crisp monochrome rasterization.
static void HookFontSetStyleStrategy(void* font_ptr, int strategy) {
    // Return site in OPALDisp::DrawText: mov rdx, [r15]; mov rcx, [rbx+18h]; call QPainter::setFont
    static constexpr uint8_t kSigOpalDrawText[] = {
        0x49, 0x8B, 0x17, 0x48, 0x8B, 0x4B, 0x18, 0xFF, 0x15
    };
    if (g_scale > 1.001 && strategy == 0x100 && MatchReturnBytes(_ReturnAddress(), kSigOpalDrawText)) {
        strategy = 0x80;
    }
    g_orig_font_style(font_ptr, strategy);
}

// Modo bakes 3D OpenGL viewport text (header pill buttons & bottom-right HUD) into
// 1-bit glBitmap display lists in WPaneGLDrawTextW and CreateGLFont.
// - Modo 16 multiplies pointSize by Qt5's integer devicePixelRatio() (1.25/1.5 -> 1),
//   leaving 3D viewport text unscaled at 125% and 150%.
// - Modo 17 truncates (pointSize * dpr) to int via cvttsd2si.
// - Both Modo 16 & 17 overwrite useLargeFont=1 with a hardcoded 56 pt without DPR scaling.
// Verify the exact GL font call sites via _ReturnAddress() so non-GL QWidget fonts
// (e.g. Modo 17 QLabel call at +0xC15C41) remain untouched, and apply exact floating-point scaling.
static void HookFontSetPointSize(void* font_ptr, int pt) {
    if (g_scale > 1.001 && font_ptr && g_qfont_set_pt_f && g_qfont_get_pt_f) {
        const auto* ret = static_cast<const uint8_t*>(_ReturnAddress());
        if (IsInFrontText(ret, 16, 1)) {
            static constexpr uint8_t kSigM16Dpr[4]  = { 0x0F, 0xAF, 0, 0x8B };
            static constexpr uint8_t kSigM17Dpr[3]  = { 0xF2, 0x0F, 0x2C };
            static constexpr uint8_t kSigLargeGl[4] = { 0x74, 0x0E, 0xBA, 0x38 };
            // Modo 16 DPR-scaled GL font sites (imul reg, reg; mov edx, reg before call)
            bool is_m16_dpr = MatchPattern(ret - 16, kSigM16Dpr, "xx?x", 4) ||
                              MatchPattern(ret - 14, kSigM16Dpr, "xx?x", 4);
            // Modo 17 DPR-scaled GL font sites (cvttsd2si edx, xmm before call)
            bool is_m17_dpr = MatchPattern(ret - 15, kSigM17Dpr, nullptr, 3) ||
                              MatchPattern(ret - 13, kSigM17Dpr, nullptr, 3);
            if (is_m16_dpr || is_m17_dpr) {
                double base_pt = g_qfont_get_pt_f(font_ptr);
                if (base_pt > 0.0) {
                    g_qfont_set_pt_f(font_ptr, base_pt * g_scale);
                    return;
                }
            }
            // Modo 16 & 17 CreateGLFont useLargeFont=1 site (je +0x0E; mov edx, 56 before call)
            if (pt > 0 && MatchPattern(ret - 16, kSigLargeGl, nullptr, 4)) {
                g_qfont_set_pt_f(font_ptr, static_cast<double>(pt) * g_scale);
                return;
            }
        }
    }
    g_orig_font_pt_size(font_ptr, pt);
}

// ============================================================================
// 3. IAT Lookup & Patching
// ============================================================================

static bool StrEq(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

static void** FindIATSlot(uint8_t* base, const char* target_symbol) {
    if (!base) return nullptr;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

    DWORD import_rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!import_rva) return nullptr;

    auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + import_rva);
    while (desc->OriginalFirstThunk || desc->Name || desc->FirstThunk) {
        auto* ilt = reinterpret_cast<uint64_t*>(base + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));
        auto* iat = reinterpret_cast<void**>(base + desc->FirstThunk);

        while (*ilt) {
            uint64_t thunk = *ilt;
            if ((thunk & IMAGE_ORDINAL_FLAG64) == 0) {
                auto* import_by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + (thunk & 0x7FFFFFFFULL));
                const char* fn_name = reinterpret_cast<const char*>(import_by_name->Name);
                if (StrEq(fn_name, target_symbol)) {
                    return iat;
                }
            }
            ++ilt;
            ++iat;
        }
        ++desc;
    }
    return nullptr;
}

template <typename Fn>
static void BindExport(HMODULE mod, const char* symbol, Fn& out_fn) {
    out_fn = reinterpret_cast<Fn>(GetProcAddress(mod, symbol));
}

template <typename Fn>
static void PatchSlot(void** slot, Fn hook_fn, Fn* out_orig = nullptr) {
    if (!slot) return;
    if (out_orig && !*out_orig) {
        *out_orig = reinterpret_cast<Fn>(*slot);
    }
    DWORD old_prot = 0;
    if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_prot)) {
        *slot = reinterpret_cast<void*>(hook_fn);
        VirtualProtect(slot, sizeof(void*), old_prot, &old_prot);
    }
}

template <typename Fn>
static void PatchIAT(uint8_t* base, const char* symbol, Fn hook_fn, Fn* out_orig = nullptr) {
    PatchSlot(FindIATSlot(base, symbol), hook_fn, out_orig);
}

static bool GetModuleTextBounds(const uint8_t* base, const uint8_t** out_begin, const uint8_t** out_end) {
    if (!base) return false;
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.NumberOfSections == 0) return false;
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if ((sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) && sec[i].Misc.VirtualSize > 0) {
            *out_begin = base + sec[i].VirtualAddress;
            *out_end   = *out_begin + sec[i].Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

static uint8_t* FindCodePattern(uint8_t* base, size_t hint_rva, const uint8_t* pat, const char* mask, size_t len) {
    const uint8_t* text_begin = nullptr;
    const uint8_t* text_end   = nullptr;
    if (!GetModuleTextBounds(base, &text_begin, &text_end)) return nullptr;
    size_t size = static_cast<size_t>(text_end - text_begin);
    if (size < len) return nullptr;
    size_t text_rva = static_cast<size_t>(text_begin - base);
    if (hint_rva >= text_rva && hint_rva - text_rva <= size - len) {
        if (MatchPattern(base + hint_rva, pat, mask, len)) {
            return base + hint_rva;
        }
    }
    uint8_t* start = const_cast<uint8_t*>(text_begin);
    for (size_t i = 0; i <= size - len; ++i) {
        if (MatchPattern(start + i, pat, mask, len)) return start + i;
    }
    return nullptr;
}

static const uint8_t* GetFunctionEndOrFallback(const uint8_t* code_addr, size_t fallback_span = 0x180) {
    if (!code_addr) return nullptr;
    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(code_addr), &image_base, nullptr);
    if (rf && image_base != 0) {
        const uint8_t* fn_end = reinterpret_cast<const uint8_t*>(image_base + rf->EndAddress);
        if (fn_end > code_addr) {
            return fn_end;
        }
    }
    return code_addr + fallback_span;
}

static bool PatchCodeBytes(uint8_t* target, const uint8_t* bytes, size_t len) {
    DWORD old_prot = 0;
    if (!target || !VirtualProtect(target, len, PAGE_EXECUTE_READWRITE, &old_prot)) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        target[i] = bytes[i];
    }
    VirtualProtect(target, len, old_prot, &old_prot);
    FlushInstructionCache(GetCurrentProcess(), target, len);
    return true;
}

template <typename Fn>
static void PatchJump14(uint8_t* target, Fn dst_fn) {
    uint8_t jmp[14] = { 0xFF, 0x25, 0, 0, 0, 0 };
    *reinterpret_cast<void**>(jmp + 6) = reinterpret_cast<void*>(dst_fn);
    PatchCodeBytes(target, jmp, sizeof(jmp));
}

static void InstallAllHooks() {
    if (g_hooked) return;

    // Modo 16 splits UI/GL into front3000.dll + frame3000.dll (+ GnzUI.dll) + Qt5Gui.dll;
    // Modo 17 statically links front/frame into modo.exe and uses Qt6Gui.dll.
    HMODULE front_mod = GetModuleHandleA("front3000.dll");
    const bool is_modo16 = (front_mod != nullptr);
    auto* front = reinterpret_cast<uint8_t*>(is_modo16 ? front_mod : GetModuleHandleA(nullptr));
    auto* frame = reinterpret_cast<uint8_t*>(is_modo16 ? GetModuleHandleA("frame3000.dll") : nullptr);
    HMODULE qtgui = GetModuleHandleA(is_modo16 ? "Qt5Gui.dll" : "Qt6Gui.dll");
    if (!front || (is_modo16 && !frame) || !qtgui) return;

    const uint8_t* text_begin = nullptr;
    const uint8_t* text_end   = nullptr;
    if (!GetModuleTextBounds(front, &text_begin, &text_end)) return;

    BindExport(qtgui, "?setDevicePixelRatio@QImage@@QEAAXN@Z",                      g_qimg_set_dpr);
    BindExport(qtgui, "?devicePixelRatio@QImage@@QEBANXZ",                          g_qimg_get_dpr);
    BindExport(qtgui, "?devicePixelRatioF@QPaintDevice@@QEBANXZ",                   g_qpd_get_dpr);
    BindExport(qtgui, "?device@QPainter@@QEBAPEAVQPaintDevice@@XZ",                 g_painter_device);
    BindExport(qtgui, "?fillRect@QPainter@@QEAAXAEBVQRect@@AEBVQBrush@@@Z",         g_painter_fill_rect);
    BindExport(qtgui, "?brush@QPainter@@QEBAAEBVQBrush@@XZ",                        g_painter_brush);
    BindExport(qtgui, "??1QImage@@UEAA@XZ",                                         g_qimg_dtor);
    BindExport(qtgui, "?scanLine@QImage@@QEAAPEAEH@Z",                              g_qimg_scanline);
    BindExport(qtgui, "?setPointSizeF@QFont@@QEAAXN@Z",                             g_qfont_set_pt_f);
    BindExport(qtgui, "?pointSizeF@QFont@@QEBANXZ",                                 g_qfont_get_pt_f);

    // 1. Required 3D OpenGL Viewport IAT slots & AGLViewport/AGLScissor targets
    void** s_fbo_ctor   = FindIATSlot(front, "??0QOpenGLFramebufferObject@@QEAA@HHAEBVQOpenGLFramebufferObjectFormat@@@Z");
    void** s_fbo_dtor   = FindIATSlot(front, "??1QOpenGLFramebufferObject@@UEAA@XZ");
    void** s_fbo_width  = FindIATSlot(front, "?width@QOpenGLFramebufferObject@@QEBAHXZ");
    void** s_fbo_height = FindIATSlot(front, "?height@QOpenGLFramebufferObject@@QEBAHXZ");
    void** s_fbo_blit   = FindIATSlot(front, "?blitFramebuffer@QOpenGLFramebufferObject@@SAXPEAV1@AEBVQRect@@01II@Z");

    void**   s_vp     = nullptr;
    void**   s_sc     = nullptr;
    uint8_t* p_agl_vp = nullptr;
    uint8_t* p_agl_sc = nullptr;
    if (is_modo16) {
        s_vp = FindIATSlot(frame, "?AGLViewport@@YAXHHHH@Z");
        s_sc = FindIATSlot(frame, "?AGLScissor@@YAXHHHH@Z");
    } else {
        // In Modo 17 (merged modo.exe), AGLViewport and AGLScissor are internal 14-byte
        // dispatch thunks: mov rax, [rip+disp32]; jmp qword ptr [rax + 0xF0 / 0x460].
        static const uint8_t pat_vp[14] = { 0x48, 0x8B, 0x05, 0, 0, 0, 0, 0x48, 0xFF, 0xA0, 0xF0, 0x00, 0x00, 0x00 };
        static const uint8_t pat_sc[14] = { 0x48, 0x8B, 0x05, 0, 0, 0, 0, 0x48, 0xFF, 0xA0, 0x60, 0x04, 0x00, 0x00 };
        p_agl_vp = FindCodePattern(front, 0xD45F60, pat_vp, "xxx????xxxxxxx", 14);
        p_agl_sc = FindCodePattern(front, 0xD370A0, pat_sc, "xxx????xxxxxxx", 14);
    }

    const bool has_agl = is_modo16 ? (s_vp && s_sc) : (p_agl_vp && p_agl_sc);
    if (!s_fbo_ctor || !s_fbo_dtor || !s_fbo_width || !s_fbo_height || !s_fbo_blit || !has_agl) {
        return;
    }

    g_main_tid         = GetCurrentThreadId();
    g_front_text_begin = text_begin;
    g_front_text_end   = text_end;

    PatchSlot(s_fbo_ctor,   &HookFboCtor,   &g_orig_fbo_ctor);
    PatchSlot(s_fbo_dtor,   &HookFboDtor,   &g_orig_fbo_dtor);
    PatchSlot(s_fbo_width,  &HookFboWidth,  &g_orig_fbo_width);
    PatchSlot(s_fbo_height, &HookFboHeight, &g_orig_fbo_height);
    PatchSlot(s_fbo_blit,   &HookFboBlit,   &g_orig_fbo_blit);
    if (is_modo16) {
        PatchSlot(s_vp, &HookViewport, &g_orig_viewport);
        PatchSlot(s_sc, &HookScissor,  &g_orig_scissor);
    } else {
        int32_t disp = *reinterpret_cast<int32_t*>(p_agl_vp + 3);
        g_agl_table = reinterpret_cast<void***>(p_agl_vp + 7 + disp);
        PatchJump14(p_agl_vp, &HookViewport);
        PatchJump14(p_agl_sc, &HookScissor);
    }

    // 2. 2D UI (OPALDisp) IAT hooks
    if (g_qimg_set_dpr && g_qimg_get_dpr && g_qpd_get_dpr) {
        PatchIAT(front, "??0QImage@@QEAA@AEBVQSize@@W4Format@0@@Z",                                                   &HookQImageCtorSize,       &g_orig_qimg_ctor_size);
        PatchIAT(front, "??0QImage@@QEAA@HHW4Format@0@@Z",                                                            &HookQImageCtorWH,         &g_orig_qimg_ctor_wh);
        PatchIAT(front, "?size@QImage@@QEBA?AVQSize@@XZ",                                                             &HookQImageSize,           &g_orig_qimg_size);
        PatchIAT(front, "?width@QImage@@QEBAHXZ",                                                                     &HookQImageWidth,          &g_orig_qimg_width);
        PatchIAT(front, "?height@QImage@@QEBAHXZ",                                                                    &HookQImageHeight,         &g_orig_qimg_height);
        PatchIAT(front, "?width@QPaintDevice@@QEBAHXZ",                                                               &HookQPaintDeviceWidth,    &g_orig_qpd_width);
        PatchIAT(front, "?height@QPaintDevice@@QEBAHXZ",                                                              &HookQPaintDeviceHeight,   &g_orig_qpd_height);
        PatchIAT(front, "?copy@QImage@@QEBA?AV1@HHHH@Z",                                                              &HookQImageCopy,           &g_orig_qimg_copy);
        PatchIAT(front, "?pixel@QImage@@QEBAIHH@Z",                                                                   &HookQImagePixel,          &g_orig_qimg_pixel);
        PatchIAT(front, "?drawImage@QPainter@@QEAAXAEBVQRect@@AEBVQImage@@0V?$QFlags@W4ImageConversionFlag@Qt@@@@@Z", &HookDrawImageRectRect,    &g_orig_draw_image_rr);
        PatchIAT(front, "?drawImage@QPainter@@QEAAXHHAEBVQImage@@HHHHV?$QFlags@W4ImageConversionFlag@Qt@@@@@Z",       &HookDrawImageXY,          &g_orig_draw_image_xy);
        PatchIAT(front, "?drawRects@QPainter@@QEAAXPEBVQRect@@H@Z",                                                   &HookDrawRects,            &g_orig_draw_rects);
        PatchIAT(front, "?drawLines@QPainter@@QEAAXPEBVQLine@@H@Z",                                                   &HookDrawLines,            &g_orig_draw_lines);
        PatchIAT(front, "?drawPoints@QPainter@@QEAAXPEBVQPoint@@H@Z",                                                 &HookDrawPoints,           &g_orig_draw_points);
        PatchIAT(front, "?drawLine@QPainter@@QEAAXHHHH@Z",                                                            &HookDrawLine4,            &g_orig_draw_line4);
        PatchIAT(front, "?drawPoint@QPainter@@QEAAXHH@Z",                                                             &HookDrawPoint2,           &g_orig_draw_point2);
        PatchIAT(front, "?setWindow@QPainter@@QEAAXHHHH@Z",                                                           &HookPainterSetWindow,     &g_orig_set_window);
        PatchIAT(front, "?setStyleStrategy@QFont@@QEAAXW4StyleStrategy@1@@Z",                                         &HookFontSetStyleStrategy, &g_orig_font_style);
        PatchIAT(front, "?setPointSize@QFont@@QEAAXH@Z",                                                              &HookFontSetPointSize,     &g_orig_font_pt_size);
    }

    // Modo 16 fractional High-DPI (125% / 150%) truncation fixes:
    // Modo 16 calls Qt5's integer QPaintDevice::devicePixelRatio() (which truncates
    // 1.25/1.5 -> 1) in WPaneGLGetPixelRatio and GnzUI's Advanced Viewport resizeGL,
    // whereas Modo 17 already uses Qt6's double overload.
    if (is_modo16 && g_qpd_get_dpr) {
        static const uint8_t pat_wp_dpr[25] = {
            0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x91, 0xD8, 0x00, 0x00, 0x00,
            0x48, 0x8B, 0x42, 0x10, 0x48, 0x85, 0xC0, 0x74, 0x0D, 0x8B, 0x40,
            0x04, 0x85, 0xC0
        };
        uint8_t* p_wp_dpr = FindCodePattern(front, 0x1B8CB0, pat_wp_dpr, nullptr, sizeof(pat_wp_dpr));
        if (p_wp_dpr) {
            PatchJump14(p_wp_dpr, &HookWPaneGLGetPixelRatio);
        }

        // Cross-register return ABI patch for GnzUI.dll (Advanced Viewport resizeGL):
        // GnzUI imports `int QPaintDevice::devicePixelRatio()` (returned in EAX) with a single
        // call at +0x22D8, whereas `double QPaintDevice::devicePixelRatioF()` returns in XMM0.
        // XMM0 stays untouched from +0x22DE through +0x22F6 (`p_gnz + 4`), where we replace
        // `movd xmm4, eax; cvtdq2pd xmm4, xmm4` (8 bytes) with `movsd xmm4, xmm0; nop` BEFORE
        // redirecting the IAT slot so EAX is never read uninitialized.
        auto* gnzui = reinterpret_cast<uint8_t*>(GetModuleHandleA("GnzUI.dll"));
        if (gnzui) {
            void** s_gnz_dpr = FindIATSlot(gnzui, "?devicePixelRatio@QPaintDevice@@QEBAHXZ");
            static const uint8_t pat_gnz[16] = {
                0xF3, 0x0F, 0xE6, 0xD2, 0x66, 0x0F, 0x6E, 0xE0,
                0xF3, 0x0F, 0xE6, 0xE4, 0xF2, 0x0F, 0x59, 0xD4
            };
            uint8_t* p_gnz = FindCodePattern(gnzui, 0x22F2, pat_gnz, nullptr, sizeof(pat_gnz));
            if (s_gnz_dpr && p_gnz) {
                static const uint8_t mov_xmm4_xmm0[8] = {
                    0xF2, 0x0F, 0x10, 0xE0, 0x0F, 0x1F, 0x40, 0x00
                };
                if (PatchCodeBytes(p_gnz + 4, mov_xmm4_xmm0, sizeof(mov_xmm4_xmm0))) {
                    PatchSlot(s_gnz_dpr, g_qpd_get_dpr);
                }
            }
        }
    }

    // Locate rotated vertical tab strip ORastCreateImage call sites (90 deg CCW and CW)
    // and their .pdata RUNTIME_FUNCTION end bounds once at startup.
    static const uint8_t pat_ccw[31] = {
        0x48, 0x8B, 0x8C, 0x24, 0xA0, 0x00, 0x00, 0x00, 0xE8, 0, 0, 0, 0,
        0x4C, 0x8B, 0xC0, 0x48, 0x8D, 0x15, 0, 0, 0, 0,
        0xB9, 0x01, 0x00, 0x00, 0x00, 0x4C, 0x8B, 0xF0
    };
    static const uint8_t pat_cw[17] = {
        0xE8, 0, 0, 0, 0, 0x44, 0x8B, 0x43, 0x50, 0xB9, 0x04, 0x00, 0x00, 0x00, 0x8B, 0x53, 0x4C
    };
    uint8_t* p_ccw = FindCodePattern(front, is_modo16 ? 0x1F6C05 : 0xD0EE45, pat_ccw, "xxxxxxxxx????xxxxxx????xxxxxxxx", sizeof(pat_ccw));
    uint8_t* p_cw  = FindCodePattern(front, is_modo16 ? 0x19733C : 0xCF0683, pat_cw,  "x????xxxxxxxxxxxx", sizeof(pat_cw));
    if (p_ccw) {
        g_ret_rot_ccw = p_ccw + 13;
        g_rot_ccw_end = GetFunctionEndOrFallback(g_ret_rot_ccw);
    }
    if (p_cw) {
        g_ret_rot_cw  = p_cw + 5;
        g_rot_cw_end  = GetFunctionEndOrFallback(g_ret_rot_cw);
    }

    // Prevent LUXQt::LUXView::paintEvent (Modo 16 +0x1724A0, Modo 17 +0xD70770) from synchronously
    // calling FireLazyHandlers(3, nullptr) (call at Modo 16 +0x1724C3, Modo 17 +0xD70791) before
    // setting m_inPaintEvent = 1. Under High-DPI, if a lazy form layout handler resizes a widget
    // mid-paint, Qt reallocates the window's QBackingStore QImage and frees the QPaintEngine*
    // cached on the stack by QWidgetPrivate::drawWidget, causing a Use-After-Free Access Violation.
    static const uint8_t pat_lazy16[15] = { 0x33, 0xD2, 0x8D, 0x4A, 0x03, 0xE8, 0, 0, 0, 0, 0x41, 0xC6, 0x46, 0x40, 0x01 };
    static const uint8_t pat_lazy17[14] = { 0x33, 0xD2, 0x8D, 0x4A, 0x03, 0xE8, 0, 0, 0, 0, 0xC6, 0x47, 0x38, 0x01 };
    uint8_t* p_lazy = is_modo16
        ? FindCodePattern(front, 0x1724BE, pat_lazy16, "xxxxxx????xxxxx", sizeof(pat_lazy16))
        : FindCodePattern(front, 0xD7078C, pat_lazy17, "xxxxxx????xxxx", sizeof(pat_lazy17));
    if (p_lazy) {
        static const uint8_t kNop5[5] = { 0x90, 0x90, 0x90, 0x90, 0x90 };
        PatchCodeBytes(p_lazy + 5, kNop5, sizeof(kNop5));
    }

    g_hooked = true;
}

extern "C" __declspec(dllexport) void SetHiDPIScale(double scale) {
    if (scale >= 1.0 && scale <= 4.0) {
        g_scale = scale;
    }
    if (g_scale > 1.001) {
        InstallAllHooks();
    }
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    (void)hinstDLL;
    if (fdwReason == DLL_PROCESS_DETACH && lpvReserved == nullptr && g_rot_buf != nullptr) {
        VirtualFree(g_rot_buf, 0, MEM_RELEASE);
        g_rot_buf = nullptr;
        g_rot_cap = 0;
    }
    return TRUE;
}
