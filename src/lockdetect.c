/*
 * lockdetect - find which process(es) hold a file or directory open on Windows.
 *
 * Uses the Windows Restart Manager API (Rstrtmgr.dll), which is the supported
 * way to discover the processes currently using a given file or directory.
 *
 * Build:  cmake -S . -B build -G "MinGW Makefiles" && cmake --build build
 * Run:    lockdetect [options] <path> [path ...]
 *
 * License: BSD-2-Clause
 *
 * AI disclosure: this source file was generated with AI assistance and
 * reviewed by the author (Mantis36558).
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <restartmanager.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define LD_VERSION L"1.0.0"

/* ------------------------------------------------------------------ */
/* Output formats                                                      */
/* ------------------------------------------------------------------ */

typedef enum {
    FMT_DEFAULT,   /* grouped by path: "PID name" lines            */
    FMT_BRIEF,     /* one "PID<TAB>name" line per unique process    */
    FMT_VERBOSE,   /* full exe path, type, service, session, ...    */
    FMT_JSON,      /* machine readable JSON                         */
    FMT_CSV        /* machine readable CSV (header row included)    */
} fmt_t;

/* ------------------------------------------------------------------ */
/* Per-file result                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    wchar_t          *path;
    RM_PROCESS_INFO  *procs;
    UINT              nprocs;
    DWORD             error;      /* 0 = success                      */
    BOOL              not_found;
} file_result;

typedef struct {
    fmt_t          fmt;
    BOOL           show_help;
    BOOL           show_version;
    int            nfiles;
    file_result   *files;
} config;

/* ------------------------------------------------------------------ */
/* Growable wide-string buffer (single place that accumulates output)  */
/* ------------------------------------------------------------------ */

typedef struct {
    wchar_t *data;
    size_t   len;
    size_t   cap;
} wbuf;

static void wb_init(wbuf *b) {
    b->data = NULL;
    b->len  = 0;
    b->cap  = 0;
}

