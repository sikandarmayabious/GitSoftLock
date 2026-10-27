# Backend: direct Python hosting

Requires Python **3.10+** with its standard `sqlite3` and `ssl` modules. There are no pip dependencies. Only this `Backend` folder is needed on the phone. Unreal and project files stay on the artists' computers.

See the [complete system guide](../Plugin/Docs/GitSoftLockGuide.md) for how ownership works in Unreal, and the [API and storage reference](API.md) for endpoints, request/response examples, validation and database tables.

## Localhost on Windows first

Open a terminal in `Backend`:

```powershell
python server.py init --repo local-demo --id alice --name "Alice"
python server.py serve
```

`start.ps1` also starts the server. If Python is not on PATH, use `./start.ps1 -PythonExecutable C:/Path/To/python.exe`. Leave this terminal running. Press Ctrl+C to stop; repeat the start command to restart. Locks and users remain in the database.

In a second terminal in `Backend`:

```powershell
python server.py status
python server.py add-user --repo local-demo --id bob --name "Bob"
```

Use `http://127.0.0.1:8791` when [enrolling Unreal](../Plugin/README.md). `init` creates the configuration, database and first user's private credential file. Repeating the same `init` preserves that user and token. Adding an existing user requires explicit rotation.

## Ubuntu inside Termux

Open your already-installed Ubuntu environment using your usual Termux command. Run these commands **inside Ubuntu**, using its root session or prefixing apt commands with `sudo` if needed:

```sh
apt update
apt install -y python3 ca-certificates
python3 --version
```

Copy this `Backend` folder into Ubuntu's private home directory, for example `~/GitSoftLock/Backend`. Keep its SQLite database in that private filesystem, not Android shared storage. Then:

```sh
cd ~/GitSoftLock/Backend
python3 server.py init --repo your-project --id alice --name "Alice"
sh start.sh
```

Keep this Ubuntu terminal session open. In another Ubuntu session, check it with:

```sh
cd ~/GitSoftLock/Backend
python3 server.py status
```

