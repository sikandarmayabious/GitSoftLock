"""Configure an Unreal workspace and validate Git changes without modifying project assets."""
from __future__ import annotations

import argparse
import configparser
import hashlib
import json
import re
from pathlib import Path
import subprocess
import sys
import urllib.request
import urllib.parse
import uuid


def git(root, *args, check=True):
    return subprocess.run(["git", "-C", str(root), *args], check=check, capture_output=True, timeout=30)


def md5(path):
    if not path.exists():
        return "missing"
    with path.open("rb") as file:
        return hashlib.file_digest(file, "md5").hexdigest()


def connection(project):
    config = configparser.ConfigParser(strict=False, interpolation=None)
    config.read(project / "Config" / "DefaultEditor.ini", encoding="utf-8-sig")
    section = "/Script/GitSoftLockEditor.GitSoftLockSettings"
    if not config.has_section(section) or config.get(section, "bEnabled", fallback="false").lower() != "true":
        raise RuntimeError("Shared project protection is not enabled in Config/DefaultEditor.ini")
    local = json.loads((project / "Saved" / "GitSoftLock" / "client.json").read_text(encoding="utf-8-sig"))
    local["url"] = config.get(section, "ServerUrl").strip('"').rstrip("/")
    local["repo"] = config.get(section, "RepositoryId").strip('"')
    return local