static void wb_free(wbuf *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static void wb_reserve(wbuf *b, size_t cap) {
    if (cap <= b->cap)
        return;
    size_t nc = b->cap ? b->cap : 256;
    while (nc < cap)
        nc *= 2;
    wchar_t *nd = (wchar_t *)realloc(b->data, nc * sizeof(wchar_t));
    if (!nd)
        return; /* out of memory: keep previous buffer */
    b->data = nd;
    b->cap  = nc;
}

static void wb_cat(wbuf *b, const wchar_t *s) {
    size_t n = wcslen(s);
    wb_reserve(b, b->len + n + 1);
    memcpy(b->data + b->len, s, (n + 1) * sizeof(wchar_t));
    b->len += n;
}

static void wb_printf(wbuf *b, const wchar_t *fmt, ...) {
    wchar_t tmp[16384];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(tmp, 16384, fmt, ap);
    va_end(ap);
    tmp[16383] = L'\0';
    wb_cat(b, tmp);
}

/* JSON string escaping, appended directly to the buffer. */
static void wb_json_escape(wbuf *b, const wchar_t *s) {
    for (; *s; s++) {
        wchar_t c = *s;
        switch (c) {
        case L'\\': wb_cat(b, L"\\\\"); break;
        case L'"':  wb_cat(b, L"\\\""); break;
        case L'\b': wb_cat(b, L"\\b");  break;
        case L'\f': wb_cat(b, L"\\f");  break;
        case L'\n': wb_cat(b, L"\\n");  break;
        case L'\r': wb_cat(b, L"\\r");  break;
        case L'\t': wb_cat(b, L"\\t");  break;
        default:
            if (c < 0x20) {
                wchar_t esc[8];
                _snwprintf(esc, 8, L"\\u%04x", (unsigned)c);
                wb_cat(b, esc);
            } else {
                wchar_t one[2] = { c, L'\0' };
                wb_cat(b, one);
            }
            break;
        }
    }
}

static void wb_json_string(wbuf *b, const wchar_t *s) {
    wb_cat(b, L"\"");
    wb_json_escape(b, s);
    wb_cat(b, L"\"");
}

/* CSV field quoting (RFC 4180 style). */
static void wb_csv_field(wbuf *b, const wchar_t *s) {
    wb_cat(b, L"\"");
    for (; *s; s++) {
        if (*s == L'"') {
            wb_cat(b, L"\"\"");
        } else {
            wchar_t one[2] = { *s, L'\0' };
            wb_cat(b, one);
        }
    }
    wb_cat(b, L"\"");
}

/* ------------------------------------------------------------------ */
/* Output sink: write wide text correctly to console or redirect        */
/* ------------------------------------------------------------------ */

static void fputs_utf8(FILE *stream, const wchar_t *s) {
    while (*s) {
        unsigned int c = (unsigned int)*s++;
        if (c >= 0xD800 && c <= 0xDBFF && *s >= 0xDC00 && *s <= 0xDFFF)
            c = 0x10000 + ((c - 0xD800) << 10) + ((unsigned int)*s++ - 0xDC00);

        if (c < 0x80) {
            fputc((int)c, stream);
        } else if (c < 0x800) {
            fputc(0xC0 | (c >> 6), stream);
            fputc(0x80 | (c & 0x3F), stream);
        } else if (c < 0x10000) {
            fputc(0xE0 | (c >> 12), stream);
            fputc(0x80 | ((c >> 6) & 0x3F), stream);
            fputc(0x80 | (c & 0x3F), stream);
        } else {
            fputc(0xF0 | (c >> 18), stream);
            fputc(0x80 | ((c >> 12) & 0x3F), stream);
            fputc(0x80 | ((c >> 6) & 0x3F), stream);
            fputc(0x80 | (c & 0x3F), stream);
        }
    }
}

/* Write a wide string: console -> WriteConsoleW, otherwise UTF-8 bytes. */
static void emit(FILE *stream, const wchar_t *s) {
    HANDLE h = (stream == stdout) ? GetStdHandle(STD_OUTPUT_HANDLE)
                                  : GetStdHandle(STD_ERROR_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode)) {
        const wchar_t *p = s;
        while (*p) {
            const wchar_t *nl = wcschr(p, L'\n');
            if (nl) {
                WriteConsoleW(h, p, (DWORD)(nl - p), NULL, NULL);
                WriteConsoleW(h, L"\r\n", 2, NULL, NULL);
                p = nl + 1;
            } else {
                WriteConsoleW(h, p, (DWORD)wcslen(p), NULL, NULL);
                break;
            }
        }
    } else {
        fputs_utf8(stream, s);
    }
}

static void errout(const wchar_t *fmt, ...) {
    wchar_t tmp[4096];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(tmp, 4096, fmt, ap);
    va_end(ap);
    tmp[4095] = L'\0';
    emit(stderr, tmp);
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static wchar_t *xwcsdup(const wchar_t *s) {
    size_t n = wcslen(s) + 1;
    wchar_t *d = (wchar_t *)malloc(n * sizeof(wchar_t));
    if (d)
        memcpy(d, s, n * sizeof(wchar_t));
    return d;
}

static const wchar_t *base_name(const wchar_t *p) {
    const wchar_t *s = p;
    for (const wchar_t *q = p; *q; q++)
        if (*q == L'\\' || *q == L'/')
            s = q + 1;
    return s;
}

static const wchar_t *proc_name(const RM_PROCESS_INFO *p) {
    if (!p->strAppName[0])
        return L"(unknown)";
    return base_name(p->strAppName);
}

static const wchar_t *app_type_name(RM_APP_TYPE t) {
    switch (t) {
    case RmMainWindow: return L"main window";
    case RmOtherWindow: return L"window";
    case RmService:     return L"service";
    case RmExplorer:    return L"explorer";
    case RmConsole:     return L"console";
    case RmCritical:    return L"critical";
    default:            return L"unknown";
    }
}

static wchar_t *win_error(DWORD err) {
    wchar_t *buf = NULL;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, err, 0, (LPWSTR)&buf, 0, NULL);
    return buf; /* NULL on failure */
}

