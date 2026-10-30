// soulx_netd — vmnet bridge daemon + worker for SoulX networking modes (zero deps).
//
// One binary, three roles:
//   soulx_netd daemon
//       Long-running root LaunchDaemon. Listens on a control unix socket and, on request,
//       spawns per-instance vmnet<->TCP bridge workers and hands back a loopback TCP port.
//   soulx_netd bridge-fd <shared|host|bridged> <listen_fd> [bridge-if]
//       Worker spawned by the daemon; accepts on an inherited listening socket (fd).
//   soulx_netd bridge <shared|host|bridged> <port> [bridge-if]
//       Standalone worker that binds 127.0.0.1:<port> itself (for manual testing).
//
// The emulator connects to a worker with:  -wifi-socket connect=127.0.0.1:<port>
// Frame format on the TCP socket is QEMU's: 4-byte big-endian length prefix + ethernet frame.
//
// Control protocol (one request per connection, newline-terminated text):
//   "START <mode> <bridge_if|->\n"  -> "OK <port>\n" | "ERR <msg>\n"
//   "STOP <port>\n"                 -> "OK\n"        | "ERR <msg>\n"
//   "PING\n"                        -> "OK\n"

#include <vmnet/vmnet.h>
#include <xpc/xpc.h>
#include <dispatch/dispatch.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

#define SOULX_MAX_PKT     2048
#define CTRL_SOCK   "/var/run/soulx-netd.sock"
#define MAX_BRIDGES 32

// ---------------------------------------------------------------------------
// bridge worker: vmnet <-> TCP pump
// ---------------------------------------------------------------------------

static interface_ref g_iface;
static dispatch_queue_t g_vmnet_q;
static int g_cfd = -1;
static uint64_t g_max_pkt = SOULX_MAX_PKT;

static int write_all(int fd, const void *buf, size_t n) {
    const char *p = buf;
    while (n) { ssize_t w = write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        if (w == 0) return -1; p += w; n -= (size_t)w; }
    return 0;
}
static int read_all(int fd, void *buf, size_t n) {
    char *p = buf;
    while (n) { ssize_t r = read(fd, p, n);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) return -1; p += r; n -= (size_t)r; }
    return 0;
}

static void on_vmnet_packets(void) {
    for (;;) {
        char buf[SOULX_MAX_PKT];
        struct iovec iov = { .iov_base = buf, .iov_len = sizeof(buf) };
        struct vmpktdesc pd = { .vm_pkt_size = sizeof(buf), .vm_pkt_iov = &iov,
                                .vm_pkt_iovcnt = 1, .vm_flags = 0 };
        int cnt = 1;
        if (vmnet_read(g_iface, &pd, &cnt) != VMNET_SUCCESS || cnt < 1) break;
        if (g_cfd < 0) continue;                           // no emulator yet: discard
        uint32_t len = htonl((uint32_t)pd.vm_pkt_size);
        if (write_all(g_cfd, &len, 4) != 0 || write_all(g_cfd, buf, pd.vm_pkt_size) != 0) {
            fprintf(stderr, "soulx_netd: emulator socket closed (vmnet->tcp)\n"); exit(0);
        }
    }
}
static void *tcp_reader(void *arg) {
    (void)arg;
    for (;;) {
        uint32_t be; if (read_all(g_cfd, &be, 4) != 0) break;
        uint32_t len = ntohl(be);
        if (len == 0 || len > g_max_pkt) { fprintf(stderr, "soulx_netd: bad frame len %u\n", len); break; }
        char buf[SOULX_MAX_PKT]; if (read_all(g_cfd, buf, len) != 0) break;
        struct iovec iov = { .iov_base = buf, .iov_len = len };
        struct vmpktdesc pd = { .vm_pkt_size = len, .vm_pkt_iov = &iov, .vm_pkt_iovcnt = 1, .vm_flags = 0 };
        int cnt = 1; vmnet_write(g_iface, &pd, &cnt);
    }
    fprintf(stderr, "soulx_netd: emulator socket closed (tcp->vmnet)\n"); exit(0); return NULL;
}

