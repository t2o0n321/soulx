/*
 * SoulX Android Emulator - 'soulx window <instance>': the window process
 *
 * soulx_window_run() parses the test-hook options, makes sure the instance runs, claims the instance's window
 * (run/<name>.window.lock + pid file run/<name>.window; a second window for the same instance brings the first
 * one to the front and exits 0), builds the application (Dock icon drawn in code, menus) and runs the AppKit
 * event loop until the window closes:
 *   - close button / Quit / Command-W: the instance stops gracefully, then the process exits
 *   - "Close Window (Keep Running)", SIGINT / SIGTERM / SIGHUP, --exit-after-ms: exits, the instance keeps running
 *   - the engine exits by itself: the window closes within a second
 * Exit codes: 0 ok, 1 failure (instance not running, stop failed, a test hook failed), 2 usage error.
 */

#import "window_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

#define WINDOW_EXIT_OK      0
#define WINDOW_EXIT_FAIL    1
#define WINDOW_EXIT_USAGE   2
#define HOOK_MS_MAX         3600000     // one hour
#define TAP_COORD_MAX       100000

// ---------------------------------------------------------------------------
// Localised text
// ---------------------------------------------------------------------------

typedef enum {
    LANG_EN = 0,
    LANG_HANT,
    LANG_HANS
} SoulXLang;

// Language tag (BCP 47, as in +[NSLocale preferredLanguages]) -> UI language.
static SoulXLang lang_for_tag(NSString* tag) {
    NSString* t = tag.lowercaseString;
    if (![t hasPrefix:@"zh"]) return LANG_EN;
    if ([t hasPrefix:@"zh-hant"] || [t hasPrefix:@"zh-tw"] || [t hasPrefix:@"zh-hk"] || [t hasPrefix:@"zh-mo"]) {
        return LANG_HANT;
    }
    return LANG_HANS;                           // zh, zh-Hans, zh-CN, zh-SG
}

static SoulXLang ui_lang(void) {
    static SoulXLang lang;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        // the user's choice (soulx language / the launcher), else the system language
        const char* chosen = soulx_ui_language();
        if (strcmp(chosen, "system") != 0) {
            lang = lang_for_tag(@(chosen));
            return;
        }
        NSString* first = [NSLocale preferredLanguages].firstObject;
        lang = lang_for_tag(first ? first : @"en");
    });
    return lang;
}

