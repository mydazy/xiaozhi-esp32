#ifndef EDUCATION_MCP_TOOLS_H
#define EDUCATION_MCP_TOOLS_H

class UiDisplay;
class McpServer;

// 注册教育卡 MCP 工具集到给定 server（本地 MCP · 设备端直接注册到 LLM tools/list）：
//   - self.education.show_stroke  汉字笔画 GIF（≤512KB 云端动态 GIF · 每次重新下载不缓存）
//   - self.education.show_card    本地 LVGL 渲染教育卡（无网络 · 主路径走 [main_top] 内联，工具为兜底）
//
// show_stroke 完整性保证（4G/WiFi 都安全）：
//   ① Ota::Download 同步阻塞返回时 buffer 完整或失败
//   ② 1KB min size 拒绝弱网截断 / 服务器 4xx 错误页
//   ③ GIF89a/87a magic 校验
//   ④ 末字节 0x3B = Trailer 验证完整性（未中途截断）
//   ⑤ Schedule 主线程二次 state 守护（state ≠ Speaking 则丢 buffer）
//   ⑥ 失败兜底：任一校验不过 → 自动 ShowEduCard(character, "") 弹大字卡
//   弱网仅"退化到大字卡"，绝不显示脏帧 / 崩溃 → 4G 模式默认开启 show_stroke
//
// include_stroke=false 时仅注册 show_card（无 PSRAM GIF 路径的板可关闭，量产中暂未使用）
void RegisterEducationMcpTools(McpServer& mcp, UiDisplay* ui, bool include_stroke = true);

#endif  // EDUCATION_MCP_TOOLS_H
