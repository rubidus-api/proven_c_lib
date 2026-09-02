#!/usr/bin/env python3
"""Wrap Typst's HTML fragments into the published manual pages.

Typst's HTML export gives correct, plain markup: headings, tables, highlighted
code. What it does not give is the things a manual *site* needs — heading
anchors, a table of contents, navigation between chapters, a language switch, or
a stylesheet. That is this script's whole job, so that the Typst step stays
responsible for the content and nothing else.

Heading ids follow GitHub's rule, because the Markdown sources link to each other
with GitHub anchors (`#5-alignment`) and those links have to keep working.
"""
import html
import os
import re
import sys
import unicodedata

TITLES = {
    'en': {
        'index': 'The manual',
        'manual-00-start-here': '0 · Start here',
        'manual-01-foundation': '1 · Foundation',
        'manual-02-allocation': '2 · Allocation',
        'manual-03-strings-text': '3 · Strings and text',
        'manual-04-containers-algorithms': '4 · Containers and algorithms',
        'manual-05-hosted-services': '5 · Hosted services',
        'manual-06-execution-and-platform': '6 · Execution and platform',
        'manual-07-alias-xcv-index': 'A · Alias index',
        'manual-08-fmt-scan': '8 · Formatting and scanning',
        'manual-freestanding': 'Freestanding',
    },
    'ko': {
        'index': '매뉴얼',
        'manual-00-start-here': '0 · 여기서부터',
        'manual-01-foundation': '1 · 기초',
        'manual-02-allocation': '2 · 할당',
        'manual-03-strings-text': '3 · 문자열과 텍스트',
        'manual-04-containers-algorithms': '4 · 컨테이너와 알고리즘',
        'manual-05-hosted-services': '5 · Hosted 서비스',
        'manual-06-execution-and-platform': '6 · 실행과 플랫폼',
        'manual-07-alias-xcv-index': '부록 A · Alias 인덱스',
        'manual-08-fmt-scan': '8 · 형식화와 파싱',
        'manual-freestanding': '프리스탠딩',
    },
}

ORDER = ['index', 'manual-00-start-here', 'manual-01-foundation', 'manual-02-allocation',
         'manual-03-strings-text', 'manual-04-containers-algorithms', 'manual-08-fmt-scan',
         'manual-05-hosted-services', 'manual-06-execution-and-platform',
         'manual-freestanding', 'manual-07-alias-xcv-index']

STRINGS = {
    'en': {'contents': 'Contents', 'on_this_page': 'On this page', 'prev': 'Previous',
           'next': 'Next', 'lang': 'Language', 'source': 'Generated from the Markdown manual',
           'pdf': 'Download PDF'},
    'ko': {'contents': '목차', 'on_this_page': '이 페이지 안에서', 'prev': '이전',
           'next': '다음', 'lang': '언어', 'source': '마크다운 매뉴얼에서 생성됨',
           'pdf': 'PDF 내려받기'},
}


def slug(text):
    """GitHub's heading-anchor rule: lowercase, drop punctuation, spaces to dashes."""
    text = unicodedata.normalize('NFC', text)
    text = re.sub(r'<[^>]+>', '', text)
    text = html.unescape(text).strip().lower()
    # Drop punctuation and symbols, keep letters, digits, marks and the dash/underscore.
    # Doing it by Unicode category rather than by an ASCII range is what keeps Hangul and
    # CJK headings intact while still removing an em dash.
    kept = []
    for ch in text:
        if ch in '-_':
            kept.append(ch)
        elif ch.isspace():
            kept.append(' ')
        elif unicodedata.category(ch)[0] in 'PS':
            continue
        else:
            kept.append(ch)
    # Each space becomes one dash. GitHub does not collapse runs, so a heading written with
    # an em dash - "state - no destroy" - keeps the double dash its Markdown links already use.
    return ''.join(kept).replace(' ', '-')