/* ------------------------------------------------------------------ */
/* Restart Manager query                                               */
/* ------------------------------------------------------------------ */

static void query_one(const wchar_t *path, file_result *fr) {
    memset(fr, 0, sizeof(*fr));
    fr->path = xwcsdup(path);

    DWORD attr = GetFileAttributesW(path);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) {
            fr->not_found = TRUE;
            fr->error     = e;
            return;
        }
    }

    WCHAR key[CCH_RM_SESSION_KEY + 1];
    DWORD session = 0;
    DWORD err = RmStartSession(&session, 0, key);
    if (err != ERROR_SUCCESS) {
        fr->error = err;
        return;
    }

    LPCWSTR resources[1];
    resources[0] = path;
    err = RmRegisterResources(session, 1, resources, 0, NULL, 0, NULL);
    if (err != ERROR_SUCCESS) {
        fr->error = err;
        RmEndSession(session);
        return;
    }

    UINT needed = 0, count = 0;
    DWORD reason = 0;
    err = RmGetList(session, &needed, &count, NULL, &reason);

    if (err == ERROR_MORE_DATA && needed > 0) {
        RM_PROCESS_INFO *buf = (RM_PROCESS_INFO *)calloc(needed, sizeof(RM_PROCESS_INFO));
        if (!buf) {
            fr->error = ERROR_NOT_ENOUGH_MEMORY;
        } else {
            count = needed;
            err = RmGetList(session, &needed, &count, buf, &reason);
            if (err == ERROR_SUCCESS) {
                fr->procs  = buf;
                fr->nprocs = count;
            } else {
                free(buf);
                fr->error = err;
            }
        }
    } else if (err == ERROR_SUCCESS) {
        fr->nprocs = 0; /* nobody holds it */
    } else {
        fr->error = err;
    }

    RmEndSession(session);
}

/* ------------------------------------------------------------------ */
/* Renderers                                                           */
/* ------------------------------------------------------------------ */

static void render_default(const config *cfg, wbuf *b) {
    for (int i = 0; i < cfg->nfiles; i++) {
        const file_result *fr = &cfg->files[i];
        wb_printf(b, L"%ls\n", fr->path);
        if (fr->not_found) {
            wb_cat(b, L"  (file not found)\n");
            continue;
        }
        if (fr->error) {
            wb_printf(b, L"  (error %lu)\n", (unsigned long)fr->error);
            continue;
        }
        if (fr->nprocs == 0) {
            wb_cat(b, L"  (not locked)\n");
            continue;
        }
        for (UINT k = 0; k < fr->nprocs; k++) {
            const RM_PROCESS_INFO *p = &fr->procs[k];
            if (p->ApplicationType == RmService && p->strServiceShortName[0])
                wb_printf(b, L"  PID %lu  %ls  [service: %ls]\n",
                          (unsigned long)p->Process.dwProcessId,
                          proc_name(p), p->strServiceShortName);
            else
                wb_printf(b, L"  PID %lu  %ls\n",
                          (unsigned long)p->Process.dwProcessId,
                          proc_name(p));
        }
    }
}

