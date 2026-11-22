// =============================================================================
//  src/writeService.js —— 插件侧统一写服务（与后端 platform::write_service 对称）
// =============================================================================
//  为什么需要（用户反馈 2026-11-22）：
//    "操作散落各代码，以后修改容易改漏，统一方法就不会漏"。
//    插件原来在 extension.js / launchCheck.js 里裸 `fs.writeFileSync`：路径靠调用点自己拼、
//    没有原子性、没有路径策略。**以后所有文件写入都必须经过本文件**。
//
//  与后端 platform::write_service（见 src/platform/write_service.h）同名的语义：
//    * root 策略：只允许 state root 下的相对路径，或显式白名单里的工程文件；
//    * 路径拒绝：绝对路径、`..`、NUL、以及任何**含 `.fpgalsp` 的路径段**；
//      （工程目录里的 `.fpgalsp` 已废弃，必须在策略层拒绝，而不是靠 code review 记住）
//    * 原子写：同目录 `.tmp-<pid>-<seq>` → rename，失败重试 3 次，最后清理 tmp；
//    * dryRun：只校验不落盘；caller：只用于错误信息/调试；
//    * never-throw：全部返回 `{ ok, path, error }`，异常在内部转成 ok=false。
//
//  门禁：test/run.js 的 "write gate" 扫描 `src/*.js` 与 `extension.js`，
//  `fs.writeFileSync|fs.appendFileSync|createWriteStream` 只允许出现在本文件里。
// =============================================================================

'use strict';

const fs = require('fs');
const path = require('path');
const { resolveStateDir } = require('./stateDir');

const MAX_WRITE_ATTEMPTS = 3;      // 与后端一致：write+rename 失败重试 3 次
const TEMP_PREFIX = '.tmp-';       // 同目录临时文件：.tmp-<pid>-<seq>
const STATE_SEGMENT = '.fpgalsp';  // 任何含这个片段的路径段一律拒绝

// 工程目录下允许插件改写的用户文件（显式白名单，新增必须在这里登记）：
//   * `.vscode/mcp.json` —— "FPGA: Install MCP Configuration" (VS Code workspace)
//   * `.mcp.json`        —— 同一命令的 Portable 选项（Cline/Claude）
//   * `*.al`             —— Anlogic 工程文件
const PROJECT_EXACT = new Set(['.vscode/mcp.json', '.mcp.json']);
const PROJECT_EXTENSIONS = ['.al'];

let sequence = 0;

function failure(error, extra) {
    return Object.assign({ ok: false, path: '', error: String(error) }, extra || {});
}

function success(absolutePath, extra) {
    return Object.assign({ ok: true, path: absolutePath, error: '' }, extra || {});
}

function messageOf(error) {
    if (!error) return 'unknown error';
    return error.stack || error.message || String(error);
}

function callerTag(options) {
    const caller = options && options.caller;
    return caller ? `[${caller}] ` : '';
}

// ---------------------------------------------------------------- path policy
//
// 把调用点给的相对路径切成路径段，并在这里一次性拒绝所有越界写法。返回 `parts` 后
// 目标路径只由 root + parts 拼出来，调用点再也无法通过 `.fpgalsp/..` 之类绕过策略。
function splitRelative(rel) {
    if (typeof rel !== 'string' || rel.trim() === '') {
        return { ok: false, error: 'path is empty' };
    }
    if (rel.includes('\0')) {
        return { ok: false, error: `path contains NUL: ${JSON.stringify(rel)}` };
    }
    const text = rel.replace(/\\/g, '/');
    // Absolute: POSIX (/x), Windows drive (C:/x, C:x) and UNC (//server/share).
    if (text.startsWith('/') || /^[A-Za-z]:/.test(text)) {
        return { ok: false, error: `absolute paths are not allowed: ${JSON.stringify(rel)}` };
    }
    const parts = text.split('/').filter((part) => part !== '' && part !== '.');
    if (!parts.length) {
        return { ok: false, error: `path has no file component: ${JSON.stringify(rel)}` };
    }
    for (const part of parts) {
        if (part === '..') {
            return { ok: false, error: `path traversal ("..") is not allowed: ${JSON.stringify(rel)}` };
        }
        if (part.toLowerCase().includes(STATE_SEGMENT)) {
            return { ok: false, error: `paths containing "${STATE_SEGMENT}" are not allowed: ${JSON.stringify(rel)}` };
        }
    }
    return { ok: true, parts };
}