def body_of(fragment):
    m = re.search(r'<body>(.*)</body>', fragment, re.S)
    body = m.group(1) if m else fragment
    # Typst reserves <h1> for a document title it does not emit, so a chapter's own title
    # arrives as <h2>. Shift every level up by one so the page has exactly one <h1> and the
    # outline a reader's browser (and a screen reader) sees matches the Markdown's structure.
    def up(m2):
        level = max(1, int(m2.group(2)) - 1)
        return '<%sh%d%s' % (m2.group(1), level, m2.group(3))
    return re.sub(r'<(/?)h([1-6])(>)', up, body)


HEADING = re.compile(r'<h([1-6])>(.*?)</h\1>', re.S)


def add_anchors(body):
    """Give every heading a GitHub-shaped id, and collect the page's own outline."""
    outline = []
    seen = {}

    def repl(m):
        level, inner = int(m.group(1)), m.group(2)
        s = slug(inner)
        if s in seen:
            seen[s] += 1
            s = '%s-%d' % (s, seen[s])
        else:
            seen[s] = 0
        text = re.sub(r'<[^>]+>', '', inner)
        outline.append((level, s, text))
        return ('<h%d id="%s">%s<a class="anchor" href="#%s">#</a></h%d>'
                % (level, s, inner, s, level))

    return HEADING.sub(repl, body), outline


def nav(lang, current, outline, pages):
    s = STRINGS[lang]
    other = 'ko' if lang == 'en' else 'en'
    out = ['<nav class="toc">']
    out.append('<div class="lang">%s: <a href="../%s/%s.html">%s</a> · <a href="manual.pdf">%s</a></div>'
               % (s['lang'], other, current, other.upper(), s['pdf']))
    out.append('<h2>%s</h2>' % s['contents'])
    for name in pages:
        cls = ' class="here"' if name == current else ''
        out.append('<a href="%s.html"%s>%s</a>' % (name, cls, html.escape(TITLES[lang][name])))
    inner = [o for o in outline if o[0] == 2]
    if inner:
        out.append('<h2>%s</h2>' % s['on_this_page'])
        for _level, anchor, text in inner:
            out.append('<a href="#%s">%s</a>' % (anchor, html.escape(text)))
    out.append('</nav>')
    return '\n'.join(out)


def page_nav(lang, pages, current):
    s = STRINGS[lang]
    i = pages.index(current)
    left = ('<a href="%s.html">← %s: %s</a>' % (pages[i - 1], s['prev'], html.escape(TITLES[lang][pages[i - 1]]))
            if i > 0 else '<span></span>')
    right = ('<a href="%s.html">%s: %s →</a>' % (pages[i + 1], s['next'], html.escape(TITLES[lang][pages[i + 1]]))
             if i + 1 < len(pages) else '<span></span>')
    return '<div class="page-nav">%s%s</div>' % (left, right)


def build(lang, srcdir, outdir, version):
    present = [n for n in ORDER if os.path.exists(os.path.join(srcdir, n + '.html'))]
    s = STRINGS[lang]
    for name in present:
        fragment = open(os.path.join(srcdir, name + '.html'), encoding='utf-8').read()
        body, outline = add_anchors(body_of(fragment))
        title = TITLES[lang][name]
        doc = f"""<!DOCTYPE html>
<html lang="{lang}">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{html.escape(title)} — proven_c_lib</title>
<link rel="stylesheet" href="manual.css">
</head>
<body>
<div class="wrap">
{nav(lang, name, outline, present)}
<main>
<div class="masthead">{html.escape(version)} · {s['source']}</div>
{body}
{page_nav(lang, present, name)}
</main>
</div>
</body>
</html>
"""
        open(os.path.join(outdir, name + '.html'), 'w', encoding='utf-8').write(doc)
    return present


def main():
    if len(sys.argv) != 5:
        print('usage: site_pages.py <lang> <fragment-dir> <out-dir> <version>', file=sys.stderr)
        return 2
    lang, srcdir, outdir, version = sys.argv[1:5]
    os.makedirs(outdir, exist_ok=True)
    pages = build(lang, srcdir, outdir, version)
    print('%s: %d page(s)' % (lang, len(pages)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
