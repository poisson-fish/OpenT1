# Simulation networking

`opent1_sim` supplies `SimNetObject` and the portable
`opent1::sim::network::{PacketStream,EventManager,GhostManager,CSDelegate}`.
They use the datagram/session protocol in `opent1_net`. No SDL, Windows handle,
game singleton, global network error buffer or pointer-sized notification key
appears in the simulation protocol.

## Using connected worlds

```cpp
#include <opent1/sim/sim_net.hpp>
#include <opent1/sim/sim_world_game.hpp>

using namespace opent1;
using namespace opent1::sim;
namespace network = opent1::sim::network;

auto registry = std::make_shared<network::Registry>();
network::registerServiceGhosts(*registry); // volume alias 1, volumetric alias 134
// Register application events/ghost classes before sharing the registry.
// registerConsoleEvent/registerRemoteCreateEvent are explicit capabilities.

SimWorld server;
auto delegate = std::make_unique<network::CSDelegate>(
    net::makeUdpTransport({{127,0,0,1},28001}), registry, true);
delegate->clock = monotonicMilliseconds; // host function returning UInt32
server.manager().addObject(std::move(delegate));

// Advance the world from the host loop. Its existing delegate wakeup receives
// packets first; its end-of-advance packet-set event performs paced sends.
server.advanceToTime(simulationSeconds);
```

A client installs a delegate with `server=false`, then calls
`connect(address, now, requestBytes)`. `acceptRequest` can inspect request bytes
before accepting. `configureStream` runs after event and ghost clients are
installed and before the stream is exposed to application connection callbacks.
Callbacks must be set before connecting. The eight-byte acceptance payload is
the little-endian assigned-client/server-manager ID pair. Servers use
2048; client peers use 2049 through 3071. A manager's local address remains object
ID zero independently of its network identity.

The delegate owns transport/session state and removes all its streams on removal.
Each stream is an owning simulation group for its registered packet clients.
Ghost managers own staged bootstrap objects, then transfer ownership to the
receiving simulation manager. Removing a ghost manager deletes its received
objects even if their local IDs have changed. Use fresh stream/client instances
for a new connection. All driving, graph edits and registry configuration use one
thread. Immutable registries may be shared between independently driven worlds.

`clock` supplies monotonic milliseconds modulo 2^32. `process(now)` and
`checkPacketSend(now)` are also available for hosts that drive networking directly.
Without a clock callback, world wakeups reuse the last explicit `process` time;
they do not read an implicit OS clock. `onRawPacket` exposes owned-session raw
datagrams during its callback. Version-0x10 discovery packets route through the
delegate's instance-owned `browser()`; other protocols reach `onRawPacket`.

## Wire behavior and bounds

