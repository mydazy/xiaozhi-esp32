#include "edu_scene_pool.h"

#include <cstring>
#include <esp_log.h>
#include <esp_random.h>
#include <nvs.h>
#include <nvs_flash.h>

#define TAG "EduScenePool"

// 默认 10 个高频刚需启蒙场景（3-9 岁儿童视角 · 玩中学）
// 风格：弱化"学习"，强化"游戏/陪伴/趣味"
// 远程可全量替换（一条字符串即可，'|' 分隔）
static constexpr const char* kDefaultNames =
    "听故事|十万为啥|猜一猜|接龙啦|唱歌啦|念古诗|说英语|比比快|转转脑|对暗号";

static constexpr const char* kNvsNamespace = "edu";
static constexpr const char* kNvsKeyBuf    = "names";   // 字符串
static constexpr const char* kNvsKeyCounts = "counts";  // blob

EduScenePool& EduScenePool::GetInstance() {
    static EduScenePool instance;
    return instance;
}

void EduScenePool::LoadDefault() {
    strlcpy(buf_, kDefaultNames, sizeof(buf_));
    if (!RebuildIndex()) {
        ESP_LOGE(TAG, "Default pool malformed (BUG)");
    }
    memset(call_counts_, 0, sizeof(call_counts_));
    ESP_LOGI(TAG, "Loaded default pool");
}

void EduScenePool::Load() {
    if (loaded_) return;
    if (!LoadFromNvs()) {
        LoadDefault();
    }
    loaded_ = true;
}

// 解析 buf_ 内 '|' 分隔串：替换为 '\0' + 填 names_[]
// 必须正好 kPoolSize 段；任意一段超过 kMaxNameBytes 视为非法
bool EduScenePool::RebuildIndex() {
    int idx = 0;
    char* p = buf_;
    names_[0] = p;
    int seg_len = 0;
    while (*p) {
        if (*p == '|') {
            if (seg_len == 0 || seg_len > kMaxNameBytes) {
                ESP_LOGW(TAG, "RebuildIndex: seg[%d] len=%d invalid", idx, seg_len);
                return false;
            }
            *p = '\0';
            idx++;
            if (idx >= kPoolSize) {
                ESP_LOGW(TAG, "RebuildIndex: too many segments");
                return false;
            }
            names_[idx] = p + 1;
            seg_len = 0;
        } else {
            seg_len++;
        }
        p++;
    }
    if (idx != kPoolSize - 1 || seg_len == 0 || seg_len > kMaxNameBytes) {
        ESP_LOGW(TAG, "RebuildIndex: expect %d segs, got %d (last len=%d)",
                 kPoolSize, idx + 1, seg_len);
        return false;
    }
    return true;
}

bool EduScenePool::LoadFromNvs() {
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) return false;

    size_t need = sizeof(buf_);
    esp_err_t err = nvs_get_str(h, kNvsKeyBuf, buf_, &need);
    if (err != ESP_OK) {
        nvs_close(h);
        return false;
    }
    // counts 缺失也允许（默认全 0）
    size_t cn = sizeof(call_counts_);
    nvs_get_blob(h, kNvsKeyCounts, call_counts_, &cn);
    nvs_close(h);

    if (!RebuildIndex()) {
        ESP_LOGW(TAG, "NVS buf malformed, fall back default");
        return false;
    }
    ESP_LOGI(TAG, "Loaded from NVS: \"%s\"", buf_);
    return true;
}

void EduScenePool::SaveBufToNvs() {
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return;
    // 重建原始 '|' 分隔字符串（RebuildIndex 把 '|' 改成 '\0' 了）
    char tmp[kBufSize];
    int p = 0;
    for (int i = 0; i < kPoolSize && names_[i]; ++i) {
        if (i > 0 && p < (int)sizeof(tmp) - 1) tmp[p++] = '|';
        int n = strlcpy(tmp + p, names_[i], sizeof(tmp) - p);
        p += n;
        if (p >= (int)sizeof(tmp)) break;
    }
    nvs_set_str(h, kNvsKeyBuf, tmp);
    nvs_commit(h);
    nvs_close(h);
}

void EduScenePool::SaveCountsToNvs() {
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, kNvsKeyCounts, call_counts_, sizeof(call_counts_));
    nvs_commit(h);
    nvs_close(h);
}

bool EduScenePool::UpdateFromString(const char* s) {
    if (!s) return false;
    // 临时拷贝到 buf_ 解析校验，校验失败回滚不影响当前
    char backup[kBufSize];
    memcpy(backup, buf_, sizeof(backup));

    if (strlcpy(buf_, s, sizeof(buf_)) >= sizeof(buf_)) {
        ESP_LOGW(TAG, "UpdateFromString: input too long");
        memcpy(buf_, backup, sizeof(buf_));
        RebuildIndex();
        return false;
    }
    if (!RebuildIndex()) {
        memcpy(buf_, backup, sizeof(buf_));
        RebuildIndex();
        return false;
    }
    SaveBufToNvs();
    last_idx_ = -1;
    // 不重置 call_counts_（保留历史去重信息）
    ESP_LOGI(TAG, "UpdateFromString: applied + NVS saved");
    return true;
}

EduPick EduScenePool::GetRandomWithCount() {
    if (!loaded_) Load();
    int idx;
    do {
        idx = static_cast<int>(esp_random() % kPoolSize);
    } while (idx == last_idx_ && kPoolSize > 1);
    last_idx_ = idx;

    if (call_counts_[idx] < UINT16_MAX) call_counts_[idx]++;
    SaveCountsToNvs();

    return EduPick{names_[idx], call_counts_[idx]};
}
