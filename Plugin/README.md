# Unreal plugin

This folder contains the editor plugin, enrollment CLI, optional Git hook and native tests. Normal plugin operation does not import or start the backend.

## Install

Copy this `Plugin` folder's **contents** into `YourProject/Plugins/GitSoftLock`, so `GitSoftLock.uplugin` is directly in that destination. Build the project's editor using the UE 5.8 C++ toolchain.

Alternatively, keep the entire independent repository at `YourProject/Plugins/GitSoftLock`. Unreal recursively discovers `Plugin/GitSoftLock.uplugin` inside it. Use only one copy of the descriptor. This is the layout used in the current repository. Generated binaries, intermediates and saved output are ignored by Git.

Install Python **3.12+** and Git on editing computers for enrollment and the optional Git guard. The backend itself needs only Python 3.10+.

## Enroll a checkout

Start the [backend](../Backend/README.md), or open the SSH tunnel to your phone. Close this project's editor before configuring it. From the **GitSoftLock repository root**, enroll the consuming project with that person's credential file:

```powershell
python Plugin/Tools/client.py --project D:/UnrealProjects/YourProject configure --url http://127.0.0.1:8791 --repo local-demo --credential-file Backend/credentials/alice.json --enable-project --install-hook
```

Replace the project directory, repository ID and credential path. For a phone backend, use its repository ID and credentials privately copied from the phone. When only the Plugin folder is installed, run `python Tools/client.py ...` from that plugin folder instead.

`--enable-project` writes the shared Git Soft Lock section in the consuming project's `Config/DefaultEditor.ini`. Share that section and the plugin/submodule reference with teammates. Everyone enrolls with their own credentials and the same repository ID. Teammates can omit `--enable-project` once shared configuration exists. The localhost URL works for every teammate when each has a tunnel to the same backend.

`--install-hook` is optional. It inserts a marked Git guard while preserving the existing shell pre-commit hook, LFS logic and `core.hooksPath`. Non-shell hooks require manual chaining. The guard checks ownership of staged Unreal assets, using personal Git configuration to locate Python, the client and project.

After moving this repository or upgrading to the two-folder layout, refresh an installed hook's client path:

```powershell
python Plugin/Tools/client.py --project D:/UnrealProjects/YourProject install-hook
```

Disable only the Git guard with `git -C D:/UnrealProjects/YourProject config softlock.enabled false`. Editor protection is configured separately.

Personal token and workspace identity live in the consuming project's `Saved/GitSoftLock`. Ensure that project's `.gitignore` excludes `Saved/`. Do not copy workspace identity between checkouts/computers. Restrict this folder to the intended Windows user. One protected editor per checkout is enforced with an open file handle. Restart Unreal after configuration.

## Editor behavior

Protection is initially **off**, with an onboarding warning, until shared `bEnabled=True` is configured. Once enabled, missing credentials or backend disconnection block project package saves. Every teammate must install and enable the plugin. Open **Tools > Git Soft Lock** for connection state and shared locks.

| Action | Result |
| --- | --- |
| Edit an available asset | Atomically claim its lock for this person and checkout. |
| Edit someone else's locked asset | Show the owner's name and **I know, just doing testing / Revert**. |
| Choose testing | Keep edits in memory without permission to save the locked file. |
| Try saving a conflicting asset | Block the write; show **Don't save / Revert changes**. |
| Backend unavailable | Block saves while protection is enabled. |
| Save your own asset | Authorize with the server and retain ownership. |
| Commit/push and return to a clean asset | Release once the editor verifies content matches upstream. |
| Close/crash/disconnect | Retain locks; reopen the same checkout to resume. |

Shared state polls every two seconds while Unreal ticks. Save authorization is synchronous with a three-second default timeout, so saves may briefly pause. Cached ownership never grants save permission. An offline badge does not allow taking a teammate's lock.

Release checks run approximately every 20 seconds. The package must have no unsaved changes, a clean worktree/index and content matching its upstream tracking ref. Missing upstream, untracked content, local-only commits and uncommitted deletions retain ownership. Keep Unreal open until the dashboard shows release after push. No automatic fetch, commit, push, reset or Git restore is performed.

After another person publishes, pull and **reload** the file before editing. The backend remembers released content fingerprints and rejects older bytes. MD5 checks content equality only; bearer tokens use SHA-256 hashing.

## Revert and coverage

Revert discards only the selected package's unsaved memory changes and reloads its existing disk version. It does not restore from Git or download another person's work. Never-saved packages need manual discard. Stop PIE first. Map reload is refused while another non-transient package is dirty, to avoid discarding unrelated work.

Coverage includes project `.uasset`/`.umap` files, project plugin content and external actor packages. Autosave checks the original asset; Save As checks original and destination. Standard editor delete/rename/move predicates check ownership. The existing engine save predicate is preserved. Cooking is exempted by Unreal itself.

This is cooperative editor save protection. Direct OS writes/deletes, disabled plugins, bypassed Git hooks and plugins replacing Unreal's save delegate remain outside the guarantee. Administrative recovery must stop the old editor; it cannot revoke an authorized write already underway. Use one repository ID across branches and avoid case-only duplicate filenames. Validation targets UE 5.8 on Windows.

## Validation

From the full repository root:

```powershell
python -m unittest discover -s Plugin/Tools -p "test_*.py" -v
python Plugin/Tools/run_integration.py --engine "C:/Program Files/Epic Games/UE_5.8"
```

Tests need the sibling `Backend` for fixtures. The native runner also accepts `--backend PATH_TO_BACKEND`. It copies native plugin source to a disposable Git/Unreal project, builds it and performs real asset/map saves. Denied writes are verified against unchanged file hashes. Output is in `Plugin/Saved/GitSoftLockTests`; the consuming project's assets are untouched. Only pass `--reuse-host` a previous disposable test host.

For visual inspection of an already-built disposable host:

```powershell
python Plugin/Tools/preview.py --engine "C:/Program Files/Epic Games/UE_5.8" --host PATH_TO_TEST_HOST
```
