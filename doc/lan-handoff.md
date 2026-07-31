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
adapter. Gesture command bindings should map explicit `handoff.grab` and
`handoff.release` actions to the long-lived service instance; they must not create
a new network service per confirmed gesture.
