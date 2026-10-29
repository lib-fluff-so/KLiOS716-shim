#!/usr/bin/env python3
"""Summarise the "SMP UFSIO:" I/O accounting lines from patch 0115.

  ufsio-report.py LOG.txt

Each UFSIO line covers the sampler window that ended with the previous
sample (45 s by default), so the table is one row per window.  "slow op"
lines are counted per window as well.  Works on a decoded sweet-log dump
(see tools/sweet-log.py).
"""
import re
import sys

NUM = r'(\d+)'
LINE = re.compile(
    r' UFSIO: cmds ' + NUM + r' ' + NUM + r'K avg ' + NUM + r'us max ' + NUM +
    r'us \| rd ' + NUM + r' ' + NUM + r'K avg ' + NUM + r'us max ' + NUM +
    r'us \| wr ' + NUM + r' ' + NUM + r'K avg ' + NUM + r'us max ' + NUM +
    r'us \| sync ' + NUM + r' avg ' + NUM + r'us max ' + NUM +
    r'us \| other ' + NUM + r' \| lockwait ' + NUM + r'us max ' + NUM +
    r'us \| sleepable ' + NUM + r' spin ' + NUM +
    r' \| hist <4K ' + NUM + r' <16K ' + NUM + r' <64K ' + NUM +
    r' <256K ' + NUM + r' >=256K ' + NUM +
    r' \| slow ' + NUM)
SLOW = re.compile(r'slow op 0x([0-9a-f]+) len (\d+) us (\d+)')
FIELDS = ('cmds', 'kib', 'avg_us', 'max_us',
          'rd_cmds', 'rd_kib', 'rd_avg_us', 'rd_max_us',
          'wr_cmds', 'wr_kib', 'wr_avg_us', 'wr_max_us',
          'sync_cmds', 'sync_avg_us', 'sync_max_us', 'other',
          'lock_us', 'lock_max_us', 'sleepable', 'spin',
          'h0', 'h1', 'h2', 'h3', 'h4', 'slows')


def main(path):
    windows, slow_log = [], []
    for line in open(path, 'r', errors='replace'):
        m = LINE.search(line)
        if m:
            windows.append(dict(zip(FIELDS, map(int, m.groups()))))
            continue
        m = SLOW.search(line)
        if m:
            slow_log.append((int(m.group(1), 16), int(m.group(2)),
                             int(m.group(3))))
    if not windows:
        sys.exit('no "SMP UFSIO:" lines in %s (patch 0115 not running?)' % path)

    hdr = ('#', 'cmds', 'MiB', 'avg', 'max', 'rd', 'rdMiB', 'rdavg', 'rdmax',
           'wr', 'wrMiB', 'wravg', 'wrmax', 'sync', 'syncavg', 'lockwait',
           'slp', 'spn', 'slow')
    print('%3s %6s %7s %6s %6s | %6s %6s %6s %6s | %6s %6s %6s %6s | '
          '%5s %7s %6s %5s %5s %5s' % hdr)
    tot = dict.fromkeys(FIELDS, 0)
    for i, w in enumerate(windows):
        for k in tot:
            tot[k] += w[k]
        print('%3d %6d %7.1f %6d %6d | %6d %6.1f %6d %6d | %6d %7.1f %6d %6d | '
              '%5d %7d %6d %5d %5d %5d' % (
                  i, w['cmds'], w['kib'] / 1024, w['avg_us'], w['max_us'],
                  w['rd_cmds'], w['rd_kib'] / 1024, w['rd_avg_us'],
                  w['rd_max_us'], w['wr_cmds'], w['wr_kib'] / 1024,
                  w['wr_avg_us'], w['wr_max_us'], w['sync_cmds'],
                  w['sync_avg_us'], w['lock_us'], w['sleepable'], w['spin'],
                  w['slows']))

    def avg(us_key, n_key):
        ns = sum(w[n_key] * w[us_key] for w in windows)
        return ns / tot[n_key] if tot[n_key] else 0

    print('\nwindows %d, commands %d, %.1f MiB, avg %d us, max %d us'
          % (len(windows), tot['cmds'], tot['kib'] / 1024,
             avg('avg_us', 'cmds'), max(w['max_us'] for w in windows)))
    print('reads %d (%.1f MiB, avg %d us, max %d us), writes %d '
          '(%.1f MiB, avg %d us), syncs %d, other %d'
          % (tot['rd_cmds'], tot['rd_kib'] / 1024, avg('rd_avg_us', 'rd_cmds'),
             max(w['rd_max_us'] for w in windows), tot['wr_cmds'],
             tot['wr_kib'] / 1024, avg('wr_avg_us', 'wr_cmds'),
             tot['sync_cmds'], tot['other']))
    print('lockwait total %d ms (max %d us), sleepable %d spin %d'
          % (tot['lock_us'] // 1000, max(w['lock_max_us'] for w in windows),
             tot['sleepable'], tot['spin']))
    print('sizes <4K %d, 4K-16K %d, 16K-64K %d, 64K-256K %d, >=256K %d'
          % tuple(tot['h%d' % i] for i in range(5)))
    print('slow commands (>=20 ms) %d' % tot['slows'])
    if slow_log:
        print('\nfirst slow ops:')
        for op, ln, us in slow_log[:20]:
            print('  0x%02x len %-8d %d us' % (op, ln, us))


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit(__doc__.strip())
    main(sys.argv[1])
