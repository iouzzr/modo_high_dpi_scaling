// Copyright 2026 iouzzr
// SPDX-License-Identifier: Apache-2.0

// high_dpi_scaling.cpp - High-DPI 3D Viewport & 2D UI (OPALDisp) Patch DLL for Foundry Modo (16.x & 17.x)
// Hooks QOpenGLFramebufferObject, AGLViewport / AGLScissor, and QImage / QPaintDevice / QPainter / QFont
// across Modo 16 (front3000.dll + frame3000.dll + Qt5Gui.dll) and Modo 17 (modo.exe + Qt6Gui.dll + Qt6OpenGL.dll)
// to render both 3D viewports and 2D UI panels/text at native High-DPI physical resolution.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <intrin.h>

#pragma intrinsic(_ReturnAddress, _AddressOfReturnAddress)

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
typedef int   (*FnDim)(const void* this_ptr);
typedef void  (*FnFboBlit)(void* target, const QRectC* targetRect, void* source, const QRectC* sourceRect, uint32_t buffers, uint32_t filter);
typedef void  (*FnRect4)(int x, int y, int w, int h);

// 2D UI (OPALDisp / QImage / QPaintDevice / QPainter / QFont) function pointer types
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
typedef void           (*FnSetDpr)(void* this_ptr, double dpr);
typedef double         (*FnGetDpr)(const void* this_ptr);

static FnFboCtor              g_orig_fbo_ctor        = nullptr;
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

static FnSetDpr               g_qimg_set_dpr         = nullptr;
static FnGetDpr               g_qimg_get_dpr         = nullptr;
static FnGetDpr               g_qpd_get_dpr          = nullptr;

static const uint8_t*         g_ret_rot_ccw          = nullptr;
static const uint8_t*         g_ret_rot_cw           = nullptr;
static double                 g_scale                = 1.0;
static bool                   g_hooked               = false;

// High-DPI buffer for rotated vertical tab strips (ORastToImage -> 90 deg rotation -> DispBlitImage)
static uint32_t*              g_rot_buf              = nullptr;
static size_t                 g_rot_cap              = 0;
static int                    g_rot_lw               = 0;
static int                    g_rot_lh               = 0;
static int                    g_rot_pw               = 0;
static int                    g_rot_ph               = 0;
static bool                   g_rot_valid            = false;

struct FboRecord {
    const void* ptr;
    int logical_w;
    int logical_h;
};

static FboRecord g_fbo_table[64];
static uint32_t  g_fbo_next = 0;

static int ScaleInt(int v) {
    if (g_scale <= 1.001) return v;
    if (v >= 0) return static_cast<int>(v * g_scale + 0.5);
    return static_cast<int>(v * g_scale - 0.5);
}

static int UnscaleInt(int v) {
    if (g_scale <= 1.001) return v;
    if (v >= 0) return static_cast<int>(v / g_scale + 0.499999);
    return static_cast<int>(v / g_scale - 0.499999);
}

static void RecordFbo(const void* ptr, int w, int h) {
    for (uint32_t i = 0; i < 64; ++i) {
        if (g_fbo_table[i].ptr == ptr) {
            g_fbo_table[i].logical_w = w;
            g_fbo_table[i].logical_h = h;
            return;
        }
    }
    uint32_t idx = (g_fbo_next++) & 63;
    g_fbo_table[idx].ptr = ptr;
    g_fbo_table[idx].logical_w = w;
    g_fbo_table[idx].logical_h = h;
}

// ============================================================================
// 1. 3D OpenGL Viewport Hooks
//    Modo creates 3D viewport FBOs at logical widget size, queries FBO
//    width()/height() for logical layout/camera checks, and issues AGLViewport,
//    AGLScissor, and blitFramebuffer in logical coordinates. We allocate the
//    FBO at physical High-DPI size, return the recorded logical size from
//    width()/height(), and scale viewport/scissor/blit rects to physical pixels.
// ============================================================================

static void* HookFboCtor(void* this_ptr, int w, int h, const void* fmt_ptr) {
    RecordFbo(this_ptr, w, h);
    return g_orig_fbo_ctor(this_ptr, ScaleInt(w), ScaleInt(h), fmt_ptr);
}

static int HookFboWidth(const void* this_ptr) {
    int pw = g_orig_fbo_width(this_ptr);
    if (g_scale <= 1.001) return pw;
    for (uint32_t i = 0; i < 64; ++i) {
        if (g_fbo_table[i].ptr == this_ptr && ScaleInt(g_fbo_table[i].logical_w) == pw) {
            return g_fbo_table[i].logical_w;
        }
    }
    return UnscaleInt(pw);
}

static int HookFboHeight(const void* this_ptr) {
    int ph = g_orig_fbo_height(this_ptr);
    if (g_scale <= 1.001) return ph;
    for (uint32_t i = 0; i < 64; ++i) {
        if (g_fbo_table[i].ptr == this_ptr && ScaleInt(g_fbo_table[i].logical_h) == ph) {
            return g_fbo_table[i].logical_h;
        }
    }
    return UnscaleInt(ph);
}

