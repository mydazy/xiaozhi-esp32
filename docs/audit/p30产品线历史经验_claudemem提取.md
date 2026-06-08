# P30 产品线历史技术经验（claude-mem 提取 · 2026-06-07）

> 来源：claude-mem 自动记忆库（v1/v2/udour 等旧仓，2026-03~05），下线前提取精华供 v31 量产复用。
> 仅保留 decision / bugfix / 会话 learned；过程性 discovery(736) 已舍弃。原始 47M db 已删，不另留备份。

## 一、关键决策（decision · 24 条）

### [mydazy] Simplify Device Webhook Authorization Configuration

["DeviceWebhookAuthVo interface originally contained deviceId, macAddress, webhookKey, webhookSecret, and webhookUrl fields","Decision made to simplify device webhook generation to only require URL + token","Simplified configuration improves user experience when integrating with openclaw","Users can now copy URL + token directly instead of managing multiple credential fields"]

### [mydazy] Device authentication flow simplified to direct pushstt request

["Device startup no longer requires requesting /ota/stttoken endpoint","Devices now authenticate directly via pushstt endpoint using Device-Id and Client-Id headers","Authentication flow simplified from two-step to single-step process"]

### [mydazy] TTS Volume and Speech Rate Configuration Standards

["ttsVolume default value set to 2","Speech rate conversion logic derives from system field configuration","TTS settings standardized for consistent audio output behavior"]

### [mydazy] 带货搭子 - AI Live Streaming Sales Assistant Product Design

["Product named \"带货搭子\" (mydazy Live Streaming Sales Partner) - 24-hour AI sales companion","Target users: small/medium merchants, individual micro-businesses, physical store owners, and brand companies lacking live streaming resources","Core workflow: add products → write/AI-generate scripts → select AI agent persona → one-click start → automated循环 pitching with interaction","Backend data model includes iot_live_task, iot_live_product, iot_live_script, iot_live_log tables under iot module","Reuses existing infrastructure: ruoyi-common-langchain4j for script generation, WebSocket/MQTT for real-time control, OSS for media storage","Three-phase roadmap: Phase 1 MVP (product CRUD + task creation + script播放), Phase 2 智能化 (AI generation + interaction), Phase 3 commercialization (analytics + template market)","Key differentiation: low cost (voice AI vs digital human rendering), interactive (AI agent-based), simple deployment (mobile/device vs cloud + high-spec PC)"]

### [mydazy] Dual Mode Architecture - Live Streaming Mode and Sales Mode

["Final product name confirmed as \"卖货搭子\" (Selling Partner) replacing \"带货搭子\" - broader scenario coverage for dual modes","Brand positioning updated to \"AI帮你卖货，直播门店都能用\" (AI helps you sell, works for both live streaming and stores)","直播模式 (Live Streaming Mode) features real-time platform integration, comment/barrage replies, paced scripts with urgency tactics","推销模式 (Sales Mode) features offline store/exhibition use, voice Q&A, detailed product introductions, pure循环播放","Dual mode value proposition: same product and script data set, daytime sales mode (store循环播放), nighttime live streaming mode (AI automated sales)","Rationale for \"卖货\" over \"带货\": wider scenario coverage naturally包含 live streaming sales plus in-store sales","Explicitly rejected \"直播推销助手\" naming - \"xx助手\" pattern lacks differentiation and memorability"]

### [mydazy] App branding finalized with DazySales as English name

["Chinese app store name confirmed as \"卖货搭子\" (Sales Companion)","English brand name set to \"DazySales\" with subtitle \"AI-Powered Sales Companion\"","Project directory simplified from \"plus-maishou\" to \"sales-app\"","Bundle identifier standardized to \"com.mydazy.sales\" for all platforms","Naming strategy marked as \"第四轮确定\" (4th round finalized) indicating iterative refinement process"]

### [mydazy] SaleMate product specification created with uni-app architecture

["Product specification document created at docs/salemate-product-spec.md defining complete feature set for SaleMate (卖货搭子)","Technology stack selected: uni-app + Vue 3 + TypeScript + WD UI for cross-platform deployment (WeChat mini-program, iOS, Android, H5)","Four-tab application architecture defined: ProductHub (商品库), LiveMode (直播), PromoteMode (推销), My (我的)","Two core operational modes specified: Live Mode (AI host 7x24 automated live streaming) and Promote Mode (store/exhibition looping product presentations)","Backend architecture defined using existing ruoyi-admin Spring Boot framework with iot_live_* tables for products, scripts, tasks, and interactions","Frontend project structure planned at sales-app/ with page routing, API layer, component library, and state management","Color system established: brand orange #FF6B35, live red #FF4757, promote blue #3742FA following Apple/WeChat design principles","MVP roadmap defined in 3 phases: Phase 1 core loop (4 weeks), Phase 2 AI features (3 weeks), Phase 3 commercialization (3 weeks)","AI script generation integrated via LangChain4j for automated product pitch creation in multiple styles (enthusiastic, professional, warm)","Bluetooth provisioning flow reused from existing plus-uniapp project for MyDazy device binding"]

### [mydazy] SaleMate Independent Project Architecture Plan Designed

["SaleMate development will extend existing IoT module with live subpackage rather than creating separate Maven module","Project will continue on dev branch without creating feature/salemate branch since SaleMate is integrated business line","Database design includes 9 new tables with iot_live_ prefix following TenantEntity pattern with snowflake IDs","Backend follows 4-sprint plan: Sprint 1 Product+Script CRUD, Sprint 2 Task system, Sprint 3 Logs+Device commands, Sprint 4 Integration","Mobile app uniapp-sales keeps current directory name and will be cleaned of device management pages","Tabbar redesigned from original structure to 4 tabs: ProductHub, LiveMode, PromoteMode, My","Team agent roles defined: salemate-lead, backend-engineer, mobile-engineer, device-engineer, qa-engineer with specific skills","Database tables support multi-tenant architecture with tenant_id field and logical deletion with is_deleted flag","Backend development follows Entity→BO→VO→Mapper→DAO→Service→Controller pattern for each feature","Mobile development spans 4 weeks: Week 1 cleanup+ProductHub, Week 2 Scripts+Promote, Week 3 Live+My, Week 4 Integration"]

### [mydazy-p30-udour] Board configuration architecture: 3 variants with shared codebase

["3 board configuration variants will be created","All variants share the same board code implementation","Variants differ only in SPIFFS configuration and default Profile settings","This approach avoids code duplication across board configurations"]

### [mydazy-p30-udour] Pomodoro Timer UI Layout Design Decisions

["Subpages for pomodoro timer, alarm clock, and \"three things\" feature should retain title headers for navigation context","Four time preset shortcuts for pomodoro timer positioned at corners of start/pause button to minimize accidental touches","Navigation back mechanism needs differentiation across different subpages"]

### [mydazy-p30-udour] Reverted Build Fix to Prioritize Chinese Localized WiFi Configuration Interface

["WIFI_CONFIG_HTML variable and EMBED_TXTFILES parameter restored to main/CMakeLists.txt configuration","Comment changed from \"no duplicate embedding needed\" to \"custom Chinese version overrides component default English version\"","Decision indicates localization requirements take priority over eliminating build system conflicts","Restoration recreates \"multiple rules generate wifi_configuration.html.S\" error condition","Approach attempts file override strategy not natively supported by ESP-IDF EMBED_TXTFILES mechanism","Alternative solutions needed to resolve both localization and build system requirements simultaneously"]

### [mydazy-p30-v1] Upstream Sync Strategy for ESP32 Firmware (xiaozhi-esp32-189 → mydazy-p30-v1)

["Plan syncs xiaozhi-esp32-189 (v1.9.56) improvements to mydazy-p30-v1 (v3.9.21) across 4 priority tiers (P0-P3)","P0 priority: WebSocket circuit breaker (3 silent failures/10 disconnect), TTS state tracking, connection reuse, weak network frame dropping","Audio improvements include decoder mutex protection, DAC warm-up (2 silent frames to prevent pop), dynamic frame duration, stack increase to 32KB","Application layer adds send failure backoff (50ms retry delay), ReconnectIfNeeded helper, enhanced AbortSpeaking protection","Preserved P30-V1 features: OTA resume, watchdog RAII, background wake handling, alarm sync, filtered device status reporting","Tooling: /upstream-diff skill for module comparison, upstream-sync-team with 4 roles (lead/analyzer/merger/validator)","UI/display differences explicitly excluded from sync to preserve P30 interface"]

### [mydazy-p30-v2] Comprehensive MyDazy P30 V2 升级实施方案完成

["Plan agent spent 403 seconds analyzing codebase with 77 tool uses and 106K tokens","Discovered v2 already has most upstream audio fixes merged (afe_wake_word.cc, audio_service.cc identical to upstream)","v2 esp-ml307 version ~3.6.4 is newer than upstream 3.6.2, no update needed","Only 8 upstream commits require merging, down from originally estimated 50+","UI comparison shows v2's mmap asset loading system superior to udour's static declarations","ai_chat_config.cc/h identified as new feature in udour requiring port to v2","Implementation divided into 3 sprints with task breakdown and risk assessment","Simulator design includes mock layer for 76 ESP32 API calls and 284x240 screen matching"]

### [mydazy-p30-v2] Touch Screen Gesture Enhancement Plan for MyDazy P30 V2

["MyDazy P30 V2 hardware supports 7 gesture types (single/double click, long press, 4-direction swipe) but application layer only implements single click for wake/interrupt","Plan enhances gesture callbacks to add swipe-down for control center, swipe-up to close, double-click for page navigation, matching udour v3.0.2 behavior","Architecture differs between versions: udour uses monolithic Display class with compiled-in assets, v2 uses layered Display → LvglDisplay → LcdDisplay → SpiLcdDisplay → UiDisplay with mmap zero-copy images","Control center will bind 6 button callbacks: network switch, AEC interrupt mode, sleep countdown, exit, volume slider, brightness slider","Optional ai_chat_config module (511 lines) enables JSON-driven UI for dynamic agent/model/voice switching with long-press menu","NextPage and PrevPage methods currently have empty implementations requiring page cycle logic: CLOCK ↔ MENU ↔ TIME_ROOM"]

### [mydazy-p30-v2] Complete API correction strategy documented for all 10 P32 compilation errors

["WakeUP is custom method not in Board base, should be called on power_save_timer_ not board instance","GetI2cBus, CleanupDisplay not in base classes, require removing override keyword to compile","SetPowerSaveMode(bool) should be SetPowerSaveLevel(PowerSaveLevel enum) throughout hierarchy","Axs5106lTouch two-phase InitializeHardware/InitializeInput must merge into single Initialize call","SystemReset FactoryReset is private instance method ResetToFactory, needs refactoring or static wrapper","WifiBoard ResetWifiConfiguration does not exist, should use EnterWifiConfigMode instead","DualNetworkBoard SetPowerSaveMode delegation must change to SetPowerSaveLevel","WifiStation GetInstance exists in ESP-IDF header, may require version check"]

### [mydazy-p30-v2] Icon Design System for MyDazy P30 Product

["Design system specifies 512×512px source size, scaling to 58×58px for deployment","Visual style: rounded square backgrounds (22% corner radius) with centered monochrome glyphs (3px stroke weight)","Color palette: 15 distinct gradient colors mapped to specific functions (purple for time, pink for photos, blue for AI, etc.)","Icon set includes 4 main menu icons, 4 time room submenu icons, 6 content library icons, and 1 logo","Recommended tooling: Recraft V3 (primary), Ideogram 2.0, Midjourney v6.1, and IconifyAI"]

### [mydazy-p30-v2] Task created to replace portMAX_DELAY with timeout versions

["Task ID 5 created to track portMAX_DELAY replacement work in mydazy-p30-v2 project","Target scope is core modules under main/ directory that use portMAX_DELAY without timeout protection","portMAX_DELAY is FreeRTOS infinite wait constant that can cause indefinite blocking","Replacement approach uses explicit timeout values instead of infinite waits"]

### [mydazy-p30-v2] Task created to fix audio task CPU core pinning

["Task ID 6 created to track audio task core pinning fixes in mydazy-p30-v2 project","audio_input and audio_output tasks will be pinned to Core1 for I/O operations","opus_codec task will be pinned to Core0 for encoding/decoding operations","Proper core affinity prevents task migration overhead and improves real-time audio performance"]

### [mydazy-p30-v2] Comprehensive fork-upstream merge strategy documented

["Architectural comparison plan created between mydazy-p30-v2 fork (v2.2.4, ESP-IDF 5.5) and xiaozhi-esp32-189 upstream (v1.9.60, ESP-IDF 5.4)","Plan identifies 7 high-value upstream modules for selective merge: media_player, acoustic_calibration, ota_http_download (resumable OTA), remote_cmd, websocket_baidu, live_companion, blufi provisioning","Fork's production bug fixes documented for upstream contribution: MQTT fragmentation timeout, ESP_ERROR_CHECK retry logic, volatile→std::atomic conversion, portMAX_DELAY timeout fixes, Schedule queue limits, audio task core binding, StartVolumeTask memory leak fix","Fork-exclusive capabilities catalogued: NFC subsystem (WS1850S), alarm_manager, comprehensive LVGL UI with 20+ pages, OGG demuxer, three-tier asset management system, emote_display with GIF animation, OLED display support","Upstream-exclusive capabilities identified: media playback, acoustic calibration, resumable HTTP OTA downloads, remote command infrastructure, live companion streaming, Bluetooth WiFi provisioning, independent wifi/ulp module organization","Board portfolio differences quantified: fork supports 15 boards (added NFC P32, printer variant, video/RNDIS, third-party integrations) vs upstream 11 boards (includes E31-4G, four E20 display variants)","Plan saved to /Users/jack/.claude/plans/typed-hatching-candy.md with execution steps and validation criteria"]

### [mydazy-p30-v2] Launched comprehensive P30 board and UI system audit

["Explore agent \"p30-ui-checker\" launched in background for mydazy-p30-v2 project","Audit scope covers board configs in main/boards/mydazy-p30/ and main/boards/mydazy-p30-4g/ directories","UI inspection includes all pages (alarm, pomodoro, todo, time_room, brain_info, content_hub, photo_wall, profile) and base components","Display driver chain review spans board-specific ui_display, lcd_display, and LVGL engine layers","Thread safety review focuses on LVGL locking, touch callback blocking, and page transition resource cleanup"]

### [mydazy-p30-v2] Memory Safety and Resource Leak Audit Initiated

