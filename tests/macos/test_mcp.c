/*
 * SoulX Android Emulator - unit tests for the MCP server (mcp.c): settings, permissions, the kill switch and the
 * protocol answers that need no running instance. SOULX_HOME is a fresh mkdtemp() directory under /private/tmp.
 */

#include <ftw.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "soulx_mac.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do {                                                        \
        g_checks++;                                                             \
        if (!(cond)) {                                                          \
            g_failures++;                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
        }                                                                       \
    } while (0)

static int rm_cb(const char* path, const struct stat* st, int flag, struct FTW* ftw) {
    (void)st;
    (void)ftw;
    if (flag == FTW_DP || flag == FTW_D || flag == FTW_DNR) rmdir(path); else unlink(path);
    return 0;
}

// Calls a tool; returns its result JSON (caller frees) and whether it was an error.
static char* call(const char* tool, const char* args, bool* err) {
    char* out = NULL;
    soulx_mcp_call_tool(tool, args, &out, err);
    return out;
}

static void test_config(void) {
    SoulXMcpConfig c;
    CHECK(soulx_mcp_config_load(&c) == 0);
    CHECK(!c.enabled && !strcmp(c.instances, "*"));            // off by default
    CHECK(c.allow_start_stop && !c.allow_install && c.allow_scripts && c.allow_lua);
    CHECK(soulx_mcp_instance_allowed(&c, "default"));
    snprintf(c.instances, sizeof(c.instances), "farm, bot");
    CHECK(soulx_mcp_instance_allowed(&c, "farm") && soulx_mcp_instance_allowed(&c, "bot"));
    CHECK(!soulx_mcp_instance_allowed(&c, "default") && !soulx_mcp_instance_allowed(&c, "far"));
    CHECK(!soulx_mcp_instance_allowed(&c, "../x"));
    c.enabled = true;
    c.allow_lua = false;
    CHECK(soulx_mcp_config_save(&c) == 0);
    SoulXMcpConfig d;
    CHECK(soulx_mcp_config_load(&d) == 0 && d.enabled && !d.allow_lua && !strcmp(d.instances, "farm, bot"));
}

static void test_calls(void) {
    SoulXInstanceConf a, b;
    soulx_conf_defaults(&a, "farm");
    a.id = soulx_reg_next_id();
    CHECK(soulx_reg_save(&a) == 0);
    soulx_conf_defaults(&b, "default");
    b.id = soulx_reg_next_id();
    CHECK(soulx_reg_save(&b) == 0);

    bool err = false;
    char* r = call("list_instances", "{}", &err);       // allowed: farm, bot (bot does not exist)
    CHECK(r && !err && strstr(r, "farm: stopped") && !strstr(r, "default:"));
    free(r);
    r = call("tap", "{\"instance\":\"default\",\"x\":1,\"y\":2}", &err);
    CHECK(r && err && strstr(r, "not allowed for AI control"));
    free(r);
    r = call("run_lua", "{\"instance\":\"farm\",\"code\":\"log(1)\"}", &err);
    CHECK(r && err && strstr(r, "running Lua code is not allowed"));
    free(r);
    r = call("install_app", "{\"instance\":\"farm\",\"path\":\"/x.apk\"}", &err);
    CHECK(r && err && strstr(r, "installing apps is not allowed"));
    free(r);
    r = call("tap", "{\"instance\":\"farm\",\"x\":-5,\"y\":2}", &err);
    CHECK(r && err && strstr(r, "'x' must be between"));
    free(r);
    r = call("no_such_tool", "{}", &err);
    CHECK(r && err && strstr(r, "unknown tool"));
    free(r);
    r = call("run_script", "{\"instance\":\"farm\",\"script\":\"/etc/passwd\"}", &err);
    CHECK(r && err && strstr(r, "only the instance's own scripts"));
    free(r);

    // the kill switch: off stops every tool at once, without restarting the server
    SoulXMcpConfig c;
    soulx_mcp_config_load(&c);
    c.enabled = false;
    CHECK(soulx_mcp_config_save(&c) == 0);
    r = call("list_instances", "{}", &err);
    CHECK(r && err && strstr(r, "turned off"));
    free(r);

    // every call is in the audit log
    char path[PATH_MAX];
    char* log = NULL;
    size_t len = 0;
    CHECK(soulx_path(path, sizeof(path), "logs/mcp.log") == 0 && soulx_read_file(path, &log, &len) == 0);
    CHECK(log && strstr(log, "\terror\ttap\t-\tinstance=default x=1 y=2") && strstr(log, "\tok\tlist_instances\t"));
    free(log);

    int n = 0;
    while (soulx_mcp_tool_names(n)) n++;
    CHECK(n == 22);
}

