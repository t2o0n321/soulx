/*
 * SoulX Android Emulator - macOS logging
 *
 * soulx_log() prints "soulx: <msg>" / "soulx: warning: <msg>" / "soulx: error: <msg>" to stderr and
 * also hands every emitted message (exactly once) to soulx_mac_log_dispatch(), which lives in
 * engine.c and forwards it to a LogCallback registered with the public soulx_set_log_callback().
 * DEBUG messages are dropped unless verbose mode is on (-v / SOULX_VERBOSE=1).
 *
 * soulx_status_line() draws a transient progress line ("\r<msg>\x1b[K") only when stderr is a
 * terminal; any regular log message or soulx_status_done() clears it first.
 *
 * Messages often carry text from untrusted sources (archive entry names, manifest.json fields, tool
 * output), so every message is made terminal-safe before it is printed or dispatched: control
 * characters (C0 except tab and newline, DEL, C1) and bytes that are not valid UTF-8 become '?', and
 * each embedded newline is followed by an indent, so no message can move the cursor, change the
 * window title or forge a separate "soulx: ..." line.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "soulx_mac.h"

#define LOG_STACK_BUF   1024
#define STATUS_BUF      1024

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static bool g_verbose = false;          // set by soulx_set_verbose (-v / --verbose)
static int  g_env_verbose = -1;         // SOULX_VERBOSE, read lazily (-1 = not read yet)
static bool g_status_active = false;    // a transient status line is on screen (guarded by flockfile)

static bool env_verbose(void) {
    if (g_env_verbose < 0) {
        const char* v = getenv("SOULX_VERBOSE");
        g_env_verbose = (v && v[0] && strcmp(v, "0") != 0) ? 1 : 0;
    }
    return g_env_verbose == 1;
}

void soulx_set_verbose(bool verbose) {
    g_verbose = verbose;
}

bool soulx_is_verbose(void) {
    return g_verbose || env_verbose();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static const char* level_prefix(int level) {
    if (level >= SOULX_LOG_ERROR) return "error: ";
    if (level == SOULX_LOG_WARN) return "warning: ";
    if (level <= SOULX_LOG_DEBUG) return "debug: ";
    return "";
}

// Caller holds flockfile(stderr).
static void clear_status_locked(void) {
    if (g_status_active) {
        fputs("\r\x1b[K", stderr);
        g_status_active = false;
    }
}

// Length of the valid UTF-8 sequence at s (1-4), 0 when s does not start one. Overlong forms,
// surrogates and code points above U+10FFFF are invalid.
static size_t utf8_seq_len(const unsigned char* s) {
    unsigned char c = s[0];
    if (c < 0x80) return 1;
    if (c >= 0xC2 && c <= 0xDF) return (s[1] & 0xC0) == 0x80 ? 2 : 0;
    if (c >= 0xE0 && c <= 0xEF) {
        unsigned char lo = (c == 0xE0) ? 0xA0 : 0x80;
        unsigned char hi = (c == 0xED) ? 0x9F : 0xBF;
        return (s[1] >= lo && s[1] <= hi && (s[2] & 0xC0) == 0x80) ? 3 : 0;
    }
    if (c >= 0xF0 && c <= 0xF4) {
        unsigned char lo = (c == 0xF0) ? 0x90 : 0x80;
        unsigned char hi = (c == 0xF4) ? 0x8F : 0xBF;
        return (s[1] >= lo && s[1] <= hi && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) ? 4 : 0;
    }
    return 0;
}

#define LOG_INDENT "  "

// Terminal-safe copy of in (see the header comment). multiline: a newline is kept and followed by
// LOG_INDENT; otherwise newlines and tabs become spaces. out must hold 3 * strlen(in) + 1 bytes.
static void sanitize_text(const char* in, char* out, bool multiline) {
    const unsigned char* s = (const unsigned char*)in;
    size_t o = 0;
    while (*s) {
        unsigned char c = *s;
        if (c == '\n' || c == '\t') {
            if (c == '\t' && multiline) {
                out[o++] = '\t';
            } else if (c == '\n' && multiline) {
                out[o++] = '\n';
                memcpy(out + o, LOG_INDENT, sizeof(LOG_INDENT) - 1);
                o += sizeof(LOG_INDENT) - 1;
            } else {
                out[o++] = ' ';
            }
            s++;
            continue;
        }
        size_t n = utf8_seq_len(s);
        bool control = c < 0x20 || c == 0x7F || (n == 2 && c == 0xC2 && s[1] < 0xA0);   // C0, DEL, C1
        if (n == 0 || control) {
            out[o++] = '?';
            s += n ? n : 1;
            continue;
        }
        memcpy(out + o, s, n);
        o += n;
        s += n;
    }
    out[o] = '\0';
}

// Cuts s so that it shows at most max_cols code points and never ends inside a UTF-8 sequence.
static void clip_utf8(char* s, size_t max_cols) {
    size_t cols = 0;
    size_t i = 0;
    while (s[i]) {
        unsigned char c = (unsigned char)s[i];
        size_t len = 1;
        if (c >= 0xF0) len = 4;
        else if (c >= 0xE0) len = 3;
        else if (c >= 0xC0) len = 2;
        for (size_t k = 1; k < len; k++) {
            if (((unsigned char)s[i + k] & 0xC0) != 0x80) {   // truncated / invalid sequence
                s[i] = '\0';
                return;
            }
        }
        if (cols + 1 > max_cols) {
            s[i] = '\0';
            return;
        }
        cols++;
        i += len;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void soulx_log(int level, const char* fmt, ...) {
    if (!fmt) return;
    if (level <= SOULX_LOG_DEBUG && !soulx_is_verbose()) return;

    char stack_buf[LOG_STACK_BUF];
    char* heap = NULL;
    char* msg = stack_buf;

    va_list ap;
    va_list ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(stack_buf, sizeof(stack_buf), fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        return;
    }
    if ((size_t)n >= sizeof(stack_buf)) {
        heap = malloc((size_t)n + 1);
        if (heap) {
            vsnprintf(heap, (size_t)n + 1, fmt, ap2);
            msg = heap;
        }
        // on allocation failure the truncated stack copy is printed
    }
    va_end(ap2);

    size_t len = strlen(msg);
    while (len > 0 && (msg[len - 1] == '\n' || msg[len - 1] == '\r')) {
        msg[--len] = '\0';
    }

    // Terminal-safe copy (it can grow: a newline becomes newline + indent)
    char safe_stack[3 * LOG_STACK_BUF + 1];
    char* safe_heap = NULL;
    char* safe = safe_stack;
    if (len > LOG_STACK_BUF) {
        safe_heap = malloc(3 * len + 1);
        safe = safe_heap;
    }
    if (safe) {
        sanitize_text(msg, safe, true);
    } else {
        safe = msg;                     // out of memory: neutralise in place (newlines become spaces)
        for (char* p = msg; *p; p++) {
            unsigned char c = (unsigned char)*p;
            if (c < 0x20 || c == 0x7F) *p = (c == '\n' || c == '\t') ? ' ' : '?';
        }
    }

    flockfile(stderr);
    clear_status_locked();
    fprintf(stderr, "soulx: %s%s\n", level_prefix(level), safe);
    fflush(stderr);
    funlockfile(stderr);

    soulx_mac_log_dispatch(level, safe);
    free(safe_heap);
    free(heap);
}

void soulx_status_line(const char* fmt, ...) {
    if (!fmt) return;
    if (!isatty(STDERR_FILENO)) {
        // The launcher reads the output through a pipe: a plain line at most once a second, which it shows in place
        // of the previous one. Others (scripts, logs) get no progress lines.
        static struct timespec last;
        struct timespec now;
        if (!getenv("SOULX_FROM_LAUNCHER")) return;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (last.tv_sec && (now.tv_sec - last.tv_sec) * 1000 + (now.tv_nsec - last.tv_nsec) / 1000000 < 1000) return;
        last = now;
        char line[STATUS_BUF];
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(line, sizeof(line), fmt, ap);
        va_end(ap);
        if (n < 0) return;
        for (char* c = line; *c; c++) if (*c == '\n' || *c == '\r') *c = ' ';
        fprintf(stderr, "soulx: %s\n", line);
        fflush(stderr);
        return;
    }

    char raw[STATUS_BUF];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(raw, sizeof(raw), fmt, ap);
    va_end(ap);
    if (n < 0) return;

    // One terminal-safe line: newlines and tabs become spaces, control characters '?'. A UTF-8
    // sequence cut by vsnprintf's truncation is invalid and becomes '?' as well.
    char buf[3 * STATUS_BUF + 1];
    sanitize_text(raw, buf, false);

    // Keep the line shorter than the terminal: a wrapped line cannot be redrawn with '\r'.
    size_t cols = 80;
    struct winsize ws;
    if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) cols = ws.ws_col;
    clip_utf8(buf, cols > 1 ? cols - 1 : 1);

    flockfile(stderr);
    fprintf(stderr, "\r%s\x1b[K", buf);
    fflush(stderr);
    g_status_active = true;
    funlockfile(stderr);
}

void soulx_status_done(void) {
    flockfile(stderr);
    if (g_status_active) {
        clear_status_locked();
        fflush(stderr);
    }
    funlockfile(stderr);
}
