// Tokenization + color tests for the bundled TextMate grammars.
//
// The second half resolves the *actual* colors used by VS Code's "Dark Modern" theme (falls back to
// an embedded copy of its rules when VS Code is not at the usual path), so a regression in scope
// names that makes different symbol kinds share one color is caught here.
//
//   node test/grammar.test.js

'use strict';

const fs = require('fs');
const path = require('path');
const vsctm = require('vscode-textmate');
const oniguruma = require('vscode-oniguruma');

let failures = 0;
function check(name, ok, detail) {
    console.log(`  [${ok ? 'PASS' : 'FAIL'}] ${name}${detail ? '  ' + detail : ''}`);
    if (!ok) failures += 1;
}

const GRAMMARS = {
    'source.verilog': path.join(__dirname, '..', 'syntaxes', 'verilog.tmLanguage.json'),
    'source.vhdl': path.join(__dirname, '..', 'syntaxes', 'vhdl.tmLanguage.json'),
};

function loadTheme() {
    const roots = [
        'C:/Program Files/Microsoft VS Code/resources/app/extensions/theme-defaults/themes',
        'C:/Program Files/Microsoft VS Code/974500e64f/resources/app/extensions/theme-defaults/themes',
        'C:/Users/' + (process.env.USERNAME || '') + '/AppData/Local/Programs/Microsoft VS Code/resources/app/extensions/theme-defaults/themes',
    ];
    for (const root of roots) {
        const base = path.join(root, 'dark_vs.json');
        const plus = path.join(root, 'dark_plus.json');
        if (!fs.existsSync(base)) continue;
        const rules = [];
        const push = (file) => {
            const theme = JSON.parse(fs.readFileSync(file, 'utf8'));
            for (const rule of theme.tokenColors || []) {
                const scopes = Array.isArray(rule.scope) ? rule.scope : [rule.scope];
                for (const scope of scopes) {
                    if (!scope || !rule.settings || !rule.settings.foreground) continue;
                    rules.push({ scope, settings: { foreground: rule.settings.foreground } });
                }
            }
        };
        push(base);
        if (fs.existsSync(plus)) push(plus);
        rules.unshift({ settings: { foreground: '#D4D4D4' } });
        return { settings: rules, source: 'dark modern (installed)' };
    }
    return {
        source: 'embedded dark modern palette',
        settings: [
            { settings: { foreground: '#D4D4D4' } },
            { scope: 'comment', settings: { foreground: '#6A9955' } },
            { scope: 'constant.numeric', settings: { foreground: '#B5CEA8' } },
            { scope: 'constant.language', settings: { foreground: '#569CD6' } },
            { scope: 'constant.character', settings: { foreground: '#569CD6' } },
            { scope: 'entity.name.tag', settings: { foreground: '#569CD6' } },
            { scope: 'entity.other.attribute-name', settings: { foreground: '#9CDCFE' } },
            { scope: 'entity.name.label', settings: { foreground: '#C8C8C8' } },
            { scope: 'entity.name.function, support.function', settings: { foreground: '#DCDCAA' } },
            { scope: 'support.class, support.type, entity.name.type, entity.name.namespace', settings: { foreground: '#4EC9B0' } },
            { scope: 'keyword.control', settings: { foreground: '#C586C0' } },
            { scope: 'storage, storage.type, storage.modifier', settings: { foreground: '#569CD6' } },
            { scope: 'keyword.operator', settings: { foreground: '#D4D4D4' } },
            { scope: 'variable', settings: { foreground: '#9CDCFE' } },
            { scope: 'variable.other.constant', settings: { foreground: '#4FC1FF' } },
            { scope: 'string', settings: { foreground: '#CE9178' } },
            { scope: 'meta.preprocessor, entity.name.function.preprocessor', settings: { foreground: '#569CD6' } },
        ],
    };
}

function makeRegistry(theme) {
    const wasm = fs.readFileSync(require.resolve('vscode-oniguruma/release/onig.wasm')).buffer;
    const onigLib = oniguruma.loadWASM(wasm).then(() => ({
        createOnigScanner: (patterns) => new oniguruma.OnigScanner(patterns),
        createOnigString: (string) => new oniguruma.OnigString(string),
    }));
    return new vsctm.Registry({
        onigLib,
        theme,
        loadGrammar: async (scope) => (GRAMMARS[scope]
            ? vsctm.parseRawGrammar(fs.readFileSync(GRAMMARS[scope], 'utf8'), GRAMMARS[scope])
            : null),
    });
}

function scopesAt(tokens, column) {
    const token = tokens.find((item) => column >= item.startIndex && column < item.endIndex);
    return token ? token.scopes.join(' ') : '';
}

function lineColors(grammar, row, ruleStack, colorMap) {
    const binary = grammar.tokenizeLine2(row, ruleStack);
    const colors = [];
    for (let index = 0; index + 1 < binary.tokens.length; index += 2) {
        const foreground = (binary.tokens[index + 1] >>> 15) & 0x1FF;
        colors.push({ start: binary.tokens[index], color: colorMap[foreground] || '#D4D4D4' });
    }
    return colors;
}