["Background code-reviewer agent launched to audit memory safety in mydazy-p30-v2/main/ directory","Audit covers six critical areas: malloc/free pairing, std::deque capacity limits, esp_timer lifecycle, I2C/SPI handle cleanup, PSRAM allocation strategy, and condition variable timeouts","Analysis targets all board implementations: mydazy-p30, mydazy-p30-4g, mydazy-e20, and mydazy-P32","Audit is read-only research with no file modifications, outputting prioritized findings (P0/P1/P2) with file:line references and remediation recommendations"]

### [mydazy-p30-v2] Isolate SpiLcdDisplay to resolve ESP32-S3 boot loop

["ESP32-S3 device enters continuous boot loop after firmware flash","Device successfully initializes flash and octal PSRAM (64 Mbit AP generation 3) before restart","Troubleshooting strategy removes UiDisplay related dependencies","Goal is to ensure SpiLcdDisplay runs independently before adding UiDisplay back"]

### [mydazy-p30-v2] Separated P30 Base and P30-4G Build Configurations

["P30 family shared configuration includes fonts, icons, emoji collection, and asset files","UiDisplay, UI assets, and UI page sources now compile only for CONFIG_BOARD_TYPE_MYDAZY_P30 base version","P30-4G board (CONFIG_BOARD_TYPE_MYDAZY_P30_4G) excluded from UiDisplay compilation","Alarm manager and UI include directories remain P30 base version exclusive","Build system supports parallel development of full-featured P30 and simplified P30-4G variants"]

### [mydazy-p30-v2] Removed remote_cmd.cc to fix compilation errors

["remote_cmd.cc had compilation errors due to Application class missing GetLiveCompanion() method","Multiple lambda capture errors occurred because lc variable was not properly captured in closures","File removal chosen as tactical fix to unblock build and ensure system can run normally","Errors occurred at lines 132, 150, 256, 269, 283, 291, 294, 298, and 302 in remote_cmd.cc"]


## 二、Bug 修复根因（bugfix · 115 条 · v31 可避坑）

### [mydazy] Fixed missing DeviceWebhookAuthVo type import in deviceApi.ts

["DeviceWebhookAuthVo added to import statement in deviceApi.ts","Import now includes five types: DeviceBo, DeviceQuery, DeviceVo, DeviceWebhookAuthVo, and BindDeviceByMacAndUserIdBo","Fix resolves undefined type reference error for generateWebhookAuth function return type","Import maintains alphabetical-ish ordering with DeviceWebhookAuthVo placed before BindDeviceByMacAndUserIdBo"]

### [mydazy] Fixed webhook type validation error message inconsistency

["WEBHOOK_ALLOWED_TYPES constant contains only \"tts\", \"ttai\", \"ai\" (3 types)","Error message incorrectly claimed to support \"tts/tti/ttai/ai\" (4 types including tti)","Updated error message in DeviceOtaController.java line 357 to show \"tts/ttai/ai\" only","Webhook validation now rejects \"tti\" type requests with accurate error message"]

### [mydazy] Fixed TTS Parameter Conflict by Removing tts_voice Setting

