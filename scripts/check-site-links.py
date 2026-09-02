#!/usr/bin/env python3
"""Every internal link in the generated manual site must resolve.

The Markdown chapters cross-reference each other by file and by GitHub heading anchor. Those
anchors are regenerated for the published pages, so a heading rewritten in Markdown can silently
turn a working cross-reference into a 404 that only a reader finds. This walks the generated
tree and checks every link that stays inside it.
"""
import os
import re
import sys


SCRIPT = re.compile(r'<script\b[^>]*>.*?</script>', re.S | re.I)


def linkable(text):
    """Every id in the page - not only the headings.

    The skip link points at <main id="content">, so collecting heading ids alone reports a
    link that works perfectly well. What must be excluded instead is <script>: an href built
    inside JavaScript is a string, not a link, and reporting it wastes the reader's attention
    on a link that does not exist.
    """
    return set(re.findall(r'\bid="([^"]+)"', SCRIPT.sub('', text)))


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else 'docs'
    broken, checked = [], 0
    for lang in sorted(os.listdir(root)):
        d = os.path.join(root, lang)
        if not os.path.isdir(d):
            continue
        pages = {n: SCRIPT.sub('', open(os.path.join(d, n), encoding='utf-8').read())
                 for n in os.listdir(d) if n.endswith('.html')}
        ids = {n: linkable(t) for n, t in pages.items()}
        for name, text in sorted(pages.items()):
            for href in re.findall(r'href="([^"]+)"', text):
                if href.startswith(('http://', 'https://', 'mailto:', '..')):
                    continue
                if href.endswith(('.css', '.pdf', '.json', '.woff2')):
                    continue
                checked += 1
                page, _, anchor = href.partition('#')
                page = page or name
                if page not in pages:
                    broken.append('%s/%s -> %s (no such page)' % (lang, name, href))
                elif anchor and anchor not in ids[page]:
                    broken.append('%s/%s -> %s (no such heading)' % (lang, name, href))
    for b in broken:
        print('check-site-links: broken: %s' % b, file=sys.stderr)
    print('check-site-links: %d internal link(s), %d broken' % (checked, len(broken)))
    return 1 if broken else 0


if __name__ == '__main__':
    sys.exit(main())
