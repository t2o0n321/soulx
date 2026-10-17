/*
 * SoulX Android Emulator - Main Header
 * Android emulator for macOS (Apple Silicon)
 * Copyright (c) 2026 SoulX Android Team
 */

#ifndef SOULX_ANDROID_H
#define SOULX_ANDROID_H

#include <stdint.h>
#include <stdbool.h>

#define SOULX_API

// Version information: SOULX_VERSION_STRING comes from the VERSION file at the top of the repository (CMake
// passes it to every target), so there is one place to change it.
#ifndef SOULX_VERSION_STRING
#error "SOULX_VERSION_STRING is not set: build with CMake, which reads it from the VERSION file"
#endif
#define SOULX_BUILD_DATE __DATE__

// Return codes (functions return >= 0 on success)
#define SOULX_OK                  0
#define SOULX_ERROR              -1
#define SOULX_ERR_NOT_SUPPORTED  -2
#define SOULX_ERR_NOT_RUNNING    -3
#define SOULX_ERR_NOT_FOUND      -4
#define SOULX_ERR_NEEDS_RESTART  -5

// Configuration limits
#define MAX_INSTANCES 16
#define MAX_CPUS 16
#define MAX_RAM_GB 32
#define DEFAULT_RAM_MB 2048
#define DEFAULT_CPUS 4
#define DEFAULT_RESOLUTION_WIDTH 1920
#define DEFAULT_RESOLUTION_HEIGHT 1080
#define DEFAULT_DPI 240

// Instance states
typedef enum {
    INSTANCE_STATE_STOPPED = 0,
    INSTANCE_STATE_STARTING,
    INSTANCE_STATE_RUNNING,
    INSTANCE_STATE_PAUSED,
    INSTANCE_STATE_STOPPING,
    INSTANCE_STATE_ERROR
} InstanceState;

// Graphics backend
typedef enum {
    GRAPHICS_BACKEND_OPENGL = 0,
    GRAPHICS_BACKEND_VULKAN,
    GRAPHICS_BACKEND_DIRECTX11,
    GRAPHICS_BACKEND_DIRECTX12,
    GRAPHICS_BACKEND_SOFTWARE
} GraphicsBackend;

// Network mode
typedef enum {
    NETWORK_MODE_NAT = 0,
    NETWORK_MODE_BRIDGE,
    NETWORK_MODE_HOST_ONLY,
    NETWORK_MODE_NONE
} NetworkMode;

// Performance profile
typedef enum {
    PERF_PROFILE_ECO = 0,
    PERF_PROFILE_BALANCED,
    PERF_PROFILE_PERFORMANCE,
    PERF_PROFILE_GAMING,
    PERF_PROFILE_CUSTOM
} PerformanceProfile;

// Instance configuration
typedef struct {
    char name[256];
    char android_version[32];
    int cpu_cores;
    int ram_mb;
    int storage_gb;
    int width;
    int height;
    int dpi;
    GraphicsBackend graphics;
    NetworkMode network;
    PerformanceProfile profile;
    bool enable_root;
    bool enable_adb;
    bool enable_keyboard_mapping;
    bool enable_gamepad;
    char shared_folder[1024];
} InstanceConfig;

// Instance runtime info
typedef struct {
    int id;
    InstanceState state;
    InstanceConfig config;
    uint64_t pid;
    double cpu_usage;
    uint64_t ram_usage_mb;
    double fps;
    uint64_t uptime_seconds;
    char ip_address[32];
    int adb_port;
    int console_port;           // emulator console port (macOS backend), 0 if unknown
    char serial[32];            // adb serial, e.g. "emulator-5554" (macOS backend)
} InstanceInfo;

// Performance metrics
typedef struct {
    double cpu_usage_percent;
    uint64_t ram_used_mb;
    uint64_t ram_total_mb;
    double gpu_usage_percent;
    uint64_t vram_used_mb;
    double fps_current;
    double fps_average;
    double frame_time_ms;
    uint64_t network_rx_bytes;
    uint64_t network_tx_bytes;
    double disk_read_mbps;
    double disk_write_mbps;
} PerformanceMetrics;

// Sensor data
typedef struct {
    float accelerometer[3];     // x, y, z
    float gyroscope[3];         // x, y, z
    float magnetometer[3];      // x, y, z
    double latitude;
    double longitude;
    double altitude;
    float light_level;
    float proximity;
    float pressure;
    float humidity;
    float temperature;
    int step_count;
} SensorData;

// Battery state
typedef struct {
    int level;                  // 0-100
    bool is_charging;
    int temperature;            // Celsius
    int voltage;                // mV
    char health[32];
} BatteryState;

// Callback types
typedef void (*InstanceStateCallback)(int instance_id, InstanceState state);
typedef void (*PerformanceCallback)(int instance_id, PerformanceMetrics* metrics);
typedef void (*LogCallback)(int level, const char* message);

// Core API functions
SOULX_API int soulx_init(void);
SOULX_API void soulx_shutdown(void);
SOULX_API const char* soulx_get_version(void);

