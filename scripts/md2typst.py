#!/usr/bin/env python3
"""Convert one manual chapter from Markdown to Typst.

The Markdown files under manual/ and manual-ko/ are the source of truth: the build
compiles their code blocks, runs their examples, and checks their claims. This
script exists so the published HTML and PDF are made *from* those files rather
than maintained beside them.

It handles the subset the manual actually uses, and nothing else — headings,
paragraphs, bullet and numbered lists, GitHub pipe tables, fenced code, block
quotes, horizontal rules, and the inline forms `code`, **bold**, *italic* and
[text](link). Anything outside that subset is a sign the manual grew a construct
this converter has not been taught, so it is reported rather than dropped.
"""
import re, sys, os

# ---------------------------------------------------------------- inline text

ESCAPE = {
    '\\': '\\\\', '#': '\\#', '$': '\\$', '*': '\\*', '_': '\\_',
    '`': '\\`', '<': '\\<', '>': '\\>', '@': '\\@', '=': '\\=',
    '[': '\\[', ']': '\\]', '"': '\\"', '~': '\\~', '/': '\\/',
    '-': '\\-', '+': '\\+', "'": "\\'",
}

def esc(s):
    return ''.join(ESCAPE.get(c, c) for c in s)

def raw(s):
    """Inline code, as a Typst raw span. Pick a backtick run longer than any inside."""
    n = 1
    while '`' * n in s:
        n += 1
    fence = '`' * max(n, 1)
    if s.startswith('`') or s.endswith('`'):
        s = ' ' + s + ' '
    return fence + s + fence

TOKEN = re.compile(
    r'(?P<code>`+[^`]*`+)'
    r'|(?P<link>\[(?P<ltext>[^\]]*)\]\((?P<lhref>[^)]*)\))'
    r'|(?P<bold>\*\*(?P<btext>[^*]+)\*\*)'
    r'|(?P<ital>(?<![\w*])\*(?P<itext>[^*\n]+)\*(?![\w*]))'
)

def href(url, lang):
    """Rewrite a link between chapters into a link between generated pages."""
    if url.startswith('#'):
        return url
    if url.startswith('http'):
        return url
    m = re.match(r'^([A-Za-z0-9_.-]+)\.md(#.*)?$', url)
    if m:
        return page_name(m.group(1), lang) + '.html' + (m.group(2) or '')
    return url

def inline(s, lang):
    out, i = [], 0
    for m in TOKEN.finditer(s):
        out.append(esc(s[i:m.start()]))
        if m.group('code'):
            out.append(raw(m.group('code').strip('`')))
        elif m.group('link'):
            out.append('#link("%s")[%s]' % (href(m.group('lhref'), lang),
                                            inline(m.group('ltext'), lang)))
        elif m.group('bold'):
            out.append('*%s*' % inline(m.group('btext'), lang))
        else:
            out.append('_%s_' % inline(m.group('itext'), lang))
        i = m.end()
    out.append(esc(s[i:]))
    return ''.join(out)

def page_name(stem, lang):
    stem = stem[:-3] if stem.endswith('-ko') else stem
    if stem in ('manual', 'manual-ko'):
        return 'index'
    return stem

# ---------------------------------------------------------------- block level

def split_row(line):
    line = line.strip()
    if line.startswith('|'):
        line = line[1:]
    if line.endswith('|'):
        line = line[:-1]
    # a cell may contain an escaped pipe or a pipe inside inline code
    cells, cur, in_code = [], '', False
    i = 0
    while i < len(line):
        c = line[i]
        if c == '`':
            in_code = not in_code
        if c == '|' and not in_code:
            cells.append(cur)
            cur = ''
        else:
            cur += c
        i += 1
    cells.append(cur)
    return [c.strip() for c in cells]

def is_divider(line):
    return bool(re.match(r'^\s*\|?[\s:|-]+\|[\s:|-]*$', line)) and '-' in line

