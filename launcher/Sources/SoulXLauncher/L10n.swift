import Foundation

/// UI text in Traditional Chinese, Simplified Chinese or English, following the system language (like SoulX's
/// window). Keys are the English text.
enum Lang { case zhHant, zhHans, en }

let currentLang: Lang = {
    let pref = Locale.preferredLanguages.first?.lowercased() ?? "en"
    if pref.hasPrefix("zh-hant") || pref.hasPrefix("zh-tw") || pref.hasPrefix("zh-hk") { return .zhHant }
    if pref.hasPrefix("zh") { return .zhHans }
    return .en
}()

private let table: [String: (String, String)] = [   // English key: (繁體, 简体)
    "Instances": ("實例", "实例"),
    "New instance": ("新增實例", "新增实例"),
    "Refresh": ("重新整理", "刷新"),
    "Running": ("執行中", "运行中"),
    "Stopped": ("已停止", "已停止"),
    "Start": ("啟動", "启动"),
    "Start in background": ("背景啟動（不開視窗）", "后台启动（不开窗口）"),
    "Stop": ("停止", "停止"),
    "Restart": ("重新啟動", "重新启动"),
    "Open window": ("開啟視窗", "打开窗口"),
    "Delete…": ("刪除…", "删除…"),
    "Delete": ("刪除", "删除"),
    "Cancel": ("取消", "取消"),
    "Create": ("建立", "创建"),
    "Apply": ("套用", "应用"),
    "Revert": ("還原", "还原"),
    "Settings": ("設定", "设置"),
    "Advanced": ("進階", "高级"),
    "Performance": ("效能等級", "性能等级"),
    "lite": ("輕量", "轻量"),
    "balanced": ("平衡", "平衡"),
    "performance": ("高效能", "高性能"),
    "Custom": ("自訂", "自定义"),
    "recommended for this Mac": ("這台 Mac 建議", "这台 Mac 建议"),
    "Orientation": ("螢幕方向", "屏幕方向"),
    "Portrait": ("直向", "竖屏"),
    "Landscape": ("橫向", "横屏"),
    "Resolution": ("解析度", "分辨率"),
    "CPU cores": ("CPU 核心", "CPU 核心"),
    "Memory": ("記憶體", "内存"),
    "Storage": ("儲存空間", "存储空间"),
    "Storage can only grow.": ("儲存空間只能變大。", "存储空间只能变大。"),
    "Graphics": ("繪圖", "图形"),
    "Automatic (Mac GPU)": ("自動（Mac GPU）", "自动（Mac GPU）"),
    "Compatibility (OpenGL ES)": ("相容模式（OpenGL ES）", "兼容模式（OpenGL ES）"),
    "Software (slow)": ("軟體（較慢）", "软件（较慢）"),
    "Metal (experimental)": ("Metal（實驗）", "Metal（实验）"),
    "Frame rate cap": ("FPS 上限", "FPS 上限"),
    "no cap": ("不限制", "不限制"),
    "DNS servers": ("DNS 伺服器", "DNS 服务器"),
    "the Mac's": ("使用 Mac 的", "使用 Mac 的"),
    "Use public DNS": ("改用公用 DNS", "改用公共 DNS"),
    "Keep screen on": ("螢幕常亮", "屏幕常亮"),
    "Lean Android (disable unused Google apps, about 600 MB less)":
        ("精簡 Android（停用遊戲用不到的 Google app，約省 600 MB）", "精简 Android（停用游戏用不到的 Google 应用，约省 600 MB）"),
    "Sound": ("聲音", "声音"),
    "Volume": ("音量", "音量"),
    "Muted": ("靜音", "静音"),
    "No sound device (turn Sound on in Settings)": ("沒有音訊裝置（請在設定開啟聲音）", "没有音频设备（请在设置开启声音）"),
    "CPU": ("CPU", "CPU"),
    "Uptime": ("已執行", "已运行"),
    "of one core": ("單核為 100%", "单核为 100%"),
    "Name": ("名稱", "名称"),
    "Lowercase letters, digits, - and _ (up to 32).": ("小寫字母、數字、- 和 _（最多 32 個）。", "小写字母、数字、- 和 _（最多 32 个）。"),
    "An instance with this name already exists.": ("已經有同名的實例。", "已经有同名的实例。"),
    "Saved. The running instance gets the new settings when it restarts.":
        ("已儲存。執行中的實例要重新啟動才會套用。", "已保存。运行中的实例需要重新启动才会应用。"),
    "Saved.": ("已儲存。", "已保存。"),
    "Delete this instance and everything on it (apps and game data)?":
        ("要刪除這個實例和裡面的所有東西（App 和遊戲資料）嗎？", "要删除这个实例和里面的所有内容（应用和游戏数据）吗？"),
    "Stop it first.": ("請先停止它。", "请先停止它。"),
    "No instances yet.": ("還沒有實例。", "还没有实例。"),
    "Select an instance.": ("選擇一個實例。", "选择一个实例。"),
    "Android runtime": ("Android 執行環境", "Android 运行环境"),
    "SoulX needs its Android runtime (engine, adb and Android 17) before the first start.":
        ("第一次啟動前，SoulX 需要下載它的 Android 執行環境（模擬器引擎、adb 和 Android 17）。",
         "第一次启动前，SoulX 需要下载它的 Android 运行环境（模拟器引擎、adb 和 Android 17）。"),
    "Download size": ("下載大小", "下载大小"),
    "I accept the Android SDK License Agreement": ("我同意 Android SDK 授權條款", "我同意 Android SDK 许可协议"),
    "Read the license": ("閱讀授權條款", "阅读许可协议"),
    "Install": ("安裝", "安装"),
    "Installing…": ("安裝中…", "安装中…"),
    "Installed.": ("安裝完成。", "安装完成。"),
    "Installation failed.": ("安裝失敗。", "安装失败。"),
    "The soulx command was not found. Build it (scripts/build_macos.sh) or set SOULX_CLI.":
        ("找不到 soulx 指令。請先編譯（scripts/build_macos.sh），或設定 SOULX_CLI。",
         "找不到 soulx 命令。请先编译（scripts/build_macos.sh），或设置 SOULX_CLI。"),
    "Starting…": ("啟動中…", "启动中…"),
    "Stopping…": ("停止中…", "停止中…"),
    "Restarting…": ("重新啟動中…", "重新启动中…"),
    "Saving…": ("儲存中…", "保存中…"),
    "Creating…": ("建立中…", "创建中…"),
    "Deleting…": ("刪除中…", "删除中…"),
    "Android": ("Android", "Android"),
    "Error": ("錯誤", "错误"),
    "OK": ("好", "好"),
    "Mac": ("Mac", "Mac"),
    "cores": ("核心", "核心"),
]

func L(_ key: String) -> String {
    guard let t = table[key] else { return key }
    switch currentLang {
    case .zhHant: return t.0
    case .zhHans: return t.1
    case .en: return key
    }
}
