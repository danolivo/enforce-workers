#!/usr/bin/env python3
"""
Reduce a `make installcheck-world` run against a server with enforce_workers
preloaded to a report that can be kept in the repository and compared.

    deviations.py collect --src PGSRC --log POSTMASTER_LOG --make-log MAKE_LOG \
                          --server-alive yes|no > report.txt
    deviations.py compare BASELINE REPORT

The report is plain text, one section per kind of evidence:

  == crash          lines of the server log that mean a backend died
  == harness        pg_regress/psql failures that are not a test diff
  == module-log     WARNING and worse raised from this library's own code
  == test <name>    the +/- lines of one test's diff

Lines that start with '~' are information only; `compare` ignores them.

A test diff is reduced before it is written.  Context lines, file names and
hunk positions are dropped, and a line removed in one place and added in
another is cancelled out: a query without ORDER BY returning its rows in a
different order leaves nothing behind.  Such a test is listed as '~reorder'.
The remaining lines keep the order they had in the diff so that a plan still
reads as a plan, but `compare` treats them as a multiset, so a reorder that
happens to land in a different hunk does not count as a change.
"""

import argparse
import collections
import os
import re
import sys

MODULE_FILES = ('enforce_workers.c', 'nlguard.c', 'seqguard.c', 'idxdefer.c')

CRASH_RE = re.compile(
    r'was terminated by (signal|exception)'
    r'|exited with exit code [2-9]'
    r'|terminating any other active server processes'
    r'|all server processes terminated; reinitializing'
    r'|\bTRAP: '
    r'|\bPANIC: ')

HARNESS_RE = re.compile(
    r'^(pg_regress|pg_isolation_regress|pg_regress_ecpg|psql): '
    r'(?!.*\(using postmaster on)')

LEVELS = ('WARNING', 'ERROR', 'FATAL', 'PANIC')
LEVEL_RE = re.compile(r'\b(%s):\s+(?:([0-9A-Z]{5}):\s+)?(.*)$' % '|'.join(LEVELS))
LOCATION_RE = re.compile(r'\bLOCATION:\s+(\S+), (\S+?):\d+')
CONT_RE = re.compile(r'\b(DETAIL|HINT|QUERY|CONTEXT):  ')


def numbers_out(s):
    """Replace what differs from run to run: OIDs, PIDs, sizes, addresses."""
    s = re.sub(r'0x[0-9a-fA-F]+', '0xN', s)
    return re.sub(r'\d+', 'N', s)


def test_name(results_path, src):
    p = os.path.normpath(results_path)
    if p.startswith(src + os.sep):
        p = p[len(src) + 1:]
    p = os.sep.join(x for x in p.split(os.sep) if x != 'results')
    return p[:-4] if p.endswith('.out') else p


def parse_diffs(path, src):
    """Yield (test, [lines]) for each file diff in one regression.diffs."""
    test, lines = None, []
    with open(path, errors='replace') as f:
        for raw in f:
            line = raw.rstrip('\n')
            if line.startswith('diff '):
                if test is not None:
                    yield test, lines
                test, lines = test_name(line.split()[-1], src), []
            elif test is None or line.startswith(('--- ', '+++ ', '@@')):
                continue
            elif line.startswith(('+', '-')):
                lines.append(line[0] + line[1:].rstrip())
    if test is not None:
        yield test, lines


def cancel_reorders(lines):
    """Drop every line that was removed in one place and added in another."""
    plus = collections.Counter(l[1:] for l in lines if l[0] == '+')
    minus = collections.Counter(l[1:] for l in lines if l[0] == '-')
    left = {'+': plus & minus, '-': plus & minus}
    out = []
    for l in lines:
        c = left[l[0]]
        if c[l[1:]] > 0:
            c[l[1:]] -= 1
        else:
            out.append(l)
    return out


def collect_tests(src):
    tests = {}
    for root, dirs, files in os.walk(src):
        dirs[:] = [d for d in dirs if d not in ('.git', 'tmp_check')]
        if 'regression.diffs' in files:
            for test, lines in parse_diffs(os.path.join(root, 'regression.diffs'), src):
                tests.setdefault(test, []).extend(lines)
    return tests


