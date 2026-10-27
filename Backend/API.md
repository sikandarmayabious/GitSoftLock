# Git Soft Lock backend API and storage reference

Protocol **1** coordinates ownership of Unreal package paths. The service uses Python's standard-library HTTP server and SQLite. This reference describes the implementation in [server.py](server.py); use the [complete guide](../Plugin/Docs/GitSoftLockGuide.md) for editor behavior and the [backend guide](README.md) for deployment.

The API grants ownership, not filesystem permissions. It does not receive asset bytes, inspect Git history, or prove that a client saved/pushed a file. Clients are responsible for applying the documented editor and publication rules.

## Transport and authentication

The default origin is `http://127.0.0.1:8791`. HTTP is allowed on loopback for local use or SSH forwarding. Direct remote listening requires a certificate and key; the native TLS server requires TLS 1.2 or later. Administrative commands run locally on the backend host and are not HTTP endpoints.

All `/v1/` endpoints use `POST` with a UTF-8 JSON object. Send:

```http
Content-Type: application/json
Authorization: Bearer YOUR_PRIVATE_TOKEN
```

Use a normal `Content-Length`. Chunked requests are rejected. Tokens must not appear in URLs, tracked files or diagnostic output. The database stores SHA-256 token hashes; the credential file exported to the user contains the usable raw token. User ID, name and repository authorization are derived from the token, not from a supplied owner name.

Every POST body includes:

| Field | Type | Meaning |
| --- | --- | --- |
| `protocol` | Integer | Must be `1`. |
| `repo` | String | Authorized shared repository ID. |
| `workspace` | String | Stable identity of this checkout. |

IDs accept 1–160 characters from `A–Z`, `a–z`, `0–9`, `_`, `.`, `:`, `@` and `-`. Authentication does not make a workspace ID secret: distinct checkouts must still keep their identities separate. A token holder is trusted not to impersonate another workspace using the same user's credentials.

## Health

`GET /health` does not require credentials. It checks that the service can query its `locks` table.

```json
{
  "protocol": 1,
  "ok": true
}
```

Success uses HTTP 200; a SQLite failure uses 503 with `ok: false`. A healthy result does not verify a user's token, repository access, Git state, or an Unreal connection. Unknown GET paths return 404.

## Snapshot and heartbeat

`POST /v1/snapshot` returns the repository's active locks. It also updates presence for locks belonging to the requesting user **and workspace**.

Request:

```json
{
  "protocol": 1,
  "repo": "demo-project",
  "workspace": "alice-checkout-1"
}
```

Example response when Alice owns one file:

```json
{
  "protocol": 1,
  "ok": true,
  "user": "alice",
  "name": "Alice",
  "locks": [
    {
      "path": "content/example.uasset",
      "owner": "alice",
      "name": "Alice",
      "workspace": "alice-checkout-1",
      "generation": "0123456789abcdef0123456789abcdef",
      "base": "11111111111111111111111111111111",
      "mine": true,
      "online": true,
      "last_seen": 1793088000.0
    }
  ]
}
```

Example timestamps, fingerprints and generations are illustrative. `mine` is relative to the authenticated user and requested workspace. `online` means the stored activity timestamp is less than 30 seconds old. `last_seen` is Unix time in seconds. Presence does not expire or release ownership. Rows are ordered by normalized path.

## Claim or confirm ownership

`POST /v1/claim` acquires an available path or confirms an existing owner's claim. The Unreal plugin uses this operation for save authorization as well as initial acquisition; there is no separate save endpoint.

Request:

```json
{
  "protocol": 1,
  "repo": "demo-project",
  "workspace": "alice-checkout-1",
  "path": "Content/Example.uasset",
  "base": "11111111111111111111111111111111"
}
```

`base` is the starting disk fingerprint. Use `missing` for a new path without a file. The server does not calculate this fingerprint itself.

Example acquisition response:

```json
{
  "protocol": 1,
  "ok": true,
  "reason": "acquired",
  "lock": {
    "path": "content/example.uasset",
    "owner": "alice",
    "name": "Alice",
    "workspace": "alice-checkout-1",
    "generation": "0123456789abcdef0123456789abcdef",
    "base": "11111111111111111111111111111111",
    "mine": true,
    "online": true
  }
}
```

| Condition | HTTP status | `ok` | `reason` | Result |
| --- | --- | --- | --- | --- |
| No owner and baseline accepted | 200 | `true` | `acquired` | Creates a new persistent generation and audit event. |
| Same user and same workspace owns it | 200 | `true` | `owned` | Keeps the generation and refreshes activity. |
| Another user or workspace owns it | 200 | `false` | `locked` | Returns the owner's lock object with `mine: false`. |
| Unowned but published fingerprint differs | 200 | `false` | `stale` | No lock created; returns an explanatory message. |

**HTTP 200 alone is not permission to save.** Clients must inspect `ok` and the returned lock identity. `locked` and `stale` are ordinary denied decisions with HTTP 200.

The existing owner is checked before the published baseline. Repeated claims by that owner confirm its existing reservation; they do not re-validate a newly supplied baseline against Git. Newly acquired lock objects omit `last_seen`; snapshot/existing-lock views include it. Clients must not require that field on all claim responses.

Example stale response:

```json
{
  "protocol": 1,
  "ok": false,
  "reason": "stale",
  "message": "Local content is older or different. Pull the published asset and reload it before editing."
}
```

## Release ownership