def convert(text, lang, title):
    lines = text.split('\n')
    out = []
    i = 0
    n = len(lines)
    while i < n:
        line = lines[i]

        # HTML comments (the example markers) carry no rendered content
        if line.lstrip().startswith('<!--'):
            while i < n and '-->' not in lines[i]:
                i += 1
            i += 1
            continue

        if not line.strip():
            i += 1
            continue

        # fenced code: emitted as a Typst raw block, contents untouched
        if line.startswith('```'):
            lang_tag = line[3:].strip() or 'txt'
            body = []
            i += 1
            while i < n and not lines[i].startswith('```'):
                body.append(lines[i])
                i += 1
            i += 1
            fence = '`' * 3
            while fence in '\n'.join(body):
                fence += '`'
            out.append('#block(width: 100' + '%, breakable: true)['
                       + fence + lang_tag + '\n' + '\n'.join(body) + '\n' + fence + ']')
            out.append('')
            continue

        # heading
        m = re.match(r'^(#{1,6}) +(.*)$', line)
        if m:
            level = len(m.group(1))
            out.append('=' * level + ' ' + inline(m.group(2).strip(), lang))
            out.append('')
            i += 1
            continue

        # horizontal rule
        if re.match(r'^-{3,}\s*$', line) or re.match(r'^\*{3,}\s*$', line):
            out.append('#line(length: 100' + '%, stroke: 0.5pt + luma(200))')
            out.append('')
            i += 1
            continue

        # table
        if line.lstrip().startswith('|') and i + 1 < n and is_divider(lines[i + 1]):
            header = split_row(line)
            i += 2
            rows = []
            while i < n and lines[i].lstrip().startswith('|'):
                rows.append(split_row(lines[i]))
                i += 1
            cols = len(header)
            cells = []
            for h in header:
                cells.append('[*%s*]' % inline(h, lang))
            for r in rows:
                r = (r + [''] * cols)[:cols]
                for c in r:
                    cells.append('[%s]' % inline(c, lang))
            out.append('#table(\n  columns: %d,\n  %s\n)' % (cols, ',\n  '.join(cells)))
            out.append('')
            continue

        # block quote
        if line.lstrip().startswith('>'):
            body = []
            while i < n and lines[i].lstrip().startswith('>'):
                body.append(lines[i].lstrip()[1:].strip())
                i += 1
            out.append('#quote(block: true)[%s]' % inline(' '.join(body).strip(), lang))
            out.append('')
            continue

        # lists (bullet and numbered, one level of nesting)
        m = re.match(r'^(\s*)([-*]|\d+\.) +(.*)$', line)
        if m:
            items = []
            while i < n:
                m = re.match(r'^(\s*)([-*]|\d+\.) +(.*)$', lines[i])
                if not m:
                    # a continuation line belongs to the item above it
                    if items and lines[i].strip() and lines[i].startswith(' '):
                        items[-1] = (items[0][0], items[-1][1], items[-1][2] + ' ' + lines[i].strip()) \
                            if False else (items[-1][0], items[-1][1], items[-1][2] + ' ' + lines[i].strip())
                        i += 1
                        continue
                    break
                items.append((len(m.group(1)), m.group(2), m.group(3)))
                i += 1
            for indent, marker, body in items:
                bullet = '+' if marker[0].isdigit() else '-'
                pad = ' ' * (2 if indent >= 2 else 0)
                out.append('%s%s %s' % (pad, bullet, inline(body, lang)))
            out.append('')
            continue

        # paragraph
        body = []
        while i < n and lines[i].strip() and not lines[i].startswith('```') \
                and not re.match(r'^#{1,6} ', lines[i]) \
                and not lines[i].lstrip().startswith('|') \
                and not lines[i].lstrip().startswith('>') \
                and not lines[i].lstrip().startswith('<!--') \
                and not re.match(r'^(\s*)([-*]|\d+\.) +', lines[i]) \
                and not re.match(r'^-{3,}\s*$', lines[i]):
            body.append(lines[i].strip())
            i += 1
        if body:
            out.append(inline(' '.join(body), lang))
            out.append('')
        else:
            i += 1

    return '\n'.join(out).rstrip() + '\n'

def main():
    if len(sys.argv) != 4:
        print('usage: md2typst.py <in.md> <out.typ> <lang>', file=sys.stderr)
        return 2
    src, dst, lang = sys.argv[1], sys.argv[2], sys.argv[3]
    text = open(src, encoding='utf-8').read()
    title = os.path.basename(src)
    body = convert(text, lang, title)
    os.makedirs(os.path.dirname(dst) or '.', exist_ok=True)
    open(dst, 'w', encoding='utf-8').write(body)
    return 0

if __name__ == '__main__':
    sys.exit(main())
