# Handshake cancellation contract

OpenT1 intentionally accepts a generation-validated Disconnect in Requesting,
AwaitingDecision and Accepting, in addition to Connected. The wire encoding is
unchanged. This is a receiver extension, not identical vanilla state handling.
It preserves the pending-cancellation/player-cleanup regression fixed in #47.

The source reference is TribesRebirth
`1105fd0890c19c13f816b91e51b9cf0658ffc63c`,
`darkstar/DNet/code/VCProtocol.cpp::receive`,
`darkstar/Sim/code/netCSDelegate.cpp`, and
`program/code/fearCSDelegate.cpp::cancelDuringConnection`. Live observations
below use a separately hashed installed native 1.11 executable; source and
runtime evidence are distinguished because those artifacts are not identical.

## State and event order

| Action | Client | Server | Termination implication |
| --- | --- | --- | --- |
| Send Request | Requesting | Unbound | Client retains a cancellable VC before a packet stream exists |
| Receive Request | Requesting | AwaitingDecision; Requested event | No acceptance has been sent |
| Accept request | Requesting | Accepting | Game delegate may already allocate representative/stream and run the connect hook |
| Receive Accept | Sends acceptance ACK before delivering Accepted | Still Accepting until an ACK is received | Local acceptance does not prove the server has consumed its ACK |
| Receive acceptance ACK | Connected | Connected; Connected event | ACK runs are processed before payload dispatch, even if the packet sequence is a duplicate |
| Disconnect after receiving Accept, with standalone ACK lost | Retired locally | ACK carried by Disconnect promotes Accepting -> Connected, then dispatches Disconnected | Supported by both implementations; it does not rely on the receiver extension |
| Disconnect before receiving Accept | Retired locally | OpenT1 retires AwaitingDecision/Accepting; vanilla does not dispatch termination until Connected | Intentional OpenT1 extension |
| Ordinary disconnect | Retired locally | Connected -> retired | One departure hook; duplicate disconnects do not repeat it |

In vanilla, processing an ACK can make the server Connected in the *same*
receive call that subsequently handles Disconnect. Looking only at the final
`Disconnect && Connected` condition misses this transition.

## Native process evidence

The [recorded matrix and header-only traces](evidence/handshake-live-2026-11-05.json)
include exact executable/memory-DLL SHA-256 values and Windows version. Native
processes use minimal scripts, real game delegates, and separate UDP sockets
through a loopback relay. No mission or mod is loaded in this handshake probe.
Its pass status means its listed cycle/count checks passed, not complete game
or UI compatibility.

| Client -> server | Three orderly/reconnect cycles | Accept received, standalone ACK/data held until Disconnect | Accept withheld, OpenT1 cancellation then replacement join |
| --- | --- | --- | --- |
| Native -> native | 3 accepts, 3 departures, empty roster | Same result | Native UI path is a separate manual check |
| Native -> OpenT1 | 3 accepts, 3 departures, empty roster | Same result | Native UI path is a separate manual check |
| OpenT1 -> native | 3 accepts, 3 departures, empty roster | Same result | Replacement succeeds; first departure occurs after replacement Request, not before it |
| OpenT1 -> OpenT1 | 3 accepts, 3 departures, empty roster | Same result | First departure observed before replacement Request |

The relay releases the old acceptance after the replacement Request, so these
last two rows also check that the old acceptance cannot complete the new join.
The native server's first replacement Request retires the old VC, but that
Request is not itself processed as a new join; the new client's retry completes
it about three seconds later. Successful reconnect therefore must not be
reported as proof that a native server consumed the earlier cancellation.

The user also exercised Back in the full installed T1Vista 1.11 shell, with the
server acceptance withheld, against both an OpenT1 server and a native server.
Back reached the installed EndGame script and returned to a usable join menu.
Both runs logged `disconnect: not connected` and sent no Disconnect packet.
This is a limit of that installed native UI/script path, not evidence that an
OpenT1 receiver ignored a packet. T1Vista is recorded separately in the broader
lifecycle evidence; it is not substituted for the stock executable in this table.

## Generation, ordering and remaining boundaries

Deterministic protocol tests cover cancellation before a server poll, while
awaiting a decision, with acceptance in flight, after local acceptance but before
server ACK receipt, and after connection. They cover duplicate acceptance,
duplicate Disconnect, an acceptance ACK carried by Disconnect, endpoint reuse,
delayed old Accept/Reject/Disconnect with both different and matching parity,
and timeout despite repeated stale control packets. Stale control generations
are rejected before ACK/RTT/timeout state mutates.

The compatibility contract has explicit limits:

- Cancellation sent before an unknown peer's delayed Request arrives cannot
  erase a request the receiver has not seen. Neither implementation retains a
  cancellation tombstone for an unknown endpoint.
- A Request with a different full generation retires the endpoint's old VC,
  following vanilla. A delayed old Request can therefore disturb endpoint reuse;
  this is distinct from stale Accept/Reject/Disconnect validation.
- Data and standalone ACK packets carry only one generation parity bit. Their
  ambiguity after two generations cannot be eliminated by full-token validation
  without changing the existing wire format.
- Packet loss can lose an orderly Disconnect. Timeout remains the fallback;
  prompt departure claims apply when the matching Disconnect is delivered.
- Native UI pending cancellation, loading-screen behavior, mission/datablock
  cleanup, kicking, shutdown, mod content and internet paths require their own
  lifecycle evidence. A source comparison alone does not establish them.

## Reproduce

On Windows, from the repository root, with an existing game installation:

```powershell
python tools/run_disconnect_interop.py `
  --opent1 build/x64-debug/opent1.exe `
  --vanilla C:/Games/Tribes `
  --output build/handshake-live
```

Use `--memory-dll PATH` only when deliberately selecting another existing
compatible memory DLL; its hash is always recorded. The tool launches hidden
native processes and never automates a UI. It writes generated scripts, process
logs, a summary and header-only packet traces to a new output directory. Native
pending UI cancellations are `not_run`, not passes. `cancel_request` is an
additional exploratory scenario for delivery before server receipt. Do not
commit the copied executables, DLLs or installed game content.
