#!/usr/bin/env python3
"""Mechanical 4.4 -> 7.0 edits applied once to the vendor files copied into drivers/audio-hi6402.
Each edit asserts its old text (exactly once) and is idempotent (skipped when already applied).
Usage: port-edit.py FILE EDIT...   EDIT = void-remove:<fn> | include:<old>=<new>"""
import re, sys

path = sys.argv[1]
s = open(path).read()
for e in sys.argv[2:]:
    kind, arg = e.split(':', 1)
    if kind == 'void-remove':
        fn = arg
        new = '= %s_void,' % fn
        if new in s:
            continue
        m = re.findall(r'\.remove\s*=\s*%s\s*,' % re.escape(fn), s)
        assert len(m) == 1, (path, e, m)
        s = s.replace(m[0], '.remove = %s_void,' % fn)
        # wrapper right before the platform_driver struct that uses it
        idx = s.index('.remove = %s_void,' % fn)
        start = s.rindex('static struct platform_driver', 0, idx)
        s = s[:start] + 'BACH2_VOID_REMOVE(%s)\n\n' % fn + s[start:]
    elif kind == 'include':
        old, new = arg.split('=', 1)
        if old not in s and new in s:
            continue
        assert s.count(old) == 1, (path, e)
        s = s.replace(old, new)
    else:
        sys.exit('unknown edit ' + e)
open(path, 'w').write(s)
