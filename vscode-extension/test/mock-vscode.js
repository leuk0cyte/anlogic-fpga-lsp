// Minimal in-memory implementation of the VS Code extension API.
//
// It is intentionally *not* a full VS Code emulator: it only implements the surface used by
// src/moduleTree.js, src/flowTree.js, src/reportsPanel.js, src/flowController.js and
// extension.js (when someone wants to exercise the wiring). test/run.js installs this module
// under `require('vscode')` through Module._load.

'use strict';

const state = {
    commands: new Map(),
    builtinCommands: new Map(),
    treeViews: [],
    webviewPanels: [],
    webviewViewProviders: {},
    outputChannels: [],
    statusBarItems: [],
    messages: { info: [], warning: [], error: [] },
    config: {},
    clipboard: [],
    progressOptions: [],
    progressReports: [],
    executedCommands: [],
    webviewMessages: [],
    openedResources: [],
    shownDocuments: [],
    untitled: 0,
    lastCancellationTokenSource: undefined,
};

class Disposable {
    constructor(callOnDispose) {
        this._callOnDispose = callOnDispose;
    }

    dispose() {
        if (this._callOnDispose) {
            const fn = this._callOnDispose;
            this._callOnDispose = undefined;
            fn();
        }
    }

    static from(...items) {
        return new Disposable(() => items.forEach((item) => item && typeof item.dispose === 'function' && item.dispose()));
    }
}

class EventEmitter {
    constructor(options) {
        this._options = options || {};
        this._listeners = new Set();
        const event = (listener, thisArgs, disposables) => {
            const bound = typeof thisArgs === 'undefined' ? listener : listener.bind(thisArgs);
            this._listeners.add(bound);
            const disposable = new Disposable(() => this._listeners.delete(bound));
            if (Array.isArray(disposables)) disposables.push(disposable);
            return disposable;
        };
        this.event = event;
    }

    fire(data) {
        for (const listener of Array.from(this._listeners)) {
            listener(data);
        }
    }

    dispose() {
        this._listeners.clear();
    }
}

class CancellationTokenSource {
    constructor() {
        const emitter = new EventEmitter();
        this._emitter = emitter;
        this.token = {
            isCancellationRequested: false,
            onCancellationRequested: (callback) => emitter.event(() => {
                this.token.isCancellationRequested = true;
                callback();
            }),
        };
    }

    cancel() {
        // Faithful to the real API: the flag flips on cancel even when nobody subscribed.
        this.token.isCancellationRequested = true;
        this._emitter.fire(undefined);
    }

    dispose() {
        this._emitter.dispose();
    }
}

class Position {
    constructor(line, character) {
        this.line = line || 0;
        this.character = character || 0;
    }

    translate(lineDelta, characterDelta) {
        return new Position(this.line + (lineDelta || 0), this.character + (characterDelta || 0));
    }

    isEqual(other) {
        return !!other && other.line === this.line && other.character === this.character;
    }
}

class Range {
    constructor(startLineOrStart, startCharacterOrEnd, endLine, endCharacter) {
        if (typeof startLineOrStart === 'number') {
            this.start = new Position(startLineOrStart, startCharacterOrEnd || 0);
            this.end = new Position(endLine === undefined ? startLineOrStart : endLine, endCharacter || 0);
        } else {
            this.start = startLineOrStart || new Position(0, 0);
            this.end = startCharacterOrEnd || this.start;
        }
    }

    get isEmpty() {
        return this.start.line === this.end.line && this.start.character === this.end.character;
    }
}

class Uri {
    constructor(scheme, authority, path, query, fragment) {
        this.scheme = scheme;
        this.authority = authority || '';
        this.path = path || '';
        this.query = query || '';
        this.fragment = fragment || '';
        this.fsPath = this.path;
    }

    static file(fsPath) {
        const normalized = String(fsPath).replace(/\\/g, '/');
        return new Uri('file', '', normalized, '', '');
    }