static void start_vmnet(const char *mode, const char *bridge_if) {
    xpc_object_t d = xpc_dictionary_create(NULL, NULL, 0);
    operating_modes_t m;
    if      (!strcmp(mode, "shared"))  m = VMNET_SHARED_MODE;
    else if (!strcmp(mode, "host"))    m = VMNET_HOST_MODE;
    else if (!strcmp(mode, "bridged")) m = VMNET_BRIDGED_MODE;
    else { fprintf(stderr, "unknown mode '%s'\n", mode); exit(2); }
    xpc_dictionary_set_uint64(d, vmnet_operation_mode_key, m);
    if (m == VMNET_BRIDGED_MODE) {
        if (!bridge_if || !bridge_if[0] || !strcmp(bridge_if, "-")) {
            fprintf(stderr, "bridged needs an interface (e.g. en0)\n"); exit(2);
        }
        xpc_dictionary_set_string(d, vmnet_shared_interface_name_key, bridge_if);
    }
    dispatch_queue_t q = dispatch_queue_create("soulx.netd.vmnet", DISPATCH_QUEUE_SERIAL);
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    __block vmnet_return_t status = (vmnet_return_t)0;
    g_iface = vmnet_start_interface(d, q, ^(vmnet_return_t s, xpc_object_t p) {
        status = s;
        if (s == VMNET_SUCCESS && p) {
            g_max_pkt = xpc_dictionary_get_uint64(p, vmnet_max_packet_size_key);
            if (g_max_pkt == 0 || g_max_pkt > SOULX_MAX_PKT) g_max_pkt = SOULX_MAX_PKT;
            const char *sub = xpc_dictionary_get_string(p, vmnet_subnet_mask_key);
            const char *dhcp = xpc_dictionary_get_string(p, vmnet_start_address_key);
            fprintf(stderr, "soulx_netd: vmnet %s up [subnet=%s dhcp_start=%s max_pkt=%llu]\n", mode,
                    sub ? sub : "?", dhcp ? dhcp : "?", g_max_pkt);
        }
        dispatch_semaphore_signal(sem);
    });
    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
    if (status != VMNET_SUCCESS) { fprintf(stderr, "soulx_netd: vmnet start failed: %d\n", status); exit(1); }
    g_vmnet_q = q;
    vmnet_interface_set_event_callback(g_iface, VMNET_INTERFACE_PACKETS_AVAILABLE, q,
        ^(interface_event_t ev, xpc_object_t p) { (void)ev; (void)p; on_vmnet_packets(); });
}

// Run one bridge: vmnet is up; accept the emulator on |listen_fd| and pump until it disconnects.
static int run_bridge(const char *mode, const char *bridge_if, int listen_fd) {
    signal(SIGPIPE, SIG_IGN);
    start_vmnet(mode, bridge_if);
    g_cfd = accept(listen_fd, NULL, NULL);
    if (g_cfd < 0) { perror("accept"); return 1; }
    int one = 1; setsockopt(g_cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    close(listen_fd);
    fprintf(stderr, "soulx_netd: emulator connected; bridging\n");
    dispatch_async(g_vmnet_q, ^{ on_vmnet_packets(); });
    pthread_t t; pthread_create(&t, NULL, tcp_reader, NULL);
    dispatch_main();
    return 0;
}

static int listen_loopback(int port) {    // port 0 => ephemeral
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0) { perror("bind"); return -1; }
    if (listen(fd, 1) != 0) { perror("listen"); return -1; }
    return fd;
}

// ---------------------------------------------------------------------------
// daemon: control socket + bridge lifecycle
// ---------------------------------------------------------------------------

static const char *g_self;                 // absolute path for re-exec
static struct { int port; pid_t pid; } g_bridges[MAX_BRIDGES];

static int valid_mode(const char *m) {
    return !strcmp(m, "shared") || !strcmp(m, "host") || !strcmp(m, "bridged");
}
static int valid_ifname(const char *s) {   // bridge interface: "en0"-ish, or "-"
    if (!strcmp(s, "-")) return 1;
    if (!s[0] || strlen(s) > 15) return 0;
    for (const char *p = s; *p; p++) if (!isalnum((unsigned char)*p)) return 0;
    return 1;
}

static void reap(int sig) {   // clear exited bridges from the table
    (void)sig; int st; pid_t p;
    while ((p = waitpid(-1, &st, WNOHANG)) > 0)
        for (int i = 0; i < MAX_BRIDGES; i++) if (g_bridges[i].pid == p) { g_bridges[i].pid = 0; g_bridges[i].port = 0; }
}

