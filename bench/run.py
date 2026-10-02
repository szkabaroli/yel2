#!/usr/bin/env python3
"""Porffor's benchmarks (external/porffor/bench) against their ports to yel (bench/*.yel), as
Porffor's bench/ci.mjs runs its own: each program compiled to C (yelc here, Porffor's CLI there),
built by the same C compiler (clang -O3), then each run RUNS times one after another; the median
wall time, the compile time, the C's size and the binary's.

  bench/run.py [name...]        (default: every bench/*.yel)

BENCH_RUNS=n runs each n times (default 3); CC picks the C compiler; PORFFOR=<dir> is Porffor's
checkout (default ../porffor, beside yel2 in external/). A program Porffor has no .js for, or a
Porffor that does not build it, has its column empty. Writes the table as Markdown to stdout and the
numbers as JSON to build/bench/results.json.
"""
import json, os, shutil, statistics, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BENCH = os.path.join(ROOT, 'bench')
OUT = os.path.join(ROOT, 'build', 'bench')
PORFFOR = os.environ.get('PORFFOR', os.path.join(os.path.dirname(ROOT), 'porffor'))
CC = os.environ.get('CC', 'clang')
RUNS = int(os.environ.get('BENCH_RUNS', '3'))


def timed(cmd, cwd=ROOT):
    t = time.perf_counter()
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    return (time.perf_counter() - t) * 1000, r


def yelc():
    """yelc built -O2 from the compiler's own C (build/yelc2.c, as ./bootstrap.sh made it)."""
    src = os.path.join(ROOT, 'build', 'yelc2.c')
    exe = os.path.join(OUT, 'yelc')
    if not os.path.exists(src):
        sys.exit('bench: no build/yelc2.c: run ./bootstrap.sh first')
    if not os.path.exists(exe) or os.path.getmtime(exe) < os.path.getmtime(src):
        subprocess.run([CC, '-O2', '-w', '-Iruntime', '-o', exe, src, '-lm'], cwd=ROOT, check=True)
        # once, untimed: a new binary's first launch (macOS checks it) is not its compile time
        subprocess.run([exe, os.path.join(BENCH, 'empty.yel'), os.path.join(OUT, 'warm.c')], cwd=ROOT, capture_output=True)
    return exe


def build_yel(name, compiler):
    c = os.path.join(OUT, name + '.yel.c')
    exe = os.path.join(OUT, name + '.yel')
    ms, r = timed([compiler, os.path.join(BENCH, name + '.yel'), c])
    if r.returncode != 0:
        return {'error': (r.stderr or r.stdout).strip().splitlines()[-1][:160]}
    r2 = subprocess.run([CC, '-O3', '-w', '-Iruntime', c, '-o', exe, '-lm'], cwd=ROOT, capture_output=True, text=True)
    if r2.returncode != 0:
        return {'error': r2.stderr.strip().splitlines()[-1][:160]}
    return {'bin': exe, 'compile_ms': ms, 'c_size': os.path.getsize(c), 'bin_size': os.path.getsize(exe)}


def build_porffor(name):
    js = os.path.join(PORFFOR, 'bench', name + '.js')
    if not os.path.exists(js) or not shutil.which('node'):
        return None
    c = os.path.join(OUT, name + '.porf.c')
    exe = os.path.join(OUT, name + '.porf')
    cli = os.path.join(PORFFOR, 'cli', 'index.js')
    ms, r = timed(['node', '--stack-size=65500', cli, 'c', js, '-o', c], cwd=PORFFOR)
    if r.returncode != 0:
        return {'error': ((r.stderr or r.stdout).strip().splitlines() or ['build failed'])[-1][:160]}
    r2 = subprocess.run([CC, '-O3', '-w', c, '-o', exe, '-lm'], capture_output=True, text=True)
    if r2.returncode != 0:
        return {'error': r2.stderr.strip().splitlines()[-1][:160]}
    return {'bin': exe, 'compile_ms': ms, 'c_size': os.path.getsize(c), 'bin_size': os.path.getsize(exe)}


def run(b):
    """b's binary RUNS times: the median wall ms, its exit, and its last run's output."""
    times, out, status = [], '', 0
    for _ in range(RUNS):
        ms, r = timed([b['bin']])
        times.append(ms)
        out, status = (r.stdout + r.stderr).strip(), r.returncode
        if status != 0:
            break
    return {'ms': statistics.median(times), 'status': status, 'output': out}


def main():
    os.makedirs(OUT, exist_ok=True)
    names = sys.argv[1:] or sorted(f[:-4] for f in os.listdir(BENCH) if f.endswith('.yel'))
    compiler = yelc()
    results = {}
    for name in names:
        y, p = build_yel(name, compiler), build_porffor(name)
        for b in (y, p):
            if b and 'error' not in b:
                b.update(run(b))
        results[name] = {'yel': y, 'porffor': p}
        print(f'{name}: done', file=sys.stderr)

    kb = lambda n: f'{n / 1024:.1f} KB'
    cell = lambda b, f: '' if not b else ('failed' if 'error' in b else f(b))
    ms = lambda b: f"{b['ms']:.0f} ms" + ('' if b['status'] == 0 else f" (exit {b['status']})")
    print(f'## yel against Porffor\n\n{RUNS} runs each, median wall time. Both built by `{CC} -O3`.\n')
    print('| program | yel | Porffor | Porffor / yel | compile (yel / Porffor) | C (yel / Porffor) | binary (yel / Porffor) |')
    print('|---|--:|--:|--:|--:|--:|--:|')
    for name, r in results.items():
        y, p = r['yel'], r['porffor']
        ratio = ''
        if y and p and 'error' not in y and 'error' not in p and y['ms'] > 0:
            ratio = f"{p['ms'] / y['ms']:.2f}×"
        compile = lambda b: '%.0f ms' % b['compile_ms']
        c_size = lambda b: kb(b['c_size'])
        bin_size = lambda b: kb(b['bin_size'])
        both = lambda f: '%s / %s' % (cell(y, f), cell(p, f) or '-')
        print('| %s | %s | %s | %s | %s | %s | %s |' % (
            name, cell(y, ms), cell(p, ms) or '-', ratio, both(compile), both(c_size), both(bin_size)))
    for name, r in results.items():
        for side in ('yel', 'porffor'):
            b = r[side]
            if b and 'error' in b:
                print(f'\n{name} ({side}): {b["error"]}')
    with open(os.path.join(OUT, 'results.json'), 'w') as f:
        json.dump({'runs': RUNS, 'cc': CC, 'results': results}, f, indent=2)


if __name__ == '__main__':
    main()
