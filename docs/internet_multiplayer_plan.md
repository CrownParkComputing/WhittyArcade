# Internet multiplayer (MANX)

> **Superseded in part, August 2026.** Phases 1–3 below were built differently
> from what this document proposed, and the tree now follows the built version.
> Instead of a self-hosted Go server with vendored libjuice ICE and RFC 8628
> device codes, the shipped design is:
>
> * **Firebase** (Auth + Firestore) on the free Spark plan as pure signalling —
>   no server to run, no Cloud Functions. The website and the security rules
>   live in their own repo, **CrownParkComputing/MANXOnline**, deployed to
>   Coolify; `firestore.rules` there is the entire security boundary, and its
>   `public/js/model.js` must stay in lock-step with `include/online_wire.h`
>   and `src/online_link.cpp` here.
> * **Minimal STUN instead of ICE.** `manx_stun` (about 200 lines, no
>   dependency) asks for the lobby socket's own public address; libjuice,
>   monocypher and TURN were not vendored. The consequence is stated honestly
>   rather than hidden: two machines both behind symmetric NAT cannot connect
>   and there is no relay.
> * **No new transport at all.** `multiplayer_lobby` builds its peer table from
>   the source address of arriving hellos, so seeding a remote public endpoint
>   makes the existing 200 ms hello stream the hole punch *and* the NAT
>   keepalive. Invitations, the roster, launching and lockstep are unchanged.
>   `manx_link` with three backends was not needed.
> * **Pairing instead of device codes**, because the launcher has no text input
>   of any kind: the cabinet shows a code and the website mints its account.
>
> **Phase 0 remains outstanding and is now the blocker on playability**: the
> negotiated input delay and the 8-frame redundancy described below are not
> built, so a connected internet match will stall. Everything about lockstep,
> streaming, security and the games-mask bug below still stands.

The goal: a site where players register, a client that links to it without
shipping a secret, friends lists, presence, and matches over the internet —
for real multiplayer games and for two-player arcade games taken in turns.

This document records the decisions and the reasoning. Where it says "already
exists", a file and line are given, because the most expensive mistake here
would be rebuilding something the tree already does.

## The correction that shapes everything

Frame streaming is **not** the fallback for emulated boards. This project already
moved to lockstep for every board, and the streaming path is dead code:
`src/main.cpp` sets `MANX_NETPLAY=1` and explicitly unsets `MANX_VIDEO_ROLE`
and `MANX_VIDEO_PORT` for every non-native-link networked launch, and nothing
in the tree ever sets them — so `network_video_link` always sees role 0 and never
starts its thread, and both branches in `polygon_renderer_gpu::present_texture`
that use it are unreachable.

That is fortunate, because streaming cannot cross the internet at this
resolution:

| Content | Raw per frame | Raw bitrate |
|---|---|---|
| Galaxian 224×256 | 229 KB | 110 Mbit/s |
| Model 2 496×384 | 762 KB | 366 Mbit/s |
| Geometry Wars 1280×720 | 3.7 MB | 1.8 Gbit/s |

Lockstep input over the same link is **~56 kbit/s each way** — an 88-byte packet
at 60 Hz. That is a 100–1000× difference, and it decides the architecture,
including the relay cost below.

**Do not resurrect `network_video_link` for internet play.**

## What already exists

- **A complete lockstep engine.** `src/arcade_input.cpp`: a 64-slot input ring
  published 3 frames ahead, frame-indexed filing that tolerates reorder and
  duplication, a bounded wait that distinguishes "slow" from "gone", and host-loop
  integration that refuses to advance the board while stalled. Unit-tested in
  `tests/netplay_lockstep_test.cpp`, with the determinism assumption itself tested
  in `tests/galaxian_determinism_test.cpp`.
- **Desync detection, and a good one.** The packet carries `hash_frame`,
  `state_hash` *and* `input_hash`, so the comparator can say *why* it diverged —
  the emulation drifted, or the link fed the two boards differently. Persistent
  operator data is hashed before frame 0, so two machines with different NVRAM are
  caught before the first frame rather than ninety seconds in.
- **HTTP, JSON and an offline doctrine.** libcurl is already linked and
  `third_party/json` is vendored. Every network entry point must return
  immediately, keep HTTP and disk work off the render thread, cache durable
  results, and degrade predictably when the cabinet has no network.
