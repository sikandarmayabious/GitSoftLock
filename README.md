# Git Soft Lock for Unreal Engine

An independent lock system for teams editing Unreal `.uasset` and `.umap` files in Git. The backend runs directly with Python and SQLite, including inside Ubuntu in Android Termux. No Docker, pip packages, cloud account, or Unreal installation is needed on the server.

**Start with the [complete guide](Plugin/Docs/GitSoftLockGuide.md)** for features, diagrams, exact notification behavior, setup, daily workflow, settings, administration and troubleshooting. Developers can use the [backend API and storage reference](Backend/API.md).

```text
GitSoftLock/
  Plugin/     Unreal editor plugin, enrollment client, Git hook and native tests
  Backend/    Standalone Python server, configuration, launchers and backend tests
```

The first person editing an asset acquires its shared lock. A teammate sees **locked by {name}** and can choose **I know, just doing testing / Revert**. Trying to save offers **Don't save / Revert changes**. Testing keeps changes in memory; it never grants permission to save another person's locked file. When protection is enabled, an unavailable backend also blocks saves.

Lock updates poll every two seconds; every save separately checks the authoritative server. Saving retains ownership. After a normal Git commit/push, ownership releases automatically once the editor sees a clean file matching its upstream. Locks survive backend restarts, editor crashes and disconnections.

## Start on localhost

From this repository's root, using Python 3.10+:

```powershell
python Backend/server.py init --repo local-demo --id alice --name "Alice"
python Backend/server.py serve
```

Leave the server terminal open. In another terminal:

```powershell
python Backend/server.py status
```

The URL is `http://127.0.0.1:8791`. Setup creates private `Backend/config.json`, `Backend/data/locks.sqlite3` and `Backend/credentials/alice.json`. They are ignored by Git. Each teammate needs their own credentials, but everyone uses the same server and repository ID.

- [Backend setup, Ubuntu in Termux, phone connection and administration](Backend/README.md)
- [Unreal plugin installation, enrollment and daily workflow](Plugin/README.md)
- [System design and implementation plan](Plugin/Docs/GitSoftLockPlan.md)

This is cooperative protection inside enrolled Unreal editors. External file writes, disabled plugins and bypassed Git hooks remain outside its control. The plugin is initially disabled until a project is enrolled. The backend stores metadata only; assets remain in Git.

## Verify

```powershell
python -m unittest discover -s Backend -p "test_*.py" -v
python -m unittest discover -s Plugin/Tools -p "test_*.py" -v
python Plugin/Tools/run_integration.py --engine "C:/Program Files/Epic Games/UE_5.8"
```

The Unreal runner builds a disposable project from this repository and tests actual package writes. Generated results stay under `Plugin/Saved/GitSoftLockTests`. It does not use or modify the enclosing project's assets.