static void HookFboBlit(void* target, const QRectC* tr, void* source, const QRectC* sr, uint32_t buffers, uint32_t filter) {
    if (g_scale <= 1.001) {
        g_orig_fbo_blit(target, tr, source, sr, buffers, filter);
        return;
    }
    int tw = tr->x2 - tr->x1 + 1;
    int th = tr->y2 - tr->y1 + 1;
    int sw = sr->x2 - sr->x1 + 1;
    int sh = sr->y2 - sr->y1 + 1;
    int tx1 = ScaleInt(tr->x1);
    int ty1 = ScaleInt(tr->y1);
    int sx1 = ScaleInt(sr->x1);
    int sy1 = ScaleInt(sr->y1);
    QRectC scaled_t = { tx1, ty1, tx1 + ScaleInt(tw) - 1, ty1 + ScaleInt(th) - 1 };
    QRectC scaled_s = { sx1, sy1, sx1 + ScaleInt(sw) - 1, sy1 + ScaleInt(sh) - 1 };
    g_orig_fbo_blit(target, &scaled_t, source, &scaled_s, buffers, filter);
}

static void HookViewport(int x, int y, int w, int h) {
    if (g_orig_viewport) {
        g_orig_viewport(ScaleInt(x), ScaleInt(y), ScaleInt(w), ScaleInt(h));
    } else if (g_agl_table && *g_agl_table) {
        reinterpret_cast<FnRect4>((*g_agl_table)[0xF0 / 8])(ScaleInt(x), ScaleInt(y), ScaleInt(w), ScaleInt(h));
    }
}

static void HookScissor(int x, int y, int w, int h) {
    if (g_orig_scissor) {
        g_orig_scissor(ScaleInt(x), ScaleInt(y), ScaleInt(w), ScaleInt(h));
    } else if (g_agl_table && *g_agl_table) {
        reinterpret_cast<FnRect4>((*g_agl_table)[0x460 / 8])(ScaleInt(x), ScaleInt(y), ScaleInt(w), ScaleInt(h));
    }
}

// ============================================================================
// 2. 2D UI (OPALDisp / QImage / QPaintDevice / QPainter / QFont) Hooks
//    Modo's 2D panels, forms, trees, and buttons are rendered by OPAL into an
//    offscreen QImage buffer (LUXQtWidget::m_image at [widget+0x48]) and then
//    blitted onto the QWidget in LUXQtWidget::paintEvent. By default, Modo
//    allocates these offscreen QImages at 1x logical pixel size with DPR=1.0.
// ============================================================================

// LUXQtWidget::updateImage (front3000.dll+0x172077) constructs its main
// offscreen backing QImage via QImage(const QSize&, Format). Allocate it at
// physical High-DPI dimensions and tag it with setDevicePixelRatio(g_scale).
static void* HookQImageCtorSize(void* this_ptr, const QSizeC* sz, int fmt) {
    if (g_scale > 1.001 && sz && g_qimg_set_dpr) {
        QSizeC scaled_sz = { ScaleInt(sz->w), ScaleInt(sz->h) };
        void* res = g_orig_qimg_ctor_size(this_ptr, &scaled_sz, fmt);
        g_qimg_set_dpr(this_ptr, g_scale);
        return res;
    }
    return g_orig_qimg_ctor_size(this_ptr, sz, fmt);
}

// ORastCreate (front3000.dll+0x1B60D0, return address +0x1B60D6) allocates
// temporary offscreen ARGB32 (fmt == 5) QImages via QImage(int w, int h, Format)
// for rasterizing text/tabs. Only scale calls originating from ORastCreate so
// raw icon/texture assets loaded elsewhere via QImage(w, h, fmt) stay 1:1.
static void* HookQImageCtorWH(void* this_ptr, int w, int h, int fmt) {
    if (g_scale > 1.001 && fmt == 5 && g_qimg_set_dpr) {
        const uint8_t* ret = reinterpret_cast<const uint8_t*>(_ReturnAddress());
        // Verify return site in ORastCreate: lea rax, [rip+disp32]; mov [rdi], rax
        if (ret && ret[0] == 0x48 && ret[1] == 0x8D && ret[2] == 0x05 &&
            ret[7] == 0x48 && ret[8] == 0x89 && ret[9] == 0x07) {
            void* res = g_orig_qimg_ctor_wh(this_ptr, ScaleInt(w), ScaleInt(h), fmt);
            g_qimg_set_dpr(this_ptr, g_scale);
            return res;
        }
    }
    return g_orig_qimg_ctor_wh(this_ptr, w, h, fmt);
}

// Report logical dimensions back to Modo for our High-DPI QImages (dpr > 1.0)
// so LUXQtWidget::updateImage (0x171F8C..0x172000) and OPAL layout math
// continue operating in logical coordinates without infinite resize loops.
static QSizeC* HookQImageSize(const void* this_ptr, QSizeC* out_sz) {
    QSizeC* res = g_orig_qimg_size(this_ptr, out_sz);
    if (g_scale > 1.001 && out_sz && g_qimg_get_dpr && g_qimg_get_dpr(this_ptr) > 1.001) {
        out_sz->w = UnscaleInt(out_sz->w);
        out_sz->h = UnscaleInt(out_sz->h);
    }
    return res;
}