NSString* SoulXText(NSString* english) {
    static NSDictionary<NSString*, NSArray<NSString*>*>* table;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        // english : @[ Traditional Chinese, Simplified Chinese ]
        table = @{
            // toolbar
            @"Back": @[ @"返回", @"返回" ],
            @"Home": @[ @"首頁", @"首页" ],
            @"Recents": @[ @"多工", @"多任务" ],
            @"Rotate": @[ @"旋轉", @"旋转" ],
            @"Record script": @[ @"錄製腳本", @"录制脚本" ],
            @"Run script": @[ @"執行腳本", @"运行脚本" ],
            @"Import script": @[ @"匯入腳本", @"导入脚本" ],
            @"Import Script": @[ @"匯入腳本", @"导入脚本" ],
            @"Import": @[ @"匯入", @"导入" ],
            @"Choose a .lua script, or a folder containing a script and the files it needs.":
                @[ @"選擇一個 .lua 腳本，或包含腳本與其所需檔案的資料夾。", @"选择一个 .lua 脚本，或包含脚本与其所需文件的文件夹。" ],
            @"Imported. Use Run script to run it.": @[ @"已匯入。用「執行腳本」來執行。", @"已导入。用「运行脚本」来运行。" ],
            @"Imported %d items.": @[ @"已匯入 %d 個項目。", @"已导入 %d 个项目。" ],
            @"Could not create the scripts folder.": @[ @"無法建立腳本資料夾。", @"无法创建脚本文件夹。" ],
            @"Nothing was imported.": @[ @"沒有匯入任何項目。", @"没有导入任何项目。" ],
            @"Import script…": @[ @"匯入腳本…", @"导入脚本…" ],
            @"Browse…": @[ @"瀏覽…", @"浏览…" ],
            @"No named scripts yet.": @[ @"尚無已命名的腳本。", @"尚无已命名的脚本。" ],
            @"No scripts yet.\nImport one to get started.":
                @[ @"尚無腳本。\n先匯入一個開始吧。", @"尚无脚本。\n先导入一个开始吧。" ],
            @"This script has no name — add a line like  -- name: My Script  at the top.":
                @[ @"這個腳本沒有名稱——請在開頭加一行  -- name: 腳本名稱", @"这个脚本没有名称——请在开头加一行  -- name: 脚本名称" ],
            @"Screenshot": @[ @"截圖", @"截图" ],
            @"Paste": @[ @"貼上", @"粘贴" ],
            @"The clipboard has no text.": @[ @"剪貼簿沒有文字。", @"剪贴板没有文字。" ],
            @"Only ASCII text can be pasted for now.": @[ @"目前只能貼上 ASCII 文字（英數與符號）。", @"目前只能粘贴 ASCII 文本（英数与符号）。" ],
            @"Pasted.": @[ @"已貼上。", @"已粘贴。" ],
            @"Keymap": @[ @"遊戲鍵位", @"游戏键位" ],
            @"Stop": @[ @"停止", @"停止" ],
            @"Editing…": @[ @"編輯中…", @"编辑中…" ],
            @"Back (Esc or right-click)": @[ @"返回（Esc 或按右鍵）", @"返回（Esc 或右键）" ],
            @"Recent apps": @[ @"最近使用的 App", @"最近使用的应用" ],
            @"Rotate the screen": @[ @"旋轉畫面", @"旋转画面" ],
            @"Record your taps as a Lua script (⌘R)": @[ @"把你的操作錄成 Lua 腳本（⌘R）", @"把你的操作录成 Lua 脚本（⌘R）" ],
            @"Run a Lua script (⌘O)": @[ @"執行 Lua 腳本（⌘O）", @"运行 Lua 脚本（⌘O）" ],
            @"Save a screenshot to Pictures/SoulX (⌘S)": @[ @"截圖存到「圖片/SoulX」（⌘S）", @"截图保存到“图片/SoulX”（⌘S）" ],
            @"Edit the game keymap (⌘K)": @[ @"編輯遊戲鍵位（⌘K）", @"编辑游戏键位（⌘K）" ],
            @"Stop recording (⌘R)": @[ @"停止錄製（⌘R）", @"停止录制（⌘R）" ],
            @"Stop the script": @[ @"停止腳本", @"停止脚本" ],
            // status bar and notices
            @"● Recording %@": @[ @"● 錄製中 %@", @"● 录制中 %@" ],
            @"▶ Script %@": @[ @"▶ 腳本執行中 %@", @"▶ 脚本运行中 %@" ],
            @"Slow display mode": @[ @"慢速顯示模式", @"慢速显示模式" ],
            @"Slow display mode: the engine shares no frames.": @[ @"慢速顯示模式：引擎沒有提供共享畫面。",
                                                                   @"慢速显示模式：引擎没有提供共享画面。" ],
            @"Show in Finder": @[ @"在 Finder 中顯示", @"在访达中显示" ],
            @"Shutting down… %lds": @[ @"正在關機… %ld 秒", @"正在关机… %ld 秒" ],
            @"Booting %@… %lds": @[ @"正在啟動 %@… %ld 秒", @"正在启动 %@… %ld 秒" ],
            @"Connecting to the screen…": @[ @"正在連接畫面…", @"正在连接画面…" ],
            @"Keymap: %@ (%d keys)": @[ @"鍵位：%@（%d 個鍵）", @"键位：%@（%d 个键）" ],
            @"Keymap: default": @[ @"鍵位：預設", @"键位：默认" ],
            @"No screen to capture yet.": @[ @"還沒有畫面可以截圖。", @"还没有画面可以截图。" ],
            @"The screenshot could not be saved.": @[ @"截圖無法儲存。", @"截图无法保存。" ],
            @"Saved %@": @[ @"已儲存 %@", @"已保存 %@" ],
            // record / run
            @"Script output · %s": @[ @"腳本輸出 · %s", @"脚本输出 · %s" ],
            @"Cannot find the soulx program.": @[ @"找不到 soulx 程式。", @"找不到 soulx 程序。" ],
            @"The script could not be started.": @[ @"無法啟動腳本。", @"无法启动脚本。" ],
            @"Script stopped.": @[ @"腳本已停止。", @"脚本已停止。" ],
            @"Script finished.": @[ @"腳本執行完畢。", @"脚本运行完毕。" ],
            @"Script failed (exit status %d).": @[ @"腳本失敗（結束代碼 %d）。", @"脚本失败（退出代码 %d）。" ],
            @"Recording failed (exit status %d).": @[ @"錄製失敗（結束代碼 %d）。", @"录制失败（退出代码 %d）。" ],
            @"Save Recording": @[ @"儲存錄製的腳本", @"保存录制的脚本" ],
            @"Recording discarded.": @[ @"已捨棄錄製內容。", @"已舍弃录制内容。" ],
            @"Cannot save the recording: %@": @[ @"無法儲存錄製內容：%@", @"无法保存录制内容：%@" ],
            @"Recording saved: %@": @[ @"已儲存錄製的腳本：%@", @"已保存录制的脚本：%@" ],
            @"Cannot create a temporary file.": @[ @"無法建立暫存檔。", @"无法创建临时文件。" ],
            @"Recording… click Record again to stop.": @[ @"錄製中⋯⋯再按一次「錄製」即可停止。",
                                                          @"录制中……再按一次“录制”即可停止。" ],
            @"Run Script": @[ @"執行腳本", @"运行脚本" ],
            @"Running %@…": @[ @"正在執行 %@…", @"正在运行 %@…" ],
            // keymap editor
            @"Add joystick": @[ @"加入搖桿", @"添加摇杆" ],
            @"All games": @[ @"所有遊戲", @"所有游戏" ],
            @"all games": @[ @"所有遊戲", @"所有游戏" ],
            @"Cancel": @[ @"取消", @"取消" ],
            @"Done": @[ @"完成", @"完成" ],
            @"Keymap: %@": @[ @"鍵位：%@", @"键位：%@" ],
            @"Click an empty spot, then press a key. Drag to move, right-click for more.":
                @[ @"點一下空白處再按一個鍵；拖曳可移動，按右鍵有更多選項。", @"点一下空白处再按一个键；拖动可移动，右键有更多选项。" ],
            @"Press a key…": @[ @"請按一個鍵…", @"请按一个键…" ],
            @"The keymap could not be saved.": @[ @"鍵位無法儲存。", @"键位无法保存。" ],
            @"See the window log for details.": @[ @"詳情請看視窗的記錄檔。", @"详情请看窗口的日志。" ],
            @"Add joystick here": @[ @"在這裡加入搖桿", @"在这里添加摇杆" ],
            @"Delete": @[ @"刪除", @"删除" ],
            @"Change key": @[ @"更換按鍵", @"更换按键" ],
            @"Make joystick (WASD)": @[ @"改成搖桿（WASD）", @"改成摇杆（WASD）" ],
            @"Use arrow keys": @[ @"使用方向鍵", @"使用方向键" ],
            @"Use WASD": @[ @"使用 WASD", @"使用 WASD" ],
            @"Make tap": @[ @"改成點擊", @"改成点击" ],
            @"Right-click a binding to delete it.": @[ @"在鍵位上按右鍵即可刪除。", @"在键位上右键即可删除。" ],
            @"This key cannot be used in a keymap.": @[ @"這個鍵不能用在鍵位裡。", @"这个键不能用在键位里。" ],
            @"Key %@ is already used.": @[ @"按鍵 %@ 已經用過了。", @"按键 %@ 已经用过了。" ],
            @"The keymap is full.": @[ @"鍵位已滿。", @"键位已满。" ],
            // menus
            @"About SoulX": @[ @"關於 SoulX", @"关于 SoulX" ],
            @"Hide SoulX": @[ @"隱藏 SoulX", @"隐藏 SoulX" ],
            @"Hide Others": @[ @"隱藏其他", @"隐藏其他" ],
            @"Show All": @[ @"顯示全部", @"全部显示" ],
            @"Quit SoulX": @[ @"結束 SoulX", @"退出 SoulX" ],
            @"View": @[ @"顯示方式", @"显示" ],
            @"Actual Size": @[ @"實際大小", @"实际大小" ],
            @"Fit to Screen": @[ @"符合螢幕", @"适合屏幕" ],
            @"Enter Full Screen": @[ @"進入全螢幕", @"进入全屏幕" ],
            @"Inspect (x, y, colour)": @[ @"檢視座標／顏色", @"查看坐标／颜色" ],
            @"Inspect": @[ @"檢視座標", @"查看坐标" ],
            @"Read x, y and colour; click to copy (⇧⌘I)": @[ @"讀取座標與顏色，點一下複製（⇧⌘I）", @"读取坐标与颜色，点一下复制（⇧⌘I）" ],
            @"Inspect: hover to read x, y and colour; click to copy": @[ @"檢視模式：移到畫面上看座標與顏色，點一下複製", @"查看模式：移到画面上看坐标与颜色，点一下复制" ],
            @"Inspect off": @[ @"檢視模式關閉", @"查看模式关闭" ],
            @"Copied: %@": @[ @"已複製：%@", @"已复制：%@" ],
            @"Hide Toolbar": @[ @"收合工具列", @"收起工具栏" ],
            @"Show Toolbar": @[ @"展開工具列", @"展开工具栏" ],
            @"Hide toolbar": @[ @"收合工具列", @"收起工具栏" ],
            @"Show toolbar": @[ @"展開工具列", @"展开工具栏" ],
            @"Show Log": @[ @"顯示執行紀錄", @"显示运行日志" ],
            @"This app stays in one orientation.": @[ @"這個 App 固定單一方向，無法旋轉。", @"这个应用固定单一方向，无法旋转。" ],
            @"Recording video… (Automation ▸ Stop Video to finish)": @[ @"錄製影片中…（自動化 ▸ 停止錄影 以結束）", @"录制视频中…（自动化 ▸ 停止录像 结束）" ],
            @"Saving video…": @[ @"儲存影片中…", @"保存视频中…" ],
            @"Could not start the recording.": @[ @"無法開始錄影。", @"无法开始录像。" ],
            @"Device": @[ @"裝置", @"设备" ],
            @"Automation": @[ @"自動化", @"自动化" ],
            @"Record": @[ @"錄製", @"录制" ],
            @"Stop Recording": @[ @"停止錄製", @"停止录制" ],
            @"Run Script…": @[ @"執行腳本…", @"运行脚本…" ],
            @"Record Video": @[ @"錄製影片", @"录制视频" ],
            @"Stop Video": @[ @"停止錄影", @"停止录像" ],
            @"Volume": @[ @"音量", @"音量" ],
            @"Portrait": @[ @"直向", @"竖屏" ],
            @"Landscape": @[ @"橫向", @"横屏" ],
            @"Navigation": @[ @"導覽", @"导览" ],
            @"Tools": @[ @"工具", @"工具" ],
            @"Output volume": @[ @"輸出音量", @"输出音量" ],
            @"Apps": @[ @"應用", @"应用" ],
            @"Live": @[ @"已連線", @"已连接" ],
            @"Connected. Android only draws when something changes, so a still screen shows no frame rate.": @[ @"已連線。Android 只在畫面有變化時才繪製，所以畫面靜止時不顯示幀率。", @"已连接。Android 只在画面有变化时才绘制，所以画面静止时不显示帧率。" ],
            @"Scripts": @[ @"腳本", @"脚本" ],
            @"Capture": @[ @"擷取", @"截取" ],
            @"Script parameters": @[ @"腳本參數", @"脚本参数" ],
            @"Install APK": @[ @"安裝 APK", @"安装 APK" ],
            @"Install app": @[ @"安裝應用程式", @"安装应用" ],
            @"Install an app (.apk/.xapk/.apks/.apkm), or drag one onto the window": @[ @"安裝應用程式（.apk/.xapk/.apks/.apkm），或把檔案拖進視窗", @"安装应用（.apk/.xapk/.apks/.apkm），或把文件拖进窗口" ],
            @"Drop an APK to install, or a file to push to the guest": @[ @"放開以安裝 APK，其他檔案推送到裝置的 Downloads", @"松开安装 APK，其他文件推送到设备的 Downloads" ],
            @"Busy — wait for the current task to finish.": @[ @"忙碌中——請等目前的工作結束。", @"忙碌中——请等当前任务结束。" ],
            @"Installing %@…": @[ @"安裝 %@…", @"安装 %@…" ],
            @"Pushing %@…": @[ @"推送 %@…", @"推送 %@…" ],
            @"Done.": @[ @"完成。", @"完成。" ],
            @"Cancelled.": @[ @"已取消。", @"已取消。" ],
            @"Failed (exit status %d).": @[ @"失敗（結束碼 %d）。", @"失败（退出码 %d）。" ],
            @"Record video": @[ @"錄影", @"录屏" ],
            @"Stop video": @[ @"停止錄影", @"停止录屏" ],
            @"Record the screen to a video (⇧⌘E)": @[ @"錄製螢幕影片（⇧⌘E）", @"录制屏幕视频（⇧⌘E）" ],
            @"Stop recording the video": @[ @"停止錄影", @"停止录屏" ],
            @"Edit": @[ @"編輯", @"编辑" ],
            @"Show Bindings": @[ @"顯示鍵位", @"显示键位" ],
            @"Window": @[ @"視窗", @"窗口" ],
            @"Minimize": @[ @"縮到最小", @"最小化" ],
            @"Close Window (Keep Running)": @[ @"關閉視窗（保持執行）", @"关闭窗口（保持运行）" ],
            @"Close and Stop": @[ @"關閉並停止", @"关闭并停止" ],
            @"Bring All to Front": @[ @"將此程式所有視窗移至最前", @"前置全部窗口" ],
            @"Android 17 emulator for your games": @[ @"玩遊戲用的 Android 17 模擬器", @"玩游戏用的 Android 17 模拟器" ],
        };
    });
    SoulXLang lang = ui_lang();
    if (lang == LANG_EN) return english;
    NSArray<NSString*>* t = table[english];
    if (!t) return english;
    return t[lang == LANG_HANT ? 0 : 1];
}

