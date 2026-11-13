title: P1-08 Reliable queues
labels: phase-1,needs-human-review
---

**Depends on:** P1-06

## Scope

- `scada_queue_open`, `scada_queue_post`, `scada_queue_next`, `scada_queue_ack`, `scada_queue_close` for event, command and result queues.
- Record structures from the contract; request id generation.
- `FULL`, `PEER_DOWN` and at-least-once redelivery.

## Acceptance

- [ ] Conformance test 10 passes.
- [ ] One million records pass through a queue with no loss, duplication or reordering.
- [ ] `scada_queue_post` passes the real-time instrumentation.

## References

contract section 9

## Review

This issue needs review by a person before it is closed.
