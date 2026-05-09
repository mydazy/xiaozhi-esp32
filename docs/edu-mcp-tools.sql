-- =====================================================================
-- 启蒙教育 MCP 工具配置初始化 SQL  (v6.3)
-- 表：ai_mcp_tool_config（云端后台 MCP 本地工具配置覆盖表）
--
-- 用途：
--   1. 注册启蒙教育系列 30 个 MCP 工具的命名 / 分组 / 排序
--   2. demo 工具 lesson_pinyin_initials 携带完整话术，is_enabled='1' 立即启用
--   3. 其余 29 个工具仅占位（is_enabled='0'），待运营按表填话术后改 '1' 启用
--
-- 命名规范：{lesson/quiz/review/track}_{学科}_{细分}
-- 分组规范：edu_{学科}（pinyin / english / math / hanzi / kids / meta）
-- 排序段位：100s 拼音 / 200s 英语 / 300s 数学 / 400s 汉字 / 500s 通识 / 900s 元工具
--
-- 配套文档：docs/edu-prompt-v6.md（云端 LLM system prompt v6.3）
-- 配套代码：main/application.cc::ExtractEduWordCards（v6.3 解析）
-- 教育卡格式（v6.3）：
--   二段 [main|bottom]            → top="" / 自动 word 卡两行
--   三段 [top|main|bottom]        → 三行完整卡（推荐 phonics / 拼音 / 汉字）
--   '|' 优先无歧义；'-' 兜底兼容（LLM 习惯 + 老 demo 格式）
--   单句可含多个 []，设备端按 ~3.5s 节拍依次弹（上限 8 张）
-- 教学方法：五段式（引入→首次教学→场景应用→快闪复习→主动回忆）
--           每个目标项弹 3 次教育卡 · 强化记忆
-- =====================================================================


-- =====================================================================
-- Part 1: Demo 工具（携带完整话术 · 立即启用）
-- =====================================================================

INSERT INTO `ai_mcp_tool_config`
  (`tenant_id`, `tool_name`, `tool_description`, `tool_group`, `tool_order`,
   `is_enabled`, `param_overrides`, `extra_config`, `remark`,
   `create_time`, `update_time`)
VALUES
  ('000000', 'lesson_pinyin_initials',
   '拼音声母教学。当用户说"教声母"/"b 怎么读"/"拼音从哪学"/"认拼音"时调用，返回声母教学话术。LLM 必须按 templates[group] 数组顺序逐行 100% 复述，每行独立成句，禁止合并/跳过/改写。卡片格式 [声母|声母|谐音字]（三段）或 [声母|谐音字]（二段），半角竖线分隔。话术内置"首次教学→场景应用→快闪复习→主动回忆"四段式，每个声母弹 3 次教育卡强化记忆。',
   'edu_pinyin', 101, '1',
   '{"group":{"description":"声母分组：lip(b p m f) / tip(d t n l) / root(g k h) / palate(j q x) / retroflex(zh ch sh r) / flat(z c s) / semi(y w)。默认 lip","example":"lip"}}',
   '{"interval_ms":3500,"templates":{"lip":["今天搭子带你认识 4 个嘴唇兄弟，准备好了吗？","b 像数字 6 倒一倒，嘴一闭气一冲：[声母|b|波]","p 像 9 反过来，嘴一爆喷口气：[声母|p|坡]","m 像两座小山峰，嘴抿住哼一哼：[声母|m|摸]","f 像一根小拐杖，上齿轻咬下唇：[声母|f|佛]","现在用它们造句！爸爸的爸，开头是 [b|波]","爬山的爬，开头是 [p|坡]","妈妈喊你回家，开头是 [m|摸]","飞机飞上天，开头是 [f|佛]","再来一遍快闪，准备好你的眼睛和嘴巴！[b|波]","[p|坡]","[m|摸]","[f|佛]","搭子来考考你：爸爸的爸是哪个声母开头呀？你来读读看！"]}}',
   '拼音声母教学（demo · lip 唇音 4 个 b p m f）· v6.3 三段格式 [声母|b|波] · 五段式 · 每声母弹 3 次卡 · 总时长 ~50 秒',
   NOW(), NOW())
ON DUPLICATE KEY UPDATE
  `tool_description` = VALUES(`tool_description`),
  `tool_group`       = VALUES(`tool_group`),
  `tool_order`       = VALUES(`tool_order`),
  `is_enabled`       = VALUES(`is_enabled`),
  `param_overrides`  = VALUES(`param_overrides`),
  `extra_config`     = VALUES(`extra_config`),
  `remark`           = VALUES(`remark`),
  `update_time`      = NOW();


-- =====================================================================
-- Part 2: 系列工具命名规划骨架（29 个 · 占位禁用）
--   - is_enabled='0' 占位，待运营按表填 extra_config 话术后改 '1' 启用
--   - tool_description 已写好 LLM 触发提示，可直接生效
--   - 命名进数据库后 UNIQUE KEY 强约束，避免命名冲突
-- =====================================================================

