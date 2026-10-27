"""Git Soft Lock backend for Ubuntu/Termux or Windows. Python standard library only."""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import signal
import sqlite3
import ssl
import sys
import threading
import time
import unicodedata
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PROTOCOL = 1
MAX_BODY = 64 * 1024
BACKEND_DIR = Path(__file__).resolve().parent
DEFAULT_CONFIG = {"bind": "127.0.0.1", "port": 8791, "database": "data/locks.sqlite3",
                  "certfile": "", "keyfile": ""}


class APIError(Exception):
    def __init__(self, status, message):
        self.status, self.message = status, message


def identifier(value, label):
    if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z0-9_.:@-]{1,160}", value):
        raise APIError(400, f"Invalid {label}")
    return value


def asset_path(value):
    if not isinstance(value, str) or not 1 <= len(value) <= 1024:
        raise APIError(400, "Invalid path")
    value = unicodedata.normalize("NFC", value.replace("\\", "/"))
    parts = value.split("/")
    if any(p in ("", ".", "..") or p.endswith((".", " ")) for p in parts):
        raise APIError(400, "Path must be repository relative without traversal")
    if any(ord(c) < 32 or c in ':*?"<>|' for c in value):
        raise APIError(400, "Invalid path characters")
    if not value.lower().endswith((".uasset", ".umap")):
        raise APIError(400, "Only Unreal asset and map packages are supported")
    # Unreal projects shared with Windows must not have case-only duplicate paths.
    return value.casefold()


def fingerprint(value):
    if value != "missing" and (not isinstance(value, str) or not re.fullmatch(r"[0-9a-fA-F]{32}", value)):
        raise APIError(400, "Invalid content fingerprint")
    return value.lower()


