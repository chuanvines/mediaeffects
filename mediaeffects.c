/*
 * mediaeffects.c - apply effects to images and videos
 *
 * Windows executable port of mediaeffects.sh. Requires ffmpeg, ffprobe and
 * ImageMagick (magick) on PATH.
 *
 * Build:
 *   gcc -O2 -o mediaeffects.exe mediaeffects.c
 *
 * Videos whose effects are all expressible as ffmpeg filters are encoded in a
 * single pass with no intermediate frames. When ImageMagick is required, frames
 * are extracted as PNG (lossless, so effects never degrade quality), ImageMagick
 * runs on disjoint frame ranges in parallel, and the frames are reassembled with
 * ffmpeg.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <process.h>

#define MAX_FX 32
#define BIG 16384

enum {
    FX_INVERT, FX_INVERTLUM, FX_FISHEYE, FX_HSV, FX_HUE, FX_EXPLODE, FX_SWIRL,
    FX_HFLIP, FX_VFLIP, FX_ROTATE, FX_MAGIK, FX_HAAH, FX_WAAW, FX_HOOH, FX_WOOW,
    FX_STRETCH, FX_RESIZE, FX_DERAIN, FX_RAIN, FX_BGR, FX_WAVE, FX_NONE
};

static const char *fx_names[] = {
    "invert", "invertlum", "fisheye", "hsv", "hue", "explode", "swirl",
    "hflip", "vflip", "rotate", "magik", "haah", "waaw", "hooh", "woow",
    "stretch", "resize", "derain", "rain", "bgr", "wave"
};

static int hide = 0;
static char *input = NULL;
static char *output = NULL;
static int fx_types[MAX_FX];
static const char *fx_params[MAX_FX];
static int nfx = 0;

static char tempdir[MAX_PATH];
static char cleanup_dir[MAX_PATH] = "";

static void logmsg(const char *fmt, ...) {
    va_list ap;
    if (hide) return;
    printf("[mediaeffects] ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static void errmsg(const char *fmt, ...) {
    va_list ap;
    fputs("mediaeffects: error: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

/* ---- process helpers ---------------------------------------------------- */

/* Run a command line (via CreateProcess, no cmd.exe) and wait for it.
 * Returns the exit code; -1 if the process could not be started. */
