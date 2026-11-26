# =============================================================================
#  tests/e2e/ —— 端到端冒烟（真进程 + 真 TD）
# =============================================================================
#  lsp_smoke.py 把 build/bin/fpgalsp-native.exe 当真正的 LSP 服务器驱动：
#  initialize -> didOpen -> fpgalsp.info -> fpgaLsp/moduleTree -> fpgalsp.check
#  -> shutdown/exit，并在退出后断言 **0 个残留 td_commands_prompt.exe**。
#
#  为什么放在 CTest 里（而不是手工脚本）：
#    * exit 时的孤儿进程问题是**跨进程**的，单元测试抓不到（PYTHON-BUGS P-5）
#    * 能力声明与协议文档逐项对照只有从 wire 上看才知道对不对
#    * 一次实测约 5-20 s（含真实 TD read_hdl），可以接受
#
#  单独运行：
#    python tests/e2e/lsp_smoke.py --server build/bin/fpgalsp-native.exe
#    ctest -L e2e --output-on-failure
# =============================================================================

if(NOT BUILD_TESTING)
    return()
endif()

if(NOT TARGET fpgalsp-native)
    message(STATUS "tests/e2e: 未找到 fpgalsp-native target，跳过 e2e 注册")
    return()
endif()

find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT Python3_Interpreter_FOUND)
    message(WARNING "tests/e2e: 未找到 Python3 解释器，跳过 e2e 注册")
    return()
endif()

add_test(
    NAME e2e.lsp_smoke
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/lsp_smoke.py"
            --server "$<TARGET_FILE:fpgalsp-native>"
)
set_tests_properties(e2e.lsp_smoke PROPERTIES
    LABELS "e2e"
    # 含一次真实 TD 检查（import_device + read_hdl）；留足余量避免机器忙时假红
    TIMEOUT 900
    # TD 检查期间会拉起 td_commands_prompt.exe，断言里用 PID 差集判断，
    # 因此本用例**不能**与其他 td 用例并行（ctest -j 时靠 RUN_SERIAL 保证）。
    RUN_SERIAL TRUE
    RESOURCE_LOCK "td-process"
)

message(STATUS "tests/e2e: 已注册 e2e.lsp_smoke（ctest -L e2e）")

# =============================================================================
#  MCP server 端到端冒烟（task-18）
# =============================================================================
#  纯协议 + 只读工具，不需要真实 TD（约 1-3 s）；只断言 wire 上的形状：
#  版本协商、39/32 个工具、isError 语义、显式占位、shutdown 干净退出。
add_test(
    NAME e2e.mcp_smoke
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/mcp_smoke.py"
            --server "$<TARGET_FILE:fpgalsp-native>"
)
set_tests_properties(e2e.mcp_smoke PROPERTIES
    LABELS "e2e"
    TIMEOUT 300
    # 与 lsp_smoke 一样会拉起子进程；为了 td 孤儿断言（PID 差集）不并行
    RUN_SERIAL TRUE
    RESOURCE_LOCK "td-process"
)

message(STATUS "tests/e2e: 已注册 e2e.mcp_smoke（ctest -L e2e）")

# =============================================================================
#  MCP flow 类工具（**只在临时副本上跑**）
# =============================================================================
#  fpga_flow_run 会驱动真实 TD 并往工程目录写 *_Runs/ 与 .fpgalsp/；
#  tests/gate-corpus 是多人共用的只读语料，写脏会让 td.flow 假失败（真实发生过）。
#  所以这个用例自己 copytree 一份副本，跑完断言原语料没有被写。
add_test(
    NAME e2e.mcp_flow_smoke
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/mcp_flow_smoke.py"
            --server "$<TARGET_FILE:fpgalsp-native>"
)
set_tests_properties(e2e.mcp_flow_smoke PROPERTIES
    LABELS "e2e"
    TIMEOUT 600
    # 会拉起 TD 子进程；与 lsp_smoke 一样不并行（PID 差集断言 / TD 争用）
    RUN_SERIAL TRUE
    RESOURCE_LOCK "td-process"
)

message(STATUS "tests/e2e: 已注册 e2e.mcp_flow_smoke（ctest -L e2e）")
# =============================================================================
#  P-A1：MCP loopback 多客户端并发
# =============================================================================
#  旧实现是 accept -> Serve（阻塞）-> 再 accept，第二个客户端能连上但永远没响应。
#  这个用例用"半行不换行"把第一个连接卡住，断言第二个客户端仍能 ping 通；
#  再用 FPGALSP_MCP_CLIENTS=1 断言超限连接收到 -32002（而不是静默挂起）。
add_test(
    NAME e2e.mcp_concurrency
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/mcp_concurrency.py"
            --server "$<TARGET_FILE:fpgalsp-native>"
)
set_tests_properties(e2e.mcp_concurrency PROPERTIES
    LABELS "e2e"
    TIMEOUT 120
    # 会拉起子进程并占用 loopback 端口；与其它 e2e 一样不并行
    RUN_SERIAL TRUE
)

message(STATUS "tests/e2e: 已注册 e2e.mcp_concurrency（ctest -L e2e）")

# =============================================================================
#  P-A3：MCP loopback 端点广告 + 令牌鉴权
# =============================================================================
#  断言端点文件（真实端口/token/pid）、强制档下无 token 被 -32001 拒绝、
#  正确/错误 token 的差异，以及 attach 代理会替旧客户端把 token 注入 initialize。
add_test(
    NAME e2e.mcp_auth
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/mcp_auth.py"
            --server "$<TARGET_FILE:fpgalsp-native>"
)
set_tests_properties(e2e.mcp_auth PROPERTIES
    LABELS "e2e"
    TIMEOUT 120
    RUN_SERIAL TRUE
)

message(STATUS "tests/e2e: 已注册 e2e.mcp_auth（ctest -L e2e）")

# -----------------------------------------------------------------------------
#  release 分支：这些用例需要一个**自备的 .al 工程**（以及本机 TD 安装）。
#  公开仓库不带内部工程语料，因此未设置 FPGA_LSP_TEST_PROJECT 时标记为跳过
#  （ctest 里显示 Not Run，而不是把'缺数据'报成失败）。
#
#      $env:FPGA_LSP_TEST_PROJECT = 'C:/path/to/your/project'
#      ctest -L e2e --output-on-failure
# -----------------------------------------------------------------------------
if(NOT DEFINED ENV{FPGA_LSP_TEST_PROJECT})
    message(STATUS "tests/e2e: 未设置 FPGA_LSP_TEST_PROJECT —— e2e 用例标记为跳过")
    foreach(_e2e IN ITEMS e2e.lsp_smoke e2e.mcp_smoke e2e.mcp_flow_smoke
                            e2e.mcp_concurrency e2e.mcp_auth)
        if(TEST ${_e2e})
            set_tests_properties(${_e2e} PROPERTIES DISABLED TRUE)
        endif()
    endforeach()
endif()