// root + parts，并在解析后再复核一次目标确实落在 root 内（词法层面的纵深防御）。
function resolveInside(root, parts) {
    const absoluteRoot = path.resolve(root);
    const target = path.resolve(absoluteRoot, ...parts);
    const relative = path.relative(absoluteRoot, target);
    if (relative === '' || relative.startsWith('..') || path.isAbsolute(relative)) {
        return failure(`target escapes ${absoluteRoot}: ${JSON.stringify(parts.join('/'))}`);
    }
    return success(target);
}

/** `%TEMP%\fpgalsp\<工程名>-<hash8>`（与后端 platform::state_dir() 逐字节一致）。 */
function stateRootFor(workspaceRoot) {
    return path.resolve(resolveStateDir(workspaceRoot));
}

// ---------------------------------------------------------------- atomic write
//
// 只在本文件里允许出现的裸写原语。先写同目录临时文件，再 rename 覆盖目标：读者永远
// 看到旧的完整文件或新的完整文件，不会看到写了一半的内容。rename 失败（Windows 上
// 常见于杀软/编辑器占用）重试 3 次，最后清理临时文件并把原因返回给调用点。
function sleepSync(ms) {
    try {
        const buffer = new Int32Array(new SharedArrayBuffer(4));
        Atomics.wait(buffer, 0, 0, ms);
    } catch (_) {
        const until = Date.now() + ms;
        while (Date.now() < until) { /* tiny synchronous backoff */ }
    }
}

function atomicWrite(absolutePath, text, options) {
    const opts = options || {};
    const tag = callerTag(opts);
    const payload = Buffer.isBuffer(text) ? text : String(text === undefined || text === null ? '' : text);
    if (opts.dryRun) return success(absolutePath, { dryRun: true });

    const directory = path.dirname(absolutePath);
    try {
        fs.mkdirSync(directory, { recursive: true });
    } catch (error) {
        return failure(`${tag}cannot create ${directory}: ${messageOf(error)}`);
    }

    let lastError;
    for (let attempt = 1; attempt <= MAX_WRITE_ATTEMPTS; attempt += 1) {
        sequence += 1;
        const temporary = path.join(directory, `${TEMP_PREFIX}${process.pid}-${sequence}`);
        try {
            fs.writeFileSync(temporary, payload, 'utf8');
            fs.renameSync(temporary, absolutePath);
            return success(absolutePath);
        } catch (error) {
            lastError = error;
            try { fs.rmSync(temporary, { force: true }); } catch (_) { /* tmp cleanup is best effort */ }
            if (attempt < MAX_WRITE_ATTEMPTS) sleepSync(10 * attempt);
        }
    }
    return failure(`${tag}write failed after ${MAX_WRITE_ATTEMPTS} attempts: ${messageOf(lastError)}`, {
        attempts: MAX_WRITE_ATTEMPTS,
    });
}

// ---------------------------------------------------------------- public API

/**
 * 写 state root 下的相对路径文件（日志/缓存/高亮报告等）。
 * @param {string} workspaceRoot 工程根（决定 state root；空则用 cwd）
 * @param {string} rel 相对 state root 的路径
 * @param {string|Buffer} text 内容
 * @param {{dryRun?: boolean, caller?: string}} [opts]
 * @returns {{ok: boolean, path: string, error: string, dryRun?: boolean}}
 */
function writeStateFile(workspaceRoot, rel, text, opts) {
    try {
        const split = splitRelative(rel);
        if (!split.ok) return failure(split.error);
        const target = resolveInside(stateRootFor(workspaceRoot), split.parts);
        if (!target.ok) return target;
        return atomicWrite(target.path, text, opts);
    } catch (error) {
        return failure(messageOf(error));
    }
}

