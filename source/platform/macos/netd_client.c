/*
 * netd_client.c — client side of the soulx_netd privileged helper.
 *
 * soulx_netd is a tiny LaunchDaemon (see platform/macos/netd/soulx_netd.c) that runs as root and
 * bridges an instance's Wi-Fi (wlan0) to an Apple vmnet interface for Host-only / Bridged networking.
 * SoulX itself stays unprivileged: it asks the daemon over a unix control socket to open a bridge and
 * gets back a loopback TCP port, which the emulator then uses with -wifi-socket connect=127.0.0.1:<port>.
 *
 * Installing the daemon is a one-time admin prompt (osascript "with administrator privileges").
 */

#include "soulx_mac.h"

#include <mach-o/dyld.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NETD_SOCK   "/var/run/soulx-netd.sock"
#define NETD_PLIST  "/Library/LaunchDaemons/com.soulx.netd.plist"
#define NETD_BIN    "/usr/local/libexec/soulx_netd"

// The LaunchDaemon plist, written to NETD_PLIST by the (admin-authorized) install step.
static const char k_plist[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n"
    "<dict>\n"
    "    <key>Label</key><string>com.soulx.netd</string>\n"
    "    <key>ProgramArguments</key><array>\n"
    "        <string>" NETD_BIN "</string><string>daemon</string>\n"
    "    </array>\n"
    "    <key>KeepAlive</key><true/>\n"
    "    <key>RunAtLoad</key><true/>\n"
    "    <key>ProcessType</key><string>Adaptive</string>\n"
    "    <key>StandardOutPath</key><string>/var/log/soulx-netd.log</string>\n"
    "    <key>StandardErrorPath</key><string>/var/log/soulx-netd.log</string>\n"
    "</dict>\n</plist>\n";

// Send one request line to the daemon and read its reply. Returns 0 ok.
static int netd_ctl(const char* req, char* reply, size_t cap) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un un;
    memset(&un, 0, sizeof un);
    un.sun_family = AF_UNIX;
    strncpy(un.sun_path, NETD_SOCK, sizeof un.sun_path - 1);
    if (connect(fd, (struct sockaddr*)&un, sizeof un) != 0) { close(fd); return -1; }
    size_t n = strlen(req);
    if (write(fd, req, n) != (ssize_t)n) { close(fd); return -1; }
    ssize_t r = read(fd, reply, cap - 1);
    close(fd);
    if (r <= 0) return -1;
    reply[r] = '\0';
    return 0;
}

bool soulx_netd_installed(void) {
    char reply[64];
    if (netd_ctl("PING\n", reply, sizeof reply) != 0) return false;
    return strncmp(reply, "OK", 2) == 0;
}

int soulx_netd_start_bridge(const char* mode, const char* bridge_if, int* port_out) {
    char req[64], reply[64];
    snprintf(req, sizeof req, "START %s %s\n", mode, (bridge_if && bridge_if[0]) ? bridge_if : "-");
    if (netd_ctl(req, reply, sizeof reply) != 0) return -1;
    int port = 0;
    if (sscanf(reply, "OK %d", &port) == 1 && port > 0) { *port_out = port; return 0; }
    soulx_log(SOULX_LOG_ERROR, "soulx_netd refused bridge: %s", reply);
    return -1;
}

int soulx_netd_stop_bridge(int port) {
    if (port <= 0) return 0;
    char req[48], reply[64];
    snprintf(req, sizeof req, "STOP %d\n", port);
    if (netd_ctl(req, reply, sizeof reply) != 0) return -1;
    return strncmp(reply, "OK", 2) == 0 ? 0 : -1;
}

