#!/usr/bin/env python3
"""
小智 MCP 接入点 — 本地 PC 端 MCP Server。

把这台 PC 注册成一个 MCP server，连接到小智后台 (wss://api.xiaozhi.me/mcp/?token=...)
后，小智 AI 可以发现并调用本文件里用 @tool 注册的本地函数。

用法:
    cp .env.example .env
    # 编辑 .env 填入 XIAOZHI_MCP_TOKEN
    pip install -r requirements.txt
    python server.py

协议参考: docs/mcp-protocol_zh.md
"""
import argparse
import asyncio
import json
import logging
import os
import re
import sys
import time
from pathlib import Path
from typing import Any, Callable, Dict

try:
    import websockets
    from websockets.exceptions import ConnectionClosed
except ImportError:
    sys.stderr.write("missing dependency: pip install -r requirements.txt\n")
    sys.exit(1)

# 教育卡片标记格式 — 对齐 main/application.cc::ExtractEduWordCards (v6.3):
#   '|' 优先：[main|bottom] / [top|main|bottom] (三段)
#   '-' 兜底：[main-bottom] (LLM 实测倾向 + demo SQL 旧格式 · rfind 最右 '-')
# 单句多 [] 设备端按 3.5s 间隔依次弹卡，上限 8 张
EDU_CARD_RE = re.compile(r"\[([^\]]+)\]")
EDU_MAIN_MAX_BYTES = 32     # main 字段上限
EDU_BOTTOM_MAX_BYTES = 48   # bottom 字段上限
EDU_TOP_MAX_BYTES = 24      # top 字段上限（仅三段格式）


def split_edu_card(body: str):
    """复刻设备端切分: '|' 优先 (1/2 个 → 二/三段)，否则 rfind '-' 兜底。返回 (top, main, bottom) 或 None。"""
    if "|" in body:
        parts = [p.strip() for p in body.split("|")]
        if len(parts) == 2:
            top, main, bottom = "", parts[0], parts[1]
        elif len(parts) == 3:
            top, main, bottom = parts
        else:
            return None
    elif "-" in body:
        main_raw, bottom = body.rsplit("-", 1)
        top, main, bottom = "", main_raw.strip(), bottom.strip()
    else:
        return None
    if not main or not bottom:
        return None
    return top, main, bottom


def validate_edu_card_lines(label: str, lines: list) -> list:
    """启动期自检 templates 里的教育卡标记是否符合设备端解析规则。"""
    errors = []
    for i, line in enumerate(lines):
        if not isinstance(line, str):
            errors.append(f"{label}#{i}: 非字符串行")
            continue
        cards = EDU_CARD_RE.findall(line)
        if len(cards) > 1:
            errors.append(f"{label}#{i}: 单行多张卡（设备端只解析首个）: {line}")
            continue
        if not cards:
            continue
        body = cards[0]
        parts = split_edu_card(body)
        if parts is None:
            errors.append(f"{label}#{i}: 必须是 [main|bottom] 或 [top|main|bottom]: [{body}]")
            continue
        top, main, bottom = parts
        if len(main.encode("utf-8")) > EDU_MAIN_MAX_BYTES:
            errors.append(f"{label}#{i}: main 超 {EDU_MAIN_MAX_BYTES} 字节: {main}")
            continue
        if len(bottom.encode("utf-8")) > EDU_BOTTOM_MAX_BYTES:
            errors.append(f"{label}#{i}: bottom 超 {EDU_BOTTOM_MAX_BYTES} 字节: {bottom}")
            continue
        if top and len(top.encode("utf-8")) > EDU_TOP_MAX_BYTES:
            errors.append(f"{label}#{i}: top 超 {EDU_TOP_MAX_BYTES} 字节: {top}")
    return errors


