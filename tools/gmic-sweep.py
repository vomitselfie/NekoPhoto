#!/usr/bin/env python3
"""Runs every G'MIC catalogue filter once at its default settings, the way the editor runs it, and sorts
them: works, no-op, changes the image size, makes several layers, blank, fails, slow. Writes results.json
and unsupported.txt (the list src/app/gmic/unsupported.txt holds) into the output folder, and with
--gallery a captioned image per working filter, in folders as G'MIC's menus are.

G'MIC runs with no display and a time limit: some catalogue entries are interactive programs (games,
tools waiting for clicks in a window of their own) and would otherwise open windows on the desktop.

    compositor-linux --headless --rpc-socket /tmp/c.sock &
    compositor-linux --call gmic.filters --params '{"all":true}' --rpc-socket /tmp/c.sock > filters.json
    tools/gmic-sweep.py filters.json photo.jpg sweep/ [--gallery]

Needs Python 3 with Pillow and NumPy, and the gmic executable.
"""
import argparse, collections, concurrent.futures as cf, datetime, json, os, re, subprocess, sys, textwrap, time
import numpy as np
from PIL import Image, ImageDraw, ImageFont

REASONS = {'resizes': 'changes the image size', 'layers': 'makes several layers', 'error': 'fails', 'blank': 'gives a blank image'}


def run_one(i, f, source, work, timeout):
    """One filter, as the editor runs it: the command in a one-line script, G'MIC parsing its arguments."""
    out, script = f'{work}/{i}.png', f'{work}/{i}.gmic'
    with open(script, 'w') as s:
        s.write('compositor_run :\n  ' + f['defaultCommand'].replace('\n', ' ') + '\n')
    env = {k: v for k, v in os.environ.items() if k not in ('DISPLAY', 'WAYLAND_DISPLAY')}
    env['OMP_NUM_THREADS'] = '3'
    start = time.time()
    try:
        p = subprocess.run(['gmic', '-v', '-1', '-m', script, f'{work}/in.png', 'compositor_run', '-o', out],
                           capture_output=True, text=True, timeout=timeout, env=env)
    except subprocess.TimeoutExpired:
        return dict(verdict='slow', secs=timeout)
    secs = time.time() - start
    parts = [x for x in os.listdir(work) if x.startswith(f'{i}_') and x.endswith('.png')]
    for x in parts: os.remove(os.path.join(work, x))
    if p.returncode == 0 and not os.path.exists(out) and len(parts) > 1:
        return dict(verdict='layers', secs=secs)
    if p.returncode != 0 or not os.path.exists(out):
        lines = re.sub(r'\x1b\[[0-9;]*m', '', p.stderr).strip().splitlines()
        return dict(verdict='error', secs=secs, err=(lines or ['?'])[-1][:200])
    result = np.asarray(Image.open(out).convert('RGBA')).astype(np.int16)
    if result.shape != source.shape:
        return dict(verdict='resizes', secs=secs)
    diff = float(np.abs(result - source).mean())
    if diff < 0.5: verdict = 'no-op'
    elif result[..., 3].max() == 0 or result[..., :3].std() < 2: verdict = 'blank'
    else: verdict = 'works'
    return dict(verdict=verdict, secs=secs, diff=round(diff, 1))