static int HookQImageWidth(const void* this_ptr) {
    int pw = g_orig_qimg_width(this_ptr);
    if (g_scale > 1.001 && g_qimg_get_dpr && g_qimg_get_dpr(this_ptr) > 1.001) {
        return UnscaleInt(pw);
    }
    return pw;
}

// Modo renders vertical tab strips by first drawing horizontal text into an
// ORast QImage, converting it into a logical-sized ILxImage in QImageToILxImage
// (front3000.dll+0x1A29E0, first querying QImage::height at +0x1A29FB), rotating
// the ILxImage 90 degrees on the CPU (CCW at +0x1F6C0D -> ret +0x1F6C12, or CW
// at +0x19733C -> ret +0x197341), and finally blitting via QPainter::drawImage.
// To prevent downsampling to 1x logical pixels during QImageToILxImage, we
// pre-rotate the full physical High-DPI QImage into g_rot_buf here and substitute
// it in HookDrawImageXY below.
static int HookQImageHeight(const void* this_ptr) {
    int ph = g_orig_qimg_height(this_ptr);
    if (g_scale > 1.001 && g_qimg_get_dpr && g_qimg_get_dpr(this_ptr) > 1.001) {
        int lh = UnscaleInt(ph);
        const uint8_t* ret = reinterpret_cast<const uint8_t*>(_ReturnAddress());
        // Verify return site at start of QImageToILxImage: mov rcx, rsi; mov ebx, eax; call QImage::width
        bool is_q2i = ret && ret[0] == 0x48 && ret[1] == 0x8B && ret[2] == 0xCE &&
                      ret[3] == 0x8B && ret[4] == 0xD8 && ret[5] == 0xFF && ret[6] == 0x15;
        if (is_q2i && g_orig_qimg_width && g_orig_qimg_pixel) {
            const uint8_t* const* stack = reinterpret_cast<const uint8_t* const*>(_AddressOfReturnAddress());
            int rot_mode = 0; // 1 = 90 deg CCW, 2 = 90 deg CW
            for (int i = 1; i <= 20; ++i) {
                if (g_ret_rot_ccw && stack[i] == g_ret_rot_ccw) {
                    rot_mode = 1;
                    break;
                }
                if (g_ret_rot_cw && stack[i] == g_ret_rot_cw) {
                    rot_mode = 2;
                    break;
                }
            }
            if (rot_mode != 0) {
                int pw = g_orig_qimg_width(this_ptr);
                int lw = UnscaleInt(pw);
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
                        g_rot_lw = lh;
                        g_rot_lh = lw;
                        g_rot_pw = ph;
                        g_rot_ph = pw;
                        g_rot_valid = true;
                    }
                }
            }
        }
        return lh;
    }
    return ph;
}

// Check QPaintDevice::devType() (vtable[1]) == QInternal::Image (3)
static bool IsQImageDevice(const void* this_ptr) {
    if (!this_ptr) return false;
    void* const* vtable = *reinterpret_cast<void* const* const*>(this_ptr);
    if (!vtable || !vtable[1]) return false;
    typedef int (*FnDevType)(const void*);
    return reinterpret_cast<FnDevType>(vtable[1])(this_ptr) == 3;
}

static int HookQPaintDeviceWidth(const void* this_ptr) {
    int pw = g_orig_qpd_width(this_ptr);
    if (g_scale > 1.001 && g_qpd_get_dpr && IsQImageDevice(this_ptr) && g_qpd_get_dpr(this_ptr) > 1.001) {
        return UnscaleInt(pw);
    }
    return pw;
}

static int HookQPaintDeviceHeight(const void* this_ptr) {
    int ph = g_orig_qpd_height(this_ptr);
    if (g_scale > 1.001 && g_qpd_get_dpr && IsQImageDevice(this_ptr) && g_qpd_get_dpr(this_ptr) > 1.001) {
        return UnscaleInt(ph);
    }
    return ph;
}

// QImage::copy and QImage::pixel operate in physical image coordinates in Qt,
// whereas Modo passes logical coordinates when cropping/sampling its offscreen QImage.
static void* HookQImageCopy(const void* this_ptr, void* out_img, int x, int y, int w, int h) {
    if (g_scale > 1.001 && g_qimg_get_dpr && g_qimg_set_dpr && g_qimg_get_dpr(this_ptr) > 1.001) {
        void* res = g_orig_qimg_copy(this_ptr, out_img, ScaleInt(x), ScaleInt(y), ScaleInt(w), ScaleInt(h));
        g_qimg_set_dpr(out_img, g_scale);
        return res;
    }
    return g_orig_qimg_copy(this_ptr, out_img, x, y, w, h);
}

