# ESP32_C6_zigbee_human_presence

This is not an application firmware. It is a minimal backend disguised as an
embedded probe.

The `ESP32-C6` runs as a Zigbee coordinator and observes one problem only:
understanding when an HU presence sensor enters and leaves the human-detected
state, without the noise of everything else. No cloud. No final display. No
decorative automation. Just network, state, logs, and a local LED that tells
the truth if the software knows it.

## Idea

The sensor should not be treated as an elegant device exposing a clean,
orthodox ZCL model. It should be treated as a node that can:

- speak standard when it feels like it
- speak vendor when that is more useful
- change `short_addr`
- ignore ZDO discovery while still transmitting useful bursts

So the software does not begin with "what spec does the device claim to
implement", but with "what is the most reliable source of truth right now".

In this project the source of truth is, in practical order:

1. real `EF00` bursts
2. `IAS Zone` notifications
3. standard `Occupancy` reports
4. discovery and metadata, but only as context, not as operational truth

## Modello mentale

The system has three distinct layers.

`Rete`

The coordinator forms or reopens the Zigbee network, registers a local
endpoint, and puts itself in a position to receive traffic that the stack
would otherwise discard.

`Contesto device`

The software keeps a small in-memory model of the current sensor: short
address, endpoint, logical type, observed cluster, presence of vendor cluster
`0xEF00`, and a few IAS metadata fields.

This context is persisted only in its stable part. Not for convenience, but to
avoid lying to itself after a reboot.

`Stato presenza`

`PRESENT` and `CLEAR` are not configuration. They are pure runtime.
If you persist them, you introduce memory where there should be observation.
As soon as you make that mistake, the system can come back already in
`PRESENT`, which is exactly the thing a presence sensor must never invent.

## Invariants

This repo lives on a few hard rules.

- Presence is not persisted.
- Sensor configuration can be persisted.
- Real bursts matter more than failed discovery.
- Bootstrap must not generate useless traffic that alters the sensor.
- The local LED is only a view of internal state, not a source of state.

If you break one of these rules, the project may still compile, but it stops
being trustworthy.

## Why the local endpoint exists

The Zigbee stack forwards some callbacks only if the coordinator really exposes
the cluster on the correct side. That is why the probe registers a local
endpoint with the required client clusters.

This is not architectural aesthetics. It is an entry tax imposed by the stack.
If you do not do it, traffic may exist on the air and still not exist for your
software.

## The HU presence case

The milestone frozen in this repo is the HU presence Zigbee sensor.

The important observations were these:

- `EF00` bursts do arrive
- the device can change `short_addr`
- `zb_discover` can fail even while the sensor is alive
- `PRESENT` and `CLEAR` are observed correctly only if the software stays
  passive and does not reinvent state at boot

So the backend stopped chasing an ideal device and started following real
traffic.

## Persistence: what is saved, what is not

Saved:

- `short_addr`
- `endpoint`
- `kind`
- `cluster_id`
- `has_vendor_cluster`
- stable IAS context fields

Not saved:

- `occupancy_known`
- `occupied`
- `zone_status`

This separation is the heart of the project. Without it, the probe becomes a
writer of fiction.

## LED as a debugging instrument

The onboard LED of the `ESP32-C6-Zero` is not a UI effect. It is an immediate
debugging instrument.

- `unknown` -> spento
- `CLEAR` -> bianco
- `PRESENT` -> viola

The board was validated with `RGB` mapping, not `GRB`. This detail looks
trivial until you ask for purple and get turquoise. At that moment you realize
the hardware is telling the truth about byte order before the logs do.

Board reference: [Waveshare ESP32-C6-Zero](https://www.waveshare.com/wiki/ESP32-C6-Zero?srsltid=AfmBOopFxMjO_4ma8wf0lXDpBSiPipDySdWLvjq7NPo_uu-O3YDNXeze)

## The files that matter

- `main/app_main.c`
  NVS bootstrap, Zigbee platform setup, main task startup
- `main/zb_coordinator.c`
  network, stack signals, raw `EF00` handler, Zigbee callback wiring
- `main/zb_presence.c`
  sensor model, discovery, bind, vendor DP parsing, context persistence, state
  transitions
- `main/presence_led.c`
  translation of software state into local LED feedback

## What it is not

It is not yet the e-ink display firmware.
It is not a finished product.
It is not a generic library for any Zigbee sensor.

It is an isolated base, useful precisely because it reduces the world to a
single question:

"is this sensor saying there is a person, or not?"

Until that question becomes boring and reliable here, integrating it into the
rest makes no sense.