// One-click connection: SoulX's entry is added to and removed from an app's JSON settings, everything else stays,
// the original is backed up, and a damaged file is never touched.  points to a temporary folder.
static void test_clients(void) {
    char fake[] = "/private/tmp/soulx-test-mcp-home-XXXXXX";
    if (!mkdtemp(fake)) return;
    char* old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
    setenv("HOME", fake, 1);
    char dir[PATH_MAX], cfg[PATH_MAX], bak[PATH_MAX + 16];
    snprintf(dir, sizeof(dir), "%s/.cursor", fake);
    soulx_mkdirs(dir);
    snprintf(cfg, sizeof(cfg), "%s/mcp.json", dir);
    static const char orig[] = "{\"mcpServers\": {\"other\": {\"command\": \"node\", \"env\": {\"K\": \"v\"}}}, \"n\": 1.5, \"u\": \"\\u4e2d\"}";
    CHECK(soulx_write_file_atomic(cfg, orig, sizeof(orig) - 1) == 0);
    char msg[512];
    CHECK(soulx_mcp_client_connect("cursor", "/opt/soulx", true, msg, sizeof(msg)) == 0);
    char* text = NULL;
    size_t len = 0;
    CHECK(soulx_read_file(cfg, &text, &len) == 0 && text);
    if (text) {
        CHECK(strstr(text, "\"soulx\": {") && strstr(text, "\"command\": \"/opt/soulx\"") && strstr(text, "\"mcp\""));
        CHECK(strstr(text, "\"other\"") && strstr(text, "\"K\": \"v\"") && strstr(text, "\"n\": 1.5") && strstr(text, "中"));
    }
    free(text);
    snprintf(bak, sizeof(bak), "%s.soulx-backup", cfg);
    text = NULL;
    CHECK(soulx_read_file(bak, &text, &len) == 0 && text && !strcmp(text, orig));    // the user's original
    free(text);
    int n = soulx_mcp_client_count(), found = 0;
    for (int i = 0; i < n; i++) {
        SoulXMcpClientInfo ci;
        if (soulx_mcp_client_info(i, "/opt/soulx", &ci) == 0 && !strcmp(ci.id, "cursor")) {
            found = 1;
            CHECK(ci.installed && !strcmp(ci.state, "connected"));
        }
        if (soulx_mcp_client_info(i, "/elsewhere/soulx", &ci) == 0 && !strcmp(ci.id, "cursor")) CHECK(!strcmp(ci.state, "other"));
    }
    CHECK(found);
    CHECK(soulx_mcp_client_connect("cursor", "/opt/soulx", false, msg, sizeof(msg)) == 0);
    text = NULL;
    CHECK(soulx_read_file(cfg, &text, &len) == 0 && text && !strstr(text, "soulx") && strstr(text, "\"other\""));
    free(text);
    // a damaged file is left exactly as it is
    static const char broken[] = "{ \"mcpServers\": { broken";
    CHECK(soulx_write_file_atomic(cfg, broken, sizeof(broken) - 1) == 0);
    CHECK(soulx_mcp_client_connect("cursor", "/opt/soulx", true, msg, sizeof(msg)) == -1 && strstr(msg, "not valid JSON"));
    text = NULL;
    CHECK(soulx_read_file(cfg, &text, &len) == 0 && text && !strcmp(text, broken));
    free(text);
    // a missing file is created; an unknown app is refused
    unlink(cfg);
    unlink(bak);
    CHECK(soulx_mcp_client_connect("cursor", "/opt/soulx", true, msg, sizeof(msg)) == 0 && soulx_path_exists(cfg));
    CHECK(soulx_mcp_client_connect("nope", "/opt/soulx", true, msg, sizeof(msg)) == -1);
    if (old_home) { setenv("HOME", old_home, 1); free(old_home); } else unsetenv("HOME");
    nftw(fake, rm_cb, 32, FTW_DEPTH | FTW_PHYS);
}

int main(void) {
    char home[] = "/private/tmp/soulx-test-mcp-XXXXXX";
    char scripts[] = "/private/tmp/soulx-test-mcp-scripts-XXXXXX";
    if (!mkdtemp(home) || !mkdtemp(scripts)) { perror("mkdtemp"); return 2; }
    setenv("SOULX_HOME", home, 1);
    setenv("SOULX_SCRIPTS", scripts, 1);
    unsetenv("SOULX_INSTANCE");
    soulx_ensure_layout();
    soulx_mcp_set_self("/usr/bin/false");               // no tool here may need to run soulx
    test_config();
    test_calls();
    test_clients();
    nftw(home, rm_cb, 32, FTW_DEPTH | FTW_PHYS);
    nftw(scripts, rm_cb, 32, FTW_DEPTH | FTW_PHYS);
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
