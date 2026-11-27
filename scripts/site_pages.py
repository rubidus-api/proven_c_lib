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
        'manual-09-networking': '9 · Networking',
        'manual-10-http': '10 · URLs and HTTP messages',
        'manual-11-http-client-server': '11 · HTTP client and server',
        'manual-12-websocket': '12 · WebSocket',
        'manual-13-certificates': '13 · Certificates and trust',
        'manual-14-tls': '14 · TLS',
        'manual-15-event-loop': '15 · The event loop and the event-driven server',
        'manual-16-compression': '16 · Compression',
        'manual-17-http2': '17 · HTTP/2 and HTTP/3',
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
        'manual-09-networking': '9 · 네트워킹',
        'manual-10-http': '10 · URL과 HTTP 메시지',
        'manual-11-http-client-server': '11 · HTTP 클라이언트와 서버',
        'manual-12-websocket': '12 · WebSocket',
        'manual-13-certificates': '13 · 인증서와 신뢰',
        'manual-14-tls': '14 · TLS',
        'manual-15-event-loop': '15 · 이벤트 루프와 이벤트 구동 서버',
        'manual-16-compression': '16 · 압축',
        'manual-17-http2': '17 · HTTP/2와 HTTP/3',
        'manual-freestanding': '프리스탠딩',
    },
}

# Reading order. The formatting reference sits next to the chapter that introduces it, and the
# alias index last, because it is a lookup table rather than something anyone reads through.
ORDER = ['index', 'manual-t-tutorial', 'manual-00-start-here', 'manual-01-foundation', 'manual-02-allocation',
         'manual-03-strings-text', 'manual-04-containers-algorithms', 'manual-08-fmt-scan',
         'manual-05-hosted-services', 'manual-09-networking', 'manual-10-http', 'manual-11-http-client-server', 'manual-12-websocket', 'manual-13-certificates', 'manual-14-tls', 'manual-15-event-loop', 'manual-16-compression', 'manual-17-http2',
         'manual-06-execution-and-platform',
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
               repos='All repos', repos_t='github.com/rubidus-api — every repository',
               back='Back', lang='Language', width='Text width', w960='960',
               wfit='Fit the window', layout='Layout', l_auto='Auto', l_wide='Wide',
               l_narrow='Narrow', site='rubidus-api'),
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
               repos='저장소 목록', repos_t='github.com/rubidus-api — 모든 저장소',
               back='이전', lang='언어', width='본문 폭', w960='960', wfit='창에 맞춤',
               layout='배치', l_auto='자동', l_wide='넓게', l_narrow='좁게',
               site='rubidus-api'),
}

REPO = 'https://github.com/rubidus-api/proven_c_lib'
SITE_ROOT = 'https://rubidus-api.github.io/'
REPOS_ROOT = 'https://github.com/rubidus-api'

# Pages that mention a call while explaining something else. A search hit belongs in the
# chapter that owns the call, so these lose to any chapter that also uses the name.
LAST_RESORT = ('index', 'manual-00-start-here')

# The site's design rules N-W2/N-W4: the menu bar and its settings are the site's (rubidus-api.github.io): the
# same storage key and the same values, so a theme or width chosen on the landing page holds here.
SITE_KEY = 'rubidus-api.site'

# Applied in <head>, before the body is painted. A theme applied afterwards shows the light
# page first and then repaints - the flash of unstyled content the book documents.
APPLY_JS = (
    "(function(){try{var s=JSON.parse(localStorage.getItem('" + SITE_KEY + "')||'{}'),"
    "r=document.documentElement;"
    "if(s.theme==='light'||s.theme==='dark')r.setAttribute('data-theme',s.theme);"
    "if(s.width==='fit')r.setAttribute('data-width','fit');"
    "if(s.layout==='wide'||s.layout==='narrow')r.setAttribute('data-layout',s.layout);"
    "}catch(e){}})();"
)