static void render_verbose(const config *cfg, wbuf *b) {
    for (int i = 0; i < cfg->nfiles; i++) {
        const file_result *fr = &cfg->files[i];
        wb_printf(b, L"File: %ls\n", fr->path);
        if (fr->not_found) {
            wb_cat(b, L"  (file not found)\n");
            continue;
        }
        if (fr->error) {
            wb_printf(b, L"  (error %lu)\n", (unsigned long)fr->error);
            continue;
        }
        if (fr->nprocs == 0) {
            wb_cat(b, L"  (not locked)\n");
            continue;
        }
        for (UINT k = 0; k < fr->nprocs; k++) {
            const RM_PROCESS_INFO *p = &fr->procs[k];
            wb_printf(b, L"  PID %lu  %ls\n", (unsigned long)p->Process.dwProcessId, proc_name(p));
            wb_printf(b, L"      executable : %ls\n", p->strAppName);
            wb_printf(b, L"      type       : %ls\n", app_type_name(p->ApplicationType));
            wb_printf(b, L"      service    : %ls\n",
                      p->strServiceShortName[0] ? p->strServiceShortName : L"-");
            wb_printf(b, L"      session    : %lu\n", (unsigned long)p->TSSessionId);
            wb_printf(b, L"      restartable: %ls\n", p->bRestartable ? L"yes" : L"no");
        }
    }
}

typedef struct {
    DWORD    pid;
    wchar_t *name;
} proc_entry;

static void add_unique(proc_entry **list, size_t *n, DWORD pid, const wchar_t *name) {
    for (size_t i = 0; i < *n; i++)
        if ((*list)[i].pid == pid && !wcscmp((*list)[i].name, name))
            return;
    proc_entry *nd = (proc_entry *)realloc(*list, (*n + 1) * sizeof(proc_entry));
    if (!nd)
        return;
    nd[*n].pid  = pid;
    nd[*n].name = xwcsdup(name);
    *list = nd;
    (*n)++;
}

static void render_brief(const config *cfg, wbuf *b) {
    proc_entry *list = NULL;
    size_t n = 0;
    for (int i = 0; i < cfg->nfiles; i++) {
        const file_result *fr = &cfg->files[i];
        for (UINT k = 0; k < fr->nprocs; k++)
            add_unique(&list, &n, fr->procs[k].Process.dwProcessId, proc_name(&fr->procs[k]));
    }
    for (size_t i = 0; i < n; i++)
        wb_printf(b, L"%lu\t%ls\n", (unsigned long)list[i].pid, list[i].name);
    for (size_t i = 0; i < n; i++)
        free(list[i].name);
    free(list);
}

static void render_json(const config *cfg, wbuf *b) {
    wb_printf(b, L"{\n  \"version\": \"%ls\",\n  \"files\": [\n", LD_VERSION);
    for (int i = 0; i < cfg->nfiles; i++) {
        const file_result *fr = &cfg->files[i];
        wb_cat(b, L"    {\n");
        wb_cat(b, L"      \"path\": ");
        wb_json_string(b, fr->path);
        wb_cat(b, L",\n");

        if (fr->not_found) {
            wb_cat(b, L"      \"error\": \"file not found\",\n");
            wb_cat(b, L"      \"processes\": []\n");
        } else if (fr->error) {
            wb_printf(b, L"      \"error\": \"error %lu\",\n", (unsigned long)fr->error);
            wb_cat(b, L"      \"processes\": []\n");
        } else {
            wb_cat(b, L"      \"processes\": [\n");
            for (UINT k = 0; k < fr->nprocs; k++) {
                const RM_PROCESS_INFO *p = &fr->procs[k];
                wb_cat(b, L"        {\n");
                wb_printf(b, L"          \"pid\": %lu,\n", (unsigned long)p->Process.dwProcessId);
                wb_cat(b, L"          \"name\": ");
                wb_json_string(b, proc_name(p));
                wb_cat(b, L",\n");
                wb_cat(b, L"          \"executable\": ");
                wb_json_string(b, p->strAppName);
                wb_cat(b, L",\n");
                wb_cat(b, L"          \"service\": ");
                wb_json_string(b, p->strServiceShortName);
                wb_cat(b, L",\n");
                wb_cat(b, L"          \"type\": ");
                wb_json_string(b, app_type_name(p->ApplicationType));
                wb_cat(b, L",\n");
                wb_printf(b, L"          \"session\": %lu,\n", (unsigned long)p->TSSessionId);
                wb_printf(b, L"          \"restartable\": %ls\n", p->bRestartable ? L"true" : L"false");
                wb_cat(b, L"        }");
                wb_cat(b, (k + 1 < fr->nprocs) ? L",\n" : L"\n");
            }
            wb_cat(b, L"      ]\n");
        }
        wb_cat(b, L"    }");
        wb_cat(b, (i + 1 < cfg->nfiles) ? L",\n" : L"\n");
    }
    wb_cat(b, L"  ]\n}\n");
}

