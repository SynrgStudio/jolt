"""Builds the docs site into _site/: copies site/, fills the shortcut and command tables from src/config.inl
(so the docs can't drift from what Jolt actually binds), and brings the media in. Run from the repo root:
    python site/build.py            -> _site/
GitHub Pages runs the same script on every push (see .github/workflows/pages.yml).
"""
import html, os, re, shutil, sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
out = os.path.join(root, '_site')
site = os.path.join(root, 'site')


def split_keys(spec):
    """'Ctrl+N, F2' -> ['Ctrl+N', 'F2']. A comma right after a modifier is the comma key, as in Jolt's SplitKeys."""
    out, cur = [], ''
    for c in spec:
        if c == ',' and not cur.strip().lower().endswith(('ctrl+', 'control+', 'shift+', 'alt+')):
            if cur.strip():
                out.append(cur.strip())
            cur = ''
        else:
            cur += c
    if cur.strip():
        out.append(cur.strip())
    return out


def keys_html(spec):
    """'Ctrl+Shift+N, F2' -> <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>N</kbd> / <kbd>F2</kbd>"""
    parts = []
    for combo in split_keys(spec):
        # "Ctrl++" and "Ctrl+-": the last char is the key itself
        if combo.endswith('++') or combo.endswith('+-'):
            mods, key = combo[:-2], combo[-1]
            keys = (mods.split('+') if mods else []) + [key]
        else:
            keys = combo.split('+')
        parts.append('+'.join('<kbd>%s</kbd>' % html.escape(k) for k in keys))
    return ' &nbsp;/&nbsp; '.join(parts) if parts else '<span class="dim">none (assign one in Ctrl+K)</span>'


def commands():
    src = open(os.path.join(root, 'src', 'config.inl'), encoding='utf-8').read()
    table = src[src.index('const CmdDef kCommands[] = {'):]
    table = table[:table.index('\n};')]
    rows = []
    for m in re.finditer(r'\{L"([a-z_]+)", Cmd::\w+, L"([^"]*)", L"((?:[^"\\]|\\.)*)"\}', table):
        name, keys, help_ = m.group(1), m.group(2), m.group(3).replace('\\"', '"')
        rows.append((name, keys, help_))
    return rows


def version():
    v = open(os.path.join(root, 'src', 'version.h'), encoding='utf-8').read()
    return re.search(r'JOLT_VERSION "([^"]+)"', v).group(1)


def main():
    rows = commands()
    if not rows:
        sys.exit('no commands found in src/config.inl')
    shortcuts = '\n'.join(
        '<tr><td>%s</td><td>%s <span class="ex">%s</span></td></tr>' % (keys_html(k), html.escape(h[0].upper() + h[1:]), html.escape(n))
        for n, k, h in rows)
    page = open(os.path.join(site, 'index.html'), encoding='utf-8').read()
    assert '<!-- SHORTCUTS -->' in page
    page = page.replace('<!-- SHORTCUTS -->', shortcuts).replace('{{VERSION}}', version()).replace('{{COUNT}}', str(len(rows)))
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(os.path.join(out, 'media'))
    open(os.path.join(out, 'index.html'), 'w', encoding='utf-8', newline='\n').write(page)
    for f in ('style.css', 'InterVariable.woff2', 'Inter-LICENSE.txt'):
        shutil.copy(os.path.join(site, f), out)
    shutil.copy(os.path.join(site, 'CNAME'), out)
    open(os.path.join(out, '.nojekyll'), 'w').close()
    for f in os.listdir(os.path.join(root, 'docs', 'media')):
        shutil.copy(os.path.join(root, 'docs', 'media', f), os.path.join(out, 'media', f))
    shutil.copy(os.path.join(root, 'assets', 'jolt-256.png'), os.path.join(out, 'media', 'jolt-256.png'))
    shutil.copy(os.path.join(root, 'assets', 'jolt.ico'), os.path.join(out, 'favicon.ico'))
    print('built _site with %d commands, version %s' % (len(rows), version()))


if __name__ == '__main__':
    main()
