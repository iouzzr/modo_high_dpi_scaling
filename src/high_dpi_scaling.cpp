// Copyright 2026 iouzzr
// SPDX-License-Identifier: Apache-2.0

// high_dpi_scaling.cpp - High-DPI 3D Viewport, 2D UI (OPALDisp), Viewport Capture,
// Render/Preview/ClipView 1:1 & Per-Monitor V2 Patch DLL for Foundry Modo (16.x & 17.x).
// Reference RVAs correspond to Modo 16.1v9 / 17.1v1; inline patches fall back to
// scanning .text via FindCodePattern.

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

struct DpiPairC {
    double first;
    double second;
};

// Minimal ntdll DLL load notification definitions
struct UnicodeStringNtdll {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
};

struct LdrDllLoadedNotificationData {
    ULONG                     Flags;
    const UnicodeStringNtdll* FullDllName;
    const UnicodeStringNtdll* BaseDllName;
    PVOID                     DllBase;
    ULONG                     SizeOfImage;
};

union LdrDllNotificationData {
    LdrDllLoadedNotificationData Loaded;
};

typedef VOID (CALLBACK *FnLdrDllNotification)(
    ULONG NotificationReason,
    const LdrDllNotificationData* NotificationData,
    PVOID Context
);
typedef LONG (NTAPI *FnLdrRegisterDllNotification)(
    ULONG Flags,
    FnLdrDllNotification NotificationFunction,
    PVOID Context,
    PVOID* Cookie
);
typedef LONG (NTAPI *FnLdrUnregisterDllNotification)(PVOID Cookie);
typedef BOOL (WINAPI *FnK32EnumProcessModules)(
    HANDLE hProcess, HMODULE* lphModule, DWORD cb, LPDWORD lpcbNeeded
);
typedef HANDLE (WINAPI *FnSetThreadDpiAwarenessContext)(HANDLE dpiContext);

// 3D OpenGL Viewport & Qt OpenGL context function pointer types
typedef void* (*FnFboFmtCtor)(void* this_ptr);
typedef void  (*FnFboFmtDtor)(void* this_ptr);
typedef void* (*FnFboCtor)(void* this_ptr, int w, int h, const void* fmt_ptr);
typedef void  (*FnFboDtor)(void* this_ptr);
typedef bool  (*FnFboBind)(void* this_ptr);
typedef int   (*FnDim)(const void* this_ptr);
typedef void  (*FnFboBlit)(void* target, const QRectC* targetRect, void* source, const QRectC* sourceRect, uint32_t buffers, uint32_t filter);
typedef void  (*FnRect4)(int x, int y, int w, int h);
typedef void* (*FnQGLCurrentContext)();
typedef void* (*FnQGLSurface)(const void* ctx);
typedef int   (*FnQSurfaceClass)(const void* surf);

// 2D UI (OPALDisp / QImage / QPaintDevice / QPainter / QFont / QApplication / QWidget) types
typedef void*          (*FnQImageCtorSize)(void* this_ptr, const QSizeC* sz, int fmt);
typedef void*          (*FnQImageCtorWH)(void* this_ptr, int w, int h, int fmt);
typedef void*          (*FnQImageCopyCtor)(void* dst_qimg, const void* src_qimg);
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
typedef DpiPairC*      (*FnOverrideDpi)(DpiPairC* out_pair, const DpiPairC* in_pair);

// QApplication / QObject / QWidget / QLabel / QPixmap / QScrollArea types
typedef bool           (*FnAppNotify)(void* app, void* receiver, void* ev);
typedef const char*    (*FnMetaClassName)(const void* meta_obj);
typedef void           (*FnWidgetUpdate)(void* widget);
typedef void*          (*FnWidgetParent)(const void* widget);
typedef QSizeC*        (*FnWidgetSize)(const void* widget, QSizeC* out_sz);
typedef void*          (*FnScrollAreaWidget)(const void* scroll_area);
typedef void*          (*FnQPixmapCopyCtor)(void* dst_pixmap, const void* src_pixmap);
typedef void           (*FnQPixmapDtor)(void* pixmap);
typedef void           (*FnLabelSetPixmap)(void* label, const void* pixmap);

// Modo internal function pointer types
typedef void           (*FnFireLazyHandlers)(int code, void* arg);
typedef void           (*FnLUXWidgetMethod)(void* widget);
typedef void           (*FnWPanePos)(void* pane, int* x, int* y, int* w, int* h);
typedef int            (*FnWPaneGLMakeCurrent)(void* pane);
typedef void           (*FnAGLCaptureView)(void* agl_view, void* out_buf);
typedef float          (*FnIViewComputeZoom)(void* iview);
typedef void           (*FnIViewGetAutoSize)(void* iview, int* out_w, int* out_h);
typedef int            (*FnIViewCheckAutoSize)(void* iview);
typedef void           (*FnRendWinPaint)(void* pane, void* disp_ref, int flags);
typedef int            (*FnRendWinUpdateImage)(void* a1, void* a2, void* a3, void* a4);
typedef void           (*FnRendWinSetZoom)(double zoom);
typedef int64_t        (*FnRendWinMouseEvent)(void* a1, void* a2, void* a3, void* a4);
typedef int            (*FnRendWinScrollbarGet)(void* a1, int a2, int a3, void* a4, void* a5, void* a6, void* a7, void* a8);
typedef int            (*FnRendWinGetScaleAndOffset)(void* a1, void* a2, void* a3, void* a4);
typedef int64_t        (*FnRendWinCenterAtNormalized)(double cx, double cy);
typedef void           (*FnRendWinPaneToNormalized)(int x, int y, void* out_nx, void* out_ny);
typedef int            (*FnRendWinNormalizedToPane)(double nx, double ny, void* out_x, void* out_y);
typedef int            (*FnImageGetSize)(void* img, int* out_w, int* out_h);
typedef void           (*FnRendWinBlitVignette)(void* orast, void* img, int ox, int oy, int pane_w, int pane_h);
typedef void           (*FnRendWinWipeBlit)(void* orast, void* slot_b, int ox, int oy, int pane_w, int pane_h);

static FnFboFmtCtor               g_fbo_fmt_ctor              = nullptr;
static FnFboFmtDtor               g_fbo_fmt_dtor              = nullptr;
static FnFboCtor                  g_orig_fbo_ctor             = nullptr;
static FnFboDtor                  g_orig_fbo_dtor             = nullptr;
static FnFboBind                  g_fbo_bind                  = nullptr;
static FnDim                      g_orig_fbo_width            = nullptr;
static FnDim                      g_orig_fbo_height           = nullptr;
static FnFboBlit                  g_orig_fbo_blit             = nullptr;
static FnRect4                    g_orig_viewport             = nullptr;
static FnRect4                    g_orig_scissor              = nullptr;
static void***                    g_agl_table                 = nullptr;
static FnQGLCurrentContext        g_qgl_current_context       = nullptr;
static FnQGLSurface               g_qgl_surface               = nullptr;
static FnQSurfaceClass            g_qsurf_class               = nullptr;
static FnGetDpr                   g_qwin_get_dpr              = nullptr;

static FnQImageCtorSize           g_orig_qimg_ctor_size       = nullptr;
static FnQImageCtorWH             g_orig_qimg_ctor_wh         = nullptr;
static FnQImageCopyCtor           g_qimg_copy_ctor            = nullptr;
static FnQImageDtor               g_qimg_dtor                 = nullptr;
static FnQImageScanLine           g_qimg_scanline             = nullptr;
static FnQImageSize               g_orig_qimg_size            = nullptr;
static FnDim                      g_orig_qimg_width           = nullptr;
static FnDim                      g_orig_qimg_height          = nullptr;
static FnDim                      g_orig_qpd_width            = nullptr;
static FnDim                      g_orig_qpd_height           = nullptr;
static FnQImageCopy               g_orig_qimg_copy            = nullptr;
static FnQImagePixel              g_orig_qimg_pixel           = nullptr;
static FnDrawImageRectRect        g_orig_draw_image_rr        = nullptr;
static FnDrawImageXY              g_orig_draw_image_xy        = nullptr;
static FnDrawRects                g_orig_draw_rects           = nullptr;
static FnDrawLines                g_orig_draw_lines           = nullptr;
static FnDrawPoints               g_orig_draw_points          = nullptr;
static FnDrawLine4                g_orig_draw_line4           = nullptr;
static FnDrawPoint2               g_orig_draw_point2          = nullptr;
static FnFillRectBrush            g_painter_fill_rect         = nullptr;
static FnPainterBrush             g_painter_brush             = nullptr;
static FnPainterSetWindow         g_orig_set_window           = nullptr;
static FnPainterDevice            g_painter_device            = nullptr;
static FnFontSetStyleStrategy     g_orig_font_style           = nullptr;
static FnFontSetPointSize         g_orig_font_pt_size         = nullptr;
static FnFontSetPointSizeF        g_qfont_set_pt_f            = nullptr;
static FnFontPointSizeF           g_qfont_get_pt_f            = nullptr;

static FnSetDpr                   g_qimg_set_dpr              = nullptr;
static FnGetDpr                   g_qimg_get_dpr              = nullptr;
static FnGetDpr                   g_qpd_get_dpr               = nullptr;

static FnAppNotify                g_orig_app_notify           = nullptr;
static FnMetaClassName            g_qmeta_class_name          = nullptr;
static FnWidgetUpdate             g_qwidget_update            = nullptr;
static FnWidgetParent             g_qwidget_parent            = nullptr;
static FnWidgetSize               g_orig_lx_qwidget_size      = nullptr;
static FnScrollAreaWidget         g_qscrollarea_widget        = nullptr;
static FnQPixmapCopyCtor          g_qpixmap_copy_ctor         = nullptr;
static FnQPixmapDtor              g_qpixmap_dtor              = nullptr;
static FnGetDpr                   g_qpixmap_get_dpr           = nullptr;
static FnSetDpr                   g_qpixmap_set_dpr           = nullptr;
static FnLabelSetPixmap           g_orig_lx_label_set_pixmap  = nullptr;

static FnFireLazyHandlers             g_fire_lazy_handlers        = nullptr;
static FnLUXWidgetMethod              g_orig_luxview_update_image = nullptr;
static FnLUXWidgetMethod              g_luxgl_screen_changed      = nullptr;
static FnWPanePos                     g_orig_wpane_get_pos        = nullptr;
static FnWPaneGLMakeCurrent           g_wpane_gl_make_current     = nullptr;
static FnAGLCaptureView               g_orig_agl_capture_view     = nullptr;
static FnIViewComputeZoom             g_orig_iview_compute_zoom   = nullptr;
static FnIViewGetAutoSize             g_orig_iview_get_auto_size  = nullptr;
static FnIViewCheckAutoSize           g_orig_iview_chk_auto_size  = nullptr;
static FnRendWinPaint                 g_orig_rendwin_paint        = nullptr;
static FnRendWinUpdateImage           g_orig_rw_update_image      = nullptr;
static FnRendWinSetZoom               g_orig_rw_set_zoom          = nullptr;
static FnRendWinMouseEvent            g_orig_rw_mouse_event       = nullptr;
static FnRendWinScrollbarGet          g_orig_rw_scrollbar_get     = nullptr;
static FnRendWinGetScaleAndOffset     g_orig_rw_get_scale_offset  = nullptr;
static FnRendWinCenterAtNormalized    g_orig_rw_center_at_norm    = nullptr;
static FnRendWinPaneToNormalized      g_orig_rw_pane_to_norm      = nullptr;
static FnRendWinNormalizedToPane      g_orig_rw_norm_to_pane      = nullptr;
static FnImageGetSize                 g_orig_image_get_size       = nullptr;
static FnRendWinBlitVignette          g_orig_rw_blit_vignette     = nullptr;
static FnRendWinWipeBlit              g_orig_rw_wipe_blit         = nullptr;

static double*                    g_rendwin_zoom              = nullptr;
static void**                     g_rendwin_pane_ptr          = nullptr;
static int*                       g_rw_wipe_mode              = nullptr;
static int*                       g_rw_spotlight_mx           = nullptr;
static int*                       g_rw_spotlight_my           = nullptr;
static int*                       g_rw_spotlight_radius       = nullptr;
static const uint8_t*             g_rw_upd_begin              = nullptr;
static const uint8_t*             g_rw_upd_end                = nullptr;

static const uint8_t*             g_front_text_begin          = nullptr;
static const uint8_t*             g_front_text_end            = nullptr;
static const uint8_t*             g_nexus_text_begin          = nullptr;
static const uint8_t*             g_nexus_text_end            = nullptr;
static const uint8_t*             g_ret_rot_ccw               = nullptr;
static const uint8_t*             g_rot_ccw_end               = nullptr;
static const uint8_t*             g_ret_rot_cw                = nullptr;
static const uint8_t*             g_rot_cw_end                = nullptr;

static double                     g_scale                     = 1.0;
static bool                       g_pmv2_mode                 = false;
static bool                       g_is_modo16                 = false;
static DWORD                      g_main_tid                  = 0;
static bool                       g_hooked                    = false;
static double                     g_active_ui_dpr             = 1.0;
static double                     g_last_gl_dpr               = 1.0;
static const void*                g_force_recreate_qimg       = nullptr;
static const void*                g_force_recreate_fbo        = nullptr;
static int                        g_paint_tx_depth            = 0;
static PVOID                      g_ldr_cookie                = nullptr;

// Main-thread state for IView and F9 RendWin rendering hooks
static const void*                g_iview_scaled_ptr          = nullptr;
static bool                       g_in_rendwin_paint          = false;
static double                     g_rendwin_paint_dpr         = 1.0;
static double                     g_rendwin_last_dpr          = 1.0;
static bool                       g_rendwin_has_dpr           = false;
static int                        g_rw_zoom_depth             = 0;
static double                     g_rw_phys_zoom              = 1.0;
static double                     g_rw_active_dpr             = 1.0;

// High-DPI buffer for rotated vertical tab strips (ORastCreateImage -> 90 deg rotation -> DispDrawImage)
static uint32_t*                  g_rot_buf                   = nullptr;
static size_t                     g_rot_cap                   = 0;
static int                        g_rot_lw                    = 0;
static int                        g_rot_lh                    = 0;
static int                        g_rot_pw                    = 0;
static int                        g_rot_ph                    = 0;
static double                     g_rot_dpr                   = 1.0;
static uint64_t                   g_rot_tick                  = 0;
static const uint8_t*             g_rot_expected_site         = nullptr;
static const uint8_t*             g_rot_expected_end          = nullptr;
static bool                       g_rot_valid                 = false;

static double AbsDouble(double v) {
    return (v < 0.0) ? -v : v;
}

static bool IsHighDpiActive() {
    return g_pmv2_mode || g_scale > 1.001;
}

static bool IsInTextRange(const uint8_t* addr, const uint8_t* begin, const uint8_t* end, size_t pre_bytes = 0, size_t post_bytes = 16) {
    if (!addr || !begin || !end) return false;
    auto u_addr  = reinterpret_cast<uintptr_t>(addr);
    auto u_begin = reinterpret_cast<uintptr_t>(begin);
    auto u_end   = reinterpret_cast<uintptr_t>(end);
    if (u_addr < u_begin + pre_bytes) return false;
    if (u_addr > u_end || u_end - u_addr < post_bytes) return false;
    return true;
}

static bool IsInFrontText(const uint8_t* addr, size_t pre_bytes = 0, size_t post_bytes = 16) {
    return IsInTextRange(addr, g_front_text_begin, g_front_text_end, pre_bytes, post_bytes);
}

