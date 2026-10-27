"""Run real UE package-save tests against an isolated project and temporary lock service."""
import argparse
import json
from pathlib import Path
import subprocess
import shutil
import threading
import uuid
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", type=Path, required=True, help="UE installation directory containing Engine/")
    parser.add_argument("--output", type=Path, help="New output directory (defaults to plugin Saved/GitSoftLockTests/run-ID)")
    parser.add_argument("--reuse-host", type=Path, help="Reuse an isolated host directory for incremental compilation")
    parser.add_argument("--backend", type=Path, default=Path(__file__).resolve().parents[2] / "Backend", help="Directory containing the backend server.py")
    args = parser.parse_args()
    if not (args.backend / "server.py").is_file():
        parser.error("Backend/server.py was not found; pass --backend to its directory")
    sys.path.insert(0, str(args.backend.resolve()))
    from server import Server, Store
    plugin = Path(__file__).resolve().parents[1]
    output = (args.output or plugin / "Saved" / "GitSoftLockTests" / ("run-" + uuid.uuid4().hex[:10])).resolve()
    if args.reuse_host:
        args.reuse_host = args.reuse_host.resolve()
        if not (args.reuse_host / "GitSoftLockTestHost.uproject").is_file():
            parser.error("--reuse-host must identify an existing isolated GitSoftLockTestHost")
    output.mkdir(parents=True, exist_ok=False)
    host = args.reuse_host or output / "Host"
    host.mkdir(parents=True, exist_ok=True)
    (host / "Config").mkdir(exist_ok=True)
    saved = host / "Saved" / "GitSoftLock"
    saved.mkdir(parents=True, exist_ok=True)
    (host / "Content").mkdir(exist_ok=True)
    # Copy only native plugin inputs. Never recurse into this repository's .git, tools,
    # credentials or Saved tree when the output is inside the plugin itself.
    fixture_plugin = host / "Plugins" / "GitSoftLock"
    shutil.copytree(plugin / "Source", fixture_plugin / "Source", dirs_exist_ok=True)
    shutil.copy2(plugin / "GitSoftLock.uplugin", fixture_plugin / "GitSoftLock.uplugin")
    # Independent Git root ensures test writes cannot enter the production repository.
    subprocess.run(["git", "init", str(host)], check=True, capture_output=True)
    uproject = host / "GitSoftLockTestHost.uproject"
    uproject.write_text(json.dumps({
        "FileVersion": 3, "EngineAssociation": "5.8", "DisableEnginePluginsByDefault": True,
        "Plugins": [{"Name": "GitSoftLock", "Enabled": True}, {"Name": "PythonScriptPlugin", "Enabled": True}]
    }, indent=2))
    (host / "Config" / "DefaultEngine.ini").write_text(
        "[/Script/EngineSettings.GameMapsSettings]\nEditorStartupMap=/Engine/Maps/Entry\n"
        "[/Script/Engine.RendererSettings]\nr.RayTracing=False\n")
    print(f"Integration output: {output}", flush=True)
    with (output / "build.log").open("w", encoding="utf-8") as stream:
        build = subprocess.run([str(args.engine / "Engine" / "Build" / "BatchFiles" / "Build.bat"),
            "UnrealEditor", "Win64", "Development", f"-Project={uproject}", "-WaitMutex", "-NoHotReloadFromIDE"],
            stdout=stream, stderr=subprocess.STDOUT, timeout=240)
    if build.returncode:
        print("Isolated build failed; inspect build.log", flush=True)
        return 1
    store = Store(output / "locks.sqlite3")
    alice = store.add_user("alice", "Alice Test", ["test"])
    bob = store.add_user("bob", "Bob Test", ["test"])
    server = Server(("127.0.0.1", 0), store)
    config = {"url": f"http://127.0.0.1:{server.server_port}", "alice": alice, "bob": bob}
    (saved / "test-connection.json").write_text(json.dumps(config))
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    log = output / "Unreal.log"
    args_ue = [str(args.engine / "Engine" / "Binaries" / "Win64" / "UnrealEditor-Cmd.exe"), str(uproject),
        "-unattended", "-nop4", "-nosplash", "-nullrhi", "-nosound", "-NoLiveCoding", "-NoMCP",
        "-ExecCmds=Automation RunTests GitSoftLock", "-TestExit=Automation Test Queue Empty",
        f"-ReportExportPath={output / 'Report'}", f"-abslog={log}"]
    try:
        with (output / "process.log").open("w", encoding="utf-8") as stream:
            result = subprocess.run(args_ue, stdout=stream, stderr=subprocess.STDOUT, timeout=240)
        report = output / "Report" / "index.json"
        if report.exists():
            data = json.loads(report.read_text(encoding="utf-8-sig"))
            print(json.dumps({k: data.get(k) for k in ("succeeded", "succeededWithWarnings", "failed", "notRun")}), flush=True)
        else:
            print("No automation report; inspect Unreal.log and process.log", flush=True)
            return 1
        passed = data.get("succeeded", 0) + data.get("succeededWithWarnings", 0)
        return 0 if result.returncode == 0 and data.get("failed") == 0 and passed >= 2 else 1
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


if __name__ == "__main__":
    raise SystemExit(main())
