/*
 * SoulX Android Emulator - unit tests for scripts.c (each instance's scripts) and the UI language setting
 *
 * Standalone program: returns 0 when every check passes and prints each failure. SOULX_HOME, SOULX_SCRIPTS and
 * SOULX_TRASH_DIR all point into fresh mkdtemp() directories under /private/tmp: the user's Documents and Trash
 * are never touched.
 */

#include <ftw.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

static char g_root[PATH_MAX];       // SOULX_SCRIPTS
static char g_trash[PATH_MAX];      // SOULX_TRASH_DIR
static char g_src[PATH_MAX];        // files to import

static int rm_cb(const char* path, const struct stat* st, int flag, struct FTW* ftw) {
    (void)st;
    (void)ftw;
    if (flag == FTW_DP || flag == FTW_D || flag == FTW_DNR) rmdir(path); else unlink(path);
    return 0;
}

static void rm_rf(const char* path) {
    if (path && strncmp(path, "/private/tmp/", 13) == 0) nftw(path, rm_cb, 32, FTW_DEPTH | FTW_PHYS);
}

static void put(const char* dir, const char* rel, const char* text) {
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/%s", dir, rel);
    char d[PATH_MAX];
    snprintf(d, sizeof(d), "%s", p);
    char* slash = strrchr(d, '/');
    if (slash) { *slash = '\0'; soulx_mkdirs(d); }
    FILE* f = fopen(p, "w");
    if (f) { fputs(text, f); fclose(f); }
}

static bool exists_in(const char* dir, const char* rel) {
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/%s", dir, rel);
    return soulx_path_exists(p);
}

static void register_instance(const char* name) {
    SoulXInstanceConf c;
    soulx_conf_defaults(&c, name);
    c.id = soulx_reg_next_id();
    CHECK(soulx_reg_save(&c) == 0);
}

// Before per-instance folders every script sat in the root: those move into default/ once; instance folders stay.
static void test_migrate(void) {
    register_instance("default");
    register_instance("farm");
    put(g_root, "old.lua", "-- name: Old\n");
    put(g_root, "pkg/main.lua", "-- name: Pkg\n");
    put(g_root, "pkg/lib/util.lua", "return {}\n");
    put(g_root, "farm/f.lua", "-- name: Farm only\n");        // already an instance's folder
    put(g_root, ".DS_Store", "x");
    CHECK(soulx_scripts_migrate() == 0);
    CHECK(exists_in(g_root, "default/old.lua"));
    CHECK(exists_in(g_root, "default/pkg/lib/util.lua"));
    CHECK(!exists_in(g_root, "old.lua") && !exists_in(g_root, "pkg"));
    CHECK(exists_in(g_root, "farm/f.lua"));                  // not moved
    CHECK(exists_in(g_root, ".soulx-per-instance"));
    // once only: a later root-level file is not swept into default/
    put(g_root, "later.lua", "-- name: Later\n");
    CHECK(soulx_scripts_migrate() == 0);
    CHECK(exists_in(g_root, "later.lua"));
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/later.lua", g_root);
    unlink(p);
}

static void test_names(void) {
    char name[128];
    put(g_src, "a.lua", "  -- @name = Daily run  \nlog(1)\n");
    put(g_src, "b.lua", "log(1)\n-- named things are not names\n");
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/a.lua", g_src);
    CHECK(soulx_script_name(p, name, sizeof(name)) == 0 && strcmp(name, "Daily run") == 0);
    snprintf(p, sizeof(p), "%s/b.lua", g_src);
    CHECK(soulx_script_name(p, name, sizeof(name)) == -1 && name[0] == '\0');
}