static bool IsInNexusText(const uint8_t* addr, size_t pre_bytes = 0, size_t post_bytes = 16) {
    return IsInTextRange(addr, g_nexus_text_begin, g_nexus_text_end, pre_bytes, post_bytes);
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

template <size_t N>
static bool MatchNexusReturnBytes(const void* ret_addr, const uint8_t (&pat)[N]) {
    const auto* ret = static_cast<const uint8_t*>(ret_addr);
    return IsInNexusText(ret, 0, N) && MatchPattern(ret, pat, nullptr, N);
}

template <size_t N, size_t M>
static bool MatchNexusReturnPattern(const void* ret_addr, const uint8_t (&pat)[N], const char (&mask)[M]) {
    static_assert(M == N + 1, "Mask length must match pattern length");
    const auto* ret = static_cast<const uint8_t*>(ret_addr);
    return IsInNexusText(ret, 0, N) && MatchPattern(ret, pat, mask, N);
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

static double GetImageDpr(const void* img) {
    if (!img || !g_qimg_get_dpr) return 1.0;
    return g_qimg_get_dpr(img);
}

static double GetPaintDeviceDpr(const void* dev) {
    if (!dev || !g_qpd_get_dpr) return 1.0;
    return g_qpd_get_dpr(dev);
}

// QWidget multiple inheritance in both Qt5 and Qt6:
// QObject occupies +0x00 (0x10 bytes), QPaintDevice occupies +0x10.
static double GetWidgetDpr(const void* widget) {
    if (!g_pmv2_mode) return g_scale;
    if (!widget || !g_qpd_get_dpr) return (g_active_ui_dpr >= 1.0) ? g_active_ui_dpr : g_scale;
    double dpr = g_qpd_get_dpr(reinterpret_cast<const uint8_t*>(widget) + 0x10);
    if (dpr >= 1.0 && dpr <= 4.0) return dpr;
    return (g_active_ui_dpr >= 1.0) ? g_active_ui_dpr : g_scale;
}

// QWindow multiple inheritance in both Qt5 and Qt6:
// QObject occupies +0x00 (0x10 bytes), QSurface occupies +0x10.
// Verify QSurface::surfaceClass() == QSurface::Window (0) before subtracting 0x10.
static double GetCurrentGLDpr() {
    if (!g_pmv2_mode) return g_scale;
    if (g_qgl_current_context && g_qgl_surface && g_qsurf_class && g_qwin_get_dpr) {
        void* ctx = g_qgl_current_context();
        if (ctx) {
            void* surf = g_qgl_surface(ctx);
            if (surf && g_qsurf_class(surf) == 0) {
                const uint8_t* qwin = reinterpret_cast<const uint8_t*>(surf) - 0x10;
                double dpr = g_qwin_get_dpr(qwin);
                if (dpr >= 1.0 && dpr <= 4.0) {
                    g_last_gl_dpr = dpr;
                    return dpr;
                }
            }
        }
    }
    if (g_active_ui_dpr >= 1.0 && g_active_ui_dpr <= 4.0) return g_active_ui_dpr;
    if (g_last_gl_dpr >= 1.0 && g_last_gl_dpr <= 4.0) return g_last_gl_dpr;
    return g_scale;
}

static double GetPaneDpr(const uint8_t* pane) {
    if (!g_pmv2_mode) return g_scale;
    if (pane && g_qpd_get_dpr) {
        const uint8_t* sub = *reinterpret_cast<const uint8_t* const*>(pane + 0xD8);
        if (sub) {
            const uint8_t* info = *reinterpret_cast<const uint8_t* const*>(sub + 0x10);
            if (info && *reinterpret_cast<const int*>(info + 4) != 0) {
                const uint8_t* glw = *reinterpret_cast<const uint8_t* const*>(sub + 0x18);
                if (glw) {
                    double dpr = g_qpd_get_dpr(glw + 0x10);
                    if (dpr >= 1.0 && dpr <= 4.0) return dpr;
                }
            }
        }
    }
    return g_pmv2_mode ? ((g_active_ui_dpr >= 1.0) ? g_active_ui_dpr : g_scale) : g_scale;
}

// ============================================================================
// Executable Trampoline & Codecave Arena (W^X: RX at rest, temporary RWX write)
// ============================================================================

static uint8_t* g_exec_arena = nullptr;
static size_t   g_exec_used  = 0;
static size_t   g_exec_cap   = 0;

static uint8_t* AllocExecStub(const uint8_t* code, size_t len) {
    size_t aligned = (len + 15u) & ~static_cast<size_t>(15u);
    if (!g_exec_arena || g_exec_used + aligned > g_exec_cap) {
        size_t alloc_sz = 4096;
        if (aligned > alloc_sz) alloc_sz = (aligned + 4095u) & ~static_cast<size_t>(4095u);
        auto* page = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, alloc_sz, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READ));
        if (!page) return nullptr;
        g_exec_arena = page;
        g_exec_used  = 0;
        g_exec_cap   = alloc_sz;
    }
    uint8_t* dst = g_exec_arena + g_exec_used;
    DWORD old_prot = 0;
    if (!VirtualProtect(dst, aligned, PAGE_EXECUTE_READWRITE, &old_prot)) {
        return nullptr;
    }
    for (size_t i = 0; i < len; ++i) {
        dst[i] = code[i];
    }
    for (size_t i = len; i < aligned; ++i) {
        dst[i] = 0xCC;
    }
    VirtualProtect(dst, aligned, PAGE_EXECUTE_READ, &old_prot);
    FlushInstructionCache(GetCurrentProcess(), dst, aligned);
    g_exec_used += aligned;
    return dst;
}

// Build a trampoline for a clean prologue (zero RIP-relative instructions in first copy_len bytes).
template <typename Fn>
static Fn BuildCleanTrampoline(uint8_t* fn_addr, size_t copy_len) {
    if (!fn_addr || copy_len < 14 || copy_len > 32) return nullptr;
    uint8_t buf[64] = {};
    for (size_t i = 0; i < copy_len; ++i) {
        buf[i] = fn_addr[i];
    }
    buf[copy_len + 0] = 0xFF;
    buf[copy_len + 1] = 0x25;
    *reinterpret_cast<uint32_t*>(buf + copy_len + 2) = 0;
    *reinterpret_cast<uintptr_t*>(buf + copy_len + 6) = reinterpret_cast<uintptr_t>(fn_addr + copy_len);
    return reinterpret_cast<Fn>(AllocExecStub(buf, copy_len + 14));
}

// Build a trampoline for prologues of the form:
//   +0..+9 : 10 clean register-push / sub rsp bytes
//   +10..+16: 48 8B 05 <disp32> (mov rax, [rip + disp32] loading __security_cookie)
template <typename Fn>
static Fn BuildSecurityCookieTrampoline(uint8_t* fn_addr) {
    if (!fn_addr || fn_addr[10] != 0x48 || fn_addr[11] != 0x8B || fn_addr[12] != 0x05) {
        return nullptr;
    }
    int32_t disp = *reinterpret_cast<int32_t*>(fn_addr + 13);
    uintptr_t cookie_addr = reinterpret_cast<uintptr_t>(fn_addr + 17 + disp);
    uint8_t buf[48] = {};
    for (size_t i = 0; i < 10; ++i) {
        buf[i] = fn_addr[i];
    }
    // movabs rax, cookie_addr; mov rax, [rax]
    buf[10] = 0x48;
    buf[11] = 0xB8;
    *reinterpret_cast<uintptr_t*>(buf + 12) = cookie_addr;
    buf[20] = 0x48;
    buf[21] = 0x8B;
    buf[22] = 0x00;
    // jmp qword ptr [rip + 0] -> fn_addr + 17
    buf[23] = 0xFF;
    buf[24] = 0x25;
    *reinterpret_cast<uint32_t*>(buf + 25) = 0;
    *reinterpret_cast<uintptr_t*>(buf + 29) = reinterpret_cast<uintptr_t>(fn_addr + 17);
    return reinterpret_cast<Fn>(AllocExecStub(buf, 37));
}

// Build a trampoline for prologues of the form:
//   +0..+8 : 9 clean register-push / sub rsp bytes
//   +9..+16: 48 83 3D <disp32> 00 (cmp qword ptr [rip + disp32], 0)
template <typename Fn>
static Fn BuildRipCmpZeroTrampoline(uint8_t* fn_addr) {
    if (!fn_addr || fn_addr[9] != 0x48 || fn_addr[10] != 0x83 ||
        fn_addr[11] != 0x3D || fn_addr[16] != 0x00) {
        return nullptr;
    }
    int32_t disp = *reinterpret_cast<int32_t*>(fn_addr + 12);
    uintptr_t global_addr = reinterpret_cast<uintptr_t>(fn_addr + 17 + disp);
    uint8_t buf[48] = {};
    for (size_t i = 0; i < 9; ++i) {
        buf[i] = fn_addr[i];
    }
    // movabs rax, global_addr; cmp qword ptr [rax], 0
    buf[9]  = 0x48;
    buf[10] = 0xB8;
    *reinterpret_cast<uintptr_t*>(buf + 11) = global_addr;
    buf[19] = 0x48;
    buf[20] = 0x83;
    buf[21] = 0x38;
    buf[22] = 0x00;
    // jmp qword ptr [rip + 0] -> fn_addr + 17
    buf[23] = 0xFF;
    buf[24] = 0x25;
    *reinterpret_cast<uint32_t*>(buf + 25) = 0;
    *reinterpret_cast<uintptr_t*>(buf + 29) = reinterpret_cast<uintptr_t>(fn_addr + 17);
    return reinterpret_cast<Fn>(AllocExecStub(buf, 37));
}

static uint8_t* g_near_arena = nullptr;
static size_t   g_near_used  = 0;
static size_t   g_near_cap   = 0;

static bool IsWithinRel32(const uint8_t* next_ip, uintptr_t dst) {
    intptr_t diff = static_cast<intptr_t>(dst) - reinterpret_cast<intptr_t>(next_ip);
    return diff >= -2147483648LL && diff <= 2147483647LL;
}

static uint8_t* AllocNearExecStub(const uint8_t* near_addr, const uint8_t* code, size_t len) {
    size_t aligned = (len + 15u) & ~static_cast<size_t>(15u);
    if (g_near_arena && g_near_used + aligned <= g_near_cap &&
        IsWithinRel32(near_addr + 5, reinterpret_cast<uintptr_t>(g_near_arena + g_near_used))) {
        uint8_t* dst = g_near_arena + g_near_used;
        DWORD old_prot = 0;
        if (!VirtualProtect(dst, aligned, PAGE_EXECUTE_READWRITE, &old_prot)) return nullptr;
        for (size_t i = 0; i < len; ++i) dst[i] = code[i];
        for (size_t i = len; i < aligned; ++i) dst[i] = 0xCC;
        VirtualProtect(dst, aligned, PAGE_EXECUTE_READ, &old_prot);
        FlushInstructionCache(GetCurrentProcess(), dst, aligned);
        g_near_used += aligned;
        return dst;
    }

    uintptr_t origin = reinterpret_cast<uintptr_t>(near_addr) & ~static_cast<uintptr_t>(0xFFFFu);
    static constexpr uintptr_t kMaxSpan = 0x70000000ULL;
    uint8_t* page = nullptr;
    for (uintptr_t step = 0x10000ULL; step <= kMaxSpan && !page; step += 0x10000ULL) {
        uintptr_t candidates[2] = {
            (origin > step + 0x10000ULL) ? (origin - step) : 0,
            origin + step
        };
        for (int ci = 0; ci < 2; ++ci) {
            uintptr_t cand = candidates[ci];
            if (!cand || !IsWithinRel32(near_addr + 5, cand)) continue;
            MEMORY_BASIC_INFORMATION mbi = {};
            if (VirtualQuery(reinterpret_cast<LPCVOID>(cand), &mbi, sizeof(mbi)) == sizeof(mbi) &&
                mbi.State == MEM_FREE && mbi.RegionSize >= 4096) {
                uintptr_t aligned_base = (reinterpret_cast<uintptr_t>(mbi.BaseAddress) + 0xFFFFu) & ~static_cast<uintptr_t>(0xFFFFu);
                if (aligned_base >= reinterpret_cast<uintptr_t>(mbi.BaseAddress) &&
                    aligned_base + 4096 <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize &&
                    IsWithinRel32(near_addr + 5, aligned_base)) {
                    page = static_cast<uint8_t*>(
                        VirtualAlloc(reinterpret_cast<LPVOID>(aligned_base), 4096,
                                     MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READ));
                    if (page) break;
                }
            }
        }
    }
    if (!page) return nullptr;
    g_near_arena = page;
    g_near_used  = 0;
    g_near_cap   = 4096;
    return AllocNearExecStub(near_addr, code, len);
}

// ============================================================================
// Dynamic Open-Addressing FBO Hash Table (VirtualAlloc + SRWLOCK, unlimited FBOs)
// ============================================================================

struct FboRecord {
    const void* ptr;
    int         logical_w;
    int         logical_h;
    int         physical_w;
    int         physical_h;
    double      dpr;
};

static const void* const kFboTombstone = reinterpret_cast<const void*>(static_cast<uintptr_t>(1));
static FboRecord*        g_fbo_slots   = nullptr;
static uint32_t          g_fbo_cap     = 0;   // Always a power of 2
static uint32_t          g_fbo_count   = 0;   // Live entries
static uint32_t          g_fbo_used    = 0;   // Live entries + tombstones
static SRWLOCK           g_fbo_lock    = SRWLOCK_INIT;

static uint32_t HashPtr(const void* ptr) {
    uint64_t x = reinterpret_cast<uintptr_t>(ptr);
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return static_cast<uint32_t>(x);
}

