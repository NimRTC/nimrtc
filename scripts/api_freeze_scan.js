'use strict';
/**
 * API-FREEZE per-symbol scanner.
 *
 * Walks every public header under `src/<mod>/include/nimrtc/**` (excluding
 * `src/third_party/**`), extracts every namespace-scope declaration
 * (class / struct / enum / union / free function / constant / alias /
 * variable) and reports whether it already carries a stability marker
 * (NIMRTC_API_STABLE / NIMRTC_API_UNSTABLE / NIMRTC_API_DEPRECATED).
 *
 * Output: build/_api_freeze_symbols.json (machine readable)
 *         build/_api_freeze_report.md  (human readable)
 *
 * Usage: node scripts/api_freeze_scan.js [--no-md]
 */

const fs = require('fs');
const path = require('path');

const ROOT = path.resolve(__dirname, '..');
const SRC = path.join(ROOT, 'src');
const BUILD = path.join(ROOT, 'build');

// ---------------------------------------------------------------------------
// Header collection
// ---------------------------------------------------------------------------

function walk(dir, out) {
    for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
        const p = path.join(dir, entry.name);
        if (entry.isDirectory()) walk(p, out);
        else if (entry.isFile() && entry.name.endsWith('.hpp')) out.push(p);
    }
    return out;
}

function collectHeaders() {
    const all = walk(SRC, []);
    return all
        .filter((p) => /[\\/]include[\\/]nimrtc[\\/]/.test(p))
        .filter((p) => !/[\\/]third_party[\\/]/.test(p))
        .sort();
}

// ---------------------------------------------------------------------------
// Source sanitising: remove comments and string/char literal bodies while
// preserving line structure (so line numbers stay accurate).
// ---------------------------------------------------------------------------

function sanitize(src) {
    let out = '';
    let i = 0;
    const n = src.length;
    let state = 'code';
    while (i < n) {
        const c = src[i];
        const c2 = src.substr(i, 2);
        if (state === 'code') {
            if (c2 === '//') { state = 'line'; out += '  '; i += 2; continue; }
            if (c2 === '/*') { state = 'block'; out += '  '; i += 2; continue; }
            if (c === '"') { state = 'str'; out += ' '; i += 1; continue; }
            if (c === "'") { state = 'chr'; out += ' '; i += 1; continue; }
            out += c; i += 1; continue;
        }
        if (state === 'line') {
            if (c === '\n') { state = 'code'; out += '\n'; } else out += ' ';
            i += 1; continue;
        }
        if (state === 'block') {
            if (c2 === '*/') { state = 'code'; out += '  '; i += 2; continue; }
            out += (c === '\n' ? '\n' : ' '); i += 1; continue;
        }
        // str / chr
        const close = state === 'str' ? '"' : "'";
        if (c === '\\') { out += '  '; i += 2; continue; }
        if (c === close) { state = 'code'; out += ' '; i += 1; continue; }
        out += (c === '\n' ? '\n' : ' '); i += 1; continue;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Declaration extraction
// ---------------------------------------------------------------------------

const MARKER_RE = /NIMRTC_API_(STABLE|UNSTABLE|DEPRECATED)/g;

function markersOf(text) {
    const found = [];
    let m;
    MARKER_RE.lastIndex = 0;
    while ((m = MARKER_RE.exec(text)) !== null) found.push(m[1]);
    return found;
}

const MARKER_OPT = '(?:NIMRTC_API_(?:STABLE|UNSTABLE)\\s+)*';

// Strip leading macro / specifier noise so the "kind" keyword becomes visible.
function headOf(text) {
    let t = text.trim();
    for (;;) {
        const before = t;
        t = t.replace(/^(?:\s|\/\*[\s\S]*?\*\/)+/, '');
        t = t.replace(/^template\s*<[^<>]*>\s*/, '');
        t = t.replace(/^(?:NIMRTC_API_(?:STABLE|UNSTABLE|DEPRECATED)\s*\([^)]*\)|NIMRTC_API_(?:STABLE|UNSTABLE)|inline|static|constexpr|consteval|constinit|explicit|virtual|friend|extern|typedef|export|__declspec\([^)]*\)|__attribute__\s*\(\([^)]*\)\)|alignas\([^)]*\)|NIMRTC_[A-Z_0-9]+)\s+/, '');
        if (t === before) break;
    }
    return t;
}

const CONTROL_WORDS = ['if', 'for', 'while', 'switch', 'return', 'sizeof', 'decltype', 'catch', 'throw'];

