# 小智 MCP 接入点 — 本地 PC Server

让 PC 上的 Python 函数可以被小智 AI 直接调用。

## 工作模式

```
PC (server.py) <--wss--> 小智后台 (api.xiaozhi.me/mcp) <----> 小智 AI
       └─ @tool 注册的函数              └─ JSON-RPC 转发
```

PC 主动连接小智后台 MCP endpoint，但角色是 **server**：被动响应 `initialize` / `tools/list` / `tools/call`，跟设备端 `main/mcp_server.cc` 同一套协议（参考 `docs/mcp-protocol_zh.md`）。

## 一次性配置

```bash
cd scripts/xiaozhi_mcp
cp .env.example .env
# 编辑 .env，把 XIAOZHI_MCP_TOKEN 填进去（从小智后台 MCP 接入点页面复制）
pip install -r requirements.txt
```

## 运行

```bash
python server.py             # 后台跑，断线自动重连
python server.py --debug     # 打印每条 JSON-RPC 收发，调试用
```

启动成功后，在小智 AI 对话里说 "调用 echo，参数 hello" 就能验证链路。

## 加新工具

打开 `server.py`，在"工具注册区"加一个 `@registry.register(...)` 装饰的函数即可：

```python
@registry.register(
    name="pc.read_file",
    description="读取本机文件首 200 字节",
    input_schema={
        "type": "object",
        "properties": {"path": {"type": "string"}},
        "required": ["path"],
    },
)
def tool_read_file(args):
    with open(args["path"], "rb") as f:
        return f.read(200).decode("utf-8", errors="replace")
```

返回值 `str` 直接当 text 内容；`dict`/`list` 自动 `json.dumps`。

## 命名约定

- 工具名沿用本仓库 `self.xxx.yyy` 风格，PC 端建议用 `pc.xxx.yyy` 区分设备端
- input_schema 用 JSON Schema，支持 `string`/`integer`/`boolean`，与设备端 `Property` 类型对齐

## 安全

- `.env` **不要提交** git（仓库根 `.gitignore` 应已排除；如未排除请加上 `scripts/xiaozhi_mcp/.env`）
- token 一旦泄漏，去小智后台重新生成会自动失效旧 token
- 工具回调里不要执行未经校验的 shell；要执行系统命令请用白名单