- **Presence transport for free.** The system libcurl speaks `wss` and ships
  `curl/websockets.h`, so a WebSocket presence channel costs no new dependency.

## What must change

**Every socket in the tree is broadcast or loopback** — the lobby, the input
link, the System 22 C139 transport and the Model 2 bus. There is no unicast peer
plumbing anywhere. This is the largest gap and the highest-leverage refactor:
extract a `manx_link` datagram interface with three backends — `broadcast`
(today's behaviour, unchanged, so LAN keeps working with the cable unplugged),
`direct` (unicast to a known address), and `ice`.

**Input delay is a compile-time constant** (`netplay_delay_frames = 3`, 50 ms).
Right for a LAN, wrong for the internet. Negotiate it from measured RTT:
`delay = clamp(ceil((RTT/2 + jitter_p95) / 16.67ms), 2, 10)`.

**No packet redundancy.** One datagram carries one frame, so one lost packet is
one guaranteed stall. On a LAN that never happens; on the internet 0.5–2 % loss
is normal. Carry the last 8 frames in every packet — about 50 extra bytes, which
changes the bandwidth not at all.

**The lobby's game mask breaks as soon as plugins exist.** It encodes games as a
64-bit mask of *indices into `supported_rom_sets()`*, and `register_plugin_games`
appends discovered plugins to that same list — so two machines with different
plugins installed assign different bits to the same game, and the mask is
meaningless between them. It also overflows silently past 23 plugins. Must become
hashed short names. This is a live LAN bug today, not only an online blocker.

## The three models, and where each applies

**Lockstep over a deterministic simulation** is the default for everything. The
useful distinction is not emulator versus plugin but *can it be snapshotted?* —
because that is what separates delay-based from rollback.

**Frame streaming** is a dead end (see above). If a board ever proves
non-deterministic, fix the board or drop it from online play.

**Taking turns** splits into two products, and only one is cheap:

- *Shared board* (as Galaga works today): one machine, both players watch it,
  so it is the same lockstep as simultaneous play with a turn overlay. No saving:
  both machines still delay every input by the transit time.
- *Relay alternating*: don't share the board. Each player plays a life on their
  own machine, the turn token passes through the server, scores accumulate into a
  shared match record. **This needs no realtime transport at all** — no lockstep,
  no NAT traversal, no determinism — and works at 400 ms across platforms and
  builds.

**Recommendation: relay-alternating is the default online for `alternating`
titles**, with shared-board kept for LAN. It is the cheapest thing here to build
and the only mode that works on hotel Wi-Fi.

`native_link` titles (Sega Rally, Daytona, Ridge Racer 2, Rave Racer, Manx TT,
Motor Raid) are out of scope for the internet: they exchange emulated comm-board
packets over a protocol designed for a metre of cable.

## Accounts

**RFC 8628 device authorization grant.** The client posts its public `client_id`,
shows an 8-character code fullscreen, and polls while the player approves it on a
phone. No `client_secret` is ever shipped in the binary.

Access token 15 minutes, memory only. Refresh token rotating and
server-revocable, written `0600` under `manx_platform::config_root()`. No OS
keychain: that is three platform integrations for a threat model where the
attacker already reads `$HOME`, and at that point they have the ROMs and saves
anyway.

**Offline is not a degraded mode.** LAN play must never require the server.
Beyond caching the profile and friends list, the server issues a 30-day signed
identity blob and the client pins the server's public key — so two machines on a
LAN with no internet still show real names and real friendships, at the cost of
one signature check.

## Presence and friends

One persistent WebSocket per client, authenticated by bearer token, heartbeat
every 25 s. Presence is **derived from the connection, never claimed** — with a
30-second grace so a Wi-Fi blip does not flicker everyone's list. `in_game` and
the game name are client-reported and therefore cosmetic; never an authorisation
input.

**No join-in-progress in v1.** A joiner needs the complete simulation state,
which does not exist for emulated boards and does not yet exist for plugins.
Ship "invite to next match" instead.

**No chat in v1.** Largest moderation and legal liability for the smallest gain
in a friends-and-invites product. Presence, invites and a block list only; a
block severs presence both ways and drops invites silently.

## Connectivity

Vendor **libjuice** (ISC, ~6k LOC, no dependencies beyond sockets) for STUN, TURN
and ICE, and **monocypher** (one `.c`/`.h`) for X25519 + XChaCha20-Poly1305.
Together about 2000 lines, matching how `third_party/` is already used, with no
OpenSSL and no new package dependency. WebRTC data channels are rejected: they
pull in OpenSSL plus usrsctp for a browser capability nothing here needs. ENet is
rejected because its reliability layer is actively unwanted — lockstep wants
redundant unreliable datagrams, not retransmission and head-of-line blocking.

The wire is currently unauthenticated beyond a magic number and a session id. On
a LAN that is tolerable; on the internet anyone who learns the address can inject
inputs. AEAD is mandatory, keyed from the server-issued match ticket, with a real
replay window — the existing sequence check is a monotonicity filter, not a replay
defence.

**Relay cost.** ~34 KB/s for a relayed 1v1 ≈ **120 MB per match-hour**: about
€0.12 per *thousand* relayed match-hours on a Hetzner-class host. Expect to relay
8–20 % of pairs (symmetric NAT and CGNAT). Issue TURN credentials only for matches
the server brokered, scoped to the match, with quotas — an open TURN server is an
open proxy.

## Security, honestly stated

In peer-to-peer lockstep both peers *are* the authority. A modified client can
give itself infinite lives and there is no cryptographic answer. What the desync
detector buys is **tamper evidence**: a modified simulation diverges within frames
and both sides are told. For friends-list play that is enough — it is what the
P2P Xbox Live-era titles had. Do not sell it as anti-cheat.

Never trust from a client: any score, any reported checksum, any presence claim,
any identity not backed by a server token, any timing claim, or the peer address
it names.

Keep `high_scores.h` exactly as it is — local, MAME-compatible, identity-free.
Any online table is **friends-scoped and unverified, and must be labelled so**. A
verified global ladder needs server-side re-simulation and is optional at best;
an unverifiable global ladder is worse than none.

## Phases

Each ships independently.

0. **Make LAN lockstep internet-shaped.** The `manx_link` interface and its
   `direct` backend, 8-frame redundancy, negotiated delay, hashed-short-name game
   mask, and wiring plugin `state_checksum()` into the desync channel. No server,
   no accounts, no new dependency; testable with two local processes under
   `tc netem`. Ships as "LAN play now survives packet loss."
1. **Internet play by join code.** libjuice + monocypher; the host's candidates
   encoded in a code the guest pastes; key derived from it. **No accounts, no
   backend beyond public STUN.** Validates ICE, delay negotiation, redundancy and
   desync detection against the real internet before any account code exists.
2. **Accounts, presence, friends.** Device-code linking, rotating refresh tokens,
   signed offline identity, cached profile, WebSocket presence, invites handing
   off to the phase-1 transport. One Go binary plus Postgres.
3. **Relay-alternating, and TURN.** Turn-token and score exchange through the
   server for every `alternating` title — almost certainly the highest
   satisfaction per line of code here — plus coturn with match-scoped credentials.
4. **Plugin rollback.** ABI v2, append-only per the header's own rule:
   `state_size`, `save_state`, `load_state`, `advance` (simulate only), `present`
   (render), `create_seeded`. Plus the deterministic math shim and plugin build
   flags.
5. **Join-in-progress, spectating, verified scores.** Only reachable after 4, and
   genuinely optional.

## Decisions for the owner

1. **Self-host.** One €5–15/month VPS covers thousands of users at this traffic.
   Managed platforms would tie a deliberately offline-first project to a service
   that can be switched off — precisely the fate of the thing this feature is
   modelled on. The cost case for managed only appears when relaying video.
2. **How much of the internet in v1?** Direct connect alone reaches 80–90 % of
   pairs. It is legitimate to ship phases 1–2 with "some networks won't work" and
   add TURN once you know how often it bites.
3. **Cross-platform matches.** Until a deterministic math shim exists, gate
   matchmaking on `{platform, build hash}`. Cheap, honest, and it turns a
   confusing mid-match desync into "this player is on a different build".