static uint32_t HookQImagePixel(const void* this_ptr, int x, int y) {
    if (g_scale > 1.001 && g_qimg_get_dpr && g_qimg_get_dpr(this_ptr) > 1.001) {
        return g_orig_qimg_pixel(this_ptr, ScaleInt(x), ScaleInt(y));
    }
    return g_orig_qimg_pixel(this_ptr, x, y);
}

// In Qt5, QPainter::drawImage(targetRect, image, sourceRect) interprets targetRect
// in logical painter coordinates but sourceRect in physical QImage pixels even when
// image.devicePixelRatio() > 1.0. Scale sourceRect from logical to physical pixels.
static void HookDrawImageRectRect(void* painter, const QRectC* tr, const void* img, const QRectC* sr, int flags) {
    if (g_scale > 1.001 && sr && img && g_qimg_get_dpr && g_qimg_get_dpr(img) > 1.001) {
        int sw = sr->x2 - sr->x1 + 1;
        int sh = sr->y2 - sr->y1 + 1;
        int sx1 = ScaleInt(sr->x1);
        int sy1 = ScaleInt(sr->y1);
        QRectC scaled_s = { sx1, sy1, sx1 + ScaleInt(sw) - 1, sy1 + ScaleInt(sh) - 1 };
        g_orig_draw_image_rr(painter, tr, img, &scaled_s, flags);
        return;
    }
    g_orig_draw_image_rr(painter, tr, img, sr, flags);
}

// Intercept the final blit of rotated vertical tab strips (captured in HookQImageHeight)
// and draw the pre-rotated physical High-DPI buffer (g_rot_buf) instead of the 1x ILxImage.
static void HookDrawImageXY(void* painter, int x, int y, const void* img, int sx, int sy, int sw, int sh, int flags) {
    if (g_scale > 1.001 && g_rot_valid && sx == 0 && sy == 0 && sw == g_rot_lw && sh == g_rot_lh &&
        g_orig_qimg_ctor_wh && g_qimg_dtor && g_qimg_scanline && g_qimg_set_dpr && g_orig_draw_image_rr) {
        g_rot_valid = false;
        alignas(16) uint8_t tmp_qimg[64];
        g_orig_qimg_ctor_wh(tmp_qimg, g_rot_pw, g_rot_ph, 5);
        g_qimg_set_dpr(tmp_qimg, g_scale);
        for (int ry = 0; ry < g_rot_ph; ++ry) {
            uint32_t* dst = reinterpret_cast<uint32_t*>(g_qimg_scanline(tmp_qimg, ry));
            const uint32_t* src = g_rot_buf + static_cast<size_t>(ry) * g_rot_pw;
            if (dst) {
                for (int rx = 0; rx < g_rot_pw; ++rx) {
                    dst[rx] = src[rx];
                }
            }
        }
        QRectC tr = { x, y, x + sw - 1, y + sh - 1 };
        QRectC sr = { 0, 0, g_rot_pw - 1, g_rot_ph - 1 };
        g_orig_draw_image_rr(painter, &tr, tmp_qimg, &sr, flags);
        g_qimg_dtor(tmp_qimg);
        return;
    }
    g_orig_draw_image_xy(painter, x, y, img, sx, sy, sw, sh, flags);
}

// OPALDisp's 2D grid primitives (FillRect, DrawLine, DrawPoint in dispatch table
// front3000.dll+0x1B5BE0) are all converted to QPainter::fillRect below so that
// adjacent logical cells k and k+1 share the exact half-open physical interval
// [round(k * g_scale), round((k + 1) * g_scale)) with zero 1px gaps or overlaps.

// OPALDisp::FillRect subtracts 1 from width/height before calling drawRects with Qt::NoPen. Restore +1.
static void HookDrawRects(void* painter, const QRectC* rects, int count) {
    if (g_scale > 1.001 && painter && rects && count == 1 && g_painter_fill_rect && g_painter_brush) {
        const uint8_t* ret = reinterpret_cast<const uint8_t*>(_ReturnAddress());
        // Verify return site in OPALDisp::FillRect: lea rcx, [rsp+38h]
        if (ret && ret[0] == 0x48 && ret[1] == 0x8D && ret[2] == 0x4C && ret[3] == 0x24 && ret[4] == 0x38) {
            QRectC full_rect = { rects[0].x1, rects[0].y1, rects[0].x2 + 1, rects[0].y2 + 1 };
            g_painter_fill_rect(painter, &full_rect, g_painter_brush(painter));
            return;
        }
    }
    g_orig_draw_rects(painter, rects, count);
}

// OPALDisp::DrawLine sets QBrush(color) and a 1-physical-pixel cosmetic QPen (width=0).
// Convert axis-aligned lines to 1-logical-pixel fillRects to prevent 1px unpainted gaps.
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
    if (g_scale > 1.001 && painter && lines && count == 1 && g_painter_fill_rect && g_painter_brush) {
        const uint8_t* ret = reinterpret_cast<const uint8_t*>(_ReturnAddress());
        if (ret && ret[0] == 0x48 && ret[1] == 0x8B && ret[2] == 0x4B && ret[3] == 0x18) {
            if (TryFillAxisLine(painter, lines[0].x1, lines[0].y1, lines[0].x2, lines[0].y2)) {
                return;
            }
        }
    }
    g_orig_draw_lines(painter, lines, count);
}