// ---------------------------------------------------------------------------
// App icon: rounded square, gradient, "S"
// ---------------------------------------------------------------------------

NSImage* SoulXAppIcon(void) {
    static NSImage* icon;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        icon = [NSImage imageWithSize:NSMakeSize(512, 512) flipped:NO drawingHandler:^BOOL(NSRect rect) {
            NSRect body = NSInsetRect(rect, rect.size.width * 0.09, rect.size.height * 0.09);
            CGFloat radius = body.size.width * 0.225;
            NSBezierPath* shape = [NSBezierPath bezierPathWithRoundedRect:body xRadius:radius yRadius:radius];
            [NSGraphicsContext saveGraphicsState];
            NSShadow* shadow = [[NSShadow alloc] init];
            shadow.shadowColor = [NSColor colorWithWhite:0 alpha:0.35];
            shadow.shadowOffset = NSMakeSize(0, -rect.size.height * 0.012);
            shadow.shadowBlurRadius = rect.size.width * 0.03;
            [shadow set];
            [[NSColor blackColor] setFill];
            [shape fill];
            [NSGraphicsContext restoreGraphicsState];
            NSGradient* g = [[NSGradient alloc] initWithColors:@[
                [NSColor colorWithSRGBRed:0.42 green:0.30 blue:0.98 alpha:1],
                [NSColor colorWithSRGBRed:0.13 green:0.62 blue:0.98 alpha:1],
                [NSColor colorWithSRGBRed:0.18 green:0.86 blue:0.74 alpha:1],
            ]];
            [g drawInBezierPath:shape angle:-60];
            NSFont* font = [NSFont systemFontOfSize:body.size.height * 0.62 weight:NSFontWeightHeavy];
            NSFontDescriptor* rounded = [font.fontDescriptor fontDescriptorWithDesign:NSFontDescriptorSystemDesignRounded];
            NSFont* round_font = rounded ? [NSFont fontWithDescriptor:rounded size:font.pointSize] : nil;
            if (round_font) font = round_font;
            NSShadow* glow = [[NSShadow alloc] init];
            glow.shadowColor = [NSColor colorWithWhite:0 alpha:0.25];
            glow.shadowOffset = NSMakeSize(0, -rect.size.height * 0.008);
            glow.shadowBlurRadius = rect.size.width * 0.02;
            NSDictionary* attrs = @{ NSFontAttributeName: font, NSForegroundColorAttributeName: NSColor.whiteColor,
                                     NSShadowAttributeName: glow };
            NSAttributedString* s = [[NSAttributedString alloc] initWithString:@"S" attributes:attrs];
            NSSize ts = s.size;
            [s drawAtPoint:NSMakePoint(NSMidX(body) - ts.width / 2, NSMidY(body) - ts.height / 2)];
            return YES;
        }];
    });
    return icon;
}

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

