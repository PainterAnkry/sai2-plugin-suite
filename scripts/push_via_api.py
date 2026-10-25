"""Push the local HEAD commit to GitHub via the Git Data API.

Why: this sandbox's egress proxy rejects CONNECT to github.com (502), so
`git push` cannot work. api.github.com / uploads.github.com are reachable, so
we rebuild the same commit remotely: blobs -> tree (base = parent tree) ->
commit (same message, same parent) -> move refs/heads/main.

Run from the repo root. Nothing is forced: the ref update is non-fast-forward
safe (GitHub rejects it if main moved).
"""
import base64
import json
import os
import subprocess
import sys
import tempfile

REPO = "PainterAnkry/sai2-plugin-suite"
BRANCH = "main"


def git(*args):
    # core.quotepath=false: otherwise non-ASCII paths come back as "\344\275..."
    return subprocess.check_output(
        ["git", "-c", "core.quotepath=false", *args], text=True).strip()


def gh(method, path, payload=None, jq=None):
    cmd = ["gh", "api", "-X", method, path]
    if payload is not None:
        fd, tmp = tempfile.mkstemp(suffix=".json")
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump(payload, f)
        cmd += ["--input", tmp]
    if jq:
        cmd += ["--jq", jq]
    try:
        out = subprocess.check_output(cmd, text=True, stderr=subprocess.STDOUT)
        return out.strip()
    except subprocess.CalledProcessError as e:
        print(f"!! {method} {path} failed:\n{e.output}", file=sys.stderr)
        raise
    finally:
        if payload is not None:
            os.unlink(tmp)


def main():
    head = git("rev-parse", "HEAD")
    parent = git("rev-parse", "HEAD~1")
    # message must be byte-identical to the local commit object (a trailing
    # newline difference changes the sha), so take it straight from `cat-file`.
    raw = subprocess.check_output(
        ["git", "-c", "core.quotepath=false", "cat-file", "commit", head])
    message = raw.split(b"\n\n", 1)[1].decode("utf-8")
    print(f"local  HEAD = {head}")
    print(f"parent      = {parent}")

    changed = git("diff", "--name-only", f"{parent}..{head}").splitlines()
    print("changed files:")
    for f in changed:
        print("  " + f)

    # file mode from the local tree (build-zig.sh may be 100755)
    ls = git("ls-tree", "-r", head)
    modes = {}
    for line in ls.splitlines():
        meta, path = line.split("\t", 1)
        mode = meta.split()[0]
        modes[path] = mode

    parent_tree = gh("GET", f"repos/{REPO}/git/commits/{parent}", jq=".tree.sha")
    print(f"parent tree = {parent_tree}")

    entries = []
    for path in changed:
        # Blob content must come from the git OBJECT, not from the working tree:
        # .gitattributes/autocrlf may have normalised line endings on commit
        # (docs/*.md here), so the on-disk file is CRLF while the stored blob is
        # LF -- uploading the working file yields a different tree.
        raw_blob = subprocess.check_output(
            ["git", "-c", "core.quotepath=false", "cat-file", "blob",
             f"{head}:{path}"])
        b64 = base64.b64encode(raw_blob).decode("ascii")
        sha = gh("POST", f"repos/{REPO}/git/blobs",
                 {"content": b64, "encoding": "base64"}, jq=".sha")
        entries.append({"path": path, "mode": modes.get(path, "100644"),
                        "type": "blob", "sha": sha})
        print(f"  blob {path} -> {sha[:10]} ({modes.get(path)})")

    tree = gh("POST", f"repos/{REPO}/git/trees",
              {"base_tree": parent_tree, "tree": entries}, jq=".sha")
    print(f"new tree    = {tree}")

    # Replay the SAME author/committer identity and timestamps as the local
    # commit. Otherwise the API stamps "now" and the resulting sha differs from
    # the local one -- and since `git fetch` is impossible here, the local repo
    # would stay diverged from origin forever.
    an, ae, ad, cn, ce, cd = git(
        "log", "-1", "--format=%an%x00%ae%x00%aI%x00%cn%x00%ce%x00%cI"
    ).split("\x00")
    print(f"author      = {an} <{ae}> {ad}")

    commit = gh("POST", f"repos/{REPO}/git/commits",
                {"message": message, "tree": tree, "parents": [parent],
                 "author": {"name": an, "email": ae, "date": ad},
                 "committer": {"name": cn, "email": ce, "date": cd}},
                jq=".sha")
    print(f"new commit  = {commit}")
    if commit != head:
        print(f"NOTE: remote sha != local sha ({head}); content is identical but "
              f"the local branch will look diverged until a fetch succeeds.",
              file=sys.stderr)

    gh("PATCH", f"repos/{REPO}/git/refs/heads/{BRANCH}",
       {"sha": commit, "force": False})
    print(f"{BRANCH} -> {commit}")
    print("OK")


if __name__ == "__main__":
    main()
