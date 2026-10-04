"""Map the blast radius of the Diablo 2 movement mod.

Source side: every function changed since the 1.5.3 tag (touched), every function that reads or
drives player movement state (affected), and whether each carries a D2_PROBE.
Binary side: the gamedb index of the stock 1.5.3 exe (Diablo 1/devilutionx/decompiled/.gamedb),
counting decompiled functions per touched module and the callers that reach them from other modules.

Usage: python blast_radius.py [out.md] [--insert-probes]
  --insert-probes  add D2_PROBE_FN(); to every touched or affected function that has no probe yet
Hard limit 120 s. Exit: 0 written, 3 missing input, 124 timeout.
"""
import os
import re
import sqlite3
import subprocess
import sys
import threading

threading.Timer(120, lambda: os._exit(124)).start()

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.abspath(os.path.join(HERE, '..', '..'))
GAMEDB = os.path.abspath(os.path.join(SRC, '..', 'devilutionx', 'decompiled', '.gamedb', 'index.sqlite'))

# Reads or writes of player movement state
MOVEMENT = re.compile(r'(\bplayer\.position\.|\bmyPlayer\.position\.|MyPlayer->position\.|Players\[[^\]]+\]\.position\.|'
                      r'\.walkpath\b|\bdPlayer\[|\bPM_WALK|isWalking\(\)|MakePlrPath|ClrPlrPath|PosOkPlayer|GetTargetPosition|'
                      r'freeMove|CMD_WALKXY|\bStartWalk\b|GetOffsetForWalking|\bViewPosition\b)')
FUNC = re.compile(r'^(?:[\w:<>,*&]+\s+)+\**&?([\w:~]+)\s*\(([^;]*)$')


def functions_in(path):
    """[(name, first_line, last_line, body)] for top level functions (DevilutionX style: '{' / '}' at column 0)."""
    lines = open(path, encoding='utf-8', errors='replace').read().replace('\r\n', '\n').split('\n')
    out, i = [], 0
    while i < len(lines):
        m = FUNC.match(lines[i])
        if m and not lines[i].startswith(('if', 'for', 'while', 'switch', 'return', 'namespace', '#', '//')):
            j = i
            while j < len(lines) and lines[j] != '{' and not lines[j].endswith(';') and j - i < 6:
                j += 1
            if j < len(lines) and lines[j] == '{':
                k = j + 1
                while k < len(lines) and lines[k] != '}':
                    k += 1
                out.append((m.group(1), i + 1, k + 1, '\n'.join(lines[j:k + 1])))
                i = k + 1
                continue
        i += 1
    return out


def insert_probes(path, first_lines):
    """Put D2_PROBE_FN(); at the top of the functions whose signatures start on first_lines (1-based)."""
    raw = open(path, 'rb').read().decode('utf-8')
    crlf = '\r\n' in raw
    lines = raw.replace('\r\n', '\n').split('\n')
    for first in sorted(first_lines, reverse=True):
        j = first - 1
        while lines[j] != '{':
            j += 1
        lines.insert(j + 1, '\tD2_PROBE_FN();')
    if not any(l == '#include "d2probe.h"' for l in lines):
        k = next(i for i, l in enumerate(lines) if l.startswith('#include'))
        lines.insert(k + 1, '#include "d2probe.h"')
    out = '\n'.join(lines)
    open(path, 'wb').write((out.replace('\n', '\r\n') if crlf else out).encode('utf-8'))


