#!/usr/bin/env python3
"""Move finished backlog items to BACKLOG-COMPLETE.md and recompute statuses.

Usage: scripts/backlog-complete.py ID [ID ...]
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BACKLOG = ROOT / "BACKLOG.md"
COMPLETE = ROOT / "BACKLOG-COMPLETE.md"
ID_RE = r"\d{8}-\d{6}-\d+"
COMPLETE_HEADER = (
    "# pg_stat_statement_context — Completed backlog items\n\n"
    "Finished items moved here from BACKLOG.md (see CLAUDE.md).\n"
)


def split_sections(lines):
    """Return {id: (start, end)} line ranges for each task section."""
    sections, cur, start = {}, None, None
    for i, line in enumerate(lines):
        m = re.match(rf"^### ({ID_RE}):", line)
        ends = m or line.startswith("## ") or line.strip() == "---"
        if ends and cur:
            sections[cur] = (start, i)
            cur = None
        if m:
            cur, start = m.group(1), i
    if cur:
        sections[cur] = (start, len(lines))
    return sections


def main(ids):
    lines = BACKLOG.read_text().splitlines(keepends=True)
    sections = split_sections(lines)
    missing = [i for i in ids if i not in sections]
    if missing:
        sys.exit(f"not found in BACKLOG.md: {', '.join(missing)}")

    done_text = COMPLETE.read_text() if COMPLETE.exists() else COMPLETE_HEADER
    head, sep, dropped = done_text.partition("\n## Dropped")
    drop = set()
    for tid in ids:
        s, e = sections[tid]
        body = "".join(lines[s:e]).rstrip() + "\n"
        body = re.sub(r"(?m)^\*\*Status:\*\*.*$", "**Status:** done", body)
        head = head.rstrip() + "\n\n" + body
        drop.update(range(s, e))
    done_text = head + ("\n" + sep.lstrip("\n") + dropped if sep else "")
    COMPLETE.write_text(done_text)

    lines = [l for i, l in enumerate(lines) if i not in drop]
    lines = [l for l in lines
             if not any(l.startswith(f"| {tid} |") for tid in ids)]

    completed = set(re.findall(rf"^### ({ID_RE}):", head, re.M))
    sections = split_sections(lines)
    status = {}
    for tid, (s, e) in sections.items():
        text = "".join(lines[s:e])
        deps = re.search(r"^\*\*Depends on:\*\*(.*)$", text, re.M)
        deps = re.findall(ID_RE, deps.group(1)) if deps else []
        oq = re.search(r"^\*\*Open questions:\*\*(.*?)(?=^\*\*Status:\*\*)",
                       text, re.M | re.S)
        has_q = bool(oq) and oq.group(1).strip().lower().rstrip(".") not in (
            "none", "")
        if has_q:
            status[tid] = "blocked-on-questions"
        elif all(d in completed for d in deps):
            status[tid] = "ready"
        else:
            status[tid] = "blocked-on-deps"
        for i in range(s, e):
            if lines[i].startswith("**Status:**"):
                lines[i] = f"**Status:** {status[tid]}\n"

    for i, l in enumerate(lines):
        m = re.match(rf"^\| ({ID_RE}) \|", l)
        if m and m.group(1) in status:
            cells = l.rstrip("\n").split("|")
            cells[-2] = f" {status[m.group(1)]} "
            lines[i] = "|".join(cells) + "\n"

    BACKLOG.write_text("".join(lines))
    ready = sorted((t for t, s in status.items() if s == "ready"),
                   key=lambda t: int(t.rsplit("-", 1)[1]))
    print("ready:", ", ".join(ready) or "(none)")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(sys.argv[1:])