DEFAULT_ENDPOINT = "wss://api.xiaozhi.me/mcp/"
# Token 必须通过环境变量 XIAOZHI_MCP_TOKEN 或命令行 --token 提供
# (敏感凭据不入库；之前硬编码的 token 已在 git 历史移除前替换为 placeholder)
EMBEDDED_TOKEN = ""
SERVER_NAME = "mydazy-p30-pc-mcp"
SERVER_VERSION = "0.1.0"
PROTOCOL_VERSION = "2024-11-05"

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger("xiaozhi-mcp")


class ToolRegistry:
    def __init__(self) -> None:
        self._tools: Dict[str, Dict[str, Any]] = {}

    def register(self, name: str, description: str, input_schema: Dict[str, Any]):
        def decorator(fn: Callable[[Dict[str, Any]], Any]):
            self._tools[name] = {
                "description": description,
                "inputSchema": input_schema,
                "fn": fn,
            }
            return fn
        return decorator

    def list_tools(self):
        return [
            {"name": name, "description": meta["description"], "inputSchema": meta["inputSchema"]}
            for name, meta in self._tools.items()
        ]

    def call(self, name: str, arguments: Dict[str, Any]) -> Any:
        if name not in self._tools:
            raise KeyError(name)
        return self._tools[name]["fn"](arguments or {})


registry = ToolRegistry()


# ---- 工具注册区: 新增工具在此添加 -----------------------------------------

@registry.register(
    name="pc.echo",
    description="回显输入文本，用于验证 MCP 链路是否打通。",
    input_schema={
        "type": "object",
        "properties": {
            "text": {"type": "string", "description": "要回显的文本"}
        },
        "required": ["text"],
    },
)
def tool_echo(args: Dict[str, Any]) -> str:
    return f"echo: {args.get('text', '')}"


# 英文字母启蒙 — 对齐 docs/edu-mcp-tools.sql 占位工具
# 双轨：[英文|中文] 触发 word 卡 + 字母谐音穿插口播
# 五段式：引入 → 首次教学 → 场景应用 → 快闪复习 → 主动回忆，每字母弹 3 次卡
# 26 字母按 4 组分批：A-G / H-N / O-T / U-Z
ENGLISH_LETTERS_INTERVAL_MS = 3500

