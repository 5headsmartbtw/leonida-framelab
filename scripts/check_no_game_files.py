"""Check Git trees for known indicators of excluded game files and payloads."""

import argparse
from functools import lru_cache
from pathlib import PurePosixPath
import subprocess


FORBIDDEN_SUFFIXES = {
    ".rpf", ".awc", ".ydd", ".ydr", ".yft", ".ymap", ".ytd",
    ".pak", ".pkg", ".xex", ".xvc", ".nsp",
    ".exe", ".dll", ".lib", ".obj", ".pdb", ".elf", ".so", ".dylib",
    ".cubin", ".fatbin", ".ptx", ".spv", ".rgba", ".rgba16f", ".bin", ".dump",
    ".zip", ".7z", ".rar", ".tar", ".gz", ".bz2", ".xz", ".zst", ".iso",
}
GAME_PATH_COMPONENTS = {
    "gta6", "gtavi", "gta vi", "grand theft auto vi",
    "gamefiles", "game_files", "game-files", "game files",
}
PAYLOAD_PREFIXES = (
    (b"RPF7", "RPF archive signature"),
    (b"RPF8", "RPF archive signature"),
    (b"MZ", "Windows executable signature"),
    (b"\x7fELF", "ELF binary signature"),
    (b"PK\x03\x04", "ZIP signature"),
    (b"PK\x05\x06", "ZIP signature"),
    (b"7z\xbc\xaf\x27\x1c", "7-Zip signature"),
    (b"Rar!\x1a\x07", "RAR signature"),
    (b"\x1f\x8b", "gzip signature"),
    (b"version https://git-lfs.github.com/spec/v1", "Git LFS pointer"),
)
PREFIX_SIZE = max(len(signature) for signature, _ in PAYLOAD_PREFIXES)


def git(*args):
    return subprocess.check_output(["git", *args])


@lru_cache(maxsize=None)
def blob_violation(object_id):
    # The prefix is enough for these checks. Do not unpack untrusted archives.
    with subprocess.Popen(
        ["git", "cat-file", "blob", object_id],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    ) as process:
        prefix = process.stdout.read(PREFIX_SIZE)
        if len(prefix) == PREFIX_SIZE and process.poll() is None:
            process.kill()
        elif process.wait():
            detail = process.stderr.read().decode("utf-8", errors="replace").strip()
            raise RuntimeError(f"Could not read Git blob {object_id}: {detail}")
    return next((label for signature, label in PAYLOAD_PREFIXES if prefix.startswith(signature)), None)


def check_tree(commit):
    for record in git("ls-tree", "-r", "-z", commit).split(b"\0"):
        if not record:
            continue
        metadata, filename = record.split(b"\t", 1)
        mode, _, object_id = metadata.split()
        name = filename.decode("utf-8", errors="backslashreplace")
        path = PurePosixPath(name.casefold())
        if mode not in (b"100644", b"100755"):
            reason = "symlinks and submodules are not accepted"
        elif path.suffix in FORBIDDEN_SUFFIXES or path.name.startswith("nvsdk_ngx"):
            reason = "excluded game or binary payload type"
        elif GAME_PATH_COMPONENTS.intersection(path.parts):
            reason = "excluded game files path"
        else:
            reason = blob_violation(object_id.decode("ascii"))
        if reason:
            yield name, reason


def resolve_commit(ref):
    return git("rev-parse", "--verify", "--end-of-options", ref + "^{commit}").decode("ascii").strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--head", default="HEAD", help="commit to inspect (default: HEAD)")
    parser.add_argument("--base", help="also inspect all commits reachable from head but not base")
    args = parser.parse_args()
    head = resolve_commit(args.head)
    commits = {head: None}
    if args.base:
        base = resolve_commit(args.base)
        for commit in git("rev-list", f"{base}..{head}").decode("ascii").splitlines():
            commits[commit] = None

    violations = {}
    for commit in commits:
        for name, reason in check_tree(commit):
            violations.setdefault((name, reason), commit)
    for (name, reason), commit in sorted(violations.items()):
        print(f"{commit[:12]} {name!r}: {reason}")
    if violations:
        print("FAIL No Game Files. Remove excluded material from every submitted commit.")
        return 1
    print(f"PASS No Game Files indicators ({len(commits)} commit tree(s))")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
