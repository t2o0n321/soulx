/*
 * SoulX Android Emulator - macOS device definitions
 *
 * Writes the AVD files the emulator reads from $ANDROID_AVD_HOME ($SOULX_HOME/avd):
 *   avd/soulx_<name>.ini              -> points at the content directory
 *   avd/soulx_<name>.avd/config.ini   -> hardware definition
 * No avdmanager, no Android Studio files.
 */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "soulx_mac.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

typedef struct {
    char avd[SOULX_NAME_MAX + 8];            // soulx_<name>
    char dir[PATH_MAX];                     // $S/avd/soulx_<name>.avd
    char ini[PATH_MAX];                     // $S/avd/soulx_<name>.ini
    char config[PATH_MAX];                  // $S/avd/soulx_<name>.avd/config.ini
} AvdPaths;

static int avd_paths(const char* name, AvdPaths* p) {
    soulx_avd_name(name, p->avd, sizeof(p->avd));
    if (!p->avd[0]) return -1;
    if (soulx_path(p->dir, sizeof(p->dir), "avd/%s.avd", p->avd) != 0) return -1;
    if (soulx_path(p->ini, sizeof(p->ini), "avd/%s.ini", p->avd) != 0) return -1;
    int n = snprintf(p->config, sizeof(p->config), "%s/config.ini", p->dir);
    if (n < 0 || (size_t)n >= sizeof(p->config)) return -1;
    // ini values end at the line break: refuse a $SOULX_HOME that would break them
    if (strpbrk(p->dir, "\r\n")) return -1;
    return 0;
}

// Atomically writes |data| unless the file already holds exactly that content
// (keeps the mtime stable so quick-boot snapshots are not needlessly distrusted).
static int write_if_changed(const char* path, const char* data, size_t len) {
    struct stat sb;
    if (lstat(path, &sb) == 0 && S_ISREG(sb.st_mode) && (size_t)sb.st_size == len) {
        char* old = NULL;
        size_t old_len = 0;
        if (soulx_read_file(path, &old, &old_len) == 0) {
            bool same = old_len == len && memcmp(old, data, len) == 0;
            free(old);
            if (same) return 0;
        }
    }
    if (soulx_write_file_atomic(path, data, len) != 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot write %s", path);
        return -1;
    }
    return 0;
}

// Emulator GPU mode for a SoulX gpu setting: hardware ("host") or SwiftShader for "software". The engine
// command line always names the mode (soulx_mac_start: "metal" runs as -gpu swangle), so this only
// records which kind of renderer the device uses.
static const char* emulator_gpu_mode(const char* gpu) {
    return strcmp(gpu, "software") == 0 ? "swiftshader_indirect" : "host";
}

// ---------------------------------------------------------------------------
// Public functions
// ---------------------------------------------------------------------------

