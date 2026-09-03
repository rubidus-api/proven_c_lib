#!/usr/bin/env python3
"""The landing page of the published manual: docs/index.html.

It is the front door, and a front door should show the whole house. So instead of two links
that say "English" and "한국어" and nothing else, it carries the complete table of contents of
both editions - every chapter and every section - so a reader can land here and go straight
to the paragraph they want, in the language they want, without first choosing a language and
then finding the contents again.

The contents come from the per-language build (site_pages.py writes contents.json beside the
fragments); this script only lays them out. An edition whose contents file is missing - a
single-language build in a fresh tree - is still linked, just without its contents.

    site_root.py <out-dir> <version> <en contents.json> <ko contents.json>
"""
import html
import json
import os
import sys


STR = {
    'en': dict(name='English', contents='Contents', pdf='PDF', read='Read the English manual'),
    'ko': dict(name='한국어', contents='목차', pdf='PDF', read='한국어 매뉴얼 읽기'),
}
ORDER = ['en', 'ko']


def load(path):
    if not path or not os.path.exists(path):
        return None
    return json.load(open(path, encoding='utf-8'))


def contents(lang, data):
    """One edition's full contents, links pointing into that edition's directory."""
    s = STR[lang]
    out = ['<nav class="contents" aria-label="%s — %s">' % (s['name'], s['contents'])]
    for p in data['pages']:
        name = p['name']
        out.append('<h2><a href="%s/%s.html">%s</a></h2>' % (lang, name, html.escape(p['title'])))
        if p['sections']:
            out.append('<ul>')
            for anchor, text in p['sections']:
                out.append('<li><a href="%s/%s.html#%s">%s</a></li>'
                           % (lang, name, anchor, html.escape(text)))
            out.append('</ul>')
    out.append('</nav>')
    return '\n'.join(out)


def edition(lang, data):
    s = STR[lang]
    head = ('<section class="edition" id="%s" lang="%s">'
            '<h2 class="edition-title">%s'
            '<span class="edition-links"><a href="%s/index.html">%s</a>'
            ' · <a href="%s/manual.pdf">%s</a></span></h2>'
            % (lang, lang, s['name'], lang, s['read'], lang, s['pdf']))
    body = contents(lang, data) if data else ''
    return head + '\n' + body + '\n</section>'


def page(version, editions):
    switch = ' · '.join('<a href="#%s">%s</a>' % (lang, STR[lang]['name']) for lang in ORDER)
    return f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>proven_c_lib — manual</title>
<link rel="stylesheet" href="en/manual.css">
<style>
/* Landing-page only: two editions on one page, each a full table of contents. */
.edition {{ margin-top:2.6rem; }}
.edition-title {{ display:flex; flex-wrap:wrap; align-items:baseline; gap:.4rem 1.2rem; }}
.edition-links {{ font-size:.85rem; font-weight:normal; }}
.switch {{ margin:.2rem 0 0; color:var(--muted); }}
nav.contents {{ margin-top:1.2rem; }}
</style>
</head>
<body>
<div class="page"><main id="content"><div class="content">
<h1>proven_c_lib</h1>
<div class="masthead">{html.escape(version)} · the manual, generated from the Markdown sources in the repository</div>
<p class="switch">{switch}</p>
{editions}
</div></main></div>
</body>
</html>
"""


def main():
    if len(sys.argv) != 5:
        print('usage: site_root.py <out-dir> <version> <en contents.json> <ko contents.json>',
              file=sys.stderr)
        return 2
    outdir, version = sys.argv[1], sys.argv[2]
    files = dict(zip(ORDER, sys.argv[3:5]))
    editions, n = [], 0
    for lang in ORDER:
        data = load(files[lang])
        editions.append(edition(lang, data))
        if data:
            n += sum(1 + len(p['sections']) for p in data['pages'])
        else:
            print('site_root: no contents for %s (%s) - linked without them'
                  % (lang, files[lang]), file=sys.stderr)
    with open(os.path.join(outdir, 'index.html'), 'w', encoding='utf-8') as f:
        f.write(page(version, '\n'.join(editions)))
    print('site_root: docs/index.html, %d contents entries' % n)
    return 0


if __name__ == '__main__':
    sys.exit(main())
