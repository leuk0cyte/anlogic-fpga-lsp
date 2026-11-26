// Copy the language server into the extension so the .vsix is self-contained.
//
// **两份**都可能被打包（原生 exe 优先，Python 作为过渡回退）：
//
//   server/bin/fpgalsp-native.exe   <- C++ 重写版（推荐；扩展优先用它）
//   server/fpgalsp/__main__.py      <- Python 版（找不到 exe 时的回退）
//
// 原生 exe 的查找顺序：
//   1. 环境变量 `FPGA_LSP_NATIVE_EXE`（CI / 手工指定）
//   2. 命令行第一个参数
//   3. 兄弟仓库的构建目录：`<extRoot>/../../fpga-lsp-cpp/build*/bin/fpgalsp-native.exe`
//   4. 本仓库构建目录：`<extRoot>/../build*/bin/fpgalsp-native.exe`
//
// Run automatically by `npm run package`; also handy before `code --extensionDevelopmentPath`.

'use strict';

const fs = require('fs');
const path = require('path');

const extRoot = path.resolve(__dirname, '..');
const dstRoot = path.join(extRoot, 'server');
const NATIVE_EXE = process.platform === 'win32' ? 'fpgalsp-native.exe' : 'fpgalsp-native';
const BUILD_DIRS = ['build', 'build-hdl', 'build-flow', 'build-mcp', 'build-fmt'];

function findNativeExe() {
    const explicit = [
        process.env.FPGA_LSP_NATIVE_EXE,
        process.argv[2],
    ].filter(Boolean);
    for (const candidate of explicit) {
        if (candidate && fs.existsSync(candidate) && fs.statSync(candidate).isFile()) {
            return candidate;
        }
    }
    // 扩展可能在 C++ 仓库内，也可能是兄弟仓库（%PYREF%\vscode-extension -> Y:\fpga-lsp-cpp）
    const repoRoots = [
        path.resolve(extRoot, '..'),
        path.resolve(extRoot, '..', '..', 'fpga-lsp-cpp'),
    ];
    for (const repo of repoRoots) {
        for (const build of BUILD_DIRS) {
            const candidate = path.join(repo, build, 'bin', NATIVE_EXE);
            if (fs.existsSync(candidate)) return candidate;
        }
    }
    return '';
}

function copyTree(src, dst) {
    fs.cpSync(src, dst, {
        recursive: true,
        filter: (file) => !file.includes(`${path.sep}__pycache__`) && !file.endsWith('.pyc'),
    });
}

function countFiles(dir) {
    let n = 0;
    if (!fs.existsSync(dir)) return 0;
    for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
        if (entry.isDirectory()) n += countFiles(path.join(dir, entry.name));
        else n += 1;
    }
    return n;
}

function sizeOf(dir) {
    let total = 0;
    if (!fs.existsSync(dir)) return 0;
    for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
        const p = path.join(dir, entry.name);
        total += entry.isDirectory() ? sizeOf(p) : fs.statSync(p).size;
    }
    return total;
}

fs.rmSync(dstRoot, { recursive: true, force: true });
fs.mkdirSync(dstRoot, { recursive: true });

let bundledNative = '';
let bundledPython = false;

// ---- 1) 原生 exe ---------------------------------------------------------
const nativeExe = findNativeExe();
if (nativeExe) {
    const dstDir = path.join(dstRoot, 'bin');
    fs.mkdirSync(dstDir, { recursive: true });
    const dst = path.join(dstDir, NATIVE_EXE);
    fs.copyFileSync(nativeExe, dst);
    bundledNative = nativeExe;
    console.log(`[bundle-server] native: ${nativeExe} -> server/bin/${NATIVE_EXE} ` +
        `(${(fs.statSync(dst).size / 1024 / 1024).toFixed(1)} MiB)`);
} else {
    console.warn('[bundle-server] 未找到 fpgalsp-native.exe（原生服务器不会被包含；' +
        '设置 FPGA_LSP_NATIVE_EXE 或先构建 build/bin）');
}

// ---- 2) Python：自 0.11.0 起**不再打包** -----------------------------------
//  本扩展只对接原生 exe（fpgalsp-native.exe）。Python 后端属于迁移期的参考实现，
//  保留在 Y:\fpga-lsp，不再随扩展分发。
//  对应的运行时回退也已从 src/locateServer.js 的 resolveServer() 中移除。
//  如果以后要恢复，把下面这段接回来即可（保留在版本历史里）。

// 必须至少有原生 exe —— 没有它 vsix 根本起不来。
if (!bundledNative) {
    console.error('[bundle-server] 未找到 fpgalsp-native.exe —— 本扩展只支持原生后端，' +
        '打包会产出一个无法启动的 vsix。请先构建 build/bin/fpgalsp-native.exe' +
        '或用 FPGA_LSP_NATIVE_EXE 指定。');
    process.exit(1);
}

// ---- 3) 许可文件：随包分发二进制时必须带上 --------------------------------
//  Apache-2.0 §4(a) 要求随二进制提供许可副本，§4(b) 要求对修改过的文件（Verible）
//  作出显著声明。VSIX 里静态链接了 Verible/abseil/re2/json/spdlog/simdutf/ctre，
//  所以 LICENSE（AGPL-3.0）与 THIRD-PARTY-NOTICES.md 必须一起进包。
const legalFiles = ['LICENSE', 'THIRD-PARTY-NOTICES.md'];
for (const name of legalFiles) {
    const fromRepoRoot = path.join(extRoot, '..', name);
    const fromExtRoot = path.join(extRoot, name);
    const src = fs.existsSync(fromRepoRoot) ? fromRepoRoot : (fs.existsSync(fromExtRoot) ? fromExtRoot : '');
    if (!src) {
        console.warn(`[bundle-server] 缺少 ${name}（发布包将不含许可文本，不合规）`);
        continue;
    }
    fs.copyFileSync(src, path.join(extRoot, name));
    console.log(`[bundle-server] legal: ${path.relative(extRoot, src) || src} -> ${name}`);
}

const files = countFiles(dstRoot);
console.log(`[bundle-server] server/: ${files} files, ` +
    `${(sizeOf(dstRoot) / 1024 / 1024).toFixed(1)} MiB ` +
    `(native=${bundledNative ? 'yes' : 'no'}, python=${bundledPython ? 'yes' : 'no'})`);