ENGLISH_LETTERS_TEMPLATES: Dict[str, list] = {
    "ag": [
        "今天搭子带你认识 7 个英语字母兄弟，每个都带一个小伙伴一起来！",
        "A 念作'诶'，A 的好朋友是 [a-pp-le|apple|苹果]",
        "B 念作'比'，B 的好朋友是 [ba-na-na|banana|香蕉]",
        "C 念作'西'，C 的好朋友是 [c-a-t|cat|小猫]",
        "D 念作'迪'，D 的好朋友是 [d-o-g|dog|小狗]",
        "E 念作'伊'，E 的好朋友是 [el-e-phant|elephant|大象]",
        "F 念作'艾弗'，F 的好朋友是 [f-i-sh|fish|小鱼]",
        "G 念作'基'，G 的好朋友是 [g-oa-t|goat|山羊]",
        "小明咬了一口 [apple|苹果]，A 是诶！",
        "小红剥开一根 [banana|香蕉]，B 是比！",
        "花园里有只 [cat|小猫]，C 是西！",
        "门口跑来 [dog|小狗]，D 是迪！",
        "动物园里 [elephant|大象]，E 是伊！",
        "水里游过 [fish|小鱼]，F 是艾弗！",
        "山上有只 [goat|山羊]，G 是基！",
        "快闪环节开始啦！[apple|苹果]",
        "[banana|香蕉]",
        "[cat|小猫]",
        "[dog|小狗]",
        "[elephant|大象]",
        "[fish|小鱼]",
        "[goat|山羊]",
        "搭子考考你：A 念什么？A 的好朋友是哪个水果？大声告诉我！",
    ],
    "hn": [
        "继续认识 7 个英语字母新朋友，搭子陪你一起读！",
        "H 念作'哎吃'，H 的好朋友是 [h-or-se|horse|小马]",
        "I 念作'艾'，I 的好朋友是 [i-ce|ice|冰块]",
        "J 念作'杰'，J 的好朋友是 [j-ui-ce|juice|果汁]",
        "K 念作'开'，K 的好朋友是 [k-i-te|kite|风筝]",
        "L 念作'哎勒'，L 的好朋友是 [l-i-on|lion|狮子]",
        "M 念作'哎姆'，M 的好朋友是 [mon-key|monkey|猴子]",
        "N 念作'恩'，N 的好朋友是 [n-o-se|nose|鼻子]",
        "草原上跑来 [horse|小马]，H 是哎吃！",
        "夏天吃一块 [ice|冰块]，I 是艾！",
        "妈妈倒一杯 [juice|果汁]，J 是杰！",
        "天上飞着 [kite|风筝]，K 是开！",
        "森林之王 [lion|狮子]，L 是哎勒！",
        "树上爬着 [monkey|猴子]，M 是哎姆！",
        "脸上有个 [nose|鼻子]，N 是恩！",
        "快闪环节开始啦！[horse|小马]",
        "[ice|冰块]",
        "[juice|果汁]",
        "[kite|风筝]",
        "[lion|狮子]",
        "[monkey|猴子]",
        "[nose|鼻子]",
        "搭子考考你：L 念什么？L 的好朋友是哪个动物？你来说！",
    ],
    "ot": [
        "再来 6 个英语字母小伙伴，搭子带你冲冲冲！",
        "O 念作'欧'，O 的好朋友是 [o-range|orange|橙子]",
        "P 念作'披'，P 的好朋友是 [pan-da|panda|熊猫]",
        "Q 念作'科尤'，Q 的好朋友是 [qu-een|queen|女王]",
        "R 念作'啊'，R 的好朋友是 [rab-bit|rabbit|兔子]",
        "S 念作'艾思'，S 的好朋友是 [s-u-n|sun|太阳]",
        "T 念作'提'，T 的好朋友是 [ti-ger|tiger|老虎]",
        "果园里摘个 [orange|橙子]，O 是欧！",
        "竹林里坐着 [panda|熊猫]，P 是披！",
        "城堡里住着 [queen|女王]，Q 是科尤！",
        "草地上蹦着 [rabbit|兔子]，R 是啊！",
        "天上挂着 [sun|太阳]，S 是艾思！",
        "山林里走来 [tiger|老虎]，T 是提！",
        "快闪环节开始啦！[orange|橙子]",
        "[panda|熊猫]",
        "[queen|女王]",
        "[rabbit|兔子]",
        "[sun|太阳]",
        "[tiger|老虎]",
        "搭子考考你：T 念什么？T 的好朋友是哪种大动物？大声喊！",
    ],
    "uz": [
        "最后 6 个英语字母兄弟，认完就毕业啦！",
        "U 念作'优'，U 的好朋友是 [um-brel-la|umbrella|雨伞]",
        "V 念作'威'，V 的好朋友是 [vi-o-lin|violin|小提琴]",
        "W 念作'达不溜'，W 的好朋友是 [win-dow|window|窗户]",
        "X 念作'艾克斯'，X 的好朋友是 [xy-lo-phone|xylophone|木琴]",
        "Y 念作'歪'，Y 的好朋友是 [yel-low|yellow|黄色]",
        "Z 念作'贼'，Z 的好朋友是 [ze-bra|zebra|斑马]",
        "下雨打开 [umbrella|雨伞]，U 是优！",
        "音乐课拉响 [violin|小提琴]，V 是威！",
        "推开 [window|窗户]，W 是达不溜！",
        "敲响 [xylophone|木琴]，X 是艾克斯！",
        "天上的小鸭 [yellow|黄色]，Y 是歪！",
        "草原上奔跑 [zebra|斑马]，Z 是贼！",
        "毕业快闪一起来！[umbrella|雨伞]",
        "[violin|小提琴]",
        "[window|窗户]",
        "[xylophone|木琴]",
        "[yellow|黄色]",
        "[zebra|斑马]",
        "搭子考考你：Z 念什么？Z 的好朋友是哪种黑白动物？说出来！",
    ],
}