function projectWhitelisted(parts) {
    const joined = parts.join('/').toLowerCase();
    if (PROJECT_EXACT.has(joined)) return true;
    const name = parts[parts.length - 1].toLowerCase();
    return PROJECT_EXTENSIONS.some((extension) => name.endsWith(extension));
}

/**
 * 写工程目录下的用户文件，只允许显式白名单（`.vscode/mcp.json`、`.mcp.json`、`*.al`）。
 * opts.backup 时先原子写入 `<file>.fpgalsp-bak`（仅当目标已存在）。
 * @param {string} workspaceRoot 工程根
 * @param {string} rel 相对工程根的路径
 * @param {string|Buffer} text 内容
 * @param {{dryRun?: boolean, backup?: boolean, caller?: string}} [opts]
 * @returns {{ok: boolean, path: string, error: string, backup?: string, dryRun?: boolean}}
 */
function writeProjectFile(workspaceRoot, rel, text, opts) {
    try {
        const split = splitRelative(rel);
        if (!split.ok) return failure(split.error);
        if (!projectWhitelisted(split.parts)) {
            return failure(`refusing to write a project file outside the whitelist: ${JSON.stringify(rel)} ` +
                '(allowed: .vscode/mcp.json, .mcp.json, *.al)');
        }
        const root = path.resolve(workspaceRoot || process.cwd());
        const target = resolveInside(root, split.parts);
        if (!target.ok) return target;

        const options = opts || {};
        let backupPath = '';
        if (options.backup && !options.dryRun) {
            try {
                if (fs.existsSync(target.path)) {
                    backupPath = `${target.path}.fpgalsp-bak`;
                    const backup = atomicWrite(backupPath, fs.readFileSync(target.path), {
                        caller: options.caller ? `${options.caller}.backup` : 'backup',
                    });
                    if (!backup.ok) {
                        return failure(`backup failed: ${backup.error}`, { path: target.path });
                    }
                }
            } catch (error) {
                return failure(`backup failed: ${messageOf(error)}`, { path: target.path });
            }
        }

        const result = atomicWrite(target.path, text, options);
        if (result.ok && backupPath) result.backup = backupPath;
        return result;
    } catch (error) {
        return failure(messageOf(error));
    }
}

/**
 * 追加文本到 state root 下的相对路径（低层原语，供 launchCheck 的批量 flush 使用）。
 * 失败不抛，返回 `{ok, path, error}`。
 */
function appendText(workspaceRoot, rel, text) {
    try {
        const split = splitRelative(rel);
        if (!split.ok) return failure(split.error);
        const target = resolveInside(stateRootFor(workspaceRoot), split.parts);
        if (!target.ok) return target;
        const payload = String(text === undefined || text === null ? '' : text);
        if (payload === '') return success(target.path);
        try {
            fs.mkdirSync(path.dirname(target.path), { recursive: true });
            fs.appendFileSync(target.path, payload, 'utf8');
        } catch (error) {
            return failure(messageOf(error));
        }
        return success(target.path);
    } catch (error) {
        return failure(messageOf(error));
    }
}

/**
 * 追加一行到 `<state>/logs/<name>.log`（自动补 '\n'；name 带 `.log` 后缀不会重复）。
 * 失败不抛，返回 `{ok, path, error}`。
 */
function appendLog(workspaceRoot, name, line) {
    try {
        const split = splitRelative(typeof name === 'string' ? name.trim() : name);
        if (!split.ok) return failure(`invalid log name: ${split.error}`);
        const parts = split.parts.slice();
        const base = parts[parts.length - 1];
        parts[parts.length - 1] = `${base.toLowerCase().endsWith('.log') ? base.slice(0, -4) : base}.log`;
        const rel = ['logs'].concat(parts).join('/');
        const text = String(line === undefined || line === null ? '' : line);
        return appendText(workspaceRoot, rel, text.endsWith('\n') ? text : `${text}\n`);
    } catch (error) {
        return failure(messageOf(error));
    }
}

module.exports = {
    stateRootFor,
    writeStateFile,
    writeProjectFile,
    appendLog,
    appendText,
};
