// =============================================================================
//  src/platform/command_line.h —— Windows 命令行拼装（B7 / S-02 修复）
// =============================================================================
//  背景：`CreateProcessW(nullptr, cmdline, ...)` 在 lpApplicationName 为 NULL 时要
//  **自己去猜** argv[0] 到哪儿结束（按空白切分 + 尝试更长前缀）。所以
//
//      CreateProcessW(nullptr, L"C:\\Program Files\\Anlogic\\TD\\bin\\td.exe", ...)
//
//  会尝试运行 `C:\Program.exe`（CWE-428 Unquoted Search Path）——exe 路径一含空格
//  就截断，既起不来也可能被同目录下预置的同名程序劫持。
//
//  正确写法（Microsoft 文档 + CPython 的做法）：
//   * lpApplicationName = exe 的完整路径（**权威**，不受命令行切分影响）
//   * lpCommandLine     = 一个合法命令行，argv[0] 按 CRT 规则加引号
//
//  本文件只做字符串处理（不调用 Win32），因此可以在任意平台编译与单测。
//  引号规则与 CommandLineToArgvW / MSVC CRT 一致：
//   * 参数为空 -> `""`
//   * 不含空格/制表/换行/引号 -> 原样
//   * 否则整段加引号；引号前的反斜杠数翻倍，末尾反斜杠数翻倍
// =============================================================================

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace fpgalsp::platform {

// 按 CRT/CommandLineToArgvW 规则引用**一个**参数。
inline std::wstring QuoteWindowsArgument(std::wstring_view arg) {
    if (arg.empty()) return {L"\"\""};
    const bool needs_quotes =
        arg.find_first_of(L" \t\n\v\"") != std::wstring_view::npos;
    if (!needs_quotes) return std::wstring(arg);

    std::wstring out;
    out.reserve(arg.size() + 8);
    out.push_back(L'"');
    std::size_t backslashes = 0;
    for (const wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            // 反斜杠 + 引号：反斜杠翻倍，再输出一个被转义的引号
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
            backslashes = 0;
            continue;
        }
        out.append(backslashes, L'\\');
        backslashes = 0;
        out.push_back(c);
    }
    // 收尾：反斜杠翻倍，否则最后一个 `"` 会被当成转义
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

// 拼出完整命令行：argv[0]（exe）+ 参数。
inline std::wstring BuildWindowsCommandLine(std::wstring_view exe,
                                            const std::vector<std::wstring>& args = {}) {
    std::wstring line = QuoteWindowsArgument(exe);
    for (const std::wstring& arg : args) {
        line.push_back(L' ');
        line += QuoteWindowsArgument(arg);
    }
    return line;
}

}  // namespace fpgalsp::platform