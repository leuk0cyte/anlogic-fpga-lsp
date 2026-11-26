// Locate the Anlogic FPGA language server for the extension.
//
// **Two server flavours are supported**（原生 exe 优先，Python 作为过渡回退）:
//
//   1. 原生 `fpgalsp-native.exe`（C++ 重写版，推荐）
//      a. 显式 `fpgaLsp.serverPath`（指向 exe 本身，或包含 exe 的目录）
//      b. 显式 `fpgaLsp.serverRoot` 下的 `fpgalsp-native.exe` / `bin/fpgalsp-native.exe`
//      c. dev checkout：`<父目录>/build/bin/fpgalsp-native.exe`、`<父目录>/build-hdl/bin/...`
//      d. .vsix 内置：`<extension>/server/bin/fpgalsp-native.exe`
//   2. Python `python -m fpgalsp`（旧路径，过渡期回退）
//      a. 显式 `fpgaLsp.serverRoot`      -> <setting>/fpgalsp/__main__.py
//      b. dev checkout                   -> <root>/fpgalsp/__main__.py
//      c. .vsix 内置                     -> <extension>/server/fpgalsp/__main__.py
//
// 优先原生：它是 C++ 重写目标，启动快、无 Python 依赖。只有找不到 exe 时才回退 Python，
// 这样旧用户升级扩展后不会因为"还没构建 exe"而突然用不了。
//
// 本模块**故意不依赖 `vscode`**，可以用纯 node 单测（见 test/）。

'use strict';

const fs = require('fs');
const path = require('path');

const NATIVE_EXE = process.platform === 'win32' ? 'fpgalsp-native.exe' : 'fpgalsp-native';

/** 目录里是否有原生 exe（直接或 bin/ 下）。返回 exe 绝对路径或 ''。 */
function nativeExeIn(dir) {
    if (!dir) return '';
    const candidates = [
        path.join(dir, NATIVE_EXE),
        path.join(dir, 'bin', NATIVE_EXE),
    ];
    for (const candidate of candidates) {
        try {
            if (fs.existsSync(candidate) && fs.statSync(candidate).isFile()) return candidate;
        } catch (_) {
            /* ignore */
        }
    }
    return '';
}

/** `p` 本身就是 exe 时返回它，否则返回 ''。 */
function asNativeExe(p) {
    if (!p) return '';
    try {
        return fs.statSync(p).isFile() ? p : '';
    } catch (_) {
        return '';
    }
}

/** True when `dir` looks like a directory that contains the `fpgalsp` Python package. */
function looksLikeServerRoot(dir) {
    if (!dir) return false;
    try {
        return fs.existsSync(path.join(dir, 'fpgalsp', '__main__.py'));
    } catch (_) {
        return false;
    }
}

/**
 * dev 模式下值得扫的仓库根：扩展所在仓库本身，以及两个仓库并列时的**兄弟** C++ 仓库。
 * （本项目实际开发布局：Y:\fpga-lsp 与 Y:\fpga-lsp-cpp 并列。）
 */
function devRoots(parent) {
    const roots = [parent];
    const sibling = path.join(parent, '..', 'fpga-lsp-cpp');
    try {
        if (fs.existsSync(sibling)) roots.push(path.resolve(sibling));
    } catch (_) {
        /* ignore */
    }
    return roots;
}

/**
 * 找原生 exe。
 * @param {{extensionPath?: string, configuredPath?: string, configuredRoot?: string,
 *          parentOfExtension?: string, devBinaryDirs?: string[]}} options
 * @returns {{exe: string, source: 'setting-path'|'setting-root'|'dev'|'bundled'|'missing'}}
 */
function resolveServerBinary(options) {
    const opts = options || {};
    const extensionPath = opts.extensionPath || '';
    const configuredPath = (opts.configuredPath || '').trim();
    const configuredRoot = (opts.configuredRoot || '').trim();
    const parent = opts.parentOfExtension || (extensionPath ? path.dirname(extensionPath) : '');

    // 1a. serverPath 直接指 exe
    if (configuredPath) {
        const direct = asNativeExe(configuredPath);
        if (direct) return { exe: direct, source: 'setting-path' };
        // 也允许指一个包含 exe 的目录
        const inDir = nativeExeIn(configuredPath);
        if (inDir) return { exe: inDir, source: 'setting-path' };
    }
    // 1b. serverRoot 下找 exe
    if (configuredRoot) {
        const inRoot = nativeExeIn(configuredRoot);
        if (inRoot) return { exe: inRoot, source: 'setting-root' };
    }
    // 1c. dev checkout：本仓库的构建目录
    //     扩展可能放在**两个**仓库里：
    //       * C++ 仓库内（<repo>/vscode-extension）  -> parent = <repo>
    //       * Python 仓库内（%PYREF%\vscode-extension）-> parent = Y:\fpga-lsp，
    //         此时 C++ 仓库是**兄弟目录** Y:\fpga-lsp-cpp（本项目的实际开发布局）
    const devDirs = opts.devBinaryDirs || (parent ? devRoots(parent).flatMap((root) =>
        ['build', 'build-hdl', 'build-flow', 'build-mcp', 'build-fmt']
            .map((name) => path.join(root, name, 'bin'))) : []);
    for (const dir of devDirs) {
        const exe = nativeExeIn(dir);
        if (exe) return { exe, source: 'dev' };
    }
    // 1d. .vsix 内置
    if (extensionPath) {
        const bundled = nativeExeIn(path.join(extensionPath, 'server'));
        if (bundled) return { exe: bundled, source: 'bundled' };
    }
    return { exe: '', source: 'missing' };
}

/**
 * 找 Python 服务器根目录（旧路径）。
 * @param {{extensionPath?: string, configured?: string, parentOfExtension?: string}} options
 * @returns {{root: string, source: 'setting'|'bundled'|'sibling'|'setting-invalid'|'missing'}}
 */
function resolveServerRoot(options) {
    const opts = options || {};
    const extensionPath = opts.extensionPath || '';
    const configured = (opts.configured || '').trim();

    if (configured && looksLikeServerRoot(configured)) {
        return { root: configured, source: 'setting' };
    }
    const parent = opts.parentOfExtension || (extensionPath ? path.dirname(extensionPath) : '');
    if (looksLikeServerRoot(parent)) {
        return { root: parent, source: 'sibling' };
    }
    const bundled = extensionPath ? path.join(extensionPath, 'server') : '';
    if (looksLikeServerRoot(bundled)) {
        return { root: bundled, source: 'bundled' };
    }
    if (configured) {
        return { root: configured, source: 'setting-invalid' };
    }
    return { root: '', source: 'missing' };
}

/**
 * 统一入口：先找原生 exe，找不到再回退 Python。
 * @returns {{kind:'native', exe:string, source:string}
 *          |{kind:'python', root:string, source:string}
 *          |{kind:'missing', source:string}}
 */
function resolveServer(options) {
    const binary = resolveServerBinary(options);
    if (binary.exe) return { kind: 'native', exe: binary.exe, source: binary.source };
    // 本扩展自 0.11.0 起**只对接原生 exe**，不再支持/不打包 Python 后端。
    return { kind: 'missing', source: binary.source };
}

module.exports = {
    NATIVE_EXE,
    asNativeExe,
    looksLikeServerRoot,
    nativeExeIn,
    resolveServer,
    resolveServerBinary,
    resolveServerRoot,
};