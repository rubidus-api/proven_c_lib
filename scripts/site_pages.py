#!/usr/bin/env python3
"""Wrap Typst's HTML fragments into the published manual pages.

Typst's HTML export gives correct, plain markup — headings, tables, highlighted code — and
nothing else. Everything a manual *site* needs is added here, following the design
proven_c_book arrived at (scripts/wrap-html.py):

  - a sticky bar carrying the manual's name, where you are, and previous/contents/next, so a
    long chapter does not have to be scrolled to the bottom to leave it;
  - a panel under it holding the version, the reading settings, the language switch, the PDF
    and the repository, and this chapter's own contents;
  - reading settings kept in the browser (localStorage) and applied in <head>, before the body
    is painted, so a chosen dark theme does not flash white first;
  - heading anchors following GitHub's rule, because the Markdown chapters link to each other
    with GitHub anchors and those links have to keep working.

One thing is this manual's own: the search index is every public function, not just headings.
A reader of a reference arrives holding a function name, and the answer they want is the
section that explains it.
"""
import html
import json
import os
import re
import sys
import unicodedata

TITLES = {
    'en': {
        'index': 'The manual',
        'manual-t-tutorial': 'T · Tutorial',
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
        'manual-t-tutorial': 'T · 따라 하며 익히기',
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

# Reading order. The formatting reference sits next to the chapter that introduces it, and the
# alias index last, because it is a lookup table rather than something anyone reads through.
ORDER = ['index', 'manual-t-tutorial', 'manual-00-start-here', 'manual-01-foundation', 'manual-02-allocation',
         'manual-03-strings-text', 'manual-04-containers-algorithms', 'manual-08-fmt-scan',
         'manual-05-hosted-services', 'manual-06-execution-and-platform',
         'manual-freestanding', 'manual-07-alias-xcv-index']

STR = {
    'en': dict(short='proven_c_lib manual', contents='Contents', here='On this page',
               prev='Previous', next='Next', top='Contents', menu='Menu', settings='Settings',
               set_width='Limit line width', set_theme='Theme', th_auto='System',
               th_light='Light', th_dark='Dark', set_colors='Colours', c_fg='Text',
               c_bg='Background', c_link='Link',
               set_note='Settings are kept in this browser only.',
               pdf='PDF', other='한국어', search='Search',
               search_ph='Find a function or a heading',
               search_none='Nothing found',
               search_hint='Searches every public function and every heading — not the full text.',
               source='generated from the Markdown manual', skip='Skip to content',
               repo='Repo', repo_t='This project on GitHub',
               home='All projects', home_t='rubidus-api.github.io — every published project',
               repos='All repos', repos_t='github.com/rubidus-api — every repository'),
    'ko': dict(short='proven_c_lib 매뉴얼', contents='목차', here='이 페이지 안에서',
               prev='이전', next='다음', top='목차', menu='메뉴', settings='설정',
               set_width='가로폭 제한', set_theme='테마', th_auto='시스템',
               th_light='밝게', th_dark='어둡게', set_colors='색', c_fg='글자',
               c_bg='배경', c_link='링크',
               set_note='설정은 이 브라우저에만 저장됩니다.',
               pdf='PDF', other='English', search='검색',
               search_ph='함수나 제목으로 찾기',
               search_none='찾은 것이 없습니다',
               search_hint='공개 함수 전부와 모든 제목에서 찾습니다 — 본문 전문은 아닙니다.',
               source='마크다운 매뉴얼에서 생성됨', skip='본문으로 건너뛰기',
               repo='저장소', repo_t='이 프로젝트의 GitHub 저장소',
               home='전체 사이트', home_t='rubidus-api.github.io — 공개된 모든 프로젝트',
               repos='저장소 목록', repos_t='github.com/rubidus-api — 모든 저장소'),
}

REPO = 'https://github.com/rubidus-api/proven_c_lib'
SITE_ROOT = 'https://rubidus-api.github.io/'
REPOS_ROOT = 'https://github.com/rubidus-api'

# Pages that mention a call while explaining something else. A search hit belongs in the
# chapter that owns the call, so these lose to any chapter that also uses the name.
LAST_RESORT = ('index', 'manual-00-start-here')

# Applied in <head>, before the body is painted. A theme applied afterwards shows the light
# page first and then repaints - the flash of unstyled content the book documents.
APPLY_JS = (
    "(function(){try{var s=JSON.parse(localStorage.getItem('pcl-read')||'{}');"
    "var r=document.documentElement;"
    "if(s.theme&&s.theme!=='auto')r.setAttribute('data-theme',s.theme);"
    "if(s.fg)r.style.setProperty('--fg',s.fg);"
    "if(s.bg)r.style.setProperty('--bg',s.bg);"
    "if(s.link)r.style.setProperty('--link',s.link);"
    "if(s.width)r.setAttribute('data-measure','on');"
    "}catch(e){}})();"
)

PANEL_JS = (
    "(function(){"
    "var b=document.querySelector('.here-btn'),p=document.getElementById('here-panel');"
    "function open(o){if(!p)return;p.hidden=!o;if(b)b.setAttribute('aria-expanded',o);}"
    "if(b)b.addEventListener('click',function(){open(p.hidden);});"
    "if(p)p.addEventListener('click',function(e){"
    "if(e.target.tagName==='A'&&(e.target.getAttribute('href')||'').charAt(0)==='#')open(false);});"
    "document.addEventListener('keydown',function(e){if(e.key==='Escape')open(false);});"
    "var box=document.getElementById('setbox'),so=document.getElementById('set-open');"
    "if(!box||!so)return;var r=document.documentElement;"
    "function load(){try{return JSON.parse(localStorage.getItem('pcl-read')||'{}');}"
    "catch(e){return {};}}"
    "function save(s){try{localStorage.setItem('pcl-read',JSON.stringify(s));}catch(e){}}"
    "function css(n){return getComputedStyle(r).getPropertyValue(n).trim();}"
    "function hex(v){var m=v.match(/^rgba?\\((\\d+)[ ,]+(\\d+)[ ,]+(\\d+)/);"
    "if(!m)return v.charAt(0)==='#'?v:'#000000';"
    "return '#'+[1,2,3].map(function(i){"
    "return ('0'+parseInt(m[i],10).toString(16)).slice(-2);}).join('');}"
    "function paint(){var s=load();"
    "document.getElementById('s-width').checked=!!s.width;"
    "var t=s.theme||'auto';"
    "[].forEach.call(box.querySelectorAll('.segb'),function(x){"
    "x.setAttribute('aria-pressed',x.dataset.theme===t);});"
    "document.getElementById('s-fg').value=s.fg||hex(css('--fg'));"
    "document.getElementById('s-bg').value=s.bg||hex(css('--bg'));"
    "document.getElementById('s-link').value=s.link||hex(css('--link'));}"
    "so.addEventListener('click',function(){box.hidden=!box.hidden;"
    "so.setAttribute('aria-expanded',!box.hidden);if(!box.hidden)paint();});"
    "document.getElementById('s-width').addEventListener('change',function(){"
    "var s=load();s.width=this.checked;save(s);"
    "if(s.width)r.setAttribute('data-measure','on');else r.removeAttribute('data-measure');});"
    # Choosing a theme is also letting go of hand-picked colours: leaving them set would
    # override the theme, and the button would appear to do nothing.
    "[].forEach.call(box.querySelectorAll('.segb'),function(x){"
    "x.addEventListener('click',function(){var s=load();s.theme=x.dataset.theme;"
    "delete s.fg;delete s.bg;delete s.link;save(s);"
    "['--fg','--bg','--link'].forEach(function(n){r.style.removeProperty(n);});"
    "if(s.theme==='auto')r.removeAttribute('data-theme');"
    "else r.setAttribute('data-theme',s.theme);setTimeout(paint,0);});});"
    "[['s-fg','--fg','fg'],['s-bg','--bg','bg'],['s-link','--link','link']]"
    ".forEach(function(a){document.getElementById(a[0]).addEventListener('input',"
    "function(){var s=load();s[a[2]]=this.value;save(s);"
    "r.style.setProperty(a[1],this.value);});});"
    "})();"
)

# The index is fetched on the first keystroke, not on load: a reader who never searches never
# pays for it.
SEARCH_JS = """
(function(){
  var q=document.getElementById('q'), out=document.getElementById('qr');
  if(!q||!out) return;
  var data=null, NONE=__NONE__;
  function esc(s){var d=document.createElement('div');d.textContent=s;return d.innerHTML;}
  function run(){
    var v=q.value.trim().toLowerCase();
    out.innerHTML='';
    if(!v||!data) return;
    var exact=[], starts=[], rest=[];
    for(var i=0;i<data.length;i++){
      var name=data[i][0].toLowerCase(), at=name.indexOf(v);
      if(at<0) continue;
      if(name===v) exact.push(data[i]);
      else if(at===0) starts.push(data[i]);
      else rest.push(data[i]);
    }
    var hits=exact.concat(starts,rest).slice(0,40);
    if(!hits.length){ out.innerHTML='<li>'+NONE+'</li>'; return; }
    out.innerHTML=hits.map(function(h){
      var label=h[3]==='fn' ? '<code>'+esc(h[0])+'</code>' : esc(h[0]);
      return '<li><a href="'+h[1]+'">'+label+'</a>'+
             (h[2]?'<span class="where">'+esc(h[2])+'</span>':'')+'</li>';
    }).join('');
  }
  q.addEventListener('input',function(){
    if(data){ run(); return; }
    fetch('search-index.json').then(function(r){return r.json();})
      .then(function(j){ data=j; run(); })
      .catch(function(){ out.innerHTML='<li>'+NONE+'</li>'; });
  });
})();
"""


def slug(text):
    """GitHub's heading-anchor rule, which the Markdown cross-references already use."""
    text = unicodedata.normalize('NFC', text)
    text = re.sub(r'<[^>]+>', '', text)
    text = html.unescape(text).strip().lower()
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
    # Each space becomes one dash; GitHub does not collapse runs, so a heading written with an
    # em dash keeps the double dash its Markdown links already carry.
    return ''.join(kept).replace(' ', '-')


def body_of(fragment):
    m = re.search(r'<body>(.*)</body>', fragment, re.S)
    body = m.group(1) if m else fragment
    # Typst reserves <h1> for a document title it does not emit, so a chapter's own title
    # arrives as <h2>. Shift every level up by one, giving the page exactly one <h1>.
    def up(m2):
        return '<%sh%d%s' % (m2.group(1), max(1, int(m2.group(2)) - 1), m2.group(3))
    body = re.sub(r'<(/?)h([1-6])(>)', up, body)
    # A table that does not fit has to scroll inside its own box rather than widening the page.
    return body.replace('<table>', '<div class="tblwrap"><table>').replace('</table>', '</table></div>')


HEADING = re.compile(r'<h([1-6])>(.*?)</h\1>', re.S)


def add_anchors(body):
    outline, seen = [], {}

    def repl(m):
        level, inner = int(m.group(1)), m.group(2)
        s = slug(inner)
        if s in seen:
            seen[s] += 1
            s = '%s-%d' % (s, seen[s])
        else:
            seen[s] = 0
        outline.append((level, s, re.sub(r'<[^>]+>', '', inner)))
        return ('<h%d id="%s">%s<a class="anchor" href="#%s" aria-hidden="true">#</a></h%d>'
                % (level, s, inner, s, level))

    return HEADING.sub(repl, body), outline


def public_symbols(root):
    """The names worth indexing: every function and function-like macro a public header
    declares. Reading them from the headers rather than scraping the pages is what keeps a
    type (`proven_err_t`) and a truncated fragment out of a search for a *call*."""
    names = set()
    hdr = os.path.join(root, 'include', 'proven')
    if not os.path.isdir(hdr):
        return names
    for f in sorted(os.listdir(hdr)):
        if not f.endswith('.h'):
            continue
        text = open(os.path.join(hdr, f), encoding='utf-8', errors='replace').read()
        for m in re.finditer(r'\b(proven_[A-Za-z0-9_]*)\(', text):
            names.add(m.group(1))
        for m in re.finditer(r'^#define\s+(PROVEN_[A-Za-z0-9_]*)\(', text, re.M):
            names.add(m.group(1))
    return names


def search_index(lang, pages, bodies, outlines, symbols):
    """Every public function, and every heading, each pointing at the section that explains it.

    A function is placed at the nearest heading above the place it is used. Two rules decide
    *which* use, and both come from what a reader is actually asking:

      - The spine and the introduction are the last resort, not the first. They name functions
        while explaining the ownership rules and the glossary; a reader searching for
        `proven_fs_sync_dir` wants the filesystem chapter, not the glossary entry for
        durability that happens to cite it three times.
      - Among the chapters, the one that uses the name most often wins, because that is the
        chapter the call belongs to. Taking the first use instead sent `proven_arena_*` to the
        chapter that mentions arenas while explaining panics.

    Within the chosen chapter it is the first use that is linked, since a chapter introduces a
    call before it lists the call again in a reference table.
    """
    entries = []
    for name in pages:
        for level, anchor, text in outlines[name]:
            if level <= 3:
                entries.append([text, '%s.html#%s' % (name, anchor), TITLES[lang][name], 'head'])

    # First pass: where each name appears, and how often, per page.
    hits = {}          # fn -> page -> (count, first-position)
    heads_of = {}
    for name in pages:
        body = bodies[name]
        heads_of[name] = [(m.start(), m.group(1), m.group(2))
                          for m in re.finditer(
                              r'<h[1-6] id="([^"]+)"[^>]*>(.*?)<a class="anchor"', body, re.S)]
        for m in re.finditer(r'<code[^>]*>((?:proven_|PROVEN_)[A-Za-z0-9_]+)', body):
            fn = m.group(1)
            if fn not in symbols:
                continue
            per_page = hits.setdefault(fn, {})
            count, first = per_page.get(name, (0, m.start()))
            per_page[name] = (count + 1, min(first, m.start()))

    # Second pass: pick the chapter, then the section inside it.
    for fn in sorted(hits):
        per_page = hits[fn]
        name = max(per_page, key=lambda p: (p not in LAST_RESORT, per_page[p][0], -pages.index(p)))
        first = per_page[name][1]
        anchor, where = None, TITLES[lang][name]
        for pos, a, head_text in heads_of[name]:
            if pos > first:
                break
            anchor, where = a, re.sub(r'<[^>]+>', '', head_text).strip()
        href = '%s.html#%s' % (name, anchor) if anchor else '%s.html' % name
        entries.append([fn, href, '%s · %s' % (TITLES[lang][name], where), 'fn'])
    return entries


def bar(lang, name, pages, version):
    s = STR[lang]
    i = pages.index(name)
    prev = pages[i - 1] if i > 0 else None
    nxt = pages[i + 1] if i + 1 < len(pages) else None
    parts = ['<span class="bar-nav">']
    parts.append('<a href="%s.html" title="%s" aria-label="%s">←</a>' % (prev, s['prev'], s['prev'])
                 if prev else '<span class="off" aria-hidden="true">←</span>')
    parts.append('<a href="index.html" title="%s" aria-label="%s">↑</a>' % (s['top'], s['top']))
    parts.append('<a href="%s.html" title="%s" aria-label="%s">→</a>' % (nxt, s['next'], s['next'])
                 if nxt else '<span class="off" aria-hidden="true">→</span>')
    parts.append('</span>')
    btn = ('<button class="here-btn" type="button" aria-expanded="false" aria-controls="here-panel">'
           '%s <span class="caret">▾</span></button>' % html.escape(TITLES[lang][name]))
    return ('<div class="bar"><strong><a href="index.html">%s</a></strong>%s%s</div>'
            % (html.escape(s['short']), btn, ''.join(parts)))


def panel(lang, name, outline, version, pdf_url):
    s = STR[lang]
    other = 'ko' if lang == 'en' else 'en'
    tools = (
        '<span class="ver">%s</span>'
        '<button class="tool" type="button" id="set-open" aria-expanded="false">⚙ %s</button>'
        '<a class="tool" href="../%s/%s.html">%s</a>'
        '<a class="tool" href="%s">⤓ %s</a>'
        '<a class="tool" href="%s" title="%s">⌥ %s</a>'
        '<a class="tool" href="%s" title="%s">⌂ %s</a>'
        '<a class="tool" href="%s" title="%s">≡ %s</a>'
        % (html.escape(version), s['settings'], other, name, s['other'], pdf_url, s['pdf'],
           REPO, s['repo_t'], s['repo'],
           SITE_ROOT, s['home_t'], s['home'],
           REPOS_ROOT, s['repos_t'], s['repos']))
    settings = (
        '<div class="setbox" id="setbox" hidden>'
        '<label class="setrow"><input type="checkbox" id="s-width"><span>%s</span></label>'
        '<div class="setrow"><span class="setlbl">%s</span><span class="seg">'
        '<button type="button" class="segb" data-theme="auto">%s</button>'
        '<button type="button" class="segb" data-theme="light">%s</button>'
        '<button type="button" class="segb" data-theme="dark">%s</button>'
        '</span></div>'
        '<div class="setrow"><span class="setlbl">%s</span>'
        '<label class="col"><span>%s</span><input type="color" id="s-fg"></label>'
        '<label class="col"><span>%s</span><input type="color" id="s-bg"></label>'
        '<label class="col"><span>%s</span><input type="color" id="s-link"></label></div>'
        '<p class="setnote">%s</p></div>'
        % (s['set_width'], s['set_theme'], s['th_auto'], s['th_light'], s['th_dark'],
           s['set_colors'], s['c_fg'], s['c_bg'], s['c_link'], s['set_note']))
    search = ('<div class="search"><label class="setlbl" for="q">%s</label> '
              '<input type="search" id="q" placeholder="%s" autocomplete="off">'
              '<p class="hint">%s</p><ul id="qr"></ul></div>'
              % (s['search'], html.escape(s['search_ph']), s['search_hint']))
    rows = ''.join('<a href="#%s">%s</a>' % (a, html.escape(t))
                   for lv, a, t in outline if lv == 2)
    here = ('<div class="here-head"><strong>%s</strong></div><div class="here-body">%s</div>'
            % (s['here'], rows)) if rows else ''
    return ('<div id="here-panel" class="here-panel" hidden><div class="tools">%s</div>%s%s%s</div>'
            % (tools, settings, search, here))


def full_contents(lang, pages, outlines):
    """index 한 쪽에 실리는 *전체* 목차.

    ★ 예전에는 이 목록이 장마다 왼쪽에 붙어 있었다(저자 지시로 걷었다). 목차는 한 자리에
      두고, 각 장은 본문만 싣는다 --- 읽는 자리에서 눈이 갈 곳을 하나로 만든다.
      장 안에서 절로 뛰는 길은 위쪽 ☰ 패널의 「이 쪽에서」가 그대로 해 준다.
    """
    s = STR[lang]
    out = ['<nav class="contents" aria-label="%s">' % s['contents']]
    for p in pages:
        if p == 'index':
            continue
        out.append('<h2><a href="%s.html">%s</a></h2>' % (p, html.escape(TITLES[lang][p])))
        rows = [o for o in outlines.get(p, []) if o[0] == 2]
        if rows:
            out.append('<ul>')
            for _lv, a, tx in rows:
                out.append('<li><a href="%s.html#%s">%s</a></li>' % (p, a, html.escape(tx)))
            out.append('</ul>')
    out.append('</nav>')
    return '\n'.join(out)


def page_nav(lang, pages, name):
    s = STR[lang]
    i = pages.index(name)
    left = ('<a href="%s.html">← %s</a>' % (pages[i - 1], html.escape(TITLES[lang][pages[i - 1]]))
            if i > 0 else '<span></span>')
    right = ('<a href="%s.html">%s →</a>' % (pages[i + 1], html.escape(TITLES[lang][pages[i + 1]]))
             if i + 1 < len(pages) else '<span></span>')
    return '<div class="nav">%s%s</div>' % (left, right)


def build(lang, srcdir, outdir, version, pdf_url, root):
    s = STR[lang]
    pages = [n for n in ORDER if os.path.exists(os.path.join(srcdir, n + '.html'))]
    bodies, outlines = {}, {}
    for name in pages:
        fragment = open(os.path.join(srcdir, name + '.html'), encoding='utf-8').read()
        bodies[name], outlines[name] = add_anchors(body_of(fragment))

    # The index is the front door, and it carries nothing but the contents and the copyright.
    # Its Markdown lists the chapters so that a reader on GitHub has a contents too; on the web
    # that list is replaced by the *full* contents - every chapter and every section - which
    # only this step, having read every chapter, can write.
    if 'index' in pages:
        h2 = list(re.finditer(r'<h2 id="([^"]+)">', bodies['index']))
        if len(h2) >= 2:
            dropped = h2[0].group(1)
            bodies['index'] = (bodies['index'][:h2[0].start()]
                               + full_contents(lang, pages, outlines)
                               + '\n' + bodies['index'][h2[1].start():])
            outlines['index'] = [o for o in outlines['index'] if o[1] != dropped]

    search_js = SEARCH_JS.replace('__NONE__', json.dumps(s['search_none']))
    for name in pages:
        title = TITLES[lang][name]
        doc = f"""<!doctype html>
<html lang="{lang}">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{html.escape(title)} — {html.escape(s['short'])}</title>
<link rel="stylesheet" href="manual.css">
<script>{APPLY_JS}</script>
</head>
<body>
<a class="skip" href="#content">{s['skip']}</a>
{bar(lang, name, pages, version)}
{panel(lang, name, outlines[name], version, pdf_url)}
<div class="page">
<main id="content"><div class="content">
<div class="masthead">{html.escape(version)} · {s['source']}</div>
{bodies[name]}
{page_nav(lang, pages, name)}
</div></main>
</div>
<script>{PANEL_JS}</script>
<script>{search_js}</script>
</body>
</html>
"""
        open(os.path.join(outdir, name + '.html'), 'w', encoding='utf-8').write(doc)

    index = search_index(lang, pages, bodies, outlines, public_symbols(root))
    with open(os.path.join(outdir, 'search-index.json'), 'w', encoding='utf-8') as f:
        json.dump(index, f, ensure_ascii=False, separators=(',', ':'))

    # The same contents, as data, for the landing page one level up (site_root.py). It goes
    # beside the fragments - in the build tree, not the site - because it is an input to the
    # next step, not something a reader fetches. The index page's own sections are included:
    # the landing page has no "on this page" panel to carry them, so here they have to be
    # listed like everyone else's.
    contents = dict(lang=lang, pages=[
        dict(name=n, title=TITLES[lang][n],
             sections=[[a, t] for lv, a, t in outlines[n] if lv == 2])
        for n in pages])
    with open(os.path.join(srcdir, 'contents.json'), 'w', encoding='utf-8') as f:
        json.dump(contents, f, ensure_ascii=False, indent=1)
    return pages, index


def main():
    if len(sys.argv) not in (5, 6):
        print('usage: site_pages.py <lang> <fragment-dir> <out-dir> <version> [pdf-url]',
              file=sys.stderr)
        return 2
    lang, srcdir, outdir, version = sys.argv[1:5]
    pdf_url = sys.argv[5] if len(sys.argv) == 6 else 'manual.pdf'
    os.makedirs(outdir, exist_ok=True)
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    pages, index = build(lang, srcdir, outdir, version, pdf_url, root)
    fns = sum(1 for e in index if e[3] == 'fn')
    print('%s: %d page(s), %d search entries (%d functions)'
          % (lang, len(pages), len(index), fns))
    return 0


if __name__ == '__main__':
    sys.exit(main())