// One-time install: copy the bundled soulx_netd helper into place and load its LaunchDaemon, all under a
// single administrator-password prompt (osascript). The helper binary sits next to this executable
// (Contents/Helpers/soulx_netd in the app, build/macos/soulx_netd in development).
int soulx_netd_install(void) {
    char self[PATH_MAX];
    uint32_t sz = sizeof self;
    if (_NSGetExecutablePath(self, &sz) != 0) return -1;
    char real[PATH_MAX];
    if (!realpath(self, real)) return -1;
    char* slash = strrchr(real, '/');
    if (!slash) return -1;
    *slash = '\0';
    char helper[PATH_MAX];
    if (snprintf(helper, sizeof helper, "%s/soulx_netd", real) >= (int)sizeof helper) return -1;
    if (access(helper, X_OK) != 0) {
        soulx_log(SOULX_LOG_ERROR, "soulx_netd helper not found next to soulx (%s)", helper);
        return -1;
    }

    // Write the privileged install steps to a temp script, then run it once with admin rights.
    char script[PATH_MAX];
    if (soulx_path(script, sizeof script, "tmp/install-netd.sh") != 0) return -1;
    FILE* f = fopen(script, "w");
    if (!f) { soulx_log(SOULX_LOG_ERROR, "cannot write %s: %s", script, strerror(errno)); return -1; }
    fprintf(f,
        "#!/bin/zsh\n"
        "set -e\n"
        "mkdir -p /usr/local/libexec\n"
        "launchctl bootout system '" NETD_PLIST "' 2>/dev/null || true\n"
        "install -m 755 -o root -g wheel '%s' '" NETD_BIN "'\n"
        "codesign --force --sign - '" NETD_BIN "' 2>/dev/null || true\n"
        "cat > '" NETD_PLIST "' <<'PLIST'\n%sPLIST\n"
        "chown root:wheel '" NETD_PLIST "'\n"
        "chmod 644 '" NETD_PLIST "'\n"
        "launchctl bootstrap system '" NETD_PLIST "' 2>/dev/null || launchctl load '" NETD_PLIST "'\n",
        helper, k_plist);
    fclose(f);
    chmod(script, 0700);

    char osa[PATH_MAX + 128];
    snprintf(osa, sizeof osa,
             "do shell script \"/bin/zsh '%s'\" with administrator privileges", script);
    const char* argv[] = { "/usr/bin/osascript", "-e", osa, NULL };
    SoulXCmdResult res;
    memset(&res, 0, sizeof res);
    SoulXCmd cmd = { .argv = argv, .timeout_ms = 120000 };
    int rc = soulx_run(&cmd, &res);
    soulx_cmd_result_free(&res);
    unlink(script);
    if (rc != 0) {
        soulx_log(SOULX_LOG_ERROR, "installing the network helper was cancelled or failed");
        return -1;
    }
    // give launchd a moment to bring the daemon up
    for (int i = 0; i < 20 && !soulx_netd_installed(); i++) usleep(100000);
    return soulx_netd_installed() ? 0 : -1;
}

// Remove the daemon (unload it, delete its plist and binary) under one admin-password prompt.
int soulx_netd_uninstall(void) {
    char script[PATH_MAX];
    if (soulx_path(script, sizeof script, "tmp/uninstall-netd.sh") != 0) return -1;
    FILE* f = fopen(script, "w");
    if (!f) { soulx_log(SOULX_LOG_ERROR, "cannot write %s: %s", script, strerror(errno)); return -1; }
    fprintf(f,
        "#!/bin/zsh\n"
        "launchctl bootout system '" NETD_PLIST "' 2>/dev/null || "
        "launchctl unload '" NETD_PLIST "' 2>/dev/null || true\n"
        "rm -f '" NETD_PLIST "' '" NETD_BIN "' '" NETD_SOCK "'\n");
    fclose(f);
    chmod(script, 0700);

    char osa[PATH_MAX + 128];
    snprintf(osa, sizeof osa,
             "do shell script \"/bin/zsh '%s'\" with administrator privileges", script);
    const char* argv[] = { "/usr/bin/osascript", "-e", osa, NULL };
    SoulXCmdResult res;
    memset(&res, 0, sizeof res);
    SoulXCmd cmd = { .argv = argv, .timeout_ms = 60000 };
    int rc = soulx_run(&cmd, &res);
    soulx_cmd_result_free(&res);
    unlink(script);
    return rc == 0 ? 0 : -1;
}
