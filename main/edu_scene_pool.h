#ifndef EDU_SCENE_POOL_H
#define EDU_SCENE_POOL_H

#include <cstdint>

// 启蒙场景池（摇一摇本地池）— 3-9 岁儿童视角的对话触发主题
//
// 统一字符串协议（远程下发 / NVS 存储 / 随机调用 同一格式）：
//   "听故事|十万为啥|猜一猜|接龙啦|唱歌啦|念古诗|说英语|比比快|转转脑|对暗号"
//
// 设计理念：
//   - 远程下发：一条 string 即可（无需 JSON 数组）
//   - NVS 存储：直接 nvs_set_str（不用 blob）
//   - 随机调用：内存中只是 buf_ 字符串 + offset/len 索引
//   - call_counts_ 持久化每场景累计调用次数 → 拼 [name N] 让云端按编号轮换内容
//
// 摇一摇行为：本地随机选一个 → SendTextToAI("[name count]") → 云端展开
struct EduPick {
    const char* name;   // 指向内部 buf_，零拷贝
    int count;          // 该场景累计调用次数（从 1 开始）
};

class EduScenePool {
public:
    static EduScenePool& GetInstance();

    // 启动时加载（先尝试 NVS，失败回退默认池）
    void Load();

    // 远程更新：'|' 分隔的 10 个场景名 → 写 NVS（保留 call_counts_ 不重置）
    bool UpdateFromString(const char* names_pipe_separated);

    // 摇一摇随机：返回场景名 + 累计调用次数（自动 +1 持久化）
    EduPick GetRandomWithCount();

    static constexpr int kPoolSize = 10;
    static constexpr int kBufSize  = 256;   // 单字符串缓冲（10 × ~20 字符够用）
    static constexpr int kMaxNameBytes = 24; // 单名最多 24 字节（≈ 8 汉字）

private:
    EduScenePool() = default;

    void LoadDefault();
    bool RebuildIndex();          // 把 buf_ 切片到 names_[]
    bool LoadFromNvs();
    void SaveBufToNvs();
    void SaveCountsToNvs();

    char        buf_[kBufSize] = {};         // 单一字符串（'|' 分隔）
    const char* names_[kPoolSize] = {};      // 指向 buf_ 内每段（'|' 替换 '\0' 后的 cstr）
    uint16_t    call_counts_[kPoolSize] = {};
    int         last_idx_ = -1;
    bool        loaded_ = false;
};

#endif  // EDU_SCENE_POOL_H
