/* SPDX-License-Identifier: MIT */
/*
 * mmiofix: proxy dinput.dll that works around a Wine winmm bug affecting
 * "Stratosphere: Conquest of the Skies" (and any app doing the same).
 *
 * The game calls mmioInstallIOProc('DOS ', proc, MMIO_INSTALLPROC) to replace the
 * default file I/O procedure, then mmioOpen(path, NULL, ...). Windows uses the
 * installed procedure; Wine's MMIO_Open hardcodes its builtin DOS procedure when the
 * name has no "FOURCC+" prefix, so the override is ignored and mmioOpen fails.
 *
 * This DLL patches the host exe's import table so that mmioOpenA passes the installed
 * 'DOS ' procedure explicitly (MMIOINFO.pIOProc). DirectInputCreateA is forwarded.
 */
#define INITGUID
#include <windows.h>
#include <mmsystem.h>
#include <ddraw.h>
#define DIRECTINPUT_VERSION 0x0500
#include <dinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FOURCC_DOS_ mmioFOURCC('D', 'O', 'S', ' ')

static LPMMIOPROC g_dos_proc;
/*
 * The game's 'DOS ' handler implements MMIOM_SEEK as "seek, store the new position in
 * lDiskOffset, return 0". Windows' mmioSeek evidently reports lDiskOffset, whereas Wine's
 * mmioSeek returns the handler's return value, so mmioDescend/mmioSeek(SEEK_CUR) see
 * position 0 and the WAV parse goes wrong ("Error reading extra bytes").
 */
static LRESULT CALLBACK dos_wrap(LPSTR info, UINT msg, LPARAM l1, LPARAM l2)
{
    LRESULT r = g_dos_proc ? g_dos_proc(info, msg, l1, l2) : MMSYSERR_ERROR;
    if (msg == MMIOM_SEEK && r == 0) r = ((LPMMIOINFO)info)->lDiskOffset;
    return r;
}

static HMMIO (WINAPI *real_mmioOpenA)(LPSTR, LPMMIOINFO, DWORD);
static LPMMIOPROC (WINAPI *real_mmioInstallIOProcA)(FOURCC, LPMMIOPROC, DWORD);
static HANDLE g_log;

static void logf_(const char *fmt, const char *a, DWORD b)
{
    char buf[512], out[640]; DWORD n;
    if (g_log == INVALID_HANDLE_VALUE || !g_log) return;
    wsprintfA(buf, fmt, a, b);
    n = wsprintfA(out, "[mmiofix] %s\r\n", buf);
    WriteFile(g_log, out, n, &n, NULL);
}

/* true if name looks like "XXXX+file" (custom ioproc prefix), as Wine's MMIO_ParseExt */
static int has_fourcc_prefix(const char *s)
{
    const char *p = strchr(s, '+');
    return p && p != s;
}

static HMMIO WINAPI my_mmioOpenA(LPSTR name, LPMMIOINFO info, DWORD flags)
{
    if (g_dos_proc && name && !(flags & (MMIO_PARSE | MMIO_EXIST | MMIO_DELETE | MMIO_GETTEMP)) &&
        (!info || (!info->fccIOProc && !info->pIOProc)) && !has_fourcc_prefix(name))
    {
        MMIOINFO mi;
        if (info) mi = *info; else memset(&mi, 0, sizeof(mi));
        mi.pIOProc = dos_wrap;
        logf_("mmioOpenA('%s') -> using app 'DOS ' proc, flags=%08x", name, flags);
        return real_mmioOpenA(name, &mi, flags);
    }
    return real_mmioOpenA(name, info, flags);
}

static LPMMIOPROC WINAPI my_mmioInstallIOProcA(FOURCC fcc, LPMMIOPROC proc, DWORD flags)
{
    LPMMIOPROC ret = real_mmioInstallIOProcA(fcc, proc, flags);
    if (fcc == FOURCC_DOS_)
    {
        if (flags & MMIO_INSTALLPROC) { g_dos_proc = proc; logf_("app installed 'DOS ' proc, flags=%08x", "", flags); }
        else if (flags & MMIO_REMOVEPROC) { g_dos_proc = NULL; logf_("app removed 'DOS ' proc, flags=%08x", "", flags); }
    }
    return ret;
}


/* ---- mouse diagnostics: log the user32 cursor/coordinate calls the game uses (MMIOFIX_LOG=1) ---- */
static BOOL (WINAPI *real_SetCursorPos)(int, int);
static BOOL (WINAPI *real_ClipCursor)(const RECT *);
static BOOL (WINAPI *real_ClientToScreen)(HWND, POINT *);
static BOOL (WINAPI *real_ScreenToClient)(HWND, POINT *);
static HHOOK (WINAPI *real_SetWindowsHookExA)(int, HOOKPROC, HINSTANCE, DWORD);
/*
 * The game acquires the DirectInput mouse in exclusive mode and tracks its own position from the DI deltas.
 * Windows then sends it no mouse-move messages; Wine still does, from the (parked, clamped) OS pointer, and the
 * game's highlight code follows those, so the highlight drifts away from the cursor it draws.
 * Fix (default on): drop WM_MOUSEMOVE at the game's own WH_MOUSE hook. MMIOFIX_MOUSE=off disables it.
 */
