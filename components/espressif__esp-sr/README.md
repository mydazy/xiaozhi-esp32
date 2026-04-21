# ESP-SR（本仓库定制精简版 @ v2.3.1 for ESP32-S3）

> **本仓库定制说明（Jack · 2026-04）**
>
> 这份 `components/espressif__esp-sr/` 是 [espressif/esp-sr v2.3.1](https://github.com/espressif/esp-sr) 的 **本地化精简 fork**。
>
> **为什么从 `managed_components/` 搬进来本地化？**
> - 本项目（P30-4G / P31）使用的唤醒词是 **定制词「搭子精灵」(`wn9_da1zijing1ling2`)**，非 esp-sr 官方 registry 的模型
> - 若继续走 `managed_components/`，`idf.py update-dependencies` 会覆盖我们的模型 + Kconfig 修改
> - 本地 `components/` 里的改动随仓库 git 追踪，永久化、不丢失
>
> **精简内容（230MB → 39MB）：**
>
> | 删除 | 保留（ESP32-S3 专用） |
> |---|---|
> | `lib/{esp32, esp32c3, esp32c5, esp32c6, esp32p4*, esp32s2}` | `lib/esp32s3`（13 MB） |
> | `include/{esp32, esp32c3, ...}` | `include/esp32s3` |
> | `esp-tts/esp_tts_chinese/{非 esp32s3}` + `samples` + `*.dat` 数据 | `esp-tts/esp_tts_chinese/esp32s3/*.a` + `include/` |
> | `model/wakenet_model/{wn7_*, wn8_*}`（非 ESP32-S3 系列） | **`model/wakenet_model/wn9_*` + `wn9s_*` 全部 54 个 + 定制 `wn9_da1zijing1ling2`** |
> | `model/multinet_model/`（30 MB，本项目 `SR_MN_*=NONE`） | — |
> | `ci/` + `.codespellrc` + `.pre-commit-config.yaml` + `conftest.py` + `pytest.ini` + `CHANGELOG.md` + `CHECKSUMS.json` + `component.mk` | `Kconfig.projbuild` + `CMakeLists.txt` + `idf_component.yml` + `LICENSE` + `src/` + `tool/` |

---

## 🎯 本项目当前启用（sdkconfig）

```
CONFIG_SR_WN_WN9_DA1ZIJING1LING2=y    # 搭子精灵 —— 主唤醒词
CONFIG_SR_NSN_WEBRTC=y                # 降噪 = WebRTC（非模型）
CONFIG_SR_VADN_WEBRTC=y               # VAD = WebRTC（非模型）
CONFIG_SR_MN_CN_NONE=y                # 中文命令词识别 = 禁用
CONFIG_SR_MN_EN_NONE=y                # 英文命令词识别 = 禁用
```

运行时日志会显示：`AFE_CONFIG: Set WakeNet Model: wn9_da1zijing1ling2`。

---

## 📋 ESP32-S3 可用唤醒词清单（esp-sr v2.3.1，完整列表）

> ✅ **本仓库已包含下表中所有 wn9_* 和 wn9s_* 系列模型文件**（共 54 个，约 16 MB）。
> 切换唤醒词只需改 `sdkconfig` 中的 `CONFIG_SR_WN_*=y` 即可（或 `idf.py menuconfig`），无需额外补文件。
>
> 例外：`wn9l_*`（Large 大模型）系列 Kconfig 选项仍可见，但**模型目录未包含**（上游 esp-sr v2.3.1
> 没在仓库附带），需要时从 [github.com/espressif/esp-sr](https://github.com/espressif/esp-sr) 单独下载。

### WakeNet9-Slim (WN9S) — 低内存/算力场景

| Kconfig 选项 | 唤醒词 | 目录名 |
|---|---|---|
| `SR_WN_WN9S_HILEXIN` | Hi,乐鑫 | `wn9s_hilexin` |
| `SR_WN_WN9S_HIESP` | Hi,ESP | `wn9s_hiesp` |
| `SR_WN_WN9S_NIHAOXIAOZHI` | 你好小智 | `wn9s_nihaoxiaozhi` |
| `SR_WN_WN9S_HIJASON` | Hi,Jason | `wn9s_hijason` |

### WakeNet9 / WakeNet9L — 标准模型（ESP32-S3 主力）

#### 中文

| Kconfig 选项 | 唤醒词 | 目录名 | 备注 |
|---|---|---|---|
| `SR_WN_WN9_HILEXIN` | Hi,乐鑫 | `wn9_hilexin` | |
| `SR_WN_WN9_HIESP` | Hi,ESP | `wn9_hiesp` | |
| `SR_WN_WN9_XIAOAITONGXUE` | 小爱同学 | `wn9_xiaoaitongxue` | |
| `SR_WN_WN9L_XIAOAITONGXUE` | 小爱同学 | `wn9l_xiaoaitongxue` | L = Large，识别率更高、内存更多 |
| `SR_WN_WN9_NIHAOXIAOZHI_TTS` | 你好小智 | `wn9_nihaoxiaozhi_tts` | xiaozhi-esp32 默认 |
| `SR_WN_WN9L_NIHAOXIAOZHI_TTS3` | 你好小智 | `wn9l_nihaoxiaozhi_tts3` | L 版 |
| **`SR_WN_WN9_DA1ZIJING1LING2`** | **搭子精灵** | **`wn9_da1zijing1ling2`** | **✅ 本项目启用** |
| `SR_WN_WN9_NIHAOMIAOBAN_TTS2` | 你好喵伴 | `wn9_nihaomiaoban_tts2` | |
| `SR_WN_WN9_XIAOLONGXIAOLONG_TTS` | 小龙小龙 | `wn9_xiaolongxiaolong_tts` | |
| `SR_WN_WN9_HIMIAOMIAO_TTS` | Hi,喵喵 | `wn9_himiaomiao_tts` | |
| `SR_WN_WN9_MIAOMIAOTONGXUE_TTS` | 喵喵同学 | `wn9_miaomiaotongxue_tts` | |
| `SR_WN_WN9_NIHAOXIAOXIN_TTS` | 你好小鑫 | `wn9_nihaoxiaoxin_tts` | |
| `SR_WN_WN9_XIAOMEITONGXUE_TTS` | 小美同学 | `wn9_xiaomeitongxue_tts` | |
| `SR_WN_WN9_HILILI_TTS` | Hi,Lily / Hi,莉莉 | `wn9_hilili_tts` | |
| `SR_WN_WN9_HITELLY_TTS` | Hi,Telly / Hi,泰力 | `wn9_hitelly_tts` | |
| `SR_WN_WN9_XIAOBINXIAOBIN_TTS` | 小滨小滨 / 小冰小冰 | `wn9_xiaobinxiaobin_tts` | |
| `SR_WN_WN9_HAIXIAOWU_TTS` | Hi,小巫 | `wn9_haixiaowu_tts` | |
| `SR_WN_WN9_XIAOYAXIAOYA_TTS2` | 小鸭小鸭 | `wn9_xiaoyaxiaoya_tts2` | |
| `SR_WN_WN9_LINAIBAN_TTS2` | 璃奈板 | `wn9_linaiban_tts2` | |
| `SR_WN_WN9_XIAOSUROU_TTS2` | 小酥肉 | `wn9_xiaosurou_tts2` | |
| `SR_WN_WN9_XIAOYUTONGXUE_TTS2` | 小宇同学 | `wn9_xiaoyutongxue_tts2` | |
| `SR_WN_WN9_XIAOMINGTONGXUE_TTS2` | 小明同学 | `wn9_xiaomingtongxue_tts2` | |
| `SR_WN_WN9_XIAOKANGTONGXUE_TTS2` | 小康同学 | `wn9_xiaokangtongxue_tts2` | |
| `SR_WN_WN9_XIAOJIANXIAOJIAN_TTS2` | 小箭小箭 | `wn9_xiaojianxiaojian_tts2` | |
| `SR_WN_WN9_XIAOTEXIAOTE_TTS2` | 小特小特 | `wn9_xiaotexiaote_tts2` | |
| `SR_WN_WN9_NIHAOXIAOYI_TTS2` | 你好小益 | `wn9_nihaoxiaoyi_tts2` | |
| `SR_WN_WN9_NIHAOBAIYING_TTS2` | 你好百应 | `wn9_nihaobaiying_tts2` | |
| `SR_WN_WN9_NIHAODONGDONG_TTS2` | 你好东东 | `wn9_nihaodongdong_tts2` | |
| `SR_WN_WN9_HIWALLE_TTS2` | Hi 瓦力 / Hi Wall-E | `wn9_hiwalle_tts2` | |
| `SR_WN_WN9_XIAOLUXIAOLU_TTS2` | 小鹿小鹿 | `wn9_xiaoluxiaolu_tts2` | |
| `SR_WN_WN9_NIHAOXIAOAN_TTS2` | 你好小安 | `wn9_nihaoxiaoan_tts2` | |
| `SR_WN_WN9_NI3HAO3XIAO3MAI4_TTS2` | 你好小脉 | `wn9_ni3hao3xiao3mai4_tts2` | |
| `SR_WN_WN9_NI3HAO3XIAO3RUI4_TTS3` | 你好小瑞 | `wn9_ni3hao3xiao3rui4_tts3` | |
| `SR_WN_WN9_HAI1XIAO3OU1_TTS3` | 嗨小欧 | `wn9_hai1xiao3ou1_tts3` | |
| `SR_WN_WN9_XIAO3JIA1XIAO3JIA1_TTS3` | 小珈小珈 | `wn9_xiao3jia1xiao3jia1_tts3` | |
| `SR_WN_WN9_XIAO3FENG1XIAO3FENG1_TTS3` | 小峰小峰 | `wn9_xiao3feng1xiao3feng1_tts3` | |
| `SR_WN_WN9_HAI1XIAO3XIANG4_TTS3` | 嗨小象 | `wn9_hai1xiao3xiang4_tts3` | |

#### 英文

| Kconfig 选项 | 唤醒词 | 目录名 |
|---|---|---|
| `SR_WN_WN9_ALEXA` | Alexa | `wn9_alexa` |
| `SR_WN_WN9_JARVIS_TTS` | Jarvis | `wn9_jarvis_tts` |
| `SR_WN_WN9_COMPUTER_TTS` | computer | `wn9_computer_tts` |
| `SR_WN_WN9_HEYWILLOW_TTS` | Hey,Willow | `wn9_heywillow_tts` |
| `SR_WN_WN9_HIMFIVE` | Hi,M Five | `wn9_himfive` |
| `SR_WN_WN9_SOPHIA_TTS` | Sophia | `wn9_sophia_tts` |
| `SR_WN_WN9_HEYWANDA_TTS` | Hey,Wanda | `wn9_heywanda_tts` |
| `SR_WN_WN9_HIJOLLY_TTS2` | Hi,Jolly | `wn9_hijolly_tts2` |
| `SR_WN_WN9_HIFAIRY_TTS2` | Hi,Fairy | `wn9_hifairy_tts2` |
| `SR_WN_WN9_HEYPRINTER_TTS` | Hey,Printer | `wn9_heyprinter_tts` |
| `SR_WN_WN9_MYCROFT_TTS` | Mycroft | `wn9_mycroft_tts` |
| `SR_WN_WN9_HIJOY_TTS` | Hi,Joy | `wn9_hijoy_tts` |
| `SR_WN_WN9_HIJASON_TTS2` | Hi,Jason | `wn9_hijason_tts2` |
| `SR_WN_WN9_ASTROLABE_TTS` | Astrolabe | `wn9_astrolabe_tts` |
| `SR_WN_WN9_HEYILY_TTS2` | Hey,Ily | `wn9_heyily_tts2` |
| `SR_WN_WN9_BLUECHIP_TTS2` | Blue Chip | `wn9_bluechip_tts2` |
| `SR_WN_WN9_HIANDY_TTS2` | Hi,Andy | `wn9_hiandy_tts2` |
| `SR_WN_WN9_HEYIVY_TTS2` | Hey,Ivy | `wn9_heyivy_tts2` |
| `SR_WN_WN9_HISTACKCHAN_TTS3` | Hi,Stack Chan | `wn9_histackchan_tts3` |
| `SR_WN_WN9_HEYKIRA_TTS3` | Hey,Kira | `wn9_heykira_tts3` |

---

## 🔧 切换唤醒词步骤

以从「搭子精灵」切到「你好小智」为例：

### 1. 补齐模型文件（精简时被删掉的）

```bash
cp -r ~/GitHub/xiaozhi-esp32-189/components/espressif__esp-sr/model/wakenet_model/wn9_nihaoxiaozhi_tts \
      components/espressif__esp-sr/model/wakenet_model/
```

### 2. 改 `sdkconfig`

```diff
-CONFIG_SR_WN_WN9_DA1ZIJING1LING2=y
+CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS=y
```

（或 `idf.py menuconfig` → `ESP Speech Recognition` → `Select wake words`）

### 3. 重编译 + 烧录

```bash
idf.py build flash monitor
```

### 4. 验证（monitor 日志）

```
AFE_CONFIG: Set WakeNet Model: wn9_nihaoxiaozhi_tts
```

---

## 🧩 如何新增一个自定义唤醒词（参照「搭子精灵」做法）

1. 把模型目录放到 `model/wakenet_model/<your_name>/`（含 `_MODEL_INFO_`、`wn9_data`、`wn9_index` 三个文件）
2. 在 `Kconfig.projbuild` 里加选项（参照 `SR_WN_WN9_DA1ZIJING1LING2` 的位置）：
   ```
       config SR_WN_WN9_YOUR_NAME
       bool "你的唤醒词中文名 (wn9_your_name)"
       default False
       help
           Custom trained wake word model
   ```
3. `sdkconfig` 启用：`CONFIG_SR_WN_WN9_YOUR_NAME=y`，关掉其他唤醒词
4. `idf.py build` —— `movemodel.py` 会自动识别 `CONFIG_SR_WN_*=y` 并把对应目录打包进 `srmodels.bin`

---

## 📦 本仓库 esp-sr 关键文件

```
components/espressif__esp-sr/
├── Kconfig.projbuild          # 唤醒词/VAD/MN/NSN 菜单，含定制 DA1ZIJING1LING2
├── CMakeLists.txt             # 链接 lib/esp32s3 下的 .a 库 + srmodels.bin 生成规则
├── idf_component.yml          # 声明对 esp-dsp / dl_fft 的依赖（IDF manager 从 registry 拉子依赖）
├── lib/esp32s3/               # 预编译静态库（AFE / WakeNet / VADNet / NSNet）
├── include/esp32s3/           # 公共头文件
├── src/                       # esp-sr 运行时源码（C 级别配置处理）
├── model/
│   ├── CMakeLists.txt
│   ├── pack_model.py          # 打包 srmodels.bin 工具
│   ├── movemodel.py           # 读 sdkconfig 选模型 + 调 pack_model.py
│   ├── model_path.c           # 运行时模型路径查找
│   └── wakenet_model/
│       └── wn9_da1zijing1ling2/   # 当前唯一保留的模型
├── esp-tts/
│   └── esp_tts_chinese/
│       ├── esp32s3/           # libesp_tts_chinese.a、libvoice_set_xiaole.a
│       └── *.dat              # TTS 语音素材
└── README.md                  # 你正在读的这份
```

---

## 🔗 上游参考

- GitHub: https://github.com/espressif/esp-sr
- 官方文档: https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/index.html
- 本仓库 fork 时间: 2026-04-21
- 基于: esp-sr `v2.3.1` (commit `98f7f642e12b2a3131e93455293a7c02e7e6433a`)