function classifyHead(text) {
    const t = headOf(text);
    let m;
    if ((m = new RegExp('^(class|struct|union)\\s+' + MARKER_OPT + '([A-Za-z_]\\w*)').exec(t))) {
        return { kind: m[1], name: m[2] };
    }
    if ((m = new RegExp('^enum(?:\\s+class|\\s+struct)?\\s+' + MARKER_OPT + '([A-Za-z_]\\w*)').exec(t))) {
        return { kind: 'enum', name: m[1] };
    }
    if ((m = /^using\s+namespace\s+([A-Za-z_][\w:]*)/.exec(t))) return { kind: 'using_namespace', name: m[1] };
    if ((m = new RegExp('^using\\s+' + MARKER_OPT + '([A-Za-z_]\\w*)\\s*=').exec(t))) return { kind: 'using', name: m[1] };
    if ((m = /^typedef\s+([\s\S]*?)\b([A-Za-z_]\w*)\s*;/.exec(t))) return { kind: 'typedef', name: m[2] };
    // free function: <return type> name(
    if ((m = /\boperator\s*([^\s(]+)\s*\(/.exec(t))) return { kind: 'function', name: 'operator' + m[1] };
    if ((m = /\b([A-Za-z_]\w*)\s*\(/.exec(t))) {
        if (CONTROL_WORDS.includes(m[1])) return null;
        return { kind: 'function', name: m[1] };
    }
    // constant / variable declaration:  <type> <name> [= ...] ;
    if ((m = /^(?:const\s+|constexpr\s+|consteval\s+|inline\s+|static\s+|extern\s+)*[\w:<>,\s*&]*?\b([A-Za-z_]\w*)\s*=/.exec(t))) {
        if (CONTROL_WORDS.includes(m[1])) return null;
        return { kind: 'constant', name: m[1] };
    }
    return null;
}

// Extract namespace-scope declarations from one header.
function extractDecls(file) {
    const raw = fs.readFileSync(file, 'utf-8');
    const code = sanitize(raw);
    const lines = code.split('\n');
    const decls = [];

    let depth = 0;
    const nsStack = [];
    let buf = [];
    let bufStart = 0;
    // True while inside a multi-line preprocessor directive (a macro whose
    // body continues on the next line via a trailing backslash).  The
    // continuation lines are NOT declarations -- treating them as such
    // would make the scanner report macro bodies (e.g. a `do { ... } while(0)`
    // logging macro) as un-annotated public functions.
    let in_macro = false;

    const flush = (endLine) => {
        const text = buf.join('\n');
        const info = classifyHead(text);
        if (info) {
            decls.push({
                kind: info.kind,
                name: info.name,
                ns: nsStack.length ? nsStack.join('::') : '(global)',
                line: bufStart,
                endLine,
                text: text.replace(/\s+/g, ' ').trim().slice(0, 220),
                markers: markersOf(text),
                hasBody: /\{/.test(text)
            });
        }
        buf = [];
    };

    for (let li = 0; li < lines.length; li++) {
        const line = lines[li];
        const t = line.trim();

        if (buf.length === 0) {
            if (t === '') continue;
            if (t.startsWith('#')) {
                // Preprocessor directive: skip it and any backslash-continued
                // continuation lines.
                in_macro = /\\\s*$/.test(line);
                continue;
            }
            if (in_macro) {
                in_macro = /\\\s*$/.test(line);
                continue;
            }
            const nm = /^namespace\s+([A-Za-z_][\w:]*)\s*\{/.exec(t);
            if (nm) {
                const rest = t.slice(t.indexOf('{') + 1);
                if (rest.includes('}')) {
                    const inner = rest.slice(0, rest.lastIndexOf('}'));
                    if (inner.trim()) {
                        nsStack.push(nm[1]);
                        buf = [inner];
                        bufStart = li + 1;
                        flush(li + 1);
                        nsStack.pop();
                    }
                } else {
                    nsStack.push(nm[1]);
                    depth = 1;
                }
                continue;
            }
            if (/^namespace\b/.test(t)) { nsStack.push('(anonymous)'); depth = 1; continue; }
            if (t === '}' || t.startsWith('} ') || t.startsWith('};')) {
                if (depth > 0) depth = 0;
                else if (nsStack.length) nsStack.pop();
                continue;
            }
            bufStart = li + 1;
            buf = [line];
        } else {
            buf.push(line);
        }

        const text = buf.join('\n');
        let braces = 0, parens = 0, hasSemi = false;
        for (const ch of text) {
            if (ch === '(') parens++;
            else if (ch === ')') parens--;
            else if (ch === '{') braces++;
            else if (ch === '}') braces--;
            else if (ch === ';' && parens === 0 && braces === 0) hasSemi = true;
        }
        if (braces > 0) continue;         // body opened, keep buffering
        if (hasSemi || braces < 0) flush(li + 1);
    }
    if (buf.length) flush(lines.length);

    return decls;
}

// ---------------------------------------------------------------------------
// Classification policy
// ---------------------------------------------------------------------------

const UNSTABLE_HEADERS = [
    /[\\/]web_transport[\\/]include[\\/]/,
    /[\\/]modules[\\/]dtls[\\/]include[\\/]nimrtc[\\/]dtls[\\/]dtls_handshake_reassembler\.hpp$/,
];

function isInternal(name, ns) {
    if (/^NIMRTC_/.test(name)) return true;
    if (/\bImpl$/.test(name)) return true;
    if (/^_/.test(name)) return true;
    if (/(^|::)(detail|internal|impl)$/.test(ns)) return true;
    return false;
}

function dominantMarker(decls) {
    let stable = 0, unstable = 0;
    for (const d of decls) {
        for (const m of d.markers) {
            if (m === 'STABLE') stable++;
            else if (m === 'UNSTABLE') unstable++;
        }
    }
    if (unstable > stable) return 'UNSTABLE';
    if (stable > 0) return 'STABLE';
    return null;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

function main() {
    const headers = collectHeaders();
    const rel = (p) => path.relative(ROOT, p).replace(/\\/g, '/');

    const perFile = [];
    const all = [];

    for (const h of headers) {
        const decls = extractDecls(h);
        perFile.push({ file: rel(h), decls });
        for (const d of decls) all.push({ ...d, file: rel(h) });
    }

    // Deduplicate forward declarations: a name declared twice (fwd decl +
    // definition) counts once; classified if ANY instance carries a marker.
    const byKey = new Map();
    for (const d of all) {
        const key = d.file + '|' + d.kind + '|' + d.name;
        if (!byKey.has(key)) byKey.set(key, []);
        byKey.get(key).push(d);
    }

    const symbols = [];
    for (const [, group] of byKey) {
        const defs = group.filter((g) => g.hasBody);
        const primary = defs[0] || group[0];
        const markers = new Set();
        for (const g of group) for (const m of g.markers) markers.add(m);
        symbols.push({
            file: primary.file,
            ns: primary.ns,
            kind: primary.kind,
            name: primary.name,
            line: primary.line,
            text: primary.text,
            markers: [...markers],
            occurrences: group.length,
            forwardDeclared: group.some((g) => !g.hasBody),
            hasDefinition: defs.length > 0
        });
    }

    symbols.sort((a, b) => (a.file === b.file ? a.line - b.line : a.file < b.file ? -1 : 1));

    const fileDominant = new Map();
    for (const pf of perFile) fileDominant.set(pf.file, dominantMarker(pf.decls));

    let already = 0;
    const missing = [];
    const internal = [];
    for (const s of symbols) {
        if (s.markers.length > 0) { already++; s.recommend = s.markers[0]; s.reason = 'already classified'; continue; }
        if (isInternal(s.name, s.ns)) {
            s.recommend = 'NONE';
            s.reason = 'internal symbol (detail namespace / Impl / _private) -- left unannotated';
            internal.push(s);
            continue;
        }
        const dom = fileDominant.get(s.file);
        let rec = 'STABLE';
        let reason = 'public namespace-scope symbol; header surface is already STABLE-classified';
        if (UNSTABLE_HEADERS.some((re) => re.test(s.file))) {
            rec = 'UNSTABLE';
            reason = 'header is an experimental seam (web_transport / dtls handshake reassembler)';
        } else if (dom === 'UNSTABLE') {
            rec = 'UNSTABLE';
            reason = 'header-level classification is UNSTABLE';
        }
        s.recommend = rec;
        s.reason = reason;
        missing.push(s);
    }

    const out = {
        generated: new Date().toISOString(),
        headerCount: headers.length,
        symbolCount: symbols.length,
        alreadyClassified: already,
        missingCount: missing.length,
        internalCount: internal.length,
        headers: perFile.map((p) => ({ file: p.file, symbols: p.decls.length })),
        symbols
    };
    fs.mkdirSync(BUILD, { recursive: true });
    fs.writeFileSync(path.join(BUILD, '_api_freeze_symbols.json'), JSON.stringify(out, null, 2));

    console.log(`Public headers : ${headers.length}`);
    console.log(`Symbols        : ${symbols.length}`);
    console.log(`Classified     : ${already}`);
    console.log(`Missing        : ${missing.length}`);
    console.log(`Internal       : ${internal.length}  (no marker expected)`);
    const kindBreak = {};
    for (const s of symbols) kindBreak[s.kind] = (kindBreak[s.kind] || 0) + 1;
    console.log('Kinds:', kindBreak);
    if (missing.length) {
        const byFile = {};
        for (const s of missing) byFile[s.file] = (byFile[s.file] || 0) + 1;
        console.log('\nMissing per file:');
        Object.entries(byFile).sort((a, b) => b[1] - a[1]).forEach(([f, c]) => console.log(`  ${c}\t${f}`));
    }
    // Non-zero exit when a public symbol still lacks its classification:
    // this is the API-FREEZE gate (ADR-015 "Verification").
    process.exitCode = missing.length ? 1 : 0;
}

main();