    static parse(value) {
        const text = String(value);
        const match = /^([A-Za-z][A-Za-z0-9+.-]*):(?:\/\/([^/?#]*))?([^?#]*)/.exec(text);
        if (!match) return Uri.file(text);
        let decoded = match[3] || '';
        try {
            decoded = decodeURIComponent(decoded);
        } catch (_) {
            // keep the raw path
        }
        const uri = new Uri(match[1], match[2] || '', decoded, '', '');
        if (/^\/[A-Za-z]:/.test(decoded)) uri.fsPath = decoded.slice(1);
        return uri;
    }

    with(change) {
        return new Uri(
            change && change.scheme !== undefined ? change.scheme : this.scheme,
            this.authority,
            change && change.path !== undefined ? change.path : this.path,
            this.query,
            this.fragment
        );
    }

    toString() {
        if (this.scheme === 'file') return `file://${this.path}`;
        return `${this.scheme}:${this.path}`;
    }

    toJSON() {
        return { scheme: this.scheme, authority: this.authority, path: this.path, fsPath: this.fsPath };
    }
}

class Location {
    constructor(uri, range) {
        this.uri = uri;
        this.range = range || new Range(0, 0, 0, 0);
    }
}

class ThemeIcon {
    constructor(id, color) {
        this.id = id;
        this.color = color;
    }
}

class MarkdownString {
    constructor(value) {
        this.value = value || '';
        this.isTrusted = false;
        this.supportThemeIcons = false;
    }

    appendText(value) {
        this.value += value;
        return this;
    }

    appendMarkdown(value) {
        this.value += value;
        return this;
    }

    appendCodeblock(value, language) {
        this.value += `\n\`\`\`${language || ''}\n${value}\n\`\`\`\n`;
        return this;
    }
}

class TreeItem {
    constructor(label, collapsibleState) {
        this.label = label;
        this.collapsibleState = collapsibleState === undefined
            ? 0 // TreeItemCollapsibleState.None
            : collapsibleState;
        this.id = undefined;
        this.description = undefined;
        this.tooltip = undefined;
        this.iconPath = undefined;
        this.contextValue = undefined;
        this.command = undefined;
        this.resourceUri = undefined;
    }
}

const TreeItemCollapsibleState = Object.freeze({ None: 0, Collapsed: 1, Expanded: 2 });
const StatusBarAlignment = Object.freeze({ Left: 1, Right: 2 });
const ProgressLocation = Object.freeze({ SourceControl: 1, Window: 10, Notification: 15 });
const ViewColumn = Object.freeze({ Active: -1, Beside: -2, One: 1, Two: 2, Three: 3 });
const ConfigurationTarget = Object.freeze({ Global: 1, Workspace: 2, WorkspaceFolder: 3 });
const TextEditorRevealType = Object.freeze({ Default: 0, InCenter: 1, InCenterIfOutsideViewport: 2, AtTop: 3 });
const ExtensionMode = Object.freeze({ Production: 1, Development: 2, Test: 3 });
class Diagnostic {
    constructor(range, message, severity) {
        this.range = range;
        this.message = message;
        this.severity = severity;
    }
}

const DiagnosticSeverity = Object.freeze({ Error: 0, Warning: 1, Information: 2, Hint: 3 });

function reset() {
    state.commands.clear();
    state.builtinCommands.clear();
    state.treeViews.length = 0;
    state.webviewPanels.length = 0;
    state.webviewViewProviders = {};
    state.outputChannels.length = 0;
    state.statusBarItems.length = 0;
    state.messages.info.length = 0;
    state.messages.warning.length = 0;
    state.messages.error.length = 0;
    state.config = {};
    state.clipboard.length = 0;
    state.progressOptions.length = 0;
    state.progressReports.length = 0;
    state.executedCommands.length = 0;
    state.webviewMessages.length = 0;
    state.openedResources.length = 0;
    state.shownDocuments.length = 0;
    state.untitled = 0;
    state.lastCancellationTokenSource = undefined;
    installBuiltins();
}

function installBuiltins() {
    state.builtinCommands.set('vscode.open', (uri) => {
        state.openedResources.push(uri);
        return Promise.resolve();
    });
    state.builtinCommands.set('clipboard.writeText', (text) => {
        state.clipboard.push(text);
        return Promise.resolve();
    });
}

const window = {
    activeTextEditor: undefined,
    visibleTextEditors: [],
    onDidChangeActiveTextEditor: new EventEmitter().event,
    onDidChangeTextEditorSelection: new EventEmitter().event,
    onDidChangeTextEditorVisibleRanges: new EventEmitter().event,

    createTreeView(viewId, options) {
        const visibility = new EventEmitter();
        const view = {
            viewId,
            options,
            treeDataProvider: options && options.treeDataProvider,
            message: undefined,
            title: undefined,
            description: undefined,
            visible: true,
            onDidChangeVisibility: visibility.event,
            onDidExpandElement: new EventEmitter().event,
            onDidCollapseElement: new EventEmitter().event,
            reveal: async () => undefined,
            dispose() {
                view.visible = false;
            },
        };
        state.treeViews.push(view);
        return view;
    },

    withProgress(options, task) {
        state.progressOptions.push(options);
        const source = new CancellationTokenSource();
        state.lastCancellationTokenSource = source;
        const progress = {
            report(data) {
                state.progressReports.push(data);
            },
        };
        return Promise.resolve(task(progress, source.token));
    },

    createOutputChannel(name) {
        const channel = {
            name,
            lines: [],
            shown: false,
            append(value) {
                this.lines.push(String(value));
            },
            appendLine(value) {
                this.lines.push(String(value));
            },
            show() {
                this.shown = true;
            },
            hide() {
                this.shown = false;
            },
            clear() {
                this.lines.length = 0;
            },
            replace(value) {
                this.lines.length = 0;
                this.lines.push(String(value));
            },
            dispose() {
                this.disposed = true;
            },
        };
        state.outputChannels.push(channel);
        return channel;
    },

    createStatusBarItem(alignment, priority) {
        const item = {
            alignment,
            priority: priority || 0,
            text: '',
            tooltip: '',
            command: undefined,
            name: undefined,
            visible: false,
            show() {
                this.visible = true;
            },
            hide() {
                this.visible = false;
            },
            dispose() {
                this.disposed = true;
            },
        };
        state.statusBarItems.push(item);
        return item;
    },

    // 0.17.2：底栏 webview view（ChipWatcher 探针面板）
    //   provider 会被记下来，测试可以调 provider.resolveWebviewView(fakeView) 驱动渲染。
    registerWebviewViewProvider(viewType, provider, options) {
        state.webviewViewProviders[viewType] = { provider, options };
        return {
            dispose() { delete state.webviewViewProviders[viewType]; },
        };
    },

    createWebviewPanel(viewType, title, showOptions, options) {
        const disposeEmitter = new EventEmitter();
        const messageEmitter = new EventEmitter();
        const webview = {
            html: '',
            options,
            cspSource: 'vscode-webview://mock-csp-source',
            onDidReceiveMessage: messageEmitter.event,
            postMessage(message) {
                state.webviewMessages.push(message);
                return Promise.resolve(true);
            },
            asWebviewUri(uri) {
                return uri;
            },
            __receive(message) {
                messageEmitter.fire(message);
            },
        };
        const panel = {
            viewType,
            title,
            showOptions,
            options,
            webview,
            visible: true,
            disposed: false,
            onDidDispose: disposeEmitter.event,
            reveal() {
                panel.visible = true;
            },
            dispose() {
                if (panel.disposed) return;
                panel.disposed = true;
                panel.visible = false;
                disposeEmitter.fire(undefined);
            },
        };
        state.webviewPanels.push(panel);
        return panel;
    },

    showInformationMessage(message, ...items) {
        state.messages.info.push({ message, items });
        return Promise.resolve(undefined);
    },

    showWarningMessage(message, ...items) {
        state.messages.warning.push({ message, items });
        return Promise.resolve(undefined);
    },

    showErrorMessage(message, ...items) {
        state.messages.error.push({ message, items });
        return Promise.resolve(undefined);
    },

    showQuickPick: async () => undefined,
    showInputBox: async () => undefined,
    showOpenDialog: async () => undefined,
    showSaveDialog: async () => undefined,
    showTextDocument: async (document, options) => {
        const editor = {
            document,
            options: options || {},
            selection: options && options.selection,
            revealRange(range, type) {
                this.revealed = { range, type };
            },
            edit: async () => true,
        };
        state.shownDocuments.push(editor);
        window.activeTextEditor = editor;
        return editor;
    },
};

const workspace = {
    textDocuments: [],
    workspaceFolders: [{
        uri: Uri.file('%WORKSPACE%/CorpusAlpha'),
        name: 'CorpusAlpha',
        index: 0,
    }],

    getConfiguration(section) {
        const prefix = section ? `${section}.` : '';
        return {
            get(key, defaultValue) {
                const full = prefix + key;
                return Object.prototype.hasOwnProperty.call(state.config, full) ? state.config[full] : defaultValue;
            },
            has(key) {
                return Object.prototype.hasOwnProperty.call(state.config, prefix + key);
            },
            inspect(key) {
                const full = prefix + key;
                return {
                    key: full,
                    defaultValue: undefined,
                    globalValue: state.config[full],
                };
            },
            update(key, value) {
                state.config[prefix + key] = value;
                return Promise.resolve();
            },
        };
    },

    onDidSaveTextDocument: new EventEmitter().event,
    onDidOpenTextDocument: new EventEmitter().event,
    onDidChangeConfiguration: new EventEmitter().event,
    onDidChangeWorkspaceFolders: new EventEmitter().event,

    createFileSystemWatcher(globPattern) {
        return {
            globPattern,
            onDidChange: new EventEmitter().event,
            onDidCreate: new EventEmitter().event,
            onDidDelete: new EventEmitter().event,
            dispose() { /* noop */ },
        };
    },

    openTextDocument: async (arg) => {
        if (typeof arg === 'string') {
            return { uri: Uri.file(arg), languageId: 'plaintext', getText: () => '' };
        }
        if (arg && arg.scheme && typeof arg.fsPath === 'string') {
            return { uri: arg, languageId: 'plaintext', getText: () => '' };
        }
        if (arg && arg.uri) {
            return { uri: arg.uri, languageId: 'plaintext', getText: () => '' };
        }
        state.untitled += 1;
        const content = arg && arg.content ? String(arg.content) : '';
        return {
            uri: Uri.parse(`untitled:Untitled-${state.untitled}`),
            languageId: (arg && arg.language) || 'plaintext',
            content,
            getText: () => content,
        };
    },

    applyEdit: async () => true,
    saveAll: async () => true,
};

const commands = {
    registerCommand(id, callback) {
        state.commands.set(id, callback);
        return new Disposable(() => {
            if (state.commands.get(id) === callback) state.commands.delete(id);
        });
    },
    async executeCommand(id, ...args) {
        state.executedCommands.push({ id, args });
        if (state.builtinCommands.has(id)) return state.builtinCommands.get(id)(...args);
        const callback = state.commands.get(id);
        if (!callback) throw new Error(`mock-vscode: command not found: ${id}`);
        return callback(...args);
    },
    getCommands() {
        return Promise.resolve(Array.from(state.commands.keys()));
    },
};

const env = {
    clipboard: {
        writeText(text) {
            state.clipboard.push(String(text));
            return Promise.resolve();
        },
        readText() {
            return Promise.resolve(state.clipboard.join(''));
        },
    },
    openExternal: async () => true,
    appName: 'Visual Studio Code Mock',
};

const languages = {
    createDiagnosticCollection: () => ({
        set() {},
        delete() {},
        clear() {},
        dispose() {},
    }),
    getLanguages: async () => [],
    setTextDocumentLanguage: async (document, languageId) => {
        if (document) document.languageId = languageId;
        return document;
    },
};

installBuiltins();

module.exports = {
    version: '1.80.0-mock',
    Disposable,
    EventEmitter,
    CancellationTokenSource,
    Position,
    Range,
    Uri,
    Location,
    ThemeIcon,
    MarkdownString,
    TreeItem,
    TreeItemCollapsibleState,
    StatusBarAlignment,
    ProgressLocation,
    ViewColumn,
    ConfigurationTarget,
    TextEditorRevealType,
    ExtensionMode,
    Diagnostic,
    DiagnosticSeverity,
    window,
    workspace,
    commands,
    env,
    languages,
    // test helpers
    __state: state,
    __reset: reset,
    __setConfiguration(values) {
        Object.assign(state.config, values || {});
    },
};