This uses one foreground Python process; no systemd service is required. PRoot supports individual long-running processes but not a normal init/service-manager environment. [PRoot limitations](https://github.com/termux/proot-distro#limitations)

Keep the phone powered and connected. Android can kill Termux processes, so a phone is not a guaranteed always-on host. If that happens, reopen Ubuntu and run `sh start.sh` again: locks survive, and connected editors deny saving until the service returns. [Termux Android process limitations](https://github.com/termux/termux-app#installation)

## Connect a Windows computer to the phone

`127.0.0.1` on a PC means that PC, not the phone. For a simple encrypted connection on the same network, use SSH forwarding. The backend stays bound to the phone's loopback address.

In a **separate native Termux session, outside Ubuntu**:

```sh
pkg install openssh
passwd
whoami
sshd
```

Set a strong SSH password, or use your existing SSH public-key setup. The `whoami` output is the Termux username. Find the phone's Wi-Fi IP in Android's network settings. Termux's SSH server normally uses port 8022. [Termux SSH authentication and port](https://github.com/termux/termux-packages/discussions/18318)

Stop any local PC test backend using port 8791 before opening the tunnel. In Windows PowerShell, replace `TERMUX_USER` and `PHONE_IP`:

```powershell
ssh -N -o ExitOnForwardFailure=yes -o ServerAliveInterval=30 -o ServerAliveCountMax=3 -L 127.0.0.1:8791:127.0.0.1:8791 -p 8022 TERMUX_USER@PHONE_IP
```

Leave the SSH terminal open, alongside the phone's Ubuntu server session. Unreal continues using `http://127.0.0.1:8791`. Each teammate opens their own tunnel to the **same phone backend** and enrolls with their own Git Soft Lock credential file. SSH access is separate from the Git Soft Lock user token; only give phone shell access to trusted people. PRoot shares the host network, so the forward reaches the Ubuntu process.

Verify from another PC terminal, in the repository root:

```powershell
python Backend/server.py status --url http://127.0.0.1:8791
```

The phone must be reachable from the PC. A private Wi-Fi IP is not reachable over the public internet. For teammates outside the network, first establish a reachable private network or an existing secure SSH route to the phone; this repository does not provision internet connectivity.

## Configuration

`init` writes `config.json` beside `server.py`. The template is `config.example.json`:

```json
{
  "bind": "127.0.0.1",
  "port": 8791,
  "database": "data/locks.sqlite3",
  "certfile": "",
  "keyfile": ""
}
```

Database/certificate/key paths inside the configuration are relative to that configuration file, regardless of the current terminal directory. The default configuration always lives beside `server.py`. Restart after editing it.

For a different configuration, put the global option before the command:

```sh
python3 server.py --config /private/path/config.json serve
python3 server.py --config /private/path/config.json list-locks
```

`GIT_SOFT_LOCK_CONFIG` selects a default config path. Global `--db` or `GIT_SOFT_LOCK_DB` overrides its database path. Use these consistently for service and administrative commands. `serve --port 8792` temporarily overrides the port; check it with `status --url http://127.0.0.1:8792`.

For direct HTTPS, set `bind` to `0.0.0.0` and provide a certificate/full chain and private key valid for the hostname your PCs use. TLS is supported directly:

```sh
python3 server.py serve --bind 0.0.0.0 --certfile /private/fullchain.pem --keyfile /private/privkey.pem
python3 server.py status --url https://locks.example.com:8791
```

Certificate issuance, renewal, DNS and reachability are external setup steps. Clients verify certificates. Plain HTTP binds to loopback only, which is sufficient for local testing and the SSH setup above.

## Users, backups and recovery

Run from `Backend` in Ubuntu; on Windows substitute `python` for `python3`:

```sh
python3 server.py add-user --id bob --name "Bob" --repo your-project
python3 server.py list-locks
python3 server.py backup
```

`credentials/bob.json` contains Bob's bearer token. Transfer it privately to Bob's PC for enrollment. Never commit credentials or share one user's file among teammates. User names in Unreal come from the authenticated server record. Repeat `--repo` to authorize multiple projects.

Backups are consistent SQLite snapshots written to new files in `backups/`. Copy them off the phone. Store configuration and any TLS material privately with your recovery information. Use one authoritative database for a project across every branch and checkout; independent per-person servers would allow conflicting owners.

To replace a lost token, explicitly rotate it into a new file, then re-enroll that person's checkout:

```sh
python3 server.py add-user --id bob --name "Bob" --repo your-project --rotate --credential-file credentials/bob-new.json
```

To revoke a user's credentials entirely, use `python3 server.py disable-user --id bob`. This retains existing locks for recovery.

For a permanently lost workspace, stop its old editor and agree which file version to keep. Obtain the exact generation from `list-locks` and use the MD5 of the agreed file (`missing` for a deleted file):

```sh
python3 server.py recover --repo your-project --path Content/Example.uasset --generation GENERATION_FROM_LIST --fingerprint AGREED_FILE_MD5 --old-editor-stopped
```

Recovery is audited. The old workspace must pull/reload before resuming. Locks never expire on a timeout. To restore a backup, stop connected editors and the backend, preserve the current database and any `-wal`/`-shm` files, replace it with the consistent backup, and reconcile newer work before resuming. Never substitute an old database while editors are connected.

Runtime paths (`config.json`, `data/`, `credentials/`, `backups/`, logs and PEM files) are ignored by Git. Newly created Linux data/credential directories are private. On Windows use an appropriate account ACL for the backend folder.

## Backend tests

```sh
python3 -m unittest discover -s . -p 'test_*.py' -v
```

Tests use temporary databases and do not affect a running installation. Actual phone hosting and network availability must be checked on your device.