static void print_usage(void) {
    printf("usage: soulx window [NAME] [options]\n"
           "Opens SoulX's window of a running instance (one window per instance).\n"
           "Test options (the window closes afterwards; the instance keeps running):\n"
           "  --dump-frame FILE    write what the window shows as a PNG, after --after-ms N (default %d)\n"
           "                       counted from the first frame\n"
           "  --simulate-tap X Y   click the screen at logical pixel X, Y after the first frame\n"
           "  --simulate-key KEY   press and release a key (A, ENTER, SPACE, UP, F1, ...) after the first frame\n"
           "  --stats-after-ms N   print fps / uploads / dropped / render times N ms after the first frame\n"
           "  --exit-after-ms N    close the window after N ms\n",
           SOULX_WINDOW_DUMP_AFTER_MS);
}

static bool parse_number(const char* s, long lo, long hi, int* out) {
    if (!s || !*s) return false;
    char* end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || *end != '\0' || v < lo || v > hi) return false;
    *out = (int)v;
    return true;
}

// Accepts the hook options. Leading words that are not options (the program, "window", the instance) are
// skipped, a later word must be the instance itself, and the global -v / -i NAME options are tolerated.
// 0 = run, 1 = help printed, -1 = usage error (reported).
static int parse_options(const char* instance, int argc, char** argv, SoulXWindowOptions* o) {
    memset(o, 0, sizeof(*o));
    o->after_ms = -1;
    int i = 0;
    while (i < argc && argv[i] && argv[i][0] != '-') i++;
    for (; i < argc && argv[i]; i++) {
        const char* a = argv[i];
        if (a[0] != '-') {
            if (instance && strcmp(a, instance) == 0) continue;
            soulx_log(SOULX_LOG_ERROR, "unexpected argument '%.64s' for 'soulx window'", a);
            return -1;
        }
        const char* val = NULL;
        char opt[32];
        const char* eq = strchr(a, '=');
        if (strncmp(a, "--", 2) == 0 && eq) {
            size_t n = (size_t)(eq - a);
            if (n >= sizeof(opt)) {
                soulx_log(SOULX_LOG_ERROR, "unknown option '%.64s' for 'soulx window'", a);
                return -1;
            }
            memcpy(opt, a, n);
            opt[n] = '\0';
            val = eq + 1;
            a = opt;
        }
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            print_usage();
            return 1;
        }
        if (strcmp(a, "-v") == 0 || strcmp(a, "--verbose") == 0) {
            soulx_set_verbose(true);
            continue;
        }
        if (strcmp(a, "--test-screenshot") == 0) {
            o->test_screenshot = true;
            continue;
        }
        if (strcmp(a, "--test-rotate") == 0) {
            o->test_rotate = true;
            continue;
        }
        if (strcmp(a, "--simulate-tap") == 0) {
            if (val || i + 2 >= argc || !parse_number(argv[i + 1], 0, TAP_COORD_MAX, &o->tap_x) ||
                !parse_number(argv[i + 2], 0, TAP_COORD_MAX, &o->tap_y)) {
                soulx_log(SOULX_LOG_ERROR, "--simulate-tap needs X and Y in screen pixels, e.g. --simulate-tap 360 800");
                return -1;
            }
            o->tap = true;
            i += 2;
            continue;
        }
        bool known = strcmp(a, "--dump-frame") == 0 || strcmp(a, "--after-ms") == 0 ||
                     strcmp(a, "--simulate-key") == 0 || strcmp(a, "--stats-after-ms") == 0 ||
                     strcmp(a, "--exit-after-ms") == 0 || strcmp(a, "-i") == 0 || strcmp(a, "--instance") == 0;
        if (!known) {
            soulx_log(SOULX_LOG_ERROR, "unknown option '%.64s' for 'soulx window' (see: soulx window --help)", argv[i]);
            return -1;
        }
        if (!val) {
            if (i + 1 >= argc || !argv[i + 1]) {
                soulx_log(SOULX_LOG_ERROR, "option '%s' needs a value", a);
                return -1;
            }
            val = argv[++i];
        }
        if (strcmp(a, "--dump-frame") == 0) {
            if (!val[0]) {
                soulx_log(SOULX_LOG_ERROR, "--dump-frame needs a file name");
                return -1;
            }
            o->dump_frame = val;
        } else if (strcmp(a, "--after-ms") == 0) {
            if (!parse_number(val, 0, HOOK_MS_MAX, &o->after_ms)) {
                soulx_log(SOULX_LOG_ERROR, "--after-ms needs a number of milliseconds (0-%d), not '%.32s'", HOOK_MS_MAX, val);
                return -1;
            }
        } else if (strcmp(a, "--stats-after-ms") == 0 || strcmp(a, "--exit-after-ms") == 0) {
            int* dst = strcmp(a, "--stats-after-ms") == 0 ? &o->stats_after_ms : &o->exit_after_ms;
            if (!parse_number(val, 1, HOOK_MS_MAX, dst)) {
                soulx_log(SOULX_LOG_ERROR, "%s needs a number of milliseconds (1-%d), not '%.32s'", a, HOOK_MS_MAX, val);
                return -1;
            }
        } else if (strcmp(a, "--simulate-key") == 0) {
            if (soulx_key_from_name(val) < 0) {
                soulx_log(SOULX_LOG_ERROR, "--simulate-key: unknown key '%.32s' (e.g. A, ENTER, SPACE, UP, F1)", val);
                return -1;
            }
            o->key = val;
        }
        // -i / --instance: the instance is soulx_window_run's parameter
    }
    if (o->after_ms >= 0 && !o->dump_frame) {
        soulx_log(SOULX_LOG_ERROR, "--after-ms only applies to --dump-frame");
        return -1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// One window process per instance
//
// run/<name>.window.lock is flock()ed for the whole life of the window process: the kernel drops the lock however
// the process ends, so a crashed window never blocks the next one. run/<name>.window holds the pid for
// soulx_window_running() (launch.c); it is written under a private name and renamed into place, so readers never
// see it half written, and removed again on exit. The lock file itself stays (unlinking a lock file races).
// ---------------------------------------------------------------------------

#define WINDOW_CLAIM_WAIT_MS    1500        // a window that holds the lock writes its pid file right away
#define WINDOW_CLAIM_POLL_MS    50

static NSString* const kActivateNotification = @"com.soulx.window.activate";   // object: instance name
// object: instance name, userInfo: {"path": the script's full path} (the launcher's Run button)
static NSString* const kRunNotification = @"com.soulx.window.run";
static NSString* const kStopNotification = @"com.soulx.window.stop";           // object: instance name (Stop button)

static int s_lock_fd = -1;
static char s_pid_path[PATH_MAX];

static pid_t pidfile_read(const char* path) {
    char* data = NULL;
    size_t len = 0;
    if (soulx_read_file(path, &data, &len) != 0) return 0;
    const char* p = data;
    if (strncmp(p, "pid=", 4) == 0) p += 4;
    char* end = NULL;
    long v = strtol(p, &end, 10);
    bool ok = end != p && v > 1 && v < INT32_MAX;
    free(data);
    return ok ? (pid_t)v : 0;
}

// 0 = this process is now the window of the instance, 1 = another window has it (*other = its pid, 0 when it
// cannot be told), -1 = error (reported).
static int window_claim(const char* name, pid_t* other) {
    *other = 0;
    char dir[PATH_MAX], lock_path[PATH_MAX], tmp[PATH_MAX];
    if (soulx_path(dir, sizeof(dir), "run") != 0 ||
        soulx_path(lock_path, sizeof(lock_path), "run/%s.window.lock", name) != 0 ||
        soulx_path(s_pid_path, sizeof(s_pid_path), "run/%s.window", name) != 0 ||
        soulx_path(tmp, sizeof(tmp), "run/.%s.window.%d", name, (int)getpid()) != 0) {
        soulx_log(SOULX_LOG_ERROR, "SoulX home path too long: %s", soulx_home());
        s_pid_path[0] = '\0';
        return -1;
    }
    if (soulx_mkdirs(dir) != 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot create %s: %s", dir, strerror(errno));
        s_pid_path[0] = '\0';
        return -1;
    }
    int fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) {
        soulx_log(SOULX_LOG_ERROR, "cannot open %s: %s", lock_path, strerror(errno));
        s_pid_path[0] = '\0';
        return -1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int e = errno;
        close(fd);
        s_pid_path[0] = '\0';
        if (e != EWOULDBLOCK) {
            soulx_log(SOULX_LOG_ERROR, "cannot lock %s: %s", lock_path, strerror(e));
            return -1;
        }
        for (int waited = 0; waited <= WINDOW_CLAIM_WAIT_MS; waited += WINDOW_CLAIM_POLL_MS) {
            pid_t pid = 0;
            if (soulx_window_running(name, &pid) && pid > 0) {
                *other = pid;
                break;
            }
            usleep(WINDOW_CLAIM_POLL_MS * 1000);
        }
        return 1;
    }

    // The lock is ours, so any pid file left behind is stale: replace it.
    char text[32];
    int n = snprintf(text, sizeof(text), "%d\n", (int)getpid());
    int pfd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    bool ok = pfd >= 0 && n > 0 && (size_t)n < sizeof(text) && write(pfd, text, (size_t)n) == n;
    int e = errno;
    if (pfd >= 0 && close(pfd) != 0) ok = false;
    if (ok && rename(tmp, s_pid_path) != 0) {
        e = errno;
        ok = false;
    }
    if (!ok) {
        (void)unlink(tmp);
        soulx_log(SOULX_LOG_ERROR, "cannot write %s: %s", s_pid_path, strerror(e));
        close(fd);
        s_pid_path[0] = '\0';
        return -1;
    }
    s_lock_fd = fd;
    return 0;
}