`POST /v1/release` releases the owning workspace's current generation and stores the final published fingerprint. The normal Unreal client sends this only after checking clean package state and matching upstream content.

Request:

```json
{
  "protocol": 1,
  "repo": "demo-project",
  "workspace": "alice-checkout-1",
  "path": "Content/Example.uasset",
  "generation": "0123456789abcdef0123456789abcdef",
  "fingerprint": "22222222222222222222222222222222"
}
```

Success:

```json
{
  "protocol": 1,
  "ok": true
}
```

If no active lock exists, the response is `ok: true` with `reason: "already_released"`; it does not rewrite the existing published fingerprint. If an active lock has a different owner, workspace or generation, the response is HTTP 409. A delayed release from an older acquisition cannot remove a newer lock.

Release, published-fingerprint update and audit insertion occur in one SQLite transaction. The server trusts the owner's supplied fingerprint and does not independently validate Git cleanliness or push completion. This endpoint is not a substitute for safe team handoff rules.

## Path and fingerprint validation

Paths must be repository-relative strings of 1–1024 characters ending in `.uasset` or `.umap`, case-insensitively. Processing normalizes Unicode to NFC, changes backslashes to slashes and case-folds the key.

Empty components, `.`/`..` traversal components, components ending in a dot or space, ASCII control characters, and `: * ? " < > |` are rejected. Leading/trailing slashes and absolute Windows paths therefore fail validation. Case-only duplicate asset names are unsuitable for this namespace.

A fingerprint is `missing` or exactly 32 hexadecimal characters, normalized to lowercase. It represents MD5 content equality. It is not a signature or security proof of the asset's origin.

## Errors and limits

JSON errors normally contain `protocol`, `ok: false` and `message`. Transport failures may have no usable JSON response; clients must deny saving rather than infer approval.

| HTTP status | Typical meaning |
| --- | --- |
| 400 | Invalid JSON, protocol, identifier, asset path, fingerprint, incomplete body, or unsupported chunked transfer. |
| 401 | Missing/malformed bearer authorization, invalid token or disabled user. |
| 403 | Valid user without access to the requested repository ID. |
| 404 | Unknown endpoint. |
| 409 | Wrong owner/workspace/generation for an active release. |
| 413 | Missing, zero, negative or oversized declared request body. |
| 503 | Storage failure or another service-side failure handled as unavailable. |

| Limit | Current value |
| --- | --- |
| JSON request body | 64 KiB maximum. |
| Authorization header | More than 1024 characters is rejected. |
| Concurrent request workers | 32; excess connections are closed, not queued as ownership requests. |
| Socket/handshake timeout | 10 seconds. |
| Listening backlog request | 64; actual socket behavior also depends on the OS. |
| SQLite connection busy timeout | 10 seconds. |
| Minimum native TLS version | TLS 1.2. |

TLS negotiation takes place inside bounded workers so one idle handshake does not stop the accept loop. These limits are resource controls, not a measured team-capacity guarantee or per-user rate-limiting service. Snapshots currently return the entire repository lock list without pagination.

## SQLite storage

The configured database uses WAL mode and `synchronous=FULL`. Each operation opens a connection and closes it after its transaction. Ownership operations use `BEGIN IMMEDIATE`, serializing concurrent decisions about a path.

| Table | Key and fields | Purpose |
| --- | --- | --- |
| `users` | `id`; `name`, unique `token_hash`, JSON `repos`, `enabled` | Authenticated identity and repository permissions. |
| `locks` | Composite primary key `(repo, path)`; `owner`, `workspace`, `generation`, `base`, `created`, `seen` | Persistent current ownership. |
| `published` | Composite primary key `(repo, path)`; `fingerprint` | Starting content required for the next acquisition. |
| `audit` | Integer `id`; `timestamp`, `action`, `repo`, `path`, `owner`, `workspace`, `detail` | Claim, release and administrative recovery history. |

Audit actions are `claim`, `release`, and `admin_recover`. The detail field contains the generation for a claim or the published fingerprint for a release/recovery. There is no built-in audit browser, retention scheduler, denied-request history, or audit event for every user-management action. HTTP access logging is suppressed.

The administrative CLI supports `init`, `serve`, `status`, `add-user`, `disable-user`, `list-locks`, `backup` and `recover`. Use [backend administration](../Plugin/Docs/GitSoftLockGuide.md#backend-administration) for safe procedures. Direct database edits can violate ownership assumptions and are not a normal workflow.

Keep one authoritative database for a project and back it up consistently. Independent replicas with separate databases can each grant ownership and are unsupported. A writable SQLite file is persistence, not a replacement for backups or reconciliation after restoring an older snapshot.

## Compatibility and source map

The server requires protocol 1. The C++ and Python clients also reject responses with an unsupported protocol. There is currently no automatic protocol negotiation or schema migration framework beyond creating missing tables.

| Component | Source |
| --- | --- |
| API validation, SQLite operations, HTTP/TLS service and CLI | [server.py](server.py) |
| Server test cases | [test_backend.py](test_backend.py) |
| Editor HTTP client and response parsing | [GitSoftLockClient.cpp](../Plugin/Source/GitSoftLockEditor/Private/GitSoftLockClient.cpp) |
| Save authorization and release scheduling | [GitSoftLockEditorModule.cpp](../Plugin/Source/GitSoftLockEditor/Private/GitSoftLockEditorModule.cpp) |
| Enrollment and optional Git guard checks | [client.py](../Plugin/Tools/client.py) |
