"""Derive the `Engine::Stats` field list from `engine.hpp`.

The point of this script is that the *document* is derived from the
*header*, not hand-maintained against it: the field list in
`docs/api/engine_api.md` §13 has to name every field the struct
actually has, and the only way to keep that true over time is to
derive it.

Two things come out of it:

  - `--markdown` writes the reference table
    (`docs/api/stats_field_reference.md`) that the API document
    links to;
  - `--check` compares the table on disk with the freshly derived
    one and exits non-zero when they differ, which is what lets CI
    fail on a *silent* drift between the header and the document.

Usage:
    python tools/dump_stats_fields.py --markdown > <file>
    python tools/dump_stats_fields.py --check <file>
"""
import re
import sys

HEADER = 'src/engine/include/nimrtc/engine/engine.hpp'

SECTION_RE = re.compile(r'^\s*//\s*[-=]{2,}\s*(.*?)\s*[-=]{2,}\s*$')
# The declaration of one member.
#
# The type may be namespace-qualified (`plugins::QoeSnapshot`) and the
# initialiser may be a brace-init (`remote_desktop{}`) as well as a
# plain `= 0`.  A pattern that only knows `= ...` silently *drops*
# those fields: the derived table then claims the struct has fewer
# members than it does, and the CI gate that is supposed to catch drift
# instead ratifies the omission.  The type alternative therefore
# accepts a qualified name, and the initialiser accepts both forms.
DECL_RE = re.compile(
    r'^\s*(?:const\s+)?'
    r'((?:[A-Za-z_]\w*::)*'          # optional namespace qualifier
    r'(?:std::(?:u?int\d+_t|size_t)|u?int\d+_t|int\d+_t|int|float|double|bool|'
    r'std::string|std::optional<[^>]+>|std::vector<[^>]+>|'
    r'[A-Z][A-Za-z0-9_]*))'
    r'\s+(\w+)\s*(?:=[^;]*|\{[^;]*\})?;')
TRAILING_RE = re.compile(r'//\s*(.*)$')
# A one-line Doxygen comment: `/** text */`.
DOC_RE = re.compile(r'^/\*\*(.+?)\*/\s*$')


def extract_block(text, start_marker):
    i = text.index(start_marker)
    d = 0
    for j in range(i, len(text)):
        if text[j] == '{':
            d += 1
        elif text[j] == '}':
            d -= 1
            if d == 0:
                return text[i:j + 1]
    raise ValueError('unterminated block: ' + start_marker)


def clean(text):
    """Strip the comment punctuation from one line of prose.

    `/** The text. */` and ` *  continuation */` and a bare `*/` all
    reduce to their prose; the doc-block delimiters are markup, not part
    of the sentence the field's definition is written in.
    """
    text = text.strip()
    for marker in ('*/', '/*!', '/**', '/*'):
        if text.startswith(marker):
            text = text[len(marker):].strip()
    if text.endswith('*/'):
        text = text[:-2].strip()
    if text.startswith('*'):
        text = text[1:].strip()
    return text


def strip_stars(text):
    return [clean(line) for line in text.splitlines()]


def code_of(raw, in_block):
    """Return the line with every comment removed.

    A brace inside a comment must not move the nesting depth, so the
    comment has to go before the depth is counted.
    """
    out = []
    i = 0
    n = len(raw)
    while i < n:
        if in_block:
            end = raw.find('*/', i)
            if end == -1:
                return ''.join(out), True
            i = end + 2
            in_block = False
            continue
        two = raw[i:i + 2]
        if two == '/*':
            in_block = True
            i += 2
            continue
        if two == '//':
            break
        out.append(raw[i])
        i += 1
    return ''.join(out), in_block


class Run:
    """A contiguous block of comment lines.

    A run is either a *section* note (it follows a section banner) or
    a *field* note (it sits directly above a declaration).  Which one
    it is only becomes clear when the next piece of code is seen, so
    the run is held until then.
    """

    def __init__(self, lines):
        self.lines = list(lines)

    def text(self):
        return ' '.join(l for l in self.lines if l).strip()