class Store:
    def __init__(self, filename):
        self.filename = str(filename)
        Path(filename).parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        with self.connect() as db:
            db.executescript("""
                PRAGMA journal_mode=WAL;
                CREATE TABLE IF NOT EXISTS users (
                    id TEXT PRIMARY KEY, name TEXT NOT NULL, token_hash TEXT UNIQUE NOT NULL,
                    repos TEXT NOT NULL, enabled INTEGER NOT NULL DEFAULT 1);
                CREATE TABLE IF NOT EXISTS locks (
                    repo TEXT NOT NULL, path TEXT NOT NULL, owner TEXT NOT NULL,
                    workspace TEXT NOT NULL, generation TEXT NOT NULL, base TEXT NOT NULL,
                    created REAL NOT NULL, seen REAL NOT NULL, PRIMARY KEY(repo,path));
                CREATE TABLE IF NOT EXISTS published (
                    repo TEXT NOT NULL, path TEXT NOT NULL, fingerprint TEXT NOT NULL,
                    PRIMARY KEY(repo,path));
                CREATE TABLE IF NOT EXISTS audit (
                    id INTEGER PRIMARY KEY, timestamp REAL NOT NULL, action TEXT NOT NULL,
                    repo TEXT, path TEXT, owner TEXT, workspace TEXT, detail TEXT);
            """)

    @contextmanager
    def connect(self):
        db = sqlite3.connect(self.filename, timeout=10)
        db.row_factory = sqlite3.Row
        db.execute("PRAGMA synchronous=FULL")
        try:
            with db:
                yield db
        finally:
            db.close()

    def add_user(self, user_id, name, repos, token=None):
        identifier(user_id, "user ID")
        if not name.strip() or len(name) > 160:
            raise ValueError("Name must contain 1-160 characters")
        for repo in repos:
            identifier(repo, "repository")
        token = token or secrets.token_urlsafe(32)
        with self.connect() as db:
            db.execute("INSERT INTO users VALUES(?,?,?,?,1) ON CONFLICT(id) DO UPDATE SET "
                       "name=excluded.name,token_hash=excluded.token_hash,repos=excluded.repos,enabled=1",
                       (user_id, name, hashlib.sha256(token.encode()).hexdigest(), json.dumps(repos)))
        return token

    @staticmethod
    def audit(db, action, repo, path, owner, workspace, detail=""):
        db.execute("INSERT INTO audit(timestamp,action,repo,path,owner,workspace,detail) VALUES(?,?,?,?,?,?,?)",
                   (time.time(), action, repo, path, owner, workspace, detail))

    def execute(self, action, token, body):
        if not isinstance(body, dict) or body.get("protocol") != PROTOCOL:
            raise APIError(400, "Unsupported protocol")
        repo = identifier(body.get("repo"), "repository")
        workspace = identifier(body.get("workspace"), "workspace")
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            user = db.execute("SELECT * FROM users WHERE token_hash=? AND enabled=1",
                              (hashlib.sha256(token.encode()).hexdigest(),)).fetchone()
            if not user:
                raise APIError(401, "Invalid credentials")
            if repo not in json.loads(user["repos"]):
                raise APIError(403, "Repository access denied")
            owner = user["id"]
            now = time.time()
            if action == "snapshot":
                db.execute("UPDATE locks SET seen=? WHERE repo=? AND owner=? AND workspace=?",
                           (now, repo, owner, workspace))
                rows = db.execute("SELECT l.*,u.name FROM locks l JOIN users u ON u.id=l.owner WHERE repo=? ORDER BY path",
                                  (repo,)).fetchall()
                return {"protocol": PROTOCOL, "ok": True, "user": owner, "name": user["name"],
                        "locks": [self.view(r, owner, workspace, now) for r in rows]}
            path = asset_path(body.get("path"))
            row = db.execute("SELECT l.*,u.name FROM locks l JOIN users u ON u.id=l.owner WHERE repo=? AND path=?",
                             (repo, path)).fetchone()
            if action == "claim":
                base = fingerprint(body.get("base"))
                if row:
                    mine = row["owner"] == owner and row["workspace"] == workspace
                    if mine:
                        db.execute("UPDATE locks SET seen=? WHERE repo=? AND path=?", (now, repo, path))
                    return {"protocol": PROTOCOL, "ok": mine, "reason": "owned" if mine else "locked",
                            "lock": self.view(row, owner, workspace, now)}
                published = db.execute("SELECT fingerprint FROM published WHERE repo=? AND path=?", (repo, path)).fetchone()
                if published and published[0] != base:
                    return {"protocol": PROTOCOL, "ok": False, "reason": "stale",
                            "message": "Local content is older or different. Pull the published asset and reload it before editing."}
                generation = secrets.token_hex(16)
                db.execute("INSERT INTO locks VALUES(?,?,?,?,?,?,?,?)",
                           (repo, path, owner, workspace, generation, base, now, now))
                self.audit(db, "claim", repo, path, owner, workspace, generation)
                return {"protocol": PROTOCOL, "ok": True, "reason": "acquired", "lock": {
                    "path": path, "owner": owner, "name": user["name"], "workspace": workspace,
                    "generation": generation, "base": base, "mine": True, "online": True}}
            if action == "release":
                generation = identifier(body.get("generation"), "generation")
                content = fingerprint(body.get("fingerprint"))
                if not row:
                    return {"protocol": PROTOCOL, "ok": True, "reason": "already_released"}
                if row["owner"] != owner or row["workspace"] != workspace or row["generation"] != generation:
                    raise APIError(409, "Only the owning workspace with the current generation may release")
                db.execute("INSERT INTO published VALUES(?,?,?) ON CONFLICT(repo,path) DO UPDATE SET fingerprint=excluded.fingerprint",
                           (repo, path, content))
                db.execute("DELETE FROM locks WHERE repo=? AND path=?", (repo, path))
                self.audit(db, "release", repo, path, owner, workspace, content)
                return {"protocol": PROTOCOL, "ok": True}
            raise APIError(404, "Unknown operation")

    @staticmethod
    def view(row, owner, workspace, now):
        return {"path": row["path"], "owner": row["owner"], "name": row["name"],
                "workspace": row["workspace"], "generation": row["generation"], "base": row["base"],
                "mine": row["owner"] == owner and row["workspace"] == workspace,
                "online": now - row["seen"] < 30, "last_seen": row["seen"]}

    def recover(self, repo, path, expected_generation, content):
        """Host-only administrative recovery. Caller must stop the previous editor first."""
        repo, path = identifier(repo, "repository"), asset_path(path)
        content = fingerprint(content)
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            row = db.execute("SELECT * FROM locks WHERE repo=? AND path=?", (repo, path)).fetchone()
            if not row or row["generation"] != expected_generation:
                raise APIError(409, "Generation changed; inspect the lock again")
            db.execute("DELETE FROM locks WHERE repo=? AND path=?", (repo, path))
            db.execute("INSERT INTO published VALUES(?,?,?) ON CONFLICT(repo,path) DO UPDATE SET fingerprint=excluded.fingerprint",
                       (repo, path, content))
            self.audit(db, "admin_recover", repo, path, row["owner"], row["workspace"], content)