Packet clients have a positional layout: matching peers must install
the same clients in the same order. The delegate installs events, then ghosts.
Client removal makes the stream unusable; it cannot silently shift the layout.
Rate headers retain two presence bits and 10-bit delay/size fields. Explicit
preferences clamp to 1-30 packets/second and 100-450 bytes by default. The
generic stream starts with a 100 ms delay and 200-byte size. `setMaxRate` has an
explicit `wireLimit` parameter that can raise the size ceiling as far as 1,023
bytes; both peers must support the selected limit. This optional API setting
does not change the game's normal 450-byte limit. Lost rate updates are
advertised again. Reply streams
retain immediate reply wakeups and otherwise wait one-and-a-half update intervals.
`setPacketFrame(optionalMilliseconds)` explicitly selects Tribes frame
pacing, clamped to 32-128 ms. Sequence throttling and lower-layer VC
window/timeout limits both apply; see [transport sessions](#transport-sessions).

Packet sizes are hard bounds rather than the old soft buffer-overrun allowance.
An event must fit the negotiated packet; event fragmentation is not provided.
Ghost encoders can return unsent mask bits to split updates across packets.
There are at most 32 packet clients and 32 outstanding packet notifications.
Notification keys are monotonically allocated UInt32 identities, never pointers;
unknown or repeated notifications cannot dereference stale storage.

Remote events retain 7-bit class tags (1024-1151), guaranteed/unreliable headers,
7-bit ordered sequences, adjacent-sequence compression, a 127-event send window,
and guaranteed retransmission ahead of later queued events. The default retained
event limit is 4096, configurable through 65536; it includes in-flight guarantees.
At most 128 ordered receive slots and 4096 polled addresses are retained. The VC
layer supplies packet deduplication; direct packet-codec callers must provide the
same delivery contract. Polled snapshots tolerate callback mutation/removal.

Event codecs are explicitly registered by type and receive role. Encoders and
decoders must use bounded streams; decoders should only decode owned values.
`packAddress`/`unpackAddress` use regular/ghost/parent address
encodings. Received ghost addresses resolve to a local generation before an
ordered event waits, so slot reuse cannot redirect a delayed event. Missing targets
are discarded. `registerConsoleEvent` retains Huffman command payloads and the
`remote` command prefix/source-ID insertion. It does not enable raw remote script
evaluation. `registerRemoteCreateEvent` retains event 1057's 10-bit class tag/full
update payload, transfers the decoded object into manager ownership and rejects
server-side receipt.

Ghost replication has 1023 usable slots; 1023 is the reserved invalid index.
ID widths grow from 3 to 10 bits. Creation, deletion, partial masks, dropped-update
mask recovery, pending-creation acknowledgement, priority/skip ordering,
scope-local filtering and scope-always bootstrap are implemented. Bootstrap
objects receive updates before registration and register in source creation
order when bootstrap finishes. Normal scope can be supplied by a camera object's
`buildScopeAndCameraInfo`; the default walks the actual manager graph with a
65,536-object/work-item bound and deduplicates set membership. `reset` stops
scoping and retires outgoing ghosts; it leaves received ghosts until removal.

The 20-byte SimNetObject persistence prefix uses explicit fixed-width words,
including the reserved zero pointer word. Unknown flag bits are retained.
Source subscriptions and local ghost deletion notifications balance removal
and ID changes without stale borrowed pointers. `SimVolume` and spatial containers
derive from `SimNetObject`; volumes/volumetrics advertise their registered class tags
and real state changes set update masks. Missing volume mounts fail registration
and therefore fail the connection rather than creating a fake resource.

Malformed packets make a stream terminal; delegates disconnect and remove it.
Native mission-light ghosts validate the fields used by their light type.
Directional-light updates can carry unused range/position fields, and
point-light updates can carry an unused direction, even though a full ghost
update transmits all of them. These unused fields are sanitized before the
light enters the scene; they must not reject the connection. A zero-range point
light contributes only ambient illumination. Regression coverage includes raw
ghost payloads with zero and non-finite unused fields and still rejects invalid
active light fields.
Application receive callbacks can run before a later client detects malformed
data, so whole-packet rollback of arbitrary game side effects is not promised.
Receive exceptions likewise poison the stream and delegate cleanup runs before
propagation. Sender callback exceptions restore reliable queues and ghost masks.
Callbacks may remove themselves, peers, clients, streams or delegates; manager
dispatch retains removed targets through the callback stack. Recursive drives
are rejected. The final byte's unused bits are ignored.

## Transport sessions

`opent1::net::Session` borrows a `Transport` and exposes owned events through
`poll`. Available datagram transports include UDP and an in-process loopback
pair. The game transport routes UDP and, on Windows when an OS provider exists,
IPX. Unsupported IPX sends return `UnsupportedTransport`; they are not tunneled
over UDP. TCP streams serve separate services such as the console and IRC.

Sessions use caller-supplied monotonic milliseconds modulo 2^32. The default
`SessionConfig` allows 64 connections, 256 queued events, and 256 received
packets per `process` call. Connection and event capacities are configurable
within the API bounds. Callers must poll events to release queue backpressure
and handle `WouldBlock`, `WindowFull`, and `Capacity` results explicitly.

The connection codec uses a 32-slot acknowledgement/notification ring. Send
window checks can refuse data before that ring is reused. Silence triggers a
ping or retransmission after successive 3,000 ms thresholds measured from the
last received packet; a connection times out after the sixteenth successful
probe, normally just over 48 seconds without incoming traffic. Backpressure or
infrequent processing can delay that timing. Guaranteed simulation events and
ghost update masks recover through packet delivery/drop notifications above
this layer; the datagram transport itself does not fragment oversized events.

## Game connection and prediction

`FearCSDelegate` adds game identity/version checks, join parameters, and server
browser responses. `FearPlugin::configureRemoteStream` configures a maximum of
30 packets per second, a 450-byte packet ceiling, and a 32 ms packet frame.
It installs `PlayerPSC` between the event and ghost clients; matching packet
client order is part of the wire contract.

Connection setup binds player representatives, datablocks, control objects,
and replicated world objects to their managers. `PlayerPSC` exchanges input
moves and control state; the game uses those updates for client prediction and
server correction. Datablock codecs and validated shape strings use explicit
wire serialization and resource checksums. The game advertises compatibility
version `1.11`; those protocol identifiers are distinct from a project release
version or build timestamp.

Regression cases cover selected event, ghost, control, discovery, and game
handshake paths. The default suite runs against test peers and bundled records,
so it does not establish interoperability with every live server or client.
