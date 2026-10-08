"""Stages the whole workspace for the G:\\MafiaCoop repo.

Framework\\ and Framework\\code\\projects\\MafiaMP\\ are checkouts of the upstream MafiaHub repos and
keep their own .git so upstream can still be pulled. Git refuses to add files inside a directory that
has its own .git (it would record a submodule pointer instead), so this hides those .git directories
for the duration of `git add -A` and restores them afterwards. Run from anywhere:
    python dev\\tools\\git_stage.py
then `git commit` and `git push` as usual."""
import os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
NESTED = [os.path.join(ROOT, "Framework", ".git"), os.path.join(ROOT, "Framework", "code", "projects", "MafiaMP", ".git")]

def main():
    hidden = []
    try:
        for g in NESTED:
            if os.path.isdir(g):
                os.rename(g, g + "_upstream")
                hidden.append(g)
        subprocess.check_call(["git", "-C", ROOT, "add", "-A"])
        out = subprocess.check_output(["git", "-C", ROOT, "status", "--short"], text=True)
        print(f"staged: {len(out.splitlines())} change(s)")
    finally:
        for g in hidden:
            os.rename(g + "_upstream", g)

if __name__ == "__main__":
    main()