static int g_mouse_swallow = 1;
static volatile DWORD g_di_poll;        /* tick of the game's last poll of the DirectInput mouse */
static int g_swallow_state = -1;
static void mlog(const char *fmt, int a, int b, int c, int d);
static HOOKPROC g_game_mouse_hook;
static LRESULT CALLBACK my_mouse_hook(int code, WPARAM w, LPARAM l)
{
    if (code >= 0 && (w == WM_MOUSEMOVE || w == WM_NCMOUSEMOVE))
    {
        /* menu mode: the game polls the DI mouse every frame and ignores the OS pointer; mission mode: it does not poll
         * and takes the position from these messages, so only drop them while the DI mouse is being polled */
        int dropping = g_di_poll && (GetTickCount() - g_di_poll) < 250;
        if (dropping != g_swallow_state) { g_swallow_state = dropping; mlog("mouse mode: %d (1 = menu/DirectInput, 0 = mission/OS messages)%d%d%d", dropping, 0, 0, 0); }
        if (dropping) return 1;                       /* nonzero: message is not delivered */
    }
    return g_game_mouse_hook(code, w, l);
}
static unsigned g_mlines;
static void mlog(const char *fmt, int a, int b, int c, int d)
{
    char buf[256], out[320]; DWORD n;
    if (!g_log || g_log == INVALID_HANDLE_VALUE || g_mlines++ > 60000) return;
    wsprintfA(buf, fmt, a, b, c, d);
    SYSTEMTIME st; GetLocalTime(&st);
    n = wsprintfA(out, "[mouse %02d:%02d:%02d.%03d] %s\r\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
    WriteFile(g_log, out, n, &n, NULL);
}
static HHOOK WINAPI my_SetWindowsHookExA(int id, HOOKPROC fn, HINSTANCE inst, DWORD tid)
{
    HHOOK h;
    if (g_mouse_swallow && id == WH_MOUSE && fn) { g_game_mouse_hook = fn; fn = my_mouse_hook; mlog("swallowing WM_MOUSEMOVE at WH_MOUSE hook%d%d%d%d", 0, 0, 0, 0); }
    h = real_SetWindowsHookExA(id, fn, inst, tid);
    mlog("SetWindowsHookExA(idHook=%d, thread=%d) -> %d%d", id, (int)tid, h != NULL, 0);
    return h;
}
static BOOL WINAPI my_SetCursorPos(int x, int y)
{
    POINT p = {-1, -1}; BOOL r = real_SetCursorPos(x, y);
    GetCursorPos(&p);
    mlog("SetCursorPos(%d,%d) -> now at %d,%d", x, y, p.x, p.y);
    return r;
}
static BOOL WINAPI my_ClipCursor(const RECT *rc)
{
    if (rc) mlog("ClipCursor(%d,%d,%d,%d)", rc->left, rc->top, rc->right, rc->bottom);
    else mlog("ClipCursor(NULL)%d%d%d%d", 0, 0, 0, 0);
    return real_ClipCursor(rc);
}
static BOOL WINAPI my_ClientToScreen(HWND h, POINT *p)
{
    int x = p ? p->x : 0, y = p ? p->y : 0; BOOL r = real_ClientToScreen(h, p);
    RECT w = {0}; GetWindowRect(h, &w);
    if (p) mlog("ClientToScreen(%d,%d) -> %d,%d", x, y, p->x, p->y);
    mlog("  window rect %d,%d,%d,%d", w.left, w.top, w.right, w.bottom);
    return r;
}
static BOOL WINAPI my_ScreenToClient(HWND h, POINT *p)
{
    int x = p ? p->x : 0, y = p ? p->y : 0; BOOL r = real_ScreenToClient(h, p);
    if (p) mlog("ScreenToClient(%d,%d) -> %d,%d", x, y, p->x, p->y);
    return r;
}

static int patch_iat(HMODULE exe)
{
    BYTE *base = (BYTE *)exe;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY *dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *imp;
    int patched = 0;
    if (!dd->VirtualAddress) return 0;
    for (imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dd->VirtualAddress); imp->Name; imp++)
    {
        IMAGE_THUNK_DATA *oft, *ft;
        int is_u32 = !lstrcmpiA((char *)(base + imp->Name), "user32.dll");
        if (lstrcmpiA((char *)(base + imp->Name), "winmm.dll") && !is_u32) continue;
        oft = (IMAGE_THUNK_DATA *)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        ft = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; oft->u1.AddressOfData; oft++, ft++)
        {
            IMAGE_IMPORT_BY_NAME *ibn;
            void *repl = NULL; DWORD old;
            if (IMAGE_SNAP_BY_ORDINAL32(oft->u1.Ordinal)) continue;
            ibn = (IMAGE_IMPORT_BY_NAME *)(base + oft->u1.AddressOfData);
            if (is_u32)
            {
                if (!strcmp((char *)ibn->Name, "SetWindowsHookExA") ? !(g_log || g_mouse_swallow) : !g_log) continue;
                if (!strcmp((char *)ibn->Name, "SetCursorPos")) { real_SetCursorPos = (void *)ft->u1.Function; repl = my_SetCursorPos; }
                else if (!strcmp((char *)ibn->Name, "SetWindowsHookExA")) { real_SetWindowsHookExA = (void *)ft->u1.Function; repl = my_SetWindowsHookExA; }
                else if (!strcmp((char *)ibn->Name, "ClipCursor")) { real_ClipCursor = (void *)ft->u1.Function; repl = my_ClipCursor; }
                else if (!strcmp((char *)ibn->Name, "ClientToScreen")) { real_ClientToScreen = (void *)ft->u1.Function; repl = my_ClientToScreen; }
                else if (!strcmp((char *)ibn->Name, "ScreenToClient")) { real_ScreenToClient = (void *)ft->u1.Function; repl = my_ScreenToClient; }
            }
            else if (!strcmp((char *)ibn->Name, "mmioOpenA")) { real_mmioOpenA = (void *)ft->u1.Function; repl = my_mmioOpenA; }
            else if (!strcmp((char *)ibn->Name, "mmioInstallIOProcA")) { real_mmioInstallIOProcA = (void *)ft->u1.Function; repl = my_mmioInstallIOProcA; }
            if (!repl) continue;
            VirtualProtect(&ft->u1.Function, sizeof(void *), PAGE_READWRITE, &old);
            ft->u1.Function = (DWORD_PTR)repl;
            VirtualProtect(&ft->u1.Function, sizeof(void *), old, &old);
            patched++;
        }
    }
    return patched;
}


