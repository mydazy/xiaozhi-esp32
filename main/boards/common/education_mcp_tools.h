#ifndef EDUCATION_MCP_TOOLS_H
#define EDUCATION_MCP_TOOLS_H

class UiDisplay;
class McpServer;

// 注册"识字笔画动画"MCP 工具（self.education.show_stroke）到给定 server。
//   ui: 用于动态注入 PSRAM GIF + SetEmotion("font") 触发显示。nullptr 则不注册。
//   依赖 Ota::Download 做 PSRAM 缓冲下载，UiDisplay::UpdateFontGif 做 emoji 槽位替换。
//   下载上限 512KB，含 GIF 头尾完整性校验；4G/WiFi 共用同一路径（4G 也直接下载）。
void RegisterEducationMcpTools(McpServer& mcp, UiDisplay* ui);

#endif  // EDUCATION_MCP_TOOLS_H