int soulx_avd_write(const SoulXInstanceConf* c) {
    if (!c || !soulx_name_valid(c->name)) return -1;

    const SoulXPackage* img = soulx_runtime_find(c->image);
    if (!img || img->kind != SOULX_PKG_IMAGE || !img->target || !img->install_rel) {
        soulx_log(SOULX_LOG_ERROR, "unknown system image '%.15s' for instance '%s'", c->image, c->name);
        return -1;
    }
    if (c->cores <= 0 || c->ram_mb <= 0 || c->data_gb <= 0 ||
        c->width <= 0 || c->height <= 0 || c->dpi <= 0) {
        soulx_log(SOULX_LOG_ERROR, "invalid hardware values for instance '%s'", c->name);
        return -1;
    }
    // Any change to config.ini invalidates the quick-boot snapshot of a running
    // engine, so device definitions are only (re)written while it is stopped.
    if (soulx_instance_running(c->name, NULL)) {
        soulx_log(SOULX_LOG_ERROR, "instance '%s' is running; its device definition can only change while it is stopped",
                 c->name);
        return -1;
    }

    AvdPaths p;
    if (avd_paths(c->name, &p) != 0) {
        soulx_log(SOULX_LOG_ERROR, "AVD path for '%s' is too long", c->name);
        return -1;
    }
    if (soulx_mkdirs(p.dir) != 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot create %s: %s", p.dir, strerror(errno));
        return -1;
    }

    char ini[PATH_MAX + 256];
    int n = snprintf(ini, sizeof(ini),
        "avd.ini.encoding=UTF-8\n"
        "path=%s\n"
        "path.rel=avd/%s.avd\n"
        "target=%s\n",
        p.dir, p.avd, img->target);
    if (n < 0 || (size_t)n >= sizeof(ini)) return -1;
    if (write_if_changed(p.ini, ini, (size_t)n) != 0) return -1;

    const char* tag_id = img->tag_id ? img->tag_id : "default";
    const char* tag_display = img->tag_display ? img->tag_display : "Default";
    char cfg[4096];
    n = snprintf(cfg, sizeof(cfg),
        "avd.ini.encoding=UTF-8\n"
        "AvdId=%s\n"
        "avd.ini.displayname=SoulX %s\n"
        "target=%s\n"
        "image.sysdir.1=%s/\n"
        "tag.id=%s\n"
        "tag.display=%s\n"
        "abi.type=arm64-v8a\n"
        "hw.cpu.arch=arm64\n"
        "hw.cpu.ncore=%d\n"
        "hw.ramSize=%d\n"
        "vm.heapSize=512\n"
        "disk.dataPartition.size=%dG\n"
        "PlayStore.enabled=%s\n"
        "hw.gpu.enabled=yes\n"
        "hw.gpu.mode=%s\n"
        "hw.lcd.width=%d\n"
        "hw.lcd.height=%d\n"
        "hw.lcd.density=%d\n"
        "hw.lcd.vsync=%d\n"
        "skin.name=%dx%d\n"
        "skin.path=%dx%d\n"
        "hw.initialOrientation=%s\n"
        "hw.keyboard=yes\n"
        "hw.mainKeys=no\n"
        "hw.dPad=no\n"
        "hw.trackBall=no\n"
        "hw.sdCard=no\n"
        "hw.audioInput=no\n"
        "hw.audioOutput=%s\n"
        "hw.camera.back=none\n"
        "hw.camera.front=none\n"
        "hw.accelerometer=yes\n"
        "hw.gyroscope=yes\n"
        "hw.gps=yes\n"
        "hw.battery=yes\n"
        "hw.sensors.proximity=no\n"
        "hw.sensors.light=no\n"
        "hw.sensors.pressure=no\n"
        "hw.sensors.humidity=no\n"
        "hw.sensors.temperature=no\n"
        "fastboot.forceColdBoot=no\n",
        p.avd, c->name, img->target, img->install_rel, tag_id, tag_display,
        c->cores, c->ram_mb, c->data_gb, img->playstore ? "yes" : "no",
        emulator_gpu_mode(c->gpu), c->width, c->height, c->dpi, c->fps,
        c->width, c->height, c->width, c->height,
        c->landscape ? "landscape" : "portrait", c->audio ? "yes" : "no");
    if (n < 0 || (size_t)n >= sizeof(cfg)) return -1;
    return write_if_changed(p.config, cfg, (size_t)n);
}

int soulx_avd_remove(const SoulXInstanceConf* c) {
    if (!c || !soulx_name_valid(c->name)) return -1;
    if (soulx_instance_running(c->name, NULL)) {
        soulx_log(SOULX_LOG_ERROR, "instance '%s' is running; stop it before removing its device data", c->name);
        return -1;
    }

    AvdPaths p;
    if (avd_paths(c->name, &p) != 0) return -1;

    int rc = 0;
    struct stat sb;
    if (lstat(p.dir, &sb) == 0 && soulx_rmtree(p.dir) != 0) {      // refuses paths outside $SOULX_HOME
        soulx_log(SOULX_LOG_ERROR, "cannot remove %s", p.dir);
        rc = -1;
    }
    if (unlink(p.ini) != 0 && errno != ENOENT) {
        soulx_log(SOULX_LOG_ERROR, "cannot remove %s: %s", p.ini, strerror(errno));
        rc = -1;
    }
    return rc;
}