static void window_release(void) {
    if (s_lock_fd < 0) return;
    if (s_pid_path[0] && pidfile_read(s_pid_path) == getpid()) (void)unlink(s_pid_path);
    close(s_lock_fd);                           // drops the lock
    s_lock_fd = -1;
}

// Brings the window of another process to the front: it listens for kActivateNotification (an activation
// requested by a background process may be refused by macOS 14+, the window can always order itself front).
static void activate_other(const char* name, pid_t pid) {
    [[NSDistributedNotificationCenter defaultCenter] postNotificationName:kActivateNotification object:@(name)
                                                                 userInfo:nil deliverImmediately:YES];
    if (pid <= 0) return;
    NSRunningApplication* app = [NSRunningApplication runningApplicationWithProcessIdentifier:pid];
    if (!app) return;
    if (@available(macOS 14.0, *)) {
        [app activateWithOptions:NSApplicationActivateAllWindows];
    } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        [app activateWithOptions:NSApplicationActivateAllWindows | NSApplicationActivateIgnoringOtherApps];
#pragma clang diagnostic pop
    }
}

// ---------------------------------------------------------------------------
// Signals: SIGINT / SIGTERM / SIGHUP close the window, the instance keeps running. They are taken over before
// anything else happens: a CLI handler setting g_soulx_interrupted would make every input.c call fail.
// ---------------------------------------------------------------------------