static void HookDrawLine4(void* painter, int x1, int y1, int x2, int y2) {
    if (g_scale > 1.001 && painter && g_painter_fill_rect && g_painter_brush) {
        if (TryFillAxisLine(painter, x1, y1, x2, y2)) {
            return;
        }
    }
    g_orig_draw_line4(painter, x1, y1, x2, y2);
}

// OPALDisp::DrawPoint uses a 1-physical-pixel cosmetic pen to erase square FillRect
// corners and draw checkmarks/arrows. Convert to a 1x1 logical cell fillRect.
static void HookDrawPoints(void* painter, const QSizeC* points, int count) {
    if (g_scale > 1.001 && painter && points && count == 1 && g_painter_fill_rect && g_painter_brush) {
        const uint8_t* ret = reinterpret_cast<const uint8_t*>(_ReturnAddress());
        if (ret && ret[0] == 0xC7 && ret[1] == 0x44 && ret[2] == 0x24 && ret[3] == 0x20) {
            QRectC full_rect = { points[0].w, points[0].h, points[0].w, points[0].h };
            g_painter_fill_rect(painter, &full_rect, g_painter_brush(painter));
            return;
        }
    }
    g_orig_draw_points(painter, points, count);
}

static void HookDrawPoint2(void* painter, int x, int y) {
    if (g_scale > 1.001 && painter && g_painter_fill_rect && g_painter_brush) {
        QRectC full_rect = { x, y, x, y };
        g_painter_fill_rect(painter, &full_rect, g_painter_brush(painter));
        return;
    }
    g_orig_draw_point2(painter, x, y);
}

// Modo calls QPainter::setWindow(0, 0, logical_w, logical_h) when initializing a
// QPainter on an OPAL QImage. Because Qt's default viewport on a DPR-scaled QImage
// is already (physical_w / dpr) == logical_w, passing logical_w to setWindow would
// apply DPR scaling a second time unless setWindow is given physical dimensions.
static void HookPainterSetWindow(void* painter, int x, int y, int w, int h) {
    if (g_scale > 1.001 && painter && g_painter_device && g_qpd_get_dpr) {
        void* dev = g_painter_device(painter);
        if (dev && IsQImageDevice(dev) && g_qpd_get_dpr(dev) > 1.001) {
            g_orig_set_window(painter, x, y, ScaleInt(w), ScaleInt(h));
            return;
        }
    }
    g_orig_set_window(painter, x, y, w, h);
}

// Modo explicitly sets QFont::NoAntialias (0x100) on UI fonts, which produces
// jagged bitmap-like glyphs on High-DPI displays. Upgrade 0x100 to
// QFont::PreferAntialias (0x80) when High-DPI scaling is active.
static void HookFontSetStyleStrategy(void* font_ptr, int strategy) {
    if (g_scale > 1.001 && strategy == 0x100) {
        // Replace QFont::NoAntialias (0x100) with QFont::PreferAntialias (0x80) at High-DPI
        strategy = 0x80;
    }
    g_orig_font_style(font_ptr, strategy);
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

static void PatchSlot(void** slot, void* new_fn, void** out_orig) {
    if (!slot) return;
    if (!*out_orig) {
        *out_orig = *slot;
    }
    DWORD old_prot = 0;
    if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_prot)) {
        *slot = new_fn;
        VirtualProtect(slot, sizeof(void*), old_prot, &old_prot);
    }
}

static bool MatchPattern(const uint8_t* p, const uint8_t* pat, const char* mask, size_t len) {
    for (size_t j = 0; j < len; ++j) {
        if (mask[j] == 'x' && p[j] != pat[j]) return false;
    }
    return true;
}

static uint8_t* FindCodePattern(uint8_t* base, size_t hint_rva, const uint8_t* pat, const char* mask, size_t len) {
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    auto* sec = IMAGE_FIRST_SECTION(nt);
    uint8_t* start = base + sec[0].VirtualAddress;
    size_t size = sec[0].Misc.VirtualSize;
    if (size < len) return nullptr;
    if (hint_rva >= sec[0].VirtualAddress && hint_rva + len <= sec[0].VirtualAddress + size) {
        if (MatchPattern(base + hint_rva, pat, mask, len)) {
            return base + hint_rva;
        }
    }
    for (size_t i = 0; i <= size - len; ++i) {
        if (MatchPattern(start + i, pat, mask, len)) return start + i;
    }
    return nullptr;
}

static void PatchJump14(uint8_t* target, void* dst_fn) {
    DWORD old_prot = 0;
    if (VirtualProtect(target, 14, PAGE_EXECUTE_READWRITE, &old_prot)) {
        target[0] = 0xFF;
        target[1] = 0x25;
        *reinterpret_cast<uint32_t*>(target + 2) = 0;
        *reinterpret_cast<void**>(target + 6) = dst_fn;
        VirtualProtect(target, 14, old_prot, &old_prot);
        FlushInstructionCache(GetCurrentProcess(), target, 14);
    }
}

