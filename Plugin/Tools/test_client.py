import json
from pathlib import Path
import subprocess
import tempfile
import threading
import unittest
import sys

# Integration fixtures use the sibling backend; enrollment itself needs only this Plugin folder.
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'Backend'))
from server import Server, Store
from client import eligible, git, staged_assets, md5, connection, install_hook

class GitTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name) / "work"
        self.root.mkdir()
        self.remote = Path(self.temp.name) / "remote.git"
        subprocess.run(["git", "init", "--bare", str(self.remote)], check=True, capture_output=True)
        git(self.root, "init")
        git(self.root, "config", "user.name", "Test")
        git(self.root, "config", "user.email", "test@example.invalid")
        git(self.root, "config", "core.autocrlf", "false")
        self.asset = self.root / "Content" / "Test.uasset"
        self.asset.parent.mkdir()
        self.asset.write_bytes(b"original\x00")
        git(self.root, "add", ".")
        git(self.root, "commit", "-m", "initial")
        git(self.root, "remote", "add", "origin", str(self.remote))

    def tearDown(self):
        # Git files can be read-only on Windows.
        import shutil, os, stat
        def fix(function, path, _):
            os.chmod(path, stat.S_IWRITE)
            function(path)
        shutil.rmtree(self.temp.name, onerror=fix)
        self.temp.cleanup()

    def push(self):
        git(self.root, "push", "-u", "origin", "HEAD")

    def test_release_only_clean_published_files(self):
        path = "Content/Test.uasset"
        self.assertFalse(eligible(self.root, path))
        self.push()
        self.assertTrue(eligible(self.root, path))
        self.asset.write_bytes(b"modified\x00")
        self.assertFalse(eligible(self.root, path))
        git(self.root, "add", path)
        self.assertFalse(eligible(self.root, path))
        git(self.root, "commit", "-m", "local only")
        self.assertFalse(eligible(self.root, path))
        self.push()
        self.assertTrue(eligible(self.root, path))

    def test_untracked_and_delete(self):
        self.push()
        new = self.root / "Content" / "New.uasset"
        new.write_bytes(b"new\x00")
        self.assertFalse(eligible(self.root, "Content/New.uasset"))
        self.asset.unlink()
        self.assertFalse(eligible(self.root, "Content/Test.uasset"))

    def test_rename_checks_both_paths(self):
        git(self.root, "mv", "Content/Test.uasset", "Content/Renamed.uasset")
        self.assertEqual(set(staged_assets(self.root)), {"Content/Test.uasset", "Content/Renamed.uasset"})

    def test_real_lfs_asset_is_released_only_after_push(self):
        if git(self.root, "lfs", "version", check=False).returncode:
            self.skipTest("Git LFS is not installed")
        git(self.root, "lfs", "install", "--local")
        git(self.root, "lfs", "track", "*.uasset")
        git(self.root, "add", ".gitattributes")
        git(self.root, "add", "--renormalize", "Content/Test.uasset")
        git(self.root, "commit", "-m", "LFS asset")
        pointer = git(self.root, "show", "HEAD:Content/Test.uasset").stdout
        self.assertTrue(pointer.startswith(b"version https://git-lfs.github.com/spec/v1"))
        self.assertEqual(self.asset.read_bytes(), b"original\x00")
        self.assertFalse(eligible(self.root, "Content/Test.uasset"))
        self.push()
        self.assertTrue(eligible(self.root, "Content/Test.uasset"))

    def test_enrollment_and_staged_guard_end_to_end(self):
        store = Store(Path(self.temp.name) / "service.sqlite3")
        token = store.add_user("alice", "Alice", ["project"])
        server = Server(("127.0.0.1", 0), store)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        credentials = Path(self.temp.name) / "alice-credentials.json"
        credentials.write_text(json.dumps({"token": token}))
        tool = Path(__file__).with_name("client.py")
        def cli(*args):
            return subprocess.run([sys.executable, str(tool), "--project", str(self.root), *args], capture_output=True, timeout=15)
        try:
            hooks = self.root / ".custom-hooks"
            hooks.mkdir()
            hook = hooks / "pre-commit"
            existing = '#!/usr/bin/env bash\nprintf "existing-hook-ran\\n"\nexit 0\n'
            hook.write_text(existing, encoding="utf-8")
            git(self.root, "config", "core.hooksPath", ".custom-hooks")
            result = cli("configure", "--url", f"http://127.0.0.1:{server.server_port}", "--repo", "project",
                         "--credential-file", str(credentials), "--enable-project", "--install-hook")
            self.assertEqual(result.returncode, 0, result.stderr.decode())
            self.assertEqual(git(self.root, "config", "core.hooksPath").stdout.strip(), b".custom-hooks")
            self.assertIn(existing.split("\n", 1)[1], hook.read_text())
            self.assertEqual(cli("install-hook").returncode, 0)
            self.assertEqual(hook.read_text().count("# BEGIN Git Soft Lock"), 1)
            self.assertNotIn(token.encode(), git(self.root, "config", "--local", "--list").stdout)
            self.assertNotIn(token, (self.root / "Config" / "DefaultEditor.ini").read_text())
            self.asset.write_bytes(b"modified\x00")
            git(self.root, "add", "Content/Test.uasset")
            self.assertNotEqual(cli("check-staged").returncode, 0)
            denied = git(self.root, "hook", "run", "pre-commit", check=False)
            self.assertNotEqual(denied.returncode, 0)
            self.assertNotIn(b"existing-hook-ran", denied.stdout + denied.stderr)
            conn = connection(self.root)
            store.execute("claim", token, dict(protocol=1, repo="project", workspace=conn["workspace"], path="Content/Test.uasset", base=md5(self.asset)))
            self.assertEqual(cli("check-staged").returncode, 0)
            allowed = git(self.root, "hook", "run", "pre-commit")
            self.assertIn(b"existing-hook-ran", allowed.stdout + allowed.stderr)
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_non_shell_hook_is_preserved(self):
        hook = self.root / ".git" / "hooks" / "pre-commit"
        existing = "#!/usr/bin/env python3\nprint('custom hook')\n"
        hook.write_text(existing, encoding="utf-8")
        with self.assertRaisesRegex(RuntimeError, "not a shell script"):
            install_hook(self.root)
        self.assertEqual(hook.read_text(), existing)



if __name__ == "__main__":
    unittest.main(verbosity=2)
