# LAN handoff

LAN handoff is an application-level Interaction/Command feature. It does not run
inside native perception and does not alter the existing authenticated gesture
validation server.

`HandoffService` broadcasts a short-lived UDP offer on port 49667. The offer
contains a random one-time claim credential and an ephemeral TCP port. A peer
claims the offer over TCP and receives one payload; successful transfer consumes
the offer. Offers expire after 30 seconds.

Security and lifecycle limits:

- payloads are limited to 16 MiB and protocol lines to 4096 bytes;
- payload IDs and transfer lengths are validated before allocation;
- claim credentials use 256 bits of randomness and fixed-time comparison;
- received names are reduced to a filename and the WPF provider verifies the
  final path remains below `Pictures/RyoikiTenkai Handoff`;
- capture, connect, transfer, save, and open operations accept cancellation;
- shutdown cancels listeners, awaits the receive/broadcast/server loops and all
  accepted clients, with a bounded five-second drain.

The WPF provider captures the application window and is deliberately only a UI
adapter. Gesture command bindings map explicit `handoff.grab` and
`handoff.release` actions to the long-lived service instance; they must not create
a new network service per confirmed gesture.

## Gesture-only operation

Transfer is started exclusively by recognized gestures, matching the reference PR.
The main window exposes no Offer/Receive buttons, and none should be reintroduced:
a manual control bypasses the ordered-event path that enforces consecutive-match
confirmation, cooldown, and duplicate-segment suppression, so it can produce a
transfer that gesture recognition would have rejected.

To use the feature, register a gesture, then bind it under **Action binding** to
either `Offer screenshot over LAN` (`handoff.grab`) or `Receive latest LAN offer`
(`handoff.release`).

`HandoffService.GetStatus()` reports the observable phase; the caller only
displays it and never advances it:

| State | Meaning |
| --- | --- |
| `Idle` | No payload held and no unexpired offer is visible. |
| `Advertising` | A grabbed payload is being broadcast, with its remaining TTL. |
| `OfferAvailable` | A peer offer is visible and can be claimed by a release gesture. |
| `Claiming` | A claim is in flight. |
| `Completed` | A payload was sent or received; shown for 10 seconds. |
| `Failed` | The last grab or release failed; the reason is shown for 10 seconds. |

Each phase change also plays one short screen effect (`HandoffEffectWindow`).
It is a separate top-level, click-through, `WS_EX_TRANSPARENT` window because WPF
content cannot reliably overlay the native preview's `HwndHost`, and because the
overlay must never intercept the input the gesture path depends on. The ring
expands for `Advertising` and contracts for `Claiming`, so send and receive are
distinguishable at a glance. `OfferAvailable` only plays on the way up from
`Idle`, so a peer re-advertising every 900 ms cannot strobe the screen.

A `handoff.release` with no unexpired offer fails rather than picking a peer, so a
release gesture can never send to an arbitrary device. When several peers are
advertising, the most recently received unexpired offer is the claim target;
selecting among simultaneous offers is not yet supported.