static const int k_signals[] = { SIGINT, SIGTERM, SIGHUP };
#define SIGNAL_COUNT ((int)(sizeof(k_signals) / sizeof(k_signals[0])))

static dispatch_source_t s_signal_sources[SIGNAL_COUNT];
static __weak SoulXWindowController* s_controller;

static void watch_signals(void) {
    for (int i = 0; i < SIGNAL_COUNT; i++) {
        int sig = k_signals[i];
        signal(sig, SIG_IGN);                   // delivered to the dispatch source instead
        dispatch_source_t s = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, (uintptr_t)sig, 0,
                                                     dispatch_get_main_queue());
        if (!s) continue;
        dispatch_source_set_event_handler(s, ^{
            soulx_log(SOULX_LOG_INFO, "signal %d: closing the window (the instance keeps running)", sig);
            [s_controller closeKeepRunning:nil];
        });
        dispatch_resume(s);
        s_signal_sources[i] = s;
    }
    g_soulx_interrupted = 0;
}

static void unwatch_signals(void) {
    for (int i = 0; i < SIGNAL_COUNT; i++) {
        if (s_signal_sources[i]) dispatch_source_cancel(s_signal_sources[i]);
        s_signal_sources[i] = nil;
    }
}

// ---------------------------------------------------------------------------
// Application delegate
// ---------------------------------------------------------------------------

@interface SoulXAppDelegate : NSObject <NSApplicationDelegate>
- (instancetype)initWithController:(SoulXWindowController*)controller enginePid:(pid_t)pid instance:(const char*)name;
@property (nonatomic, readonly) int exitCode;
@end

@implementation SoulXAppDelegate {
    SoulXWindowController* _controller;
    pid_t _enginePid;
    NSString* _instance;
    NSMutableArray* _sources;   // engine watch
    BOOL _finished;
    BOOL _replyPending;         // applicationShouldTerminate: answered NSTerminateLater
}

- (instancetype)initWithController:(SoulXWindowController*)controller enginePid:(pid_t)pid instance:(const char*)name {
    self = [super init];
    if (!self) return nil;
    _controller = controller;
    _enginePid = pid;
    _instance = @(name);
    _exitCode = WINDOW_EXIT_FAIL;
    _sources = [NSMutableArray array];
    __weak SoulXAppDelegate* weakSelf = self;
    controller.onFinish = ^(int code) {
        [weakSelf finishWithCode:code];
    };
    return self;
}