static void render_csv(const config *cfg, wbuf *b) {
    wb_cat(b, L"path,pid,name,executable,service,type,session,restartable\n");
    for (int i = 0; i < cfg->nfiles; i++) {
        const file_result *fr = &cfg->files[i];
        if (fr->error)
            continue; /* already reported on stderr */
        if (fr->nprocs == 0) {
            wb_csv_field(b, fr->path);
            wb_cat(b, L",,,,,,,\n");
            continue;
        }
        for (UINT k = 0; k < fr->nprocs; k++) {
            const RM_PROCESS_INFO *p = &fr->procs[k];
            wb_csv_field(b, fr->path);
            wb_printf(b, L",%lu", (unsigned long)p->Process.dwProcessId);
            wb_cat(b, L",");
            wb_csv_field(b, proc_name(p));
            wb_cat(b, L",");
            wb_csv_field(b, p->strAppName);
            wb_cat(b, L",");
            wb_csv_field(b, p->strServiceShortName);
            wb_cat(b, L",");
            wb_csv_field(b, app_type_name(p->ApplicationType));
            wb_printf(b, L",%lu", (unsigned long)p->TSSessionId);
            wb_printf(b, L",%ls\n", p->bRestartable ? L"yes" : L"no");
        }
    }
}

/* ------------------------------------------------------------------ */
/* Usage / version                                                     */
/* ------------------------------------------------------------------ */

static void print_help(wbuf *b) {
    wb_cat(b,
        L"lockdetect v" LD_VERSION L" - find which process is holding a file or directory\n"
        L"\n"
        L"Usage:\n"
        L"  lockdetect [options] <path> [path ...]\n"
        L"\n"
        L"Options:\n"
        L"  -h, --help      Show this help and exit\n"
        L"  -v, --version   Show version and exit\n"
        L"  -b, --brief     Brief output: one \"PID<TAB>name\" line per unique process\n"
        L"  -V, --verbose   Verbose output: full executable, type, service, session\n"
        L"  -j, --json      JSON output\n"
        L"  -c, --csv       CSV output (header row included)\n"
        L"  --              Treat all remaining arguments as paths\n"
        L"\n"
        L"With no format flag the output is grouped by path:\n"
        L"    C:\\dir\\file.txt\n"
        L"      PID 1234  notepad.exe\n"
        L"\n"
        L"Notes:\n"
        L"  * Detection uses the Windows Restart Manager (Rstrtmgr.dll); it reports\n"
        L"    every process currently using the file or directory, not only exclusive\n"
        L"    locks. Some holders (e.g. kernel components) may not be reported.\n"
        L"  * Requires Windows Vista or later.\n"
        L"  * -v is reserved for --version; use -V/--verbose for detailed output.\n"
        L"\n"
        L"Examples:\n"
        L"  lockdetect C:\\some\\file.txt\n"
        L"  lockdetect -b C:\\some\\file.txt C:\\other\n"
        L"  lockdetect --json C:\\some\\file.txt\n"
        L"  lockdetect -V C:\\some\\directory\n");
}

/* ------------------------------------------------------------------ */
/* Argument parsing                                                    */
/* ------------------------------------------------------------------ */