// Start a bridge worker; returns its loopback port, or -1.
static int bridge_start(const char *mode, const char *bridge_if) {
    int slot = -1; for (int i = 0; i < MAX_BRIDGES; i++) if (g_bridges[i].pid == 0) { slot = i; break; }
    if (slot < 0) return -1;
    int lfd = listen_loopback(0);
    if (lfd < 0) return -1;
    struct sockaddr_in sa; socklen_t sl = sizeof(sa);
    getsockname(lfd, (struct sockaddr *)&sa, &sl);
    int port = ntohs(sa.sin_port);
    char fdbuf[16]; snprintf(fdbuf, sizeof(fdbuf), "%d", lfd);

    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    // the worker inherits lfd (we did not set CLOEXEC); it uses it to accept the emulator.
    char *const argv[] = { (char *)g_self, "bridge-fd", (char *)mode, fdbuf, (char *)bridge_if, NULL };
    pid_t pid;
    int rc = posix_spawn(&pid, g_self, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(lfd);                              // the worker holds its own copy
    if (rc != 0) { fprintf(stderr, "soulx_netd: spawn worker failed: %s\n", strerror(rc)); return -1; }
    g_bridges[slot].pid = pid; g_bridges[slot].port = port;
    fprintf(stderr, "soulx_netd: bridge pid=%d port=%d mode=%s if=%s\n", pid, port, mode, bridge_if);
    return port;
}

static int bridge_stop(int port) {
    for (int i = 0; i < MAX_BRIDGES; i++) if (g_bridges[i].port == port && g_bridges[i].pid) {
        kill(g_bridges[i].pid, SIGTERM); g_bridges[i].pid = 0; g_bridges[i].port = 0; return 0;
    }
    return -1;
}

static void handle_client(int cfd) {
    char line[256]; ssize_t n = read(cfd, line, sizeof(line) - 1);
    if (n <= 0) { close(cfd); return; }
    line[n] = 0; line[strcspn(line, "\r\n")] = 0;

    char cmd[16] = "", a1[32] = "", a2[32] = "";
    int got = sscanf(line, "%15s %31s %31s", cmd, a1, a2);
    char reply[64] = "ERR bad request\n";

    if (got >= 1 && !strcmp(cmd, "PING")) {
        snprintf(reply, sizeof(reply), "OK\n");
    } else if (got >= 2 && !strcmp(cmd, "START") && valid_mode(a1) &&
               (a2[0] ? valid_ifname(a2) : 1)) {
        int port = bridge_start(a1, a2[0] ? a2 : "-");
        if (port > 0) snprintf(reply, sizeof(reply), "OK %d\n", port);
        else          snprintf(reply, sizeof(reply), "ERR start failed\n");
    } else if (got >= 2 && !strcmp(cmd, "STOP")) {
        int port = atoi(a1);
        snprintf(reply, sizeof(reply), bridge_stop(port) == 0 ? "OK\n" : "ERR no such bridge\n");
    }
    write_all(cfd, reply, strlen(reply));
    close(cfd);
}

static int run_daemon(void) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, reap);
    unlink(CTRL_SOCK);
    int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un un; memset(&un, 0, sizeof(un));
    un.sun_family = AF_UNIX; strncpy(un.sun_path, CTRL_SOCK, sizeof(un.sun_path) - 1);
    if (bind(lfd, (struct sockaddr *)&un, sizeof(un)) != 0) { perror("bind ctrl"); return 1; }
    // admins (who installed us) may command the daemon; others may not.
    chmod(CTRL_SOCK, 0660);
    // best effort: set group to 'admin' (gid 80 on macOS) so admin users can connect
    chown(CTRL_SOCK, 0, 80);
    if (listen(lfd, 8) != 0) { perror("listen ctrl"); return 1; }
    fprintf(stderr, "soulx_netd: daemon listening on %s\n", CTRL_SOCK);
    for (;;) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) { if (errno == EINTR) continue; perror("accept ctrl"); continue; }
        handle_client(cfd);
    }
}

// ---------------------------------------------------------------------------

int main(int argc, char **argv) {
    g_self = argv[0];
    if (argc >= 2 && !strcmp(argv[1], "daemon")) {
        return run_daemon();
    }
    if (argc >= 4 && !strcmp(argv[1], "bridge-fd")) {
        return run_bridge(argv[2], argc > 4 ? argv[4] : "-", atoi(argv[3]));
    }
    if (argc >= 4 && !strcmp(argv[1], "bridge")) {
        int lfd = listen_loopback(atoi(argv[3]));
        if (lfd < 0) return 1;
        fprintf(stderr, "soulx_netd: listening on 127.0.0.1:%s\n", argv[3]);
        return run_bridge(argv[2], argc > 4 ? argv[4] : "-", lfd);
    }
    fprintf(stderr,
        "usage:\n  %s daemon\n  %s bridge <shared|host|bridged> <port> [bridge-if]\n", argv[0], argv[0]);
    return 2;
}
