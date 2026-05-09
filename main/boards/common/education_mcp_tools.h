#ifndef EDUCATION_MCP_TOOLS_H
#define EDUCATION_MCP_TOOLS_H

class UiDisplay;
class McpServer;

// 注册教育卡 MCP 工具集到给定 server：
//   - self.education.show_stroke  汉字笔画 GIF（≤512KB 云端下载，PSRAM 缓冲）
//   - self.education.set_mode     LLM 教学 prompt 切换（触发器，无流量）
//   - self.education.show_card    本地 LVGL 渲染卡片（无网络）
void RegisterEducationMcpTools(McpServer& mcp, UiDisplay* ui, bool include_stroke = true);

#endif  // EDUCATION_MCP_TOOLS_H
