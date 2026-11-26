// Update Project (command id `fpgaLsp.updateProject` -> LSP `fpgalsp.updateProject`).
//
// Why this module exists: the command used to show only "see logs" on failure and wrote
// **nothing** to the output channel, while the actual TangDynasty diagnosis only lived in the
// JSON `output` field. Users saw a failed command with an empty log, so there was no way to
// tell a real `open_project -update` failure from a TD warning.
//
// Every outcome is now logged on one line (command / project / result or exception) and a
// failure always carries a readable reason. The dependency injection keeps the reporting
// logic unit-testable without a VS Code instance.
//
// Note: all user-visible strings are English (the extension UI is English-only).

'use strict';

const OUTPUT_PREFIX = '[updateProject]';
const FAILED_HINT = '(see FPGA LSP output for details)';
const COMMAND_ID = 'fpgalsp.updateProject';

function compact(value) {
    if (value === undefined) return 'undefined';
    try {
        return JSON.stringify(value);
    } catch (_) {
        return String(value);
    }
}

function lastNonEmptyLine(text) {
    if (typeof text !== 'string' || !text) return '';
    const lines = text.split(/\r?\n/).map((line) => line.trim()).filter(Boolean);
    return lines.length ? lines[lines.length - 1] : '';
}

// Pick the most useful sentence out of a failed response:
//   1. the backend's `error` field (added by analysis::UpdateProject when it can explain),
//   2. `reason` (some commands use it instead),
//   3. the last non-empty line of the TD `output` tail (that is where TD puts PRJ-xxxx/HDL-xxxx),
//   4. a neutral fallback so the message is never "undefined".
function failureReason(result) {
    if (!result) return 'no response from the language server';
    if (typeof result.error === 'string' && result.error.trim()) return result.error.trim();
    if (typeof result.reason === 'string' && result.reason.trim()) return result.reason.trim();
    const tail = lastNonEmptyLine(result.output);
    if (tail) return tail;
    return 'unknown error (the response has no error/output field)';
}

async function runUpdateProject(deps) {
    const execute = deps.execute;
    const appendLine = deps.appendLine || (() => undefined);
    const showInformationMessage = deps.showInformationMessage || (() => Promise.resolve(undefined));
    const showErrorMessage = deps.showErrorMessage || (() => Promise.resolve(undefined));
    const showWarningMessage = deps.showWarningMessage || (() => Promise.resolve(undefined));
    const onShowLogs = deps.onShowLogs;
    const project = deps.project || 'unknown project';

    const reportFailure = async (reason, detail) => {
        appendLine(`${OUTPUT_PREFIX} FAILED command=${COMMAND_ID} project=${project} ${detail}`);
        const choice = await showErrorMessage(`Update Project failed: ${reason} ${FAILED_HINT}`, 'Show Logs');
        if (choice === 'Show Logs' && onShowLogs) await onShowLogs();
        return { ok: false, reason };
    };

    let result;
    try {
        result = await execute();
    } catch (err) {
        const reason = (err && err.message) || String(err);
        return reportFailure(reason, `exception=${reason}`);
    }

    if (!result || !result.ok) {
        return reportFailure(failureReason(result), `result=${compact(result)}`);
    }

    appendLine(`${OUTPUT_PREFIX} ok command=${COMMAND_ID} project=${project} ` +
        `al=${result.al || '(not reported)'}`);
    // The backend can succeed while the `.al` still records a stale `<Project Path="...">`
    // (the project folder was moved or copied). Update Project is fine because the TD session
    // cwd is the .al folder, but the user should know: opening the project once in the TD GUI
    // re-records that path. Measured on C:/work/CorpusBeta (Path="D:/projects/old-location/...").
    if (typeof result.warning === 'string' && result.warning.trim()) {
        const warning = result.warning.trim();
        appendLine(`${OUTPUT_PREFIX} WARNING command=${COMMAND_ID} project=${project} warning=${warning}`);
        const choice = await showWarningMessage(`Update Project completed with a warning: ${warning}`, 'Show Logs');
        if (choice === 'Show Logs' && onShowLogs) await onShowLogs();
        if (deps.refresh) await deps.refresh();
        return { ok: true, warning };
    }
    await showInformationMessage('FPGA: open_project -update completed (.al is in sync with disk)');
    if (deps.refresh) await deps.refresh();
    return { ok: true };
}

module.exports = { runUpdateProject, failureReason, OUTPUT_PREFIX, COMMAND_ID };
