"""Collects every file we changed or added in the two upstream repos (MafiaHub Framework and
MafiaMP) into G:\\MafiaCoop\\overlay\\, preserving paths, plus VERSIONS.txt with the upstream
commits. `dev\\apply_overlay.cmd` copies it back onto a fresh clone. Run after every code change
before committing."""
import os, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FRAMEWORK = os.path.join(ROOT, "Framework")
MAFIAMP = os.path.join(FRAMEWORK, "code", "projects", "MafiaMP")
OVERLAY = os.path.join(ROOT, "overlay")
SKIP_DIRS = ("builds", "logs", ".packages", "node_modules", "vcpkg_installed", "_disabled")

def git(repo, *args):
    return subprocess.check_output(["git", "-C", repo, *args], text=True, encoding="utf-8", errors="replace")

def changed_files(repo):
    out = git(repo, "status", "--porcelain", "--untracked-files=all")
    files = []
    for line in out.splitlines():
        if not line.strip():
            continue
        status, path = line[:2], line[3:]
        if " -> " in path:
            path = path.split(" -> ")[1]
        if status.strip() == "D":
            continue
        path = path.strip().strip('"')
        if path.startswith(SKIP_DIRS) or any(("/" + d + "/") in ("/" + path) for d in SKIP_DIRS):
            continue
        if os.path.isdir(os.path.join(repo, path)):
            continue
        files.append(path)
    return files

def export(repo, name):
    files = changed_files(repo)
    dest_root = os.path.join(OVERLAY, name)
    if os.path.isdir(dest_root):
        shutil.rmtree(dest_root)
    for rel in files:
        src = os.path.join(repo, rel)
        dst = os.path.join(dest_root, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(src, dst)
    print(f"{name}: {len(files)} files")
    return files

def main():
    os.makedirs(OVERLAY, exist_ok=True)
    fw_head = git(FRAMEWORK, "rev-parse", "HEAD").strip()
    mp_head = git(MAFIAMP, "rev-parse", "HEAD").strip()
    fw_remote = git(FRAMEWORK, "remote", "get-url", "origin").strip()
    mp_remote = git(MAFIAMP, "remote", "get-url", "origin").strip()
    # MafiaMP is nested inside Framework; export it first and exclude it from the Framework export.
    mp_files = export(MAFIAMP, "MafiaMP")
    fw_files = [f for f in changed_files(FRAMEWORK) if not f.startswith("code/projects/MafiaMP")]
    dest_root = os.path.join(OVERLAY, "Framework")
    if os.path.isdir(dest_root):
        shutil.rmtree(dest_root)
    for rel in fw_files:
        dst = os.path.join(dest_root, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(os.path.join(FRAMEWORK, rel), dst)
    print(f"Framework: {len(fw_files)} files")
    with open(os.path.join(OVERLAY, "VERSIONS.txt"), "w", encoding="utf-8") as f:
        f.write(f"Framework {fw_remote} {fw_head}\nMafiaMP {mp_remote} {mp_head}\n")
        f.write("\nApply: clone Framework at that commit into G:\\MafiaCoop\\Framework, clone MafiaMP at its commit into\n"
                "Framework\\code\\projects\\MafiaMP, then run dev\\apply_overlay.cmd and dev\\build_all.cmd.\n")
    print("overlay written to", OVERLAY)

if __name__ == "__main__":
    main()