def collect_log(path):
    """
    Crashes, and messages at WARNING or above whose LOCATION is in this
    library.  With log_error_verbosity = verbose the LOCATION line follows the
    message and its DETAIL, HINT, QUERY and CONTEXT lines.
    """
    crashes, module = set(), set()
    if not path or not os.path.exists(path):
        return crashes, module
    pending = None
    with open(path, errors='replace') as f:
        for line in f:
            line = line.rstrip('\n')
            if CRASH_RE.search(line) or 'Failed process was running:' in line:
                crashes.add(numbers_out(strip_prefix(line)))
            m = LEVEL_RE.search(line)
            if m:
                pending = m
                continue
            loc = LOCATION_RE.search(line)
            if loc:
                func, fname = loc.groups()
                fname = os.path.basename(fname)
                if pending is not None and fname in MODULE_FILES:
                    level, code, msg = pending.groups()
                    module.add('%s %s %s (%s): %s' % (
                        level, code or '-----', func, fname, numbers_out(msg)))
                pending = None
            elif not line.startswith('\t') and not CONT_RE.search(line):
                pending = None
    return crashes, module


def strip_prefix(line):
    """Drop log_line_prefix ('%m [%p] %b ') so that runs compare."""
    m = re.match(r'^\d{4}-\d\d-\d\d \S+ \S+ \[\d+\] (.*)$', line)
    return m.group(1) if m else line


def collect_harness(path, src):
    out = set()
    if not path or not os.path.exists(path):
        return out
    with open(path, errors='replace') as f:
        for line in f:
            line = line.rstrip('\n')
            if HARNESS_RE.search(line):
                out.add(line.replace(src + os.sep, ''))
    return out


def cmd_collect(args):
    src = os.path.abspath(args.src)
    tests = collect_tests(src)
    crashes, module = collect_log(args.log)
    harness = collect_harness(args.make_log, src)

    w = sys.stdout.write
    w('~ enforce_workers installcheck-world deviations\n')
    if args.label:
        w('~ %s\n' % args.label)
    w('\n== server\n')
    w('alive at end: %s\n' % args.server_alive)
    w('\n== crash\n')
    for l in sorted(crashes):
        w(l + '\n')
    w('\n== harness\n')
    for l in sorted(harness):
        w(l + '\n')
    w('\n== module-log\n')
    for l in sorted(module):
        w(l + '\n')
    for test in sorted(tests):
        lines = cancel_reorders(tests[test])
        if not lines:
            w('\n~reorder %s\n' % test)
            continue
        w('\n== test %s\n' % test)
        for l in lines:
            w(l + '\n')
    return 0


def parse_report(path):
    sections = collections.OrderedDict()
    cur = None
    with open(path, errors='replace') as f:
        for line in f:
            line = line.rstrip('\n')
            if line.startswith('~'):
                cur = None
                continue
            if line.startswith('== '):
                cur = line[3:]
                sections[cur] = collections.Counter()
            elif cur is not None and line:
                sections[cur][line] += 1
    return sections


def cmd_compare(args):
    summary = os.environ.get('GITHUB_STEP_SUMMARY')
    out = []

    if not os.path.exists(args.baseline):
        out.append('No baseline at `%s`. Take `report.txt` from the run '
                   'artifact and commit it there.' % args.baseline)
        emit(out, summary)
        return 1

    base, cur = parse_report(args.baseline), parse_report(args.report)
    names = list(base) + [n for n in cur if n not in base]
    changed = 0
    for name in names:
        b, c = base.get(name), cur.get(name)
        if b == c:
            continue
        changed += 1
        if b is None:
            out.append('### NEW `%s`' % name)
        elif c is None:
            out.append('### GONE `%s`' % name)
        else:
            out.append('### CHANGED `%s`' % name)
        gone = (b or collections.Counter()) - (c or collections.Counter())
        new = (c or collections.Counter()) - (b or collections.Counter())
        body = ['  only in baseline: ' + l for l in sorted(gone.elements())] + \
               ['  only in this run: ' + l for l in sorted(new.elements())]
        out.append('```\n' + '\n'.join(body[:200]) +
                   ('\n  ... %d more' % (len(body) - 200) if len(body) > 200 else '') +
                   '\n```')

    if changed:
        out.insert(0, '**%d section(s) differ from `%s`.** If the change is '
                   'intended, replace the baseline with `report.txt` from the '
                   'run artifact.' % (changed, args.baseline))
    else:
        out.insert(0, 'No deviations beyond `%s`.' % args.baseline)
    emit(out, summary)
    return 1 if changed else 0


def emit(lines, summary):
    text = '\n\n'.join(lines) + '\n'
    sys.stdout.write(text)
    if summary:
        with open(summary, 'a') as f:
            f.write(text)


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    c = sub.add_parser('collect')
    c.add_argument('--src', required=True)
    c.add_argument('--log')
    c.add_argument('--make-log')
    c.add_argument('--server-alive', default='unknown')
    c.add_argument('--label')
    c.set_defaults(func=cmd_collect)
    p = sub.add_parser('compare')
    p.add_argument('baseline')
    p.add_argument('report')
    p.set_defaults(func=cmd_compare)
    args = ap.parse_args()
    sys.exit(args.func(args))


if __name__ == '__main__':
    main()
