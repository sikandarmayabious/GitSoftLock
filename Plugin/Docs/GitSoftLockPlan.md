# Git Soft Lock: implementation plan

For current usage, features and operational instructions, read the [complete guide](GitSoftLockGuide.md). The [API reference](../../Backend/API.md) describes the implemented backend protocol.

## Contract

An editor-only UE 5.8 plugin protects project `.uasset` and `.umap` packages (including project plugin content and external actor packages). The first editor to modify a package atomically claims its repository-relative filename on a shared service reached through localhost, SSH forwarding or HTTPS. Repository IDs are shared across branches: branching must not silently create a second owner of a binary file.

Conflicting edits display the server-authenticated owner's name and **I know, just doing testing / Revert**. Testing only suppresses repeat edit warnings for that package; it never grants save permission. Each attempted save independently obtains server authorization for both the original package and any project destination, then chains Unreal's original save predicate. Failure denies saving and displays **Don't save / Revert changes**. Autosave is checked against the original asset. Cooking is excluded by Unreal's save API itself.

## Components

All paths below are relative to the independent GitSoftLock repository; no component depends on an enclosing Unreal project checkout.

1. `Plugin/Source/` and `Plugin/GitSoftLock.uplugin`: native editor module, project settings, local credential file, automatic claims, two-second snapshot/heartbeat polling, save veto, conflict notifications, lock dashboard, guarded package reload, and native automation tests. Install the Plugin contents or the complete repository in a consuming project's `Plugins/GitSoftLock` directory.
2. `Backend/server.py`: Python standard-library HTTP/TLS service; SQLite WAL transactions; per-user bearer credentials stored hashed; repository authorization; persistent owner/workspace locks; audit history; published-content fingerprints; fenced releases; bounded requests. Runs independently of Unreal, without pip dependencies or Docker.
3. `Plugin/Tools/client.py` and `Plugin/Hooks/pre-commit`: setup CLI and optional Git validation. Explicit hook installation preserves the consuming repository's LFS/custom hook and hooks-directory setting. Paths come from the chosen project and personal Git config, not a hard-coded parent project.
4. `Backend/start.sh`, `start.ps1` and configuration: direct foreground startup, idempotent initialization, status, explicit credential rotation, persistent database, backup/recovery commands. Ubuntu in Termux is the intended phone host; SSH forwarding provides the initial encrypted remote connection.
5. `Plugin/Tools/run_integration.py`: builds a disposable Unreal test project from this plugin's native source and installed engine, keeping generated artifacts in `Plugin/Saved/`.

Each consuming project owns its shared server URL/repository ID and its private local credentials/workspace state. These are runtime inputs, not source-code dependencies. The plugin has no default project identity.

## Consistency and recovery

- Lock acquisition is a SQLite write transaction, never a Git commit or a last-writer-wins JSON upload.
- Owner identity comes from credentials, never an arbitrary editor display-name field. A stable workspace ID identifies one checkout; an OS file handle prevents simultaneous protected editor processes in that checkout. Do not copy the private workspace configuration to another machine.
- Heartbeats indicate presence only. Locks do not expire or disappear on disconnect, server restart, or editor shutdown. A persistent fencing generation prevents delayed release requests from releasing a later acquisition.
- Releasing saved work requires a clean index/worktree for that asset and matching content on the configured upstream. The editor must also report no unsaved package changes. A successful save alone does not release.
- Released content fingerprints reject an older checkout attempting to claim the asset. Reloading/pulling newer content is required. Fingerprints are consistency checks, not credential/security hashes.
- Explicit administrative recovery is reserved for a stopped editor and a verified working copy. Revocation cannot make a distributed in-flight disk write reversible; operational recovery must stop the old editor first.
- Service unavailability, malformed responses, missing credentials, and ownership ambiguity fail closed when protection is enabled. An unconfigured installation visibly reports protection off; deployment requires enabling the shared project policy.

## Boundaries

This is cooperative editor save protection, not a filesystem or Git-host access-control system. Every teammate must enable it. External programs, disabled plugins, unsupported engine save paths, direct filesystem deletion/rename, and cooking can bypass editor enforcement. Git validation is defense in depth. Standard editor delete/rename/move predicates check ownership, and move destinations and save destinations are protected. A content reload discards only the requested asset's unsaved state; map reload is refused if it would discard other dirty packages. Existing on-disk changes are never silently restored from Git.

## Verification before delivery

- Service tests: two concurrent claimants, identity isolation, cross-repository denial, normalization, stale fingerprints, persistence, fencing, restart, offline presence, authorization, malformed requests, and request limits.
- Git tests: modified/staged/untracked/deleted, local-only commit, pushed commit, upstream absent, and LFS handling without destructive checkout.
- UE compile plus actual `UPackage::SavePackage` tests: owner success, peer denial with byte hash unchanged, testing acknowledgement still denied, unreachable service denial, autosave and alternate destination denied, original engine delegate preserved, package reload behavior.
- Inspect editor notification buttons and lock status in an isolated test project. Do not close or discard the user's running editor or use its real modified assets for tests.

## Hosting and rollout

First validate the Python backend and Unreal integration on PC localhost. Copy only `Backend/` into Ubuntu's private filesystem on the Android phone, initialize one authoritative database, and run `sh start.sh` in the Ubuntu session. Connect PCs through SSH forwarding and enroll each teammate. No cloud account, container runtime or systemd is required. Native TLS is optional for a reachable hostname with a trusted certificate.

Phone process uptime and network routing are external constraints: Android can stop the process, and a LAN IP is not an internet endpoint. Service failures retain locks and deny saves. Restarting with the same database restores authoritative state. Local integration tests do not constitute device or internet deployment validation.