static void test_list_import_rename_trash(void) {
    SoulXScript list[16];
    int n = soulx_scripts_list("default", list, 16);
    CHECK(n == 2);                                            // old.lua, pkg/main.lua (lib/ is not a script)
    if (n == 2) {
        CHECK(strcmp(list[0].file, "old.lua") == 0 && !list[0].package && strcmp(list[0].name, "Old") == 0);
        CHECK(strcmp(list[1].file, "pkg/main.lua") == 0 && list[1].package && strcmp(list[1].top, "pkg") == 0);
    }
    CHECK(soulx_scripts_list("farm", list, 16) == 1);         // instances keep their own scripts
    CHECK(soulx_scripts_list("empty", list, 16) == 0);        // no folder yet: none

    // import a file and a folder; importing again replaces it and keeps the old copy in the Trash
    put(g_src, "new.lua", "log(\"new\")\n");
    put(g_src, "tool/run.lua", "-- name: Tool\n");
    char src[PATH_MAX], dest[PATH_MAX];
    snprintf(src, sizeof(src), "%s/new.lua", g_src);
    CHECK(soulx_scripts_import("farm", src, dest, sizeof(dest)) == 0 && exists_in(g_root, "farm/new.lua"));
    snprintf(src, sizeof(src), "%s/tool/", g_src);           // a trailing slash is fine
    CHECK(soulx_scripts_import("farm", src, dest, sizeof(dest)) == 0 && exists_in(g_root, "farm/tool/run.lua"));
    snprintf(src, sizeof(src), "%s/new.lua", g_src);
    CHECK(soulx_scripts_import("farm", src, dest, sizeof(dest)) == 0);
    CHECK(exists_in(g_trash, "new.lua"));
    snprintf(src, sizeof(src), "%s/notes.txt", g_src);
    put(g_src, "notes.txt", "x");
    CHECK(soulx_scripts_import("farm", src, dest, sizeof(dest)) == -1);   // not a script
    CHECK(soulx_scripts_list("farm", list, 16) == 3);

    // rename: adds a name line, or replaces the existing one
    CHECK(soulx_scripts_set_name("farm", "new.lua", "Fresh") == 0);
    char path[PATH_MAX], name[128];
    snprintf(path, sizeof(path), "%s/farm/new.lua", g_root);
    CHECK(soulx_script_name(path, name, sizeof(name)) == 0 && strcmp(name, "Fresh") == 0);
    CHECK(soulx_scripts_set_name("farm", "new.lua", "Fresher") == 0);
    char* text = NULL;
    size_t len = 0;
    CHECK(soulx_read_file(path, &text, &len) == 0 && text && strcmp(text, "-- name: Fresher\nlog(\"new\")\n") == 0);
    free(text);
    CHECK(soulx_scripts_set_name("farm", "../x.lua", "Bad") == -1);
    CHECK(soulx_scripts_set_name("farm", "new.lua", "two\nlines") == -1);

    // find by display name, file name or stem; ambiguous names refuse
    char found[PATH_MAX];
    CHECK(soulx_scripts_find("farm", "Fresher", found, sizeof(found)) == 0 && strstr(found, "/farm/new.lua"));
    CHECK(soulx_scripts_find("farm", "run", found, sizeof(found)) == 0 && strstr(found, "/farm/tool/run.lua"));
    CHECK(soulx_scripts_find("farm", "Old", found, sizeof(found)) == -1);   // default's, not farm's
    CHECK(soulx_scripts_set_name("farm", "f.lua", "Tool") == 0);           // now two scripts are "Tool"
    CHECK(soulx_scripts_find("farm", "Tool", found, sizeof(found)) == -2);

    // remove: only top-level entries, into the Trash (a second one with the same name gets a unique name)
    CHECK(soulx_scripts_trash("farm", "tool") == 0 && !exists_in(g_root, "farm/tool") && exists_in(g_trash, "tool"));
    CHECK(soulx_scripts_trash("farm", "new.lua") == 0 && !exists_in(g_root, "farm/new.lua"));
    CHECK(soulx_scripts_trash("farm", "../default") == -1 && exists_in(g_root, "default"));
    CHECK(soulx_scripts_trash("farm", "missing.lua") == -1);
    CHECK(soulx_scripts_trash("farm", ".soulx-per-instance") == -1);
}

static void test_language(void) {
    unsetenv("SOULX_LANG");
    CHECK(strcmp(soulx_ui_language(), "system") == 0);
    CHECK(soulx_set_ui_language("zh-TW") == 0 && strcmp(soulx_ui_language(), "zh-hant") == 0);
    CHECK(soulx_set_ui_language("fr") == -1 && strcmp(soulx_ui_language(), "zh-hant") == 0);
    setenv("SOULX_LANG", "en", 1);
    CHECK(strcmp(soulx_ui_language(), "en") == 0);           // the environment wins
    unsetenv("SOULX_LANG");
    CHECK(soulx_set_ui_language("system") == 0 && strcmp(soulx_ui_language(), "system") == 0);
}

int main(void) {
    char home[] = "/private/tmp/soulx-test-scripts-home-XXXXXX";
    char root[] = "/private/tmp/soulx-test-scripts-root-XXXXXX";
    char trash[] = "/private/tmp/soulx-test-scripts-trash-XXXXXX";
    char src[] = "/private/tmp/soulx-test-scripts-src-XXXXXX";
    if (!mkdtemp(home) || !mkdtemp(root) || !mkdtemp(trash) || !mkdtemp(src)) {
        perror("mkdtemp");
        return 2;
    }
    snprintf(g_root, sizeof(g_root), "%s", root);
    snprintf(g_trash, sizeof(g_trash), "%s", trash);
    snprintf(g_src, sizeof(g_src), "%s", src);
    setenv("SOULX_HOME", home, 1);
    setenv("SOULX_SCRIPTS", root, 1);
    setenv("SOULX_TRASH_DIR", trash, 1);
    soulx_ensure_layout();

    test_migrate();
    test_names();
    test_list_import_rename_trash();
    test_language();

    rm_rf(home);
    rm_rf(root);
    rm_rf(trash);
    rm_rf(src);
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
