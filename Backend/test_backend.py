import concurrent.futures
import json
from pathlib import Path
import subprocess
import tempfile
import threading
import unittest
import shutil
import socket
import ssl
import urllib.error
import urllib.request
import sys
import sqlite3
import time

from server import APIError, Server, Store, asset_path, load_config, run_server

class ServiceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.db = Path(self.temp.name) / "locks.sqlite3"
        self.store = Store(self.db)
        self.a = self.store.add_user("alice", "Alice", ["project"])
        self.b = self.store.add_user("bob", "Bob", ["project"])

    def tearDown(self):
        self.temp.cleanup()

    def call(self, action="claim", token=None, **fields):
        return self.store.execute(action, token or self.a, dict(protocol=1, repo="project", workspace="a",
            path="Content/Test.uasset", base="0" * 32, **fields))

    def test_simultaneous_claims_have_one_winner(self):
        barrier = threading.Barrier(2)
        def claim(token):
            barrier.wait()
            return self.call(token=token)
        with concurrent.futures.ThreadPoolExecutor() as pool:
            results = list(pool.map(claim, [self.a, self.b]))
        self.assertEqual(sum(r["ok"] for r in results), 1)
        self.assertEqual({r["lock"]["name"] for r in results}, {next(r["lock"]["name"] for r in results if r["ok"])})

    def test_persistence_and_reentrant_owner(self):
        first = self.call()
        self.store = Store(self.db)
        self.assertEqual(first["lock"]["generation"], self.call()["lock"]["generation"])
        self.assertFalse(self.call(token=self.b)["ok"])

    def test_same_user_other_workspace_is_conflict(self):
        self.call()
        result = self.store.execute("claim", self.a, dict(protocol=1, repo="project", workspace="other", path="content/test.uasset", base="0"*32))
        self.assertFalse(result["ok"])

    def test_paths_share_case_and_separator_key(self):
        self.call()
        body = dict(protocol=1, repo="project", workspace="other", path="CONTENT\\test.UASSET", base="0"*32)
        self.assertFalse(self.store.execute("claim", self.b, body)["ok"])

    def test_invalid_paths(self):
        for path in ("../a.uasset", "/a.uasset", "C:/a.umap", "Content/../a.umap", "Content//a.umap", "Content/a. /a.umap", "x.txt"):
            with self.subTest(path=path), self.assertRaises(APIError):
                asset_path(path)

    def test_identity_and_repository_authorization(self):
        with self.assertRaises(APIError) as error:
            self.call(token="invalid")
        self.assertEqual(error.exception.status, 401)
        with self.assertRaises(APIError) as error:
            self.store.execute("snapshot", self.a, dict(protocol=1, repo="other", workspace="a"))
        self.assertEqual(error.exception.status, 403)

    def test_non_owner_cannot_release(self):
        lock = self.call()["lock"]
        with self.assertRaises(APIError):
            self.call("release", self.b, generation=lock["generation"], fingerprint="1"*32)
        self.assertFalse(self.call(token=self.b)["ok"])

    def test_release_fencing_and_stale_checkout(self):
        first = self.call()["lock"]
        self.call("release", generation=first["generation"], fingerprint="1"*32)
        self.assertEqual(self.call(token=self.b)["reason"], "stale")
        body = dict(protocol=1, repo="project", workspace="a", path="Content/Test.uasset", base="1"*32)
        new = self.store.execute("claim", self.a, body)
        with self.assertRaises(APIError):
            self.call("release", generation=first["generation"], fingerprint="1"*32)
        self.assertNotEqual(first["generation"], new["lock"]["generation"])

    def test_offline_owner_does_not_expire(self):
        self.call()
        with self.store.connect() as db:
            db.execute("UPDATE locks SET seen=0")
        self.assertFalse(self.call(token=self.b)["ok"])
        snap = self.store.execute("snapshot", self.b, dict(protocol=1, repo="project", workspace="b"))
        self.assertFalse(snap["locks"][0]["online"])

    def test_heartbeat_and_disable_user(self):
        self.call()
        with self.store.connect() as db:
            db.execute("UPDATE locks SET seen=0")
        self.assertTrue(self.call("snapshot")["locks"][0]["online"])
        with self.store.connect() as db:
            db.execute("UPDATE users SET enabled=0 WHERE id='alice'")
        with self.assertRaises(APIError):
            self.call()
        self.assertFalse(self.call(token=self.b)["ok"])

    def test_recovery_is_fenced_and_audited(self):
        lock = self.call()["lock"]
        self.store.recover("project", "Content/Test.uasset", lock["generation"], "0"*32)
        self.assertTrue(self.call(token=self.b)["ok"])
        with self.store.connect() as db:
            self.assertEqual(db.execute("SELECT count(*) FROM audit WHERE action='admin_recover'").fetchone()[0], 1)

    def test_http_boundaries(self):
        server = Server(("127.0.0.1", 0), self.store)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            url = f"http://127.0.0.1:{server.server_port}/v1/claim"
            valid = json.dumps(dict(protocol=1, repo="project", workspace="a", path="Content/Test.uasset", base="0"*32)).encode()
            for data, token, status in [(valid, "bad", 401), (b"{", self.a, 400), (b"[]", self.a, 400), (b"x"*65537, self.a, 413)]:
                request = urllib.request.Request(url, data, {"Authorization": "Bearer " + token})
                with self.assertRaises(urllib.error.HTTPError) as error:
                    urllib.request.urlopen(request, timeout=2)
                self.assertEqual(error.exception.code, status)
            request = urllib.request.Request(url, valid, {"Authorization": "Bearer " + self.a})
            with urllib.request.urlopen(request, timeout=2) as response:
                self.assertTrue(json.load(response)["ok"])
        finally:
            server.shutdown()
            server.server_close()
            thread.join()



class ManagementTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.config = self.root / "private-service" / "config.json"
        self.script = Path(__file__).with_name("server.py")
        self.process = None

    def tearDown(self):
        if self.process and self.process.poll() is None:
            self.process.terminate()
            self.process.wait(timeout=10)
        if self.process and self.process.stdout:
            self.process.stdout.close()
        self.temp.cleanup()

    def cli(self, *args, check=True):
        result = subprocess.run([sys.executable, str(self.script), "--config", str(self.config), *args],
                                cwd=self.root, capture_output=True, text=True, timeout=15)
        if check:
            self.assertEqual(result.returncode, 0, result.stderr)
        return result

    def init(self):
        return self.cli("init", "--id", "alice", "--name", "Alice", "--repo", "project")

    def start(self, *args):
        self.process = subprocess.Popen([sys.executable, str(self.script), "--config", str(self.config), "serve", "--port", "0", *args],
            cwd=self.root, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
            future = pool.submit(self.process.stdout.readline)
            try:
                line = future.result(timeout=10)
            except concurrent.futures.TimeoutError:
                self.process.kill()
                self.process.wait(timeout=5)
                raise
        self.assertIn(" listening at ", line)
        return line.strip().split(" at ", 1)[1]

    def test_init_is_idempotent_and_paths_are_config_relative(self):
        result = self.init()
        credentials = self.config.parent / "credentials/alice.json"
        original = credentials.read_bytes()
        token = json.loads(original)["token"]
        self.assertNotIn(token, result.stdout + result.stderr)
        self.init()
        self.assertEqual(credentials.read_bytes(), original)
        self.assertTrue((self.config.parent / "data/locks.sqlite3").exists())
        self.assertFalse((self.root / "locks.sqlite3").exists())

    def test_rotation_requires_explicit_action(self):
        self.init()
        old = json.loads((self.config.parent / "credentials/alice.json").read_text())["token"]
        new_file = self.config.parent / "credentials/alice-new.json"
        denied = self.cli("add-user", "--id", "alice", "--name", "Alice", "--repo", "project", "--credential-file", str(new_file), check=False)
        self.assertNotEqual(denied.returncode, 0)
        self.assertFalse(new_file.exists())
        self.cli("add-user", "--id", "alice", "--name", "Alice", "--repo", "project", "--rotate", "--credential-file", str(new_file))
        new = json.loads(new_file.read_text())["token"]
        store = Store(self.config.parent / "data/locks.sqlite3")
        body = dict(protocol=1, repo="project", workspace="test")
        with self.assertRaises(APIError):
            store.execute("snapshot", old, body)
        self.assertTrue(store.execute("snapshot", new, body)["ok"])

    def test_backup_preserves_ownership_and_refuses_overwrite(self):
        self.init()
        token = json.loads((self.config.parent / "credentials/alice.json").read_text())["token"]
        store = Store(self.config.parent / "data/locks.sqlite3")
        store.execute("claim", token, dict(protocol=1, repo="project", workspace="a", path="Content/Test.uasset", base="0"*32))
        self.cli("backup")
        backups = list((self.config.parent / "backups").glob("*.sqlite3"))
        self.assertEqual(len(backups), 1)
        restored = Store(backups[0])
        snapshot = restored.execute("snapshot", token, dict(protocol=1, repo="project", workspace="a"))
        self.assertEqual(len(snapshot["locks"]), 1)
        denied = self.cli("backup", "--output", str(backups[0]), check=False)
        self.assertNotEqual(denied.returncode, 0)

    def test_foreground_service_health_status_and_restart(self):
        self.init()
        token = json.loads((self.config.parent / "credentials/alice.json").read_text())["token"]
        store = Store(self.config.parent / "data/locks.sqlite3")
        claim = dict(protocol=1, repo="project", workspace="a", path="Content/Test.uasset", base="0"*32)
        generation = store.execute("claim", token, claim)["lock"]["generation"]
        for _ in range(2):
            url = self.start()
            self.assertTrue(url.startswith("http://127.0.0.1:"))
            self.assertIn("Healthy", self.cli("status", "--url", url).stdout)
            self.assertEqual(store.execute("claim", token, claim)["lock"]["generation"], generation)
            self.process.terminate()
            self.process.wait(timeout=10)
            self.process.stdout.close()
        self.assertNotEqual(self.cli("status", "--url", url, check=False).returncode, 0)

    @unittest.skipUnless(shutil.which("openssl"), "Optional HTTPS test needs the OpenSSL CLI to generate a temporary certificate")
    def test_https_claims_and_idle_handshake_do_not_block_service(self):
        self.init()
        cert, key = self.root / "cert.pem", self.root / "key.pem"
        subprocess.run([shutil.which("openssl"), "req", "-x509", "-newkey", "rsa:2048", "-sha256", "-nodes",
            "-keyout", str(key), "-out", str(cert), "-subj", "/CN=localhost",
            "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1", "-days", "1"],
            capture_output=True, check=True, timeout=20)
        url = self.start("--certfile", str(cert), "--keyfile", str(key))
        self.assertTrue(url.startswith("https://"))
        context = ssl.create_default_context(cafile=str(cert))
        port = int(url.rsplit(":", 1)[1])
        with socket.create_connection(("127.0.0.1", port), timeout=2):
            # A client that never completes TLS must not block the accept loop.
            with urllib.request.urlopen(url + "/health", context=context, timeout=2) as response:
                self.assertTrue(json.load(response)["ok"])
            with self.assertRaises(urllib.error.URLError):
                urllib.request.urlopen(url + "/health", timeout=2)  # untrusted certificate
            store = Store(self.config.parent / "data/locks.sqlite3")
            alice = json.loads((self.config.parent / "credentials/alice.json").read_text())["token"]
            bob = store.add_user("bob", "Bob", ["project"])
            body = json.dumps(dict(protocol=1, repo="project", workspace="a", path="Content/Test.uasset", base="0"*32)).encode()
            for token, allowed in ((alice, True), (bob, False)):
                request = urllib.request.Request(url + "/v1/claim", body, {"Authorization": "Bearer " + token})
                with urllib.request.urlopen(request, context=context, timeout=2) as response:
                    result = json.load(response)
                self.assertEqual(result["ok"], allowed)
                self.assertEqual(result["lock"]["name"], "Alice")

    def test_invalid_config_and_plaintext_remote_binding_are_rejected(self):
        self.config.parent.mkdir()
        self.config.write_text('{"port": "8791"}')
        with self.assertRaises(ValueError):
            load_config(self.config)
        store = Store(self.root / "locks.sqlite3")
        with self.assertRaises(ValueError):
            run_server(store, "0.0.0.0", 0)
        with self.assertRaises(ValueError):
            run_server(store, "127.0.0.1", 70000)
        with self.assertRaises(ValueError):
            run_server(store, "127.0.0.1", 0, "certificate.pem", "")


if __name__ == "__main__":
    unittest.main(verbosity=2)
