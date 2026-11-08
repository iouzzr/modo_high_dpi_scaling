// high_dpi_scaling.cpp - High-DPI 3D Viewport & 2D UI (OPALDisp) Patch DLL for Foundry Modo
// Hooks QOpenGLFramebufferObject (in front3000.dll), AGLViewport / AGLScissor (in frame3000.dll),
// and QImage / QPaintDevice / QPainter / QFont (in front3000.dll) to render both 3D viewports
// and 2D UI panels/text at native High-DPI physical resolution (125%, 150%, 200%, etc.).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <intrin.h>

#pragma intrinsic(_ReturnAddress)

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
typedef void*    (*FnQImageCtorSize)(void* this_ptr, const QSizeC* sz, int fmt);
typedef void*    (*FnQImageCtorWH)(void* this_ptr, int w, int h, int fmt);
typedef QSizeC*  (*FnQImageSize)(const void* this_ptr, QSizeC* out_sz);
typedef void*    (*FnQImageCopy)(const void* this_ptr, void* out_img, int x, int y, int w, int h);
typedef uint32_t (*FnQImagePixel)(const void* this_ptr, int x, int y);
typedef void     (*FnDrawImageRectRect)(void* painter, const QRectC* tr, const void* img, const QRectC* sr, int flags);
typedef void     (*FnPainterSetWindow)(void* painter, int x, int y, int w, int h);
typedef void*    (*FnPainterDevice)(const void* painter);
typedef void     (*FnFontSetStyleStrategy)(void* font_ptr, int strategy);
typedef void     (*FnSetDpr)(void* this_ptr, double dpr);
typedef double   (*FnGetDpr)(const void* this_ptr);

static FnFboCtor              g_orig_fbo_ctor        = nullptr;
static FnDim                  g_orig_fbo_width       = nullptr;
static FnDim                  g_orig_fbo_height      = nullptr;
static FnFboBlit              g_orig_fbo_blit        = nullptr;
static FnRect4                g_orig_viewport        = nullptr;
static FnRect4                g_orig_scissor         = nullptr;

static FnQImageCtorSize       g_orig_qimg_ctor_size  = nullptr;
static FnQImageCtorWH         g_orig_qimg_ctor_wh    = nullptr;
static FnQImageSize           g_orig_qimg_size       = nullptr;
static FnDim                  g_orig_qimg_width      = nullptr;
static FnDim                  g_orig_qimg_height     = nullptr;
static FnDim                  g_orig_qpd_width       = nullptr;
static FnDim                  g_orig_qpd_height      = nullptr;
static FnQImageCopy           g_orig_qimg_copy       = nullptr;
static FnQImagePixel          g_orig_qimg_pixel      = nullptr;
static FnDrawImageRectRect    g_orig_draw_image_rr   = nullptr;
static FnPainterSetWindow     g_orig_set_window      = nullptr;
static FnPainterDevice        g_painter_device       = nullptr;
static FnFontSetStyleStrategy g_orig_font_style      = nullptr;

static FnSetDpr               g_qimg_set_dpr         = nullptr;
static FnGetDpr               g_qimg_get_dpr         = nullptr;
static FnGetDpr               g_qpd_get_dpr          = nullptr;

static uint8_t*               g_front_base           = nullptr;
static double                 g_scale                = 1.0;
static bool                   g_hooked               = false;

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
    g_orig_viewport(ScaleInt(x), ScaleInt(y), ScaleInt(w), ScaleInt(h));
}

static void HookScissor(int x, int y, int w, int h) {
    g_orig_scissor(ScaleInt(x), ScaleInt(y), ScaleInt(w), ScaleInt(h));
}

// ============================================================================
// 2. 2D UI (OPALDisp / QImage / QPaintDevice / QPainter / QFont) Hooks
// ============================================================================

static void* HookQImageCtorSize(void* this_ptr, const QSizeC* sz, int fmt) {
    if (g_scale > 1.001 && sz && g_qimg_set_dpr) {
        QSizeC scaled_sz = { ScaleInt(sz->w), ScaleInt(sz->h) };
        void* res = g_orig_qimg_ctor_size(this_ptr, &scaled_sz, fmt);
        g_qimg_set_dpr(this_ptr, g_scale);
        return res;
    }
    return g_orig_qimg_ctor_size(this_ptr, sz, fmt);
}