static int parse_args(int argc, wchar_t **argv, config *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->fmt = FMT_DEFAULT;

    int npaths = 0;
    BOOL end_opts = FALSE;

    for (int i = 1; i < argc; i++) {
        const wchar_t *a = argv[i];
        if (!end_opts && a[0] == L'-' && a[1] != L'\0') {
            if (!wcscmp(a, L"--")) {
                end_opts = TRUE;
            } else if (!wcscmp(a, L"-h") || !wcscmp(a, L"--help")) {
                cfg->show_help = TRUE;
            } else if (!wcscmp(a, L"-v") || !wcscmp(a, L"--version")) {
                cfg->show_version = TRUE;
            } else if (!wcscmp(a, L"-b") || !wcscmp(a, L"--brief")) {
                cfg->fmt = FMT_BRIEF;
            } else if (!wcscmp(a, L"-V") || !wcscmp(a, L"--verbose")) {
                cfg->fmt = FMT_VERBOSE;
            } else if (!wcscmp(a, L"-j") || !wcscmp(a, L"--json")) {
                cfg->fmt = FMT_JSON;
            } else if (!wcscmp(a, L"-c") || !wcscmp(a, L"--csv")) {
                cfg->fmt = FMT_CSV;
            } else {
                errout(L"lockdetect: unknown option: %ls\n", a);
                return 1;
            }
        } else {
            npaths++;
        }
    }

    if (cfg->show_help || cfg->show_version)
        return 0;

    if (npaths == 0) {
        errout(L"lockdetect: no path specified (see -h for help)\n");
        return 1;
    }

    cfg->files = (file_result *)calloc((size_t)npaths, sizeof(file_result));
    if (!cfg->files)
        return 1;

    end_opts = FALSE;
    int idx = 0;
    for (int i = 1; i < argc; i++) {
        const wchar_t *a = argv[i];
        if (!end_opts && !wcscmp(a, L"--")) {
            end_opts = TRUE;
            continue;
        }
        if (!end_opts && a[0] == L'-' && a[1] != L'\0')
            continue;
        query_one(a, &cfg->files[idx++]);
    }
    cfg->nfiles = idx;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Cleanup                                                             */
/* ------------------------------------------------------------------ */

static void free_files(config *cfg) {
    for (int i = 0; i < cfg->nfiles; i++) {
        free(cfg->files[i].path);
        free(cfg->files[i].procs);
    }
    free(cfg->files);
    cfg->files = NULL;
    cfg->nfiles = 0;
}

static void report_errors(const config *cfg) {
    for (int i = 0; i < cfg->nfiles; i++) {
        const file_result *fr = &cfg->files[i];
        if (fr->not_found) {
            errout(L"lockdetect: %ls: file not found\n", fr->path);
        } else if (fr->error) {
            wchar_t *msg = win_error(fr->error);
            errout(L"lockdetect: %ls: error %lu: %ls\n", fr->path,
                   (unsigned long)fr->error, msg ? msg : L"(no description)");
            if (msg)
                LocalFree(msg);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int main(void) {
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
        return 1;

    config cfg;
    int rc = parse_args(argc, argv, &cfg);
    if (rc != 0) {
        LocalFree(argv);
        return 2;
    }

    wbuf out;
    wb_init(&out);

    if (cfg.show_help) {
        print_help(&out);
    } else if (cfg.show_version) {
        wb_printf(&out, L"lockdetect %ls\n", LD_VERSION);
    } else {
        int had_error = 0;
        for (int i = 0; i < cfg.nfiles; i++)
            if (cfg.files[i].error != 0)
                had_error = 1;

        /* JSON embeds errors inline; other formats report them on stderr. */
        if (cfg.fmt != FMT_JSON)
            report_errors(&cfg);

        switch (cfg.fmt) {
        case FMT_BRIEF:   render_brief(&cfg, &out);   break;
        case FMT_VERBOSE: render_verbose(&cfg, &out); break;
        case FMT_JSON:    render_json(&cfg, &out);    break;
        case FMT_CSV:     render_csv(&cfg, &out);     break;
        default:          render_default(&cfg, &out); break;
        }

        if (had_error)
            rc = 1;
    }

    emit(stdout, out.data);

    wb_free(&out);
    free_files(&cfg);
    LocalFree(argv);
    return rc;
}