["Removed .ttsVoice(\"搭子精灵\") from BaiduAIAgentCallConfig builder to prevent parameter conflict","tts_voice and tts+tts_url are mutually exclusive in Baidu's API","When both are set, Baidu prioritizes tts_voice and ignores vol/spd/pit parameters in tts_url","Added explanatory comments documenting the mutual exclusivity and its consequences","Now uses tts+tts_url complete control mode to support cu

### [mydazy] Fixed AI self-interruption by disabling full-duplex mode and increasing TTS delay

["Full-duplex mode disabled by setting dfda parameter from true to false to pause ASR during TTS playback","TTS end delay increased from 100ms to 300ms to prevent residual audio from triggering ASR","Configuration change applies to buildCustomConfigWithRole method in BaiduRtcAiProvider.java at line 416","Users can still interrupt AI speech using configured interruption words despite dfda being dis

### [mydazy] WiFi scanning state now shows loading overlay

["Loading overlay v-if condition updated from isConnecting || isConfiguring to include isScanning state","Template comment updated to reflect WiFi scanning state in overlay description","Loading overlay now displays during BLE connection, WiFi scanning, and credential configuration phases","Fix addresses user requirement to show waiting state during WiFi list retrieval instead of popup"]

### [mydazy] Clear status messages on WiFi scan timeout

["WiFi scan timeout handler now clears statusMessage and statusSubMessage values","Timeout occurs after 15 seconds if WiFi scan does not complete","Prevents loading overlay from remaining stuck on screen after timeout","Fixed in wifiScanTimer setTimeout callback in chooseWifi.vue"]

### [mydazy] Clear status messages on WiFi scan error

["scanWifiList error handler now clears statusMessage and statusSubMessage","Ensures UI state cleanup when WiFi scan fails with exception","Complements timeout handler fix for comprehensive error handling","Prevents loading overlay from persisting after scan failures"]

### [mydazy] Prevent WiFi scan completion from clearing configuration state

["parseWifiList function now only clears isScanning flag, not isConfiguring","Prevents race condition where scan result parsing interferes with ongoing configuration process","Configuration state (isConfiguring) now managed independently from scan state","Comment updated to clarify state management separation"]

### [mydazy] Added 15-second timeout and preserved WiFi list during rescan

["Removed wifiList.value = [] from scanWifiList to preserve existing WiFi networks during rescan","Added 15-second setTimeout that automatically stops isScanning.value if device doesn't respond","Simplified writeData call formatting from multi-line to single-line for code brevity","Removed verbose comments about sequence numbers and timeout behavior to streamline code","User requested 15-second ti

### [mydazy] Increased BLE notification delay from 500ms to 1500ms

["Changed setTimeout delay in enableNotification from 500ms to 1500ms after enabling BLE notifications","Updated comment to explain \"500ms不够，会导致10007\" (500ms not enough, causes 10007 error)","This is the root cause fix for 10007 errors occurring on every page entry","ESP32 needs adequate time after BLE_CONNECT event to initialize security layer before characteristic value writes","Completes all 

### [mydazy] BLE Write Channel Readiness Detection Added to WiFi Configuration

["scanWifi function now implements 15-attempt retry loop with 1-second intervals to verify write characteristic readiness","writeReady flag tracks BLE write channel availability status across scan and configuration phases","handleWifiSelect waits up to 5 seconds for writeReady flag before attempting configuration commands","Configuration phase displays \"等待蓝牙通道就绪...\" status message if write chann

### [mydazy] Dynamic BLE Write Mode Selection Based on Characteristic Properties

["BLE write mode now dynamically determined by inspecting wc.properties at connection time","Code checks props_w.writeNoResponse first, falls back to props_w.write if unsupported","Comment indicates this change addresses root cause of error 10007 (characteristic does not support write without response)","Changed logging from generic characteristic properties to specific writeType selection and JSO

### [mydazy] Declare writeType Variable with Safe Default Value

["writeType variable declared at module level with default value 'write' (write with response)","Default value ensures fallback behavior if characteristic property detection fails","Comment explains variable is automatically adjusted during connect() based on characteristic properties","Variable declared alongside other BLE state variables (mtu, heartbeat, timeout, configOk)","Safe default 'write'

### [mydazy] Replace Hardcoded writeType with Dynamic Variable in bleWrite Function

["Changed writeType parameter in uni.writeBLECharacteristicValue from hardcoded 'writeNoResponse' to writeType variable","Type assertion 'as any' bypasses TypeScript literal type checking for writeType parameter","bleWrite function now respects write mode determined during connect() characteristic discovery","All BLE write operations through bleWrite now use capability-aware write mode","Completes

### [mydazy] Fixed Hardcoded writeType in scanWifi Function

["Updated scanWifi function's uni.writeBLECharacteristicValue call to use writeType variable instead of hardcoded 'writeNoResponse'","scanWifi sends WiFi scan command directly without going through bleWrite wrapper function","All BLE write operations in wifiConfig.vue now use capability-detected write mode","Ensures consistent write mode behavior across WiFi scanning and provisioning commands","Co

### [mydazy] Status Display Failure After Successful Network Configuration

["WiFi provisioning completes successfully: connected to SSID \"1bom.cn\" with IP 192.168.1.104","BLE connection properly terminated and cleaned up after configuration","Device transitions to STA mode and application state becomes \"idle\"","Display status set to \"待命\" (standby) instead of expected success/connected state","Status determination logic needs comparison with historical versions to i

### [mydazy-p30-udour] Fixed screenshot rendering synchronization

["Added lv_refr_now(NULL) call to force LVGL immediate frame rendering before screenshot capture","Added SDL_RenderPresent(renderer) call to force SDL to present current frame before pixel readback","Changes made to save_screenshot_bmp function in lvgl_ui/main.cpp","Fix ensures automated test screenshots capture complete visual state rather than incomplete frames"]

### [mydazy-p30-udour] Fixed weekday display spacing in Chinese

["Changed weekday format from \"星期 %s\" to \"星期%s\" in ui_display.cc line 852","Display now shows \"星期一\" instead of \"星期 一\" (more natural Chinese format)","UpdateClockTime function in ui_display.cc modified","Affects weekday label display on clock UI"]

### [mydazy-p30-udour] Brain Info Page Scroll Area Height Corrected for Standard Header

["Scroll container height changed from 204px to 192px in BrainInfoPage::Create","Comment updated to reflect correct calculation: 240 - 48 instead of 240 - 36","Adjustment aligns with HEADER_HEIGHT constant of 48px defined in ui_config.h","Previous 36px height was from custom header implementation before CreatePageHeader refactor"]

### [mydazy-p30-udour] Alarm Page Swipe Areas Repositioned to Avoid Back Button Conflict

["Swipe area vertical position changed from y=-20 to y=10 in CreateSwipeAreas function","Left arrow repositioned from (16, -20) to (8, 10) with x-offset closer to edge","Right arrow repositioned from (-16, -20) to (-8, 10) maintaining symmetry","Arrow color lightened from 0x555555 to 0x666666 for improved visibility","Comment updated to clarify avoidance of back button area instead of bottom area"

### [mydazy-p30-udour] Fixed right arrow icon font in alarm page navigation

["Right arrow label in alarm page was incorrectly using font_puhui_20_4 (Chinese text font)","Changed to font_awesome_20_4 to properly render FONT_AWESOME_ANGLE_RIGHT icon","Left arrow already correctly used font_awesome_20_4, creating visual inconsistency","Bug was in CreateSwipeAreas() function at line 129 of alarm_page.cc"]

### [mydazy-p30-udour] Removed MENU_ITEMS array reference from click handler logging

["OnMenuItemClicked log statement changed from \"菜单项点击: id=%d, name=%s\" with MENU_ITEMS[menu_id].name to \"菜单项点击: id=%d\" with only menu_id","Change necessary because MENU_ITEMS static array was removed during profile-aware refactoring","Menu item name no longer available in OnMenuItemClicked since names come from profile configuration in CreateMenuUI"]

### [mydazy-p30-udour] Release script name validation case-sensitivity issue

["Release script validation requires build name to start with board_type using case-sensitive comparison","Build name \"MyDazy-P30-4G-Child\" fails validation against board_type \"mydazy-p30-4g\"","Validation error: \"name MyDazy-P30-4G-Child 必须以 mydazy-p30-4g 开头\"","Python startswith() method performs case-sensitive string matching","Issue affects both config_child.json and config_tour.json varia

### [mydazy-p30-udour] Fixed child variant build name to match case-sensitive validation

["Build name in config_child.json changed from \"MyDazy-P30-4G-Child\" to \"mydazy-p30-4g-child\"","Lowercase hyphenated naming now matches board_type \"mydazy-p30-4g\" format","Satisfies release script's case-sensitive startswith() validation check","Build output filename will now be releases/v3.0.2_mydazy-p30-4g-child.zip"]

### [mydazy-p30-udour] Fixed tour variant build name to match case-sensitive validation

["Build name in config_tour.json changed from \"MyDazy-P30-4G-Tour\" to \"mydazy-p30-4g-tour\"","Lowercase hyphenated naming consistent with board_type \"mydazy-p30-4g\" format","Both variant configs (child and tour) now use consistent naming convention","Build output filename will be releases/v3.0.2_mydazy-p30-4g-tour.zip","Three builds now follow pattern: mydazy-p30-4g, mydazy-p30-4g-child, myda

### [mydazy-p30-udour] Child variant build linker failure

["Child variant build progressed through 2375 compilation steps successfully","Linker failure occurred at step 2376 while creating xiaozhi.elf executable","Build process correctly compiled board-specific files including mydazy_p30_board.cc","All library objects built successfully before link stage","Full error output persisted to file for diagnostics"]

### [mydazy-p30-udour] Child variant linker errors for WiFi netif symbols

["Linker errors for esp_netif_create_default_wifi_ap, esp_netif_dhcps_stop, esp_netif_dhcps_start symbols","Errors originate from wifi_configuration_ap.cc in 78__esp-wifi-connect component","Same code built successfully with default configuration before fullclean","fullclean removed all build artifacts before child variant build attempt","WiFi netif symbols are part of esp_netif component infrastr

### [mydazy-p30-udour] Default variant config also requires name fix for validation

["Default config.json build name \"MyDazy-P30-4G\" also fails case-sensitive validation","All three variant configs (default, child, tour) require lowercase-hyphenated naming","Earlier successful build bypassed release script by using idf.py build directly","Release script validation affects all board configurations uniformly","Naming issue discovered across entire board configuration set"]

### [mydazy-p30-udour] Case-insensitive build name validation in release script

["Validation changed from name.startswith(board_type) to name.lower().startswith(board_type.lower())","Allows build names in any case (PascalCase, lowercase, etc.) to match board_type","Single-line fix in release.py instead of modifying multiple config files","Maintains backward compatibility with existing naming conventions","Enables both \"MyDazy-P30-4G\" and \"mydazy-p30-4g\" naming styles"]

### [mydazy-p30-udour] Validation fix successful but linker errors persist

["Case-insensitive validation fix allows build to proceed past name check","Build configuration shows mydazy-p30-4g-child with UI_PROFILE_CHILD settings","Same undefined reference errors for esp_netif WiFi functions persist during linking","fullclean followed by build may be removing managed components or dependencies","Build failure is configuration/dependency issue, not validation or naming prob

### [mydazy-p30-udour] Re-enabled WiFi SoftAP support for AP configuration

["CONFIG_ESP_WIFI_SOFTAP_SUPPORT changed from n to y in sdkconfig.defaults","Comment updated from \"蓝牙配网不需要\" to \"WiFi AP 配网需要\"","Re-enabling SoftAP adds approximately 15KB to firmware size","Change necessary for esp_netif_create_default_wifi_ap function to be available","Addresses one of three undefined reference errors in wifi_configuration_ap"]

### [mydazy-p30-udour] Re-enabled LWIP DHCP server for WiFi AP configuration

["CONFIG_LWIP_DHCPS changed from n to y in sdkconfig.defaults","Comment updated from \"热点需要，STA 不需要\" to \"WiFi AP 配网需要\"","Re-enabling DHCP server adds approximately 10KB SRAM usage","Enables esp_netif_dhcps_stop and esp_netif_dhcps_start functions","Completes fix for all three undefined reference errors in wifi_configuration_ap","Combined with SoftAP enable adds approximately 25KB to firmware fo

### [mydazy-p30-udour] Fixed Screenshot Timing Race Condition in Auto-Test

["Replaced two-step test pattern (500ms switch + 800ms screenshot) with single atomic 1000ms step","New switch_verify_and_shot() function performs switch → double lv_refr_now() → verify → screenshot without gaps","Added explicit double-frame rendering (lv_refr_now twice) to ensure LVGL completes all widget updates before screenshot","Eliminated 24 separate test steps, consolidated from switch/shot

### [mydazy-p30-udour] Fixed Font Placeholder Compilation Using Weak Symbol Aliases

["Replaced struct initialization approach with __attribute__((weak, alias(\"lv_font_montserrat_14\")))","Weak alias creates direct symbol reference at link time without requiring compile-time constant initializers","Both ui_font_number and ui_font_font_ai_lx_en_30 now directly alias lv_font_montserrat_14","Weak attribute allows future strong symbol definitions to override placeholder fonts"]

### [mydazy-p30-udour] Fixed Font Placeholders Using Constructor Functions for Runtime Initialization

["Declared ui_font_number and ui_font_font_ai_lx_en_30 as uninitialized global lv_font_t structures","Created constructor functions with __attribute__((constructor)) that run before main()","Constructor functions use memcpy() to copy lv_font_montserrat_14 structure at runtime","line_height adjusted post-copy to simulate font sizes: 48 for number font, 30 for AI font","Approach works on macOS/Darwi

### [mydazy-p30-udour] Added missing Font Awesome icons to simulator stub

["Added 9 missing Font Awesome icon definitions to lvgl_ui/stubs/font_awesome.h: STAR, MUSIC, COMMENT, GLOBE, GAMEPAD, COMPASS, HOUSE, PLAY, HEADPHONES","Simulator uses stub files to mock hardware dependencies, requiring manual synchronization with main headers","Fixed compilation errors: \"use of undeclared identifier\" for FONT_AWESOME_MUSIC, COMMENT, GLOBE, GAMEPAD, COMPASS, STAR","Each icon de

### [mydazy-p30-udour] Z-order corrected for time buttons to overlay start button

["UI creation order changed from CreateTimeButtons-then-CreateStartButton to CreateStartButton-then-CreateTimeButtons","Later-created LVGL objects appear on top in Z-order stacking","Time buttons now overlay start button ensuring their click targets remain accessible","Comment added explaining layering intent: start button at bottom layer, time buttons at top layer"]

### [mydazy-p30-udour] Adjusted grid layout spacing to prevent bottom edge collision

["GRID_SPACING_Y reduced from 25 to 16 pixels to compress vertical layout","GRID_OFFSET_Y changed from +5 to -3 pixels to shift entire grid upward","Added documentation noting 284×240 screen with 25px corner radius requires bottom clearance","Changes apply to all pages using 2x2 grid layout: main menu, time room, content hub","Modification in main/display/ui/ui_config.h affects centralized configu

### [mydazy-p30-udour] ESP32 Firmware Build Blocked by Duplicate Build Rule for wifi_configuration.html.S

["ESP-IDF 5.4 environment activated successfully with Python 3.10.13 on ESP32-S3 target","Project configuration completed with app version 3.0.2 and LVGL 9.3.0 integration","Build uses O3 optimization level and includes 120+ ESP-IDF components plus managed components","Configuration warnings for unknown kconfig symbols: ESP_WIFI_STATIC_RX_BUFFER, CAMERA_NO_AFFINITY, CAMERA_DMA_BUFFER_SIZE_MAX, CAM

### [mydazy-p30-udour] Build Failure Root Cause: Duplicate wifi_configuration.html Files in Multiple Locations

["wifi_configuration.html exists in managed_components/78__esp-wifi-connect/assets/ directory","Duplicate wifi_configuration.html also exists in main/wifi/assets/ directory","Both files configured to generate wifi_configuration.html.S assembly file via ESP-IDF embed mechanism","Ninja build system detects conflict at line 42081 with error: multiple rules generate wifi_configuration.html.S","Duplica

### [mydazy-p30-udour] Confirmed: Both Main Component and Managed Component Embed Same WiFi Configuration HTML Files

["Managed component 78__esp-wifi-connect/CMakeLists.txt explicitly embeds assets/wifi_configuration.html via EMBED_TXTFILES","Managed component also embeds wifi_configuration_done.html alongside the main file","Both main component and managed component register same filenames through EMBED_TXTFILES mechanism","ESP-IDF EMBED_TXTFILES generates assembly file named after source file regardless of dir

### [mydazy-p30-udour] Fixed Build Failure by Removing Duplicate WiFi Configuration HTML Embedding from Main Component

["Deleted WIFI_CONFIG_HTML variable definition that referenced main/wifi/assets/ HTML files","Removed EMBED_TXTFILES parameter with WIFI_CONFIG_HTML from idf_component_register call","Added explanatory comment: WiFi configuration resources embedded by 78__esp-wifi-connect component, no duplicate needed","Preserved all other idf_component_register parameters including EMBED_FILES for sound assets",

### [mydazy-p30-udour] Resolved Build Conflict by Removing EMBED_TXTFILES from Managed WiFi Component

["Removed EMBED_TXTFILES section entirely from managed_components/78__esp-wifi-connect/CMakeLists.txt","Eliminated embedding of assets/wifi_configuration.html and assets/wifi_configuration_done.html from managed component","Main component retains EMBED_TXTFILES for Chinese customized versions from main/wifi/assets/","Change allows main component's localized HTML to serve as authoritative embedded 

### [mydazy-p30-udour] WiFi HTML Embedding Conflict Resolved: Build Progresses to 2260/2385 Files Before New Error

["Build successfully completed CMake configuration and Ninja file generation without wifi_configuration.html.S conflict","Compilation progressed to 2260 out of 2385 files before encountering new error","New compilation error in photo_wall_page.cc line 237: snprintf format truncation warning","Buffer overflow warning indicates text buffer size 8 bytes insufficient for potential 14-byte \"%d/%d\" ou

### [mydazy-p30-udour] Fixed Format Truncation Warning in Photo Wall Page by Increasing Buffer Size

["Increased buffer size from char text[8] to char text[16] in PhotoWallPage::UpdatePageIndicator() method","Fix addresses GCC format-truncation warning for \"%d/%d\" format string that could output up to 14 bytes","Buffer size change matches pattern used in alarm_page.cc which also displays pagination with 16-byte buffer","Modification at line 236 of photo_wall_page.cc handles edge case of maximum

### [mydazy-p30-udour] Applied Defensive Value Clamping to Alarm Page Pagination to Satisfy Format-Truncation Checks

["Modified alarm_page.cc pagination formatting to mask current_index + 1 and total with 0xFF before snprintf","Bitwise mask limits values to 8-bit range (0-255) reducing maximum output from 14 bytes to 8 bytes","Maximum formatted output \"255/255\" requires only 7 characters plus null terminator fitting in 16-byte buffer","Defensive programming pattern adds runtime value clamping beyond buffer siz

### [mydazy-p30-udour] ESP32 Stack Overflow in SC7A20H Accelerometer Task

["SC7A20H accelerometer task crashes with stack overflow during logging operations","Crash occurs at Sc7a20h::TaskLoop() line 221 in sc7a20h.cc","Stack canary watchpoint triggered indicates task stack size insufficient for operations","Backtrace shows deep call chain through esp_log_write, vprintf, uart_write, and mutex operations","System initializes successfully (Touch chip ID 0x510601, GPIO con

### [mydazy-p30-udour] Null Pointer Crash in BLUFI Bluetooth Initialization

["Crash occurs in npl_freertos_eventq_init at npl_os_freertos.c:297 with null pointer dereference (address 0x00000000)","Call chain: app_main → Application::Start() → DualNetworkBoard::StartNetwork() → WifiBoard::StartBlufiConfigMode() → Blufi::Start() → esp_blufi_host_init()","Register A2 is 0x00000000 indicating null pointer passed to eventq_init function","Bluetooth MAC b8:f8:62:f4:59:6e succes

### [mydazy-p30-v1] DAC restart warmup to prevent audio pops on output re-enable

["DAC output re-enable now outputs 2 warmup silence frames before real audio data","Warmup frames allow I2S DMA to stabilize after power state transition","was_silent flag set to true after warmup to trigger fade-in on next real frame","Output enable logic moved before fade-in processing to ensure warmup happens first","Updated comment to clarify fade-in now handles both silence recovery and DAC r

### [mydazy-p30-v1] Added 50ms backoff to audio send retry to prevent hot loop CPU spinning

["Audio send retry now delays 50ms before re-triggering MAIN_EVENT_SEND_AUDIO","Previous behavior immediately retried without delay, creating hot loop on persistent failures","Comment updated to explain backoff prevents main thread from spinning on failures","Only applies when channel is still open but send operation fails"]

### [mydazy-p30-v1] Fixed startup page freeze by holding LCD power LOW through restart

["rtc_gpio_hold_en(AUDIO_PWR_EN_GPIO) added in PrepareForReboot() and OnBeforeRestart() methods in mydazy_p30_board.cc","AUDIO_PWR_EN_GPIO (GPIO9) controls ME6211 LDO which powers both LCD (JD9853) and audio chips via shared AUD_VDD-3.3V rail","RTC GPIO hold ensures pin stays LOW through esp_restart() call, maintaining power-off state during 500ms delay","Previous behavior allowed GPIO9 to float o

### [mydazy-p30-v2] AI_LX_ML307 Board Build Configuration Completed

["AI_LX_ML307 board type configuration was incomplete: only BOARD_TYPE set, no UI compilation","Added 21 lines matching base P30 configuration: fonts, assets paths, UI source glob, alarm manager","DEFAULT_ASSETS_EXTRA_FILES includes fonts and images directories for cbin_font dynamic loading","P30_UI_PAGE_SOURCES glob pattern compiles all display/ui/*.cc files","Added ui_display.cc to SOURCES and b

### [mydazy-p30-v2] Power Save Timer Wake Method Correction

["Board::GetInstance().WakeUP() method does not exist in Board class","Correct wake mechanism is power_save_timer_->WakeUp() on PowerSaveTimer instance","Added null pointer check before calling power_save_timer_->WakeUp() for safety","Method name case correction: WakeUP → WakeUp (uppercase P vs lowercase p)"]

### [mydazy-p30-v2] Removed Nonexistent ui.h Header Include

["Include directive #include \"ui.h\" referenced nonexistent file in project","Removed from main/boards/mydazy-p30-4g/mydazy_p30_board.cc to fix compilation","Required UI headers already included: ui_display.h and control_center.h"]

### [mydazy-p30-v2] Fixed CONFIG_BOARD_TYPE_MYDAZY_P30 Missing UiDisplay Sources

["Added ui_display.cc source compilation: list(APPEND SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/boards/mydazy-p30/ui_display.cc)","Added boards/mydazy-p30 include directory for ui_display.h header resolution","Base P30 configuration now matches AI_LX_ML307 4G variant configuration","Ensures both WiFi-only and 4G variants compile with full UiDisplay UI system"]

### [mydazy-p30-v2] P32 board compilation failures revealed after board switch

["mydazy_p30_board.cc fails compilation with override errors for GetI2cBus, SetPowerSaveMode, and CleanupDisplay methods","Multiple missing class members: Board::WakeUP, Axs5106lTouch::InitializeHardware, WifiStation::GetInstance, SystemReset::FactoryReset","Touch driver method InitializeInput does not exist, should be Initialize","DISPLAY_LCD_TE pin definition has negative left shift count causin

### [mydazy-p30-v2] DISPLAY_LCD_TE set to GPIO_NUM_NC causes negative left shift warning

["DISPLAY_LCD_TE defined as GPIO_NUM_NC at line 69 of config.h","Commented-out alternative definition shows GPIO_NUM_40 was intended TE pin","mydazy_p30_board.cc line 182 attempts (1ULL << DISPLAY_LCD_TE) bit shift with NC value","Compiler warning: left shift count is negative when shifting by GPIO_NUM_NC","LCD TE (Tearing Effect) signal marked as reserved/unused in comments"]

### [mydazy-p30-v2] Fixed DISPLAY_LCD_TE GPIO configuration to skip when set to NC

["Modified InitializeGpio() in mydazy_p30_board.cc lines 177-191","Added if (DISPLAY_LCD_TE != GPIO_NUM_NC) guard around GPIO configuration","Prevents compiler warning about negative left shift count","TE (tearing effect) signal pin unused in P32 board, defined as GPIO_NUM_NC","GPIO configuration now only executes when pin is validly defined"]

### [mydazy-p30-v2] Added SDL2 Library Directory to Linker Search Path

["Added target_link_directories() call for mydazy_simulator executable","Configured linker to search SDL2_LIBRARY_DIRS before linking","Modification inserted before existing target_link_libraries() call","Uses pkg-config SDL2_LIBRARY_DIRS variable already available from pkg_check_modules"]

### [mydazy-p30-v2] Build Failure: lv_snapshot_take API Not Available

["Compilation of simulator/main.cpp fails at line 125 with undeclared identifier lv_snapshot_take","LVGL libraries (lvgl and lvgl_thorvg) build successfully at 95% completion before main.cpp compilation error","save_screenshot() function requires lv_snapshot_take() to render LVGL object tree to ARGB8888 buffer for BMP export","UDOUR reference implementation uses lv_snapshot_take successfully indic

### [mydazy-p30-v2] Enable LVGL Snapshot Feature for Screenshot Support

["simulator/lv_conf.h now defines LV_USE_SNAPSHOT=1 in EXTRA COMPONENTS section alongside LV_USE_GIF","LVGL snapshot feature was disabled by default causing lv_snapshot_take() to be undeclared during compilation","Snapshot API enables rendering LVGL object tree to independent buffer for screenshot export without SDL surface dependency"]

### [mydazy-p30-v2] Simulator Build Successful After Enabling Snapshot Feature

["Simulator build completed 100% successfully after enabling LV_USE_SNAPSHOT in lv_conf.h","All UI page components compiled: pomodoro_page, alarm_page, todo_page, photo_wall_page, brain_info_page, time_room_page, content_hub_page, control_center","LVGL and lvgl_thorvg libraries built successfully with harmless empty table of contents warning for thorvg archive","mydazy_simulator executable created

### [mydazy-p30-v2] Screenshot Memory Allocation Failures During Auto-Test

["Auto-test executes successfully navigating all 9 pages plus control center with correct page switching validated","Every screenshot fails with \"lv_draw_buf_create_ex: No memory: 284x240, cf: 16, stride: 1136, 272640Byte\" error in lv_draw_buf.c:297","LVGL memory pool configured at 256KB (LV_MEM_SIZE in lv_conf.h) insufficient for 272,640 byte ARGB8888 snapshot buffer","Several pages show \"Cann

### [mydazy-p30-v2] Increase LVGL Memory Pool to 2MB for Snapshot Buffer Allocation

["simulator/lv_conf.h LV_MEM_SIZE increased from 256KB to 2MB (2,097,152 bytes) to resolve snapshot allocation failures","Snapshot buffer requires 272,640 bytes for 284×240 ARGB8888 format leaving insufficient space in original 256KB pool","Desktop simulator environment has abundant memory making 2MB allocation reasonable unlike embedded ESP32 constraints","Configuration comment updated to indicat

### [mydazy-p30-v2] Set Parent Container for Lazy Page Initialization

["sim_display.cpp InitPageModules() now calls EnsureParent(lv_screen_active()) for all 7 feature pages after construction","UiPageBase::Show() automatically calls Create(parent_) when page not yet created if parent_ is set, enabling lazy initialization pattern","Previous implementation created page objects but didn't set parent causing \"Cannot show page: not created and no parent set\" warnings",

### [mydazy-p30-v2] EnsureParent() Protected Access Violation

["All 7 EnsureParent() calls in sim_display.cpp fail with \"EnsureParent is a protected member of UiPageBase\" compilation errors","UiPageBase::EnsureParent() method has protected access level preventing external callers from setting parent container","Lazy initialization pattern requires parent to be set before Show() can auto-create page UI","Protected access implies EnsureParent() was designed 

### [mydazy-p30-v2] Eager Page Creation for Desktop Simulator

["sim_display.cpp InitPageModules() now calls Create(lv_screen_active()) directly on all 7 pages instead of attempting EnsureParent()","Create() is public virtual method in UiPageBase making it the correct API for external page initialization","Desktop simulator with 2MB LVGL memory pool can afford eager page creation unlike memory-constrained embedded hardware","Comment updated to explain rationa

### [mydazy-p30-v2] Simulator Build Successful After Eager Page Creation Fix

["Simulator build completed successfully after switching to Create() for page initialization","All compilation errors resolved by using public API instead of protected member access","mydazy_simulator executable created with eager page initialization and 2MB memory pool"]

### [mydazy-p30-v2] RGB565 Byte Order Correction in Binary Image Parser

["RGB565 data interpretation changed from big-endian to little-endian in ParseBinImage function","Byte order changed from `(src[i*3] << 8) | src[i*3+1]` to `src[i*3] | (src[i*3+1] << 8)`","Fix applies to RGB565A8 interleaved format (0x0B) stored as 3 bytes per pixel","Comment updated from \"大端序，SPI LCD 常见\" to \"小端序\" reflecting correct endianness","Change affects color conversion from RGB565 to A

### [mydazy-p30-v2] Variable Name Consistency Fix

["Variable reference `dsc->data = argb_data` changed to `dsc->data = argb_buf`","Fix corrects inconsistency introduced when buffer variable was renamed in previous edit","Variable argb_data was renamed to argb_buf but one reference was missed","Bug would have caused compilation error due to undefined variable argb_data"]

### [mydazy-p30-v2] Test build fails with undefined font symbols at link time

["Test build completes 100% of LVGL compilation but fails during final linking of mydazy_tests executable","Linker reports undefined symbol _font_puhui_20_4 referenced from ui_helpers.cc.o in CreatePageHeader, CreateBackButton, CreateLabel, CreateIconButton functions","CMakeLists.txt defines BUILTIN_TEXT_FONT=font_puhui_20_4 and includes managed_components/78__xiaozhi-fonts/include directory","Fon

### [mydazy-p30-v2] Font linking resolved with lightweight stub implementation

["Created test/font_stubs.c with minimal lv_font_t descriptors for font_puhui_20_4, font_awesome_20_4, and font_awesome_30_4","Font stubs contain dummy glyph data (single space character) sufficient for linking but avoiding full font file compilation","Added font_stubs.c to TEST_SOURCES in test/CMakeLists.txt","Build succeeded with 100% completion after reconfiguration, linking mydazy_tests execut

### [mydazy-p30-v2] Fixed system crash during mode switch by adding retry logic and graceful error handling to audio codec

["Replaced ESP_ERROR_CHECK with manual error handling using esp_err_t return values","Added 3-attempt retry loop with 100ms delays for esp_codec_dev_open() to handle transient I2C issues","EnableOutput now returns early on failure instead of aborting system via ESP_ERROR_CHECK","Volume setting failure logs warning but continues operation rather than crashing","Codec close failure logs warning but 

### [mydazy-p30-v2] Audio codec I2C timeout causes crash on board mode switch

["Triple-click button triggers switch from 4G (ML307) to WiFi board mode","Audio playback attempt during mode switch causes I2C device 0x30 (audio codec) timeout","BoxAudioCodec::EnableOutput() calls esp_codec_dev_open() which fails with ESP_FAIL","ESP_ERROR_CHECK macro at box_audio_codec.cc:235 aborts instead of gracefully handling I2C failure","I2S channel disable fails with \"channel has not be

### [mydazy-p30-v2] Fixed ESP_ERROR_CHECK abort in audio codec I2C timeout during board switching

["ISSUE-17 documents P0 crash: EnableOutput ESP_ERROR_CHECK caused abort on I2C timeout, marked as fixed 2026-03-23","Trigger chain: 4G modem disconnected → triple-click WiFi config mode → audio codec reinitialize → I2C bus timeout → ESP_ERROR_CHECK abort at box_audio_codec.cc:235","Fix implements 3-retry mechanism with 100ms intervals plus error logging instead of ESP_ERROR_CHECK abort","ISSUE-18

### [mydazy-p30-v2] Fixed Timer Resource Leak in AudioService Destructor

["AudioService destructor now stops and deletes audio_power_timer_ before object destruction","Timer cleanup added at lines 60-63 in main/audio/audio_service.cc following existing destructor cleanup pattern","Fix prevents ESP timer handle leak when AudioService object is destroyed during service lifecycle changes","Cleanup includes null pointer check, esp_timer_stop(), and esp_timer_delete() calls

### [mydazy-p30-v2] Fixed Memory Leak in StartVolumeTask When Task Creation Fails

["StartVolumeTask in mydazy_p30_board.cc now captures xTaskCreatePinnedToCore return value for error checking","Task parameter tuple allocated before task creation and stored in params variable for proper cleanup","Error handler deletes params tuple if task creation fails (ret != pdPASS) preventing memory leak","Running flag reset to false on task creation failure to maintain correct state synchro

### [mydazy-p30-v2] Applied Volume Task Memory Leak Fix to P32 Board Variant

["StartVolumeTask in mydazy-P32/mydazy_p30_board.cc now includes identical error handling as mydazy-p30-4g variant","Fix ensures consistent resource leak prevention across both P30-4G and P32 hardware variants","Error handling pattern now standardized: allocate params, check xTaskCreatePinnedToCore return, cleanup on failure","Both board variants now properly delete params tuple and reset running 

### [mydazy-p30-v2] Replaced portMAX_DELAY with 5-second timeout in main event loop

["Main event loop in application.cc now waits maximum 5 seconds for events instead of infinite wait","Timeout implemented using pdMS_TO_TICKS(5000) macro for 5000 milliseconds","Timeout handling added with continue statement to retry event wait after timeout expires","Event group waits for ALL_EVENTS including errors, network changes, audio events, and state changes","Comment added in Chinese indi

### [mydazy-p30-v2] Replaced portMAX_DELAY with 500ms timeout in audio input task

["AudioInputTask in audio_service.cc now waits maximum 500 milliseconds for audio events","Audio input task monitors three event types: audio testing, wake word detection, and audio processor","Timeout cases handled by existing fallback delay logic at end of loop","500ms timeout chosen as appropriate balance for real-time audio processing responsiveness"]

### [mydazy-p30-v2] Replaced portMAX_DELAY with 1-second timeout in AFE wake word detection

["AudioDetectionTask in afe_wake_word.cc now uses 1-second timeout for both event wait and audio fetch operations","Event group wait monitors DETECTION_RUNNING_EVENT with 1000ms timeout instead of infinite wait","AFE interface fetch_with_delay call also changed from infinite to 1000ms timeout","Existing error handling for null results and ESP_FAIL covers timeout scenarios","Wake word detection use

### [mydazy-p30-v2] Replaced portMAX_DELAY with 1-second timeout in AFE audio processor

["AudioProcessorTask in afe_audio_processor.cc now uses 1-second timeout for event wait and audio fetch operations","Event group wait monitors PROCESSOR_RUNNING state with 1000ms timeout instead of infinite wait","AFE interface fetch_with_delay call changed from infinite to 1000ms timeout for voice processing","Audio processor handles voice processing including AEC, VAD, noise suppression, and AGC

### [mydazy-p30-v2] Replaced portMAX_DELAY with 5-second timeout in LED event task

["EventTask in gpio_led.cc now waits maximum 5 seconds for task notifications instead of infinite wait","Task notification triggered by LEDC fade completion callback from interrupt context","On timeout, OnFadeEnd() still executes to maintain LED fade animation continuity","LED fade task handles breathing animation effects for various device states","5-second timeout chosen to allow periodic fade p

### [mydazy-p30-v2] Replaced portMAX_DELAY with 5-second timeout in camera JPEG queue sends

["Two xQueueSend operations in esp32_camera.cc now use 5-second timeout instead of infinite wait","JPEG encoder sends compressed image chunks through FreeRTOS queue to HTTP upload thread","Timeout prevents encoder thread from blocking indefinitely if queue consumer stops","Applied to both normal chunk sends and error/terminator chunk sends","Part of camera image explanation feature that uploads ph

### [mydazy-p30-v2] Replaced portMAX_DELAY with 5-second timeout in camera JPEG queue receives

["Two xQueueReceive operations in esp32_camera.cc now use 5-second timeout instead of infinite wait","First timeout added to error cleanup path that drains queue when HTTP connection fails","Second timeout added to main upload loop that receives JPEG chunks for transmission","Timeout prevents consumer thread from blocking indefinitely if producer fails","Existing error handling detects timeout and

### [mydazy-p30-v2] Replaced portMAX_DELAY with 5-second timeout in video camera JPEG queue sends

["Two xQueueSend operations in esp_video.cc now use 5-second timeout instead of infinite wait","Video camera uses same JPEG encoding pipeline pattern as still camera for AI image explanation","Encoder sends compressed chunks through queue to HTTP upload thread","Applied to both normal chunk sends and error/terminator chunk sends in video path","Complements existing camera fixes for consistent time

### [mydazy-p30-v2] Replaced portMAX_DELAY with 5-second timeout in video camera JPEG queue receives

["Two xQueueReceive operations in esp_video.cc now use 5-second timeout instead of infinite wait","First timeout added to cleanup path that drains queue when HTTP connection fails","Second timeout added to main upload loop receiving JPEG chunks for transmission","Mirrors earlier esp32_camera.cc fixes for consistent timeout handling","Completes bidirectional timeout protection for video camera JPEG

### [mydazy-p30-v2] Replaced portMAX_DELAY with 30-second timeout in RNDIS network initialization

["StartNetwork() in rndis_board.cc now waits maximum 30 seconds for IP address instead of infinite wait","RNDIS provides USB network connectivity for devices without WiFi or Ethernet","Timeout covers DHCP negotiation and USB device enumeration time","30-second timeout chosen as reasonable upper bound for network initialization","Wait occurs during board startup after USB RNDIS driver installation"

### [mydazy-p30-v2] Resolved task_config.h dependency with inline constant definitions

["Removed #include \"task_config.h\" from media_player.cc line 14","Added inline definitions: TASK_STACK_MEDIA_PLAYER = 8192 bytes (4096×2), TASK_PRIORITY_MEDIA_PLAYER = 3, TASK_CORE_BUSINESS = 1","Stack size of 8KB allocated for media player task to handle decoder operations and callback processing","Priority level 3 provides medium-priority scheduling in FreeRTOS task hierarchy","Core assignment

### [mydazy-p30-v2] Fixed CloseAudioChannel signature to match Protocol base class

["Updated WebsocketBaiduProtocol::CloseAudioChannel signature from void CloseAudioChannel() to void CloseAudioChannel(bool send_goodbye = true)","Signature now matches Protocol base class pure virtual method at protocol.h line 68","Default parameter value (true) matches base class default, maintaining expected behavior","Fixes compilation error from method signature mismatch between override and b

### [mydazy-p30-v2] Updated CloseAudioChannel implementation signature to accept send_goodbye parameter

["Updated method signature from void CloseAudioChannel() to void CloseAudioChannel(bool send_goodbye) at line 311","Signature now matches header declaration, resolving linker error from declaration-definition mismatch","Implementation body unchanged, parameter available but not yet utilized in current logic","Method performs atomic media_ready_ flag reset and conditional goodbye message sending ba

### [mydazy-p30-v2] Added missing atomic and chrono headers to Baidu protocol

["Added #include <atomic> to websocket_baidu_protocol.h after #include <memory> at line 10","Added #include <chrono> for std::chrono::steady_clock::time_point usage at line 11","Fixes 7 compilation errors: incomplete type for std::atomic<bool> (5 fields) and std::atomic<int> (2 fields)","Resolves errors for licensed_, media_ready_, is_speaking_, is_playing_music_, device_info_sent_, audio_send_fai

### [mydazy-p30-v2] Added public SendRawText wrapper to expose protected Protocol::SendText

["SendRawText method added as public member at line 79 in protocol.h","Method implemented inline as one-line wrapper delegating to protected SendText method","Preserves encapsulation by keeping SendText protected for derived class implementation","Enables Application::SendProtocolText to send text through protocol without breaking access control","Uses wrapper pattern to bridge public interface an

### [mydazy-p30-v2] Updated SendProtocolText to use public SendRawText wrapper

["Changed line 994 in application.cc from protocol_->SendText(text) to protocol_->SendRawText(text)","Fixes compilation error \"Protocol::SendText is protected within this context\"","Uses newly added public wrapper method that delegates to protected implementation","Maintains encapsulation while providing required functionality"]

### [mydazy-p30-v2] Added live_companion.h include to resolve incomplete type error

["Added #include \"live_companion.h\" at line 13 in application.cc","Include placed after settings.h and before conditional includes","Resolves \"invalid application of 'sizeof' to incomplete type 'LiveCompanion'\" error","Enables unique_ptr<LiveCompanion> member to properly manage object lifecycle","Forward declaration in application.h was insufficient for unique_ptr operations"]

### [mydazy-p30-v2] Stubbed out ProcessCustomContent call in remote_cmd file sync handler

["Removed Ota instance creation and ProcessCustomContent call at lines 201-207","Added TODO comment indicating ProcessCustomContent needs porting from 189 codebase","Added ESP_LOGW warning \"File sync not yet implemented in V2\" for runtime visibility","Preserved emotion setting functionality from success path","Removed failure alert path since no actual download occurs","Files sync command will n

### [mydazy-p30-v2] Fixed missing sound constant and deep sleep method in OnSleep handler

["Changed sound constant from Lang::Sounds::OGG_UNBUNDLE to OGG_DISCONNECT at line 326","Replaced Board::EnterDeepSleep call with TODO comment, warning log, and esp_restart() at lines 328-330","TODO comment indicates \"V2 使用 Board 特定的深睡方法\" (V2 uses Board-specific deep sleep method)","Warning log states \"Deep sleep not yet implemented in V2 RemoteCmd\"","Device will now reboot instead of entering

### [mydazy-p30-v2] Fixed PhotoWallPage PSRAM leak by freeing GIF cache on Destroy

["PhotoWallPage preloads 6 GIF files into PSRAM cache during Create() to avoid file I/O during navigation","Previous implementation only freed GIF cache in destructor, not in Destroy() method","Pages can be destroyed and recreated during UI lifecycle without object deletion, causing PSRAM to remain allocated","FreeAllGifs() now called in Destroy() ensures PSRAM is released when page is destroyed, 

### [mydazy-p30-v2] Fixed PowerManager ADC read error handling to prevent crashes in timer callback

["ReadBatteryAdcData() is called from esp_timer callback every second to read battery voltage via ADC","ESP_ERROR_CHECK macro calls abort() on any ESP error, causing immediate system crash","Calling abort() from timer callback context is unsafe and can leave system in inconsistent state","New implementation checks error code, logs warning with esp_err_to_name(), and returns gracefully on ADC read 

### [mydazy-p30-v2] Fixed ADC calibration error handling in PowerManager timer callback

["adc_cali_raw_to_voltage converts raw ADC values to calibrated voltage readings using eFuse calibration data","Function is called within ReadBatteryAdcData() timer callback when do_calibration1_chan0_ is true","Original ESP_ERROR_CHECK would abort system if calibration handle became invalid or eFuse read failed","New error handling logs warning and returns early, allowing system to retry on next 

### [mydazy-p30-v2] Fixed build_default_assets.py to accept multiple extra_files paths

["Modified scripts/build_default_assets.py line 827 to add nargs='*' parameter to --extra_files argument","Build was failing when CMake passed two paths: main/assets/fonts and main/assets/images to --extra_files","Argparse now accepts zero or more values for --extra_files instead of single value","The build_assets_integrated function already had logic to handle extra_files_path as list or string"]

### [mydazy-p30-v2] Task Watchdog Timeout During LVGL UI Initialization

["Task watchdog timeout occurs at 11217ms and 21217ms during startup","Main task hangs in UiDisplay::CreateEmotionUI() at line 433 in ui_display.cc","Backtrace shows blocking in lv_obj_set_style_pad_all and lv_obj_invalidate operations","IDLE0 task on CPU 0 cannot reset watchdog while main task processes LVGL operations","Hang occurs during MyDazyP30Board constructor calling InitializeDisplay()"]

### [mydazy-p30-v2] Fixed watchdog timeout causing device restarts during UI initialization

["Device was continuously restarting due to main task watchdog timeout (10 second limit) during UI initialization","CreateEmotionUI() and InitPageModules() were blocking main task for over 10 seconds","Refactored UiDisplay constructor to defer CreateEmotionUI() and InitPageModules() to LVGL task using lv_timer_create with 100ms delay","Boot logo and clock UI remain synchronously created as they ar

### [mydazy-p30-v2] UI initialization moved from constructor timer to explicit SetupUI method

["UiDisplay constructor no longer creates lv_timer with 50ms callback for UI initialization","New SetupUI() method explicitly creates UI components: CreateBootUI(), CreateClockUI(), CreateEmotionUI(), InitPageModules()","SetupUI() calls LcdDisplay::SetupUI() first to create default theme and container before custom UI","SetupUI() is designed to be called from Application::Initialize after Board co

### [mydazy-p30-v2] Disabled ULP SC7A20H accelerometer wake functionality to fix boot issues

["All ULP and SC7A20H accelerometer code wrapped with `#if CONFIG_EN_SC7A20H_WAKE` conditional compilation guards","ulp_wakeup.h header file provides empty stub implementations when ULP feature is disabled","Firmware builds successfully at 3.01MB (0x301ed0 bytes) with 24% free space remaining in partition","Bootloader binary size is 0x3f80 bytes with 50% free space","Strategy follows xiaozhi-esp32

### [mydazy-p30-v2] Doubled Audio Output Task Stack Size to 8KB

["Changed audio_output task stack size from 2048*2 (4096 bytes) to 4096*2 (8192 bytes) in audio_service.cc line 148","Stack size increase targets CONFIG_USE_AUDIO_PROCESSOR enabled build configuration","Task runs on Core 1 with priority 4 and handles audio playback queue operations","Audio output task calls codec_->OutputData which invokes esp_codec_dev_write and i2s_channel_write where crash occu

### [mydazy-p30-v2] Increased Audio Output Task Stack for Non-Processor Configuration

["Changed audio_output task stack size from 2048 bytes to 4096 bytes in audio_service.cc line 162","Stack increase applies to #else branch when CONFIG_USE_AUDIO_PROCESSOR is disabled","Both audio processor configurations now have increased stack allocations for audio output task","With audio processor enabled: 8KB stack, without audio processor: 4KB stack","Maintains consistent approach to address

### [mydazy-p30-v2] Fixed Crash from Accessing Empty Audio Playback Queue After Timeout

["LoadProhibited crash at 0x84963a20 caused by dereferencing invalid pointer from empty deque front() call","condition_variable::wait_for returns after 500ms timeout even when predicate is false and queue is empty","Code attempted audio_playback_queue_.front() without verifying queue non-empty after timeout","Added explicit empty() check before front() access in AudioOutputTask at line 301","Calli

### [mydazy-p30-v2] AudioOutputTask crash fix with condition_variable wait semantics

["AudioOutputTask crashed at memcpy with EXCVADDR 0x84963a20 when accessing empty audio_playback_queue_","condition_variable::wait_for(500ms) can timeout and return with predicate false, but code assumed queue was non-empty","All wait_for calls in audio_service.cc changed to wait with predicates ensuring queue state before access","Crash occurred in BoxAudioCodec::Write → i2s_channel_write → memcp

### [mydazy-p30-v2] Restored asset configuration for P30-4G board to fix emoticon display

["Verified existence of assets/fonts directory containing font_english_30_4.bin and font_number_88_4.bin","Verified existence of assets/images directory containing app_alarm.bin, app_chat.bin, and other UI images","Verified existence of boards/mydazy-p30/ui/emoji directory containing angry.gif, confident.gif, confused.gif and other emotion GIFs","Modified main/CMakeLists.txt lines 97-104 to restor

### [mydazy-p30-v2] Remove Incorrect SPIFFS Mount for Memory-Mapped Assets Partition

["MountStorage() function removed from mydazy_p30_board.cc completely","Assets partition uses esp_mmap_assets format for direct memory-mapped access","Assets class reads partition data via mmap without filesystem layer","Previous SPIFFS mount attempt with format_if_mount_failed=true would corrupt mmap data","SPIFFS mount error -10025 was caused by attempting to mount non-SPIFFS partition"]

### [mydazy-p30-v2] 修复 Assets 打包参数覆盖 BUG 并统一图标加载路径

["CMakeLists.txt 第 387 行和 392 行传递两次 --extra_files 参数，Python argparse nargs='*' 只保留最后一个值，导致 assets/images 目录 32 个图标文件被 cbin/mydazy 覆盖","状态栏图标(WiFi/4G/电池)同时存在两套: assets/icons/*.c 编译 C 数组(260KB 占 rodata) 和 assets/images/*.bin 分区文件(232KB mmap 零拷贝)","修复方案: 合并为单个 --extra_files 参数，两个目录作为多个值传入，删除 assets/icons/ 整个目录(15 个 .c 文件)","lcd_display.cc 三处 SetupUI 代码从 LV_IMAGE_DECLARE(ui_img_icon_xxx_png) + &amp;ui

### [mydazy-p30-v2] Compile error: constexpr variable using runtime LV_HOR_RES macro

["Compilation error at lcd_display.cc:1362 in ShowWifiQrCode function: \"call to non-'constexpr' function 'int32_t lv_display_get_horizontal_resolution(const lv_display_t*)'\"","Error triggered by line \"constexpr int kCenterW = LV_HOR_RES - kBarW * 2;\" attempting to use constexpr with runtime value","LV_HOR_RES macro from lvgl__lvgl/src/core/../display/lv_display.h:697 expands to function call r

### [mydazy-p30-v2] Fixed constexpr compile error by changing kCenterW to const int

["Modified line 1362 in lcd_display.cc from \"constexpr int kCenterW\" to \"const int kCenterW\"","Fix allows kCenterW to be evaluated at runtime when ShowWifiQrCode executes instead of requiring compile-time constant","constexpr requires compile-time evaluation but LV_HOR_RES expands to lv_display_get_horizontal_resolution() function call","const qualifier maintains read-only semantics while perm

### [mydazy-p30-v2] Hide QR Code After Activation Completes

["HandleActivationDoneEvent() now calls display->HideWifiQrCode() to dismiss QR code overlay","QR code cleanup occurs before showing version notification and success sound","Prevents activation QR code from persisting on screen after activation completes"]


## 三、会话学习要点（learned）

- **[mydazy]** Current webhook interface (/ota/webhook/send) only supports `tts` and `ai` message types via whitelist in DeviceOtaController.java:49. Push flow is transparent JSON passthrough: controller validates type and webhook signature, service layer queries device and 
- **[mydazy]** Webhook push system uses HMAC-SHA256 signature verification with nonce-based replay prevention (300s window). webhookKey format is whk_{deviceId}_{random} for device identification. DevicePlatformConfig stores webhook credentials in JSON field. Push flow: exte
- **[mydazy]** Webhook signature validation is working correctly. Device lookup retrieves device ID 1693278 successfully. The push flow reaches Xiaozhi platform but fails because local test data uses fake serial number (XZ_TEST_3C0F02) instead of real device serial. Webhook 
- **[mydazy]** Backend webhook system is fully implemented with signature-based authentication (key/secret), authorization generation endpoint (POST /iot/device/generateWebhookAuth), and external webhook entry point (POST /ota/webhook/send). Webhook configuration is stored i
- **[mydazy]** The webhook system now supports both legacy paths (/ota/webhook/send) and new streamlined path (/ota/push) for backward compatibility. Signature verification works correctly at the new endpoint. Push failures to 小智平台 with test serial numbers are expected behav
- **[mydazy]** Webhook system supports multiple types (tts, tti, ttai, ai). Legacy webhook paths (/webhook, /webhook/send) existed alongside /push endpoint. Authentication parameter was inconsistently named "key"
- **[mydazy]** Java backend successfully started via background task after initial classpath method failed. Webhook approach is event-driven and stateless, making it ideal for OpenClaw's AI agent gateway pattern with 100-1000 device scale. WebSocket provides lower latency an
- **[mydazy]** OpenClaw AI Gateway operates as an event-driven gateway where Webhook callbacks are optimal for current scale because: (1) OpenClaw doesn't need to maintain device connections - only callback URLs, (2) AI responses are low-frequency events (1-2 per conversatio
- **[mydazy]** Simplified authentication patterns significantly improve third-party integration user experience. Token-only mode eliminates the need for signature headers, making openclaw, n8n, and other webhook consumers easier to configure. Long text content (>100 chars) b
- **[mydazy]** AgentVectorMatchService was previously modified to handle graceful shutdown: uses AtomicBoolean shuttingDown flag, listens to ContextClosedEvent, and catches RedissonShutdownException to prevent errors during application shutdown when async vector cache warmin
- **[mydazy]** Device authentication can use stateless token approach with HMAC-SHA256 signature combining device ID and client ID. Token format Base64(deviceId:clientId:signature) allows server-side verification without Redis storage. Existing `/ota/push` endpoint was servi
- **[mydazy]** The current implementation used HMAC-SHA256 token generation with Base64 encoding for device STT push authentication. The new simplified approach aligns /ota/pushstt authentication with other OTA endpoints that use Device-Id + Client-Id headers directly, reduc
- **[mydazy]** The STT push endpoint was using a different authentication mechanism (token query parameters) than other OTA endpoints (header-based). This inconsistency required devices to pre-fetch tokens and added complexity with HMAC token generation and verification. Sta
- **[mydazy]** The pushstt endpoint's behavior depends on webhook configuration: it routes to `ttai` (TTS with AI processing) when webhooks are configured, and falls back to standard `tts` when no webhook is present.
- **[mydazy]** The OTA service uses Device-Id and Client-Id headers for client identification. The endpoint path structure combines context-path and Controller mapping without additional version prefixes. The service was running on port 5501 before being stopped
- **[mydazy]** Sa-Token supports dual-layer authentication bypass: method-level `@SaIgnore` annotations combined with URL pattern exclusions in configuration files for redundant protection
- **[mydazy]** The OTA system uses Sa-Token authentication framework with two bypass mechanisms: @SaIgnore annotation and URL exclusion list. Three push endpoints now configured for anonymous access: /ota/pushstt uses Device-Id + Client-Id authentication, while /ota/pushtts 
- **[mydazy]** Push notification system successfully delivers TTS messages through Baidu push service, returning HTTP 200 status codes and generating unique messageIds for tracking delivery (format: baidu_[timestamp]_[id])
- **[mydazy]** The endpoint now automatically transforms shorthand format {"type":"tts","text":"..."} into the full format {"type":"tts","data":{"text":"..."}} internally before pushing to devices. Both formats are supported despite the initial request to only support one fo
- **[mydazy]** pushStt endpoint now implements flexible type handling: explicit type specification via `{"type":"tts","text":"xxx"}` or automatic type detection based on webhook configuration presence (webhook configured → ttai, no webhook → tts). Current format design align
- **[mydazy]** Webhook configuration is stored in `iot_device.platform_config` JSON field under a `webhook` object containing webhookKey, webhookSecret, enabled flag, and callbackUrl. Configuration can be accessed via PC management backend (Device Management → Webhook Author
- **[mydazy]** The OTA system supports bidirectional TTS/STT flows: devices send speech-to-text via /ota/pushstt, server can forward to configured webhooks for AI processing, and results are pushed back to devices as text-to-speech via ttai type messages. The /ota/pushtts en
- **[mydazy]** The callback logic was streamlined to: return R.ok() immediately when no callbackUrl is present, return fixed messages ("通知成功" for success, "通知失败" for failure) instead of parsing complex responses, and removed unnecessary response parsing logic
- **[mydazy]** Webhook configuration is stored in the `platform_config` LONGTEXT JSON field within the `iot_device` table, not as separate database tables. The configuration maps to a `DeviceWebhookConfig` object containing webhookKey (public token), webhookSecret (HMAC sign
- **[mydazy]** The system uses a JSON extension field pattern where all webhook configuration lives in the existing `iot_device.platform_config` LONGTEXT/JSON field. New features are added by nesting sub-objects in JSON (like the `webhook` sub-object of `DeviceWebhookConfig`
- **[mydazy]** The API endpoint is accessible and returns SSE streaming format with 200 OK status. Authentication token is valid. The service returns chat.completion.chunk objects with model "nationwide". However, the backend AI model service returns an error message "我遇到一点问
- **[mydazy]** The Baidu voice chat integration uses a two-tier configuration system: devices send Device-Id headers to retrieve their bound agent configuration (AgentBaidu table), which determines whether to use custom roles/prompts or default presets like "情感陪伴老师" (Emotion
- **[mydazy]** Baidu's VoiceChat API supports 30+ configuration fields organized into 7 categories: core config (userId, deviceId, sceneRole), sceneRoleCfg (name, prompt), TTS config (provider, voice ID, volume, speed), ASR config (lang, codec, VAD, long audio mode), LLM con
- **[mydazy]** The parameter flow works in three layers: (1) Backend calls generateAIAgentCall with server-configured parameters, (2) Device connects and sends additional/override parameters via WebSocket, (3) Device-sent values override server configuration. Parameters span
- **[mydazy]** The configuration system has 22 parameters across 8 categories; 16 parameters are device-side only; configuration flows from database → Service → Provider → Baidu API → device WebSocket; server-side and device-side configurations have different responsibilitie
- **[mydazy]** Baidu Voice Chat uses WebSocket connection with AK/SK authentication requiring accessKey, secretKey, appId, and resourceId. Configuration supports environment variable overrides using pattern ${BAIDU_RTC_ACCESS_KEY:default_value}. WebSocket endpoint is wss://r
- **[mydazy]** Baidu Cloud RTC parameters fall into two categories: migratable parameters (audio codec, bitrate, ASR VAD settings, TTS delays, interruption controls) and hardware-dependent parameters (device model, cloud 3A/AEC URLs, voice fingerprinting) that must remain de
- **[mydazy]** The Baidu AI Agent configuration already contains fields for 12 extensible features that are defined but not yet enabled. The top three high-priority features (TTS voice tuning for volume/speed/pitch, LLM model switching for DeepSeek/Wenxin/custom models, and 
- **[mydazy]** The ai_agent_baidu database table had llm_model, lang_code, and voice_config fields that existed but were not being read - the code was hardcoded to use DEEPSEEK_3_2 for LLM, "zh" for language, and 1000189 for default voice ID
- **[mydazy]** Baidu RTC integration uses ERNIE-4.0 LLM with emotion recognition that injects emotional context into responses and modulates TTS output. System supports voice fingerprint recognition, configurable interruption keywords, VAD timeout settings, and MCP protocol 
- **[mydazy]** TTS voice configuration was incomplete, lacking essential parameters for controlling speech speed (语速) and intonation/pitch (语调)
- **[mydazy]** Baidu Cloud's TTS API has two mutually exclusive modes: simplified mode (tts_voice only) and full control mode (tts + tts_url with vol/spd/pit parameters). When both are set simultaneously, tts_voice takes precedence and tts_url custom parameters are ignored
- **[mydazy]** The Baidu voice chat system uses voice model IDs (switched from 1000006 to 1000374), supports configurable TTS speed settings, and has an auto-pause mechanism that exits chat after inactivity. The ttsSpeed parameter defaults to 2.0 when not set in database, pr
- **[mydazy]** The Baidu voice chat system uses a two-stage configuration approach: server-side provides complete configuration with conservative defaults (designed for devices without hardware AEC), while device-side can send override parameters via WebSocket to optimize fo
- **[mydazy]** The Baidu voice chat system uses a layered configuration approach where server-side provides base settings that can be overridden by device-side parameters. ASR VAD (Voice Activity Detection) has two key parameters: timeout duration (how long to wait in silenc
- **[mydazy]** Baidu TTS/ASR configuration uses `langCode` from database (`agent.getLangCode()`), currently set to `zh-CN`, but Baidu API documentation specifies `zh` for Mandarin Chinese. The `buildTtsUrlInternal` function intentionally omits `vol` and `pit` parameters when
- **[mydazy]** The Baidu RTC AI Provider uses a dual-mode TTS system: CCLONE prefix for voice cloning and numeric IDs for built-in voices. TTS parameters were previously nullable, leading to incomplete tts_url output. Language codes required normalization from zh-CN/zh_CN to
- **[mydazy]** TTS configuration uses three key parameters: vol (volume, 0.5-2.0) controls loudness for device output; spd (speed, 0.5-2.0) controls playback rate to reduce waiting time; pit (pitch, 0.5-2.0) controls voice tone with 1.0 being most natural. Current defaults a
- **[mydazy]** Cloud 3A official documentation only exposes enable/disable toggle without granular controls; AEC sensitivity directly impacts interruption behavior - too strong suppresses user speech during TTS playback, too weak causes TTS echo to trigger false ASR detectio
- **[mydazy]** The plus-app is a uni-app/HBuilderX project targeting iOS App Store with version 5.5.0. It requires Bluetooth, camera, location, local network, photo library, and microphone permissions for smart device connectivity. Production API endpoint is https://www.myda
- **[mydazy]** The file previously contained important BLE improvements including reliableBleWrite retry mechanism, MTU negotiation, Huawei heartbeat keepalive, and platform-specific adaptations (iOS vs Android/Huawei). The WiFi scanning UI uses status messages that control 
- **[mydazy]** BluFi protocol's write characteristic 0xFF01 has property "Write Without Response" only and does not support "Write With Response" mode. Android was incorrectly using standard `write` mode which caused error 10007. Cross-platform BLE write modes behave differe
- **[mydazy]** The WiFi scan button state is controlled by `isScanning` flag which is set to false only when `parseWifiList` receives actual WiFi list data, or after a 15-second timeout. The implementation continues waiting for WiFi list data even if the BLE write operation 
- **[mydazy]** The scanning button state is currently controlled solely by receiving valid WiFi list data from `parseWifiList`. Button behavior: enters continuous rotation on page load, continues spinning even when BLE write operations fail, only stops when device successful
- **[mydazy]** The 10007 error occurs because the 500ms delay between enableNotification() and scanWifiList() is insufficient - ESP32 needs more time to initialize its security layer after receiving BLE_CONNECT event. Despite the error, WiFi lists still return successfully b
- **[mydazy]** ESP32 devices require longer initialization time (1500ms vs 500ms) after enabling BLE notifications to properly handle BluFi protocol commands. WiFi list state management during asynchronous scan operations causes UI flashing when arrays are cleared before new
- **[mydazy]** The current 3-page architecture has a critical race condition: provisioning commands are sent in chooseWifi, then navigateTo is called to bindDevice, but the 200-500ms page navigation window creates a timing gap where device provisioning results may arrive bef
- **[mydazy]** The 2-page architecture is optimal: chooseDevice must remain independent (Bluetooth scanning is a distinct workflow stage), but chooseWifi and bindDevice should merge into a single page with state machine transitions. This eliminates all BLE cross-page issues 
- **[mydazy]** The original two-page architecture introduced several technical issues: ~120 lines of duplicated BLE connection code per page, ~80 lines of duplicated frame parsing code, a ~700ms data loss window during page navigation, complex URL parameter passing for devic
- **[mydazy]** BLE timing is critical for reliable configuration: MTU negotiation can interfere with write characteristics; retry intervals need sufficient spacing (1 second) for BLE stack recovery; ESP32 needs adequate initialization time (2 seconds); BLE scan duration of 1
- **[mydazy]** BLE write characteristic may not be immediately ready after connection establishment despite successful service/characteristic discovery and notification subscription; ESP32 needs time for BLE_CONNECT event processing and security layer initialization; write c
- **[mydazy]** Claude CLI is configured to execute a stop hook at .claude/hooks/stop.js but the file does not exist at the expected path. The error is non-blocking (doesn't prevent stop operation) but triggers repeatedly on every session end, causing recurring noise and user
- **[mydazy]** Old code succeeded accidentally: scan command failed immediately, then 10-30 seconds of user interaction (browsing WiFi list, entering password) gave BLE stack time to stabilize naturally before configuration command. New refactored code fails because `tryWrit
- **[mydazy]** Bluetooth provisioning write issues can be caused by three main factors: BLE characteristic not supporting writeNoResponse property, empty UUID strings being passed to write operations, or timing issues in the BLE stack implementation
- **[mydazy]** Error 10007 occurs when using 'writeNoResponse' write mode on BLE characteristics that don't support it. BLE characteristics have properties that must be queried (getBLEDeviceCharacteristics) to determine which write modes are supported: writeNoResponse vs wri
- **[mydazy]** ESP32 network configuration success notification uses a specific format: length byte followed by SSID data. The handleResult function requires 3-layer judgment logic: (1) JSON format detection for device info, (2) WiFi SSID presence check for configuration suc
- **[mydazy]** The BLE connection workflow was over-instrumented with 5 steps that could be consolidated to 3 meaningful steps (scanning, connecting, configuring). Several reactive refs (svcId/wrId/rdId/wrType) didn't need to be reactive since they're only set once during co
- **[mydazy]** wifiConfig.vue simplified from 1832 lines (2 files) to 498 lines with complete functionality. ESP32 device successfully receives SSID and password but encounters btc_blufi_recv_handler error claiming payload is 8192 bytes (exceeding expected packet length), ca
- **[mydazy]** The uni-app framework routing warning is harmless internal behavior. uni-app prioritizes `$page` object for routing, but during page initialization moments when `$page` isn't mounted yet, it falls back to the `route` attribute as a compatibility mechanism. Thi
- **[mydazy]** The WiFi configuration flow refactoring is complete with wifiConfig.vue (498 lines) as the new streamlined component. The old components (chooseWifi.vue, bindDevice.vue) are still functional and support manual binding, but are candidates for deprecation. utils
- **[mydazy]** The device configuration flow had redundant wifiConfig.vue that could be consolidated into chooseWifi.vue. The BluFi protocol functions are shared between WiFi configuration and manual device binding flows. The navigation path from chooseDevice to WiFi configu
- **[mydazy]** The original Bot page used a compact cell-group list layout with gray backgrounds, hidden usage instructions in a collapse component, and inline input fields. The page lacked clear visual hierarchy and authorization status indicators. Spacing and color scheme 
- **[mydazy]** Selected "带货搭子" as the product name leveraging the viral "搭子" (partner) social term for built-in shareability. Core differentiation is low cost (voice AI vs expensive digital human rendering) combined with AI-powered interaction capabilities and simple mobile 
- **[mydazy]** The project will be called `plus-maishou` (卖货搭子 - "Sales Companion"). Key technical requirements include: multi-platform deployment (iOS/Android APP + Mini Program), Bluetooth device pairing as a core feature, and following uni-app + Vue3 + TypeScript + WD UI 
- **[mydazy]** The app concept is "卖货搭子" (sales companion/buddy) - a sales assistance tool. The English name "SaleMate" provides a direct translation (Sale + Mate) that maintains the companion concept while being internationally accessible. Naming convention balances Chinese
- **[mydazy]** uni-app provides the best fit for this project because: (1) WeChat mini-program is the core user acquisition channel for target users (small/medium merchants), (2) team has existing plus-uniapp experience with reusable components (API wrappers, Bluetooth provi
- **[mydazy]** SaleMate requires integration between product inventory management, livestream task scheduling, IoT device control, and real-time analytics. The app follows a clear state machine for live task management (draft→scheduled→ongoing→completed/cancelled). Tech stac
- **[mydazy]** Figma MCP uses font names directly rather than CSS font-family syntax; requires fonts to be locally installed or available via Google Fonts; PingFang SC is the recommended Chinese font for macOS users as it's pre-installed and well-supported
- **[mydazy]** Claude Preview provides the most complete solution for UI prototyping with features including PingFang SC Chinese font support (native to Mac), 375px mobile viewport simulation, interactive state handling (clicks, tab switching), and adherence to product speci
- **[mydazy]** The SaleMate application follows a tab-based mobile interface pattern with distinct modes for managing products, live streaming sessions, sales promotions, and user data. Each tab has specific UI components: search and filtering for products, task cards with h
- **[mydazy]** The prototype renders correctly in the browser with proper Chinese font display and adheres to the product specification design standards. All 4 primary tab pages are functioning and visually complete, confirming that the foundational navigation structure and 
- **[mydazy]** LiveScript module follows the project's established four-layer architecture pattern matching the Ad module conventions: Entity with TenantEntity inheritance and snowflake IDs, BO/VO with AutoMappers and validation groups, Dao with comprehensive query building 
- **[mydazy]** Background agent "创建LiveTask后端CRUD" previously completed generation of 20 backend files across three entities (LiveTask, LiveTaskProduct, LiveInteraction) with full CRUD implementation including Entity/BO/VO/Mapper/DAO/Service/Controller layers. These files we
- **[mydazy-p30-udour]** Project uses LVGL framework on ESP32-S3 with multiple UI pages. Four testing approaches available: SquareLine Studio (visual GUI designer), SDL2 PC simulator, ESP-IDF + QEMU, and LVGL Web simulator. Display system implements DisplayLockGuard with 500ms timeout
- **[mydazy-p30-udour]** 70% of UI code is pure LVGL and portable without modification. Only 6 ESP-IDF API calls need stubbing (esp_timer_get_time, esp_task_wdt_reset, taskYIELD, esp_app_desc, heap_caps_get_free_size, SPI LCD Panel). UI code separation is excellent with clear layering
- **[mydazy-p30-udour]** LVGL UI code can run on desktop by creating stub implementations for all ESP-IDF dependencies, allowing UI testing without hardware. The simulator uses SDL2 to render a 284x240 window matching the actual display dimensions.
- **[mydazy-p30-udour]** Complete startup sequence verified: UI initializes with logo, brand text, clock, expressions, and page modules at 0.0s; rendering begins at 0.2s; boot logo hides and clock interface appears at 2.1s; BootAnimation callback confirms clock interface entry at 5.1s
- **[mydazy-p30-udour]** The system has 8 page types with a lazy-creation navigation pattern. SwitchToPage() is the core navigation method using hide-then-show logic. NextPage/PrevPage are unimplemented stubs. Modern pages inherit from UiPageBase with Create→Show→Hide→Destroy lifecycl
- **[mydazy-p30-udour]** LVGL timer callbacks enable non-blocking sequential test steps. SDL2 provides window resizing (SDL_SetWindowSize) and framebuffer capture (SDL_RenderReadPixels, SDL_SaveBMP) without external dependencies. The PageType enum and SwitchToPage() method enable prog
- **[mydazy-p30-udour]** LVGL simulator successfully provides real-time preview for local development. Window scaling (`--scale 2`) improves interaction testing. File system dependencies (SPIFFS) cause expected asset loading failures in simulator but don't break core functionality. Ex
- **[mydazy-p30-udour]** Control center background transparency was causing clock content to bleed through (LV_OPA_90 was too transparent). Pomodoro time buttons were scattered in corners rather than grouped logically. Alarm weekday buttons lacked visual definition with no background 
- **[mydazy-p30-udour]** Screenshot function had a 1-frame rendering delay caused by timing mismatch between capture and frame buffer update. Fixed by accessing SDL texture directly through LVGL driver data to re-render before pixel readout. UI consistency issues stemmed from scattere
- **[mydazy-p30-udour]** The MyDazy-P30 application has a stable navigation structure with 2x2 grid layouts for main menus, consistent back arrow functionality, and proper time updates across all screens. All interactive elements (buttons, day-of-week selectors, timer controls) are fu
- **[mydazy-p30-udour]** The alarm page navigation arrows require Font Awesome icon font to properly display angle icons. The left arrow was already correctly configured, but the right arrow was using the wrong font family, causing visual inconsistency. The project successfully builds
- **[mydazy-p30-udour]** The existing system has full media playback (`MediaPlayer` class), AI agent configuration, and voice switching capabilities already implemented in code but completely hidden from users. Current menu has redundant elements (unnecessary "View Time" button, redun
- **[mydazy-p30-udour]** Three distinct UI profiles were designed to maximize hardware sensor integration: Tourism Guide mode prioritizes quick navigation between location-based audio content with Vol± long-press for location switching; Children's Learning mode implements parental con
- **[mydazy-p30-udour]** A single firmware with profile-based configuration provides better maintainability than multiple firmware packages. Constexpr configuration tables can drive the same UI components with different menu layouts, icons, and feature sets without code duplication. N
- **[mydazy-p30-udour]** The real differentiation between variants is in assets (SPIFFS content) rather than code. SPIFFS content includes different emoji packs (general vs cartoon vs scenic photos), audio resources (general alerts vs children's songs/stories vs tour commentary), and 
- **[mydazy-p30-udour]** The build system uses variant-specific JSON configs to customize firmware behavior. Two critical bugs blocked compilation: (1) release.py used case-sensitive string matching causing board name validation failures, and (2) sdkconfig.defaults disabled WiFi AP su
- **[mydazy-p30-udour]** The system implements a sophisticated profile-driven UI architecture where each mode shares core functionality but differs in branding, menu structure, and interaction features. Key differences: DESK and CHILD modes show photo wall in Time Room while TOUR show
- **[mydazy-p30-udour]** USB disconnection during ESP-IDF flashing at 58% can be caused by: poor USB cable contact, excessive baud rate (2Mbps may be too fast for stable connection), or insufficient power supply during high-current full flash operations. Lower baud rates (921600) and 
- **[mydazy-p30-udour]** LVGL 9.3.0 simulator successfully renders all UI pages with Profile-driven 4-grid layouts. GIF animations don't render from SPIFFS in simulator environment (expected limitation). Screenshot timing captures post-navigation states due to fast page transitions. F
- **[mydazy-p30-udour]** The screenshot issue is a timing artifact - automated tests use timers to switch pages, and screenshot delays captured the Time House screen after navigation rather than the target pages. Page creation logs (`[I][PomodoroPage]`, `[I][UiDisplay]`) confirm the p
- **[mydazy-p30-udour]** LVGL requires explicit file system driver registration to load assets from custom paths. The `S:` prefix convention maps to SPIFFS storage, requiring a driver in `main.cpp` that translates `S:/spiffs/xxx` paths to the local `../spiffs/xxx` file system. GIF ani
- **[mydazy-p30-udour]** Root cause identified: files in `main/assets/images/` and `main/assets/fonts/` had been replaced with placeholder blocks in a previous session. The original SquareLine Studio-generated assets were still in git history and could be restored via git checkout
- **[mydazy-p30-udour]** The product architecture centers on a 284×240 touchscreen ESP32-S3 device with dual WiFi/4G networking. The UI system uses a Profile-driven menu approach where each mode (office companion, child learning, tourism guide) has customized AI behavior, safety filte
- **[mydazy-p30-udour]** UI follows consistent 3+1 grid pattern (3 feature buttons + 1 home button) across all submenu pages. Content Hub displays different content based on device mode: desktop mode shows photo wall/music/stories, children mode shows songs/stories/English, travel mod
- **[mydazy-p30-udour]** Small screen devices (284x240) require distinct visual differentiation through color-coded icons and clear functional grouping. Font Awesome icons integrate well alongside SquareLine Studio icons. 3+1 layout pattern (3 functions + home button) provides effecti
- **[mydazy-p30-udour]** Removing 48px title bars and replacing with overlay back arrows recovers 20% of vertical screen space. Enlarging time preset buttons (50×36 to 54×38) improves touch accuracy. Swipe gestures provide more natural navigation than arrow buttons alone. Strategic bu
- **[mydazy-p30-udour]** The application consists of multiple feature modules with different UI patterns: Pomodoro uses a title bar with four corner time buttons, Small Alarm has title bar with delete and weekday selection controls, Three Things maintains its original title bar design
- **[mydazy-p30-udour]** The simulator translates mouse clicks to touch operations, allowing desktop-based testing of mobile/touch interactions. The testing flow involves clicking the clock icon to access the main menu, then navigating to each feature module (Pomodoro, Alarm, Three Th
- **[mydazy-p30-udour]** Application structure includes main clock interface with 2×2 grid menu (AI companion, Time House, Content, Settings), sub-menus with 3+1 layouts, and specialized screens for timers, alarms, todos, and media. Three pages had previous layout optimizations: pomod
- **[mydazy-p30-udour]** Menu pages use a standardized architecture: both time_room_page and content_hub_page share identical layout calculation logic using GRID_BTN_SIZE, GRID_SPACING_X/Y, GRID_LABEL_HEIGHT, and GRID_LABEL_GAP constants from ScreenConfig. Layout positions are calcula
- **[mydazy-p30-udour]** Both major submenu pages (Time Room and Content Hub) share identical 2x2 grid layout architecture with consistent spacing using ScreenConfig constants (GRID_BTN_SIZE, GRID_SPACING_X/Y, GRID_LABEL_HEIGHT, GRID_LABEL_GAP). All layout calculations dynamically cen
- **[mydazy-p30-udour]** The three menu pages had inconsistent layout parameters: submenu buttons were 60px vs main menu 85px, spacing varied (60px/8px vs 25px/25px), and border radius differed (16px vs 20px). All three pages use 2×2 grid layouts with icon buttons and labels.
- **[mydazy-p30-udour]** All pages currently have normal layouts. Three menu pages (main menu, Time House, Something Interesting) share unified icon sizes and spacing. The Pomodoro timer page (page 05) currently displays: title bar with back button, countdown timer, four corner time p
- **[mydazy-p30-udour]** Horizontal capsule selector pattern provides clearer visual hierarchy and better user feedback: selected items get orange borders and backgrounds, unselected items are grayed out, and the entire selector fades during active timing to reduce distraction
- **[mydazy-p30-udour]** Spatial distribution of controls around a central action button (start/stop) can prevent accidental touches - 8px spacing is effective. Vertical stacking with left/right grouping creates better touch targets than horizontal rows. Dimming inactive controls duri
- **[mydazy-p30-udour]** The current layout places time buttons at X coordinates of ±92 with 50px width, resulting in edges at 117 and 167 on a 284px wide screen. This creates approximately 25px margins on each side and maintains a 30px gap between the start button and time buttons to
- **[mydazy-p30-udour]** Current spacing configuration uses dy parameter and pixel-based vertical intervals between UI buttons. Tighter spacing increases risk of accidental touch interactions
- **[mydazy-p30-udour]** The application uses centralized UI configuration in `ui_config.h` with parameters like GRID_BTN_SIZE, GRID_SPACING_X/Y for consistent layouts. Visual centering requires manual offset adjustments (GRID_OFFSET_Y) rather than automatic calculation. Button state 
- **[mydazy-p30-udour]** The alarm screen uses LVGL (LV_LAYOUT_FLEX) for layout management; replacing text indicators with visual icons (bell icons) improves usability; flex containers enable automatic horizontal centering; page indicators enhance navigation for multi-item views; cent
- **[mydazy-p30-udour]** Small screen UI requires aggressive vertical space optimization. Logo size reduction (80x83 → 60x62) saves 28px. Info cards can move from Y=118 to Y=90 with compressed line spacing (120px → 112px) to show all content above fold. Flex layout enables automatic i
- **[mydazy-p30-udour]** Menu grid spacing parameters control bottom clearance: GRID_SPACING_Y and GRID_OFFSET_Y adjustments can move UI elements away from rounded corner areas. Three separate profile configurations (Desktop/Children/Cultural Tourism) share similar menu structures tha
- **[mydazy-p30-udour]** Stack canary watchpoint detected overflow in accelerometer task with deep call chain through esp_log_write, vprintf, uart_write, and mutex operations. Crash occurs after successful initialization of Touch sensor (chip 0x510601) and GPIO configurations. Stack o
- **[mydazy-p30-udour]** Stack overflow in SC7A20H accelerometer task caused by ESP_LOGI → vprintf → uart_write call chain exceeding 2048-byte stack allocation. The logging infrastructure on ESP32 requires more stack space than initially allocated. BLUFI initialization crash involves 
- **[mydazy-p30-udour]** The `app-flash` command only flashes the application partition and does not update the SPIFFS storage partition where GIF emoji files are stored. Full `flash` or `storage-flash` commands are needed to deploy resources. The loading.gif file is referenced in cod
- **[mydazy-p30-udour]** The UI system uses LVGL framework with SPIFFS filesystem mounted at `/storage` on physical hardware. GIF assets load correctly on actual ESP32 devices even though simulator shows different path behavior (simulator uses `/spiffs/` paths while LVGL expects `S:` 
- **[mydazy-p30-udour]** Both projects share identical screen resolution (284×240) and UI page system architecture with matching base components. Key differences: ui_display.cc/h location (board-level in P30-V2 vs generic in UDOUR), page count (8 vs 9 types), profile system presence, 
- **[mydazy-p30-udour]** V2 firmware has two display modes based on board type: LVGL full UI (UiDisplay) for mydazy-p30/p30-4g and expression mode (LxDisplay, work-in-progress) for mydazy-p30-wifi. UDOUR version adds CONTENT_HUB page, ui_profile system, ai_chat_config, and quick_chat_
- **[mydazy-p30-v1]** The comparison targets multiple subsystems: WebSocket improvements (circuit breaker, connection reuse, TTS tracking), audio enhancements (weak network handling, decoder mutex, DAC warmup, dynamic frame duration), and application core features (send backoff, fe
- **[mydazy-p30-v1]** The task requires coordinated comparison work across multiple aspects of the ESP32 project. A team structure was deemed appropriate for parallel analysis of different components or files.
- **[mydazy-p30-v1]** rtc_gpio_hold_dis must be called BEFORE gpio_config() to release hold state, otherwise GPIO configuration cannot take effect. When rtc_gpio_hold_en() is used to preserve GPIO state across esp_restart(), the hold must be explicitly released before reconfiguring
- **[mydazy-p30-v1]** The header file was recently modified to add an optional parameter to DetectBaudRate: `bool DetectBaudRate(int preferred_baud_rate = -1)`, but the implementation file still defines the function without any parameters: `bool AtUart::DetectBaudRate()`. This mism
- **[mydazy-p30-v1]** WifiCsi feature integrates at multiple levels: core implementation in main/wifi/, control interface in MCP server and remote commands, initialization hooks in board-specific code, build system entries in CMakeLists.txt, and ESP-IDF configuration flags in sdkco
- **[mydazy-p30-v1]** WifiCsi feature provided WiFi Channel State Information-based human proximity detection with three-tier zone classification (near/medium/far) using variance analysis. Feature integrated at multiple levels: MCP tool for AI agent control, RemoteCmd JSON API for 
- **[mydazy-p30-v2]** ESP-IDF 5.5.3 works with esptool 4.12.dev1, and the environment can be activated using the `idf55` command
- **[mydazy-p30-v2]** Latest xiaozhi-esp32 version migrated from object-oriented wrapper methods to direct ESP-IDF calls: WakeUP() moved to power_save_timer, SetPowerSaveMode() changed to SetPowerSaveLevel() with enum parameter, WifiStation singleton replaced with esp_wifi_stop(), 
- **[mydazy-p30-v2]** V2 project has 7 UI pages plus control center and clock system migrated from UDOUR; UDOUR has 9 pages with full control center; neither project currently has SDL simulator setup; both projects share similar LVGL-based page architecture that could benefit from 
- **[mydazy-p30-v2]** In-project simulator with independent CMake build system completely avoids ESP-IDF conflicts while enabling direct access to UI source code. Minimal ESP-IDF API mocking is required (logging, NVS, esp_timer, FreeRTOS mutexes) since SDL directly replaces display
- **[mydazy-p30-v2]** The LVGL simulator runs in a 284×240 SDL window on macOS. Navigation works through both mouse clicks (touch simulation) and keyboard shortcuts. The UI follows a hierarchical structure: clock home → menu page → individual feature pages. IMKCFRunLoopWakeUpReliab
- **[mydazy-p30-v2]** The UI architecture already properly integrates two complementary layers: lvgl_display/ provides status bar, notifications, battery icons, and screenshot capabilities through the base class hierarchy, while ui/ directory pages (pomodoro, alarms, etc.) are comp
- **[mydazy-p30-v2]** V2's resource loading system is fully aligned with migrated UI code from UDOUR. Fonts are compiled into firmware via managed_components/xiaozhi-fonts using file(GLOB) on src/*.c, making LV_FONT_DECLARE() references link correctly on ESP32. Images use UiImageMa
- **[mydazy-p30-v2]** Root cause of icon rendering failure: convert_c_to_lvgl_bin.py generates cf=0x0B which LVGL 9.x interprets as LV_COLOR_FORMAT_A1, but actual data is RGB565+A8 separated plane format. Detection pattern: cf=0x0B combined with stride/w==3 indicates RGB565A8 forma
- **[mydazy-p30-v2]** The UI configuration in `ui_config.h` controls critical layout parameters for grid-based navigation. Reducing header height (48px→36px) and vertical spacing (16px→8px) prevents label overflow, while increasing button size (64px→72px) with tighter horizontal sp
- **[mydazy-p30-v2]** 88px font size provides optimal digital display effect matching real device appearance. Scale=2 at 568x480 window dimensions maintains compact, accurate representation across all three pages.
- **[mydazy-p30-v2]** Application had multiple UX inconsistencies spanning navigation patterns (mixed back button styles), visual hierarchy (low contrast text), touch ergonomics (small interactive areas), and design language (mixed icon styles across time/photo/AI/content sections)
- **[mydazy-p30-v2]** The crash is a newly discovered bug not documented in project history. System uses DualNetworkBoard abstraction for runtime switching between WiFi and 4G (ML307) modes. When user triple-clicks boot button, code calls app.Alert("配网模式", "切换到WiFi", "logo", OGG_NE
- **[mydazy-p30-v2]** Project mydazy-p30-v2 has lcd_driver component referencing external driver from xiaozhi-esp32-189 project at components/esp_lcd_jd9853. Runtime logs reveal touch screen issues: firmware upgrade failures, I2C transaction timeouts at device address 0x1E, inabili
- **[mydazy-p30-v2]** Board type configuration is controlled via CONFIG_BOARD_TYPE_MYDAZY_P30/P32 flags in sdkconfig file. The same codebase supports multiple hardware variants through build-time configuration switches. The build process uses sed commands with fallback logic to ens
- **[mydazy-p30-v2]** Current system has 8 fixes that compiled successfully but are uncommitted. Audio input/output and opus_codec tasks use xTaskCreate which assigns cores randomly rather than pinning to specific cores (Core1 for I/O, Core0 for codec would be optimal). Version man
- **[mydazy-p30-v2]** The codebase has technical debt around RTOS timeout constants being used directly without abstraction and audio processing tasks lacking CPU affinity configuration
- **[mydazy-p30-v2]** The firmware had multiple categories of issues: improper error checking with ESP_ERROR_CHECK, queue configuration problems, MQTT reliability issues, missing volatile qualifiers, timer handling bugs, memory leaks, and non-compliant direct usage of portMAX_DELAY
- **[mydazy-p30-v2]** P30-V2 has superior code quality with atomic variables, CPU core binding, proper timeouts, and error handling, plus extensive LVGL-based UI system. V1(189) contains valuable standalone modules for media playback, acoustic calibration, Baidu WebSocket protocol,
- **[mydazy-p30-v2]** Multiple independent analysis methods confirmed the same architectural insights: P30-V2's superiority in UI/hardware/code-quality versus V1(189)'s specialized audio and protocol capabilities. The convergence of findings from different exploration agents valida
- **[mydazy-p30-v2]** The live_companion and remote_cmd modules depend on specific Application API methods (ForceListeningMode, SendTextToAI, GetLiveCompanion) that exist in a 189-specific implementation but are not present in the current Application base class. These API additions
- **[mydazy-p30-v2]** The mydazy-p30 is an embedded audio/RTC device with comprehensive capabilities including multi-format audio playback (MP3/WAV/OGG/FLAC/AAC), dual-channel acoustic calibration (REF+MIC), Baidu RTC integration for real-time communication, and OTA update function
- **[mydazy-p30-v2]** The codebase has critical threading issues in PowerManager where non-atomic variables are accessed from timer callbacks without synchronization (fixed in 4G variant). LVGL tree deletion during UiDisplay destruction can trigger watchdog timeouts when PSRAM cont
- **[mydazy-p30-v2]** The project demonstrates strong engineering fundamentals: zero portMAX_DELAY misuse, all volatile removed except inline assembly, 60+ proper atomic operations, timer reuse patterns preventing leaks, and queue bounds throughout. However, critical gaps exist in 
- **[mydazy-p30-v2]** The argparse configuration was using single-value mode for --extra_files, but the build system passes multiple space-separated paths (fonts and images directories); the downstream code already had logic to handle lists of paths using isinstance checks and iter
- **[mydazy-p30-v2]** Project already has complete NMEA parsing code that extracts latitude/longitude (degree-minute to decimal conversion), satellite count, HDOP, speed; ML307R has built-in GNSS controlled via AT commands (AT+MGNSS, AT+MGNSSLOC); Current test version uses 1-second
- **[mydazy-p30-v2]** ESP-IDF watchdog timeout is 10 seconds for main task; CreateEmotionUI() contains heavy operations (GIF widget creation, multiple LVGL containers); InitPageModules() instantiates 8 page modules (pomodoro, alarm, todo, photo wall, brain info, time room, content 
- **[mydazy-p30-v2]** Partition table was changed to v2/16m.csv which requires full flash (not app-flash). Restart behavior has three distinct patterns detectable from serial logs. UI initialization was blocking main task - fixed by moving CreateBootUI/ClockUI/EmotionUI/InitPageMod
- **[mydazy-p30-v2]** P30 base version is the proper V2 board configuration with complete UI system including UiDisplay class with SetupUI override, lcd_driver_factory standard initialization, ULP support in config.json, LV_USE_LODEPNG and LV_USE_GIF support, PRINT_HALT panic mode 
- **[mydazy-p30-v2]** The boards/README.md provides comprehensive guidance for ESP32 board customization, emphasizing incremental feature enablement: first ensure basic boot/operation, then add extended capabilities. Conditional compilation with CONFIG flags allows clean feature to
- **[mydazy-p30-v2]** Board initialization should be split into two phases: minimal core features needed for basic boot/display (GPIO, I2C, SPI, display, buttons, backlight) and extended features that can be enabled incrementally (storage, touch, power management, sensors). Non-ess
- **[mydazy-p30-v2]** The initialization flow has two distinct phases: Board construction handles hardware setup (GPIO, I2C, SPI, display, buttons, storage, touch, power), while Application::Initialize() handles UI setup. Moving UI creation from Board constructor to Application's S
- **[mydazy-p30-v2]** The I2S crash in the full firmware version was likely caused by API incompatibilities introduced when restoring features like DisplayFonts and other extended functionality. The audio write path is straightforward: audio data flows from the playback queue throu
- **[mydazy-p30-v2]** The codebase underwent major cleanup removing: entire esp-ml307 component (AT modem/network stack), printer board variant and thermal printer support, sensor drivers (sc7a20h, axp2101, afsk_demod), ULP/RTC wakeup code, and managed font components. Audio servic
- **[mydazy-p30-v2]** The P30 V2 project has documented memory constraints (60KB+ internal RAM required), tracked issues from P0-P2 priority levels, and established architecture with Application/AudioService/Protocol/Board components. Memory hotspots include OPUS encoder (26.6KB) a
- **[mydazy-p30-v2]** The project has multiple functional domains that need restoration: audio, display, network, board-level, protocol. A previous full-featured version (.cc.full) existed with capabilities like touch, power management, SC7A20H sensor, and remote control that were 
- **[mydazy-p30-v2]** The sc7a20h accelerometer component follows basic ESP-IDF component structure with proper driver dependencies. Component is written in C++ (sc7a20h.cc) and has standard ESP-IDF component registration with include directories and dependency declarations.
- **[mydazy-p30-v2]** The project follows ESP-IDF component naming conventions: esp_lcd_* prefix for display components, esp_lcd_touch_* prefix for touch drivers, and chip model naming for sensors (matching conventions like mpu6050)
- **[mydazy-p30-v2]** The SPIFFS configuration is already correct: MountStorage() in mydazy_p30_board.cc uses partition_label = "assets", and the partition table (partitions/v2/16m.csv) defines an 8MB "assets" SPIFFS partition at offset 0x800000. The mount failure occurs because th
- **[mydazy-p30-v2]** WS1850S NFC reader uses I2C address 0x28 with 5-layer driver architecture: I2C registers → command execution → ISO14443A/B protocols → NDEF extensions → application APIs. Component naming follows esp_&lt;function&gt;_&lt;chip&gt; pattern. ESP-IDF v5.3+ compone
- **[mydazy-p30-v2]** Custom boards require proper asset management setup including index.json file for emoticon/sticker display. The documentation specifies that emoji collections (twemoji_32, twemoji_64) must be configured in CMakeLists.txt with DEFAULT_EMOJI_COLLECTION, and prop
- **[mydazy-p30-v2]** Assets can be deployed using pre-built SPIFFS image (assets.bin) that gets flashed alongside firmware, eliminating the need for runtime filesystem formatting. This approach avoids the 89-second formatting delay on first boot and ensures assets are available im
- **[mydazy-p30-v2]** The mydazy project supports multiple hardware variants that share common assets (fonts, images, emojis) but have different configurations. The P30 series boards use the same font configuration (font_puhui_basic_20_4 + font_awesome_20_4) and require full asset 
- **[mydazy-p30-v2]** NVS (Non-Volatile Storage) stores network configuration preference (network.type: 0=WiFi, 1=4G/ML307). Erasing NVS forces device to revert to default_net_type=1 (4G mode). This provides a diagnostic path to isolate whether display problems are WiFi-specific or
- **[mydazy-p30-v2]** Date format buffer size calculation: `%04d年%02d月%02d日` requires 17 bytes plus null terminator (18 total), but compiler calculates up to 43 based on tm_year maximum value; 48 bytes is sufficient
- **[mydazy-p30-v2]** P30-4G board operates with simplified UI using SpiLcdDisplay (no LVGL pages), UiFontManager provides centralized font management replacing hardcoded font references, system has 11-state machine with Idle as a key state, and UI text should use UiFontManager::Ge
- **[mydazy-p30-v2]** 189 upstream provides runtime hot-switching between Bluetooth and WiFi AP configuration modes via SwitchConfigMode() without reboot, uses two-phase BluFi initialization to avoid RF conflicts (InitializeController() then Start()), implements SmartConnect with m
- **[mydazy-p30-v2]** P30-V2 has successfully migrated 189's low-level WiFi components (WifiStation, WifiAp, SsidManager, Blufi classes totaling ~3000 lines) but the WifiBoard abstraction layer that wraps these for board-level code is incomplete. Two critical methods are missing fr
- **[mydazy-p30-v2]** The P32 board variant uses WiFi mode instead of ML307 cellular modem, which avoids initializing ML307 UHCI DMA. Testing with this configuration can isolate whether crashes are caused by DMA channel conflicts between the cellular modem and audio subsystem.
- **[mydazy-p30-v2]** Display system uses 4-layer inheritance (Display → LvglDisplay → LcdDisplay → SpiLcdDisplay → UiDisplay) with JD9853 SPI LCD at 60MHz and 284x40 PSRAM double buffering; LVGL runs at P5 priority on Core1 with 30ms timer cycle; touch flow progresses from AXS5106
- **[mydazy-p30-v2]** esp_task_wdt_reset() requires explicit task registration via esp_task_wdt_add() before calling; LVGL timer contexts should use taskYIELD() instead of watchdog resets; CMake passing same --flag multiple times to Python argparse nargs='*' only retains final occu
- **[mydazy-p30-v2]** Memory index serves as navigation layer summarizing key content from each documentation file; entries require periodic updates as underlying documents evolve to maintain accurate searchability
- **[mydazy-p30-v2]** There is a pre-existing GPS build (zip file) from April 2nd that could potentially be used for testing, though it may not reflect the latest code changes. The build process uses release.py for compilation
- **[mydazy-p30-v2]** QR code display is a local-only feature not present in upstream. Local implementation includes: ShowWifiQrCode/HideWifiQrCode methods in lcd_display.cc, QR code overlay UI in brain_info_page.cc with clickable semi-transparent black overlay, LVGL qrcode widget 
- **[mydazy-p30-v2]** The mydazy-p30-v2 device has dual configuration modes (Bluetooth and Hotspot) with mode switching via double-click; LVGL QR code library can be enabled via sdkconfig; existing activation code display infrastructure uses digit sounds for audio feedback
- **[mydazy-p30-v2]** The device implements three distinct QR code workflows: BluFi (Bluetooth) configuration showing BLE pairing URL, AP (WiFi hotspot) configuration showing WiFi credentials, and device activation/binding showing activation codes. Each scenario has different visua
- **[mydazy-p30-v2]** QR code rendering time correlates with both pixel dimensions and data complexity. Larger QR versions (determined by encoded data length) increase module count and rendering duration. Reducing URL from 56 to 48 characters drops QR version from ~4 (33×33 modules
- **[mydazy-p30-v2]** Upstream made fundamental architectural changes that conflict with P30-4G production customizations: (1) wifi_board.cc completely rewritten with WifiManager class replacing custom SmartConnect/ConfigMode logic, (2) audio task priorities reduced (output: 10→4, 
- **[mydazy-p30-v2]** The min_free valley of 17.8KB (vs 39.6KB current free) is caused by concurrent factor overlay, not single large allocations. Three scenarios identified: (1) OTA download + WebSocket TLS simultaneous connections creating 42KB burst (16KB+16KB TLS buffers + 4KB 