class Server(ThreadingHTTPServer):
    daemon_threads = True
    request_queue_size = 64

    def __init__(self, address, store):
        self.store = store
        self.tls_context = None
        self.slots = threading.BoundedSemaphore(32)
        super().__init__(address, Handler)

    def process_request(self, request, address):
        if not self.slots.acquire(blocking=False):
            self.shutdown_request(request)
            return
        try:
            super().process_request(request, address)
        except Exception:
            self.slots.release()
            raise

    def process_request_thread(self, request, address):
        try:
            if self.tls_context is not None:
                # Handshake in a bounded worker, never in accept(): one idle TLS
                # connection must not stop other editors authorizing their saves.
                request.settimeout(10)
                request = self.tls_context.wrap_socket(request, server_side=True, do_handshake_on_connect=False)
                try:
                    request.do_handshake()
                except OSError:
                    self.shutdown_request(request)
                    return
            super().process_request_thread(request, address)
        finally:
            self.slots.release()


class Handler(BaseHTTPRequestHandler):
    server_version = "GitSoftLock/1"

    def setup(self):
        super().setup()
        self.connection.settimeout(10)

    def log_message(self, fmt, *args):
        # Do not print credentials, arbitrary bodies, or user-supplied path strings.
        pass

    def respond(self, status, body):
        data = json.dumps(body, ensure_ascii=True).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path != "/health":
            self.respond(404, {"protocol": PROTOCOL, "ok": False})
            return
        try:
            with self.server.store.connect() as db:
                db.execute("SELECT path FROM locks LIMIT 1").fetchone()
            self.respond(200, {"protocol": PROTOCOL, "ok": True})
        except sqlite3.Error:
            self.respond(503, {"protocol": PROTOCOL, "ok": False})

    def do_POST(self):
        try:
            if self.path not in ("/v1/snapshot", "/v1/claim", "/v1/release"):
                raise APIError(404, "Unknown endpoint")
            if self.headers.get("Transfer-Encoding"):
                raise APIError(400, "Chunked requests are not supported")
            length = int(self.headers.get("Content-Length", "0"))
            if not 0 < length <= MAX_BODY:
                raise APIError(413, "Invalid request size")
            auth = self.headers.get("Authorization", "")
            if not auth.startswith("Bearer ") or len(auth) > 1024:
                raise APIError(401, "Bearer credentials required")
            data = self.rfile.read(length)
            if len(data) != length:
                raise APIError(400, "Incomplete request")
            body = json.loads(data)
            result = self.server.store.execute(self.path.rsplit("/", 1)[1], auth[7:], body)
            self.respond(200, result)
        except APIError as error:
            self.respond(error.status, {"protocol": PROTOCOL, "ok": False, "message": error.message})
        except (ValueError, UnicodeError):
            self.respond(400, {"protocol": PROTOCOL, "ok": False, "message": "Malformed JSON or length"})
        except (TimeoutError, ConnectionError, BrokenPipeError):
            pass
        except Exception:
            self.respond(503, {"protocol": PROTOCOL, "ok": False, "message": "Storage unavailable; saves must remain blocked"})


