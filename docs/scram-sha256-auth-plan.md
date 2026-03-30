# SCRAM-SHA-256 Authentication for Bareos

## Problem

Bareos currently uses CRAM-MD5 (RFC 2195) for challenge-response
authentication between all daemons (Director, File Daemon, Storage
Daemon, bconsole, webui-proxy). CRAM-MD5 has the following weaknesses:

- **MD5 password storage**: passwords are stored as `MD5(password)`,
  with no salt and no key-stretching. An attacker with the config file
  can brute-force billions of candidates per second on modern GPUs.
- **MD5-based HMAC**: HMAC-MD5 is not actively broken, but MD5 is
  formally deprecated in FIPS 140-3 and SOC2 compliance frameworks.
- **Static credential**: the stored `MD5(password)` is a permanent
  secret that never rotates and cannot be made expensive to obtain.
- **RFC 2195 is obsolete**: formally deprecated for SASL use in 2008.

TLS (already enabled by default since Bareos 18.2) protects the wire
exchange, so the CRAM-MD5 credentials are not exposed on the network
in normal deployments. The concern is the config file and compliance.

## Proposed Solution: SCRAM-SHA-256

Replace CRAM-MD5 with SCRAM-SHA-256 (RFC 5802, RFC 7677). SCRAM is
the modern SASL successor to CRAM-MD5 — same mutual challenge-response
pattern, same integration points, but with every weakness corrected.

### Improvements over CRAM-MD5

| Property            | CRAM-MD5 (current)        | SCRAM-SHA-256               |
|---------------------|---------------------------|-----------------------------|
| Hash algorithm      | MD5 (deprecated)          | SHA-256                     |
| Password storage    | `MD5(password)`, no salt  | PBKDF2-SHA-256 verifier     |
| Brute-force cost    | Trivial (GPU: billions/s) | Expensive (310 000+ rounds) |
| Per-user salt       | No                        | Yes                         |
| Mutual auth         | Yes                       | Yes (server proof)          |
| TLS channel binding | No                        | Optional (PLUS variant)     |
| Standard status     | Obsolete (RFC 2195)       | Current (RFC 7677)          |
| In production use   | Legacy mail systems       | PostgreSQL, MongoDB, XMPP   |

### Scope

This plan covers **daemon-to-daemon and console authentication only**.
NDMP uses cleartext passwords and is explicitly out of scope.

## Architecture

### Assumptions / constraints

- **Immediate TLS is required.** Non-immediate-TLS (pre-18.2) clients
  are not supported. This simplifies the design significantly: all
  authentication happens inside an already-established TLS session.
- **Backward compatibility is optional.** A per-Console `AuthProtocol`
  directive allows old CRAM-MD5 clients to continue working against
  explicitly-configured legacy consoles during a transition period.
- **No new library dependencies.** OpenSSL (already a required
  dependency) provides `PKCS5_PBKDF2_HMAC`, `HMAC`, and `EVP_sha256`.

### Version gating

A new `BareosVersionNumber::kRelease_26_0` constant gates the new
protocol. The server selects the auth method based on the client
version extracted from the `Hello` frame (already done for the 18.2
TLS transition in `try_tls_handshake_as_a_server.cc`).

### Mechanism negotiation (inside TLS tunnel)

```
Client → Server:  Hello <name> calling version 26.x\n

Server → Server:  auth-methods scram-sha-256 cram-md5\n
                  (or only "auth-methods scram-sha-256" if legacy disabled)

Client → Server:  auth-select scram-sha-256\n

--- SCRAM exchange (4 frames) ---
Client → Server:  n,,n=<username>,r=<cnonce>          (client-first)
Server → Client:  r=<cnonce><snonce>,s=<salt>,i=<N>   (server-first)
Client → Server:  c=biws,r=<nonces>,p=<ClientProof>   (client-final)
Server → Client:  v=<ServerSignature>                  (server-final / mutual)
```

Old clients (version < 26.0) receive `auth cram-md5 <challenge>` as
before — the version check prevents them from ever seeing `auth-methods`.

### Password storage

SCRAM requires a **verifier** — a tuple of `(StoredKey, ServerKey,
salt, iterations)` derived from the plaintext password via PBKDF2.
This is fundamentally different from the current `MD5(password)` and
cannot be derived from it — a plaintext password is required to
generate the verifier.

Config file format (new `p_encoding_scram_sha256`):
```
Password = "scram-sha-256:i=310000,s=<salt-b64>,sk=<StoredKey-b64>,svk=<ServerKey-b64>"
```

The existing `p_encoding_md5` format is retained for consoles
explicitly configured with `AuthProtocol = cram-md5`.

### `AuthProtocol` directive

New optional directive on `Director` and `Console` resources:

```
Console {
  Name    = webui-admin
  Password = "scram-sha-256:..."
  AuthProtocol = scram-sha-256   # default for new/reset passwords
}

Console {
  Name    = legacy-bconsole
  Password = "md5:..."
  AuthProtocol = cram-md5        # explicit legacy mode
}
```

`AuthProtocol` defaults to `scram-sha-256` when the password has
`p_encoding_scram_sha256` encoding, and to `cram-md5` otherwise
(inferred from the existing `p_encoding_md5` stored value).