static void InstallAllHooks() {
    if (g_hooked) return;

    // Modo 16 splits UI/GL into front3000.dll + frame3000.dll + Qt5Gui.dll;
    // Modo 17 statically links front/frame into modo.exe and uses Qt6Gui.dll.
    auto* front = reinterpret_cast<uint8_t*>(GetModuleHandleA("front3000.dll"));
    if (!front) front = reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr));
    auto* frame = reinterpret_cast<uint8_t*>(GetModuleHandleA("frame3000.dll"));
    if (!frame) frame = front;
    HMODULE qtgui = GetModuleHandleA("Qt5Gui.dll");
    if (!qtgui) qtgui = GetModuleHandleA("Qt6Gui.dll");
    if (!front || !frame || !qtgui) return;

    g_qimg_set_dpr      = reinterpret_cast<FnSetDpr>(GetProcAddress(qtgui, "?setDevicePixelRatio@QImage@@QEAAXN@Z"));
    g_qimg_get_dpr      = reinterpret_cast<FnGetDpr>(GetProcAddress(qtgui, "?devicePixelRatio@QImage@@QEBANXZ"));
    g_qpd_get_dpr       = reinterpret_cast<FnGetDpr>(GetProcAddress(qtgui, "?devicePixelRatioF@QPaintDevice@@QEBANXZ"));
    if (!g_qpd_get_dpr) {
        g_qpd_get_dpr   = reinterpret_cast<FnGetDpr>(GetProcAddress(qtgui, "?devicePixelRatio@QPaintDevice@@QEBANXZ"));
    }
    g_painter_device    = reinterpret_cast<FnPainterDevice>(GetProcAddress(qtgui, "?device@QPainter@@QEBAPEAVQPaintDevice@@XZ"));
    g_painter_fill_rect = reinterpret_cast<FnFillRectBrush>(GetProcAddress(qtgui, "?fillRect@QPainter@@QEAAXAEBVQRect@@AEBVQBrush@@@Z"));
    g_painter_brush     = reinterpret_cast<FnPainterBrush>(GetProcAddress(qtgui, "?brush@QPainter@@QEBAAEBVQBrush@@XZ"));
    g_qimg_dtor         = reinterpret_cast<FnQImageDtor>(GetProcAddress(qtgui, "??1QImage@@UEAA@XZ"));
    g_qimg_scanline     = reinterpret_cast<FnQImageScanLine>(GetProcAddress(qtgui, "?scanLine@QImage@@QEAAPEAEH@Z"));

    // 1. 3D OpenGL Viewport IAT slots
    void** s_fbo_ctor   = FindIATSlot(front, "??0QOpenGLFramebufferObject@@QEAA@HHAEBVQOpenGLFramebufferObjectFormat@@@Z");
    void** s_fbo_width  = FindIATSlot(front, "?width@QOpenGLFramebufferObject@@QEBAHXZ");
    void** s_fbo_height = FindIATSlot(front, "?height@QOpenGLFramebufferObject@@QEBAHXZ");
    void** s_fbo_blit   = FindIATSlot(front, "?blitFramebuffer@QOpenGLFramebufferObject@@SAXPEAV1@AEBVQRect@@01II@Z");
    void** s_vp         = FindIATSlot(frame, "?AGLViewport@@YAXHHHH@Z");
    void** s_sc         = FindIATSlot(frame, "?AGLScissor@@YAXHHHH@Z");

    // In Modo 17 (merged modo.exe), AGLViewport and AGLScissor are internal 14-byte
    // dispatch thunks: mov rax, [rip+disp32]; jmp qword ptr [rax + 0xF0 / 0x460].
    uint8_t* p_agl_vp = nullptr;
    uint8_t* p_agl_sc = nullptr;
    if (!s_vp || !s_sc) {
        static const uint8_t pat_vp[14] = { 0x48, 0x8B, 0x05, 0, 0, 0, 0, 0x48, 0xFF, 0xA0, 0xF0, 0x00, 0x00, 0x00 };
        static const uint8_t pat_sc[14] = { 0x48, 0x8B, 0x05, 0, 0, 0, 0, 0x48, 0xFF, 0xA0, 0x60, 0x04, 0x00, 0x00 };
        p_agl_vp = FindCodePattern(front, 0xD45F60, pat_vp, "xxx????xxxxxxx", 14);
        p_agl_sc = FindCodePattern(front, 0xD370A0, pat_sc, "xxx????xxxxxxx", 14);
    }

    // 2. 2D UI (OPALDisp) IAT slots
    void** s_qimg_csize = FindIATSlot(front, "??0QImage@@QEAA@AEBVQSize@@W4Format@0@@Z");
    void** s_qimg_cwh   = FindIATSlot(front, "??0QImage@@QEAA@HHW4Format@0@@Z");
    void** s_qimg_size  = FindIATSlot(front, "?size@QImage@@QEBA?AVQSize@@XZ");
    void** s_qimg_w     = FindIATSlot(front, "?width@QImage@@QEBAHXZ");
    void** s_qimg_h     = FindIATSlot(front, "?height@QImage@@QEBAHXZ");
    void** s_qpd_w      = FindIATSlot(front, "?width@QPaintDevice@@QEBAHXZ");
    void** s_qpd_h      = FindIATSlot(front, "?height@QPaintDevice@@QEBAHXZ");
    void** s_qimg_copy  = FindIATSlot(front, "?copy@QImage@@QEBA?AV1@HHHH@Z");
    void** s_qimg_pixel = FindIATSlot(front, "?pixel@QImage@@QEBAIHH@Z");
    void** s_draw_img   = FindIATSlot(front, "?drawImage@QPainter@@QEAAXAEBVQRect@@AEBVQImage@@0V?$QFlags@W4ImageConversionFlag@Qt@@@@@Z");
    void** s_draw_imgxy = FindIATSlot(front, "?drawImage@QPainter@@QEAAXHHAEBVQImage@@HHHHV?$QFlags@W4ImageConversionFlag@Qt@@@@@Z");
    void** s_draw_rects = FindIATSlot(front, "?drawRects@QPainter@@QEAAXPEBVQRect@@H@Z");
    void** s_draw_lines = FindIATSlot(front, "?drawLines@QPainter@@QEAAXPEBVQLine@@H@Z");
    void** s_draw_pts   = FindIATSlot(front, "?drawPoints@QPainter@@QEAAXPEBVQPoint@@H@Z");
    void** s_draw_line4 = FindIATSlot(front, "?drawLine@QPainter@@QEAAXHHHH@Z");
    void** s_draw_pt2   = FindIATSlot(front, "?drawPoint@QPainter@@QEAAXHH@Z");
    void** s_set_win    = FindIATSlot(front, "?setWindow@QPainter@@QEAAXHHHH@Z");
    void** s_font_style = FindIATSlot(front, "?setStyleStrategy@QFont@@QEAAXW4StyleStrategy@1@@Z");

    if (s_fbo_ctor && s_fbo_width && s_fbo_height && s_fbo_blit && ((s_vp && s_sc) || (p_agl_vp && p_agl_sc))) {
        PatchSlot(s_fbo_ctor,   reinterpret_cast<void*>(&HookFboCtor),   reinterpret_cast<void**>(&g_orig_fbo_ctor));
        PatchSlot(s_fbo_width,  reinterpret_cast<void*>(&HookFboWidth),  reinterpret_cast<void**>(&g_orig_fbo_width));
        PatchSlot(s_fbo_height, reinterpret_cast<void*>(&HookFboHeight), reinterpret_cast<void**>(&g_orig_fbo_height));
        PatchSlot(s_fbo_blit,   reinterpret_cast<void*>(&HookFboBlit),   reinterpret_cast<void**>(&g_orig_fbo_blit));
        if (s_vp && s_sc) {
            PatchSlot(s_vp,     reinterpret_cast<void*>(&HookViewport),  reinterpret_cast<void**>(&g_orig_viewport));
            PatchSlot(s_sc,     reinterpret_cast<void*>(&HookScissor),   reinterpret_cast<void**>(&g_orig_scissor));
        } else {
            int32_t disp = *reinterpret_cast<int32_t*>(p_agl_vp + 3);
            g_agl_table = reinterpret_cast<void***>(p_agl_vp + 7 + disp);
            PatchJump14(p_agl_vp, reinterpret_cast<void*>(&HookViewport));
            PatchJump14(p_agl_sc, reinterpret_cast<void*>(&HookScissor));
        }

        if (g_qimg_set_dpr && g_qimg_get_dpr && g_qpd_get_dpr) {
            PatchSlot(s_qimg_csize, reinterpret_cast<void*>(&HookQImageCtorSize),       reinterpret_cast<void**>(&g_orig_qimg_ctor_size));
            PatchSlot(s_qimg_cwh,   reinterpret_cast<void*>(&HookQImageCtorWH),         reinterpret_cast<void**>(&g_orig_qimg_ctor_wh));
            PatchSlot(s_qimg_size,  reinterpret_cast<void*>(&HookQImageSize),           reinterpret_cast<void**>(&g_orig_qimg_size));
            PatchSlot(s_qimg_w,     reinterpret_cast<void*>(&HookQImageWidth),          reinterpret_cast<void**>(&g_orig_qimg_width));
            PatchSlot(s_qimg_h,     reinterpret_cast<void*>(&HookQImageHeight),         reinterpret_cast<void**>(&g_orig_qimg_height));
            PatchSlot(s_qpd_w,      reinterpret_cast<void*>(&HookQPaintDeviceWidth),    reinterpret_cast<void**>(&g_orig_qpd_width));
            PatchSlot(s_qpd_h,      reinterpret_cast<void*>(&HookQPaintDeviceHeight),   reinterpret_cast<void**>(&g_orig_qpd_height));
            PatchSlot(s_qimg_copy,  reinterpret_cast<void*>(&HookQImageCopy),           reinterpret_cast<void**>(&g_orig_qimg_copy));
            PatchSlot(s_qimg_pixel, reinterpret_cast<void*>(&HookQImagePixel),          reinterpret_cast<void**>(&g_orig_qimg_pixel));
            PatchSlot(s_draw_img,   reinterpret_cast<void*>(&HookDrawImageRectRect),    reinterpret_cast<void**>(&g_orig_draw_image_rr));
            PatchSlot(s_draw_imgxy, reinterpret_cast<void*>(&HookDrawImageXY),          reinterpret_cast<void**>(&g_orig_draw_image_xy));
            PatchSlot(s_draw_rects, reinterpret_cast<void*>(&HookDrawRects),            reinterpret_cast<void**>(&g_orig_draw_rects));
            PatchSlot(s_draw_lines, reinterpret_cast<void*>(&HookDrawLines),            reinterpret_cast<void**>(&g_orig_draw_lines));
            PatchSlot(s_draw_pts,   reinterpret_cast<void*>(&HookDrawPoints),           reinterpret_cast<void**>(&g_orig_draw_points));
            PatchSlot(s_draw_line4, reinterpret_cast<void*>(&HookDrawLine4),            reinterpret_cast<void**>(&g_orig_draw_line4));
            PatchSlot(s_draw_pt2,   reinterpret_cast<void*>(&HookDrawPoint2),           reinterpret_cast<void**>(&g_orig_draw_point2));
            PatchSlot(s_set_win,    reinterpret_cast<void*>(&HookPainterSetWindow),     reinterpret_cast<void**>(&g_orig_set_window));
            PatchSlot(s_font_style, reinterpret_cast<void*>(&HookFontSetStyleStrategy), reinterpret_cast<void**>(&g_orig_font_style));
        }

        // Locate rotated vertical tab strip callers (90 deg CCW and CW) once at startup.
        static const uint8_t pat_ccw[21] = {
            0x48, 0x8B, 0x8C, 0x24, 0xA0, 0x00, 0x00, 0x00, 0xE8, 0, 0, 0, 0,
            0x4C, 0x8B, 0xC0, 0x48, 0x8D, 0x15, 0, 0
        };
        static const uint8_t pat_cw[17] = {
            0xE8, 0, 0, 0, 0, 0x44, 0x8B, 0x43, 0x50, 0xB9, 0x04, 0x00, 0x00, 0x00, 0x8B, 0x53, 0x4C
        };
        uint8_t* p_ccw = FindCodePattern(front, (s_vp && s_sc) ? 0x1F6C05 : 0xD0EE45, pat_ccw, "xxxxxxxxx????xxxxxx??", 21);
        uint8_t* p_cw  = FindCodePattern(front, (s_vp && s_sc) ? 0x19733C : 0xCF0683, pat_cw,  "x????xxxxxxxxxxxx", 17);
        if (p_ccw) g_ret_rot_ccw = p_ccw + 13;
        if (p_cw)  g_ret_rot_cw  = p_cw + 5;

        // Prevent LUXQtWidget::paintEvent (Modo 16 +0x1724A0, Modo 17 +0xD70770) from synchronously
        // calling FireLazyHandlers(3, nullptr) before setting m_inPaintEvent = 1.
        // Under High-DPI, if a lazy form layout handler resizes a widget mid-paint, Qt reallocates
        // the window's QBackingStore QImage and frees the QPaintEngine* cached on the stack by
        // QWidgetPrivate::drawWidget, causing a Use-After-Free Access Violation upon return.
        static const uint8_t pat_lazy16[15] = { 0x33, 0xD2, 0x8D, 0x4A, 0x03, 0xE8, 0, 0, 0, 0, 0x41, 0xC6, 0x46, 0x40, 0x01 };
        static const uint8_t pat_lazy17[14] = { 0x33, 0xD2, 0x8D, 0x4A, 0x03, 0xE8, 0, 0, 0, 0, 0xC6, 0x47, 0x38, 0x01 };
        uint8_t* p_lazy = (s_vp && s_sc)
            ? FindCodePattern(front, 0x1724BE, pat_lazy16, "xxxxxx????xxxxx", 15)
            : FindCodePattern(front, 0xD7078C, pat_lazy17, "xxxxxx????xxxx", 14);
        if (p_lazy) {
            uint8_t* p_call = p_lazy + 5;
            DWORD old_prot = 0;
            if (VirtualProtect(p_call, 5, PAGE_EXECUTE_READWRITE, &old_prot)) {
                for (int i = 0; i < 5; ++i) p_call[i] = 0x90;
                VirtualProtect(p_call, 5, old_prot, &old_prot);
                FlushInstructionCache(GetCurrentProcess(), p_call, 5);
            }
        }

        g_hooked = true;
    }
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
    (void)fdwReason;
    (void)lpvReserved;
    return TRUE;
}
