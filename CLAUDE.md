# Phase Align

Read `docs/DEVELOPMENT.md` for the layout, building, testing and the rules that keep the code correct.

- **Python** (the reference implementations in `reference/`, `tools/`) always goes through the project venv: `.venv/bin/python`
  or `source .venv/bin/activate`, never the system `python3` or `pip`. Dependencies are in `reference/requirements.txt`.
  If `.venv/` is missing: `python3 -m venv .venv && .venv/bin/pip install -r reference/requirements.txt`.
- The only mention of another product anywhere in the repo (docs, code, comments, commit messages) is the README's single
  "Inspired by..." sentence. Don't name any mic, preamp or hardware model.
- Format touched C++ with the repo's `.clang-format`. Ask before committing; don't push unless asked.