static int run_cmdline(const char *cmd) {
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD code;
    if (!hide) printf("[mediaeffects] exec: %s\n", cmd);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    char *cmdline = _strdup(cmd);
    if (!CreateProcessA(NULL, cmdline, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        free(cmdline);
        return -1;
    }
    free(cmdline);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

static void run(const char *fmt, ...) {
    char cmd[BIG];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    int rc = run_cmdline(cmd);
    if (rc != 0) errmsg("command failed: %s", cmd);
}

/* Run a command with stdout redirected to a file. */
static void run_capture(const char *outfile, const char *fmt, ...) {
    char cmd[BIG];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    if (!hide) printf("[mediaeffects] exec: %s\n", cmd);
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;
    HANDLE hOut = CreateFileA(outfile, GENERIC_WRITE, FILE_SHARE_WRITE, &sa,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hOut;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    memset(&pi, 0, sizeof(pi));
    char *cmdline = _strdup(cmd);
    if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        free(cmdline);
        CloseHandle(hOut);
        errmsg("could not start: %s", cmd);
    }
    free(cmdline);
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hOut);
}

/* Read the first non-empty line of a file into buf. */
static void read_line(const char *path, char *buf, size_t n) {
    FILE *f = fopen(path, "r");
    buf[0] = 0;
    if (!f) return;
    if (fgets(buf, (int)n, f) == NULL) buf[0] = 0;
    fclose(f);
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        buf[--len] = 0;
}

/* Recursively delete a directory tree. */
static void delete_tree(const char *dir) {
    char pat[MAX_PATH];
    WIN32_FIND_DATAA fd;
    _snprintf(pat, sizeof(pat), "%s\\*", dir);
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            continue;
        char path[MAX_PATH];
        _snprintf(path, sizeof(path), "%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            delete_tree(path);
        else
            DeleteFileA(path);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    RemoveDirectoryA(dir);
}

static void cleanup() {
    if (cleanup_dir[0]) delete_tree(cleanup_dir);
}

/* ---- effects ------------------------------------------------------------ */

static int uses_ffmpeg(int fx) {
    switch (fx) {
    case FX_INVERT: case FX_BGR: case FX_FISHEYE: case FX_HFLIP: case FX_VFLIP:
    case FX_ROTATE: case FX_STRETCH: case FX_WAVE: case FX_HAAH: case FX_WAAW:
    case FX_HOOH: case FX_WOOW: case FX_RESIZE:
        return 1;
    default:
        return 0;
    }
}

static const char *fx_filename(int fx) {
    return fx_names[fx];
}

/* dims is "WxH"; returns "W:H" for crop/scale strings. */
static void dims_colon(const char *dims, char *out, size_t n) {
    size_t i;
    for (i = 0; i < n - 1 && dims[i]; i++)
        out[i] = (dims[i] == 'x' || dims[i] == 'X') ? ':' : dims[i];
    out[i] = 0;
}

/* Append the ffmpeg filter string for one effect to *dst. */
static void append_vf(char *dst, size_t n, int fx, const char *param, const char *dims) {
    char tmp[4096];
    size_t len = strlen(dst);
    char *p = dst + len;
    size_t rem = n - len;

    switch (fx) {
    case FX_INVERT:
        _snprintf(p, rem, "format=rgb24,negate");
        break;
    case FX_HFLIP:
        _snprintf(p, rem, "hflip");
        break;
    case FX_VFLIP:
        _snprintf(p, rem, "vflip");
        break;
    case FX_ROTATE:
        _snprintf(p, rem, "rotate=%s*3.141592653589793/180", param);
        break;
    case FX_FISHEYE:
        _snprintf(p, rem, "format=yuv444p16le,geq='p(W*0.5+(X-W*0.5)*max(1-(%s)*gauss(-3.3333*pow(hypot((X-W*0.5)/(W*0.5),(Y-H*0.5)/(H*0.5)),2)),0),H*0.5+(Y-H*0.5)*max(1-(%s)*gauss(-3.3333*pow(hypot((X-W*0.5)/(W*0.5),(Y-H*0.5)/(H*0.5)),2)),0))',scale=iw:ih,format=yuv420p",
                  param, param);
        break;
    case FX_STRETCH: {
        char x[64], y[64], d[64];
        const char *comma = strchr(param, ',');
        size_t xl = comma ? (size_t)(comma - param) : strlen(param);
        _snprintf(x, sizeof(x), "%.*s", (int)xl, param);
        _snprintf(y, sizeof(y), "%s", comma ? comma + 1 : x);
        dims_colon(dims, d, sizeof(d));
        _snprintf(p, rem, "format=yuv444p,rotate=0:iw*1.1:ih*1.1,geq='p((W/2)+(X-(W/2))/%s,(H/2)+(Y-(H/2))/%s)',scale=iw:ih,crop=%s,format=yuv420p",
                  x, y, d);
        break;
    }
    case FX_WAVE: {
        double xf, yf, xa, ya, xp, yp, xs, ys;
        sscanf(param, "%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf",
               &xf, &yf, &xa, &ya, &xp, &yp, &xs, &ys);
        char d[64];
        dims_colon(dims, d, sizeof(d));
        _snprintf(p, rem, "format=yuv444p,scale=640:640,geq='p(X-((sin((T*5*%.4f+(%.4f))+(Y/H)*(PI*%.4f)))*(-%.4f*600)),Y-((sin((T*5*%.4f+(%.4f))+(X/W)*(PI*%.4f)))*(-%.4f*600)))',scale=%s,format=yuv420p",
                  xs, xp, xf, xa, ys, yp, yf, ya, d);
        break;
    }
    case FX_BGR:
        _snprintf(p, rem, "colorchannelmixer=rr=0:rg=0:rb=1:gr=0:gg=1:gb=0:br=1:bg=0:bb=0");
        break;
    case FX_HAAH:
        _snprintf(p, rem, "crop=iw/2:ih:0:0,split[a][b];[b]hflip[c];[a][c]hstack=inputs=2");
        break;
    case FX_WAAW:
        _snprintf(p, rem, "hflip,crop=iw/2:ih:0:0,split[a][b];[b]hflip[c];[a][c]hstack=inputs=2");
        break;
    case FX_HOOH:
        _snprintf(p, rem, "transpose=1,crop=iw/2:ih:0:0,split[a][b];[b]hflip[c];[a][c]hstack=inputs=2,transpose=2");
        break;
    case FX_WOOW:
        _snprintf(p, rem, "transpose=1,hflip,crop=iw/2:ih:0:0,split[a][b];[b]hflip[c];[a][c]hstack=inputs=2,transpose=2");
        break;
    case FX_RESIZE: {
        if (strchr(param, 'x') || strchr(param, 'X')) {
            _snprintf(p, rem, "scale=%s:force_original_aspect_ratio=decrease:force_divisible_by=2", param);
        } else if (strchr(param, '%')) {
            char pct[64];
            _snprintf(pct, sizeof(pct), "%.*s", (int)(strlen(param) - 1), param);
            _snprintf(p, rem, "scale=trunc(iw*%s/100/2)*2:trunc(ih*%s/100/2)*2", pct, pct);
        } else {
            _snprintf(p, rem, "scale=%s:-2", param);
        }
        break;
    }
    default:
        p[0] = 0;
        return;
    }
    (void)tmp;
}

/* Append the ImageMagick option string for one effect to *dst. */
static void append_magick(char *dst, size_t n, int fx, const char *param) {
    char tmp[4096];
    size_t len = strlen(dst);
    char *p = dst + len;
    size_t rem = n - len;
    double v;

    switch (fx) {
    case FX_INVERTLUM:
        _snprintf(p, rem, " -colorspace lab -channel r -negate +colorspace");
        break;
    case FX_HSV: {
        int h = 0, s = 0, va = 0;
        sscanf(param, "%d,%d,%d", &h, &s, &va);
        _snprintf(p, rem, " -modulate %d,%d,%d", 100 + va, 100 + s,
                  100 + (int)(h * 100 / 180));
        break;
    }
    case FX_HUE:
        _snprintf(p, rem, " -colorspace yuv -fx angle=%s*pi/180;channel(u,.5+(u.g-.5)*cos(angle)-(u.b-.5)*sin(angle),.5+(u.g-.5)*sin(angle)+(u.b-.5)*cos(angle)) -colorspace srgb",
                  param);
        break;
    case FX_EXPLODE:
        v = atof(param);
        _snprintf(p, rem, " -implode %.4f", -v);
        break;
    case FX_SWIRL:
        _snprintf(p, rem, " -swirl %s", param);
        break;
    case FX_MAGIK:
        _snprintf(p, rem, " -liquid-rescale 50%%x50%% -resize 200%%");
        break;
    case FX_DERAIN:
        _snprintf(p, rem, " -colorspace srgb -set colorspace rgb -colorspace hsv -set colorspace rgb");
        break;
    case FX_RAIN:
        _snprintf(p, rem, " -colorspace srgb -set colorspace hsv -colorspace scrgb -set colorspace srgb");
        break;
    default:
        p[0] = 0;
        return;
    }
    (void)tmp;
}

/* ---- segments ----------------------------------------------------------- */

typedef struct {
    int type;          /* 0 = ffmpeg, 1 = magick */
    char arg[8192];
} Segment;

static Segment segs[MAX_FX];
static int nsegs = 0;

static void build_segments(const char *dims) {
    int i;
    char tmp[8192];
    nsegs = 0;
    for (i = 0; i < nfx; i++) {
        int type = uses_ffmpeg(fx_types[i]) ? 0 : 1;
        tmp[0] = 0;
        if (uses_ffmpeg(fx_types[i]))
            append_vf(tmp, sizeof(tmp), fx_types[i], fx_params[i], dims);
        else
            append_magick(tmp, sizeof(tmp), fx_types[i], fx_params[i]);
        if (nsegs > 0 && segs[nsegs - 1].type == type) {
            if (type == 0)
                strncat(segs[nsegs - 1].arg, ",", sizeof(segs[nsegs - 1].arg) - strlen(segs[nsegs - 1].arg) - 1);
            strncat(segs[nsegs - 1].arg, tmp, sizeof(segs[nsegs - 1].arg) - strlen(segs[nsegs - 1].arg) - 1);
        } else {
            segs[nsegs].type = type;
            _snprintf(segs[nsegs].arg, sizeof(segs[nsegs].arg), "%s", tmp);
            nsegs++;
        }
    }
}

/* ---- effect docs -------------------------------------------------------- */

static void write_effect_c(int fx, const char *param) {
    FILE *f;
    char path[MAX_PATH];
    _snprintf(path, sizeof(path), "effects\\%s.c", fx_filename(fx));
    CreateDirectoryA("effects", NULL);
    f = fopen(path, "w");
    if (!f) return;
    switch (fx) {
    case FX_INVERT:
        fprintf(f, "/* invert.c - ffmpeg format=rgb24,negate */\nconst char *const mediaeffects_invert_vf = \"format=rgb24,negate\";\n");
        break;
    case FX_INVERTLUM:
        fprintf(f, "/* invertlum.c - ImageMagick LAB invert */\nconst char *const mediaeffects_invertlum_magick = \"-colorspace lab -channel r -negate +colorspace\";\n");
        break;
    case FX_FISHEYE: {
        char vf[4096];
        vf[0] = 0;
        append_vf(vf, sizeof(vf), fx, param, "WxH");
        fprintf(f, "/* fisheye.c - strength %s */\nconst char *const mediaeffects_fisheye_vf = \"%s\";\n", param, vf);
        break;
    }
    case FX_HSV:
        fprintf(f, "/* hsv.c - %s */\nconst char *const mediaeffects_hsv_magick = \"-modulate ...\";\n", param);
        break;
    case FX_HUE:
        fprintf(f, "/* hue.c - angle %s */\nconst char *const mediaeffects_hue_magick = \"-colorspace yuv -fx ...\";\n", param);
        break;
    case FX_EXPLODE:
        fprintf(f, "/* explode.c - strength %s */\nconst char *const mediaeffects_explode_magick = \"-implode ...\";\n", param);
        break;
    case FX_SWIRL:
        fprintf(f, "/* swirl.c - strength %s */\nconst char *const mediaeffects_swirl_magick = \"-swirl %s\";\n", param, param);
        break;
    case FX_HFLIP:
        fprintf(f, "/* hflip.c - ffmpeg hflip */\nconst char *const mediaeffects_hflip_vf = \"hflip\";\n");
        break;
    case FX_VFLIP:
        fprintf(f, "/* vflip.c - ffmpeg vflip */\nconst char *const mediaeffects_vflip_vf = \"vflip\";\n");
        break;
    case FX_ROTATE:
        fprintf(f, "/* rotate.c - angle %s */\nconst char *const mediaeffects_rotate_vf = \"rotate=%s*3.141592653589793/180\";\n", param, param);
        break;
    case FX_MAGIK:
        fprintf(f, "/* magik.c - ImageMagick liquid rescale */\nconst char *const mediaeffects_magik_magick = \"-liquid-rescale 50%%x50%% -resize 200%%\";\n");
        break;
    case FX_HAAH:
        fprintf(f, "/* haah.c - ffmpeg mirror half */\nconst char *const mediaeffects_haah_vf = \"crop=iw/2:ih:0:0,split[a][b];[b]hflip[c];[a][c]hstack=inputs=2\";\n");
        break;
    case FX_WAAW:
        fprintf(f, "/* waaw.c - ffmpeg */\nconst char *const mediaeffects_waaw_vf = \"hflip,crop=iw/2:ih:0:0,split[a][b];[b]hflip[c];[a][c]hstack=inputs=2\";\n");
        break;
    case FX_HOOH:
        fprintf(f, "/* hooh.c - ffmpeg */\nconst char *const mediaeffects_hooh_vf = \"transpose=1,crop=iw/2:ih:0:0,split[a][b];[b]hflip[c];[a][c]hstack=inputs=2,transpose=2\";\n");
        break;
    case FX_WOOW:
        fprintf(f, "/* woow.c - ffmpeg */\nconst char *const mediaeffects_woow_vf = \"transpose=1,hflip,crop=iw/2:ih:0:0,split[a][b];[b]hflip[c];[a][c]hstack=inputs=2,transpose=2\";\n");
        break;
    case FX_STRETCH: {
        char vf[4096];
        vf[0] = 0;
        append_vf(vf, sizeof(vf), fx, param, "WxH");
        fprintf(f, "/* stretch.c - %s */\nconst char *const mediaeffects_stretch_vf = \"%s\";\n", param, vf);
        break;
    }
    case FX_RESIZE: {
        char vf[4096];
        vf[0] = 0;
        append_vf(vf, sizeof(vf), fx, param, "WxH");
        fprintf(f, "/* resize.c - %s */\nconst char *const mediaeffects_resize_vf = \"%s\";\n", param, vf);
        break;
    }
    case FX_DERAIN:
        fprintf(f, "/* derain.c - ImageMagick */\nconst char *const mediaeffects_derain_magick = \"-colorspace srgb -set colorspace rgb -colorspace hsv -set colorspace rgb\";\n");
        break;
    case FX_RAIN:
        fprintf(f, "/* rain.c - ImageMagick */\nconst char *const mediaeffects_rain_magick = \"-colorspace srgb -set colorspace hsv -colorspace scrgb -set colorspace srgb\";\n");
        break;
    case FX_BGR:
        fprintf(f, "/* bgr.c - ffmpeg colorchannelmixer */\nconst char *const mediaeffects_bgr_vf = \"colorchannelmixer=rr=0:rg=0:rb=1:gr=0:gg=1:gb=0:br=1:bg=0:bb=0\";\n");
        break;
    case FX_WAVE: {
        char vf[4096];
        vf[0] = 0;
        append_vf(vf, sizeof(vf), fx, param, "WxH");
        fprintf(f, "/* wave.c - %s */\nconst char *const mediaeffects_wave_vf = \"%s\";\n", param, vf);
        break;
    }
    default:
        break;
    }
    fclose(f);
    logmsg("effect: %s -> effects\\%s.c", fx_filename(fx), fx_filename(fx));
}

/* ---- image processing --------------------------------------------------- */

static void process_image(void) {
    int i, ns;
    char dims[64];
    if (nfx == 0) {
        run("magick \"%s\" \"%s\"", input, output);
        return;
    }
    dims[0] = 0;
    /* get dims via a temp file */
    {
        char dimfile[MAX_PATH];
        _snprintf(dimfile, sizeof(dimfile), "%s\\dims.txt", tempdir);
        run_capture(dimfile, "magick \"%s\" -format %%wx%%h info:", input);
        read_line(dimfile, dims, sizeof(dims));
    }
    build_segments(dims);
    ns = nsegs;
    for (i = 0; i < ns; i++) {
        const char *in = (i == 0) ? input : cleanup_dir;
        const char *out = (i == ns - 1) ? output : cleanup_dir;
        char curin[MAX_PATH], curout[MAX_PATH];
        if (i == 0) _snprintf(curin, sizeof(curin), "%s", input);
        else _snprintf(curin, sizeof(curin), "%s\\img_%d.png", cleanup_dir, i - 1);
        if (i == ns - 1) _snprintf(curout, sizeof(curout), "%s", output);
        else _snprintf(curout, sizeof(curout), "%s\\img_%d.png", cleanup_dir, i);
        logmsg("processing: %s pass %d/%d", segs[i].type ? "magick" : "ffmpeg", i + 1, ns);
        if (segs[i].type == 1)
            run("magick \"%s\" %s \"%s\"", curin, segs[i].arg, curout);
        else
            run("ffmpeg -y -v error -i \"%s\" -vf \"%s\" \"%s\"", curin, segs[i].arg, curout);
        (void)in; (void)out;
    }
}

/* ---- video processing --------------------------------------------------- */

static int count_frames(const char *dir, const char *cur) {
    WIN32_FIND_DATAA fd;
    char pat[MAX_PATH];
    int count = 0;
    _snprintf(pat, sizeof(pat), "%s\\%s_*.png", dir, cur);
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        count++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return count;
}

/* Run ImageMagick on disjoint frame ranges in parallel; -scene keeps the
 * output numbering aligned with each chunk's starting frame. */
static void video_pass_magick(const char *dir, const char *cur, const char *next,
                              const char *args) {
    int nframes, cores, chunk, start, n, i;
    nframes = count_frames(dir, cur);
    if (nframes <= 1) {
        run("magick \"%s\\%s_*.png\" %s -define png:compression-level=1 \"%s\\%s_%%05d.png\"", dir, cur, args, dir, next);
        return;
    }
    {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        cores = (int)si.dwNumberOfProcessors;
    }
    if (cores < 1) cores = 1;
    if (cores > nframes) cores = nframes;
    chunk = (nframes + cores - 1) / cores;
    n = (nframes + chunk - 1) / chunk;
    {
        HANDLE *handles = malloc(sizeof(HANDLE) * n);
        char **cmds = malloc(sizeof(char *) * n);
        for (start = 0, i = 0; start < nframes; start += chunk, i++) {
            int hi = start + chunk - 1;
            if (hi >= nframes) hi = nframes - 1;
            cmds[i] = malloc(BIG);
            _snprintf(cmds[i], BIG,
                      "magick \"%s\\%s_%%05d.png[%d-%d]\" %s -define png:compression-level=1 -scene %d \"%s\\%s_%%05d.png\"",
                      dir, cur, start, hi, args, start, dir, next);
            if (!hide) printf("[mediaeffects] exec: %s\n", cmds[i]);
            STARTUPINFOA si0;
            PROCESS_INFORMATION pi;
            memset(&si0, 0, sizeof(si0));
            si0.cb = sizeof(si0);
            memset(&pi, 0, sizeof(pi));
            if (CreateProcessA(NULL, cmds[i], NULL, NULL, FALSE, CREATE_NO_WINDOW,
                               NULL, NULL, &si0, &pi))
                handles[i] = pi.hProcess;
            else
                handles[i] = NULL;
        }
        for (start = 0, i = 0; start < nframes; start += chunk, i++) {
            if (handles[i]) {
                DWORD code;
                WaitForSingleObject(handles[i], INFINITE);
                GetExitCodeProcess(handles[i], &code);
                CloseHandle(handles[i]);
                if (code != 0) {
                    errmsg("magick frame pass failed");
                }
            }
        }
        for (i = 0; i < n; i++) free(cmds[i]);
        free(cmds);
        free(handles);
    }
}

static void video_pass_ffmpeg(const char *dir, const char *cur, const char *next,
                              const char *vf) {
    run("ffmpeg -y -v error -start_number 0 -i \"%s\\%s_%%05d.png\" -vf \"%s\" -start_number 0 \"%s\\%s_%%05d.png\"",
        dir, cur, vf, dir, next);
}

static void get_video_fps(const char *vinput, const char *fpsfile, char *fps, size_t n) {
    run_capture(fpsfile, "ffprobe -v error -select_streams v:0 -show_entries stream=avg_frame_rate -of csv=p=0 -i \"%s\"",
                vinput);
    read_line(fpsfile, fps, n);
    if (fps[0] == 0 || strcmp(fps, "0/0") == 0 || strcmp(fps, "N/A") == 0)
        _snprintf(fps, n, "%s", "25");
}

static void process_video(void) {
    char fps[64], dims[64];
    char cur[2] = "f";
    char *next;
    int i, ns;

    if (nfx == 0) {
        run("ffmpeg -y -v error -i \"%s\" -map 0:v -map 0:a? -c copy \"%s\"", input, output);
        return;
    }
    {
        char dimfile[MAX_PATH];
        _snprintf(dimfile, sizeof(dimfile), "%s\\dims.txt", tempdir);
        run_capture(dimfile, "ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=s=x:p=0 -i \"%s\"",
                    input);
        read_line(dimfile, dims, sizeof(dims));
    }
    build_segments(dims);
    ns = nsegs;

    /* Fast path: every effect is an ffmpeg filter -> single pass, no frames. */
    if (ns == 1 && segs[0].type == 0) {
        run("ffmpeg -y -v error -i \"%s\" -vf \"%s\" -c:v libx264 -crf 18 -preset veryfast -pix_fmt yuv420p -c:a aac -b:a 128k \"%s\"",
            input, segs[0].arg, output);
        return;
    }

    run("ffmpeg -y -v error -i \"%s\" -map 0:v -start_number 0 -c:v png \"%s\\f_%%05d.png\"",
        input, tempdir);

    {
        char fpsfile[MAX_PATH];
        _snprintf(fpsfile, sizeof(fpsfile), "%s\\fps.txt", tempdir);
        get_video_fps(input, fpsfile, fps, sizeof(fps));
    }

    for (i = 0; i < ns; i++) {
        const char *cnext = (strcmp(cur, "a") == 0) ? "b" : "a";
        next = (char *)cnext;
        logmsg("processing: %s pass %d/%d", segs[i].type ? "magick" : "ffmpeg", i + 1, ns);
        if (segs[i].type == 1)
            video_pass_magick(tempdir, cur, next, segs[i].arg);
        else
            video_pass_ffmpeg(tempdir, cur, next, segs[i].arg);
        cur[0] = next[0];
    }

    run("ffmpeg -y -v error -r %s -start_number 0 -i \"%s\\%s_%%05d.png\" -i \"%s\" -map 0:v -map 1:a? -c:v libx264 -crf 18 -preset veryfast -pix_fmt yuv420p -c:a aac -b:a 128k \"%s\"",
        fps, tempdir, cur, input, output);
}

/* ---- argument parsing --------------------------------------------------- */

static int is_num(const char *s) {
    char *end;
    strtod(s, &end);
    return *end == 0 && end != s;
}

static int is_csv(const char *s) {
    const char *p = s;
    int n = 0;
    if (!*s) return 0;
    for (;;) {
        char *end;
        strtod(p, &end);
        if (end == p) return 0;
        n++;
        if (*end == 0) break;
        if (*end != ',' && *end != ';') return 0;
        p = end + 1;
    }
    return n >= 1;
}

static int is_size(const char *s) {
    const char *p = s;
    if (!*s) return 0;
    while (*p && *p >= '0' && *p <= '9') p++;
    if (*p == 'x' || *p == 'X') {
        p++;
        if (!(*p >= '0' && *p <= '9')) return 0;
        while (*p && *p >= '0' && *p <= '9') p++;
    }
    if (*p == '%') p++;
    return *p == 0;
}

static const char *ext(const char *path) {
    const char *dot = strrchr(path, '.');
    return dot ? dot + 1 : "";
}

static int has_ext(const char *path, const char **list, int n) {
    const char *e = ext(path);
    int i;
    for (i = 0; i < n; i++)
        if (_stricmp(e, list[i]) == 0) return 1;
    return 0;
}

static int is_img(const char *path) {
    static const char *imgs[] = { "png", "jpg", "jpeg", "gif" };
    return has_ext(path, imgs, 4);
}

static int is_vid(const char *path) {
    static const char *vids[] = { "mp4", "mov", "mxf", "mkv", "avi" };
    return has_ext(path, vids, 5);
}

static void add_fx(int fx, const char *param) {
    if (nfx >= MAX_FX) errmsg("too many effects (max %d)", MAX_FX);
    fx_types[nfx] = fx;
    fx_params[nfx] = param ? _strdup(param) : "";
    nfx++;
}

static void help(void) {
    printf(
        "mediaeffects - apply effects to images and videos\n"
        "\n"
        "Usage:\n"
        "  mediaeffects <input> <output> [effects...] [options]\n"
        "\n"
        "Effects:\n"
        "  --invert              Negate colors (ffmpeg -vf negate style)\n"
        "  --invertlum           Invert luminosity in LAB colorspace\n"
        "                        (ImageMagick -colorspace lab -channel r -negate style)\n"
        "  --fisheye [strength]  Fisheye distortion, default 0.5 (ffmpeg geq style)\n"
        "  --hsv H,S,V           Hue shift in degrees, saturation and value shifts\n"
        "                        (0 = no change). e.g. --hsv 180,0,0\n"
        "                        (ImageMagick -modulate style)\n"
        "  --hue [degrees]       Rotate hue in degrees, default 90 (ImageMagick\n"
        "                        -colorspace yuv -fx channel rotation style)\n"
        "  --explode [strength]  Explode outward, default 1.0 (-implode style)\n"
        "  --swirl [strength]    Swirl in degrees, default 45 (-swirl style)\n"
        "  --hflip               Mirror horizontally (ffmpeg -vf hflip)\n"
        "  --vflip               Mirror vertically (ffmpeg -vf vflip)\n"
        "  --rotate [degrees]    Rotate by degrees, default 90 (ffmpeg -vf rotate)\n"
        "  --magik               Liquid-rescale to 50%%x50%%, then resize 200%%\n"
        "  --haah                Mirror left half onto right (ffmpeg crop/hflip/hstack)\n"
        "  --waaw                Flop, then mirror left half onto right\n"
        "  --hooh                Rotate 90, mirror half, rotate back\n"
        "  --woow                Rotate 90 + flop, mirror half, rotate back\n"
        "  --stretch [x,y]       Stretch/squeeze horizontally and vertically,\n"
        "                        default 1.1,1.1 (ffmpeg geq style)\n"
        "                        numbers separated by , or ;\n"
        "  --resize [WxH|W|N%%]   Resize with ffmpeg scale, e.g. 1280x720, 500, 50%%\n"
        "  --derain              Fake derain color trick (ImageMagick colorspace)\n"
        "  --rain                Fake rain color trick (ImageMagick colorspace)\n"
        "  --bgr                 Swap color channels to BGR (ffmpeg colorchannelmixer)\n"
        "  --wave [params]       Sinusoidal wave distortion (ffmpeg geq style).\n"
        "                        params: xfreq,yfreq,xamp,yamp,xphase,yphase[,xspeed,yspeed]\n"
        "                        default 3.2,3.2,0.05,0.05,0.628,0.628,0,0\n"
        "                        numbers separated by , or ;\n"
        "\n"
        "Options:\n"
        "  --hidelogs            Hide progress logs\n"
        "  --help                Show this help and exit\n"
        "\n"
        "Supported formats:\n"
        "  Images: .png .jpg .jpeg .gif\n"
        "  Videos: .mp4 .mov .mxf .mkv .avi\n"
        "\n"
        "Videos whose effects are all expressible as ffmpeg filters are encoded in a\n"
        "single pass with no intermediate files. When ImageMagick is required, frames\n"
        "are extracted as PNG (lossless), processed in parallel chunks, and reassembled\n"
        "with ffmpeg.\n"
        "\n"
        "Effect source code is generated into the effects/ folder\n"
        "(invert.c, invertlum.c, fisheye.c, hsv.c, hue.c, explode.c, swirl.c, hflip.c,\n"
        "vflip.c, rotate.c, magik.c, haah.c, waaw.c, hooh.c, woow.c, stretch.c,\n"
        "resize.c, derain.c, rain.c, bgr.c, wave.c).\n"
        "\n"
        "Examples:\n"
        "  mediaeffects in.png out.png --invert\n"
        "  mediaeffects in.png out.png --invert --fisheye 1 --hsv 180,0,0\n"
        "  mediaeffects clip.mp4 out.mp4 --fisheye 0.5 --hsv 45,20,-10\n");
}

static void parse_args(int argc, char **argv) {
    int i;
    char buf[256];
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            help();
            exit(0);
        } else if (strcmp(a, "--hidelogs") == 0) {
            hide = 1;
        } else if (strcmp(a, "--invert") == 0) {
            add_fx(FX_INVERT, "");
        } else if (strcmp(a, "--invertlum") == 0) {
            add_fx(FX_INVERTLUM, "");
        } else if (strcmp(a, "--fisheye") == 0) {
            const char *p = "0.5";
            if (i + 1 < argc && is_num(argv[i + 1])) p = argv[++i];
            add_fx(FX_FISHEYE, p);
        } else if (strcmp(a, "--explode") == 0) {
            const char *p = "1.0";
            if (i + 1 < argc && is_num(argv[i + 1])) p = argv[++i];
            add_fx(FX_EXPLODE, p);
        } else if (strcmp(a, "--swirl") == 0) {
            const char *p = "45";
            if (i + 1 < argc && is_num(argv[i + 1])) p = argv[++i];
            add_fx(FX_SWIRL, p);
        } else if (strcmp(a, "--rotate") == 0) {
            const char *p = "90";
            if (i + 1 < argc && is_num(argv[i + 1])) p = argv[++i];
            add_fx(FX_ROTATE, p);
        } else if (strcmp(a, "--hsv") == 0) {
            if (i + 1 >= argc)
                errmsg("'--hsv' requires a value like 180,0,0");
            add_fx(FX_HSV, argv[++i]);
        } else if (strcmp(a, "--hue") == 0) {
            const char *p = "90";
            if (i + 1 < argc && is_num(argv[i + 1])) p = argv[++i];
            add_fx(FX_HUE, p);
        } else if (strcmp(a, "--hflip") == 0) {
            add_fx(FX_HFLIP, "");
        } else if (strcmp(a, "--vflip") == 0) {
            add_fx(FX_VFLIP, "");
        } else if (strcmp(a, "--magik") == 0) {
            add_fx(FX_MAGIK, "");
        } else if (strcmp(a, "--haah") == 0) {
            add_fx(FX_HAAH, "");
        } else if (strcmp(a, "--waaw") == 0) {
            add_fx(FX_WAAW, "");
        } else if (strcmp(a, "--hooh") == 0) {
            add_fx(FX_HOOH, "");
        } else if (strcmp(a, "--woow") == 0) {
            add_fx(FX_WOOW, "");
        } else if (strcmp(a, "--derain") == 0) {
            add_fx(FX_DERAIN, "");
        } else if (strcmp(a, "--rain") == 0) {
            add_fx(FX_RAIN, "");
        } else if (strcmp(a, "--bgr") == 0) {
            add_fx(FX_BGR, "");
        } else if (strcmp(a, "--stretch") == 0) {
            const char *p = "1.1,1.1";
            if (i + 1 < argc && is_csv(argv[i + 1])) {
                _snprintf(buf, sizeof(buf), "%s", argv[++i]);
                { char *s; while ((s = strchr(buf, ';'))) *s = ','; }
                p = buf;
            }
            if (!strchr(p, ',')) {
                _snprintf(buf, sizeof(buf), "%s,%s", p, p);
                p = buf;
            }
            add_fx(FX_STRETCH, p);
        } else if (strcmp(a, "--resize") == 0) {
            if (i + 1 >= argc)
                errmsg("'--resize' requires a size like 1280x720, 500 or 50%%");
            if (!is_size(argv[i + 1]))
                errmsg("invalid size '%s' for '--resize' (expected 1280x720, 500 or 50%%)", argv[i + 1]);
            add_fx(FX_RESIZE, argv[++i]);
        } else if (strcmp(a, "--wave") == 0) {
            const char *p = "3.2,3.2,0.05,0.05,0.628,0.628,0,0";
            if (i + 1 < argc && is_csv(argv[i + 1])) {
                _snprintf(buf, sizeof(buf), "%s", argv[++i]);
                { char *s; while ((s = strchr(buf, ';'))) *s = ','; }
                p = buf;
                /* pad wave to 8 values */
                {
                    double v[8] = {0,0,0,0,0,0,0,0};
                    int got = sscanf(p, "%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf",
                                     &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
                    _snprintf(buf, sizeof(buf), "%g,%g,%g,%g,%g,%g,%g,%g",
                              v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
                    (void)got;
                    p = buf;
                }
            }
            add_fx(FX_WAVE, p);
        } else if (a[0] == '-' && a[1] == '-') {
            errmsg("unknown option '%s' (see --help)", a);
        } else if (!input) {
            input = _strdup(a);
        } else if (!output) {
            output = _strdup(a);
        } else {
            errmsg("unexpected argument '%s' (see --help)", a);
        }
    }
}

int main(int argc, char **argv) {
    char tmp[MAX_PATH];
    int i;
    if (argc < 2) {
        help();
        errmsg("missing input or output file");
    }
    parse_args(argc, argv);
    atexit(cleanup);

    if (!input || !output) {
        help();
        errmsg("missing input or output file");
    }
    if (strcmp(input, output) == 0)
        errmsg("input and output are the same file: %s", input);
    {
        WIN32_FIND_DATAA fd;
        if (FindFirstFileA(input, &fd) == INVALID_HANDLE_VALUE)
            errmsg("input file not found or unreadable: %s", input);
    }
    if (!is_img(input) && !is_vid(input))
        errmsg("unsupported input format '.%s'", ext(input));
    if (!is_img(output) && !is_vid(output))
        errmsg("unsupported output format '.%s'", ext(output));

    /* temp dir */
    GetTempPathA(sizeof(tmp), tmp);
    _snprintf(tempdir, sizeof(tempdir), "%smediaeffects_video_%d", tmp, (int)_getpid());
    CreateDirectoryA(tempdir, NULL);
    _snprintf(cleanup_dir, sizeof(cleanup_dir), "%s", tempdir);

    logmsg("input : %s", input);
    logmsg("output: %s", output);

    for (i = 0; i < nfx; i++)
        write_effect_c(fx_types[i], fx_params[i]);

    if (is_img(input))
        process_image();
    else
        process_video();

    logmsg("done: %s", output);
    return 0;
}
