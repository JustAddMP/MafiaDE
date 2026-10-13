# MafiaCoop network protocol

Phase 2 (connection). Code: `src/net/`. All integers are little-endian. `varint` = unsigned LEB128.
`str` = varint byte length + UTF-8 bytes (no terminator). Everything runs single-threaded inside `Poll()` on the
game's render thread; there are no network threads.

```
Session  (session.cpp)   handshake, player list, ping, chat, app messages, host relay
   |  Transport interface (transport.h): Host / Connect / Send(Reliable|Unreliable) / Poll / Disconnect / Close
   +-- UdpTransport   (udp_transport.cpp)   Winsock UDP, own reliability       peer id = 1, 2, 3 ...
   +-- SteamTransport (steam_transport.cpp) ISteamNetworking006 P2P             peer id = SteamID64
SteamLobby (steam_lobby.cpp)  lobby, invites, rich presence -> gives the guest the host's SteamID
```

Transport guarantees: `Reliable` messages arrive once, in order, per peer, up to 256 KB. `Unreliable` messages may
be lost or reordered, up to 1100 bytes. A peer that is silent for 10 s is dropped (`Timeout`). Local `Disconnect()`
raises no event; remote close, timeout, refused and failed connects raise `Disconnected(reason)`.

## Version rules

| Value | Where | Rule |
|---|---|---|
| `kProtocolVersion` (session.h, currently 1) | Hello | Must match exactly. Bump on ANY incompatible change to session or app messages. |
| `modBuild` (SessionConfig, set by the dll from its build number) | Hello | Must match exactly (sync code must be identical on both sides). |
| UDP wire version (byte 2 of every datagram, currently 1) | UDP header | Datagrams with another version are ignored. Bump only if the UDP framing changes. |
| Lobby data `mc_version` / `mc_proto` | Steam lobby | Informational (shown before connecting); the Hello check is authoritative. |

A mismatch is answered with `Reject{code 1, "version mismatch: host has protocol X build Y, you have ..."}` and the
guest's `onDisconnected` receives `"rejected by host: <reason>"`.

## Session messages

Every transport message starts with a one-byte session message type.

| Id | Name | Dir | Channel | Body |
|---|---|---|---|---|
| 0x01 | Hello | G->H | R | u32 magic `0x504F434D` ("MCOP"), u32 protocolVersion, u32 modBuild, str name (<= 32 bytes used) |
| 0x02 | Welcome | H->G | R | u8 yourId, u8 maxPlayers, u8 count, count x {u8 id, str name} (includes host and you) |
| 0x03 | Reject | H->G | R | u8 code (1 version, 2 full, 3 bad handshake), str reason (human readable) |
| 0x04 | PeerJoined | H->G | R | u8 id, str name |
| 0x05 | PeerLeft | H->G | R | u8 id, str reason ("closed by remote", "timed out", ...) |
| 0x06 | Ping | both | U | u32 sender time ms |
| 0x07 | Pong | both | U | u32 echoed time ms |
| 0x08 | Chat | both | R | u8 from (host overwrites with the real sender), str text (<= 256) |
| 0x09 | PingTable | H->G | U | u8 count, count x {u8 id, u16 rtt ms} (every 2 s, for a ping display) |
| 0x10 | App | both | R/U | varint appId, u8 from, u8 target, u8 flags (bit0 = sent reliable), payload (rest) |

Handshake: transport connects -> guest sends Hello -> host replies Welcome (and broadcasts PeerJoined to the
others) or Reject (then drops the connection after 1 s so the Reject is delivered). The host drops connections that
send no Hello within 5 s and ignores everything but Hello before the handshake completes. Max 4 players; player ids:
host = 0, guests = 1..3 (lowest free id is reused). `0xFF` = all players.

Ping/Pong: each side pings every 1 s (unreliable). Guests keep their own RTT to the host; the host's PingTable fills
in the others.

### App messages and relay

Later phases register handlers with `session.On(appId, handler(from, ByteReader&))` and send with
`SendToHost`, `Broadcast`, `SendTo(player)`. Guests only ever talk to the host:

- guest `SendToHost` -> target 0: the host handles it.
- guest `Broadcast` -> target 0xFF: the host handles it AND relays it to every other guest.
- guest `SendTo(p)` -> target p: the host relays it to p only (or handles it if p = 0).
- host `Broadcast` / `SendTo` go straight to the guests. Host `SendToHost` loops back to its own handler.

The host always rewrites `from` with the real sender (guests cannot spoof each other) and relays on the same
channel the sender used (flags bit0).

App id allocation (u16, varint-encoded):

| Range | Owner |
|---|---|
| 0x0000-0x00FF | tests / debug |
| 0x0100-0x01FF | phase 3: player sync (ped transform, anim, weapon, vehicle seat) |
| 0x0200-0x02FF | phase 4: world (NPCs, cars, weather, time, mission state) |
| 0x0300-0x03FF | phase 5: UI / notifications |
| 0x0400-0x04FF | phase 6: damage, objectives |

## UDP transport

Datagrams are at most 1200 bytes (MTU). IPv4, address `ip[:port]` (default port 27960).

Header (8 bytes): `u16 0x434D` ("MC"), `u8 wireVersion` (1), `u8 type`, `u32 token`. The token is a random nonce
chosen by the connecting side; every packet after the handshake must carry it (stale or spoofed packets from the
same address are ignored).

| Type | Name | Body |
|---|---|---|
| 1 | CONNECT | u64 cookie (0 on first try), zero padding to 64 bytes |
| 2 | CHALLENGE | u64 cookie |
| 3 | ACCEPT | - |
| 4 | DATA | ack section, then units |
| 5 | PING | u32 id, u32 sender ms |
| 6 | PONG | echoed id, ms |
| 7 | DISCONNECT | - (sent twice) |
| 8 | REFUSE | u8 reason (1 not hosting, 2 full) |

Handshake (stateless anti-spoof): client sends CONNECT(cookie 0) every 250 ms. The host answers with
CHALLENGE(cookie = SipHash-2-4(secret, ip, port, nonce, 5-second time bucket)) without allocating anything; the
padded CONNECT is always larger than the CHALLENGE, so the host cannot be used as an amplifier. The client
repeats CONNECT with the cookie; the host accepts cookies from the current or previous bucket, creates the
connection and sends ACCEPT (re-sent if a duplicate CONNECT arrives). Any DATA/PING from the host also completes the
client's handshake if ACCEPT was lost. The client gives up after 10 s (`ConnectFailed`).

Keepalive: PING every 0.5 s; a peer is dropped after 10 s without any packet (`Timeout`). PING/PONG gives the RTT
and the loss estimate (fraction of settled pings without a pong, last 64).

DATA ack section: `u16 cumulative` (next reliable seq expected; everything before it was received), `u8 n` (0..8),
`n x u32` selective bits: bit k of word w = seq `cumulative + 1 + 32w + k` is buffered. Every DATA carries it; an
ack-only DATA is sent in the same Poll when reliable data arrived and nothing else is going out.

Units (as many as fit in one datagram):

```
u8 kind   0 unreliable | 1 reliable whole | 2 fragment first | 3 fragment middle | 4 fragment last
u16 seq   (reliable kinds only; 16-bit wrapping)
varint length, bytes
```

Reliable channel: each unit has a sequence number; window 256 units; the receiver buffers out-of-order units and
delivers strictly in order. Unacked units are resent after an RTO of `srtt + 4*rttvar` (50 ms .. 1 s, Karn's rule:
only first transmissions give RTT samples), doubled per retry up to 8x. Up to 128 new units per Poll. Messages
over 1100 bytes are split into fragments (first fragment starts with `u32 totalLength`, max 256 KB); since
fragments are reliable and ordered, reassembly is a simple append.

Test simulation (`UdpOptions.simLoss/simLatencyMs/simJitterMs/simDuplicate`) applies to outgoing datagrams; jitter
reorders them. `tests/net_loopback.cpp` runs at 20% loss + 20 +-15 ms + 5% duplicates.

## Steam transport