def parse(body):
    """Return the list of (section, type, name, comment) records.

    Only the *direct* members of the struct are recorded: a nested
    struct's own members belong to that struct's documentation, not to
    the parent's field list.
    """
    fields = []
    section = ''
    run = None             # the comment run currently open
    depth = 0              # the brace depth inside the struct body
    in_block = False
    doc_open = False       # is a multi-line /** */ block still open?
    after_banner = False   # is the open run the section's note?

    def close_run():
        """Return the open run's text and forget it."""
        nonlocal run
        if run is None:
            return ''
        text = run.text()
        run = None
        return text

    def finish_trailing():
        """Return whatever note is open just above the declaration.

        A `//` note that sits between the field's doc block and the field
        *is* the field's.

        The ` * ` continuation lines of a multi-line doc block are appended
        to its run earlier, so by the time a declaration is reached the
        open run is only ever a `//` note -- or the tail of the section's
        own paragraph, which `after_banner` already filters out.
        """
        return close_run()

    for raw in body.splitlines()[1:-1]:
        stripped = raw.strip()

        # ---- the code, with the comments removed -----------------------
        code, in_block = code_of(raw, in_block)

        # ---- a section banner: // ---- something ---- --------------------
        #
        # Checked *before* the doc block: a banner is a line comment, and
        # the `----` around its title are the section markers.
        m = SECTION_RE.match(raw)
        if m:
            close_run()
            section = m.group(1).strip()
            after_banner = True
            continue

        # ---- a doc block: /** ... */ -----------------------------------
        if stripped.startswith('/*') and not code.strip():
            if '*/' in raw:
                inner = raw.split('/*', 1)[1].split('*/', 1)[0]
                stripped_doc = strip_stars(inner)
                run = Run(stripped_doc)
                after_banner = False
                # A one-line `/** text */` is complete; a multi-line block
                # continues on its ` * ` lines.  (`code_of()` unwinds a
                # block comment it opened on the same line, so `in_block`
                # says nothing about *this* block -- the presence of the
                # closing `*/` does.)
                doc_open = True
                continue
            run = Run(strip_stars(raw))
            doc_open = True
            after_banner = False
            continue

        # ---- the continuation of a multi-line doc block ------------------
        if doc_open:
            if stripped.startswith('*') and not code.strip():
                run.lines.extend(strip_stars(stripped))
                if '*/' in stripped:
                    doc_open = False
                continue
            doc_open = False

        # ---- a plain comment --------------------------------------------
        if stripped.startswith('//'):
            text = stripped[2:].strip()
            if not text:
                # A blank comment line ends the paragraph but not the
                # run; the run ends at the next piece of code.
                if run is not None:
                    run.lines.append('')
                continue
            if run is None:
                run = Run([])
            run.lines.append(text)
            continue

        if not code.strip():
            # A blank line ends the run: the next comment run is a new
            # one, and it is not the section's note.  The *section* stays
            # open, though -- a banner's scope is the code that follows it
            # until the next banner, not the paragraphs directly under it.
            if run is not None:
                run = None
            if after_banner:
                after_banner = False
            continue

        # ---- a declaration ----------------------------------------------
        m = DECL_RE.match(code)
        if m and depth == 0:
            note = ''
            t = TRAILING_RE.search(raw)
            if t:
                note = t.group(1).strip()
            prose = finish_trailing()
            if after_banner:
                # The run was the *section's* note, not the field's.
                prose = ''
            parts = [p for p in (prose, note) if p]
            fields.append((section, m.group(1), m.group(2), ' '.join(parts)))
            after_banner = False
            depth += code.count('{') - code.count('}')
            continue

        # ---- anything else ends the comment run -------------------------
        close_run()
        after_banner = False
        depth += code.count('{') - code.count('}')

    return fields


def table_cell(comment):
    """Turn a field's prose into one safe Markdown table cell.

    The header's comments are Doxygen prose: they carry `@brief`, run
    for several paragraphs, and sometimes contain fenced code.  A table
    cell needs *one* sentence-shaped string, so the structural markup
    is dropped and the prose is kept; nothing is invented, and the
    header remains the place the full definition lives.
    """
    lines = []
    for line in comment.splitlines():
        text = line.strip()
        if text.startswith('@'):
            text = text.split(None, 1)[1] if ' ' in text else ''
            text = text.strip().lstrip('-').strip()
        if text.lstrip().startswith('```'):
            continue
        if text:
            lines.append(text)
    prose = ' '.join(lines)
    # A pipe inside the prose would break the table.
    prose = prose.replace('|', '\\|')
    return re.sub(r'\s+', ' ', prose).strip()


def markdown(fields):
    out = []
    out.append('# The `Engine::Stats` field reference')
    out.append('')
    out.append('<!-- GENERATED by tools/dump_stats_fields.py -- do not edit by hand. -->')
    out.append('')
    out.append('Every field `NimRTCEngine::Stats` carries, in declaration order,')
    out.append('with the definition the header itself gives it.  The table is')
    out.append('derived from `src/engine/include/nimrtc/engine/engine.hpp` by')
    out.append('`tools/dump_stats_fields.py`; CI fails when it drifts from the')
    out.append('header, so it cannot go stale.')
    out.append('')
    out.append('| # | Field | Type | Meaning |')
    out.append('|---|---|---|---|')

    n = 0
    for section, typ, name, comment in fields:
        n += 1
        prose = table_cell(comment)
        out.append('| %d | `%s` | `%s` | %s |' % (n, name, typ, prose or '--'))
    out.append('')
    out.append('**%d fields.**' % len(fields))
    out.append('')
    return '\n'.join(out)


def main():
    text = open(HEADER, encoding='utf-8').read()
    body = extract_block(text, 'struct NIMRTC_API_STABLE Stats {')
    fields = parse(body)

    if '--markdown' in sys.argv:
        print(markdown(fields))
        return 0

    if '--check' in sys.argv:
        path = sys.argv[sys.argv.index('--check') + 1]
        want = markdown(fields)
        have = open(path, encoding='utf-8').read()
        if want.strip() != have.strip():
            print('[stats-fields] DRIFT: %s does not match the header.' % path)
            print('[stats-fields] Regenerate it with:')
            print('[stats-fields]   python tools/dump_stats_fields.py --markdown > %s' % path)
            return 1
        print('[stats-fields] OK: %s matches the header (%d fields).' % (path, len(fields)))
        return 0

    print('field count: %d' % len(fields))
    print()
    for section, typ, name, comment in fields:
        print('### %s' % name)
        print('    type    : %s' % typ)
        print('    section : %s' % section)
        if comment:
            print('    comment : %s' % comment[:400])
        print()
    return 0


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    sys.exit(main())
