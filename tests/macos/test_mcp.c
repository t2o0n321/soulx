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
    nftw(home, rm_cb, 32, FTW_DEPTH | FTW_PHYS);
    nftw(scripts, rm_cb, 32, FTW_DEPTH | FTW_PHYS);
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