@registry.register(
    name="edu.english.letters",
    description=(
        "英文字母启蒙教学。用户说\"教字母\"/\"abc\"/\"26 个字母\"/\"教英文\"时调用。"
        "\n\n★铁律 1（音画同步）★ 每个 sentence_start 严格只含 1 个字母 + 1 个 [] 标记。"
        "禁止把 2 个或更多字母塞进同一句话（屏幕弹卡 3.5 秒/张，多卡同句 = 语音讲完了屏幕还在切）。"
        "\n反例：'A 念诶，B 念比，C 念西。' ❌ — 三个字母塞一句"
        "\n正例：'A 念诶。' '\\nB 念比。' '\\nC 念西。' ✅ — 每个字母一句话独立成 sentence_start"
        "\n\n★铁律 2（逐行原文）★ 必须按 templates[group] 数组**逐行原文输出**，"
        "禁止合并多行、改写文本、省略任何一行。每行就是 TTS 一个 sentence_start。"
        "\n\n教育卡格式（半角中括号+半角竖线）："
        "二段 [main|bottom] 渲染 word 卡两行；"
        "三段 [top|main|bottom] 渲染完整三行（top=自然拼读 phonics 如 a-pp-le）。"
        "竖线 | 和中括号 [ ] 不要朗读，TTS 应直接跳过。"
        "若用 dash 也兼容（[main-bottom]），但优先 |。默认 group=ag。"
    ),
    input_schema={
        "type": "object",
        "properties": {
            "group": {
                "type": "string",
                "enum": ["ag", "hn", "ot", "uz"],
                "description": "字母分组：ag(A-G 7个) / hn(H-N 7个) / ot(O-T 6个) / uz(U-Z 6个)",
                "default": "ag",
            },
        },
    },
)
def tool_lesson_english_letters(args: Dict[str, Any]) -> Dict[str, Any]:
    group = (args.get("group") or "ag").lower()
    if group not in ENGLISH_LETTERS_TEMPLATES:
        group = "ag"
    return {
        "group": group,
        "interval_ms": ENGLISH_LETTERS_INTERVAL_MS,
        "templates": {group: ENGLISH_LETTERS_TEMPLATES[group]},
    }


# --------------------------------------------------------------------------


def make_response(req_id: Any, result: Any) -> str:
    return json.dumps({"jsonrpc": "2.0", "id": req_id, "result": result}, ensure_ascii=False)


def make_error(req_id: Any, code: int, message: str) -> str:
    return json.dumps(
        {"jsonrpc": "2.0", "id": req_id, "error": {"code": code, "message": message}},
        ensure_ascii=False,
    )


async def handle_message(raw: str) -> str | None:
    try:
        msg = json.loads(raw)
    except json.JSONDecodeError as e:
        log.warning("invalid json: %s", e)
        return None

    method = msg.get("method")
    req_id = msg.get("id")
    params = msg.get("params") or {}

    if req_id is None:
        log.debug("notification ignored: %s", method)
        return None

    if method == "initialize":
        return make_response(req_id, {
            "protocolVersion": PROTOCOL_VERSION,
            "capabilities": {"tools": {}},
            "serverInfo": {"name": SERVER_NAME, "version": SERVER_VERSION},
        })

    # MCP 规范的心跳: 必须回空 result, 否则 broker 视为僵尸连接断开
    if method == "ping":
        return make_response(req_id, {})

    if method == "tools/list":
        return make_response(req_id, {"tools": registry.list_tools(), "nextCursor": ""})

    if method == "tools/call":
        name = params.get("name", "")
        arguments = params.get("arguments") or {}
        log.info("tools/call %s %s", name, arguments)
        try:
            result = registry.call(name, arguments)
        except KeyError:
            return make_error(req_id, -32601, f"Unknown tool: {name}")
        except Exception as e:
            log.exception("tool error")
            return make_response(req_id, {
                "content": [{"type": "text", "text": f"error: {e}"}],
                "isError": True,
            })
        text = result if isinstance(result, str) else json.dumps(result, ensure_ascii=False)
        return make_response(req_id, {
            "content": [{"type": "text", "text": text}],
            "isError": False,
        })

    return make_error(req_id, -32601, f"Method not found: {method}")