def request(conn, action, **fields):
    url = urllib.parse.urlparse(conn["url"])
    if url.username or url.password or url.query or url.fragment:
        raise RuntimeError("Use a plain server URL without embedded credentials")
    if url.scheme != "https" and not (url.scheme == "http" and url.hostname in ("127.0.0.1", "localhost")):
        raise RuntimeError("HTTPS is required outside loopback development")
    body = dict(protocol=1, repo=conn["repo"], workspace=conn["workspace"], **fields)
    req = urllib.request.Request(conn["url"] + "/v1/" + action, json.dumps(body).encode(),
                                 {"Authorization": "Bearer " + conn["token"], "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=5) as response:
        result = json.load(response)
    if result.get("protocol") != 1:
        raise RuntimeError("Unsupported service response")
    return result


def eligible(root, path):
    """True only if this asset is clean and already matches the tracking branch."""
    if git(root, "rev-parse", "--verify", "@{upstream}", check=False).returncode:
        return False
    if git(root, "status", "--porcelain=v1", "--untracked-files=all", "--", path).stdout.strip():
        return False
    return git(root, "diff", "--quiet", "@{upstream}", "--", path, check=False).returncode == 0


def staged_assets(root):
    # Disable rename detection: both old and new paths require ownership.
    raw = git(root, "diff", "--cached", "--name-only", "--no-renames", "-z").stdout
    return [p.decode("utf-8") for p in raw.split(b"\0") if p and p.lower().endswith((b".uasset", b".umap"))]


def install_hook(root):
    """Add our guard to the consuming repo's active shell hook without changing core.hooksPath."""
    filename = Path(git(root, "rev-parse", "--path-format=absolute", "--git-path", "hooks/pre-commit").stdout.decode().strip())
    template = Path(__file__).resolve().parents[1] / "Hooks" / "pre-commit"
    body = template.read_text(encoding="utf-8").split("\n", 1)[1]
    text = filename.read_text(encoding="utf-8") if filename.exists() else "#!/usr/bin/env sh\n"
    first, _, rest = text.partition("\n")
    if not first.startswith("#!") or not re.search(r"(?:/|\s)(?:ba|da|z|k)?sh(?:\s|$)", first):
        raise RuntimeError("Existing pre-commit hook is not a shell script. Chain Hooks/pre-commit manually; it was left unchanged.")
    begin, end = "# BEGIN Git Soft Lock", "# END Git Soft Lock"
    if text.count(begin) != text.count(end) or text.count(begin) > 1:
        raise RuntimeError("Existing Git Soft Lock hook markers are inconsistent; hook left unchanged")
    if begin in text:
        text, count = re.subn(r"(?ms)^# BEGIN Git Soft Lock\n.*?^# END Git Soft Lock(?:\n|$)", lambda _: body, text)
        if count != 1:
            raise RuntimeError("Existing Git Soft Lock hook block is malformed; hook left unchanged")
    else:
        text = first + "\n" + body + "\n" + rest
    filename.parent.mkdir(parents=True, exist_ok=True)
    filename.write_text(text, encoding="utf-8", newline="\n")
    filename.chmod(filename.stat().st_mode | 0o111)
    return filename


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, default=Path.cwd())
    sub = parser.add_subparsers(dest="command", required=True)
    configure = sub.add_parser("configure")
    configure.add_argument("--credential-file", type=Path, required=True)
    configure.add_argument("--url", required=True)
    configure.add_argument("--repo", required=True)
    configure.add_argument("--enable-project", action="store_true", help="Write shared policy to DefaultEditor.ini")
    configure.add_argument("--install-hook", action="store_true", help="Add the bundled guard to this project's existing shell pre-commit hook")
    sub.add_parser("install-hook", help="Install/update the optional guard in the selected project's active shell hook")
    sub.add_parser("status")
    sub.add_parser("check-staged")
    args = parser.parse_args()
    project = args.project.resolve()
    root = Path(git(project, "rev-parse", "--show-toplevel").stdout.decode().strip())
    if args.command == "configure":
        token = json.loads(args.credential_file.read_text(encoding="utf-8-sig"))["token"]
        directory = project / "Saved" / "GitSoftLock"
        directory.mkdir(parents=True, exist_ok=True)
        filename = directory / "client.json"
        old = json.loads(filename.read_text()) if filename.exists() else {}
        workspace_file = directory / "workspace.txt"
        workspace = old.get("workspace") or (workspace_file.read_text().strip() if workspace_file.exists() else str(uuid.uuid4()))
        conn = dict(url=args.url.rstrip("/"), repo=args.repo, workspace=workspace, token=token)
        result = request(conn, "snapshot")
        if not result.get("ok"):
            raise RuntimeError("Service rejected the workspace")
        # No secrets enter a tracked file or command-line Git config value.
        filename.write_text(json.dumps(dict(token=token, workspace=workspace), indent=2), encoding="utf-8")
        filename.chmod(0o600)
        workspace_file.write_text(workspace, encoding="utf-8")
        if args.enable_project:
            if any(c in args.url + args.repo for c in '\r\n"'):
                raise RuntimeError("Invalid configuration characters")
            ini = project / "Config" / "DefaultEditor.ini"
            text = ini.read_text(encoding="utf-8-sig") if ini.exists() else ""
            section = "[/Script/GitSoftLockEditor.GitSoftLockSettings]"
            # Replace only our section; Unreal INI files may contain repeated keys elsewhere.
            text = re.sub(r"(?ms)^\[/Script/GitSoftLockEditor.GitSoftLockSettings\]\s*\n.*?(?=^\[|\Z)", "", text)
            text += f'\n{section}\nbEnabled=True\nServerUrl="{conn["url"]}"\nRepositoryId="{conn["repo"]}"\n'
            ini.parent.mkdir(parents=True, exist_ok=True)
            ini.write_text(text, encoding="utf-8")
        git(root, "config", "softlock.enabled", "true")
        git(root, "config", "softlock.python", sys.executable)
        git(root, "config", "softlock.project", str(project))
        git(root, "config", "softlock.client", str(Path(__file__).resolve()))
        if args.install_hook:
            print(f"Installed optional Git guard: {install_hook(root)}")
        print(f"Configured for {result['name']}. Restart Unreal. Keep Saved/GitSoftLock private.")
    elif args.command == "install-hook":
        git(root, "config", "softlock.python", sys.executable)
        git(root, "config", "softlock.project", str(project))
        git(root, "config", "softlock.client", str(Path(__file__).resolve()))
        print(f"Installed optional Git guard: {install_hook(root)}. Enrollment enables it for this clone.")
    elif args.command == "status":
        result = request(connection(project), "snapshot")
        for lock in result.get("locks", []):
            print(f"{lock['path']} | {lock['name']} | {'this workspace' if lock['mine'] else 'other workspace'} | {'online' if lock['online'] else 'offline'}")
    elif args.command == "check-staged":
        paths = staged_assets(root)
        if not paths:
            return
        snapshot = request(connection(project), "snapshot")
        if not snapshot.get("ok"):
            raise RuntimeError("Could not verify lock ownership")
        locks = {l["path"]: l for l in snapshot.get("locks", [])}
        denied = []
        for path in paths:
            lock = locks.get(path.replace("\\", "/").casefold())
            if not lock or not lock.get("mine"):
                denied.append(f"{path}: " + (f"locked by {lock['name']}" if lock else "no lock; acquire in Unreal before committing"))
        if denied:
            raise RuntimeError("Commit blocked by Git Soft Lock:\n" + "\n".join(denied))
        print(f"Git Soft Lock: verified ownership of {len(paths)} staged asset paths.")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"Git Soft Lock: {error}", file=sys.stderr)
        sys.exit(1)
