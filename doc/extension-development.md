# VS Code 扩展：开发、测试与打包

面向维护者。用户文档见 [vscode-extension/README.md](../vscode-extension/README.md)。

## 目录与定位逻辑

扩展只对接**原生可执行文件** `fpgalsp-native.exe`（Verible 已内嵌其中），不再有 Python 后端。
服务端定位在 `vscode-extension/src/locateServer.js`（相对本仓库根目录），依次尝试：

1. 设置项 `fpgaLsp.serverPath`（显式路径）
2. 打包内置的 `server/bin/fpgalsp-native.exe`
3. 同仓库的 `build*/bin/fpgalsp-native.exe`（开发态）

## 常用命令

```powershell
cd Y:\fpga-lsp-cpp\vscode-extension
npm install                # 首次
npm test                   # 单元测试
npm run test:locate        # 服务端定位逻辑
npm run test:integration   # 与真实服务端的集成
npm run test:vscode        # 在 VS Code 里跑（脚本见 scripts/run-vscode-test.ps1）
npm run bundle:server      # 从 ../build/bin 取 exe 放进 server/bin
npm run package            # bundle:server + vsce package -> fpga-lsp-anlogic-<version>.vsix
code --extensionDevelopmentPath="Y:\fpga-lsp-cpp\vscode-extension" %WORKSPACE%\CorpusAlpha
```

## 打包注意事项

- **必须先构建出 exe**（`build/bin/fpgalsp-native.exe`），否则 `bundle-server.js` 会报错
  并提示先构建；它不会打出一个"看起来正常但无法启动"的包。
- **绝不能把 `node_modules/` 排除进 `.vscodeignore`**。本扩展没有 webpack/esbuild 打包，
  运行期依赖（`vscode-languageclient`）必须以 `node_modules` 原样随包发布。历史教训：
  0.10.32 排除过一次，结果 `activate()` 在 `require` 阶段就抛异常——扩展"看起来装上了"
  但完全不工作，且因为激活失败发生在写第一行日志之前，所有自我诊断都没跑起来。
- 打包命令本身在 `package.json` 的 `scripts.package` 里；`vsce` 需要可用
  （`npx --no-install @vscode/vsce --version` 能解析到即可离线使用）。
- 打包后建议抽查：包内 `extension/server/bin/fpgalsp-native.exe` 的字节数与
  `build/bin` 一致。

## 版本号

`package.json` 的 `version` 与 `package-lock.json` 根节点的 `version` **必须同步**
（历史上漂移过一次：package.json 0.13.0 / lock 0.10.29）。改版本时两处一起改。