static void* HookQImageCtorWH(void* this_ptr, int w, int h, int fmt) {
    if (g_scale > 1.001 && fmt == 5 && g_qimg_set_dpr) {
        const uint8_t* ret = reinterpret_cast<const uint8_t*>(_ReturnAddress());
        // Check if called from ORastCreate (0x1801B60D0 -> ret at 0x1801B60D6)
        bool is_orast = (g_front_base && ret == g_front_base + 0x1B60D6) ||
                        (ret && ret[0] == 0x48 && ret[1] == 0x8D && ret[2] == 0x05 &&
                         ret[7] == 0x48 && ret[8] == 0x89 && ret[9] == 0x07);
        if (is_orast) {
            void* res = g_orig_qimg_ctor_wh(this_ptr, ScaleInt(w), ScaleInt(h), fmt);
            g_qimg_set_dpr(this_ptr, g_scale);
            return res;
        }
    }
    return g_orig_qimg_ctor_wh(this_ptr, w, h, fmt);
}

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

static int HookQImageHeight(const void* this_ptr) {
    int ph = g_orig_qimg_height(this_ptr);
    if (g_scale > 1.001 && g_qimg_get_dpr && g_qimg_get_dpr(this_ptr) > 1.001) {
        return UnscaleInt(ph);
    }
    return ph;
}

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

static void InstallAllHooks() {
    if (g_hooked) return;

    auto* front = reinterpret_cast<uint8_t*>(GetModuleHandleA("front3000.dll"));
    auto* frame = reinterpret_cast<uint8_t*>(GetModuleHandleA("frame3000.dll"));
    HMODULE qt5gui = GetModuleHandleA("Qt5Gui.dll");
    if (!front || !frame || !qt5gui) return;

    g_front_base     = front;
    g_qimg_set_dpr   = reinterpret_cast<FnSetDpr>(GetProcAddress(qt5gui, "?setDevicePixelRatio@QImage@@QEAAXN@Z"));
    g_qimg_get_dpr   = reinterpret_cast<FnGetDpr>(GetProcAddress(qt5gui, "?devicePixelRatio@QImage@@QEBANXZ"));
    g_qpd_get_dpr    = reinterpret_cast<FnGetDpr>(GetProcAddress(qt5gui, "?devicePixelRatioF@QPaintDevice@@QEBANXZ"));
    g_painter_device = reinterpret_cast<FnPainterDevice>(GetProcAddress(qt5gui, "?device@QPainter@@QEBAPEAVQPaintDevice@@XZ"));

    // 1. 3D OpenGL Viewport IAT slots
    void** s_fbo_ctor   = FindIATSlot(front, "??0QOpenGLFramebufferObject@@QEAA@HHAEBVQOpenGLFramebufferObjectFormat@@@Z");
    void** s_fbo_width  = FindIATSlot(front, "?width@QOpenGLFramebufferObject@@QEBAHXZ");
    void** s_fbo_height = FindIATSlot(front, "?height@QOpenGLFramebufferObject@@QEBAHXZ");
    void** s_fbo_blit   = FindIATSlot(front, "?blitFramebuffer@QOpenGLFramebufferObject@@SAXPEAV1@AEBVQRect@@01II@Z");
    void** s_vp         = FindIATSlot(frame, "?AGLViewport@@YAXHHHH@Z");
    void** s_sc         = FindIATSlot(frame, "?AGLScissor@@YAXHHHH@Z");

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
    void** s_set_win    = FindIATSlot(front, "?setWindow@QPainter@@QEAAXHHHH@Z");
    void** s_font_style = FindIATSlot(front, "?setStyleStrategy@QFont@@QEAAXW4StyleStrategy@1@@Z");

    if (s_fbo_ctor && s_fbo_width && s_fbo_height && s_fbo_blit && s_vp && s_sc) {
        PatchSlot(s_fbo_ctor,   reinterpret_cast<void*>(&HookFboCtor),   reinterpret_cast<void**>(&g_orig_fbo_ctor));
        PatchSlot(s_fbo_width,  reinterpret_cast<void*>(&HookFboWidth),  reinterpret_cast<void**>(&g_orig_fbo_width));
        PatchSlot(s_fbo_height, reinterpret_cast<void*>(&HookFboHeight), reinterpret_cast<void**>(&g_orig_fbo_height));
        PatchSlot(s_fbo_blit,   reinterpret_cast<void*>(&HookFboBlit),   reinterpret_cast<void**>(&g_orig_fbo_blit));
        PatchSlot(s_vp,         reinterpret_cast<void*>(&HookViewport),  reinterpret_cast<void**>(&g_orig_viewport));
        PatchSlot(s_sc,         reinterpret_cast<void*>(&HookScissor),   reinterpret_cast<void**>(&g_orig_scissor));

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
            PatchSlot(s_set_win,    reinterpret_cast<void*>(&HookPainterSetWindow),     reinterpret_cast<void**>(&g_orig_set_window));
            PatchSlot(s_font_style, reinterpret_cast<void*>(&HookFontSetStyleStrategy), reinterpret_cast<void**>(&g_orig_font_style));
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