async def run_once(endpoint: str, token: str) -> None:
    url = f"{endpoint}?token={token}" if "?" not in endpoint else f"{endpoint}&token={token}"
    log.info("connecting %s", endpoint)
    async with websockets.connect(url, max_size=4 * 1024 * 1024, ping_interval=30) as ws:
        log.info("connected (tools=%d)", len(registry.list_tools()))
        async for raw in ws:
            if isinstance(raw, bytes):
                raw = raw.decode("utf-8", errors="replace")
            log.debug("<- %s", raw)
            reply = await handle_message(raw)
            if reply is not None:
                log.debug("-> %s", reply)
                await ws.send(reply)


async def run_forever(endpoint: str, token: str) -> None:
    backoff = 1
    while True:
        started = time.monotonic()
        try:
            await run_once(endpoint, token)
            backoff = 1
        except ConnectionClosed as e:
            log.warning("connection closed: %s", e)
        except Exception:
            log.exception("connection error")
        # 连接超过 30s 算一次成功的会话，重置退避
        if time.monotonic() - started > 30:
            backoff = 1
        delay = min(backoff, 60)
        log.info("reconnect in %ds", delay)
        await asyncio.sleep(delay)
        backoff = min(backoff * 2, 60)


def load_env_file(path: Path) -> None:
    if not path.exists():
        return
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        os.environ.setdefault(k.strip(), v.strip().strip('"').strip("'"))


def main() -> int:
    here = Path(__file__).resolve().parent
    load_env_file(here / ".env")

    parser = argparse.ArgumentParser(description="Xiaozhi MCP local server")
    parser.add_argument("--token", default=os.environ.get("XIAOZHI_MCP_TOKEN") or EMBEDDED_TOKEN,
                        help="MCP endpoint token (必须通过 --token 或 XIAOZHI_MCP_TOKEN 环境变量提供)")
    parser.add_argument("--endpoint", default=os.environ.get("XIAOZHI_MCP_ENDPOINT", DEFAULT_ENDPOINT),
                        help=f"MCP endpoint base URL (默认 {DEFAULT_ENDPOINT})")
    parser.add_argument("--debug", action="store_true", help="打印每条 JSON-RPC 收发")
    parser.add_argument("--check", action="store_true", help="仅校验 templates 合规，不连接云端")
    args = parser.parse_args()

    if args.debug:
        log.setLevel(logging.DEBUG)

    # 启动期自检 — 教育卡 templates 必须符合设备端 ExtractEduWordCard 解析规则
    errors = []
    for grp, lines in ENGLISH_LETTERS_TEMPLATES.items():
        errors.extend(validate_edu_card_lines(f"english_letters[{grp}]", lines))
    if errors:
        log.error("教育卡 templates 校验失败 (%d 项):", len(errors))
        for e in errors:
            log.error("  %s", e)
        return 3
    log.info("templates 校验通过 (4 组 / 78 张卡 / 23·23·19·19 行)")

    if args.check:
        log.info("--check 模式仅校验，不连接云端")
        return 0

    if not args.token:
        log.error("缺少 token: 请用 --token 或设置 XIAOZHI_MCP_TOKEN 环境变量")
        return 2

    try:
        asyncio.run(run_forever(args.endpoint, args.token))
    except KeyboardInterrupt:
        log.info("bye")
    return 0


if __name__ == "__main__":
    sys.exit(main())
