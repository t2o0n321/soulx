import Foundation

/// UI text in Traditional Chinese, Simplified Chinese or English, following the system language (like SoulX's
/// window). Keys are the English text.
enum Lang { case zhHant, zhHans, en }

/// The user's choice as `soulx language` stores it: "system", "en", "zh-hant" or "zh-hans".
enum LanguageChoice: String, CaseIterable, Identifiable {
    case system, en, zhHant = "zh-hant", zhHans = "zh-hans"
    var id: String { rawValue }
    /// Each language is named in itself, so it can be found whatever the current language is.
    var label: String {
        switch self {
        case .system: return L("Same as the Mac")
        case .en: return "English"
        case .zhHant: return "繁體中文"
        case .zhHans: return "简体中文"
        }
    }
}

private func systemLang() -> Lang {
    let pref = Locale.preferredLanguages.first?.lowercased() ?? "en"
    if pref.hasPrefix("zh-hant") || pref.hasPrefix("zh-tw") || pref.hasPrefix("zh-hk") || pref.hasPrefix("zh-mo") { return .zhHant }
    if pref.hasPrefix("zh") { return .zhHans }
    return .en
}

/// The language in use; the app sets it from the saved choice and when the user picks another one.
nonisolated(unsafe) var currentLang: Lang = systemLang()