INSERT IGNORE INTO `ai_mcp_tool_config`
  (`tenant_id`, `tool_name`, `tool_description`, `tool_group`, `tool_order`,
   `is_enabled`, `param_overrides`, `extra_config`, `remark`,
   `create_time`, `update_time`)
VALUES

-- ──────── 拼音 edu_pinyin 100-199（4-6 岁主攻） ────────
('000000','lesson_pinyin_finals',    '拼音韵母教学。用户说"教韵母"/"a o e"/"复韵母"/"前后鼻韵母"时调用','edu_pinyin',102,'0',NULL,NULL,'拼音 24 韵母 · 单 6/复 9/前鼻 5/后鼻 4',NOW(),NOW()),
('000000','lesson_pinyin_whole',     '整体认读音节教学。用户说"教整体认读"/"yi wu yu"/"zhi chi shi"时调用','edu_pinyin',103,'0',NULL,NULL,'整体认读 16 个 · 不可拼读必须整记',NOW(),NOW()),
('000000','lesson_pinyin_tones',     '拼音四声教学。用户说"教四声"/"X 字四声"/"声调"时调用','edu_pinyin',104,'0',NULL,NULL,'四声 · 支持任意拼音字符四声展示',NOW(),NOW()),
('000000','lesson_pinyin_blend',     '拼读教学。用户说"拼一拼"/"b 加 a 怎么读"/"怎么拼"时调用','edu_pinyin',105,'0',NULL,NULL,'声母+韵母+声调 拼读训练',NOW(),NOW()),
('000000','quiz_pinyin',             '拼音综合测验。用户说"考考我拼音"/"我会读了"时调用','edu_pinyin',199,'0',NULL,NULL,'拼音综合 quiz · 主动回忆',NOW(),NOW()),

-- ──────── 英语启蒙 edu_english 200-299 ────────
('000000','lesson_english_letters',  '英文字母教学。用户说"教字母"/"abc"/"26 个字母"时调用','edu_english',201,'0',NULL,NULL,'26 字母 · 4 组分批 + 自然拼读 phonics',NOW(),NOW()),
('000000','lesson_english_phonics',  '自然拼读教学。用户说"自然拼读"/"phonics"/"字母音"时调用','edu_english',202,'0',NULL,NULL,'A 发 /æ/ B 发 /b/...',NOW(),NOW()),
('000000','lesson_english_numbers',  '数字英文教学。用户说"英文数数"/"one two three"时调用','edu_english',203,'0',NULL,NULL,'1-20 / 整十数 / 100 以内',NOW(),NOW()),
('000000','lesson_english_colors',   '颜色英文教学。用户说"教颜色英文"/"red blue"时调用','edu_english',204,'0',NULL,NULL,'11 颜色 · basic + extended',NOW(),NOW()),
('000000','lesson_english_words',    '高频词教学。用户说"教单词"/"水果英文"/"动物英文"时调用','edu_english',205,'0',NULL,NULL,'家人/动物/水果/身体/天气 主题词包',NOW(),NOW()),
('000000','quiz_english',            '英语综合测验。用户说"考考我英语"时调用','edu_english',299,'0',NULL,NULL,'英语综合 quiz',NOW(),NOW()),

-- ──────── 数学启蒙 edu_math 300-399 ────────
('000000','lesson_math_count',       '数数教学。用户说"数数"/"教数字"/"1 到 10"时调用 · 中英双语','edu_math',301,'0',NULL,NULL,'1-100 · 中英双语',NOW(),NOW()),
('000000','lesson_math_addition',    '加法教学。用户说"教加法"/"1+1"时调用','edu_math',302,'0',NULL,NULL,'10/20/100 以内加法',NOW(),NOW()),
('000000','lesson_math_subtract',    '减法教学。用户说"教减法"时调用','edu_math',303,'0',NULL,NULL,'10/20/100 以内减法',NOW(),NOW()),
('000000','lesson_math_multiply',    '乘法口诀教学。用户说"教乘法"/"九九表"/"X 的乘法"时调用','edu_math',304,'0',NULL,NULL,'9×9 乘法 · 81 句完整口诀',NOW(),NOW()),
('000000','lesson_math_shapes',      '形状教学。用户说"教形状"/"圆形方形"时调用','edu_math',305,'0',NULL,NULL,'圆 方 三角 长方 椭圆 五角星 菱形',NOW(),NOW()),
('000000','lesson_math_time',        '时间教学。用户说"教时钟"/"星期"/"几点"时调用','edu_math',306,'0',NULL,NULL,'时钟 / 星期 / 月份 / 季节',NOW(),NOW()),
('000000','quiz_math',               '数学综合测验。用户说"考考我数学"时调用','edu_math',399,'0',NULL,NULL,'数学综合 quiz',NOW(),NOW()),

