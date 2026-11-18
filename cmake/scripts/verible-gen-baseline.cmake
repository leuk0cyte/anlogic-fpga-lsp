# =============================================================================
#  cmake/scripts/verible-gen-baseline.cmake — 生成物计数基线
# =============================================================================
#  用途：`计数基线` 层（task-10 第 2 层）。抓的是**"锚点数量变了但没归零"**这种
#  场景——那是 `count > 0` 断言和 `count_a == count_b` 差分断言都抓不到的。
#
#  三层防线的分工（缺一不可）：
#    第 1 层 差分断言  —— 不依赖任何固化常量："补丁把每一处锚点都处理了"就绿。
#                        工具升级后自动适应，不制造假阳性。见 verible-gen-check.cmake
#    第 2 层 计数基线  —— 本文件。固化"当前工具链下锚点应有的数量"。
#                        数量变化即失败，报错信息给出"期望 N，实际 M"。
#    第 3 层 版本软断言 —— 工具版本不匹配时 WARNING（不阻断），让人看见。
#
#  ★★★ 更新本文件的规则（重要，请勿跳过）★★★
#  基线的每一次更新都必须能回答"为什么变了"。如果某次提交里
#    · 只改了本文件，
#    · 而没有任何其他变化（bison/flex 版本、verilog.y、verilog.lex、
#      GENERATOR-NOTES.md 都没动），
#  那么这次基线更新**大概率是为了让红的构建变绿**，应当被 review 拒绝。
#
#  这条把"是否滑坡"变成了可机械检查的规则：
#      git diff --name-only | grep verible-gen-baseline.cmake
#  且同时不含 *.y / *.lex / GENERATOR-NOTES.md / 工具版本文件
#      => 标记为"需人工 review 基线变更"
#
#  绝对不要因为"数字对不上"就顺手改这里。先搞清楚为什么对不上。
# =============================================================================

# -----------------------------------------------------------------------------
# 工具链版本（第 3 层软断言的依据）
# -----------------------------------------------------------------------------
# 本工程验证过的版本。升级时这里会 WARNING（不是 FATAL_ERROR：升级不一定错，
# 但必须让人看见，否则"为了能编译先改断言"的坏习惯会毁掉这道门）。
set(VERIBLE_BASELINE_BISON_VERSION "3.8.2")
set(VERIBLE_BASELINE_FLEX_VERSION  "2.6.4")

# -----------------------------------------------------------------------------
# G4: gen-verilog-token-enum
# -----------------------------------------------------------------------------
# 从 verilog.tab.hh 抽取的 token 枚举块数量（当前 bison 输出里只有 1 块）
set(VERIBLE_BASELINE_G4_ENUM_BLOCKS 1)

# -----------------------------------------------------------------------------
# G5: verilog-parse-interface
# -----------------------------------------------------------------------------
# 从 verilog.tab.hh 删除的"枚举段"块数量（当前只有 1 块）
#
# 历史：G5 的原 Bazel sed 写的是 '#ifndef YYTOKENTYPE'，而 bison 3.8.2 配合
# `%define api.prefix {verilog_}` 生成的是 '#ifndef VERILOG_TOKENTYPE'。
# 若只按字面匹配 YYTOKENTYPE，命中 0 次、静默空操作 —— parse-interface 会退化成
# verilog.tab.hh 的原样拷贝。本基线固定为 1 就是为了防止它悄悄退回 0。
set(VERIBLE_BASELINE_G5_ENUM_BLOCKS 1)

# -----------------------------------------------------------------------------
# G6a: std_move_parser_symbols
# -----------------------------------------------------------------------------
# 四类锚点在 bison 3.8.2 输出中的出现次数（也是补丁后的替换次数）。
#   注意 "= yylval;" 是 2 次、"= yyval;" 是 1 次 —— 两者不同，别想当然。
set(VERIBLE_BASELINE_G6A_MOVE_YYLVAL   2)   # std::move(yylval) 出现次数
set(VERIBLE_BASELINE_G6A_MOVE_YYVAL    1)   # std::move(yyval) 出现次数
set(VERIBLE_BASELINE_G6A_MOVE_DEFAULT  1)   # std::move(yyval_default) 出现次数
set(VERIBLE_BASELINE_G6A_COMMENTED     1)   # 被 '// ' 注释掉的 yyval 复位语句数

# -----------------------------------------------------------------------------
# G6b: record_recovered_syntax_errors
# -----------------------------------------------------------------------------
# 原始 verilog.tab.cc 中 '++yynerrs;' 的出现次数，也等于应插入的补丁组数。
#
# ★ 这个数字是 G6b 最核心的指纹。如果 bison 升级改变了错误恢复路径，
#   这个数会变 —— 这正是 `count>0` 和差分断言都抓不到的场景。
#
#   注意：strlen("++yynerrs;") == 10（不是 11）。历史上有过一次差一错误，
#   导致计数恒为 0，让"hits=0 即失败"误报。
set(VERIBLE_BASELINE_G6B_YYNERRS 2)

# -----------------------------------------------------------------------------
# 汇总：供 verible-gen-check.cmake 直接 foreach 遍历比对
# -----------------------------------------------------------------------------
# 每一项形如 "名称=期望值"，检查脚本会解析成 名称/值 两段。
set(VERIBLE_GEN_BASELINE_COUNTS
    "g4.blocks=${VERIBLE_BASELINE_G4_ENUM_BLOCKS}"
    "g5.blocks=${VERIBLE_BASELINE_G5_ENUM_BLOCKS}"
    "g6a.move_yylval=${VERIBLE_BASELINE_G6A_MOVE_YYLVAL}"
    "g6a.move_yyval=${VERIBLE_BASELINE_G6A_MOVE_YYVAL}"
    "g6a.move_yyval_default=${VERIBLE_BASELINE_G6A_MOVE_DEFAULT}"
    "g6a.commented=${VERIBLE_BASELINE_G6A_COMMENTED}"
    "g6b.yynerrs=${VERIBLE_BASELINE_G6B_YYNERRS}"
)