**Which steam_api.** The game does not load the `steam_api64.dll` in its folder: it memory-loads an embedded 2015
steam_api without a flat API (`docs/re-notes.md` 3.1). So the dll loads the on-disk `steam_api64.dll` itself
(`LoadLibraryW` from the game folder, falling back to a copy next to mafiacoop.dll), three seconds after the first
frame calls ITS `SteamAPI_Init` (a second pipe to the same running Steam client; the game's own Steam keeps working)
and from then on calls ITS `SteamAPI_RunCallbacks` once per frame from `Coop::Tick` (`src/dll/coop.cpp`). We never
call `SteamAPI_Shutdown`. Verified in game: `SteamAPI_Init` succeeds in two concurrent instances, persona name and
SteamID are read, and a friends-only lobby is created.

**Interface versions.** That on-disk `steam_api64.dll` (Steam Client API, source-control id 5697398). Its
exports, checked with pefile, are flat-API accessors `SteamAPI_SteamUser_v020`, `SteamAPI_SteamFriends_v017`,
`SteamAPI_SteamMatchmaking_v009`, `SteamAPI_SteamUtils_v009`, `SteamAPI_SteamNetworking_v006`,
`SteamAPI_SteamNetworkingSockets_v008`, `SteamAPI_SteamNetworkingUtils_v003`. We target exactly those
(`src/net/steam_flat.h`). (The game exe itself was built against older headers - SteamFriends015,
SteamNetworking005 - and uses its embedded copy; we never touch that one.)

**Why ISteamNetworking006 and not ISteamNetworkingMessages.** This dll predates ISteamNetworkingMessages: there
is no `SteamAPI_SteamNetworkingMessages_*` / `SteamAPI_ISteamNetworkingMessages_*` export. (The Steam client itself
does hand out `SteamNetworkingMessages002` through `SteamInternal_FindOrCreateUserInterface`, but using it would
mean calling C++ vtables directly, without any flat API - rejected as too fragile.) ISteamNetworking's P2P API is
the direct predecessor with the same model - message-based, addressed by SteamID, NAT traversal + Valve relay,
session requests the receiver must accept - and today's Steam client implements it on top of the same relay
network. Everything we need is a flat export: `SendP2PPacket`, `IsP2PPacketAvailable`, `ReadP2PPacket`,
`AcceptP2PSessionWithUser`, `CloseP2PSessionWithUser`, `AllowP2PPacketRelay`. Receiving is pure polling.

All our traffic uses P2P channel 7. Packet = `u8 0x4D` ('M'), `u8 ctrl`, body:

| ctrl | Name | Send type | Body |
|---|---|---|---|
| 1 | CONNECT | reliable, re-sent every 1 s | - |
| 2 | ACCEPT | reliable | - |
| 3 | DATA | reliable (`k_EP2PSendReliable`, ordered, <= 1 MB) | message |
| 4 | DATA | unreliable (`k_EP2PSendUnreliable`) | message |
| 5/6 | PING/PONG | unreliable no-delay | u32 id |
| 7 | DISCONNECT | reliable | - |
| 8 | REFUSE | reliable | - |

Connected/Disconnected events come from this control layer (CONNECT/ACCEPT, DISCONNECT, 10 s keepalive timeout,
20 s connect timeout) plus `P2PSessionConnectFail_t`.

**Accepting sessions.** `P2PSessionRequest_t` (id 1202) arrives when an unknown user sends to us. The host accepts
only if the accept filter allows the SteamID; the dll sets it to "is a member of our lobby". A request that is not
allowed yet is remembered for 15 s and accepted as soon as the filter allows it (lobby membership can become
visible a moment after the guest's first packet). Default filter: refuse everyone. Sending to a user implicitly
accepts their session, so the guest side needs nothing.

Two instances on one PC share a SteamID and cannot use this transport; use UDP (`127.0.0.1`) for that.

**Host = UDP + Steam at once.** The host's Session runs over a `MultiTransport` (`src/net/multi_transport.h`) that
owns a `UdpTransport` and a `SteamTransport`, hosts on both and routes each peer to the part it connected through.
Peer ids cannot collide: UDP ids are small counters, Steam ids are SteamID64s (a collision would be logged and the
new peer dropped). Guests use a single transport.

## Steam lobby and invites

`SteamLobby` (ISteamMatchmaking009 + ISteamFriends017):

- `Create(4)`: friends-only lobby, max 4. Lobby data `mc_version`, `mc_proto`, `mc_host` (host SteamID64),
  joinable. Rich presence `connect = "+connect_lobby <lobbyId>"` and `status`, so "Join Game" in the friends list
  works.
- `OpenInviteDialog()`: `ActivateGameOverlayInviteDialog(lobby)`.
- Invite accepted from the overlay -> `GameLobbyJoinRequested_t` (333). "Join Game" while running ->
  `GameRichPresenceJoinRequested_t` (337) with the connect string. Game started by Steam via "Join Game" ->
  `+connect_lobby <id>` on the command line (`SteamLobby::CommandLineLobby()`). All three call `onJoinRequested`
  (default: `Join`).
- `Join(id)` -> `LobbyEnter_t` -> host SteamID = lobby data `mc_host` (fallback: lobby owner) ->
  `SteamTransport::Connect(hostSteamId)`.

## Steam callbacks: approach and risks

Two kinds of results are needed:

1. **Call results** (`CreateLobby` -> `LobbyCreated_t`, `JoinLobby` -> `LobbyEnter_t`): polled every frame with
   `ISteamUtils::IsAPICallCompleted` / `GetAPICallResult`. No registration needed.
2. **Broadcast callbacks** (`P2PSessionRequest_t`, `P2PSessionConnectFail_t`, `GameLobbyJoinRequested_t`,
   `GameRichPresenceJoinRequested_t`, `LobbyChatUpdate_t`, `LobbyEnter_t`): these only reach code that registered for
   them, through the steam_api instance they were registered with.

`steam_flat.cpp` builds a `CCallbackBase`-compatible object (MSVC x64 layout: vtable at +0,
`uint8 m_nCallbackFlags` at +8, `int m_iCallback` at +12; three virtuals: the two `Run` overloads and
`GetCallbackSizeBytes`) and registers it with OUR steam_api's `SteamAPI_RegisterCallback(obj, id)`. Because the
game's callbacks go through its embedded steam_api, nobody else pumps ours: `Coop::Tick` calls our
`SteamAPI_RunCallbacks` every frame on the render thread, which calls `Run`. `Run` only copies the payload into an
SRWLOCK-guarded queue; `steam::Pump()` (called from `SteamTransport::Poll` / `SteamLobby::Poll`) hands it to the
subscribers. Both `Run` slots do the same thing (enqueue, ignore extra arguments), so the MSVC overload-ordering
quirk (overloaded virtuals are laid out in reverse declaration order) cannot misroute a call. (Classic dispatch is
kept rather than `SteamAPI_ManualDispatch_*`; with our own instance either would work, the registration path is the
one verified by the smoke test and in game.)

Verified by `tests/steam_smoke.cpp` against a copy of the dll and in game: the `LobbyEnter_t` broadcast is delivered
through our registered object, and the `LobbyCreated_t` call result is still readable by polling after
`SteamAPI_RunCallbacks` ran.

Risks:

- **Layout assumption.** If a future steam_api changed `CCallbackBase` (stable across SDK releases for many years),
  registration would break. We load a fixed dll from the game folder, so this only matters if the game updates it.
- **Two steam_api instances in one process.** Ours and the game's embedded one each have their own pipe to
  steamclient64.dll. Seen working (init, persona, lobby create); the game's overlay was not exercised by hand yet.
- **Unregistering** while dispatching would race; the dll never unregisters (objects live until process exit); only
  the standalone smoke test calls `steam::UnregisterAll()` before `SteamAPI_Shutdown`.
- **Call result consumed by RunCallbacks.** Steam might drop a call result nobody registered for when
  `RunCallbacks` sees its completion. Not observed, but as a safety net `SteamLobby` also listens for the
  `LobbyEnter_t` broadcast (posted for both create and join) and uses it if the call result cannot be read or does
  not arrive within 2 s.
- **Flat calls from the render thread.** Steam API calls are internally synchronised. The dll calls `SteamAPI_Init`
  once and `SteamAPI_RunCallbacks` per frame on its own instance only; never `SteamAPI_Shutdown`.