static bool RehashFboTableLocked(uint32_t new_cap) {
    if (new_cap < 512) new_cap = 512;
    auto* new_slots = static_cast<FboRecord*>(
        VirtualAlloc(nullptr, static_cast<size_t>(new_cap) * sizeof(FboRecord),
                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!new_slots) return false;

    uint32_t mask = new_cap - 1u;
    uint32_t moved = 0;
    if (g_fbo_slots) {
        for (uint32_t i = 0; i < g_fbo_cap; ++i) {
            const void* k = g_fbo_slots[i].ptr;
            if (k && k != kFboTombstone) {
                uint32_t idx = HashPtr(k) & mask;
                while (new_slots[idx].ptr != nullptr) {
                    idx = (idx + 1u) & mask;
                }
                new_slots[idx] = g_fbo_slots[i];
                ++moved;
            }
        }
        VirtualFree(g_fbo_slots, 0, MEM_RELEASE);
    }
    g_fbo_slots = new_slots;
    g_fbo_cap   = new_cap;
    g_fbo_count = moved;
    g_fbo_used  = moved;
    return true;
}

static void RecordFbo(const void* ptr, int lw, int lh, int pw, int ph, double dpr) {
    if (!ptr || ptr == kFboTombstone) return;
    AcquireSRWLockExclusive(&g_fbo_lock);
    if (!g_fbo_slots) {
        if (!RehashFboTableLocked(512)) {
            ReleaseSRWLockExclusive(&g_fbo_lock);
            return;
        }
    } else if (static_cast<uint64_t>(g_fbo_used + 1u) * 4ULL >= static_cast<uint64_t>(g_fbo_cap) * 3ULL) {
        // Rehash at 75% (live + tombstones): double capacity only when live entries >= 37.5%,
        // otherwise compact tombstones in-place into a fresh table of the same capacity.
        uint32_t target_cap = (static_cast<uint64_t>(g_fbo_count) * 8ULL >= static_cast<uint64_t>(g_fbo_cap) * 3ULL)
            ? (g_fbo_cap * 2u)
            : g_fbo_cap;
        if (target_cap >= g_fbo_cap) {
            RehashFboTableLocked(target_cap);
        }
    }

    uint32_t mask = g_fbo_cap - 1u;
    uint32_t idx = HashPtr(ptr) & mask;
    int64_t first_tombstone = -1;

    // Probe the entire chain until nullptr so an existing key behind tombstones
    // is updated in-place rather than inserted a second time.
    for (uint32_t step = 0; step < g_fbo_cap; ++step) {
        const void* cur = g_fbo_slots[idx].ptr;
        if (cur == ptr) {
            g_fbo_slots[idx] = { ptr, lw, lh, pw, ph, dpr };
            ReleaseSRWLockExclusive(&g_fbo_lock);
            return;
        }
        if (cur == nullptr) {
            uint32_t dst = (first_tombstone >= 0) ? static_cast<uint32_t>(first_tombstone) : idx;
            if (first_tombstone < 0) {
                ++g_fbo_used;
            }
            g_fbo_slots[dst] = { ptr, lw, lh, pw, ph, dpr };
            ++g_fbo_count;
            ReleaseSRWLockExclusive(&g_fbo_lock);
            return;
        }
        if (cur == kFboTombstone && first_tombstone < 0) {
            first_tombstone = static_cast<int64_t>(idx);
        }
        idx = (idx + 1u) & mask;
    }

    if (first_tombstone >= 0) {
        g_fbo_slots[static_cast<uint32_t>(first_tombstone)] = { ptr, lw, lh, pw, ph, dpr };
        ++g_fbo_count;
    }
    ReleaseSRWLockExclusive(&g_fbo_lock);
}

static void EraseFbo(const void* ptr) {
    if (!ptr || ptr == kFboTombstone) return;
    AcquireSRWLockExclusive(&g_fbo_lock);
    if (g_fbo_slots && g_fbo_cap > 0) {
        uint32_t mask = g_fbo_cap - 1u;
        uint32_t idx = HashPtr(ptr) & mask;
        for (uint32_t step = 0; step < g_fbo_cap; ++step) {
            const void* cur = g_fbo_slots[idx].ptr;
            if (cur == nullptr) break;
            if (cur == ptr) {
                g_fbo_slots[idx] = { kFboTombstone, 0, 0, 0, 0, 0.0 };
                if (g_fbo_count > 0) --g_fbo_count;
                break;
            }
            idx = (idx + 1u) & mask;
        }
    }
    ReleaseSRWLockExclusive(&g_fbo_lock);
}

static bool LookupFbo(const void* ptr, FboRecord* out_rec = nullptr) {
    if (!ptr || ptr == kFboTombstone) return false;
    AcquireSRWLockShared(&g_fbo_lock);
    if (g_fbo_slots && g_fbo_cap > 0) {
        uint32_t mask = g_fbo_cap - 1u;
        uint32_t idx = HashPtr(ptr) & mask;
        for (uint32_t step = 0; step < g_fbo_cap; ++step) {
            const void* cur = g_fbo_slots[idx].ptr;
            if (cur == nullptr) break;
            if (cur == ptr) {
                if (out_rec) *out_rec = g_fbo_slots[idx];
                ReleaseSRWLockShared(&g_fbo_lock);
                return true;
            }
            idx = (idx + 1u) & mask;
        }
    }
    ReleaseSRWLockShared(&g_fbo_lock);
    return false;
}

static void FreeFboTable() {
    AcquireSRWLockExclusive(&g_fbo_lock);
    if (g_fbo_slots) {
        VirtualFree(g_fbo_slots, 0, MEM_RELEASE);
        g_fbo_slots = nullptr;
    }
    g_fbo_cap   = 0;
    g_fbo_count = 0;
    g_fbo_used  = 0;
    ReleaseSRWLockExclusive(&g_fbo_lock);
}

// ============================================================================
// Qt Meta-Object Helpers & QApplication::notify / LUXView Hooks (Problems 5 & 6a)
// ============================================================================

static void* QtMetaCast(void* qobj, const char* class_name) {
    if (!qobj || !class_name) return nullptr;
    void* const* vtable = *reinterpret_cast<void* const* const*>(qobj);
    if (!vtable || !vtable[1]) return nullptr;
    typedef void* (*FnMetaCast)(void*, const char*);
    return reinterpret_cast<FnMetaCast>(vtable[1])(qobj, class_name);
}

static const char* QtClassName(const void* qobj) {
    if (!qobj || !g_qmeta_class_name) return nullptr;
    void* const* vtable = *reinterpret_cast<void* const* const*>(qobj);
    if (!vtable || !vtable[0]) return nullptr;
    typedef const void* (*FnMetaObj)(const void*);
    const void* meta = reinterpret_cast<FnMetaObj>(vtable[0])(qobj);
    return meta ? g_qmeta_class_name(meta) : nullptr;
}

static bool StrEq(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (*a != *b) return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

// LUXQt::LUXView::updateImage (Modo 16 +0x171F60, Modo 17 +0xD702D0):
// Scope g_active_ui_dpr to the widget's live monitor DPR and force m_image
// reallocation on cross-screen DPI transitions even when logical QSize is unchanged.
static void HookLUXViewUpdateImage(void* lux_view) {
    if (!lux_view || !g_orig_luxview_update_image) return;
    const bool is_main = (GetCurrentThreadId() == g_main_tid);
    double prev_dpr = g_active_ui_dpr;
    const void* prev_force = g_force_recreate_qimg;

    if (is_main) {
        double live_dpr = GetWidgetDpr(lux_view);
        g_active_ui_dpr = live_dpr;
        if (g_pmv2_mode) {
            const void* m_img = reinterpret_cast<const uint8_t*>(lux_view) + (g_is_modo16 ? 0x48 : 0x40);
            double img_dpr = GetImageDpr(m_img);
            if (AbsDouble(img_dpr - live_dpr) > 0.005) {
                g_force_recreate_qimg = m_img;
            }
        }
    }

    g_orig_luxview_update_image(lux_view);

    if (is_main) {
        g_force_recreate_qimg = prev_force;
        g_active_ui_dpr = prev_dpr;
    }
}

// QApplication::notify hook:
// 1. Problem 6(a): Flush FireLazyHandlers(3, nullptr) at top-level UpdateRequest (77)
//    or Expose (206) BEFORE Qt locks QBackingStore and caches QPaintEngine* on the stack.
// 2. Problem 5: Track per-widget/per-window live DPR on the main thread and handle
//    QEvent::ScreenChangeInternal (214) for LUXQt::LUXGLWidget and LUXQt::LUXView.
static bool HookAppNotify(void* app, void* receiver, void* ev) {
    if (!ev || !g_orig_app_notify) {
        return g_orig_app_notify ? g_orig_app_notify(app, receiver, ev) : false;
    }

    if (GetCurrentThreadId() != g_main_tid) {
        return g_orig_app_notify(app, receiver, ev);
    }

    // QEvent::m_type (ushort) sits at +0x10 in Qt5 (vptr + QEventPrivate*) and +0x08 in Qt6 (vptr only).
    const uint16_t ev_type = *reinterpret_cast<const uint16_t*>(
        reinterpret_cast<const uint8_t*>(ev) + (g_is_modo16 ? 0x10 : 0x08));

    double prev_ui_dpr = g_active_ui_dpr;
    if (g_pmv2_mode && receiver) {
        void* qw = QtMetaCast(receiver, "QWidget");
        if (qw) {
            g_active_ui_dpr = GetWidgetDpr(qw);
        } else if (ev_type == 77 || ev_type == 206 || ev_type == 214) {
            void* qwin = QtMetaCast(receiver, "QWindow");
            if (qwin && g_qwin_get_dpr) {
                double wdpr = g_qwin_get_dpr(qwin);
                if (wdpr >= 1.0 && wdpr <= 4.0) {
                    g_active_ui_dpr = wdpr;
                }
            }
        }
    }

    const bool is_paint_tx = (ev_type == 77 || ev_type == 206 || ev_type == 12);
    if (is_paint_tx) {
        if (g_paint_tx_depth == 0 && (ev_type == 77 || ev_type == 206) && g_fire_lazy_handlers) {
            ++g_paint_tx_depth;
            g_fire_lazy_handlers(3, nullptr);
            --g_paint_tx_depth;
        }
        ++g_paint_tx_depth;
    }

    bool res = g_orig_app_notify(app, receiver, ev);

    // QEvent::ScreenChangeInternal == 214, QEvent::Show == 17 in both Qt5 and Qt6.
    if (g_pmv2_mode && (ev_type == 214 || ev_type == 17) && receiver) {
        const char* cls = QtClassName(receiver);
        if (cls) {
            if (StrEq(cls, "LUXQt::LUXGLWidget")) {
                double live_dpr = GetWidgetDpr(receiver);
                g_active_ui_dpr = live_dpr;
                const auto* glw_bytes = static_cast<const uint8_t*>(receiver);
                const auto* pane = *reinterpret_cast<const uint8_t* const*>(
                    glw_bytes + (g_is_modo16 ? 0x78 : 0x80));
                const auto* sub = pane
                    ? *reinterpret_cast<const uint8_t* const*>(pane + 0xD8)
                    : nullptr;
                const void* fbo = sub
                    ? *reinterpret_cast<const void* const*>(sub + 0x20)
                    : nullptr;
                bool fbo_stale = false;
                if (fbo) {
                    FboRecord fbo_rec = {};
                    double fbo_dpr = LookupFbo(fbo, &fbo_rec) ? fbo_rec.dpr : 1.0;
                    fbo_stale = (AbsDouble(fbo_dpr - live_dpr) > 0.005);
                }
                if (ev_type == 214 || (fbo_stale && g_force_recreate_fbo == nullptr)) {
                    const void* prev_force_fbo = g_force_recreate_fbo;
                    if (fbo_stale) {
                        g_force_recreate_fbo = fbo;
                    }
                    if (g_luxgl_screen_changed) {
                        g_luxgl_screen_changed(receiver);
                    }
                    g_force_recreate_fbo = prev_force_fbo;
                    if (g_qwidget_update) {
                        g_qwidget_update(receiver);
                    }
                }
            } else if (ev_type == 214 && StrEq(cls, "LUXQt::LUXView")) {
                if (g_orig_luxview_update_image) {
                    HookLUXViewUpdateImage(receiver);
                }
                if (g_qwidget_update) {
                    g_qwidget_update(receiver);
                }
            }
        }
    }

    if (is_paint_tx) {
        --g_paint_tx_depth;
    }
    g_active_ui_dpr = prev_ui_dpr;
    return res;
}

// ============================================================================
// 1. 3D OpenGL Viewport Hooks
// ============================================================================

static void* HookFboCtor(void* this_ptr, int w, int h, const void* fmt_ptr) {
    double dpr = GetCurrentGLDpr();
    if (IsHighDpiActive() && dpr > 1.001 && w > 0 && h > 0) {
        int pw = ScaleByDpr(w, dpr);
        int ph = ScaleByDpr(h, dpr);
        void* res = g_orig_fbo_ctor(this_ptr, pw, ph, fmt_ptr);
        RecordFbo(this_ptr, w, h, pw, ph, dpr);
        return res;
    }
    EraseFbo(this_ptr);
    return g_orig_fbo_ctor(this_ptr, w, h, fmt_ptr);
}

static void HookFboDtor(void* this_ptr) {
    if (g_force_recreate_fbo && this_ptr == g_force_recreate_fbo &&
        GetCurrentThreadId() == g_main_tid) {
        g_force_recreate_fbo = nullptr;
    }
    EraseFbo(this_ptr);
    g_orig_fbo_dtor(this_ptr);
}

static int HookFboWidth(const void* this_ptr) {
    const bool is_main = (GetCurrentThreadId() == g_main_tid);
    if (g_force_recreate_fbo && this_ptr == g_force_recreate_fbo && is_main) {
        return 0;
    }
    int pw = g_orig_fbo_width(this_ptr);
    FboRecord rec = {};
    const bool tracked = LookupFbo(this_ptr, &rec);
    if (g_pmv2_mode && is_main && this_ptr) {
        double fbo_dpr = tracked ? rec.dpr : 1.0;
        if (AbsDouble(fbo_dpr - GetCurrentGLDpr()) > 0.005) {
            return 0;
        }
    }
    if (IsHighDpiActive() && tracked && rec.physical_w == pw) {
        return rec.logical_w;
    }
    return pw;
}

static int HookFboHeight(const void* this_ptr) {
    const bool is_main = (GetCurrentThreadId() == g_main_tid);
    if (g_force_recreate_fbo && this_ptr == g_force_recreate_fbo && is_main) {
        return 0;
    }
    int ph = g_orig_fbo_height(this_ptr);
    FboRecord rec = {};
    const bool tracked = LookupFbo(this_ptr, &rec);
    if (g_pmv2_mode && is_main && this_ptr) {
        double fbo_dpr = tracked ? rec.dpr : 1.0;
        if (AbsDouble(fbo_dpr - GetCurrentGLDpr()) > 0.005) {
            return 0;
        }
    }
    if (IsHighDpiActive() && tracked && rec.physical_h == ph) {
        return rec.logical_h;
    }
    return ph;
}

static void HookFboBlit(void* target, const QRectC* tr, void* source, const QRectC* sr, uint32_t buffers, uint32_t filter) {
    if (!IsHighDpiActive() || !tr || !sr) {
        g_orig_fbo_blit(target, tr, source, sr, buffers, filter);
        return;
    }
    FboRecord src_rec = {};
    FboRecord dst_rec = {};
    const bool has_src = (source != nullptr) && LookupFbo(source, &src_rec);
    const bool has_dst = (target != nullptr) && LookupFbo(target, &dst_rec);
    double dpr_s = (source == nullptr) ? GetCurrentGLDpr() : (has_src ? src_rec.dpr : 1.0);
    double dpr_t = (target == nullptr) ? GetCurrentGLDpr() : (has_dst ? dst_rec.dpr : 1.0);
    if (dpr_s <= 1.001 && dpr_t <= 1.001) {
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
    int tx1 = ScaleByDpr(tr->x1, dpr_t);
    int ty1 = ScaleByDpr(tr->y1, dpr_t);
    int tx2 = ScaleByDpr(static_cast<int64_t>(tr->x2) + 1, dpr_t) - 1;
    int ty2 = ScaleByDpr(static_cast<int64_t>(tr->y2) + 1, dpr_t) - 1;
    int sx1 = ScaleByDpr(sr->x1, dpr_s);
    int sy1 = ScaleByDpr(sr->y1, dpr_s);
    int sx2 = ScaleByDpr(static_cast<int64_t>(sr->x2) + 1, dpr_s) - 1;
    int sy2 = ScaleByDpr(static_cast<int64_t>(sr->y2) + 1, dpr_s) - 1;
    QRectC scaled_t = { tx1, ty1, tx2, ty2 };
    QRectC scaled_s = { sx1, sy1, sx2, sy2 };
    g_orig_fbo_blit(target, &scaled_t, source, &scaled_s, buffers, filter);
}

static void ScaleGLBox(int x, int y, int w, int h, int* ox, int* oy, int* ow, int* oh) {
    double dpr = GetCurrentGLDpr();
    if (!IsHighDpiActive() || dpr <= 1.001) {
        *ox = x;
        *oy = y;
        *ow = w;
        *oh = h;
        return;
    }
    int px1 = ScaleByDpr(x, dpr);
    int py1 = ScaleByDpr(y, dpr);
    *ox = px1;
    *oy = py1;
    *ow = (w > 0) ? (ScaleByDpr(static_cast<int64_t>(x) + w, dpr) - px1) : ScaleByDpr(w, dpr);
    *oh = (h > 0) ? (ScaleByDpr(static_cast<int64_t>(y) + h, dpr) - py1) : ScaleByDpr(h, dpr);
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

static double HookWPaneGLGetPixelRatio(const uint8_t* pane) {
    return GetPaneDpr(pane);
}

// ============================================================================
// 2. 2D UI (OPALDisp / QImage / QPaintDevice / QPainter / QFont) Hooks
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

static void* HookQImageCtorSize(void* this_ptr, const QSizeC* sz, int fmt) {
    static constexpr uint8_t kSigUpdateImage[] = { 0x90, 0x48, 0x8B, 0xCF, 0xFF, 0x15 };
    double dpr = g_pmv2_mode ? g_active_ui_dpr : g_scale;
    if (IsHighDpiActive() && dpr > 1.001 && fmt == 5 && sz && sz->w > 0 && sz->h > 0 && g_qimg_set_dpr &&
        MatchReturnBytes(_ReturnAddress(), kSigUpdateImage)) {
        QSizeC scaled_sz = { ScaleByDpr(sz->w, dpr), ScaleByDpr(sz->h, dpr) };
        void* res = g_orig_qimg_ctor_size(this_ptr, &scaled_sz, fmt);
        g_qimg_set_dpr(this_ptr, dpr);
        return res;
    }
    return g_orig_qimg_ctor_size(this_ptr, sz, fmt);
}

static void* HookQImageCtorWH(void* this_ptr, int w, int h, int fmt) {
    static constexpr uint8_t kSigORastCreate[] = { 0x48, 0x8D, 0x05, 0, 0, 0, 0, 0x48, 0x89, 0x07 };
    if (IsHighDpiActive() && fmt == 5 && w > 0 && h > 0 && g_qimg_set_dpr &&
        MatchReturnPattern(_ReturnAddress(), kSigORastCreate, "xxx????xxx")) {
        double dpr = (g_in_rendwin_paint && GetCurrentThreadId() == g_main_tid && g_rendwin_paint_dpr > 1.001)
            ? g_rendwin_paint_dpr
            : (g_pmv2_mode ? g_active_ui_dpr : g_scale);
        if (dpr > 1.001) {
            void* res = g_orig_qimg_ctor_wh(this_ptr, ScaleByDpr(w, dpr), ScaleByDpr(h, dpr), fmt);
            g_qimg_set_dpr(this_ptr, dpr);
            return res;
        }
    }
    return g_orig_qimg_ctor_wh(this_ptr, w, h, fmt);
}

static QSizeC* HookQImageSize(const void* this_ptr, QSizeC* out_sz) {
    QSizeC* res = g_orig_qimg_size(this_ptr, out_sz);
    if (out_sz && g_force_recreate_qimg && this_ptr == g_force_recreate_qimg &&
        GetCurrentThreadId() == g_main_tid) {
        out_sz->w = -1;
        out_sz->h = -1;
        return res;
    }
    if (IsHighDpiActive() && out_sz) {
        double dpr = GetImageDpr(this_ptr);
        if (dpr > 1.001) {
            out_sz->w = UnscaleDimByDpr(out_sz->w, dpr);
            out_sz->h = UnscaleDimByDpr(out_sz->h, dpr);
        }
    }
    return res;
}

static int HookQImageWidth(const void* this_ptr) {
    if (g_force_recreate_qimg && this_ptr == g_force_recreate_qimg &&
        GetCurrentThreadId() == g_main_tid) {
        return -1;
    }
    int pw = g_orig_qimg_width(this_ptr);
    if (IsHighDpiActive()) {
        double dpr = GetImageDpr(this_ptr);
        if (dpr > 1.001) {
            return UnscaleDimByDpr(pw, dpr);
        }
    }
    return pw;
}

static int HookQImageHeight(const void* this_ptr) {
    int ph = g_orig_qimg_height(this_ptr);
    if (!IsHighDpiActive()) return ph;

    static constexpr uint8_t kSigQImageToILx[] = { 0x48, 0x8B, 0xCE, 0x8B, 0xD8, 0xFF, 0x15 };
    const bool is_q2i_main = MatchReturnBytes(_ReturnAddress(), kSigQImageToILx) &&
                             GetCurrentThreadId() == g_main_tid;
    if (is_q2i_main) {
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
    if (IsHighDpiActive() && IsQImageDevice(this_ptr)) {
        double dpr = GetPaintDeviceDpr(this_ptr);
        if (dpr > 1.001) {
            return UnscaleDimByDpr(pw, dpr);
        }
    }
    return pw;
}

static int HookQPaintDeviceHeight(const void* this_ptr) {
    int ph = g_orig_qpd_height(this_ptr);
    if (IsHighDpiActive() && IsQImageDevice(this_ptr)) {
        double dpr = GetPaintDeviceDpr(this_ptr);
        if (dpr > 1.001) {
            return UnscaleDimByDpr(ph, dpr);
        }
    }
    return ph;
}

static void* HookQImageCopy(const void* this_ptr, void* out_img, int x, int y, int w, int h) {
    if (IsHighDpiActive() && g_qimg_set_dpr) {
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
    if (IsHighDpiActive()) {
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

static void HookDrawImageRectRect(void* painter, const QRectC* tr, const void* img, const QRectC* sr, int flags) {
    if (IsHighDpiActive() && sr && img) {
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

static void HookDrawImageXY(void* painter, int x, int y, const void* img, int sx, int sy, int sw, int sh, int flags) {
    static constexpr uint8_t kSigOpalBlitImage[] = {
        0x89, 0x7C, 0x24, 0x20, 0x44, 0x8B, 0xCE, 0x44, 0x8B, 0xC5
    };
    if (g_rot_valid && IsHighDpiActive() && GetCurrentThreadId() == g_main_tid &&
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
    if (g_in_rendwin_paint && IsHighDpiActive() && GetCurrentThreadId() == g_main_tid &&
        g_rendwin_paint_dpr > 1.001 && MatchReturnBytes(_ReturnAddress(), kSigOpalBlitImage) &&
        IsHighDpiPainter(painter) && img && GetImageDpr(img) <= 1.001 &&
        g_qimg_copy_ctor && g_qimg_dtor && g_qimg_set_dpr &&
        g_orig_qimg_width && g_orig_qimg_height) {
        int pw = g_orig_qimg_width(img);
        int ph = g_orig_qimg_height(img);
        if (pw > 0 && ph > 0 && sx == 0 && sy == 0 && (sw < 0 || sw == pw) && (sh < 0 || sh == ph)) {
            alignas(16) uint8_t tmp_qimg[64] = {};
            g_qimg_copy_ctor(tmp_qimg, img);
            g_qimg_set_dpr(tmp_qimg, g_rendwin_paint_dpr);
            g_orig_draw_image_xy(painter, x, y, tmp_qimg, 0, 0, -1, -1, flags);
            g_qimg_dtor(tmp_qimg);
            return;
        }
    }
    g_orig_draw_image_xy(painter, x, y, img, sx, sy, sw, sh, flags);
}

static constexpr uint8_t kSigOpalFillRect[]  = { 0x48, 0x8D, 0x4C, 0x24, 0x38, 0xFF, 0x15 };
static constexpr uint8_t kSigOpalDrawLine[]  = { 0x48, 0x8B, 0x4B, 0x18, 0xFF, 0x15 };
static constexpr uint8_t kSigOpalDrawPoint[] = { 0xC7, 0x44, 0x24, 0x20, 0x01, 0x00, 0x00, 0x00 };

static void HookDrawRects(void* painter, const QRectC* rects, int count) {
    if (IsHighDpiActive() && rects && count == 1 && g_painter_fill_rect && g_painter_brush &&
        MatchReturnBytes(_ReturnAddress(), kSigOpalFillRect) && IsHighDpiPainter(painter)) {
        QRectC full_rect = { rects[0].x1, rects[0].y1, rects[0].x2 + 1, rects[0].y2 + 1 };
        g_painter_fill_rect(painter, &full_rect, g_painter_brush(painter));
        return;
    }
    g_orig_draw_rects(painter, rects, count);
}

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
    if (IsHighDpiActive() && lines && count == 1 && g_painter_fill_rect && g_painter_brush &&
        MatchReturnBytes(_ReturnAddress(), kSigOpalDrawLine) && IsHighDpiPainter(painter) &&
        TryFillAxisLine(painter, lines[0].x1, lines[0].y1, lines[0].x2, lines[0].y2)) {
        return;
    }
    g_orig_draw_lines(painter, lines, count);
}

static void HookDrawLine4(void* painter, int x1, int y1, int x2, int y2) {
    if (IsHighDpiActive() && g_painter_fill_rect && g_painter_brush &&
        MatchReturnBytes(_ReturnAddress(), kSigOpalDrawLine) && IsHighDpiPainter(painter) &&
        TryFillAxisLine(painter, x1, y1, x2, y2)) {
        return;
    }
    g_orig_draw_line4(painter, x1, y1, x2, y2);
}

static void HookDrawPoints(void* painter, const QSizeC* points, int count) {
    if (IsHighDpiActive() && points && count == 1 && g_painter_fill_rect && g_painter_brush &&
        MatchReturnBytes(_ReturnAddress(), kSigOpalDrawPoint) && IsHighDpiPainter(painter)) {
        QRectC full_rect = { points[0].w, points[0].h, points[0].w, points[0].h };
        g_painter_fill_rect(painter, &full_rect, g_painter_brush(painter));
        return;
    }
    g_orig_draw_points(painter, points, count);
}

static void HookDrawPoint2(void* painter, int x, int y) {
    if (IsHighDpiActive() && g_painter_fill_rect && g_painter_brush &&
        MatchReturnBytes(_ReturnAddress(), kSigOpalDrawPoint) && IsHighDpiPainter(painter)) {
        QRectC full_rect = { x, y, x, y };
        g_painter_fill_rect(painter, &full_rect, g_painter_brush(painter));
        return;
    }
    g_orig_draw_point2(painter, x, y);
}

static void HookPainterSetWindow(void* painter, int x, int y, int w, int h) {
    if (IsHighDpiActive() && painter && g_painter_device) {
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

static void HookFontSetStyleStrategy(void* font_ptr, int strategy) {
    static constexpr uint8_t kSigOpalDrawText[] = {
        0x49, 0x8B, 0x17, 0x48, 0x8B, 0x4B, 0x18, 0xFF, 0x15
    };
    double dpr = g_pmv2_mode ? g_active_ui_dpr : g_scale;
    if (IsHighDpiActive() && dpr > 1.001 && strategy == 0x100 &&
        MatchReturnBytes(_ReturnAddress(), kSigOpalDrawText)) {
        strategy = 0x80;
    }
    g_orig_font_style(font_ptr, strategy);
}

static void HookFontSetPointSize(void* font_ptr, int pt) {
    double dpr = GetCurrentGLDpr();
    if (IsHighDpiActive() && dpr > 1.001 && font_ptr && g_qfont_set_pt_f && g_qfont_get_pt_f) {
        const auto* ret = static_cast<const uint8_t*>(_ReturnAddress());
        if (IsInFrontText(ret, 16, 1)) {
            static constexpr uint8_t kSigM16Dpr[4]  = { 0x0F, 0xAF, 0, 0x8B };
            static constexpr uint8_t kSigM17Dpr[3]  = { 0xF2, 0x0F, 0x2C };
            static constexpr uint8_t kSigLargeGl[4] = { 0x74, 0x0E, 0xBA, 0x38 };
            bool is_m16_dpr = MatchPattern(ret - 16, kSigM16Dpr, "xx?x", 4) ||
                              MatchPattern(ret - 14, kSigM16Dpr, "xx?x", 4);
            bool is_m17_dpr = MatchPattern(ret - 15, kSigM17Dpr, nullptr, 3) ||
                              MatchPattern(ret - 13, kSigM17Dpr, nullptr, 3);
            if (is_m16_dpr || is_m17_dpr) {
                double base_pt = g_qfont_get_pt_f(font_ptr);
                if (base_pt > 0.0) {
                    g_qfont_set_pt_f(font_ptr, base_pt * dpr);
                    return;
                }
            }
            if (pt > 0 && MatchPattern(ret - 16, kSigLargeGl, nullptr, 4)) {
                g_qfont_set_pt_f(font_ptr, static_cast<double>(pt) * dpr);
                return;
            }
        }
    }
    g_orig_font_pt_size(font_ptr, pt);
}

// ============================================================================
// 3. Problem 1: Stateless AGLCaptureView & WPaneGetPosition Pairing + Fallback
// ============================================================================

static bool QueryPaneFboPhysicalSize(const uint8_t* pane, int lw, int lh, int* out_pw, int* out_ph, void*** out_fbo_slot = nullptr) {
    if (out_fbo_slot) *out_fbo_slot = nullptr;
    if (pane) {
        auto* sub = *reinterpret_cast<uint8_t* const*>(pane + 0xD8);
        if (sub) {
            auto** fbo_slot = reinterpret_cast<void**>(sub + 0x20);
            if (out_fbo_slot) *out_fbo_slot = fbo_slot;
            void* orig_fbo = *fbo_slot;
            if (orig_fbo) {
                FboRecord rec = {};
                if (LookupFbo(orig_fbo, &rec) && rec.physical_w > 0 && rec.physical_h > 0) {
                    *out_pw = rec.physical_w;
                    *out_ph = rec.physical_h;
                    return (rec.physical_w != lw || rec.physical_h != lh);
                }
                if (g_orig_fbo_width && g_orig_fbo_height) {
                    int fw = g_orig_fbo_width(orig_fbo);
                    int fh = g_orig_fbo_height(orig_fbo);
                    if (fw > 0 && fh > 0) {
                        *out_pw = fw;
                        *out_ph = fh;
                        return (fw != lw || fh != lh);
                    }
                }
            }
        }
    }
    double dpr = GetPaneDpr(pane);
    *out_pw = ScaleByDpr(lw, dpr);
    *out_ph = ScaleByDpr(lh, dpr);
    return dpr > 1.001;
}

// Stateless post-call return signatures in nexus (nexus3000.dll in M16, modo.exe in M17)
// for the 3 built-in viewport capture chains (A: clip.addStill/scene.thumbnail,
// B: gl.snapshot/.lxo thumbnail, C: gl.capture & Cached Playback).
static constexpr uint8_t kSigWPaneCapAB[16] = {
    0x8B, 0x44, 0x24, 0x30, 0x44, 0x8D, 0x24, 0x85,
    0x00, 0x00, 0x00, 0x00, 0x41, 0x8B, 0xC4, 0x0F
};
static constexpr uint8_t kSigWPaneCapC1[19] = {
    0x44, 0x39, 0x64, 0x24, 0x00, 0x0F, 0x84, 0x00,
    0x00, 0x00, 0x00, 0x85, 0xF6, 0x0F, 0x85, 0x00,
    0x00, 0x00, 0x00
};
static constexpr uint8_t kSigWPaneCapC2[10] = {
    0xE9, 0xC3, 0xFE, 0xFF, 0xFF, 0xF2, 0x0F, 0x10, 0x45, 0xA8
};
static constexpr uint8_t kSigWPaneCapC3[9] = {
    0x8B, 0x44, 0x24, 0x50, 0x39, 0x45, 0x00, 0x0F, 0x85
};

static double QueryRendWinDpr() {
    if (!g_pmv2_mode) return g_scale;
    if (g_in_rendwin_paint && g_rendwin_paint_dpr >= 1.0 && g_rendwin_paint_dpr <= 4.0) {
        return g_rendwin_paint_dpr;
    }
    if (g_rendwin_has_dpr && g_rendwin_last_dpr >= 1.0 && g_rendwin_last_dpr <= 4.0) {
        return g_rendwin_last_dpr;
    }
    if (g_active_ui_dpr >= 1.0 && g_active_ui_dpr <= 4.0) {
        return g_active_ui_dpr;
    }
    return g_scale;
}

static bool IsNativeCaptureWPaneCaller(const void* ret_addr) {
    return MatchNexusReturnBytes(ret_addr, kSigWPaneCapAB) ||
           MatchNexusReturnPattern(ret_addr, kSigWPaneCapC1, "xxxx?xx??xxxxxx??xx") ||
           MatchNexusReturnBytes(ret_addr, kSigWPaneCapC2) ||
           MatchNexusReturnPattern(ret_addr, kSigWPaneCapC3, "xxxxxx?xx");
}

static constexpr uint8_t kSigWPaneFitToWin[16] = {
    0x66, 0x0F, 0x6E, 0x44, 0x24, 0x50, 0x66, 0x0F,
    0x6E, 0x5C, 0x24, 0x58, 0xF3, 0x0F, 0xE6, 0xC0
};
static constexpr uint8_t kSigWPaneKeyCmd17[15] = {
    0x66, 0x44, 0x0F, 0x6E, 0x8C, 0x24, 0xC0, 0x00,
    0x00, 0x00, 0xF3, 0x45, 0x0F, 0xE6, 0xC9
};
static constexpr uint8_t kSigWPaneCmd17[18] = {
    0x66, 0x44, 0x0F, 0x6E, 0x44, 0x24, 0x48, 0xF3,
    0x45, 0x0F, 0xE6, 0xC0, 0x66, 0x44, 0x0F, 0x6E,
    0x65, 0x87
};

static bool IsRendWinPhysWPaneCaller(const void* ret_addr) {
    const auto* ret = static_cast<const uint8_t*>(ret_addr);
    if (g_rw_upd_begin && g_rw_upd_end && ret >= g_rw_upd_begin && ret < g_rw_upd_end) {
        return true;
    }
    return MatchNexusReturnBytes(ret_addr, kSigWPaneFitToWin) ||
           MatchNexusReturnBytes(ret_addr, kSigWPaneKeyCmd17) ||
           MatchNexusReturnBytes(ret_addr, kSigWPaneCmd17);
}

static void HookWPaneGetPosition(void* pane, int* x, int* y, int* w, int* h) {
    int lx = 0, ly = 0, lw = 0, lh = 0;
    g_orig_wpane_get_pos(pane, &lx, &ly, &lw, &lh);
    int ox = lx, oy = ly, ow = lw, oh = lh;

    if (IsHighDpiActive() && pane && lw > 0 && lh > 0) {
        const void* ret = _ReturnAddress();
        if (IsRendWinPhysWPaneCaller(ret)) {
            double dpr = QueryRendWinDpr();
            if (dpr > 1.001) {
                ow = ScaleByDpr(lw, dpr);
                oh = ScaleByDpr(lh, dpr);
            }
        } else if (IsNativeCaptureWPaneCaller(ret)) {
            int pw = lw, ph = lh;
            if (QueryPaneFboPhysicalSize(static_cast<const uint8_t*>(pane), lw, lh, &pw, &ph) &&
                pw > 0 && ph > 0) {
                ow = pw;
                oh = ph;
            }
        }
    }

    if (x) *x = ox;
    if (y) *y = oy;
    if (w) *w = ow;
    if (h) *h = oh;
}

static constexpr uint8_t kSigAGLCapA[14] = {
    0x8B, 0x54, 0x24, 0x34, 0x33, 0xF6, 0x85, 0xD2,
    0x7E, 0x00, 0x8B, 0x4C, 0x24, 0x30
};
static constexpr uint8_t kSigAGLCapB[12] = {
    0x8B, 0x94, 0x24, 0xA8, 0x00, 0x00, 0x00, 0x33,
    0xF6, 0x85, 0xD2, 0x7E
};
static constexpr uint8_t kSigAGLCapC[12] = {
    0x8B, 0x54, 0x24, 0x54, 0x45, 0x33, 0xC9, 0x41,
    0x8B, 0xF9, 0x85, 0xD2
};

static bool IsNativeCaptureAGLCaller(const void* ret_addr) {
    return MatchNexusReturnPattern(ret_addr, kSigAGLCapA, "xxxxxxxxx?xxxx") ||
           MatchNexusReturnBytes(ret_addr, kSigAGLCapB) ||
           MatchNexusReturnBytes(ret_addr, kSigAGLCapC);
}

static void HookAGLCaptureView(void* agl_view, void* out_buf) {
    if (!agl_view || !out_buf || !IsHighDpiActive()) {
        g_orig_agl_capture_view(agl_view, out_buf);
        return;
    }

    auto* impl = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(agl_view) + 0x10);
    if (!impl) {
        g_orig_agl_capture_view(agl_view, out_buf);
        return;
    }

    int* p_w = reinterpret_cast<int*>(impl + 0x90);
    int* p_h = reinterpret_cast<int*>(impl + 0x94);
    int lw = *p_w;
    int lh = *p_h;
    if (lw <= 0 || lh <= 0) {
        g_orig_agl_capture_view(agl_view, out_buf);
        return;
    }

    auto* pane = *reinterpret_cast<uint8_t**>(impl + 0xD8);
    void** fbo_slot = nullptr;
    int pw = lw, ph = lh;
    bool has_scaled_fbo = QueryPaneFboPhysicalSize(pane, lw, lh, &pw, &ph, &fbo_slot);
    if (!has_scaled_fbo || pw <= 0 || ph <= 0 || (pw == lw && ph == lh)) {
        g_orig_agl_capture_view(agl_view, out_buf);
        return;
    }

    // Tier 1: Built-in callers A/B/C already allocated pw * ph * 4 via paired HookWPaneGetPosition.
    // Temporarily set impl+0x90 / impl+0x94 to physical (pw, ph) to read the full native FBO.
    if (IsNativeCaptureAGLCaller(_ReturnAddress())) {
        *p_w = pw;
        *p_h = ph;
        g_orig_agl_capture_view(agl_view, out_buf);
        *p_w = lw;
        *p_h = lh;
        return;
    }

    // Tier 2: External plugin / unknown caller allocated lw * lh * 4 logical bytes.
    // Downsample the physical FBO into a temporary single-sample (lw, lh) FBO via GL_LINEAR
    // and temporarily substitute *fbo_slot so glReadPixels writes exact lw * lh * 4 full-frame pixels.
    if (fbo_slot && *fbo_slot && g_fbo_fmt_ctor && g_fbo_fmt_dtor &&
        g_orig_fbo_ctor && g_orig_fbo_dtor && g_orig_fbo_blit) {
        void* orig_fbo = *fbo_slot;
        if (g_wpane_gl_make_current && pane) {
            g_wpane_gl_make_current(pane);
        }
        alignas(16) uint8_t fmt_buf[32] = {};
        alignas(16) uint8_t tmp_fbo[64] = {};
        g_fbo_fmt_ctor(fmt_buf);
        g_orig_fbo_ctor(tmp_fbo, lw, lh, fmt_buf);
        QRectC dst_r = { 0, 0, lw - 1, lh - 1 };
        QRectC src_r = { 0, 0, pw - 1, ph - 1 };
        g_orig_fbo_blit(tmp_fbo, &dst_r, orig_fbo, &src_r, 0x4000u, 0x2601u);
        *fbo_slot = tmp_fbo;
        g_orig_agl_capture_view(agl_view, out_buf);
        *fbo_slot = orig_fbo;
        if (g_fbo_bind) {
            g_fbo_bind(orig_fbo);
        }
        g_orig_fbo_dtor(tmp_fbo);
        g_fbo_fmt_dtor(fmt_buf);
        return;
    }

    g_orig_agl_capture_view(agl_view, out_buf);
}

// ============================================================================
// 4. Problem 3.1: Built-in Preview Viewport (IView / 'PREV') Native 1:1
// ============================================================================

static double QueryIViewDpr(const void* iview) {
    if (!iview) return g_scale;
    const auto* iv = static_cast<const uint8_t*>(iview);
    const auto* agl_view = *reinterpret_cast<const uint8_t* const*>(iv + 0x440);
    if (agl_view) {
        const auto* impl = *reinterpret_cast<const uint8_t* const*>(agl_view + 0x10);
        if (impl) {
            const auto* pane = *reinterpret_cast<const uint8_t* const*>(impl + 0xD8);
            if (pane) {
                return GetPaneDpr(pane);
            }
        }
    }
    return g_pmv2_mode ? g_active_ui_dpr : g_scale;
}

struct ScopedIViewPhysDims {
    uint8_t* iv;
    int      orig_w;
    int      orig_h;
    double   dpr;
    bool     active;

    explicit ScopedIViewPhysDims(void* iview)
        : iv(static_cast<uint8_t*>(iview)), orig_w(0), orig_h(0), dpr(1.0), active(false) {
        if (!iv || !IsHighDpiActive() || GetCurrentThreadId() != g_main_tid) return;
        if (g_iview_scaled_ptr == iview) return;
        dpr = QueryIViewDpr(iview);
        if (dpr <= 1.001) return;
        int* pw_slot = reinterpret_cast<int*>(iv + 0x664);
        int* ph_slot = reinterpret_cast<int*>(iv + 0x668);
        orig_w = *pw_slot;
        orig_h = *ph_slot;
        if (orig_w <= 0 || orig_h <= 0) return;
        const bool has_header = ((*reinterpret_cast<const uint8_t*>(iv + 0x3E8) & 2u) != 0);
        *pw_slot = ScaleByDpr(orig_w, dpr);
        *ph_slot = (has_header && orig_h > 20)
            ? (ScaleByDpr(orig_h - 20, dpr) + 20)
            : ScaleByDpr(orig_h, dpr);
        g_iview_scaled_ptr = iview;
        active = true;
    }

    ~ScopedIViewPhysDims() {
        if (active) {
            *reinterpret_cast<int*>(iv + 0x664) = orig_w;
            *reinterpret_cast<int*>(iv + 0x668) = orig_h;
            g_iview_scaled_ptr = nullptr;
        }
    }
};

// IViewComputeZoom returns render_pixels_per_logical_screen_pixel in XMM0.
// Scaling [iview+0x664/0x668] to physical (pw, ph) computes inv_zoom_physical (1.0f at 100%,
// render_w / pw at Fit), and multiplying by dpr converts to logical pane/glOrtho space so
// IViewDraw, panning, region drag, and IViewPaneGetFullResScreenPixel align to 0.0px.
static float HookIViewComputeZoom(void* iview) {
    ScopedIViewPhysDims scope(iview);
    float inv_zoom = g_orig_iview_compute_zoom(iview);
    if (scope.active && scope.dpr > 1.001) {
        return static_cast<float>(static_cast<double>(inv_zoom) * scope.dpr);
    }
    return inv_zoom;
}

static void HookIViewGetAutoRenderSize(void* iview, int* out_w, int* out_h) {
    ScopedIViewPhysDims scope(iview);
    g_orig_iview_get_auto_size(iview, out_w, out_h);
}

static int HookIViewCheckAutoRenderSize(void* iview) {
    ScopedIViewPhysDims scope(iview);
    return g_orig_iview_chk_auto_size(iview);
}

// ============================================================================
// 5. Problem 3.2: Built-in 2D Tiled Image Viewer (ClipView) Mipmap Selection
// ============================================================================

static int ClipViewCheckThresholdImpl(const uint8_t* rbx, uint32_t lvl_w, uint32_t lvl_h, float ref_xmm6, size_t off_w, size_t off_h) {
    if (!rbx) return 0;
    double dpr = 1.0;
    if (IsHighDpiActive()) {
        const auto* pane = *reinterpret_cast<const uint8_t* const*>(rbx + 0x148);
        dpr = GetPaneDpr(pane);
    }
    const float aspect = *reinterpret_cast<const float*>(rbx + 0x100);
    if (aspect > ref_xmm6) {
        int32_t raw_w = *reinterpret_cast<const int32_t*>(rbx + off_w);
        uint32_t phys_w = static_cast<uint32_t>(ScaleByDpr(raw_w, dpr));
        return (lvl_w > phys_w) ? 1 : 0;
    }
    int32_t raw_h = *reinterpret_cast<const int32_t*>(rbx + off_h);
    uint32_t phys_h = static_cast<uint32_t>(ScaleByDpr(raw_h, dpr));
    return (lvl_h > phys_h) ? 1 : 0;
}

static int ClipViewCheckLoop1(const uint8_t* rbx, uint32_t lvl_w, uint32_t lvl_h, float ref_xmm6) {
    return ClipViewCheckThresholdImpl(rbx, lvl_w, lvl_h, ref_xmm6, 0x130, 0x134);
}

static int ClipViewCheckLoop2(const uint8_t* rbx, uint32_t lvl_w, uint32_t lvl_h, float ref_xmm6) {
    return ClipViewCheckThresholdImpl(rbx, lvl_w, lvl_h, ref_xmm6, 0x104, 0x108);
}

// Build x64 codecave stubs for ClipViewUpdateMip Loop 1 and Loop 2.
// At p1 / p2 right after `call GetMipSize`, RSP is 16-byte aligned.
// Subbing 0x20 preserves RSP % 16 == 0 before `call rax` and provides 32B shadow space.
static uint8_t* BuildClipViewLoopStub(uint8_t* site, bool is_loop1) {
    uint8_t code[96] = {};
    size_t c = 0;

    // sub rsp, 0x20
    code[c++] = 0x48; code[c++] = 0x83; code[c++] = 0xEC; code[c++] = 0x20;
    // mov rcx, rbx
    code[c++] = 0x48; code[c++] = 0x8B; code[c++] = 0xCB;
    // mov edx, dword ptr [rbp - 0x35]
    code[c++] = 0x8B; code[c++] = 0x55; code[c++] = 0xCB;
    // mov r8d, dword ptr [rbp - 0x39]
    code[c++] = 0x44; code[c++] = 0x8B; code[c++] = 0x45; code[c++] = 0xC7;
    // movaps xmm3, xmm6
    code[c++] = 0x0F; code[c++] = 0x28; code[c++] = 0xDE;
    // movabs rax, helper_fn; call rax
    code[c++] = 0x48; code[c++] = 0xB8;
    *reinterpret_cast<uintptr_t*>(code + c) = is_loop1
        ? reinterpret_cast<uintptr_t>(&ClipViewCheckLoop1)
        : reinterpret_cast<uintptr_t>(&ClipViewCheckLoop2);
    c += 8;
    code[c++] = 0xFF; code[c++] = 0xD0;
    // add rsp, 0x20
    code[c++] = 0x48; code[c++] = 0x83; code[c++] = 0xC4; code[c++] = 0x20;
    // test eax, eax
    code[c++] = 0x85; code[c++] = 0xC0;

    if (is_loop1) {
        // Reload ecx = [rbp - 0x39], edx = [rbp - 0x35] without altering EFLAGS
        code[c++] = 0x8B; code[c++] = 0x4D; code[c++] = 0xC7;
        code[c++] = 0x8B; code[c++] = 0x55; code[c++] = 0xCB;
        // je below_or_equal (+21 bytes: 7B mov + 14B jmp)
        code[c++] = 0x74; code[c++] = 21;
        // mov dword ptr [rbp - 0x31], 1
        code[c++] = 0xC7; code[c++] = 0x45; code[c++] = 0xCF;
        code[c++] = 0x01; code[c++] = 0x00; code[c++] = 0x00; code[c++] = 0x00;
        // jmp [rip + 0] -> site + 0x52
        code[c++] = 0xFF; code[c++] = 0x25;
        *reinterpret_cast<uint32_t*>(code + c) = 0; c += 4;
        *reinterpret_cast<uintptr_t*>(code + c) = reinterpret_cast<uintptr_t>(site + 0x52); c += 8;
        // below_or_equal: jmp [rip + 0] -> site + 0x2C
        code[c++] = 0xFF; code[c++] = 0x25;
        *reinterpret_cast<uint32_t*>(code + c) = 0; c += 4;
        *reinterpret_cast<uintptr_t*>(code + c) = reinterpret_cast<uintptr_t>(site + 0x2C); c += 8;
    } else {
        // Reload eax = [rbp - 0x39], ecx = [rbp - 0x35] without altering EFLAGS
        code[c++] = 0x8B; code[c++] = 0x45; code[c++] = 0xC7;
        code[c++] = 0x8B; code[c++] = 0x4D; code[c++] = 0xCB;
        // je below_or_equal (+14 bytes)
        code[c++] = 0x74; code[c++] = 14;
        // jmp [rip + 0] -> site + 0x34
        code[c++] = 0xFF; code[c++] = 0x25;
        *reinterpret_cast<uint32_t*>(code + c) = 0; c += 4;
        *reinterpret_cast<uintptr_t*>(code + c) = reinterpret_cast<uintptr_t>(site + 0x34); c += 8;
        // below_or_equal: jmp [rip + 0] -> site + 0x23
        code[c++] = 0xFF; code[c++] = 0x25;
        *reinterpret_cast<uint32_t*>(code + c) = 0; c += 4;
        *reinterpret_cast<uintptr_t*>(code + c) = reinterpret_cast<uintptr_t>(site + 0x23); c += 8;
    }

    return AllocExecStub(code, c);
}

// ============================================================================
// 6. Problem 3.3: Built-in F9 Render Window (RendWin) Native 1:1 & Picking
// ============================================================================

static double QueryDispRefDpr(const void* disp_ref) {
    if (disp_ref) {
        const void* qimg = *reinterpret_cast<const void* const*>(
            static_cast<const uint8_t*>(disp_ref) + 0x08);
        if (qimg) {
            double dpr = GetImageDpr(qimg);
            if (dpr >= 1.0 && dpr <= 4.0) return dpr;
        }
    }
    return g_pmv2_mode ? g_active_ui_dpr : g_scale;
}

// Re-entrant logical zoom scope for RendWin coordinate/HUD/scrollbar/mouse functions.
// RendWin stores a single scalar `double g_rendwin_zoom` representing physical zoom
// (e.g. 1.0 == 100% 1:1 physical pixels). Functions operating in logical pane pixels
// temporarily view `g_rendwin_zoom / dpr` while preserving adjacent .data globals.
struct ScopedRendWinLogicalZoom {
    bool active;

    ScopedRendWinLogicalZoom() : active(false) {
        if (!IsHighDpiActive() || !g_rendwin_zoom || GetCurrentThreadId() != g_main_tid) return;
        if (g_rw_zoom_depth == 0) {
            double dpr = QueryRendWinDpr();
            if (dpr <= 1.001) return;
            g_rw_phys_zoom  = *g_rendwin_zoom;
            g_rw_active_dpr = dpr;
            *g_rendwin_zoom = g_rw_phys_zoom / dpr;
            g_rw_zoom_depth = 1;
            active = true;
        } else {
            ++g_rw_zoom_depth;
            active = true;
        }
    }

    ~ScopedRendWinLogicalZoom() {
        if (active) {
            --g_rw_zoom_depth;
            if (g_rw_zoom_depth <= 0) {
                g_rw_zoom_depth = 0;
                if (g_rendwin_zoom) {
                    *g_rendwin_zoom = g_rw_phys_zoom;
                }
            }
        }
    }
};

static void HookRendWinSetZoom(double zoom) {
    if (g_rw_zoom_depth > 0 && GetCurrentThreadId() == g_main_tid && g_rendwin_zoom) {
        *g_rendwin_zoom = g_rw_phys_zoom;
        g_orig_rw_set_zoom(zoom);
        g_rw_phys_zoom  = *g_rendwin_zoom;
        *g_rendwin_zoom = g_rw_phys_zoom / g_rw_active_dpr;
        return;
    }
    g_orig_rw_set_zoom(zoom);
}

static int HookRendWinUpdateImage(void* a1, void* a2, void* a3, void* a4) {
    if (g_rw_zoom_depth > 0 && GetCurrentThreadId() == g_main_tid && g_rendwin_zoom) {
        *g_rendwin_zoom = g_rw_phys_zoom;
        int res = g_orig_rw_update_image(a1, a2, a3, a4);
        g_rw_phys_zoom  = *g_rendwin_zoom;
        *g_rendwin_zoom = g_rw_phys_zoom / g_rw_active_dpr;
        return res;
    }
    return g_orig_rw_update_image(a1, a2, a3, a4);
}

static void HookRendWinPaint(void* pane, void* disp_ref, int flags) {
    if (!IsHighDpiActive() || !pane || !disp_ref || GetCurrentThreadId() != g_main_tid) {
        g_orig_rendwin_paint(pane, disp_ref, flags);
        return;
    }

    double prev_dpr = g_rendwin_last_dpr;
    bool   had_dpr  = g_rendwin_has_dpr;
    double dpr      = QueryDispRefDpr(disp_ref);
    g_rendwin_last_dpr = dpr;
    g_rendwin_has_dpr  = true;

    if (had_dpr && AbsDouble(dpr - prev_dpr) > 0.01 && g_orig_rw_update_image) {
        HookRendWinUpdateImage(nullptr, nullptr, nullptr, nullptr);
    }

    if (dpr <= 1.001) {
        g_orig_rendwin_paint(pane, disp_ref, flags);
        return;
    }

    g_in_rendwin_paint  = true;
    g_rendwin_paint_dpr = dpr;
    {
        ScopedRendWinLogicalZoom scope;
        g_orig_rendwin_paint(pane, disp_ref, flags);
    }
    g_in_rendwin_paint  = false;
}

static int64_t HookRendWinMouseEvent(void* a1, void* a2, void* a3, void* a4) {
    ScopedRendWinLogicalZoom scope;
    return g_orig_rw_mouse_event(a1, a2, a3, a4);
}

static int HookRendWinScrollbarGet(void* a1, int a2, int a3, void* a4, void* a5, void* a6, void* a7, void* a8) {
    ScopedRendWinLogicalZoom scope;
    return g_orig_rw_scrollbar_get(a1, a2, a3, a4, a5, a6, a7, a8);
}

static int HookRendWinGetScaleAndOffset(void* a1, void* a2, void* a3, void* a4) {
    ScopedRendWinLogicalZoom scope;
    return g_orig_rw_get_scale_offset(a1, a2, a3, a4);
}

static int64_t HookRendWinCenterAtNormalized(double cx, double cy) {
    ScopedRendWinLogicalZoom scope;
    return g_orig_rw_center_at_norm(cx, cy);
}

static void HookRendWinPaneToNormalized(int x, int y, void* out_nx, void* out_ny) {
    ScopedRendWinLogicalZoom scope;
    g_orig_rw_pane_to_norm(x, y, out_nx, out_ny);
}

static int HookRendWinNormalizedToPane(double nx, double ny, void* out_x, void* out_y) {
    ScopedRendWinLogicalZoom scope;
    return g_orig_rw_norm_to_pane(nx, ny, out_x, out_y);
}

static int HookRendWinCenterImageGetSize(void* img, int* out_w, int* out_h) {
    int res = g_orig_image_get_size(img, out_w, out_h);
    if (IsHighDpiActive() && GetCurrentThreadId() == g_main_tid) {
        double dpr = QueryRendWinDpr();
        if (dpr > 1.001) {
            if (out_w && *out_w > 0) *out_w = UnscaleDimByDpr(*out_w, dpr);
            if (out_h && *out_h > 0) *out_h = UnscaleDimByDpr(*out_h, dpr);
        }
    }
    return res;
}

static void HookRendWinBlitVignette(void* orast, void* img, int ox, int oy, int pane_w, int pane_h) {
    if (IsHighDpiActive() && g_in_rendwin_paint && GetCurrentThreadId() == g_main_tid &&
        g_rendwin_paint_dpr > 1.001) {
        pane_w = ScaleByDpr(pane_w, g_rendwin_paint_dpr);
        pane_h = ScaleByDpr(pane_h, g_rendwin_paint_dpr);
    }
    g_orig_rw_blit_vignette(orast, img, ox, oy, pane_w, pane_h);
}

static void HookRendWinWipeBlit(void* orast, void* slot_b, int ox, int oy, int pane_w, int pane_h) {
    if (IsHighDpiActive() && g_in_rendwin_paint && GetCurrentThreadId() == g_main_tid &&
        g_rendwin_paint_dpr > 1.001 && g_rw_wipe_mode && *g_rw_wipe_mode == 1 &&
        g_rw_spotlight_mx && g_rw_spotlight_my && g_rw_spotlight_radius &&
        *g_rw_spotlight_mx >= 0 && *g_rw_spotlight_my >= 0) {
        const double dpr = g_rendwin_paint_dpr;
        const int orig_mx  = *g_rw_spotlight_mx;
        const int orig_my  = *g_rw_spotlight_my;
        const int orig_rad = *g_rw_spotlight_radius;
        *g_rw_spotlight_mx     = ox + ScaleByDpr(static_cast<int64_t>(orig_mx) - ox, dpr);
        *g_rw_spotlight_my     = oy + ScaleByDpr(static_cast<int64_t>(orig_my) - oy, dpr);
        *g_rw_spotlight_radius = ScaleByDpr(orig_rad, dpr);
        g_orig_rw_wipe_blit(orast, slot_b, ox, oy, pane_w, pane_h);
        *g_rw_spotlight_mx     = orig_mx;
        *g_rw_spotlight_my     = orig_my;
        *g_rw_spotlight_radius = orig_rad;
        return;
    }
    g_orig_rw_wipe_blit(orast, slot_b, ox, oy, pane_w, pane_h);
}

// ============================================================================
// 7. Problem 3.4: Third-Party .lx Qt IPR Hooks (OctaneRender for Modo.lx, etc.)
// ============================================================================

static constexpr uint32_t kMaxIprScrollAreas = 64;
static const void*        g_ipr_scroll_areas[kMaxIprScrollAreas] = {};
static uint32_t           g_ipr_scroll_next = 0;
static SRWLOCK            g_ipr_lock = SRWLOCK_INIT;

static void RecordIprScrollArea(const void* sa) {
    if (!sa) return;
    AcquireSRWLockExclusive(&g_ipr_lock);
    for (uint32_t i = 0; i < kMaxIprScrollAreas; ++i) {
        if (g_ipr_scroll_areas[i] == sa) {
            ReleaseSRWLockExclusive(&g_ipr_lock);
            return;
        }
    }
    for (uint32_t i = 0; i < kMaxIprScrollAreas; ++i) {
        if (g_ipr_scroll_areas[i] == nullptr) {
            g_ipr_scroll_areas[i] = sa;
            ReleaseSRWLockExclusive(&g_ipr_lock);
            return;
        }
    }
    g_ipr_scroll_areas[(g_ipr_scroll_next++) & (kMaxIprScrollAreas - 1u)] = sa;
    ReleaseSRWLockExclusive(&g_ipr_lock);
}

static void EvictIprScrollArea(const void* sa) {
    if (!sa) return;
    AcquireSRWLockExclusive(&g_ipr_lock);
    for (uint32_t i = 0; i < kMaxIprScrollAreas; ++i) {
        if (g_ipr_scroll_areas[i] == sa) {
            g_ipr_scroll_areas[i] = nullptr;
        }
    }
    ReleaseSRWLockExclusive(&g_ipr_lock);
}

static bool IsIprScrollArea(void* widget) {
    if (!widget || !g_qscrollarea_widget) return false;
    bool was_cached = false;
    AcquireSRWLockShared(&g_ipr_lock);
    for (uint32_t i = 0; i < kMaxIprScrollAreas; ++i) {
        if (g_ipr_scroll_areas[i] == widget) {
            was_cached = true;
            break;
        }
    }
    ReleaseSRWLockShared(&g_ipr_lock);

    // Always validate live QObject type hierarchy via qt_metacast so a destroyed
    // IPR QScrollArea whose heap address is reused by another QWidget is evicted.
    void* sa = QtMetaCast(widget, "QScrollArea");
    if (sa && sa == widget) {
        void* content = g_qscrollarea_widget(sa);
        if (content && QtMetaCast(content, "QLabel") != nullptr) {
            if (!was_cached) {
                RecordIprScrollArea(sa);
            }
            return true;
        }
    }
    if (was_cached) {
        EvictIprScrollArea(widget);
    }
    return false;
}

static void* FindParentIprScrollArea(void* label) {
    if (!label || !g_qwidget_parent || !g_qscrollarea_widget) return nullptr;
    void* cur = label;
    for (int depth = 0; depth < 3; ++depth) {
        cur = g_qwidget_parent(cur);
        if (!cur) break;
        void* sa = QtMetaCast(cur, "QScrollArea");
        if (sa && g_qscrollarea_widget(sa) == label) {
            RecordIprScrollArea(sa);
            return sa;
        }
    }
    return nullptr;
}

static void HookLxQLabelSetPixmap(void* label, const void* pixmap) {
    if (IsHighDpiActive() && label && pixmap &&
        g_qpixmap_get_dpr && g_qpixmap_set_dpr && g_qpixmap_copy_ctor && g_qpixmap_dtor) {
        double pix_dpr = g_qpixmap_get_dpr(pixmap);
        if (pix_dpr <= 1.001) {
            double target_dpr = GetWidgetDpr(label);
            if (target_dpr > 1.001 && FindParentIprScrollArea(label) != nullptr) {
                alignas(16) uint8_t tmp_pix[64] = {};
                g_qpixmap_copy_ctor(tmp_pix, pixmap);
                g_qpixmap_set_dpr(tmp_pix, target_dpr);
                g_orig_lx_label_set_pixmap(label, tmp_pix);
                g_qpixmap_dtor(tmp_pix);
                return;
            }
        }
    }
    g_orig_lx_label_set_pixmap(label, pixmap);
}

static QSizeC* HookLxQWidgetSize(const void* widget, QSizeC* out_sz) {
    QSizeC* res = g_orig_lx_qwidget_size(widget, out_sz);
    if (IsHighDpiActive() && widget && out_sz && IsIprScrollArea(const_cast<void*>(widget))) {
        double dpr = GetWidgetDpr(widget);
        if (dpr > 1.001) {
            out_sz->w = ScaleByDpr(out_sz->w, dpr);
            out_sz->h = ScaleByDpr(out_sz->h, dpr);
        }
    }
    return res;
}

// ============================================================================
// 8. IAT Lookup, Pattern Scanning & Hook Installation
// ============================================================================

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
    if (!mod) return;
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
        *slot = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(hook_fn));
        VirtualProtect(slot, sizeof(void*), old_prot, &old_prot);
    }
}

template <typename Fn>
static void PatchIAT(uint8_t* base, const char* symbol, Fn hook_fn, Fn* out_orig = nullptr) {
    PatchSlot(FindIATSlot(base, symbol), hook_fn, out_orig);
}

static bool HasLxExtension(const wchar_t* s, size_t len) {
    if (!s || len < 3) return false;
    wchar_t c0 = s[len - 3];
    wchar_t c1 = s[len - 2];
    wchar_t c2 = s[len - 1];
    if (c1 >= L'A' && c1 <= L'Z') c1 = static_cast<wchar_t>(c1 + (L'a' - L'A'));
    if (c2 >= L'A' && c2 <= L'Z') c2 = static_cast<wchar_t>(c2 + (L'a' - L'A'));
    return (c0 == L'.' && c1 == L'l' && c2 == L'x');
}

static void PatchLxModuleIAT(uint8_t* mod_base) {
    if (!mod_base) return;
    void** s_set_pix = FindIATSlot(mod_base, "?setPixmap@QLabel@@QEAAXAEBVQPixmap@@@Z");
    void** s_w_size  = FindIATSlot(mod_base, "?size@QWidget@@QEBA?AVQSize@@XZ");
    if (s_set_pix) {
        PatchSlot(s_set_pix, &HookLxQLabelSetPixmap, &g_orig_lx_label_set_pixmap);
    }
    if (s_w_size) {
        PatchSlot(s_w_size, &HookLxQWidgetSize, &g_orig_lx_qwidget_size);
    }
}

static VOID CALLBACK OnLdrDllNotification(ULONG reason, const LdrDllNotificationData* data, PVOID /*ctx*/) {
    // LDR_DLL_NOTIFICATION_REASON_LOADED == 1
    if (reason != 1 || !data || !data->Loaded.DllBase) return;
    const UnicodeStringNtdll* name = data->Loaded.BaseDllName ? data->Loaded.BaseDllName : data->Loaded.FullDllName;
    if (!name || !name->Buffer || name->Length < 3 * sizeof(wchar_t)) return;
    size_t char_len = name->Length / sizeof(wchar_t);
    if (HasLxExtension(name->Buffer, char_len)) {
        PatchLxModuleIAT(static_cast<uint8_t*>(data->Loaded.DllBase));
    }
}

static void InstallLxModuleHooks() {
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (k32) {
        auto fn_enum = reinterpret_cast<FnK32EnumProcessModules>(
            GetProcAddress(k32, "K32EnumProcessModules"));
        if (fn_enum) {
            HMODULE mods[1024] = {};
            DWORD needed = 0;
            if (fn_enum(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
                DWORD count = needed / sizeof(HMODULE);
                if (count > 1024) count = 1024;
                wchar_t path_buf[1024] = {};
                for (DWORD i = 0; i < count; ++i) {
                    DWORD n = GetModuleFileNameW(mods[i], path_buf, 1024);
                    if (n >= 3 && n < 1024 && HasLxExtension(path_buf, n)) {
                        PatchLxModuleIAT(reinterpret_cast<uint8_t*>(mods[i]));
                    }
                }
            }
        }
    }

    if (!g_ldr_cookie) {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll) {
            auto fn_reg = reinterpret_cast<FnLdrRegisterDllNotification>(
                GetProcAddress(ntdll, "LdrRegisterDllNotification"));
            if (fn_reg) {
                fn_reg(0, &OnLdrDllNotification, nullptr, &g_ldr_cookie);
            }
        }
    }
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

static uint8_t* FindPatternInRange(uint8_t* begin, uint8_t* end, const uint8_t* pat, const char* mask, size_t len) {
    if (!begin || !end || end <= begin) return nullptr;
    size_t span = static_cast<size_t>(end - begin);
    if (span < len) return nullptr;
    for (size_t i = 0; i <= span - len; ++i) {
        if (MatchPattern(begin + i, pat, mask, len)) return begin + i;
    }
    return nullptr;
}

static DWORD GetRootFunctionBeginRva(DWORD64 image_base, const RUNTIME_FUNCTION* rf) {
    const RUNTIME_FUNCTION* cur = rf;
    for (int depth = 0; depth < 16 && cur; ++depth) {
        DWORD uw_rva = cur->UnwindData;
        if ((uw_rva & 1u) != 0) {
            cur = reinterpret_cast<const RUNTIME_FUNCTION*>(
                image_base + static_cast<DWORD64>(uw_rva & ~1u));
            continue;
        }
        const auto* uw = reinterpret_cast<const uint8_t*>(image_base + uw_rva);
        const uint8_t flags = static_cast<uint8_t>(uw[0] >> 3);
        // UNW_FLAG_CHAININFO == 0x4
        if ((flags & 0x4u) == 0) {
            return cur->BeginAddress;
        }
        const uint32_t code_slots = (static_cast<uint32_t>(uw[2]) + 1u) & ~1u;
        cur = reinterpret_cast<const RUNTIME_FUNCTION*>(uw + 4 + code_slots * 2);
    }
    return cur ? cur->BeginAddress : 0;
}

static const uint8_t* GetFunctionEndOrFallback(const uint8_t* code_addr, size_t fallback_span = 0x180) {
    if (!code_addr) return nullptr;
    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(code_addr), &image_base, nullptr);
    if (rf && image_base != 0) {
        const DWORD root_begin = GetRootFunctionBeginRva(image_base, rf);
        DWORD cur_end = rf->EndAddress;
        for (int seg = 0; seg < 64; ++seg) {
            PRUNTIME_FUNCTION next_rf = nullptr;
            for (DWORD pad = 0; pad <= 16; ++pad) {
                DWORD64 next_base = 0;
                PRUNTIME_FUNCTION cand = RtlLookupFunctionEntry(
                    image_base + cur_end + pad, &next_base, nullptr);
                if (cand && next_base == image_base) {
                    next_rf = cand;
                    break;
                }
            }
            if (!next_rf || next_rf->BeginAddress < cur_end ||
                next_rf->BeginAddress > cur_end + 16 || next_rf->EndAddress <= cur_end) {
                break;
            }
            if (GetRootFunctionBeginRva(image_base, next_rf) != root_begin) {
                break;
            }
            cur_end = next_rf->EndAddress;
        }
        const uint8_t* fn_end = reinterpret_cast<const uint8_t*>(image_base + cur_end);
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
    *reinterpret_cast<uintptr_t*>(jmp + 6) = reinterpret_cast<uintptr_t>(dst_fn);
    PatchCodeBytes(target, jmp, sizeof(jmp));
}

static void* ResolveRelCallTarget(const uint8_t* call_insn) {
    if (!call_insn || call_insn[0] != 0xE8) return nullptr;
    int32_t disp = *reinterpret_cast<const int32_t*>(call_insn + 1);
    return const_cast<uint8_t*>(call_insn + 5 + disp);
}

template <typename Fn>
static bool PatchRelCall(uint8_t* call_insn, Fn dst_fn) {
    if (!call_insn || call_insn[0] != 0xE8) return false;
    uintptr_t target = reinterpret_cast<uintptr_t>(dst_fn);
    if (!IsWithinRel32(call_insn + 5, target)) {
        uint8_t stub_code[14] = { 0xFF, 0x25, 0, 0, 0, 0 };
        *reinterpret_cast<uintptr_t*>(stub_code + 6) = target;
        uint8_t* near_stub = AllocNearExecStub(call_insn, stub_code, sizeof(stub_code));
        if (!near_stub) return false;
        target = reinterpret_cast<uintptr_t>(near_stub);
        if (!IsWithinRel32(call_insn + 5, target)) return false;
    }
    int32_t rel = static_cast<int32_t>(
        static_cast<intptr_t>(target) - reinterpret_cast<intptr_t>(call_insn + 5));
    uint8_t patch[5] = { 0xE8 };
    *reinterpret_cast<int32_t*>(patch + 1) = rel;
    return PatchCodeBytes(call_insn, patch, sizeof(patch));
}

static void InstallAllHooks() {
    if (g_hooked) return;

    HMODULE front_mod = GetModuleHandleA("front3000.dll");
    const bool is_modo16 = (front_mod != nullptr);
    g_is_modo16 = is_modo16;

    auto* front = reinterpret_cast<uint8_t*>(is_modo16 ? front_mod : GetModuleHandleA(nullptr));
    auto* frame = reinterpret_cast<uint8_t*>(is_modo16 ? GetModuleHandleA("frame3000.dll") : reinterpret_cast<HMODULE>(front));
    auto* nexus = reinterpret_cast<uint8_t*>(is_modo16 ? GetModuleHandleA("nexus3000.dll") : reinterpret_cast<HMODULE>(front));
    HMODULE qtgui     = GetModuleHandleA(is_modo16 ? "Qt5Gui.dll"     : "Qt6Gui.dll");
    HMODULE qtcore    = GetModuleHandleA(is_modo16 ? "Qt5Core.dll"    : "Qt6Core.dll");
    HMODULE qtwidgets = GetModuleHandleA(is_modo16 ? "Qt5Widgets.dll" : "Qt6Widgets.dll");
    HMODULE qtopengl  = is_modo16 ? qtgui : GetModuleHandleA("Qt6OpenGL.dll");
    if (!front || !frame || !qtgui) return;

    const uint8_t* text_begin = nullptr;
    const uint8_t* text_end   = nullptr;
    if (!GetModuleTextBounds(front, &text_begin, &text_end)) return;

    // Problem 6(c): Permanently pin high_dpi_scaling.dll in the process address space
    // before writing any IAT or inline jump pointers into our code pages.
    HMODULE pinned_self = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&InstallAllHooks),
        &pinned_self
    );

    const uint8_t* nx_begin = nullptr;
    const uint8_t* nx_end   = nullptr;
    if (nexus && GetModuleTextBounds(nexus, &nx_begin, &nx_end)) {
        g_nexus_text_begin = nx_begin;
        g_nexus_text_end   = nx_end;
    }

    BindExport(qtgui, "?setDevicePixelRatio@QImage@@QEAAXN@Z",                      g_qimg_set_dpr);
    BindExport(qtgui, "?devicePixelRatio@QImage@@QEBANXZ",                          g_qimg_get_dpr);
    BindExport(qtgui, "?devicePixelRatioF@QPaintDevice@@QEBANXZ",                   g_qpd_get_dpr);
    BindExport(qtgui, "?devicePixelRatio@QWindow@@QEBANXZ",                         g_qwin_get_dpr);
    BindExport(qtgui, "?currentContext@QOpenGLContext@@SAPEAV1@XZ",                 g_qgl_current_context);
    BindExport(qtgui, "?surface@QOpenGLContext@@QEBAPEAVQSurface@@XZ",              g_qgl_surface);
    BindExport(qtgui, "?surfaceClass@QSurface@@QEBA?AW4SurfaceClass@1@XZ",          g_qsurf_class);
    BindExport(qtgui, "?device@QPainter@@QEBAPEAVQPaintDevice@@XZ",                 g_painter_device);
    BindExport(qtgui, "?fillRect@QPainter@@QEAAXAEBVQRect@@AEBVQBrush@@@Z",         g_painter_fill_rect);
    BindExport(qtgui, "?brush@QPainter@@QEBAAEBVQBrush@@XZ",                        g_painter_brush);
    BindExport(qtgui, "??0QImage@@QEAA@AEBV0@@Z",                                   g_qimg_copy_ctor);
    BindExport(qtgui, "??1QImage@@UEAA@XZ",                                         g_qimg_dtor);
    BindExport(qtgui, "?scanLine@QImage@@QEAAPEAEH@Z",                              g_qimg_scanline);
    BindExport(qtgui, "?setPointSizeF@QFont@@QEAAXN@Z",                             g_qfont_set_pt_f);
    BindExport(qtgui, "?pointSizeF@QFont@@QEBANXZ",                                 g_qfont_get_pt_f);
    BindExport(qtgui, "??0QPixmap@@QEAA@AEBV0@@Z",                                  g_qpixmap_copy_ctor);
    BindExport(qtgui, "??1QPixmap@@UEAA@XZ",                                        g_qpixmap_dtor);
    BindExport(qtgui, "?devicePixelRatio@QPixmap@@QEBANXZ",                         g_qpixmap_get_dpr);
    BindExport(qtgui, "?setDevicePixelRatio@QPixmap@@QEAAXN@Z",                     g_qpixmap_set_dpr);

    BindExport(qtopengl,  "??0QOpenGLFramebufferObjectFormat@@QEAA@XZ",             g_fbo_fmt_ctor);
    BindExport(qtopengl,  "??1QOpenGLFramebufferObjectFormat@@QEAA@XZ",             g_fbo_fmt_dtor);
    BindExport(qtopengl,  "?bind@QOpenGLFramebufferObject@@QEAA_NXZ",               g_fbo_bind);

    BindExport(qtcore,    "?className@QMetaObject@@QEBAPEBDXZ",                     g_qmeta_class_name);
    BindExport(qtwidgets, "?update@QWidget@@QEAAXXZ",                               g_qwidget_update);
    BindExport(qtwidgets, "?parentWidget@QWidget@@QEBAPEAV1@XZ",                    g_qwidget_parent);
    BindExport(qtwidgets, "?widget@QScrollArea@@QEBAPEAVQWidget@@XZ",               g_qscrollarea_widget);
    BindExport(qtwidgets, "?setPixmap@QLabel@@QEAAXAEBVQPixmap@@@Z",                g_orig_lx_label_set_pixmap);
    BindExport(qtwidgets, "?size@QWidget@@QEBA?AVQSize@@XZ",                        g_orig_lx_qwidget_size);

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

    // 2. 2D UI (OPALDisp) & QApplication::notify IAT hooks
    PatchIAT(front, "?notify@QApplication@@UEAA_NPEAVQObject@@PEAVQEvent@@@Z", &HookAppNotify, &g_orig_app_notify);

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

    // Modo 16 fractional High-DPI truncation fixes
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

    // Rotated vertical tab strip call sites (90 deg CCW and CW)
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

    // Problem 6(a): Resolve FireLazyHandlers address before NOPing out the mid-paint call
    // inside LUXQt::LUXView::paintEvent so HookAppNotify flushes it safely pre-paint.
    static const uint8_t pat_lazy16[15] = { 0x33, 0xD2, 0x8D, 0x4A, 0x03, 0xE8, 0, 0, 0, 0, 0x41, 0xC6, 0x46, 0x40, 0x01 };
    static const uint8_t pat_lazy17[14] = { 0x33, 0xD2, 0x8D, 0x4A, 0x03, 0xE8, 0, 0, 0, 0, 0xC6, 0x47, 0x38, 0x01 };
    uint8_t* p_lazy = is_modo16
        ? FindCodePattern(front, 0x1724BE, pat_lazy16, "xxxxxx????xxxxx", sizeof(pat_lazy16))
        : FindCodePattern(front, 0xD7078C, pat_lazy17, "xxxxxx????xxxx", sizeof(pat_lazy17));
    if (p_lazy) {
        g_fire_lazy_handlers = reinterpret_cast<FnFireLazyHandlers>(ResolveRelCallTarget(p_lazy + 5));
        static const uint8_t kNop5[5] = { 0x90, 0x90, 0x90, 0x90, 0x90 };
        PatchCodeBytes(p_lazy + 5, kNop5, sizeof(kNop5));
    }

    // Problem 5: Hook LUXQt::LUXView::updateImage (15B clean prologue) & resolve LUXGLWidget::handleScreenChanged
    static const uint8_t pat_lux_upd[24] = {
        0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x56, 0x57,
        0x48, 0x83, 0xEC, 0x70, 0x48, 0x8B, 0xF9, 0x48,
        0x8D, 0x94, 0x24, 0x98, 0x00, 0x00, 0x00, 0xFF
    };
    uint8_t* p_lux_upd = FindCodePattern(front, is_modo16 ? 0x171F60 : 0xD702D0, pat_lux_upd, nullptr, sizeof(pat_lux_upd));
    if (p_lux_upd) {
        g_orig_luxview_update_image = BuildCleanTrampoline<FnLUXWidgetMethod>(p_lux_upd, 15);
        if (g_orig_luxview_update_image) {
            PatchJump14(p_lux_upd, &HookLUXViewUpdateImage);
        }
    }

    static const uint8_t pat_gl_scr16[20] = {
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20,
        0x48, 0x8B, 0x41, 0x78, 0x48, 0x8B, 0xF9, 0x48, 0x85, 0xC0
    };
    static const uint8_t pat_gl_scr17[23] = {
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20,
        0x48, 0x8B, 0x81, 0x80, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xF9,
        0x48, 0x85, 0xC0
    };
    uint8_t* p_gl_scr = is_modo16
        ? FindCodePattern(front, 0x1BA120, pat_gl_scr16, nullptr, sizeof(pat_gl_scr16))
        : FindCodePattern(front, 0xDA12B0, pat_gl_scr17, nullptr, sizeof(pat_gl_scr17));
    if (p_gl_scr) {
        g_luxgl_screen_changed = reinterpret_cast<FnLUXWidgetMethod>(p_gl_scr);
    }

    // Problem 1: WPaneGetPosition & AGLCaptureView hooks
    static const uint8_t pat_wp_get_pos[56] = {
        0x40, 0x53, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x48,
        0x48, 0x8B, 0x05, 0, 0, 0, 0, 0x48, 0x33, 0xC4,
        0x48, 0x89, 0x44, 0x24, 0x30, 0x48, 0x8B, 0x41, 0x18, 0x49,
        0x8B, 0xF0, 0x48, 0x8B, 0x9C, 0x24, 0x90, 0x00, 0x00, 0x00,
        0x4C, 0x8D, 0x44, 0x24, 0x20, 0x4C, 0x8B, 0xF2, 0x49, 0x8B,
        0xF9, 0xBA, 0x01, 0x00, 0x00, 0x00
    };
    static const char kMaskWPanePos[] = "xxxxxxxxxxxxx????xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";

    uint8_t* p_wp_get = FindCodePattern(front, is_modo16 ? 0x66EE0 : 0xC56C30, pat_wp_get_pos, kMaskWPanePos, sizeof(pat_wp_get_pos));
    static const uint8_t pat_agl_cap[22] = {
        0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18,
        0x57, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8B, 0x59, 0x10, 0x48,
        0x8B, 0xF2
    };
    uint8_t* p_agl_cap = FindCodePattern(front, is_modo16 ? 0x7AD0 : 0xD2EF50, pat_agl_cap, nullptr, sizeof(pat_agl_cap));

    if (p_wp_get && p_agl_cap) {
        if (p_agl_cap[0x6F] == 0xE8) {
            g_wpane_gl_make_current = reinterpret_cast<FnWPaneGLMakeCurrent>(ResolveRelCallTarget(p_agl_cap + 0x6F));
        }
        g_orig_wpane_get_pos    = BuildSecurityCookieTrampoline<FnWPanePos>(p_wp_get);
        g_orig_agl_capture_view = BuildCleanTrampoline<FnAGLCaptureView>(p_agl_cap, 15);
        if (g_orig_wpane_get_pos && g_orig_agl_capture_view) {
            PatchJump14(p_wp_get,  &HookWPaneGetPosition);
            PatchJump14(p_agl_cap, &HookAGLCaptureView);
        }
    }

    // Problem 3.1: IView (Preview viewport) native 1:1 hooks
    static const uint8_t pat_iv_zoom[34] = {
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10,
        0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20,
        0x41, 0x56, 0x48, 0x83, 0xEC, 0x60, 0xF3, 0x0F, 0x10, 0x89,
        0xC0, 0x06, 0x00, 0x00
    };
    static const uint8_t pat_iv_auto16[23] = {
        0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18,
        0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x83, 0xB9, 0x80, 0x00,
        0x00, 0x00, 0x00
    };
    static const uint8_t pat_iv_auto17[17] = {
        0x40, 0x53, 0x56, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x20,
        0x48, 0x83, 0xB9, 0x80, 0x00, 0x00, 0x00, 0x00
    };
    static const uint8_t pat_iv_chk[24] = {
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x30,
        0x8B, 0x81, 0x64, 0x06, 0x00, 0x00, 0x33, 0xFF, 0x48, 0x8B,
        0xD9, 0x85, 0xC0, 0x0F
    };

    uint8_t* p_iv_zoom = FindCodePattern(frame, is_modo16 ? 0x225560 : 0x97B620, pat_iv_zoom, nullptr, sizeof(pat_iv_zoom));
    uint8_t* p_iv_auto = is_modo16
        ? FindCodePattern(frame, 0x229BE0, pat_iv_auto16, nullptr, sizeof(pat_iv_auto16))
        : FindCodePattern(frame, 0x97F880, pat_iv_auto17, nullptr, sizeof(pat_iv_auto17));
    uint8_t* p_iv_chk  = FindCodePattern(frame, is_modo16 ? 0x22FA10 : 0x984D00, pat_iv_chk, nullptr, sizeof(pat_iv_chk));

    if (p_iv_zoom) {
        g_orig_iview_compute_zoom = BuildCleanTrampoline<FnIViewComputeZoom>(p_iv_zoom, 15);
        if (g_orig_iview_compute_zoom) {
            PatchJump14(p_iv_zoom, &HookIViewComputeZoom);
        }
    }
    if (p_iv_auto) {
        g_orig_iview_get_auto_size = BuildCleanTrampoline<FnIViewGetAutoSize>(p_iv_auto, is_modo16 ? 15 : 17);
        if (g_orig_iview_get_auto_size) {
            PatchJump14(p_iv_auto, &HookIViewGetAutoRenderSize);
        }
    }
    if (p_iv_chk) {
        g_orig_iview_chk_auto_size = BuildCleanTrampoline<FnIViewCheckAutoSize>(p_iv_chk, 16);
        if (g_orig_iview_chk_auto_size) {
            PatchJump14(p_iv_chk, &HookIViewCheckAutoRenderSize);
        }
    }

    // Problem 3.2: ClipView 2D Tiled Image mipmap selection threshold upgrade
    static const uint8_t pat_cv_loop1[25] = {
        0xF3, 0x0F, 0x10, 0x83, 0x00, 0x01, 0x00, 0x00, 0x0F, 0x2F, 0xC6,
        0x8B, 0x4D, 0xC7, 0x8B, 0x55, 0xCB, 0x76, 0x11,
        0x3B, 0x93, 0x30, 0x01, 0x00, 0x00
    };
    static const uint8_t pat_cv_loop2[25] = {
        0xF3, 0x0F, 0x10, 0x83, 0x00, 0x01, 0x00, 0x00, 0x0F, 0x2F, 0xC6,
        0x8B, 0x45, 0xC7, 0x8B, 0x4D, 0xCB, 0x76, 0x08,
        0x3B, 0x8B, 0x04, 0x01, 0x00, 0x00
    };
    uint8_t* p_cv1 = FindCodePattern(front, is_modo16 ? 0x205618 : 0xD1C918, pat_cv_loop1, nullptr, sizeof(pat_cv_loop1));
    uint8_t* p_cv2 = FindCodePattern(front, is_modo16 ? 0x205778 : 0xD1CA78, pat_cv_loop2, nullptr, sizeof(pat_cv_loop2));
    if (p_cv1 && p_cv2) {
        uint8_t* stub1 = BuildClipViewLoopStub(p_cv1, true);
        uint8_t* stub2 = BuildClipViewLoopStub(p_cv2, false);
        if (stub1 && stub2) {
            PatchJump14(p_cv1, stub1);
            PatchJump14(p_cv2, stub2);
        }
    }

    // Problem 3.3: Built-in F9 Render Window (RendWin) 1:1 physical display & picking
    if (nexus && g_orig_wpane_get_pos) {
        static const uint8_t pat_rw_upd16[35] = {
            0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10,
            0x48, 0x89, 0x78, 0x18, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56,
            0x41, 0x57, 0x48, 0x8D, 0x68, 0xA1, 0x48, 0x81, 0xEC, 0xD0, 0x00,
            0x00, 0x00
        };
        static const uint8_t pat_rw_upd17[38] = {
            0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10,
            0x48, 0x89, 0x78, 0x18, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56,
            0x41, 0x57, 0x48, 0x8D, 0xA8, 0x68, 0xFD, 0xFF, 0xFF, 0x48, 0x81,
            0xEC, 0x70, 0x03, 0x00, 0x00
        };
        static const uint8_t pat_rw_mulsd[17] = {
            0xF3, 0x0F, 0xE6, 0xC0, 0xF2, 0x0F, 0x59, 0x05, 0, 0, 0, 0,
            0xF2, 0x44, 0x0F, 0x2C, 0xC0
        };
        static const uint8_t pat_rw_paint16[46] = {
            0x44, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10,
            0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x53, 0x41, 0x56, 0x41,
            0x57, 0x48, 0x8D, 0x6C, 0x24, 0xC1, 0x48, 0x81, 0xEC, 0xC8,
            0x00, 0x00, 0x00, 0x4C, 0x8B, 0xF1, 0x41, 0x8B, 0xD8, 0x48,
            0x8B, 0x89, 0xE0, 0x00, 0x00, 0x00
        };
        static const uint8_t pat_rw_paint17[44] = {
            0x44, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10,
            0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x53, 0x56, 0x57, 0x48,
            0x8D, 0x6C, 0x24, 0xC1, 0x48, 0x81, 0xEC, 0xF8, 0x00, 0x00,
            0x00, 0x48, 0x8B, 0xD9, 0x41, 0x8B, 0xF0, 0x48, 0x8B, 0x89,
            0xE0, 0x00, 0x00, 0x00
        };
        static const uint8_t pat_rw_setz16[25] = {
            0x40, 0x57, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00,
            0x48, 0x83, 0x3D, 0, 0, 0, 0, 0x00,
            0x0F, 0x29, 0x74, 0x24, 0x30, 0x0F, 0x28, 0xF0
        };
        static const uint8_t pat_rw_setz17[24] = {
            0x40, 0x57, 0x48, 0x83, 0xEC, 0x50, 0x0F, 0x29, 0x74, 0x24, 0x20,
            0x0F, 0x28, 0xF0, 0xE8, 0, 0, 0, 0, 0x48, 0x85, 0xC0, 0x74, 0x0F
        };
        static const uint8_t pat_rw_mouse16[25] = {
            0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x20, 0x55, 0x56, 0x57,
            0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x68, 0xA1, 0x48, 0x81,
            0xEC, 0xD0, 0x00, 0x00, 0x00
        };
        static const uint8_t pat_rw_mouse17[24] = {
            0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x48, 0x89, 0x70, 0x20,
            0x55, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C,
            0x24, 0xA0
        };
        static const uint8_t pat_rw_scroll16[25] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10,
            0x57, 0x48, 0x83, 0xEC, 0x40, 0x83, 0x3D, 0, 0, 0, 0, 0x00,
            0x49, 0x8B, 0xF1
        };
        static const uint8_t pat_rw_scroll17[26] = {
            0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18,
            0x48, 0x89, 0x74, 0x24, 0x20, 0x41, 0x56, 0x48, 0x83, 0xEC,
            0x60, 0x33, 0xDB, 0x4D, 0x8B, 0xF1
        };
        static const uint8_t pat_rw_vig[26] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10,
            0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x40,
            0x48, 0x8B, 0xC2, 0x41, 0x8B, 0xF0
        };
        static const uint8_t pat_rw_wipe[24] = {
            0x48, 0x85, 0xD2, 0x0F, 0x84, 0, 0x05, 0x00, 0x00,
            0x48, 0x8B, 0xC4, 0x44, 0x89, 0x48, 0x20, 0x44, 0x89, 0x40, 0x18,
            0x48, 0x89, 0x50, 0x10
        };

        uint8_t* p_rw_upd = is_modo16
            ? FindCodePattern(nexus, 0x3575F0, pat_rw_upd16, nullptr, sizeof(pat_rw_upd16))
            : FindCodePattern(nexus, 0x1B6330, pat_rw_upd17, nullptr, sizeof(pat_rw_upd17));
        uint8_t* p_rw_paint = is_modo16
            ? FindCodePattern(nexus, 0x34B6F0, pat_rw_paint16, nullptr, sizeof(pat_rw_paint16))
            : FindCodePattern(nexus, 0x1AF7D0, pat_rw_paint17, nullptr, sizeof(pat_rw_paint17));
        uint8_t* p_rw_setz = is_modo16
            ? FindCodePattern(nexus, 0x35D850, pat_rw_setz16, "xxxxxxxxxxxx????xxxxxxxxx", sizeof(pat_rw_setz16))
            : FindCodePattern(nexus, 0x1BB670, pat_rw_setz17, "xxxxxxxxxxxxxxx????xxxxx", sizeof(pat_rw_setz17));
        uint8_t* p_rw_mouse = is_modo16
            ? FindCodePattern(nexus, 0x3497B0, pat_rw_mouse16, nullptr, sizeof(pat_rw_mouse16))
            : FindCodePattern(nexus, 0x1ADA20, pat_rw_mouse17, nullptr, sizeof(pat_rw_mouse17));
        uint8_t* p_rw_scroll = is_modo16
            ? FindCodePattern(nexus, 0x343420, pat_rw_scroll16, "xxxxxxxxxxxxxxxxx????xxxx", sizeof(pat_rw_scroll16))
            : FindCodePattern(nexus, 0x1A8BB0, pat_rw_scroll17, nullptr, sizeof(pat_rw_scroll17));
        uint8_t* p_rw_vig   = FindCodePattern(nexus, is_modo16 ? 0x33DB80 : 0x1A4F40, pat_rw_vig, nullptr, sizeof(pat_rw_vig));
        uint8_t* p_rw_wipe  = FindCodePattern(nexus, is_modo16 ? 0x34C1B0 : 0x1B05D0, pat_rw_wipe, "xxxxx?xxxxxxxxxxxxxxxxxx", sizeof(pat_rw_wipe));

        uint8_t* upd_end = nullptr;
        uint8_t* p_mulsd = nullptr;
        if (p_rw_upd) {
            upd_end = const_cast<uint8_t*>(GetFunctionEndOrFallback(p_rw_upd, 0xC00));
            p_mulsd = FindPatternInRange(p_rw_upd, upd_end, pat_rw_mulsd, "xxxxxxxx????xxxxx", sizeof(pat_rw_mulsd));
        }

        if (p_rw_upd && p_mulsd && p_rw_paint) {
            // Single scalar double g_rendwin_zoom (never touch adjacent .data globals).
            int32_t z_disp = *reinterpret_cast<int32_t*>(p_mulsd + 8);
            g_rendwin_zoom = reinterpret_cast<double*>(p_mulsd + 12 + z_disp);
            g_rw_upd_begin = p_rw_upd;
            g_rw_upd_end   = upd_end;

            g_orig_rw_update_image = BuildCleanTrampoline<FnRendWinUpdateImage>(p_rw_upd, 15);
            if (g_orig_rw_update_image) {
                PatchJump14(p_rw_upd, &HookRendWinUpdateImage);
            }

            if (p_rw_setz) {
                g_orig_rw_set_zoom = is_modo16
                    ? BuildRipCmpZeroTrampoline<FnRendWinSetZoom>(p_rw_setz)
                    : BuildCleanTrampoline<FnRendWinSetZoom>(p_rw_setz, 14);
                if (g_orig_rw_set_zoom) {
                    PatchJump14(p_rw_setz, &HookRendWinSetZoom);
                }
            }

            if (p_rw_mouse) {
                g_orig_rw_mouse_event = BuildCleanTrampoline<FnRendWinMouseEvent>(p_rw_mouse, is_modo16 ? 14 : 15);
                if (g_orig_rw_mouse_event) {
                    PatchJump14(p_rw_mouse, &HookRendWinMouseEvent);
                }
            }

            if (p_rw_scroll) {
                g_orig_rw_scrollbar_get = BuildCleanTrampoline<FnRendWinScrollbarGet>(p_rw_scroll, 15);
                if (g_orig_rw_scrollbar_get) {
                    PatchJump14(p_rw_scroll, &HookRendWinScrollbarGet);
                }
            }

            if (p_rw_vig) {
                g_orig_rw_blit_vignette = BuildCleanTrampoline<FnRendWinBlitVignette>(p_rw_vig, 20);
                if (g_orig_rw_blit_vignette) {
                    PatchJump14(p_rw_vig, &HookRendWinBlitVignette);
                }
            }

            // Patch centering & A/B split-wipe ImageGetSize call sites to return logical dims.
            if (is_modo16) {
                static const uint8_t pat_c1[26] = {
                    0x4C, 0x8D, 0x44, 0x24, 0x50, 0x48, 0x8D, 0x54, 0x24, 0x48,
                    0xE8, 0, 0, 0, 0, 0x8B, 0xC7, 0x2B, 0x44, 0x24, 0x48, 0x99,
                    0x2B, 0xC2, 0xD1, 0xF8
                };
                static const uint8_t pat_c2[32] = {
                    0x48, 0x8D, 0x54, 0x24, 0x78, 0xE8, 0, 0, 0, 0,
                    0x44, 0x8B, 0x44, 0x24, 0x78, 0x8B, 0xC5, 0x44, 0x8B, 0x8C,
                    0x24, 0x80, 0x00, 0x00, 0x00, 0x41, 0x2B, 0xC0, 0x99, 0x2B,
                    0xC2, 0xD1
                };
                static const uint8_t pat_c3[35] = {
                    0x4C, 0x8D, 0x44, 0x24, 0x78, 0x48, 0x8D, 0x54, 0x24, 0x70,
                    0xE8, 0, 0, 0, 0, 0x44, 0x8B, 0x44, 0x24, 0x70, 0xB9, 0x00,
                    0x00, 0x00, 0x00, 0x44, 0x8B, 0x4C, 0x24, 0x78, 0x8B, 0xC5,
                    0x41, 0x2B, 0xC0
                };
                static const uint8_t pat_c4[25] = {
                    0x4C, 0x8D, 0x45, 0xDB, 0x48, 0x8D, 0x55, 0xD7, 0xE8, 0, 0, 0, 0,
                    0x8B, 0x4D, 0xD7, 0x45, 0x8B, 0xCC, 0x44, 0x8B, 0x45, 0xDB, 0x8B, 0xC7
                };
                uint8_t* m1 = FindCodePattern(nexus, 0x34A396, pat_c1, "xxxxxxxxxx????xxxxxxxxxxxx", sizeof(pat_c1));
                uint8_t* m2 = FindCodePattern(nexus, 0x34A930, pat_c2, "xxxxx????xxxxxxxxxxxxxxxxxxxxxxx", sizeof(pat_c2));
                uint8_t* m3 = FindCodePattern(nexus, 0x34B235, pat_c3, "xxxxxxxxxx????xxxxxxxxxxxxxxxxxxxxx", sizeof(pat_c3));
                uint8_t* m4 = FindCodePattern(nexus, 0x34B81E, pat_c4, "xxxxxxxxx????xxxxxxxxxxxx", sizeof(pat_c4));
                uint8_t* calls[4] = {
                    m1 ? (m1 + 10) : nullptr,
                    m2 ? (m2 + 5)  : nullptr,
                    m3 ? (m3 + 10) : nullptr,
                    m4 ? (m4 + 8)  : nullptr
                };
                for (int i = 0; i < 4; ++i) {
                    if (!calls[i]) continue;
                    void* tgt = ResolveRelCallTarget(calls[i]);
                    if (!tgt) continue;
                    if (!g_orig_image_get_size) {
                        g_orig_image_get_size = reinterpret_cast<FnImageGetSize>(tgt);
                    }
                    if (reinterpret_cast<FnImageGetSize>(tgt) == g_orig_image_get_size) {
                        PatchRelCall(calls[i], &HookRendWinCenterImageGetSize);
                    }
                }
            } else {
                static const uint8_t pat_c1[26] = {
                    0x4C, 0x8D, 0x44, 0x24, 0x50, 0x48, 0x8D, 0x54, 0x24, 0x48,
                    0xE8, 0, 0, 0, 0, 0x8B, 0xC6, 0x2B, 0x44, 0x24, 0x48, 0x99,
                    0x2B, 0xC2, 0xD1, 0xF8
                };
                static const uint8_t pat_c2[30] = {
                    0x4C, 0x8D, 0x44, 0x24, 0x68, 0x48, 0x8D, 0x54, 0x24, 0x60,
                    0xE8, 0, 0, 0, 0, 0x44, 0x8B, 0x44, 0x24, 0x60, 0x8B, 0xC5,
                    0x44, 0x8B, 0x4C, 0x24, 0x68, 0x41, 0x2B, 0xC0
                };
                static const uint8_t pat_c3[25] = {
                    0x4C, 0x8D, 0x45, 0xAB, 0x48, 0x8D, 0x55, 0xA7, 0xE8, 0, 0, 0, 0,
                    0x8B, 0x4D, 0xA7, 0x45, 0x8B, 0xCC, 0x44, 0x8B, 0x45, 0xAB, 0x8B, 0xC6
                };
                uint8_t* m1 = FindCodePattern(nexus, 0x1AE76B, pat_c1, "xxxxxxxxxx????xxxxxxxxxxxx", sizeof(pat_c1));
                uint8_t* m2 = FindCodePattern(nexus, 0x1AECD4, pat_c2, "xxxxxxxxxx????xxxxxxxxxxxxxxxx", sizeof(pat_c2));
                uint8_t* m3 = FindCodePattern(nexus, 0x1AF907, pat_c3, "xxxxxxxxx????xxxxxxxxxxxx", sizeof(pat_c3));
                uint8_t* calls[3] = {
                    m1 ? (m1 + 10) : nullptr,
                    m2 ? (m2 + 10) : nullptr,
                    m3 ? (m3 + 8)  : nullptr
                };
                for (int i = 0; i < 3; ++i) {
                    if (!calls[i]) continue;
                    void* tgt = ResolveRelCallTarget(calls[i]);
                    if (!tgt) continue;
                    if (!g_orig_image_get_size) {
                        g_orig_image_get_size = reinterpret_cast<FnImageGetSize>(tgt);
                    }
                    if (reinterpret_cast<FnImageGetSize>(tgt) == g_orig_image_get_size) {
                        PatchRelCall(calls[i], &HookRendWinCenterImageGetSize);
                    }
                }
            }

            // Patch Spotlight WipeBlit call inside RendWinPaint.
            if (p_rw_wipe) {
                uint8_t* wipe_scan_end = p_rw_wipe + 0x280;
                if (g_nexus_text_end && wipe_scan_end > g_nexus_text_end) {
                    wipe_scan_end = const_cast<uint8_t*>(g_nexus_text_end);
                }
                static const uint8_t pat_wipe_mode[10] = {
                    0x8B, 0x05, 0, 0, 0, 0, 0x85, 0xC0, 0x75, 0x33
                };
                static const uint8_t pat_wipe_mxy[9] = {
                    0x83, 0x3D, 0, 0, 0, 0, 0xFF, 0x0F, 0x84
                };
                static const uint8_t pat_wipe_rad[14] = {
                    0x41, 0x8B, 0xD5, 0x8B, 0x0D, 0, 0, 0, 0,
                    0x2B, 0xD1, 0x83, 0xC2, 0xFD
                };

                uint8_t* m_mode = FindPatternInRange(
                    p_rw_wipe, wipe_scan_end, pat_wipe_mode, "xx????xxxx", sizeof(pat_wipe_mode));
                if (m_mode) {
                    int32_t d = *reinterpret_cast<int32_t*>(m_mode + 2);
                    g_rw_wipe_mode = reinterpret_cast<int*>(m_mode + 6 + d);
                }

                uint8_t* m_mx = FindPatternInRange(
                    p_rw_wipe, wipe_scan_end, pat_wipe_mxy, "xx????xxx", sizeof(pat_wipe_mxy));
                if (m_mx) {
                    int32_t dx = *reinterpret_cast<int32_t*>(m_mx + 2);
                    g_rw_spotlight_mx = reinterpret_cast<int*>(m_mx + 7 + dx);
                    uint8_t* m_my = FindPatternInRange(
                        m_mx + 7, wipe_scan_end, pat_wipe_mxy, "xx????xxx", sizeof(pat_wipe_mxy));
                    if (m_my) {
                        int32_t dy = *reinterpret_cast<int32_t*>(m_my + 2);
                        g_rw_spotlight_my = reinterpret_cast<int*>(m_my + 7 + dy);
                    }
                }

                uint8_t* m_rad = FindPatternInRange(
                    p_rw_wipe, wipe_scan_end, pat_wipe_rad, "xxxxx????xxxxx", sizeof(pat_wipe_rad));
                if (m_rad) {
                    int32_t dr = *reinterpret_cast<int32_t*>(m_rad + 5);
                    g_rw_spotlight_radius = reinterpret_cast<int*>(m_rad + 9 + dr);
                }

                if (g_rw_wipe_mode && g_rw_spotlight_mx && g_rw_spotlight_my && g_rw_spotlight_radius) {
                    auto* paint_end = const_cast<uint8_t*>(GetFunctionEndOrFallback(p_rw_paint, 0xB00));
                    for (uint8_t* cur = p_rw_paint; cur + 5 <= paint_end; ++cur) {
                        if (cur[0] == 0xE8 && ResolveRelCallTarget(cur) == p_rw_wipe) {
                            g_orig_rw_wipe_blit = reinterpret_cast<FnRendWinWipeBlit>(p_rw_wipe);
                            PatchRelCall(cur, &HookRendWinWipeBlit);
                            break;
                        }
                    }
                }
            }

            if (is_modo16) {
                static const uint8_t pat_rw_sc16[17] = {
                    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x48, 0x08, 0x41, 0x54,
                    0x41, 0x55, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x70
                };
                uint8_t* p_sc16 = FindCodePattern(nexus, 0x35CB10, pat_rw_sc16, nullptr, sizeof(pat_rw_sc16));
                if (p_sc16) {
                    if (p_sc16[0x27] == 0x48 && p_sc16[0x28] == 0x8B && p_sc16[0x29] == 0x0D) {
                        int32_t pdisp = *reinterpret_cast<int32_t*>(p_sc16 + 0x2A);
                        g_rendwin_pane_ptr = reinterpret_cast<void**>(p_sc16 + 0x2E + pdisp);
                    }
                    g_orig_rendwin_paint       = BuildCleanTrampoline<FnRendWinPaint>(p_rw_paint, 15);
                    g_orig_rw_get_scale_offset = BuildCleanTrampoline<FnRendWinGetScaleAndOffset>(p_sc16, 17);
                    if (g_orig_rendwin_paint && g_orig_rw_get_scale_offset) {
                        PatchJump14(p_rw_paint, &HookRendWinPaint);
                        PatchJump14(p_sc16,     &HookRendWinGetScaleAndOffset);
                    }
                }
            } else {
                static const uint8_t pat_rw_center_norm[46] = {
                    0x48, 0x8B, 0xC4, 0x53, 0x48, 0x81, 0xEC, 0xC0, 0x00, 0x00, 0x00,
                    0x0F, 0x29, 0x70, 0xE8, 0x0F, 0x29, 0x78, 0xD8, 0x44, 0x0F, 0x29,
                    0x40, 0xC8, 0x44, 0x0F, 0x29, 0x48, 0xB8, 0x44, 0x0F, 0x29, 0x50,
                    0xA8, 0x44, 0x0F, 0x29, 0x58, 0x98, 0x44, 0x0F, 0x28, 0xC1, 0x44,
                    0x0F, 0x28
                };
                static const uint8_t pat_rw_pane2norm[21] = {
                    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18,
                    0x48, 0x89, 0x7C, 0x24, 0x20, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x70
                };
                static const uint8_t pat_rw_norm2pane[21] = {
                    0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x81, 0xEC, 0x90, 0x00,
                    0x00, 0x00, 0x0F, 0x29, 0xB4, 0x24, 0x80, 0x00, 0x00, 0x00
                };
                uint8_t* p_c_norm = FindCodePattern(nexus, 0x1BA9F0, pat_rw_center_norm, nullptr, sizeof(pat_rw_center_norm));
                uint8_t* p_p2norm = FindCodePattern(nexus, 0x1BAC30, pat_rw_pane2norm,   nullptr, sizeof(pat_rw_pane2norm));
                uint8_t* p_norm2p = FindCodePattern(nexus, 0x1BAE90, pat_rw_norm2pane,   nullptr, sizeof(pat_rw_norm2pane));
                if (p_c_norm && p_p2norm && p_norm2p) {
                    if (p_p2norm[0x2C] == 0x48 && p_p2norm[0x2D] == 0x8B && p_p2norm[0x2E] == 0x0D) {
                        int32_t pdisp = *reinterpret_cast<int32_t*>(p_p2norm + 0x2F);
                        g_rendwin_pane_ptr = reinterpret_cast<void**>(p_p2norm + 0x33 + pdisp);
                    }
                    g_orig_rendwin_paint      = BuildCleanTrampoline<FnRendWinPaint>(p_rw_paint, 15);
                    g_orig_rw_center_at_norm  = BuildCleanTrampoline<FnRendWinCenterAtNormalized>(p_c_norm, 15);
                    g_orig_rw_pane_to_norm    = BuildCleanTrampoline<FnRendWinPaneToNormalized>(p_p2norm, 15);
                    g_orig_rw_norm_to_pane    = BuildCleanTrampoline<FnRendWinNormalizedToPane>(p_norm2p, 21);
                    if (g_orig_rendwin_paint && g_orig_rw_center_at_norm &&
                        g_orig_rw_pane_to_norm && g_orig_rw_norm_to_pane) {
                        PatchJump14(p_rw_paint, &HookRendWinPaint);
                        PatchJump14(p_c_norm,   &HookRendWinCenterAtNormalized);
                        PatchJump14(p_p2norm,   &HookRendWinPaneToNormalized);
                        PatchJump14(p_norm2p,   &HookRendWinNormalizedToPane);
                    }
                }
            }
        }
    }

    // Problem 3.4: Third-party .lx IPR IAT hooks & dynamic DLL load listener
    InstallLxModuleHooks();

    g_hooked = true;
}

// ============================================================================
// 9. Exported C Entry Points
// ============================================================================

// Problem 2: Force QtGui to evaluate and cache QT_FONT_DPI inside its function-local
// static inside QPlatformScreen::overrideDpi before Python restores the environment.
extern "C" __declspec(dllexport) void LockQtOverrideDpi() {
    struct TargetSym {
        const char* dll_name;
        const char* mangled;
    };
    static const TargetSym kTargets[] = {
        { "Qt5Gui.dll", "?overrideDpi@QPlatformScreen@@SA?AU?$QPair@NN@@AEBU2@@Z" },
        { "Qt6Gui.dll", "?overrideDpi@QPlatformScreen@@SA?AU?$pair@NN@std@@AEBU23@@Z" },
    };
    for (size_t i = 0; i < sizeof(kTargets) / sizeof(kTargets[0]); ++i) {
        HMODULE hmod = GetModuleHandleA(kTargets[i].dll_name);
        if (!hmod) continue;
        auto fn = reinterpret_cast<FnOverrideDpi>(GetProcAddress(hmod, kTargets[i].mangled));
        if (!fn) continue;
        DpiPairC out_p = { 0.0, 0.0 };
        DpiPairC in_p  = { 96.0, 96.0 };
        fn(&out_p, &in_p);
        return;
    }
}

extern "C" __declspec(dllexport) void SetHiDPIConfig(double scale, int dynamic_pmv2) {
    if (scale >= 1.0 && scale <= 4.0) {
        g_scale         = scale;
        g_active_ui_dpr = scale;
        g_last_gl_dpr   = scale;
    }
    g_pmv2_mode = (dynamic_pmv2 != 0);

    if (g_pmv2_mode) {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32) {
            auto fn_set_ctx = reinterpret_cast<FnSetThreadDpiAwarenessContext>(
                GetProcAddress(user32, "SetThreadDpiAwarenessContext"));
            if (fn_set_ctx) {
                // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4
                fn_set_ctx(reinterpret_cast<HANDLE>(static_cast<intptr_t>(-4)));
            }
        }
    }

    if (IsHighDpiActive()) {
        InstallAllHooks();
    }
}

extern "C" __declspec(dllexport) void SetHiDPIScale(double scale) {
    SetHiDPIConfig(scale, g_pmv2_mode ? 1 : 0);
}

#ifndef HIDPI_UNIT_TEST
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    (void)hinstDLL;
    if (fdwReason == DLL_PROCESS_DETACH && lpvReserved == nullptr) {
        if (g_ldr_cookie) {
            HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
            if (ntdll) {
                auto fn_unreg = reinterpret_cast<FnLdrUnregisterDllNotification>(
                    GetProcAddress(ntdll, "LdrUnregisterDllNotification"));
                if (fn_unreg) {
                    fn_unreg(g_ldr_cookie);
                }
            }
            g_ldr_cookie = nullptr;
        }
        if (g_rot_buf != nullptr) {
            VirtualFree(g_rot_buf, 0, MEM_RELEASE);
            g_rot_buf = nullptr;
            g_rot_cap = 0;
        }
        FreeFboTable();
    }
    return TRUE;
}
#endif