def gallery(results, work, folder):
    """A captioned image per working filter: the result over a checkerboard, then its name, menu and command."""
    font = lambda name, size: ImageFont.truetype(name, size) if os.path.exists(name) else ImageFont.load_default()
    bold, regular = font('/usr/share/fonts/noto/NotoSans-Bold.ttf', 22), font('/usr/share/fonts/noto/NotoSans-Regular.ttf', 15)
    safe = lambda s: re.sub(r'[\\/:*?"<>|]', '-', s).strip()[:120]
    Image.open(f'{work}/in.png').convert('RGB').save(f'{folder}/_original.jpg', quality=90)
    seen = collections.Counter()
    for r in results:
        if r['verdict'] != 'works': continue
        img = Image.open(f'{work}/{r["i"]}.png').convert('RGBA')
        card = Image.new('RGB', (img.width, img.height + 92), (24, 24, 28))
        board = Image.new('RGB', img.size, (200, 200, 200))
        d = ImageDraw.Draw(board)
        for y in range(0, img.height, 16):
            for x in range((y // 16 % 2) * 16, img.width, 32): d.rectangle([x, y, x + 15, y + 15], fill=(235, 235, 235))
        board.paste(img, (0, 0), img)
        card.paste(board, (0, 0))
        d = ImageDraw.Draw(card)
        d.text((12, img.height + 6), r['name'], font=bold, fill=(255, 255, 255))
        d.text((12, img.height + 38), r['folder'] or '(no folder)', font=regular, fill=(170, 170, 180))
        d.text((12, img.height + 58), '\n'.join(textwrap.wrap(r['cmd'], 88)[:2]), font=regular, fill=(120, 200, 255))
        where = os.path.join(folder, *[safe(p) for p in (r['folder'] or 'Other').split(' / ')])
        os.makedirs(where, exist_ok=True)
        name = safe(r['name'])
        seen[(where, name)] += 1
        n = seen[(where, name)]
        card.save(os.path.join(where, name + (f' ({n})' if n > 1 else '') + '.jpg'), quality=88)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('filters', help='gmic.filters output (JSON)')
    ap.add_argument('image', help='a photo to run the filters on')
    ap.add_argument('out', help='folder for results.json, unsupported.txt and the gallery')
    ap.add_argument('--gallery', action='store_true', help='a captioned image per working filter')
    ap.add_argument('--timeout', type=int, default=30)
    ap.add_argument('--jobs', type=int, default=8)
    a = ap.parse_args()
    filters = json.load(open(a.filters))['filters']
    work = os.path.join(a.out, 'work')
    os.makedirs(work, exist_ok=True)
    img = Image.open(a.image).convert('RGBA')
    img = img.resize((640, round(img.height * 640 / img.width)), Image.LANCZOS)
    img.save(f'{work}/in.png')
    source = np.asarray(img).astype(np.int16)
    results = [None] * len(filters)
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        futures = {ex.submit(run_one, i, f, source, work, a.timeout): i for i, f in enumerate(filters)}
        for k, fut in enumerate(cf.as_completed(futures)):
            i = futures[fut]
            results[i] = dict(i=i, name=filters[i]['name'], folder=filters[i]['folder'], cmd=filters[i]['defaultCommand'], **fut.result())
            if k % 100 == 0: print(f'{k}/{len(filters)}', file=sys.stderr, flush=True)
    json.dump(results, open(os.path.join(a.out, 'results.json'), 'w'), indent=1)
    version = subprocess.run(['gmic', '-v', '-1', 'e', '${-version}'], capture_output=True, text=True).stderr.strip().splitlines()[-1:] or ['?']
    listed = {}
    for r in results:
        if r['verdict'] in REASONS: listed.setdefault(r['cmd'].split()[0], (REASONS[r['verdict']], r['name']))
    with open(os.path.join(a.out, 'unsupported.txt'), 'w') as f:
        f.write("# G'MIC catalogue filters that do not work in this editor at their default settings, by command.\n")
        f.write(f"# Made by tools/gmic-sweep.py with G'MIC {version[0]} on {datetime.date.today()}: each filter ran once at its\n")
        f.write(f"# defaults on a 640-pixel-wide photo, with no display and a {a.timeout} s limit. The G'MIC dialog hides these unless\n")
        f.write("# \"Show all filters\" is on. Filters that change nothing at their defaults, and slow ones, are not listed.\n")
        for cmd, (why, name) in sorted(listed.items()): f.write(f'{cmd}\t{why}\t{name}\n')
    print(collections.Counter(r['verdict'] for r in results).most_common())
    if a.gallery: gallery(results, work, a.out)


if __name__ == '__main__':
    main()
