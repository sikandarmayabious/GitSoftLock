"""Open the already-built isolated test host with real lock conflict notifications."""
import argparse
import json
from pathlib import Path
import subprocess
import threading
import uuid
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", type=Path, required=True)
    parser.add_argument("--host", type=Path, required=True)
    parser.add_argument("--backend", type=Path, default=Path(__file__).resolve().parents[2] / "Backend")
    args = parser.parse_args()
    if not (args.backend / "server.py").is_file():
        parser.error("Backend/server.py was not found; pass --backend to its directory")
    sys.path.insert(0, str(args.backend.resolve()))
    from server import Server, Store
    project = args.host / "GitSoftLockTestHost.uproject"
    if not project.is_file():
        parser.error("Use the isolated host created by run_integration.py")
    saved = args.host / "Saved" / "GitSoftLock"
    saved.mkdir(parents=True, exist_ok=True)
    store = Store(saved / ("preview-" + uuid.uuid4().hex + ".sqlite3"))
    alice = store.add_user("alice", "Alice Test", ["test"])
    bob = store.add_user("bob", "Bob Test", ["test"])
    server = Server(("127.0.0.1", 0), store)
    (saved / "test-connection.json").write_text(json.dumps({
        "url": f"http://127.0.0.1:{server.server_port}", "alice": alice, "bob": bob}))
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        process = subprocess.Popen([str(args.engine / "Engine/Binaries/Win64/UnrealEditor.exe"), str(project),
            "-nosplash", "-NoLiveCoding", "-NoMCP", "-ExecCmds=GitSoftLock.TestPreview",
            f"-abslog={saved / 'preview.log'}"])
        print(f"Preview editor PID: {process.pid}", flush=True)
        return process.wait()
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


if __name__ == "__main__":
    raise SystemExit(main())