-- ──────── 汉字启蒙 edu_hanzi 400-499 ────────
('000000','lesson_hanzi_chars',      '识字教学。用户说"教字"/"教我认字"时调用','edu_hanzi',401,'0',NULL,NULL,'800 常用字 · 一年级范围',NOW(),NOW()),
('000000','lesson_hanzi_strokes',    '笔画教学。用户说"X 怎么写"/"教笔画"时调用 · 配合 show_stroke 笔画动画','edu_hanzi',402,'0',NULL,NULL,'8 基本笔画 横竖撇捺点折提钩',NOW(),NOW()),
('000000','lesson_hanzi_radicals',   '偏旁部首教学。用户说"教偏旁"/"X 字旁"时调用','edu_hanzi',403,'0',NULL,NULL,'50 高频偏旁',NOW(),NOW()),
('000000','lesson_hanzi_words',      '组词造句教学。用户说"X 字组词"/"造句"时调用','edu_hanzi',404,'0',NULL,NULL,'词组 / 同音字 / 形近字',NOW(),NOW()),
('000000','quiz_hanzi',              '识字综合测验。用户说"考考我识字"时调用','edu_hanzi',499,'0',NULL,NULL,'识字综合 quiz',NOW(),NOW()),

-- ──────── 通识启蒙（3-9 岁）edu_kids 500-599 ────────
('000000','lesson_kids_animals',     '动物认知。用户说"教动物"/"小猫小狗"时调用 · 双语','edu_kids',501,'0',NULL,NULL,'家禽/家畜/野生/水生 4 类',NOW(),NOW()),
('000000','lesson_kids_fruits',      '水果认知。用户说"教水果"/"苹果香蕉"时调用 · 双语','edu_kids',502,'0',NULL,NULL,'15+ 常见水果',NOW(),NOW()),
('000000','lesson_kids_family',      '家人称谓。用户说"爸爸妈妈"/"爷爷奶奶英文"时调用 · 双语','edu_kids',503,'0',NULL,NULL,'8 家人称谓',NOW(),NOW()),
('000000','lesson_kids_body',        '身体部位。用户说"教身体"/"眼睛鼻子"时调用 · 双语','edu_kids',504,'0',NULL,NULL,'10+ 身体部位',NOW(),NOW()),
('000000','lesson_kids_weather',     '天气认知。用户说"教天气"/"下雨刮风"时调用 · 双语','edu_kids',505,'0',NULL,NULL,'晴 雨 雪 云 风 雷 雾',NOW(),NOW()),
('000000','lesson_kids_jobs',        '职业认知。用户说"教职业"/"医生老师"时调用 · 双语','edu_kids',506,'0',NULL,NULL,'10+ 常见职业',NOW(),NOW()),
('000000','lesson_kids_traffic',     '交通工具。用户说"教汽车飞机"/"交通工具"时调用 · 双语','edu_kids',507,'0',NULL,NULL,'10+ 交通工具',NOW(),NOW()),

-- ──────── 元工具（间隔重复 / 学习记录）edu_meta 900-999 ────────
('000000','review_today',            '今日复习。用户说"今天学了啥"/"复习一下"时调用，按今日教学记录回顾','edu_meta',901,'0',NULL,NULL,'今日已学项目快闪复习 · 间隔 1h/4h',NOW(),NOW()),
('000000','review_yesterday',        '昨日复习。每日早晨主动调用，按艾宾浩斯曲线回顾昨日内容','edu_meta',902,'0',NULL,NULL,'艾宾浩斯曲线 · 1 day 复习点',NOW(),NOW()),
('000000','track_progress',          '学习进度查询。家长说"娃学到哪了"/"进度"时调用','edu_meta',903,'0',NULL,NULL,'学习进度统计 · 家长查询入口',NOW(),NOW());


-- =====================================================================
-- 验证 SQL（执行后跑一遍确认）
-- =====================================================================

-- 1) 查看所有教育系列工具状态
-- SELECT tool_name, tool_group, tool_order, is_enabled, LEFT(remark, 40) AS note
-- FROM ai_mcp_tool_config
-- WHERE tool_group LIKE 'edu_%'
-- ORDER BY tool_order;

-- 2) 查看 demo 工具的扩展配置
-- SELECT tool_name, JSON_PRETTY(extra_config) AS script
-- FROM ai_mcp_tool_config
-- WHERE tool_name = 'lesson_pinyin_initials';

-- 3) 启用某工具（运营填完话术后执行）
-- UPDATE ai_mcp_tool_config
-- SET is_enabled = '1',
--     extra_config = '{...新话术 JSON...}',
--     update_time = NOW()
-- WHERE tool_name = 'lesson_pinyin_finals';

-- 4) 一键禁用某分组（紧急回滚）
-- UPDATE ai_mcp_tool_config SET is_enabled = '0' WHERE tool_group = 'edu_pinyin';

-- 5) 统计各分组工具数量
-- SELECT tool_group, COUNT(*) AS total,
--        SUM(CASE WHEN is_enabled='1' THEN 1 ELSE 0 END) AS enabled
-- FROM ai_mcp_tool_config
-- WHERE tool_group LIKE 'edu_%'
-- GROUP BY tool_group;