# The menu (a <details>): its radios show and change the settings; Escape and a click outside
# close it, and so does following a link to a section of this page. The back button goes back
# in the history when there is one, and to the contents otherwise.
PANEL_JS = (
    "(function(){var K='" + SITE_KEY + "',d=document,r=d.documentElement,m=d.querySelector('.menu'),s={};"
    "try{s=JSON.parse(localStorage.getItem(K))||{};}catch(e){}"
    "['theme','width','layout'].forEach(function(g){var v=s[g]||({theme:'system',width:'960',layout:'auto'})[g],"
    "x=d.getElementById(g+'-'+v);if(x)x.checked=true;});"
    "function apply(){var t=s.theme;if(t==='light'||t==='dark')r.setAttribute('data-theme',t);"
    "else r.removeAttribute('data-theme');"
    "if(s.width==='fit')r.setAttribute('data-width','fit');else r.removeAttribute('data-width');"
    "if(s.layout==='wide'||s.layout==='narrow')r.setAttribute('data-layout',s.layout);"
    "else r.removeAttribute('data-layout');}"
    "d.addEventListener('change',function(e){var t=e.target;if(t.type!=='radio'||!m||!m.contains(t))return;"
    "s[t.name]=t.id.slice(t.name.length+1);try{localStorage.setItem(K,JSON.stringify(s));}catch(e){}apply();});"
    "if(!m)return;"
    "d.addEventListener('keydown',function(e){if(e.key==='Escape'&&m.open){m.open=false;"
    "m.querySelector('summary').focus();}});"
    "d.addEventListener('click',function(e){if(m.open&&!m.contains(e.target))m.open=false;});"
    "m.addEventListener('click',function(e){var a=e.target.closest('a');"
    "if(a&&(a.getAttribute('href')||'').charAt(0)==='#')m.open=false;});"
    "var b=d.querySelector('.bar .back');if(b)b.addEventListener('click',function(e){"
    "if(history.length>1){e.preventDefault();history.back();}});"
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


def settings_html(lang):
    """N-W4: theme, text width (960 by default), and layout (N-W1) - the landing page's set."""
    s = STR[lang]
    def group(name, legend, opts, default):
        rows = ''.join('<label><input type="radio" name="%s" id="%s-%s"%s> %s</label>'
                       % (name, name, v, ' checked' if v == default else '', html.escape(t))
                       for v, t in opts)
        return '<fieldset><legend>%s</legend>%s</fieldset>' % (html.escape(legend), rows)
    return (group('theme', s['set_theme'], [('system', s['th_auto']), ('light', s['th_light']),
                                            ('dark', s['th_dark'])], 'system')
            + group('width', s['width'], [('960', s['w960']), ('fit', s['wfit'])], '960')
            + group('layout', s['layout'], [('auto', s['l_auto']), ('wide', s['l_wide']),
                                            ('narrow', s['l_narrow'])], 'auto'))


def language_html(lang, other_href):
    """N-W4's language: the two editions; the one being read is marked, the other is a link."""
    s = STR[lang]
    rows = []
    for code, label in (('en', 'English'), ('ko', '한국어')):
        if code == lang:
            rows.append('<span class="cur" aria-current="true">%s</span>' % label)
        else:
            rows.append('<a href="%s" hreflang="%s">%s</a>' % (other_href, code, label))
    return '<fieldset class="langs"><legend>%s</legend>%s</fieldset>' % (s['lang'], ''.join(rows))


def bar_html(lang, here_text, here_href, items, panel_inner, back_href):
    """N-W2: from the left, the menu (N-W4's settings inside), back, where the reader is; on a
    wide window the items spread to the right, on a narrow one (N-L3: 900) they move into the
    menu. The same bar as the site's landing page."""
    s = STR[lang]
    links = ''.join('<a href="%s"%s>%s</a>' % (h, (' title="%s"' % html.escape(t)) if t else '', html.escape(x))
                    for x, h, t in items)
    return ('<nav class="bar" aria-label="%s">'
            '<details class="menu"><summary aria-label="%s">☰</summary>'
            '<div class="panel"><nav class="goto">%s</nav>%s</div></details>'
            '<a class="back" href="%s" aria-label="%s" title="%s">←</a>'
            '<a class="here" href="%s">%s</a>'
            '<div class="items">%s</div></nav>'
            % (s['menu'], s['menu'], links, panel_inner, back_href, s['back'], s['back'],
               here_href, html.escape(here_text), links))


def bar(lang, name, pages, outline, version, pdf_url):
    s = STR[lang]
    i = pages.index(name)
    prev = pages[i - 1] if i > 0 else None
    nxt = pages[i + 1] if i + 1 < len(pages) else None
    other = 'ko' if lang == 'en' else 'en'
    items = [(s['contents'], 'index.html', '')]
    if prev:
        items.append(('← ' + s['prev'], prev + '.html', TITLES[lang][prev]))
    if nxt:
        items.append((s['next'] + ' →', nxt + '.html', TITLES[lang][nxt]))
    items += [(s['pdf'], pdf_url, ''), (s['repo'], REPO, s['repo_t']), (s['site'], SITE_ROOT, s['home_t'])]
    search = ('<div class="search"><label class="setlbl" for="q">%s</label> '
              '<input type="search" id="q" placeholder="%s" autocomplete="off">'
              '<p class="hint">%s</p><ul id="qr"></ul></div>'
              % (s['search'], html.escape(s['search_ph']), s['search_hint']))
    rows = ''.join('<a href="#%s">%s</a>' % (a, html.escape(t)) for lv, a, t in outline if lv == 2)
    here = ('<div class="here-sec"><div class="setlbl">%s</div>%s</div>' % (s['here'], rows)) if rows else ''
    inner = (search + here + language_html(lang, '../%s/%s.html' % (other, name))
             + settings_html(lang) + '<p class="ver">%s</p>' % html.escape(version))
    here_text = TITLES[lang][name] if name != 'index' else s['short']
    return bar_html(lang, here_text, '#top', items, inner, 'index.html')


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
<body id="top">
<a class="skip" href="#content">{s['skip']}</a>
{bar(lang, name, pages, outlines[name], version, pdf_url)}
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