- (void)finishWithCode:(int)code {
    if (_finished) return;
    _finished = YES;
    _exitCode = code;
    for (dispatch_source_t s in _sources) dispatch_source_cancel(s);
    [_sources removeAllObjects];
    [[NSDistributedNotificationCenter defaultCenter] removeObserver:self];
    if (_replyPending) {
        [NSApp replyToApplicationShouldTerminate:YES];     // AppKit ends the process (applicationWillTerminate:)
        return;
    }
    [NSApp stop:nil];
    // -stop: takes effect after the next event: post one
    NSEvent* wake = [NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint modifierFlags:0
                                      timestamp:0 windowNumber:0 context:nil subtype:0 data1:0 data2:0];
    [NSApp postEvent:wake atStart:YES];
}

- (void)watchEngine {
    __weak SoulXWindowController* c = _controller;
    pid_t pid = _enginePid;
    dispatch_source_t exitSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_PROC, (uintptr_t)pid,
                                                          DISPATCH_PROC_EXIT, dispatch_get_main_queue());
    if (exitSource) {
        dispatch_source_set_event_handler(exitSource, ^{
            [c engineDidExit];
        });
        dispatch_resume(exitSource);
        [_sources addObject:exitSource];
    }
    // a second opinion once a second (a process source cannot watch a process that is already gone)
    dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    if (timer) {
        dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC,
                                  NSEC_PER_SEC / 10);
        dispatch_source_set_event_handler(timer, ^{
            if (!soulx_pid_alive(pid)) [c engineDidExit];
        });
        dispatch_resume(timer);
        [_sources addObject:timer];
    }
}

- (void)activateRequest:(NSNotification*)note {
    (void)note;
    [_controller bringToFront];
}

- (void)runRequest:(NSNotification*)note {
    NSString* path = note.userInfo[@"path"];
    if (path.length) [_controller requestRun:path];
}

- (void)stopRequest:(NSNotification*)note {
    (void)note;
    [_controller requestStop];
}

- (void)applicationDidFinishLaunching:(NSNotification*)note {
    (void)note;
    [self watchEngine];
    [[NSDistributedNotificationCenter defaultCenter] addObserver:self selector:@selector(activateRequest:)
                                                            name:kActivateNotification object:_instance];
    [[NSDistributedNotificationCenter defaultCenter] addObserver:self selector:@selector(runRequest:)
                                                            name:kRunNotification object:_instance];
    [[NSDistributedNotificationCenter defaultCenter] addObserver:self selector:@selector(stopRequest:)
                                                            name:kStopNotification object:_instance];
    [_controller showWindow:nil];
    if (@available(macOS 14.0, *)) {
        [NSApp activate];
    } else {
        [NSApp activateIgnoringOtherApps:YES];
    }
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)app {
    (void)app;
    if (_finished) return NSTerminateNow;
    // Dock "Quit", logout, an Apple event: stop the instance first, then let AppKit end the process
    _replyPending = YES;
    [_controller closeAndStop:nil];
    return NSTerminateLater;
}

- (void)applicationWillTerminate:(NSNotification*)note {
    (void)note;
    window_release();                           // AppKit exits without returning from -run
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)app {
    (void)app;
    return NO;
}

- (BOOL)applicationShouldHandleReopen:(NSApplication*)app hasVisibleWindows:(BOOL)visible {
    (void)app;
    (void)visible;
    [_controller bringToFront];
    return NO;
}

- (BOOL)applicationSupportsSecureRestorableState:(NSApplication*)app {
    (void)app;
    return YES;
}

- (void)about:(id)sender {
    (void)sender;
    NSDictionary* opts = @{
        NSAboutPanelOptionApplicationName: @"SoulX",
        NSAboutPanelOptionApplicationVersion: @SOULX_VERSION_STRING,
        NSAboutPanelOptionVersion: @"",
        NSAboutPanelOptionApplicationIcon: SoulXAppIcon(),
        NSAboutPanelOptionCredits: [[NSAttributedString alloc]
            initWithString:SoulXText(@"Android 17 emulator for your games")
                attributes:@{ NSFontAttributeName: [NSFont systemFontOfSize:11],
                              NSForegroundColorAttributeName: NSColor.secondaryLabelColor }],
    };
    [NSApp orderFrontStandardAboutPanelWithOptions:opts];
}

@end

// ---------------------------------------------------------------------------
// Menus
// ---------------------------------------------------------------------------

static NSMenuItem* add_item(NSMenu* menu, NSString* title, SEL action, id target, NSString* key,
                            NSEventModifierFlags mods) {
    NSMenuItem* item = [menu addItemWithTitle:title action:action keyEquivalent:key ? key : @""];
    item.target = target;
    item.keyEquivalentModifierMask = mods;
    return item;
}

static NSMenu* add_menu(NSMenu* bar, NSString* title) {
    NSMenuItem* holder = [bar addItemWithTitle:title action:NULL keyEquivalent:@""];
    NSMenu* m = [[NSMenu alloc] initWithTitle:title];
    holder.submenu = m;
    return m;
}