/* ------------------------------------------------------------------------------------
 * DirectDraw diagnostics / workaround experiments (surface2 vtable hooks)
 *   MMIOFIX_EXP=nosrc   Flip(src,flags) -> Flip(NULL, flags|DDFLIP_WAIT)
 *   MMIOFIX_EXP=blt     emulate Flip with a back->front Blt copy
 *   MMIOFIX_EXP=touch   after Flip, Lock/Unlock back and front buffer (force a sync)
 *   MMIOFIX_EXP=syncsrc|syncdst|syncboth  force a read-back (Lock/Unlock readonly) of the Blt/BltFast source and/or destination
 *   MMIOFIX_EXP=sysblt  emulate Flip by a back->front copy ONLY for system-memory flip chains (as DirectDraw's emulation layer did); video-memory chains keep the real swap
 *   MMIOFIX_EXP=trace   log every small (cursor-sized) Blt/BltFast with surface ids, rects, flags and pixel checksums
 *   dump: create a file named dump.now in the game dir (or press F11) -> dump_N_front/back.bmp
 * ---------------------------------------------------------------------------------- */
enum { EXP_NONE, EXP_NOSRC, EXP_BLT, EXP_TOUCH, EXP_SYNCSRC, EXP_SYNCDST, EXP_SYNCBOTH, EXP_TRACE, EXP_SYSBLT };
static int g_exp;
static HRESULT (STDMETHODCALLTYPE *orig_Flip)(IDirectDrawSurface2 *, IDirectDrawSurface2 *, DWORD);
static unsigned g_flips, g_dumpseq, g_auto_ms, g_last_auto;

static void logs(const char *fmt, unsigned a, unsigned b, unsigned c)
{
    if (fmt[0] == '%' && fmt[1] == 's' && !fmt[2])
    {
        char out[400]; DWORD n;
        if (!g_log || g_log == INVALID_HANDLE_VALUE) return;
        n = wsprintfA(out, "[mmiofix] %s\r\n", (const char *)(size_t)a);
        WriteFile(g_log, out, n, &n, NULL);
        return;
    }
    char buf[256], out[320]; DWORD n;
    if (!g_log || g_log == INVALID_HANDLE_VALUE) return;
    wsprintfA(buf, fmt, a, b, c);
    n = wsprintfA(out, "[mmiofix] %s\r\n", buf);
    WriteFile(g_log, out, n, &n, NULL);
}

static unsigned ch(DWORD px, DWORD mask)
{
    unsigned sh = 0, bits = 0, v;
    if (!mask) return 0;
    while (!((mask >> sh) & 1)) sh++;
    while ((mask >> (sh + bits)) & 1) bits++;
    v = (px & mask) >> sh;
    return bits >= 8 ? v >> (bits - 8) : (v * 255) / ((1u << bits) - 1);
}

static PALETTEENTRY g_pal[256]; static int g_have_pal;