def load_config(filename):
    config = dict(DEFAULT_CONFIG)
    if filename.exists():
        supplied = json.loads(filename.read_text(encoding="utf-8-sig"))
        if not isinstance(supplied, dict) or set(supplied) - set(config):
            raise ValueError("Config must be an object with bind, port, database, certfile and keyfile fields")
        config.update(supplied)
    if not isinstance(config["bind"], str) or not config["bind"]:
        raise ValueError("Config bind must be an IPv4 address or hostname")
    if type(config["port"]) is not int or not 0 <= config["port"] <= 65535:
        raise ValueError("Config port must be an integer between 0 and 65535")
    if not isinstance(config["database"], str) or not config["database"]:
        raise ValueError("Config database must be a nonempty path")
    for key in ("database", "certfile", "keyfile"):
        if not isinstance(config[key], str):
            raise ValueError(f"Config {key} must be a path string")
        if config[key]:
            path = Path(config[key]).expanduser()
            config[key] = str(path if path.is_absolute() else filename.parent / path)
    return config


def credential_path(config_file, user_id, explicit):
    name = re.sub(r"[^A-Za-z0-9_-]", "_", identifier(user_id, "user ID"))
    return Path(explicit).expanduser().resolve() if explicit else config_file.parent / "credentials" / (name + ".json")


