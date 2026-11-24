// line_budget.js -- the acceptance test's independent line counter.
//
// The gate (`tools/check_sample_line_budget.py`) is the source of truth
// in CI; this module re-implements the SAME counting rules in JS so the
// acceptance test can verify the sample's budget without trusting the
// gate on its own word (spec §二 / §三: a limit that drifts is a claim
// that drifted).  The two implementations are pinned to each other by
// the gate's self-test and by the acceptance test's cross-check.
//
// Counting rules (spec §二):
//   * empty lines do not count;
//   * a line whose first token is `//` does not count;
//   * every line of a `/* ... */` block comment does not count, including
//     the first and last line;
//   * a block comment followed by code on the same line DOES count;
//   * `//` inside a string literal is NOT a comment ("http://host" is a
//     code line, never a comment).
'use strict';
const fs = require('node:fs');
const path = require('node:path');

/** Count non-comment code lines in C/C++ source (mirror of the gate). */
function countNonCommentLines(text) {
    let inBlock = false;
    let count = 0;
    for (const line of text.split(/\r?\n/)) {
        const [hasCode, next] = scanLine(line, inBlock);
        if (hasCode) count += 1;
        inBlock = next;
    }
    return count;
}

function scanLine(line, inBlock) {
    let hasCode = false;
    let i = 0;
    const n = line.length;
    while (i < n) {
        const c = line[i];
        if (inBlock) {
            if (c === '*' && i + 1 < n && line[i + 1] === '/') {
                inBlock = false;
                i += 2;
            } else {
                i += 1;
            }
        } else if (c === '"' || c === "'") {
            // A string/char literal: a comment marker inside it is data.
            hasCode = true;
            const quote = c;
            i += 1;
            while (i < n) {
                if (line[i] === '\\') i += 2;
                else if (line[i] === quote) { i += 1; break; }
                else i += 1;
            }
        } else if (c === '/' && i + 1 < n && line[i + 1] === '/') {
            i = n; // `//` line comment: the rest of the line is not code.
        } else if (c === '/' && i + 1 < n && line[i + 1] === '*') {
            inBlock = true;
            i += 2;
        } else {
            if (!/\s/.test(c)) hasCode = true;
            i += 1;
        }
    }
    return [hasCode, inBlock];
}

/** All `.cpp` / `.hpp` under the sample directory (relative paths). */
function sampleSourceFiles(sampleDir) {
    const out = [];
    const walk = (dir) => {
        for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
            const full = path.join(dir, entry.name);
            if (entry.isDirectory()) walk(full);
            else if (entry.name.endsWith('.cpp') || entry.name.endsWith('.hpp')) {
                out.push(path.relative(sampleDir, full).split(path.sep).join('/'));
            }
        }
    };
    walk(sampleDir);
    return out.sort();
}

/**
 * Judge the sample against its declared budget.
 * @returns {{ total: number, budget: number, limit: number,
 *             status: 'pass'|'warn'|'fail', files: Array<{name, lines}> }}
 */
function budgetStatus(sampleDir, declaration) {
    const budget = declaration.budget;
    const limit = Math.floor(budget * (1.0 + declaration.drift_allowance));
    const files = sampleSourceFiles(sampleDir)
        .map((name) => ({ name, lines: countNonCommentLines(
            fs.readFileSync(path.join(sampleDir, name), 'utf8')) }));
    const total = files.reduce((s, f) => s + f.lines, 0);
    let status = 'pass';
    if (total > limit) status = 'fail';
    else if (total > budget) status = 'warn';
    return { total, budget, limit, status, files };
}

/** Read and validate the sample's budget declaration. */
function loadDeclaration(sampleDir) {
    const raw = fs.readFileSync(path.join(sampleDir, 'sample_line_budget.json'), 'utf8');
    const data = JSON.parse(raw);
    const budget = Number(data.budget);
    const drift = Number(data.drift_allowance);
    if (!Number.isFinite(budget) || !Number.isFinite(drift)) {
        throw new Error('sample_line_budget.json: budget/drift_allowance must be numbers');
    }
    return { budget, drift_allowance: drift, exemptions: data.exemptions || [] };
}

module.exports = {
    countNonCommentLines,
    sampleSourceFiles,
    budgetStatus,
    loadDeclaration,
};