static void dump_surface(IDirectDrawSurface2 *s, const char *tag)
{
    DDSURFACEDESC d; char name[64]; HANDLE f; DWORD n; unsigned x, y, w, h, bpp, rowsz;
    BITMAPFILEHEADER bf; BITMAPINFOHEADER bi; BYTE *row;
    memset(&d, 0, sizeof(d)); d.dwSize = sizeof(d);
    if (IDirectDrawSurface2_Lock(s, NULL, &d, DDLOCK_READONLY | DDLOCK_WAIT, NULL) != DD_OK) { logs("dump %u: Lock failed (%s)", g_dumpseq, 0, 0); return; }
    w = d.dwWidth; h = d.dwHeight; bpp = d.ddpfPixelFormat.dwRGBBitCount;
    rowsz = (w * 3 + 3) & ~3u;
    row = HeapAlloc(GetProcessHeap(), 0, rowsz);
    wsprintfA(name, "dump_%u_%s.bmp", g_dumpseq, tag);
    f = CreateFileA(name, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    memset(&bf, 0, sizeof(bf)); memset(&bi, 0, sizeof(bi));
    bf.bfType = 0x4d42; bf.bfOffBits = sizeof(bf) + sizeof(bi); bf.bfSize = bf.bfOffBits + rowsz * h;
    bi.biSize = sizeof(bi); bi.biWidth = w; bi.biHeight = h; bi.biPlanes = 1; bi.biBitCount = 24;
    WriteFile(f, &bf, sizeof(bf), &n, NULL); WriteFile(f, &bi, sizeof(bi), &n, NULL);
    for (y = h; y-- > 0;)
    {
        BYTE *src = (BYTE *)d.lpSurface + (size_t)y * d.lPitch;
        for (x = 0; x < w; x++)
        {
            DWORD px = bpp == 32 ? ((DWORD *)src)[x] : bpp == 16 ? ((WORD *)src)[x] : src[x];
            if (bpp == 8) { if (g_have_pal) { row[x*3] = g_pal[px&255].peBlue; row[x*3+1] = g_pal[px&255].peGreen; row[x*3+2] = g_pal[px&255].peRed; } else row[x*3] = row[x*3+1] = row[x*3+2] = (BYTE)px; }
            else { row[x*3] = ch(px, d.ddpfPixelFormat.dwBBitMask); row[x*3+1] = ch(px, d.ddpfPixelFormat.dwGBitMask); row[x*3+2] = ch(px, d.ddpfPixelFormat.dwRBitMask); }
        }
        WriteFile(f, row, rowsz, &n, NULL);
    }
    CloseHandle(f); HeapFree(GetProcessHeap(), 0, row);
    IDirectDrawSurface2_Unlock(s, NULL);
    logs("dumped %u (%ux%u)", g_dumpseq, w, h); logs("  at flip #%u tick %u", g_flips, GetTickCount(), 0);
}

static IDirectDrawSurface2 *get_back(IDirectDrawSurface2 *front)
{
    DDSCAPS caps; IDirectDrawSurface2 *back = NULL;
    memset(&caps, 0, sizeof(caps)); caps.dwCaps = DDSCAPS_BACKBUFFER;
    if (IDirectDrawSurface2_GetAttachedSurface(front, &caps, &back) != DD_OK) return NULL;
    return back;
}

static void maybe_dump(IDirectDrawSurface2 *front)
{
    int want = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    if (!want && !(g_flips % 15) && GetFileAttributesA("dump.now") != INVALID_FILE_ATTRIBUTES) { DeleteFileA("dump.now"); want = 1; }
    if (!want && g_auto_ms && g_dumpseq < 80)
    {
        DWORD now = GetTickCount();
        if (now - g_last_auto >= g_auto_ms) { g_last_auto = now; want = 1; }
    }
    if (!want) return;
    g_dumpseq++;
    g_have_pal = 0;
    {
        IDirectDrawPalette *pal = NULL;
        if (IDirectDrawSurface2_GetPalette(front, &pal) == DD_OK && pal)
        {
            if (IDirectDrawPalette_GetEntries(pal, 0, 0, 256, g_pal) == DD_OK)
            {
                g_have_pal = 1;
                logs("palette: [0]=%02x%02x%02x", g_pal[0].peRed, g_pal[0].peGreen, g_pal[0].peBlue);
                logs("palette: [1]=%02x%02x%02x [128]=%02x", g_pal[1].peRed, g_pal[1].peGreen, g_pal[1].peBlue);
                logs("palette: [255]=%02x%02x%02x", g_pal[255].peRed, g_pal[255].peGreen, g_pal[255].peBlue);
            }
            IDirectDrawPalette_Release(pal);
        }
        else logs("front has no palette (hr failed)", 0, 0, 0);
    }
    dump_surface(front, "front");
    { IDirectDrawSurface2 *back = get_back(front); if (back) { dump_surface(back, "back"); IDirectDrawSurface2_Release(back); } }
}

static HRESULT STDMETHODCALLTYPE my_Flip(IDirectDrawSurface2 *This, IDirectDrawSurface2 *target, DWORD flags)
{
    HRESULT hr;
    g_flips++;
    if (g_flips <= 12) logs("Flip #%u explicit_target=%u flags=%08x", g_flips, target != NULL, flags);
    if (g_exp == EXP_NOSRC) { target = NULL; flags |= DDFLIP_WAIT; }
    if (g_exp == EXP_SYSBLT)
    {
        DDSURFACEDESC d; memset(&d, 0, sizeof(d)); d.dwSize = sizeof(d);
        if (IDirectDrawSurface2_GetSurfaceDesc(This, &d) == DD_OK && (d.ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY))
        {
            IDirectDrawSurface2 *src = target; int rel = 0;
            if (!src) { src = get_back(This); rel = 1; }
            hr = src ? IDirectDrawSurface2_Blt(This, NULL, src, NULL, DDBLT_WAIT, NULL) : DDERR_NOTFLIPPABLE;
            if (rel && src) IDirectDrawSurface2_Release(src);
            maybe_dump(This);
            return hr;
        }
    }
    if (g_exp == EXP_BLT)
    {
        IDirectDrawSurface2 *src = target; int rel = 0;
        if (!src) { src = get_back(This); rel = 1; }
        hr = src ? IDirectDrawSurface2_Blt(This, NULL, src, NULL, DDBLT_WAIT, NULL) : DDERR_NOTFLIPPABLE;
        if (rel && src) IDirectDrawSurface2_Release(src);
    }
    else hr = orig_Flip(This, target, flags);
    if (g_exp == EXP_TOUCH)
    {
        IDirectDrawSurface2 *b = get_back(This), *list[2]; int i; DDSURFACEDESC d;
        list[0] = This; list[1] = b;
        for (i = 0; i < 2; i++) if (list[i])
        {
            memset(&d, 0, sizeof(d)); d.dwSize = sizeof(d);
            if (IDirectDrawSurface2_Lock(list[i], NULL, &d, DDLOCK_WAIT, NULL) == DD_OK) IDirectDrawSurface2_Unlock(list[i], NULL);
        }
        if (b) IDirectDrawSurface2_Release(b);
    }
    maybe_dump(This);
    return hr;
}


static HRESULT (STDMETHODCALLTYPE *orig_Blt)(IDirectDrawSurface2 *, RECT *, IDirectDrawSurface2 *, RECT *, DWORD, DDBLTFX *);
static HRESULT (STDMETHODCALLTYPE *orig_BltFast)(IDirectDrawSurface2 *, DWORD, DWORD, IDirectDrawSurface2 *, RECT *, DWORD);

/* readonly Lock/Unlock of a rect forces wined3d to bring the sysmem copy up to date */
static void force_sync(IDirectDrawSurface2 *s, RECT *r)
{
    DDSURFACEDESC d;
    if (!s) return;
    memset(&d, 0, sizeof(d)); d.dwSize = sizeof(d);
    if (IDirectDrawSurface2_Lock(s, r, &d, DDLOCK_READONLY | DDLOCK_WAIT, NULL) == DD_OK) IDirectDrawSurface2_Unlock(s, r);
}

static int exp_src(void) { return g_exp == EXP_SYNCSRC || g_exp == EXP_SYNCBOTH; }
static int exp_dst(void) { return g_exp == EXP_SYNCDST || g_exp == EXP_SYNCBOTH; }

/* ---- trace mode: cursor-sized blits with before/after/source checksums ---- */
#define MAXSURF 64
static void *g_surf[MAXSURF]; static int g_nsurf; static unsigned g_tracelines;

static int surf_id(IDirectDrawSurface2 *s)
{
    int i; DDSURFACEDESC d;
    for (i = 0; i < g_nsurf; i++) if (g_surf[i] == s) return i;
    if (g_nsurf >= MAXSURF) return 99;
    g_surf[g_nsurf] = s;
    memset(&d, 0, sizeof(d)); d.dwSize = sizeof(d);
    if (IDirectDrawSurface2_GetSurfaceDesc(s, &d) == DD_OK)
        logs("SURF s%u = %ux%u", g_nsurf, d.dwWidth, d.dwHeight), logs("  s%u caps=%08x bpp=%u", g_nsurf, d.ddsCaps.dwCaps, d.ddpfPixelFormat.dwRGBBitCount);
    return g_nsurf++;
}

static unsigned rect_hash(IDirectDrawSurface2 *s, RECT *r)
{
    DDSURFACEDESC d; unsigned h = 2166136261u; LONG y, x, bytes; BYTE *p;
    if (!s || !r) return 0;
    memset(&d, 0, sizeof(d)); d.dwSize = sizeof(d);
    if (IDirectDrawSurface2_Lock(s, r, &d, DDLOCK_READONLY | DDLOCK_WAIT, NULL) != DD_OK) return 1;
    bytes = (r->right - r->left) * (d.ddpfPixelFormat.dwRGBBitCount / 8);
    for (y = 0; y < r->bottom - r->top; y++)
    {
        p = (BYTE *)d.lpSurface + (size_t)y * d.lPitch;
        for (x = 0; x < bytes; x++) h = (h ^ p[x]) * 16777619u;
    }
    IDirectDrawSurface2_Unlock(s, r);
    return h;
}

static int small_rect(RECT *r) { return r && (r->right - r->left) >= 10 && (r->right - r->left) <= 20 && (r->bottom - r->top) >= 10 && (r->bottom - r->top) <= 20; }

static HRESULT STDMETHODCALLTYPE my_Blt(IDirectDrawSurface2 *This, RECT *dr, IDirectDrawSurface2 *src, RECT *sr, DWORD flags, DDBLTFX *fx)
{
    HRESULT hr;
    if (g_exp == EXP_TRACE && g_tracelines < 20000 && dr && small_rect(dr))
    {
        unsigned pre = rect_hash(This, dr), sh = src ? rect_hash(src, sr) : 0, post;
        int di = surf_id(This), si = src ? surf_id(src) : -1;
        hr = orig_Blt(This, dr, src, sr, flags, fx);
        post = rect_hash(This, dr);
        g_tracelines++;
        { char b[200]; wsprintfA(b, "BLT s%d[%d,%d-%d,%d] <- s%d[%d,%d-%d,%d] fl=%08x pre=%08x src=%08x post=%08x hr=%08x", di, (int)dr->left,(int)dr->top,(int)dr->right,(int)dr->bottom, si, sr?(int)sr->left:-1, sr?(int)sr->top:-1, sr?(int)sr->right:-1, sr?(int)sr->bottom:-1, (unsigned)flags, pre, sh, post, (unsigned)hr); logs("%s", (unsigned)(size_t)b, 0, 0); }
        return hr;
    }
    if (exp_src() && src) force_sync(src, sr);
    hr = orig_Blt(This, dr, src, sr, flags, fx);
    if (exp_dst()) force_sync(This, dr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE my_BltFast(IDirectDrawSurface2 *This, DWORD x, DWORD y, IDirectDrawSurface2 *src, RECT *sr, DWORD trans)
{
    HRESULT hr;
    if (g_exp == EXP_TRACE && g_tracelines < 20000 && sr && small_rect(sr))
    {
        RECT dr; unsigned pre, sh, post; int di, si;
        dr.left = x; dr.top = y; dr.right = x + (sr->right - sr->left); dr.bottom = y + (sr->bottom - sr->top);
        pre = rect_hash(This, &dr); sh = rect_hash(src, sr); di = surf_id(This); si = surf_id(src);
        hr = orig_BltFast(This, x, y, src, sr, trans);
        post = rect_hash(This, &dr);
        g_tracelines++;
        { char b[200]; wsprintfA(b, "FAST s%d[%d,%d-%d,%d] <- s%d[%d,%d-%d,%d] tr=%08x pre=%08x src=%08x post=%08x", di, (int)dr.left,(int)dr.top,(int)dr.right,(int)dr.bottom, si, (int)sr->left,(int)sr->top,(int)sr->right,(int)sr->bottom, (unsigned)trans, pre, sh, post); logs("%s", (unsigned)(size_t)b, 0, 0); }
        return hr;
    }
    if (exp_src() && src) force_sync(src, sr);
    hr = orig_BltFast(This, x, y, src, sr, trans);
    if (exp_dst() && sr) { RECT dr; dr.left = x; dr.top = y; dr.right = x + (sr->right - sr->left); dr.bottom = y + (sr->bottom - sr->top); force_sync(This, &dr); }
    return hr;
}

static HRESULT (STDMETHODCALLTYPE *orig_Flip1)(IDirectDrawSurface *, IDirectDrawSurface *, DWORD);
static HRESULT STDMETHODCALLTYPE my_Flip1(IDirectDrawSurface *This, IDirectDrawSurface *target, DWORD flags)
{
    HRESULT hr = orig_Flip1(This, target, flags);
    IDirectDrawSurface2 *s2 = NULL;
    g_flips++;
    if (g_flips <= 6) logs("Flip1 #%u flags=%08x", g_flips, flags, 0);
    if (IDirectDrawSurface_QueryInterface(This, &IID_IDirectDrawSurface2, (void **)&s2) == DD_OK && s2)
    {
        maybe_dump(s2);
        IDirectDrawSurface2_Release(s2);
    }
    return hr;
}

static void install_dd_hooks(void)
{
    static int done;
    HMODULE dd; HRESULT (WINAPI *create)(GUID *, LPDIRECTDRAW *, IUnknown *);
    LPDIRECTDRAW d = NULL; LPDIRECTDRAWSURFACE s1 = NULL; IDirectDrawSurface2 *s2 = NULL;
    DDSURFACEDESC sd; void **vtbl; DWORD old; char e[16];
    if (done) return;
    done = 1;
    g_exp = EXP_SYSBLT;                      /* the fix is on by default; MMIOFIX_EXP=none turns it off */
    if (GetEnvironmentVariableA("MMIOFIX_EXP", e, sizeof(e)))
        g_exp = !lstrcmpiA(e, "none") ? EXP_NONE : !lstrcmpiA(e, "nosrc") ? EXP_NOSRC : !lstrcmpiA(e, "blt") ? EXP_BLT : !lstrcmpiA(e, "touch") ? EXP_TOUCH : !lstrcmpiA(e, "syncsrc") ? EXP_SYNCSRC : !lstrcmpiA(e, "syncdst") ? EXP_SYNCDST : !lstrcmpiA(e, "syncboth") ? EXP_SYNCBOTH : !lstrcmpiA(e, "trace") ? EXP_TRACE : !lstrcmpiA(e, "sysblt") ? EXP_SYSBLT : EXP_NONE;
    if (GetEnvironmentVariableA("MMIOFIX_AUTODUMP", e, sizeof(e))) { g_auto_ms = (unsigned)atoi(e) * 1000u; g_last_auto = GetTickCount(); }
    if (!g_log && !g_exp && !g_auto_ms && !GetEnvironmentVariableA("MMIOFIX_DUMP", e, sizeof(e))) return;   /* nothing requested */
    dd = LoadLibraryA("ddraw.dll");
    create = dd ? (void *)GetProcAddress(dd, "DirectDrawCreate") : NULL;
    if (!create || create(NULL, &d, NULL) != DD_OK) { logs("ddraw hook: DirectDrawCreate failed", 0, 0, 0); return; }
    memset(&sd, 0, sizeof(sd)); sd.dwSize = sizeof(sd);
    sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT; sd.dwWidth = sd.dwHeight = 8;
    sd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
    {
        HRESULT hr1, hr2 = E_FAIL, hr3 = E_FAIL;
        hr1 = IDirectDraw_SetCooperativeLevel(d, NULL, DDSCL_NORMAL);
        hr2 = IDirectDraw_CreateSurface(d, &sd, &s1, NULL);
        if (hr2 == DD_OK) hr3 = IDirectDrawSurface_QueryInterface(s1, &IID_IDirectDrawSurface2, (void **)&s2);
        if (hr3 == DD_OK)
        {
            vtbl = (void **)s2->lpVtbl;
            VirtualProtect(&vtbl[11], sizeof(void *), PAGE_READWRITE, &old);
            orig_Flip = (void *)vtbl[11]; vtbl[11] = (void *)my_Flip;
            VirtualProtect(&vtbl[11], sizeof(void *), old, &old);
            VirtualProtect(&vtbl[5], 3 * sizeof(void *), PAGE_READWRITE, &old);
            orig_Blt = (void *)vtbl[5]; vtbl[5] = (void *)my_Blt;
            orig_BltFast = (void *)vtbl[7]; vtbl[7] = (void *)my_BltFast;
            VirtualProtect(&vtbl[5], 3 * sizeof(void *), old, &old);
            {
                void **v1 = (void **)s1->lpVtbl;
                VirtualProtect(&v1[11], sizeof(void *), PAGE_READWRITE, &old);
                orig_Flip1 = (void *)v1[11]; v1[11] = (void *)my_Flip1;
                VirtualProtect(&v1[11], sizeof(void *), old, &old);
            }
            logs("ddraw hook installed, exp=%u", g_exp, 0, 0);
            IDirectDrawSurface2_Release(s2);
        }
        else logs("ddraw hook failed: SetCoop=%08x CreateSurface=%08x QI=%08x", (unsigned)hr1, (unsigned)hr2, (unsigned)hr3);
    }
    if (s1) IDirectDrawSurface_Release(s1);
    IDirectDraw_Release(d);
}


/* ---- DirectInput mouse diagnostics (MMIOFIX_LOG=1) ---- */
static void *g_mouse_dev;
static HRESULT (STDMETHODCALLTYPE *orig_CreateDevice)(IDirectInputA *, REFGUID, IDirectInputDeviceA **, IUnknown *);
static HRESULT (STDMETHODCALLTYPE *orig_GetDeviceState)(IDirectInputDeviceA *, DWORD, void *);
static HRESULT (STDMETHODCALLTYPE *orig_GetDeviceData)(IDirectInputDeviceA *, DWORD, DIDEVICEOBJECTDATA *, DWORD *, DWORD);
static HRESULT (STDMETHODCALLTYPE *orig_SetProperty)(IDirectInputDeviceA *, REFGUID, const DIPROPHEADER *);
static HRESULT (STDMETHODCALLTYPE *orig_SetCoop)(IDirectInputDeviceA *, HWND, DWORD);
static HRESULT (STDMETHODCALLTYPE *orig_SetDataFormat)(IDirectInputDeviceA *, const DIDATAFORMAT *);

static HRESULT STDMETHODCALLTYPE my_GetDeviceState(IDirectInputDeviceA *This, DWORD cb, void *data)
{
    HRESULT r = orig_GetDeviceState(This, cb, data);
    if ((void *)This == g_mouse_dev) g_di_poll = GetTickCount() | 1;
    if ((void *)This == g_mouse_dev && r == DI_OK && cb >= 12)
    {
        DIMOUSESTATE *m = (DIMOUSESTATE *)data; POINT p = {-1, -1};
        static int sx, sy;
        if (m->lX || m->lY) { sx += m->lX; sy += m->lY; GetCursorPos(&p); mlog("DI dx=%d dy=%d sum=%d,%d", m->lX, m->lY, sx, sy); mlog("  os cursor %d,%d%d%d", p.x, p.y, 0, 0); }
    }
    return r;
}
static HRESULT STDMETHODCALLTYPE my_GetDeviceData(IDirectInputDeviceA *This, DWORD cbo, DIDEVICEOBJECTDATA *d, DWORD *n, DWORD fl)
{
    HRESULT r = orig_GetDeviceData(This, cbo, d, n, fl);
    if ((void *)This == g_mouse_dev) g_di_poll = GetTickCount() | 1;
    if ((void *)This == g_mouse_dev && d && n && SUCCEEDED(r))
    {
        DWORD i; POINT p = {-1, -1}; if (*n) GetCursorPos(&p);
        for (i = 0; i < *n && i < 8; i++) mlog("DI data ofs=%d val=%d  (cursor %d,%d)", d[i].dwOfs, (int)d[i].dwData, p.x, p.y);
    }
    return r;
}
static HRESULT STDMETHODCALLTYPE my_SetProperty(IDirectInputDeviceA *This, REFGUID g, const DIPROPHEADER *h)
{
    if ((void *)This == g_mouse_dev && h)
        mlog("DI SetProperty(prop=%d) obj=%d how=%d", (int)(DWORD_PTR)g, h->dwObj, h->dwHow, h->dwSize >= 20 ? (int)((DIPROPDWORD *)h)->dwData : -1);
    return orig_SetProperty(This, g, h);
}
static HRESULT STDMETHODCALLTYPE my_SetCoop(IDirectInputDeviceA *This, HWND w, DWORD fl)
{
    if ((void *)This == g_mouse_dev) mlog("DI mouse SetCooperativeLevel(flags=%x)%d%d%d", fl, 0, 0, 0);
    return orig_SetCoop(This, w, fl);
}
static HRESULT STDMETHODCALLTYPE my_SetDataFormat(IDirectInputDeviceA *This, const DIDATAFORMAT *f)
{
    if ((void *)This == g_mouse_dev && f)
        mlog("DI mouse SetDataFormat(size=%d flags=%x objs=%d)", f->dwDataSize, f->dwFlags, f->dwNumObjs, 0);
    return orig_SetDataFormat(This, f);
}
static void patch_vt(void **vt, int idx, void *fn, void **orig)
{
    DWORD old;
    if (*orig || vt[idx] == fn) return;
    *orig = vt[idx];
    VirtualProtect(&vt[idx], sizeof(void *), PAGE_READWRITE, &old);
    vt[idx] = fn;
    VirtualProtect(&vt[idx], sizeof(void *), old, &old);
}
static HRESULT STDMETHODCALLTYPE my_CreateDevice(IDirectInputA *This, REFGUID g, IDirectInputDeviceA **out, IUnknown *outer)
{
    HRESULT r = orig_CreateDevice(This, g, out, outer);
    if (SUCCEEDED(r) && out && *out && IsEqualGUID(g, &GUID_SysMouse))
    {
        void **vt = *(void ***)*out;
        g_mouse_dev = *out;
        mlog("DI CreateDevice(SysMouse) -> %d%d%d%d", 1, 0, 0, 0);
        patch_vt(vt, 9, my_GetDeviceState, (void **)&orig_GetDeviceState);
        patch_vt(vt, 10, my_GetDeviceData, (void **)&orig_GetDeviceData);
        patch_vt(vt, 6, my_SetProperty, (void **)&orig_SetProperty);
        patch_vt(vt, 11, my_SetDataFormat, (void **)&orig_SetDataFormat);
        patch_vt(vt, 13, my_SetCoop, (void **)&orig_SetCoop);
    }
    return r;
}

__declspec(dllexport) HRESULT WINAPI DirectInputCreateA(HINSTANCE hinst, DWORD ver, LPDIRECTINPUTA *out, LPUNKNOWN outer)
{
    static HRESULT (WINAPI *fn)(HINSTANCE, DWORD, LPDIRECTINPUTA *, LPUNKNOWN);
    install_dd_hooks();
    if (!fn)
    {
        char path[MAX_PATH];
        HMODULE m;
        GetSystemDirectoryA(path, MAX_PATH);
        lstrcatA(path, "\\dinput.dll");           /* absolute path -> the real (builtin) dinput */
        m = LoadLibraryA(path);
        if (m) fn = (void *)GetProcAddress(m, "DirectInputCreateA");
    }
    {
        HRESULT hr = fn ? fn(hinst, ver, out, outer) : E_FAIL;
        if (SUCCEEDED(hr) && out && *out)
        {
            IDirectInputA *di = (IDirectInputA *)*out;
            patch_vt(*(void ***)di, 3, my_CreateDevice, (void **)&orig_CreateDevice);
            mlog("DirectInputCreateA ver=%x%d%d%d", ver, 0, 0, 0);
        }
        return hr;
    }
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *res)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        char tmp[8];
        DisableThreadLibraryCalls(inst);
        if (GetEnvironmentVariableA("MMIOFIX_LOG", tmp, sizeof(tmp)))
            g_log = CreateFileA("mmiofix.log", GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
        if (GetEnvironmentVariableA("MMIOFIX_MOUSE", tmp, sizeof(tmp)) && !strcmp(tmp, "off")) g_mouse_swallow = 0;
        logf_("loaded, patched %s imports: %u", "winmm", patch_iat(GetModuleHandleW(NULL)));
    }
    return TRUE;
}