def provision_user(store, user_id, name, repos, filename, *, rotate=False, allow_existing=False):
    """Never print a token or silently rotate a teammate's credentials."""
    identifier(user_id, "user ID")
    for repo in repos:
        identifier(repo, "repository")
    if not name.strip() or len(name) > 160:
        raise ValueError("Name must contain 1-160 characters")
    with store.connect() as db:
        existing = db.execute("SELECT * FROM users WHERE id=?", (user_id,)).fetchone()
    if existing and not rotate:
        if allow_existing and filename.exists():
            saved = json.loads(filename.read_text(encoding="utf-8-sig"))
            digest = hashlib.sha256(saved.get("token", "").encode()).hexdigest()
            if digest == existing["token_hash"] and existing["enabled"] and set(json.loads(existing["repos"])) == set(repos) and existing["name"] == name:
                print(f"Existing user retained. Private credential file: {filename}")
                return
        raise ValueError("User already exists. Use add-user --rotate and a NEW credential filename to replace credentials")
    filename.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    fd = os.open(filename, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as file:
            token = store.add_user(user_id, name, repos)
            json.dump({"token": token}, file)
    except Exception:
        # Only the empty file exclusively created by this operation is removed.
        if filename.exists() and filename.stat().st_size == 0:
            filename.unlink()
        raise
    print(f"User ready. Private credential file: {filename}")


def run_server(store, bind, port, certfile="", keyfile=""):
    if type(port) is not int or not 0 <= port <= 65535:
        raise ValueError("Port must be between 0 and 65535")
    if bool(certfile) != bool(keyfile):
        raise ValueError("HTTPS requires both certfile and keyfile")
    if not certfile and bind not in ("127.0.0.1", "localhost"):
        raise ValueError("For remote access use an SSH tunnel, or provide a trusted TLS certificate and key")
    server = Server((bind, port), store)
    try:
        if certfile:
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.minimum_version = ssl.TLSVersion.TLSv1_2
            context.load_cert_chain(certfile, keyfile)
            server.tls_context = context
        scheme = "https" if certfile else "http"
        print(f"Git Soft Lock listening at {scheme}://{bind}:{server.server_port}", flush=True)
        print(f"Database: {store.filename}\nStop with Ctrl+C. Locks persist after stopping.", flush=True)
        previous = None
        if threading.current_thread() is threading.main_thread():
            # shutdown() must execute on a different thread from serve_forever().
            previous = signal.signal(signal.SIGTERM, lambda *_: threading.Thread(target=server.shutdown, daemon=True).start())
        try:
            server.serve_forever(poll_interval=0.25)
        except KeyboardInterrupt:
            pass
        finally:
            if previous is not None:
                signal.signal(signal.SIGTERM, previous)
    finally:
        server.server_close()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=Path(os.environ.get("GIT_SOFT_LOCK_CONFIG", BACKEND_DIR / "config.json")))
    parser.add_argument("--db", default=os.environ.get("GIT_SOFT_LOCK_DB"), help="Override the configured database path")
    sub = parser.add_subparsers(dest="command", required=True)
    init = sub.add_parser("init", help="Create local config, database and first user; safe to repeat with the same user")
    init.add_argument("--id", required=True)
    init.add_argument("--name", required=True)
    init.add_argument("--repo", action="append", required=True)
    init.add_argument("--credential-file")
    serve = sub.add_parser("serve")
    serve.add_argument("--bind")
    serve.add_argument("--port", type=int)
    serve.add_argument("--certfile")
    serve.add_argument("--keyfile")
    status = sub.add_parser("status", help="Check the running service and its database")
    status.add_argument("--url", help="Override health-check origin, e.g. https://locks.example.com:8791")
    user = sub.add_parser("add-user")
    user.add_argument("--id", required=True)
    user.add_argument("--name", required=True)
    user.add_argument("--repo", action="append", required=True)
    user.add_argument("--credential-file", help="New private file; defaults to credentials/USER.json beside config")
    user.add_argument("--rotate", action="store_true", help="Explicitly replace an existing user's token; use a new credential filename")
    disable = sub.add_parser("disable-user")
    disable.add_argument("--id", required=True)
    sub.add_parser("list-locks")
    backup = sub.add_parser("backup")
    backup.add_argument("--output", help="Defaults to a new timestamped file under backups/ beside config")
    recover = sub.add_parser("recover")
    recover.add_argument("--repo", required=True)
    recover.add_argument("--path", required=True)
    recover.add_argument("--generation", required=True)
    recover.add_argument("--fingerprint", required=True)
    recover.add_argument("--old-editor-stopped", action="store_true", required=True)
    args = parser.parse_args(argv)
    config_file = args.config.expanduser().resolve()
    config = load_config(config_file)
    if args.command == "status":
        host = "127.0.0.1" if config["bind"] == "0.0.0.0" else config["bind"]
        origin = args.url or f"{'https' if config['certfile'] else 'http'}://{host}:{config['port']}"
        with urllib.request.urlopen(origin.rstrip("/") + "/health", timeout=3) as response:
            result = json.load(response)
        if result.get("protocol") != PROTOCOL or not result.get("ok"):
            raise ValueError("Service health check failed")
        print(f"Healthy: {origin}")
        return
    if args.command == "init" and not config_file.exists():
        config_file.parent.mkdir(parents=True, exist_ok=True)
        with config_file.open("x", encoding="utf-8") as file:
            json.dump(DEFAULT_CONFIG, file, indent=2)
            file.write("\n")
    store = Store(Path(args.db).expanduser().resolve() if args.db else config["database"])
    if args.command == "serve":
        run_server(store, args.bind or config["bind"], args.port if args.port is not None else config["port"],
                   args.certfile if args.certfile is not None else config["certfile"],
                   args.keyfile if args.keyfile is not None else config["keyfile"])
    elif args.command in ("init", "add-user"):
        provision_user(store, args.id, args.name, args.repo, credential_path(config_file, args.id, args.credential_file),
                       rotate=getattr(args, "rotate", False), allow_existing=args.command == "init")
        if args.command == "init":
            print(f"Config: {config_file}\nReady. Start with: python3 server.py --config \"{config_file}\" serve")
    elif args.command == "disable-user":
        with store.connect() as db:
            db.execute("UPDATE users SET enabled=0 WHERE id=?", (args.id,))
        print("Credentials disabled; existing locks retained for recovery.")
    elif args.command == "list-locks":
        with store.connect() as db:
            print(json.dumps([dict(row) for row in db.execute("SELECT * FROM locks")], indent=2))
    elif args.command == "backup":
        output = Path(args.output).expanduser().resolve() if args.output else config_file.parent / "backups" / (time.strftime("locks-%Y%m%d-%H%M%S-") + secrets.token_hex(3) + ".sqlite3")
        if output.exists():
            parser.error("Backup destination already exists")
        output.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        # Reserve exclusively; never replace an existing backup after a race.
        fd = os.open(output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        os.close(fd)
        destination = sqlite3.connect(output)
        try:
            with store.connect() as source:
                source.backup(destination)
        finally:
            destination.close()
        print(f"Consistent SQLite backup: {output}")
    elif args.command == "recover":
        store.recover(args.repo, args.path, args.generation, args.fingerprint)
        print("Recovered lock; previous owner must reload before resuming.")


if __name__ == "__main__":
    try:
        main()
    except (APIError, OSError, ValueError, sqlite3.Error) as error:
        print(f"Git Soft Lock: {error}", file=sys.stderr)
        sys.exit(1)