func applyLanguage(_ choice: LanguageChoice) {
    switch choice {
    case .system: currentLang = systemLang()
    case .en: currentLang = .en
    case .zhHant: currentLang = .zhHant
    case .zhHans: currentLang = .zhHans
    }
}

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
    "Status": ("狀態", "状态"),
    "More": ("更多", "更多"),
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
    "Language": ("語言", "语言"),
    "Advanced settings": ("進階設定", "高级设置"),
    "Scripts": ("腳本", "脚本"),
    "AI": ("AI", "AI"),
    "AI control": ("AI 控制", "AI 控制"),
    "On": ("開", "开"),
    "Off": ("關", "关"),
    "Let AI clients control SoulX (MCP)": ("讓 AI 控制 SoulX（MCP）", "让 AI 控制 SoulX（MCP）"),
    "An AI app (an MCP client) can look at the screen and tap, swipe and type in the instances you allow. Turning this off stops it at once.": ("AI 應用程式（MCP 用戶端）可以在你允許的實例裡看畫面、點擊、滑動和打字。關閉後立即停止。", "AI 应用（MCP 客户端）可以在你允许的实例里看画面、点击、滑动和打字。关闭后立即停止。"),
    "Local only: the AI app starts SoulX and talks to it directly. No network port is opened.": ("只在本機：AI 應用程式直接啟動 SoulX 並和它溝通，不會開啟任何網路連接埠。", "仅限本机：AI 应用直接启动 SoulX 并和它通信，不会开启任何网络端口。"),
    "Instances the AI may use": ("AI 可以使用的實例", "AI 可以使用的实例"),
    "All instances": ("全部實例", "全部实例"),
    "Only these": ("只有這些", "只有这些"),
    "What the AI may do": ("AI 可以做什麼", "AI 可以做什么"),
    "Look and touch": ("看畫面與操作", "看画面与操作"),
    "Always (screenshots, text, taps, swipes, keys, apps)": ("一律允許（截圖、讀字、點擊、滑動、按鍵、App）", "一律允许（截图、读字、点击、滑动、按键、应用）"),
    "Start and stop instances": ("啟動和停止實例", "启动和停止实例"),
    "Install apps": ("安裝 App", "安装应用"),
    "Installs .apk files from your Mac. Leave off unless you need it.": ("會從你的 Mac 安裝 .apk 檔。不需要就保持關閉。", "会从你的 Mac 安装 .apk 文件。不需要就保持关闭。"),
    "Run your scripts": ("執行你的腳本", "运行你的脚本"),
    "Only the scripts in each instance's own scripts folder.": ("只限每個實例自己腳本資料夾裡的腳本。", "仅限每个实例自己脚本文件夹里的脚本。"),
    "Run Lua code it writes": ("執行 AI 寫的 Lua 程式", "运行 AI 写的 Lua 代码"),
    "Always sandboxed: it can operate the instance but cannot read or change files on your Mac.": ("一律在沙盒中執行：可以操作實例，但不能讀取或修改你 Mac 上的檔案。", "一律在沙盒中运行：可以操作实例，但不能读取或修改你 Mac 上的文件。"),
    "Never": ("一律不允許", "一律不允许"),
    "Create, delete or change instances; setup": ("建立、刪除或修改實例；安裝執行環境", "创建、删除或修改实例；安装运行环境"),
    "Connect an AI app": ("連接 AI 應用程式", "连接 AI 应用"),
    "Command and argument (stdio)": ("指令與參數（stdio）", "命令与参数（stdio）"),
    "As JSON (most AI apps)": ("JSON 設定（多數 AI 應用程式）", "JSON 配置（多数 AI 应用）"),
    "Copy": ("拷貝", "拷贝"),
    "Recent activity": ("最近的活動", "最近的活动"),
    "No supported AI app was found on this Mac. Install one (for example an AI desktop app or code editor with MCP support), or set it up by hand below.": ("這台 Mac 上沒有找到支援的 AI 應用程式。可以先安裝一個（例如支援 MCP 的 AI 桌面 App 或程式編輯器），或用下方的手動設定。", "这台 Mac 上没有找到支持的 AI 应用。可以先安装一个（例如支持 MCP 的 AI 桌面应用或代码编辑器），或用下方的手动设置。"),
    "Set up by hand (advanced)": ("手動設定（進階）", "手动设置（高级）"),
    "SoulX adds itself to the app's own settings and keeps everything else in them (the old file is backed up first).": ("SoulX 會把自己加進該 App 的設定，其他設定都不會動（修改前會先備份）。", "SoulX 会把自己加进该应用的设置，其他设置都不会动（修改前会先备份）。"),
    "Connect": ("連接", "连接"),
    "Disconnect": ("中斷連接", "断开连接"),
    "Repair": ("修復", "修复"),
    "Remove": ("移除", "移除"),
    "Page %d of %d · %d calls": ("第 %d／%d 頁 · 共 %d 筆", "第 %d／%d 页 · 共 %d 条"),
    "Newest": ("最新", "最新"),
    "Newer": ("較新", "较新"),
    "Older": ("較舊", "较旧"),
    "Turn on": ("開啟", "开启"),
    "Added, but AI control is off: the app cannot use SoulX": ("已加入，但 AI 控制已關閉，目前無法使用", "已加入，但 AI 控制已关闭，目前无法使用"),
    "Move SoulX to your Applications folder first, then open it from there. Running from Downloads or the disk image, macOS gives it a temporary location that AI apps could not find later.": ("請先把 SoulX 移到「應用程式」資料夾，再從那裡打開。從「下載項目」或磁碟映像檔執行時，macOS 會給它一個暫時的位置，AI 應用程式之後會找不到。", "请先把 SoulX 移到“应用程序”文件夹，再从那里打开。从“下载”或磁盘映像运行时，macOS 会给它一个临时位置，AI 应用之后会找不到。"),
    "SoulX was removed from %@. Restart it to apply.": ("已從 %@ 移除 SoulX。重新開啟它即可生效。", "已从 %@ 移除 SoulX。重新打开它即可生效。"),
    "Connected": ("已連接", "已连接"),
    "Not connected": ("未連接", "未连接"),
    "Connected to another copy of SoulX": ("連到另一份 SoulX（按修復改成這一份）", "连到另一份 SoulX（按修复改成这一份）"),
    "Its settings file is damaged; SoulX does not touch it": ("它的設定檔格式有誤，SoulX 不會去動它", "它的设置文件格式有误，SoulX 不会去动它"),
    "%@ is connected. Restart it to start using SoulX.": ("已連接 %@。重新開啟它就能開始使用 SoulX。", "已连接 %@。重新打开它就能开始使用 SoulX。"),
    "No AI activity yet.": ("還沒有 AI 活動。", "还没有 AI 活动。"),
    "No scripts yet. Import a .lua file or a script folder.": ("還沒有腳本。匯入 .lua 檔或腳本資料夾。", "还没有脚本。导入 .lua 文件或脚本文件夹。"),
    "Run scripts from the instance's window.": ("在實例的視窗裡執行腳本。", "在实例的窗口里运行脚本。"),
    "Open folder": ("打開資料夾", "打开文件夹"),
    "Import…": ("匯入…", "导入…"),
    "Rename script": ("重新命名腳本", "重命名脚本"),
    "Rename": ("重新命名", "重命名"),
    "Rename…": ("重新命名…", "重命名…"),
    "The name shown in the window's script list.": ("視窗腳本清單裡顯示的名稱。", "窗口脚本列表里显示的名称。"),
    "Move to Trash": ("移到垃圾桶", "移到废纸篓"),
    "Move to Trash…": ("移到垃圾桶…", "移到废纸篓…"),
    "Show in Finder": ("在 Finder 中顯示", "在访达中显示"),
    "Move the folder \"%@\" (the whole script package) to the Trash?": ("要把資料夾「%@」（整個腳本包）移到垃圾桶嗎？", "要把文件夹「%@」（整个脚本包）移到废纸篓吗？"),
    "Move \"%@\" to the Trash?": ("要把「%@」移到垃圾桶嗎？", "要把「%@」移到废纸篓吗？"),
    "no name: the window will not list it": ("沒有名稱：視窗不會列出", "没有名称：窗口不会列出"),
    "Hardware": ("硬體", "硬件"),
    "Display and performance": ("畫面與效能", "画面与性能"),
    "System": ("系統", "系统"),
    "Lean Android": ("精簡 Android", "精简 Android"),
    "Try Compatibility if a game shows black or broken graphics.": ("遊戲畫面全黑或破圖時，改用相容模式。", "游戏画面全黑或花屏时，改用兼容模式。"),
    "30 FPS uses about 30% less CPU, good for idle farming.": ("30 FPS 約省 30% CPU，適合掛機。", "30 FPS 约省 30% CPU，适合挂机。"),
    "Change this when Android has no internet (VPN or company network).": ("Android 上不了網時（VPN 或公司網路）再修改。", "Android 上不了网时（VPN 或公司网络）再修改。"),
    "Disables preinstalled Google apps games never use; about 600 MB less memory.": ("停用遊戲用不到的預裝 Google app，約省 600 MB 記憶體。", "停用游戏用不到的预装 Google 应用，约省 600 MB 内存。"),
    "Off removes the sound device; saves a little CPU.": ("關閉會移除音訊裝置，可省一點 CPU。", "关闭会移除音频设备，可省一点 CPU。"),
    "Same as the Mac": ("跟隨 Mac", "跟随 Mac"),
]

// keys that are identifiers rather than English text
private let english: [String: String] = ["lite": "Lite", "balanced": "Balanced", "performance": "Performance"]

func L(_ key: String) -> String {
    guard let t = table[key] else { return english[key] ?? key }
    switch currentLang {
    case .zhHant: return t.0
    case .zhHans: return t.1
    case .en: return english[key] ?? key
    }
}
