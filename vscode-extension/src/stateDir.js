// =============================================================================
//  src/stateDir.js —— 运行状态目录（原来的 <workspace>/.fpgalsp）
// =============================================================================
//  用户要求：插件不要把 `.fpgalsp` 建在工作目录里，改放系统临时目录。
//
//  为什么放在这里统一解析，而不是各处 path.join(cwd, '.fpgalsp', ...)：
//    * 日志、高亮报告、格式化校验模板、MCP 日志都要落同一个地方；
//    * 服务端（C++）也要用同一个目录 —— 通过环境变量 `FPGALSP_STATE_DIR`
//      传下去（见 audit/2026-11-19/fix/WAVE3-PLAN.md §1）。**单一事实源**。
//
//  目录形状：<os tmpdir>/fpgalsp/<工程目录名>-<hash8>/
//    hash8 = 工程根绝对路径（正斜杠、小写）的 FNV-1a 32 位，十六进制 8 位。
//    这样：同一工程多次启动落到同一目录；不同工程互不覆盖；名字可读。
//
//  环境变量 `FPGALSP_STATE_DIR` 优先级最高（方便用户/CI 指定，也方便验收脚本）。
// =============================================================================
const os = require('os');
const path = require('path');

const STATE_ENV = 'FPGALSP_STATE_DIR';

/** FNV-1a 32 位（JS 侧；C++ 侧不用算，只读环境变量）。 */
function fnv1a32(text) {
  let hash = 0x811c9dc5;
  const bytes = Buffer.from(String(text), 'utf8');
  for (const byte of bytes) {
    hash ^= byte;
    hash = Math.imul(hash, 0x01000193) >>> 0;
  }
  return hash >>> 0;
}

function hash8(absolutePath) {
  return fnv1a32(String(absolutePath).replace(/\\/g, '/').toLowerCase())
    .toString(16)
    .padStart(8, '0');
}

/**
 * 解析状态目录（绝对路径）。
 * @param {string} projectRoot 工程根目录（绝对路径；空则用 cwd）
 */
function resolveStateDir(projectRoot) {
  const override = process.env[STATE_ENV];
  if (override && String(override).trim() !== '') return path.normalize(String(override).trim());

  const root = path.resolve(projectRoot || process.cwd());
  const baseName = path.basename(root) || 'workspace';
  const safe = baseName.replace(/[^A-Za-z0-9._-]/g, '_');
  return path.join(os.tmpdir(), 'fpgalsp', `${safe}-${hash8(root)}`);
}

/** <stateDir>/<...parts> */
function statePath(projectRoot, ...parts) {
  return path.join(resolveStateDir(projectRoot), ...parts);
}

/** 启动服务端时用的环境变量片段（C++ 侧读它决定 logs/run/cache）。 */
function stateEnv(projectRoot) {
  return { [STATE_ENV]: resolveStateDir(projectRoot) };
}

module.exports = { STATE_ENV, fnv1a32, hash8, resolveStateDir, statePath, stateEnv };