## File-by-file Implementation Plan

### Phase 1 — Foundation types

**`core/src/include/version_numbers.h`**
- Add `kRelease_26_0 = static_cast<uint32_t>(2600)`

**`core/src/lib/s_password.h`**
- Add `p_encoding_scram_sha256` to `password_encoding` enum

**`core/src/lib/scram_sha256.h`** (new file)
- `ScramSha256Verifier` struct: `salt`, `iterations`, `stored_key`,
  `server_key` (all `std::string`, base64-encoded for storage)
- `ScramSha256Verifier GenerateVerifier(std::string_view password,
  int iterations = 310000)`
- `ScramSha256Handshake` class (mirrors `CramMd5Handshake`):
  - `DoHandshake(bool initiated_by_remote)`
  - `HandshakeResult result`

**`core/src/lib/scram_sha256.cc`** (new file)
- PBKDF2-SHA-256 key derivation via `PKCS5_PBKDF2_HMAC`
- `H(x)` = SHA-256, `HMAC(k,d)` = HMAC-SHA-256 (OpenSSL `EVP`)
- Per RFC 5802 derivations:
  - `SaltedPassword = PBKDF2(password, salt, i, keylen=32)`
  - `ClientKey       = HMAC(SaltedPassword, "Client Key")`
  - `StoredKey       = H(ClientKey)`
  - `ServerKey       = HMAC(SaltedPassword, "Server Key")`
  - `AuthMessage     = client-first-bare + "," + server-first + "," + client-final-no-proof`
  - `ClientSignature = HMAC(StoredKey, AuthMessage)`
  - `ClientProof     = ClientKey XOR ClientSignature`
  - `ServerSignature = HMAC(ServerKey, AuthMessage)`

### Phase 2 — Config parsing

**`core/src/lib/parse_conf.cc`**
- Add `CFG_TYPE_SCRAM_PASSWORD` resource type
- `StoreScramPassword()`: accepts plaintext, generates verifier,
  stores as `scram-sha-256:i=...,s=...,sk=...,svk=...` string
- `StoreMd5Password()`: unchanged (existing)
- Password printer: emit `scram-sha-256:...` or `md5:...` prefix

**`core/src/dird/dird_conf.cc`**
- Add `AuthProtocol` field to `ConsoleResource` (enum:
  `kAuthProtocolScramSha256`, `kAuthProtocolCramMd5`)
- Wire `CFG_TYPE_SCRAM_PASSWORD` into `StoreAutopassword()` for
  Console/Director resources when `AuthProtocol = scram-sha-256`

### Phase 3 — Auth dispatch

**`core/src/lib/bsock.cc`** (`TwoWayAuthenticate` /
`AuthenticateInboundConnection` / `AuthenticateOutboundConnection`)
- Server side (inbound): after reading `Hello`, check
  `connected_daemon_version_` and `auth_protocol`:
  - `>= kRelease_26_0` + `scram-sha-256` → send `auth-methods`, run
    `ScramSha256Handshake`
  - Otherwise → existing CRAM-MD5 path (unchanged)
- Client side (outbound): detect `auth-methods` frame and dispatch
  to `ScramSha256Handshake` or fall back to CRAM-MD5

**`core/src/dird/authenticate_console.cc`**
- `ConsoleAuthenticator::DoDefaultAuthentication()` and
  `DoNamedAuthentication()` — pass `AuthProtocol` from resource to
  `AuthenticateInboundConnection`

### Phase 4 — Password migration tooling

**`core/src/dird/dird.cc`** / new subcommand
- `bareos-dir --hash-password <plaintext>`: prints a ready-to-paste
  `scram-sha-256:...` verifier string for config files

### Phase 5 — Tests

**`core/src/tests/test_scram_sha256.cc`** (new file)
- `constexpr`-compatible unit tests for verifier generation (where
  possible; PBKDF2 is not constexpr, so GoogleTest `TEST()` cases)
- Round-trip test: generate verifier → run handshake → assert success
- Wrong-password test: assert `WRONG_HASH` result
- Replay-attack test: server rejects own challenge
- Verifier serialization/deserialization round-trip

## Migration Path for Operators

1. **New installs (26.0+)**: `bareos-dir` generates SCRAM verifiers
   by default. All daemons ship with SCRAM passwords out of the box.

2. **Upgrades from 25.x**: existing `md5:` passwords continue to work
   — the Director infers `AuthProtocol = cram-md5` from the
   `p_encoding_md5` stored value. No config change required to keep
   the old behaviour.

3. **Opt-in per console**: admin resets the password for a console
   (requires knowing the plaintext), sets `AuthProtocol = scram-sha-256`.
   That console now only accepts 26.0+ clients.

4. **Future deprecation** (27.0 or later): `AuthProtocol = cram-md5`
   emits a deprecation warning; a later release removes CRAM-MD5 support
   entirely, mirroring how non-immediate-TLS clients were dropped in 18.2.

## Out of Scope

- NDMP protocol (uses cleartext; separate concern)
- WebUI / restapi HTTP authentication (already uses separate mechanisms)
- OIDC / SSO (separate plan: `docs/webui-vue-oidc-plan.md`)
- SCRAM channel binding (`-PLUS` variant) — desirable future work once
  the base implementation is stable
