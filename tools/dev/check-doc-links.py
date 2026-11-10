import os
import re
import sys

root = sys.argv[1] if len(sys.argv) > 1 else "."
link = re.compile(r"\]\(([^)\s#]+)(#[^)]*)?\)")
bad = []
for dirpath, dirnames, filenames in os.walk(root):
    dirnames[:] = [d for d in dirnames if d not in (".git", "out", "evidence", "node_modules", "prebuilt", "dist")]
    for fn in filenames:
        if not fn.endswith(".md"):
            continue
        p = os.path.join(dirpath, fn)
        with open(p, encoding="utf-8", errors="replace") as f:
            for i, line in enumerate(f, 1):
                for m in link.finditer(line):
                    t = m.group(1)
                    if t.startswith(("http://", "https://", "mailto:", "<")):
                        continue
                    t2 = t.replace("%20", " ")
                    tgt = os.path.normpath(os.path.join(dirpath, t2))
                    if not os.path.exists(tgt):
                        bad.append(f"{p}:{i} -> {t}")
print("\n".join(bad) if bad else "no broken links")
print(f"checked {len(bad)} broken")