async function main() {
    const theme = loadTheme();
    const registry = makeRegistry(theme);
    const colorMap = registry.getColorMap();

    // ------------------------------------------------------------------ Verilog
    const verilog = await registry.loadGrammar('source.verilog');
    const verilogRows = [
        'module top(input clk, output logic [7:0] q);',
        'wire [7:0] data;',
        'parameter WIDTH = 8;',
        "assign data = 8'hFF; // note",
        'pwm_gen #(.W(8)) u_pwm (.clk(clk));',
        'always @(posedge clk) begin : blk',
        '  q <= $unsigned(data) + 1;',
        '  $display("hi");',
        'end',
        'endmodule',
    ];
    let stack = vsctm.INITIAL_STATE;
    const verilogTokens = [];
    const verilogColors = [];
    for (const row of verilogRows) {
        verilogTokens.push(verilog.tokenizeLine(row, stack).tokens);
        verilogColors.push(lineColors(verilog, row, stack, colorMap));
        stack = verilog.tokenizeLine(row, stack).ruleStack;
    }
    const scope = (needle, offset = 0) => {
        const line = verilogRows.findIndex((row) => row.includes(needle));
        return line < 0 ? '' : scopesAt(verilogTokens[line], verilogRows[line].indexOf(needle) + offset);
    };
    const color = (needle, offset = 0) => {
        const line = verilogRows.findIndex((row) => row.includes(needle));
        if (line < 0) return '';
        const column = verilogRows[line].indexOf(needle) + offset;
        const hit = verilogColors[line].filter((entry) => entry.start <= column).pop();
        return hit ? String(hit.color).toLowerCase() : '';
    };

    check('verilog: module keyword', /keyword\.control\.verilog/.test(scope('module')));
    check('verilog: module name is a type', /entity\.name\.type\.verilog/.test(scope('top')));
    check('verilog: port is a tag', /entity\.name\.tag\.port\.verilog/.test(scope('clk')));
    check('verilog: signal is a variable', /variable\.other\.readwrite\.verilog/.test(scope('data')));
    check('verilog: parameter is a constant', /variable\.other\.constant\.verilog/.test(scope('WIDTH')));
    check('verilog: instance name is a function', /entity\.name\.function\.instance\.verilog/.test(scope('u_pwm')));
    check('verilog: instance module is a type', /entity\.name\.type\.instance\.verilog/.test(scope('pwm_gen')));
    check('verilog: net type keyword', /support\.type\.verilog/.test(scope('wire')));
    check('verilog: number literal', /constant\.numeric\.verilog/.test(scope("8'hFF")));
    check('verilog: comment', /comment\.line\.double-slash\.verilog/.test(scope('// note')));
    check('verilog: system task', /support\.function\.system\.verilog/.test(scope('$unsigned')));
    check('verilog: label', /entity\.name\.label\.verilog/.test(scope('blk')));
    check('verilog: string', /string\.quoted\.double\.verilog/.test(scope('"hi"')));

    const groups = {
        module: color('top'),
        port: color('clk'),
        signal: color('data'),
        parameter: color('WIDTH'),
        instance: color('u_pwm'),
        keyword: color('always'),
        type: color('wire'),
        number: color("8'hFF"),
        comment: color('// note'),
        string: color('"hi"'),
        operator: color('<='),
        system: color('$unsigned'),
    };
    const distinct = new Set(Object.values(groups).filter(Boolean));
    check('dark modern: different symbol kinds have different colors',
        groups.port !== groups.signal && groups.signal !== groups.parameter
        && groups.module !== groups.instance && groups.keyword !== groups.number
       ,
        JSON.stringify(groups));
    check('dark modern: at least 8 distinct token colors in one file', distinct.size >= 8,
        `${distinct.size}: ${[...distinct].join(',')}`);

    // ------------------------------------------------------------------ VHDL
    const vhdl = await registry.loadGrammar('source.vhdl');
    const vhdlRows = [
        'library ieee;',
        'entity demo is',
        '  port(clk : in std_logic; q : out std_logic);',
        'end entity;',
        'architecture rtl of demo is',
        "  signal s : std_logic := '0';",
        '  constant N : integer := 16#FF#;',
        'begin',
        '  q <= s after 5 ns;  -- note',
        'end architecture;',
    ];
    stack = vsctm.INITIAL_STATE;
    const vhdlTokens = [];
    for (const row of vhdlRows) {
        vhdlTokens.push(vhdl.tokenizeLine(row, stack).tokens);
        stack = vhdl.tokenizeLine(row, stack).ruleStack;
    }
    const vscope = (needle, offset = 0) => {
        const line = vhdlRows.findIndex((row) => row.includes(needle));
        return line < 0 ? '' : scopesAt(vhdlTokens[line], vhdlRows[line].indexOf(needle) + offset);
    };
    check('vhdl: entity name', /entity\.name\.type\.vhdl/.test(vscope('demo')));
    check('vhdl: port name', /entity\.name\.tag\.port\.vhdl/.test(vscope('clk')));
    check('vhdl: signal name', /variable\.other\.readwrite\.vhdl/.test(vscope('s ')));
    check('vhdl: constant name', /variable\.other\.constant\.vhdl/.test(vscope('N ')));
    check('vhdl: std_logic type', /support\.type\.vhdl/.test(vscope('std_logic')));
    check('vhdl: based literal', /constant\.numeric\.vhdl/.test(vscope('16#FF#')));
    check('vhdl: comment', /comment\.line\.double-dash\.vhdl/.test(vscope('-- note')));
    check('vhdl: operator', /keyword\.operator\.vhdl/.test(vscope('<=')));
    check('vhdl: keyword', /keyword\.control\.vhdl/.test(vscope('library')));

    fs.writeFileSync(path.join(__dirname, 'grammar-result.txt'),
        failures ? `GRAMMAR: FAIL (${failures})` : 'GRAMMAR: PASS');
    console.log(`theme: ${theme.source}`);
    console.log(failures ? `\n${failures} FAILED` : '\nALL PASS');
    process.exit(failures === 0 ? 0 : 1);
}

main().catch((error) => {
    console.error(error);
    process.exit(1);
});
