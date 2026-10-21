/*
 * SoulX Android Emulator - network capture (macOS host)
 *
 * Records what an instance sends and receives over WiFi (wlan0, the network apps use) into a .pcap file under
 * ~/Documents/SoulX Network Captures/<instance>/, started and stopped while the instance keeps running.
 *
 * The engine runs with its built-in WiFi (-feature -WiFiPacketStream), which gives wlan0 a QEMU network backend
 * named "virtio-wifi", and with a QMP control socket (-qemu -qmp unix:run/<name>.qmp,server,nowait).
 * network-capture-start attaches a QEMU "filter-dump" object to that backend (object-add) and network-capture-stop removes it
 * again (object-del): the engine itself writes the file, and nothing is recorded outside those two calls.
 *
 * Why not netsim, the default WiFi: one netsimd serves every running instance and can only record all of them,
 * from its own start (--pcap); the service that would switch a single radio's capture on and off is disabled in
 * the emulator's netsimd build. The engine's own -tcpdump / "network capture" only sees eth0, the cellular
 * interface, which Android leaves idle while WiFi is up.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "soulx_mac.h"

#define QMP_TIMEOUT_MS    5000
#define QMP_LINE_MAX      16384
#define QMP_OBJECT_ID     "soulxcap"
#define QMP_WIFI_BACKEND  "virtio-wifi"
#define QMP_WIFI_NIC      "virtio-wifi-device"     // listed by "info network" only while the built-in WiFi runs

#define PCAP_GLOBAL_LEN   24u
#define PCAP_RECORD_LEN   16u
#define PCAP_SANE_LEN     (1024u * 1024u)

// ---------------------------------------------------------------------------
// Where things live
// ---------------------------------------------------------------------------

// ~/Documents/SoulX Network Captures/<instance>/<timestamp>.pcap, or <$SOULX_NETWORK_CAPTURES>/<instance>/... (the folder is
// created, an existing file is never reused). User-visible, unlike $SOULX_HOME: finding the file and opening it
// in Wireshark is the whole point.
static int network_capture_dest_for(const char* instance, char* out, size_t cap) {
    if (cap) out[0] = '\0';
    char dir[PATH_MAX];
    int n;
    const char* base = getenv("SOULX_NETWORK_CAPTURES");
    if (base && base[0] == '/') {
        n = snprintf(dir, sizeof(dir), "%s/%s", base, instance);
    } else {
        struct passwd pw;
        struct passwd* res = NULL;
        char pwbuf[4096];
        const char* home = NULL;
        if (getpwuid_r(getuid(), &pw, pwbuf, sizeof(pwbuf), &res) == 0 && res && res->pw_dir && res->pw_dir[0] == '/') {
            home = res->pw_dir;
        } else {
            home = getenv("HOME");
        }
        if (!home || home[0] != '/') return -1;
        n = snprintf(dir, sizeof(dir), "%s/Documents/SoulX Network Captures/%s", home, instance);
    }
    if (n < 0 || (size_t)n >= sizeof(dir) || soulx_mkdirs(dir) != 0) return -1;
    char stamp[32];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H.%M.%S", &tm);
    for (int i = 1; i < 100; i++) {
        if (i == 1) n = snprintf(out, cap, "%s/%s.pcap", dir, stamp);
        else n = snprintf(out, cap, "%s/%s-%d.pcap", dir, stamp, i);
        if (n <= 0 || (size_t)n >= cap) return -1;
        if (access(out, F_OK) != 0) return 0;
    }
    return -1;
}

static int qmp_socket_path(const char* instance, char* out, size_t cap) {
    struct sockaddr_un probe;
    if (!soulx_name_valid(instance) || soulx_path(out, cap, "run/%s.qmp", instance) != 0) return -1;
    return strlen(out) < sizeof(probe.sun_path) ? 0 : -1;       // a unix socket path holds about 100 bytes
}

int soulx_mac_network_capture_qmp_arg(const char* instance, char* out, size_t cap) {
    char path[PATH_MAX], dir[PATH_MAX];
    if (qmp_socket_path(instance, path, sizeof(path)) != 0 || soulx_path(dir, sizeof(dir), "run") != 0) return -1;
    if (soulx_mkdirs(dir) != 0) return -1;
    (void)unlink(path);                                          // left behind by an engine that died

    // QEMU gives up starting when it cannot create the socket, and recording is optional: try it here first (a
    // $SOULX_HOME on a volume without socket files, say), so the instance still starts, only without recording.
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    memcpy(sa.sun_path, path, strlen(path) + 1);
    int probe = socket(AF_UNIX, SOCK_STREAM, 0);
    if (probe < 0) return -1;
    bool bindable = bind(probe, (struct sockaddr*)&sa, sizeof(sa)) == 0;
    close(probe);
    if (bindable) (void)unlink(path);
    if (!bindable) return -1;

    size_t n = (size_t)snprintf(out, cap, "unix:");
    for (const char* p = path; *p; p++) {                        // QEMU splits option values at commas: ",," is one
        if (n + 3 >= cap) return -1;
        if (*p == ',') out[n++] = ',';
        out[n++] = *p;
    }
    int tail = snprintf(out + n, cap - n, ",server,nowait");
    return (tail > 0 && (size_t)tail < cap - n) ? 0 : -1;
}

// ---------------------------------------------------------------------------
// QMP: line-oriented JSON over a unix socket. One client is served at a time, so every call connects, does its
// few commands and hangs up.
// ---------------------------------------------------------------------------

typedef struct {
    int fd;
    size_t len;
    char buf[QMP_LINE_MAX];
    char reply[QMP_LINE_MAX];
} Qmp;

static bool qmp_write_all(int fd, const char* s) {
    size_t len = strlen(s), off = 0;
    while (off < len) {
        ssize_t w = write(fd, s + off, len - off);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        off += (size_t)w;
    }
    return true;
}

static bool qmp_read_line(Qmp* q, char* line, size_t cap, double deadline_ms) {
    for (;;) {
        char* nl = memchr(q->buf, '\n', q->len);
        if (nl) {
            size_t n = (size_t)(nl - q->buf);
            size_t keep = n < cap - 1 ? n : cap - 1;
            memcpy(line, q->buf, keep);
            while (keep && line[keep - 1] == '\r') keep--;
            line[keep] = '\0';
            q->len -= n + 1;
            memmove(q->buf, nl + 1, q->len);
            return true;
        }
        if (q->len == sizeof(q->buf)) return false;              // longer than anything QMP says here
        double left = deadline_ms - soulx_now_ms();
        if (left <= 0) return false;
        struct pollfd pfd = { .fd = q->fd, .events = POLLIN, .revents = 0 };
        int r = poll(&pfd, 1, (int)left + 1);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        ssize_t got = read(q->fd, q->buf + q->len, sizeof(q->buf) - q->len);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return false;
        q->len += (size_t)got;
    }
}

// Runs one command. 0: it worked ({"return": ...} is in q->reply); 1: QEMU refused it ({"error": ...});
// -1: no usable answer. Asynchronous {"event": ...} lines in between are skipped.
static int qmp_run(Qmp* q, const char* command) {
    if (!qmp_write_all(q->fd, command) || !qmp_write_all(q->fd, "\r\n")) return -1;
    double deadline = soulx_now_ms() + QMP_TIMEOUT_MS;
    while (qmp_read_line(q, q->reply, sizeof(q->reply), deadline)) {
        if (!strncmp(q->reply, "{\"return\"", 9)) return 0;
        if (!strncmp(q->reply, "{\"error\"", 8)) return 1;
    }
    return -1;
}

static void qmp_error_text(const Qmp* q, char* out, size_t cap) {
    const char* p = strstr(q->reply, "\"desc\": \"");
    size_t n = 0;
    if (p) {
        for (p += 9; *p && *p != '"' && n + 1 < cap; p++) {
            if (*p == '\\' && p[1]) p++;
            out[n++] = *p;
        }
    }
    out[n] = '\0';
    if (!n) snprintf(out, cap, "no details");
}

static void qmp_close(Qmp* q) {
    if (q->fd >= 0) close(q->fd);
    q->fd = -1;
}

// Connects to the instance's QMP socket and gets through the capabilities handshake; logs why it could not.
static int qmp_open(Qmp* q, const char* instance) {
    q->fd = -1;
    q->len = 0;
    char path[PATH_MAX];
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    if (qmp_socket_path(instance, path, sizeof(path)) != 0) {
        soulx_log(SOULX_LOG_ERROR, "no capture channel path for '%s'", instance);
        return -1;
    }
    sa.sun_family = AF_UNIX;
    memcpy(sa.sun_path, path, strlen(path) + 1);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) != 0) {
        close(fd);
        soulx_log(SOULX_LOG_ERROR, "'%s' has no capture channel (it was started by an older SoulX); "
                 "restart it first: soulx restart %s", instance, instance);
        return -1;
    }
    q->fd = fd;
    double deadline = soulx_now_ms() + QMP_TIMEOUT_MS;
    if (!qmp_read_line(q, q->reply, sizeof(q->reply), deadline) || strncmp(q->reply, "{\"QMP\"", 6) != 0 ||
        qmp_run(q, "{\"execute\":\"qmp_capabilities\"}") != 0) {
        qmp_close(q);
        soulx_log(SOULX_LOG_ERROR, "the capture channel of '%s' does not answer (is another capture command "
                 "still running?)", instance);
        return -1;
    }
    return 0;
}

static bool json_quote(const char* s, char* out, size_t cap) {
    size_t n = 0;
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') {
            if (n + 2 >= cap) return false;
            out[n++] = '\\';
            out[n++] = (char)ch;
        } else if (ch < 0x20) {
            if (n + 6 >= cap) return false;
            n += (size_t)snprintf(out + n, cap - n, "\\u%04x", ch);
        } else {
            if (n + 1 >= cap) return false;
            out[n++] = (char)ch;
        }
    }
    if (n >= cap) return false;
    out[n] = '\0';
    return true;
}

// ---------------------------------------------------------------------------
// Start / stop
// ---------------------------------------------------------------------------

// Packets in a finished capture (filter-dump writes the host's byte order); -1 when it is not a pcap file.
static long pcap_packet_count(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    struct stat sb;
    unsigned char hdr[PCAP_GLOBAL_LEN];
    if (fstat(fileno(f), &sb) != 0 || fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        fclose(f);
        return -1;
    }
    bool little = hdr[0] == 0xd4 && hdr[1] == 0xc3 && hdr[2] == 0xb2 && hdr[3] == 0xa1;
    bool big = hdr[0] == 0xa1 && hdr[1] == 0xb2 && hdr[2] == 0xc3 && hdr[3] == 0xd4;
    if (!little && !big) {
        fclose(f);
        return -1;
    }
    long count = 0;
    off_t pos = PCAP_GLOBAL_LEN;
    unsigned char rec[PCAP_RECORD_LEN];
    while (fread(rec, 1, sizeof(rec), f) == sizeof(rec)) {
        uint32_t incl = little ? ((uint32_t)rec[8] | (uint32_t)rec[9] << 8 | (uint32_t)rec[10] << 16 |
                                  (uint32_t)rec[11] << 24)
                               : ((uint32_t)rec[11] | (uint32_t)rec[10] << 8 | (uint32_t)rec[9] << 16 |
                                  (uint32_t)rec[8] << 24);
        if (incl > PCAP_SANE_LEN || pos + (off_t)PCAP_RECORD_LEN + (off_t)incl > sb.st_size) break;
        if (fseeko(f, (off_t)incl, SEEK_CUR) != 0) break;
        pos += (off_t)PCAP_RECORD_LEN + (off_t)incl;
        count++;
    }
    fclose(f);
    return count;
}

// Attaches the dump to wlan0's backend: 0 recording, -1 not (logged).
static int network_capture_attach(Qmp* q, const char* name, const char* dest) {
    // The engine's netsim WiFi (a feature override on a diagnostic run) has the backend but no card on it: the
    // file would stay empty, so say so instead of recording nothing.
    if (qmp_run(q, "{\"execute\":\"human-monitor-command\",\"arguments\":{\"command-line\":\"info network\"}}") == 0 &&
        !strstr(q->reply, QMP_WIFI_NIC)) {
        soulx_log(SOULX_LOG_ERROR, "'%s' is not using the engine's built-in WiFi, so its WiFi traffic cannot be "
                 "recorded", name);
        return -1;
    }
    char quoted[2 * PATH_MAX], command[2 * PATH_MAX + 256];
    if (!json_quote(dest, quoted, sizeof(quoted))) return -1;
    int n = snprintf(command, sizeof(command),
                     "{\"execute\":\"object-add\",\"arguments\":{\"qom-type\":\"filter-dump\",\"id\":\"" QMP_OBJECT_ID
                     "\",\"props\":{\"netdev\":\"" QMP_WIFI_BACKEND "\",\"file\":\"%s\"}}}", quoted);
    if (n < 0 || (size_t)n >= sizeof(command)) return -1;
    int rc = qmp_run(q, command);
    if (rc != 0) {
        char why[256];
        if (rc == 1) qmp_error_text(q, why, sizeof(why));
        else snprintf(why, sizeof(why), "no answer");
        soulx_log(SOULX_LOG_ERROR, "could not start recording '%s': %s", name, why);
        return -1;
    }
    return 0;
}

int soulx_mac_network_capture_start(const char* instance, char* dest_out, size_t cap) {
    if (dest_out && cap) dest_out[0] = '\0';
    SoulXInstanceConf c;
    SoulXRunState st;
    char serial[64];
    if (soulx_mac_require_running(instance, &c, &st, serial, sizeof(serial)) != 0) return -1;
    if (st.network_capture_path[0]) {
        soulx_log(SOULX_LOG_ERROR, "'%s' is already recording network traffic (%s)", c.name, st.network_capture_path);
        return -1;
    }
    char dest[1024];
    if (network_capture_dest_for(c.name, dest, sizeof(dest)) != 0) {
        soulx_log(SOULX_LOG_ERROR, "could not set up a capture file for '%s'", c.name);
        return -1;
    }

    Qmp* q = calloc(1, sizeof(*q));
    if (!q) return -1;
    int rc = -1;
    if (qmp_open(q, c.name) == 0 && network_capture_attach(q, c.name, dest) == 0) {
        snprintf(st.network_capture_path, sizeof(st.network_capture_path), "%s", dest);
        if (soulx_state_save(c.name, &st) == 0) {
            if (dest_out) snprintf(dest_out, cap, "%s", dest);
            rc = 0;
        } else {
            (void)qmp_run(q, "{\"execute\":\"object-del\",\"arguments\":{\"id\":\"" QMP_OBJECT_ID "\"}}");
            (void)unlink(dest);
            soulx_log(SOULX_LOG_ERROR, "could not save the capture state of '%s'", c.name);
        }
    }
    qmp_close(q);
    free(q);
    return rc;
}

int soulx_mac_network_capture_stop(const char* instance, char* dest_out, size_t cap) {
    if (dest_out && cap) dest_out[0] = '\0';
    SoulXInstanceConf c;
    SoulXRunState st;
    char serial[64];
    if (soulx_mac_require_running(instance, &c, &st, serial, sizeof(serial)) != 0) return -1;
    if (!st.network_capture_path[0]) {
        soulx_log(SOULX_LOG_ERROR, "'%s' is not recording network traffic", c.name);
        return -1;
    }

    Qmp* q = calloc(1, sizeof(*q));
    if (!q) return -1;
    int rc = -1;
    if (qmp_open(q, c.name) == 0) {
        int dr = qmp_run(q, "{\"execute\":\"object-del\",\"arguments\":{\"id\":\"" QMP_OBJECT_ID "\"}}");
        if (dr == -1) {
            soulx_log(SOULX_LOG_ERROR, "could not stop the recording of '%s': no answer", c.name);
        } else {
            if (dr == 1) {                                       // gone already (the engine started over): forget it
                char why[256];
                qmp_error_text(q, why, sizeof(why));
                soulx_log(SOULX_LOG_WARN, "'%s' was no longer recording (%s)", c.name, why);
            }
            char dest[1024];
            snprintf(dest, sizeof(dest), "%s", st.network_capture_path);
            st.network_capture_path[0] = '\0';
            if (soulx_state_save(c.name, &st) != 0) {
                soulx_log(SOULX_LOG_DEBUG, "could not clear the capture state of '%s'", c.name);
            }
            long packets = pcap_packet_count(dest);
            if (packets >= 0) soulx_log(SOULX_LOG_INFO, "'%s': saved %ld packet(s) to %s", c.name, packets, dest);
            else soulx_log(SOULX_LOG_INFO, "'%s': saved the capture to %s", c.name, dest);
            if (dest_out) snprintf(dest_out, cap, "%s", dest);
            rc = 0;
        }
    }
    qmp_close(q);
    free(q);
    return rc;
}