def changed_lines():
    """Lines changed since 1.5.3, per file. Lines that only add probes are instrumentation, not behaviour changes."""
    diff = subprocess.run(['git', '-C', SRC, 'diff', '-U0', '1.5.3', '--', 'Source'], capture_output=True, text=True, timeout=60).stdout
    changed, current, line_no = {}, None, 0
    for line in diff.split('\n'):
        if line.startswith('+++ b/'):
            current = line[6:]
            changed.setdefault(current, set())
        elif line.startswith('@@') and current:
            m = re.search(r'\+(\d+)(?:,(\d+))?', line)
            line_no = int(m.group(1))
            if m.group(2) == '0':
                changed[current].add(line_no)  # pure deletion after this line
        elif current and line.startswith('+') and not line.startswith('+++'):
            if not re.search(r'D2_PROBE|#include "d2probe.h"|d2probe::', line):
                changed[current].add(line_no)
            line_no += 1
        elif current and line.startswith('-') and not line.startswith('---'):
            changed[current].add(line_no)
    return changed


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    out_path = args[0] if args else os.path.join(HERE, 'blast_radius.md')
    changed = changed_lines()
    touched, affected = [], []
    for root, _, files in os.walk(os.path.join(SRC, 'Source')):
        for name in files:
            if not name.endswith('.cpp'):
                continue
            path = os.path.join(root, name)
            rel = os.path.relpath(path, SRC).replace('\\', '/')
            lines_changed = changed.get(rel, set())
            if rel == 'Source/d2probe.cpp':
                continue  # the instrumentation itself
            new_file = rel == 'Source/d2mod.cpp'
            for fn, first, last, body in functions_in(path):
                probed = 'D2_PROBE' in body
                hits = len(MOVEMENT.findall(body))
                if new_file or any(first <= l <= last for l in lines_changed):
                    touched.append((rel, fn, first, probed, 'new file' if new_file else 'changed'))
                elif hits:
                    affected.append((rel, fn, first, probed, hits))

    if '--insert-probes' in sys.argv:
        todo = {}
        for f, fn, ln, p, _ in touched + affected:
            if not p:
                todo.setdefault(f, []).append(ln)
        for f, firsts in todo.items():
            path = os.path.join(SRC, f)
            src = open(path, encoding='utf-8').read().replace('\r\n', '\n').split('\n')
            firsts = [ln for ln in firsts if 'constexpr' not in src[ln - 1]]
            insert_probes(path, firsts)
        print(f'inserted probes into {sum(len(v) for v in todo.values())} functions in {len(todo)} files; rerun without --insert-probes to refresh the map')
        return 0

    md = ['# Blast radius: Diablo 2 movement in Diablo 1', '',
          'Generated by `tools/d2harness/blast_radius.py` from `git diff 1.5.3` and a scan of `Source/` for code that reads or drives',
          'player movement state (positions, walk paths, the dPlayer grid, walk modes and commands, free movement).', '']
    md += [f'## Touched: {len(touched)} functions changed or added', '', '| File | Function | Line | Probe |', '|---|---|---|---|']
    md += [f'| {f} | `{fn}` | {ln} | {"yes" if p else "no"} |' for f, fn, ln, p, _ in sorted(touched)]
    probed_aff = sum(1 for a in affected if a[3])
    md += ['', f'## Affected: {len(affected)} functions depend on movement state ({probed_aff} carry probes)', '',
           'Ranked by how many movement references they hold. Functions not probed individually are covered by the per-tick',
           'invariant checks (grid, solidity, overlap, speed) and by the demo replay comparisons.', '',
           '| File | Function | Line | Refs | Probe |', '|---|---|---|---|---|']
    md += [f'| {f} | `{fn}` | {ln} | {h} | {"yes" if p else "-"} |' for f, fn, ln, p, h in sorted(affected, key=lambda a: (-a[4], a[0], a[1]))]

    if os.path.exists(GAMEDB):
        db = sqlite3.connect(GAMEDB)
        modules = {}
        for addr, name, module in db.execute('select address, name, module from port_module_assignment'):
            modules[name] = module
        ids = {name: fid for fid, name in db.execute('select id, name from functions')}
        by_id = {fid: modules.get(name, 'unknown') for name, fid in ids.items()}
        touched_modules = sorted({'game/' + os.path.splitext(f[len('Source/'):])[0] for f, *_ in touched})
        md += ['', '## Binary side (stock 1.5.3 exe, gamedb index)', '',
               'Functions the module pass assigned to each touched source file, and how many functions in other modules call into them.',
               'Most of the stripped LTO binary is still unassigned, so these are lower bounds.', '',
               '| Module | Functions | Callers from other modules |', '|---|---|---|']
        for mod in touched_modules:
            members = {fid for fid, m in by_id.items() if m == mod}
            callers = {src for src, dst in db.execute('select src_id, dst_id from edges') if dst in members and by_id.get(src) != mod}
            md.append(f'| {mod} | {len(members)} | {len(callers)} |')
    else:
        md += ['', f'(gamedb index not found at {GAMEDB}; binary side skipped)']

    open(out_path, 'w', encoding='utf-8').write('\n'.join(md) + '\n')
    print(f'touched={len(touched)} affected={len(affected)} probed_affected={probed_aff} -> {out_path}')
    return 0


if __name__ == '__main__':
    code = main()
    sys.stdout.flush()
    os._exit(code)
