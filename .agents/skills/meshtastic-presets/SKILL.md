---
name: meshtastic-presets
description: Reference table of all Meshtastic LoRa modem presets — general-purpose (Short/Medium/Long × Fast/Slow/Turbo/Moderate) and narrowband (Lite/Narrow/Tiny) — with SF, bandwidth, coding rate, data rate and region legality per preset. Use whenever the user names any Meshtastic preset or asks what parameters, defaults or regional restrictions a preset has — even in passing, e.g. "use LongFast for this run" or "same preset as the capture".
---

# Meshtastic presets

## General-purpose presets

| Preset                | Alt name                             | Data rate  | SF  | CR  | BW      |
| --------------------- | ------------------------------------ | ---------- | --- | --- | ------- |
| Short Range / Turbo   | Short Turbo                          | 21.88 kbps | 7   | 4/5 | 500 kHz |
| Short Range / Fast    | Short Fast                           | 10.94 kbps | 7   | 4/5 | 250 kHz |
| Short Range / Slow    | Short Slow                           | 6.25 kbps  | 8   | 4/5 | 250 kHz |
| Medium Range / Turbo  | Medium Turbo                         | 7.03 kbps  | 9   | 4/5 | 500 kHz |
| Medium Range / Fast   | Medium Fast                          | 3.52 kbps  | 9   | 4/5 | 250 kHz |
| Medium Range / Slow   | Medium Slow                          | 1.95 kbps  | 10  | 4/5 | 250 kHz |
| Long Range / Turbo    | Long Turbo                           | 1.34 kbps  | 11  | 4/8 | 500 kHz |
| Long Range / Fast     | **Long Fast** (Meshtastic default)   | 1.07 kbps  | 11  | 4/5 | 250 kHz |
| Long Range / Moderate | Long Moderate                        | 0.34 kbps  | 11  | 4/8 | 125 kHz |

## Narrowband presets (region-restricted)

| Preset        | Alt name    | Data rate | SF  | CR  | BW       | Regions                                                  |
| ------------- | ----------- | --------- | --- | --- | -------- | -------------------------------------------------------- |
| Lite / Fast   | Lite Fast   | 1.76 kbps | 9   | 4/5 | 125 kHz  | EU_866 (865.6–867.6 MHz SRD)                             |
| Lite / Slow   | Lite Slow   | 0.98 kbps | 10  | 4/5 | 125 kHz  | EU_866                                                   |
| Narrow / Fast | Narrow Fast | 2.28 kbps | 7   | 4/6 | 62.5 kHz | EU_N_868, 70cm + 1.25m amateur (ITU\*\_70CM, ITU2_125CM) |
| Narrow / Slow | Narrow Slow | 1.30 kbps | 8   | 4/6 | 62.5 kHz | EU_N_868, 70cm + 1.25m amateur                           |
| Tiny / Fast   | Tiny Fast   | 0.68 kbps | 7   | 4/5 | 15.6 kHz | 2m amateur (ITU\*\_2M)                                   |
| Tiny / Slow   | Tiny Slow   | 0.33 kbps | 8   | 4/6 | 15.6 kHz | 2m amateur                                               |

## Region and compatibility rules

- Nodes hear each other only on a shared preset; where the primary channel
  name is unchanged, the preset also determines the frequency slot.
- LONG_FAST is the firmware default (Modem Preset unset = LONG_FAST). A US
  node whose region is first set from its own screen starts on LONG_TURBO
  instead — LONG_TURBO and LONG_FAST cannot hear each other.
- The region gates the preset list: EU_868 offers the general presets minus
  all three Turbos (500 kHz is not permitted in its 869.4–869.65 MHz band).
  A preset the region disallows is silently replaced by the region's default;
  a remote-admin change containing one is rejected whole.
- EU_868, EU_866 and EU_N_868 share a regulatory domain: selecting a Lite or
  Narrow preset while on EU_868 switches the region to EU_866 or EU_N_868,
  and any general-purpose preset EU_868 allows switches it back.
- Narrowband family defaults: LITE_FAST, NARROW_SLOW, TINY_FAST.
- Tiny presets occupy 15.6 kHz, so they require a TCXO of ±5 ppm or better
  and work only with SX127x and SX126x radios.

## Trade-offs (why the table looks the way it does)

- Each SF step up doubles airtime and adds ~2.5 dB link budget. SF5/6 exist
  only on SX126x/LR11xx/SX128x; SX127x/RF95 clamp a requested SF5/6 to SF11.
- Each BW doubling costs almost 3 dB link budget but raises the data rate
  proportionally. Bandwidths below 31 kHz are unstable without a quality
  crystal oscillator.
- Higher coding rate (4/5 → 4/8) buys reliability with proportional
  throughput loss: 1.25×, 1.5×, 1.75×, 2× overhead.
- The presets trade speed against range: faster presets lower per-device
  airtime and help congestion on large/busy meshes; slower presets buy range
  where seconds of delay are acceptable.