// Instance management
SOULX_API int soulx_create_instance(const InstanceConfig* config);
SOULX_API int soulx_delete_instance(int instance_id);
SOULX_API int soulx_start_instance(int instance_id);
SOULX_API int soulx_stop_instance(int instance_id);
SOULX_API int soulx_pause_instance(int instance_id);
SOULX_API int soulx_resume_instance(int instance_id);
SOULX_API int soulx_restart_instance(int instance_id);
SOULX_API int soulx_clone_instance(int source_id, const char* new_name);
SOULX_API int soulx_get_instance_count(void);
SOULX_API int soulx_get_instance_info(int instance_id, InstanceInfo* info);
SOULX_API int soulx_list_instances(InstanceInfo* instances, int max_count);

// Configuration
SOULX_API int soulx_set_config(int instance_id, const InstanceConfig* config);
SOULX_API int soulx_get_config(int instance_id, InstanceConfig* config);
SOULX_API int soulx_set_resolution(int instance_id, int width, int height, int dpi);
SOULX_API int soulx_set_performance_profile(int instance_id, PerformanceProfile profile);

// Graphics
SOULX_API int soulx_set_graphics_backend(int instance_id, GraphicsBackend backend);
SOULX_API int soulx_set_fps_limit(int instance_id, int fps);
SOULX_API int soulx_take_screenshot(int instance_id, const char* path);
SOULX_API int soulx_start_recording(int instance_id, const char* path);
SOULX_API int soulx_stop_recording(int instance_id);

// Input handling
SOULX_API int soulx_send_touch(int instance_id, int x, int y, int action);
SOULX_API int soulx_send_multitouch(int instance_id, int* x, int* y, int* actions, int count);
SOULX_API int soulx_send_key(int instance_id, int keycode, int action);
SOULX_API int soulx_send_text(int instance_id, const char* text);
SOULX_API int soulx_send_gesture(int instance_id, const char* gesture_type);

// Sensors
SOULX_API int soulx_set_sensor_data(int instance_id, const SensorData* data);
SOULX_API int soulx_set_location(int instance_id, double lat, double lon, double alt);
SOULX_API int soulx_set_battery(int instance_id, const BatteryState* state);

// Networking
SOULX_API int soulx_set_network_mode(int instance_id, NetworkMode mode);
SOULX_API int soulx_set_network_speed(int instance_id, int download_kbps, int upload_kbps);
SOULX_API int soulx_set_proxy(int instance_id, const char* host, int port);
SOULX_API int soulx_enable_vpn(int instance_id, const char* config_path);

// File system
SOULX_API int soulx_install_apk(int instance_id, const char* apk_path);
SOULX_API int soulx_uninstall_app(int instance_id, const char* package_name);
SOULX_API int soulx_push_file(int instance_id, const char* local_path, const char* remote_path);
SOULX_API int soulx_pull_file(int instance_id, const char* remote_path, const char* local_path);
SOULX_API int soulx_set_shared_folder(int instance_id, const char* folder_path);

// ADB
SOULX_API int soulx_adb_connect(int instance_id);
SOULX_API int soulx_adb_command(int instance_id, const char* command, char* output, int max_len);
SOULX_API int soulx_get_adb_port(int instance_id);

// Backup/Restore
SOULX_API int soulx_backup(int instance_id, const char* backup_path);
SOULX_API int soulx_restore(int instance_id, const char* backup_path);
SOULX_API int soulx_snapshot_create(int instance_id, const char* name);
SOULX_API int soulx_snapshot_restore(int instance_id, const char* name);
SOULX_API int soulx_snapshot_delete(int instance_id, const char* name);

// Macros
SOULX_API int soulx_macro_record_start(int instance_id);
SOULX_API int soulx_macro_record_stop(int instance_id, const char* save_path);
SOULX_API int soulx_macro_play(int instance_id, const char* macro_path, int repeat_count);
SOULX_API int soulx_macro_stop(int instance_id);

// Performance monitoring
SOULX_API int soulx_get_performance(int instance_id, PerformanceMetrics* metrics);
SOULX_API void soulx_set_performance_callback(PerformanceCallback callback);
SOULX_API void soulx_set_state_callback(InstanceStateCallback callback);
SOULX_API void soulx_set_log_callback(LogCallback callback);

// Clipboard
SOULX_API int soulx_clipboard_copy(int instance_id, const char* text);
SOULX_API int soulx_clipboard_paste(int instance_id, char* buffer, int max_len);
SOULX_API int soulx_clipboard_sync_enable(int instance_id, bool enable);

// Extended API (implemented by the macOS backend)
SOULX_API int soulx_find_instance(const char* name);              // instance id, or SOULX_ERR_NOT_FOUND
SOULX_API int soulx_wait_boot(int instance_id, int timeout_s);
SOULX_API int soulx_launch_app(int instance_id, const char* package_name);
SOULX_API int soulx_stop_app(int instance_id, const char* package_name);
SOULX_API int soulx_send_swipe(int instance_id, int x1, int y1, int x2, int y2, int duration_ms);
SOULX_API int soulx_run_script(int instance_id, const char* script_path);

#endif // SOULX_ANDROID_H