static void build_menus(SoulXAppDelegate* app, SoulXWindowController* c) {
    const NSEventModifierFlags cmd = NSEventModifierFlagCommand;
    NSMenu* bar = [[NSMenu alloc] initWithTitle:@""];

    NSMenu* soulx = add_menu(bar, @"SoulX");
    add_item(soulx, SoulXText(@"About SoulX"), @selector(about:), app, nil, 0);
    [soulx addItem:[NSMenuItem separatorItem]];
    add_item(soulx, SoulXText(@"Hide SoulX"), @selector(hide:), nil, @"h", cmd);
    add_item(soulx, SoulXText(@"Hide Others"), @selector(hideOtherApplications:), nil, @"h", cmd | NSEventModifierFlagOption);
    add_item(soulx, SoulXText(@"Show All"), @selector(unhideAllApplications:), nil, nil, 0);
    [soulx addItem:[NSMenuItem separatorItem]];
    add_item(soulx, SoulXText(@"Quit SoulX"), @selector(closeAndStop:), c, @"q", cmd);

    NSMenu* view = add_menu(bar, SoulXText(@"View"));
    add_item(view, SoulXText(@"Actual Size"), @selector(soulxActualSize:), c, @"0", cmd);
    add_item(view, SoulXText(@"Fit to Screen"), @selector(soulxFit:), c, @"9", cmd);
    add_item(view, SoulXText(@"Enter Full Screen"), @selector(toggleFullScreen:), nil, @"f",
             cmd | NSEventModifierFlagControl);
    [view addItem:[NSMenuItem separatorItem]];
    add_item(view, SoulXText(@"Hide Toolbar"), @selector(soulxToggleToolbar:), c, @"t", cmd | NSEventModifierFlagOption);
    add_item(view, SoulXText(@"Inspect (x, y, colour)"), @selector(soulxInspect:), c, @"i", cmd | NSEventModifierFlagShift);

    NSMenu* device = add_menu(bar, SoulXText(@"Device"));
    add_item(device, SoulXText(@"Back"), @selector(soulxBack:), c, @"\x1b", 0);
    add_item(device, SoulXText(@"Home"), @selector(soulxHome:), c, nil, 0);
    add_item(device, SoulXText(@"Recents"), @selector(soulxRecents:), c, nil, 0);
    add_item(device, SoulXText(@"Rotate"), @selector(soulxRotate:), c, nil, 0);
    [device addItem:[NSMenuItem separatorItem]];
    add_item(device, SoulXText(@"Screenshot"), @selector(soulxScreenshot:), c, @"s", cmd);
    add_item(device, SoulXText(@"Paste"), @selector(soulxPaste:), c, @"v", cmd);

    NSMenu* automation = add_menu(bar, SoulXText(@"Automation"));
    add_item(automation, SoulXText(@"Record"), @selector(soulxRecord:), c, @"r", cmd);
    add_item(automation, SoulXText(@"Run Script…"), @selector(soulxRunScript:), c, @"o", cmd);
    add_item(automation, SoulXText(@"Stop"), @selector(soulxStopScript:), c, @".", cmd);
    [automation addItem:[NSMenuItem separatorItem]];
    add_item(automation, SoulXText(@"Record Video"), @selector(soulxRecordVideo:), c, @"e", cmd | NSEventModifierFlagShift);
    add_item(automation, SoulXText(@"Show Log"), @selector(soulxShowLog:), c, @"l", cmd | NSEventModifierFlagShift);

    NSMenu* keymap = add_menu(bar, SoulXText(@"Keymap"));
    add_item(keymap, SoulXText(@"Edit"), @selector(soulxEditKeymap:), c, @"k", cmd);
    add_item(keymap, SoulXText(@"Show Bindings"), @selector(soulxShowBindings:), c, nil, 0);

    NSMenu* window = add_menu(bar, SoulXText(@"Window"));
    add_item(window, SoulXText(@"Minimize"), @selector(performMiniaturize:), nil, @"m", cmd);
    [window addItem:[NSMenuItem separatorItem]];
    add_item(window, SoulXText(@"Close Window (Keep Running)"), @selector(closeKeepRunning:), c, @"w",
             cmd | NSEventModifierFlagOption);
    add_item(window, SoulXText(@"Close and Stop"), @selector(closeAndStop:), c, @"w", cmd);
    [window addItem:[NSMenuItem separatorItem]];
    add_item(window, SoulXText(@"Bring All to Front"), @selector(arrangeInFront:), nil, nil, 0);

    NSApp.mainMenu = bar;
    NSApp.windowsMenu = window;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int soulx_window_run(const char* instance, int argc, char** argv) {
    SoulXWindowOptions opts;
    int parsed = parse_options(instance, argc, argv, &opts);
    if (parsed > 0) return WINDOW_EXIT_OK;
    if (parsed < 0) return WINDOW_EXIT_USAGE;

    SoulXInstanceConf conf;
    if (soulx_resolve_instance(instance, &conf) != 0) {
        soulx_log(SOULX_LOG_ERROR, "no instance '%.64s' (see: soulx list)",
                 instance && instance[0] ? instance : SOULX_DEFAULT_INSTANCE);
        return WINDOW_EXIT_FAIL;
    }
    SoulXRunState st;
    if (!soulx_instance_running(conf.name, &st)) {
        soulx_log(SOULX_LOG_ERROR, "'%s' is not running (start it with: soulx start %s)", conf.name, conf.name);
        return WINDOW_EXIT_FAIL;
    }

    signal(SIGPIPE, SIG_IGN);                   // a dead adb child must not kill the window
    pid_t other = 0;
    int claim = window_claim(conf.name, &other);
    if (claim < 0) return WINDOW_EXIT_FAIL;
    if (claim > 0) {
        if (other > 0) soulx_log(SOULX_LOG_INFO, "the window of '%s' is already open (pid %d)", conf.name, (int)other);
        else soulx_log(SOULX_LOG_INFO, "the window of '%s' is already open", conf.name);
        @autoreleasepool {
            activate_other(conf.name, other);
        }
        return WINDOW_EXIT_OK;
    }

    watch_signals();
    int code = WINDOW_EXIT_FAIL;
    @autoreleasepool {
        [NSProcessInfo processInfo].processName = @"SoulX";
        NSApplication* app = [NSApplication sharedApplication];
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        app.applicationIconImage = SoulXAppIcon();

        SoulXWindowController* controller = [[SoulXWindowController alloc] initWithConf:&conf state:&st options:&opts];
        if (controller) {
            s_controller = controller;
            SoulXAppDelegate* delegate = [[SoulXAppDelegate alloc] initWithController:controller enginePid:st.pid
                                                                           instance:conf.name];
            app.delegate = delegate;
            build_menus(delegate, controller);
            [app run];
            code = delegate.exitCode;
            app.delegate = nil;
            s_controller = nil;
        } else {
            soulx_log(SOULX_LOG_ERROR, "cannot open the window of '%s'", conf.name);
        }
    }
    unwatch_signals();
    window_release();
    return code;
}
