# Beam State PVs for MLDP Time-Series Tagging

This document describes the EPICS Process Variables (PVs) used to tag time-series
data as it is ingested into the **Machine Learning Data Platform (MLDP)**. These
tags capture the machine's **operating mode**, **beam destination**, and
**beam rate** for each collected sample.

> **Tagging principle:** Always tag by the **decoded enum string**, not the raw
> enum index. Enum orderings differ between the HXR (hard) and SXR (soft) lines.

---

## 1. Operating Mode — SASE vs. Self-Seeding

Labels each record with the lasing / seeding configuration.

| PV | Purpose |
|----|---------|
| `MPS:UNDH:2850:HXRSS_MODE` | HXR hard-line self-seeding mode (`DBF_ENUM`). SASE vs. Seeded on the hard line. |
| `MPS:UNDS:3500:SXRSS_MODE` | SXR soft-line self-seeding mode (`DBF_ENUM`). SASE vs. Seeded (and delay-line / slit) on the soft line. |
| `XTAL:UNDH:2850:IN_ENCDR_MPS` | HXR seeding-crystal insertion / MPS flag — physical crystal in vs. out (complements `HXRSS_MODE`). |

**HXR mode enum — `MPS:UNDH:2850:HXRSS_MODE` (3 states)**

| Value | State |
|-------|-------|
| 0 | Undefined State |
| 1 | Seeded Mode |
| 2 | SASE / Phase Shift Mode |

**SXR mode enum — `MPS:UNDS:3500:SXRSS_MODE` (5 states)**

| Value | State |
|-------|-------|
| 0 | Undefined State |
| 1 | SASE Mode |
| 2 | Seeded Mode |
| 3 | Delay Line Mode |
| 4 | SLIT Mode |

> **Note:** Enum orderings differ (HXR: `Seeded = 1`, `SASE = 2`; SXR: `SASE = 1`,
> `Seeded = 2`). Decode to strings before tagging.
>
> **Physical contrast:** HXR self-seeding uses a seeding **crystal** (hard line);
> SXR self-seeding uses a **grating monochromator** (soft line).

---

## 2. Beam Destination — where the beam is going

Tags which dump / destination each shot was delivered to.

| PV | Purpose |
|----|---------|
| `PHYS:SYS0:1:CU_HXR` | Hard-line beam destination (`DBF_ENUM`). "Slow" PV. |
| `PHYS:SYS0:1:CU_SXR` | Soft-line beam destination (`DBF_ENUM`). "Slow" PV. |
| `PHYS:SYS0:1:CU_HXRBR` | BSA readback of HXR destination (append `BR` Post suffix). Numeric, follows the same enum. |
| `PHYS:SYS0:1:CU_SXRBR` | BSA readback of SXR destination (append `BR` Post suffix). Numeric, follows the same enum. |

**HXR destination enum — `PHYS:SYS0:1:CU_HXR` (7 states)**

| Value | State |
|-------|-------|
| 0 | Beam Off |
| 1 | Inj. Spectrometer |
| 2 | TD-11 |
| 3 | D2 |
| 4 | BYKIK |
| 5 | TDUND |
| 6 | HXR Dump |

**SXR destination enum — `PHYS:SYS0:1:CU_SXR` (7 states)**

| Value | State |
|-------|-------|
| 0 | Beam Off |
| 1 | Inj. Spectrometer |
| 2 | TD-11 |
| 3 | ST-CLTS |
| 4 | BYKIKS |
| 5 | TDUNDB |
| 6 | SXR Dump |

> **Note:** HXR and SXR destination enums share indices 0–2 but diverge at higher
> indices. Decode to strings before tagging. For shot-level, time-aligned tagging,
> prefer the **BR (BSA)** variants.

---

## 3. Beam Rate — desired / scheduled vs. actual

Tags delivered rate and beam-on / off state per destination.

| PV | Purpose |
|----|---------|
| `EVNT:SYS0:1:NC_HARD_INJRATE` | Numerical Hard-line injection rate — **authoritative** for whether Hard beam is delivered. |
| `EVNT:SYS0:1:NC_SOFT_INJRATE` | Numerical Soft-line injection rate — **authoritative** for whether Soft beam is delivered. |
| `IOC:IN20:EV01:RG02_ACTRATE` | Timing-system enum (`HXR n / SXR n`). Informational; **not** used to decide beam presence. Not BSA. |
| `IOC:BSY0:MP01:PC_RATE` | MPS permit enum. Check for **NO PERMIT** = enum **1**. Not BSA. |

**Example — a 120 Hz total split across the two lines:**

| PV | Value |
|----|-------|
| `EVNT:SYS0:1:NC_HARD_INJRATE` | 110 |
| `EVNT:SYS0:1:NC_SOFT_INJRATE` | 10 |
| **Total** | **120 Hz** (110 Hard + 10 Soft) |

> The 120 Hz total is apportioned between Hard and Soft destinations. Sum the two
> `INJRATE` PVs to confirm the total delivered rate and to compute the per-line
> duty cycle for each tagged sample.

---

## Ingestion & Tagging Notes

- **Decode enum strings** — tag by string, never raw index; HXR and SXR use
  different enum orderings for both mode and destination PVs.
- **Beam-off is a destination state** (enum `0` on both `CU_HXR` / `CU_SXR`) —
  use it together with the `INJRATE` PVs to confirm beam-on / off per line.
- **Non-BSA rate PVs** (`RG02_ACTRATE`, `PC_RATE`) — repeat the previous value
  when the alarm state is not `INVALID`.
- **Mode-PV quality** — treat `Undefined State` (enum `0`) and elevated severity
  (e.g., `STATE` / `MAJOR`) as invalid / unknown when tagging.
- **Severity handling** — the published BR PVs do not yet propagate `.SEVR`;
  mark `NaN` / `INVALID` samples on the ingestion side.
- **Beam-off detection** — mode and destination PVs update at 120 Hz, but the
  logic first checks for a beam request in the timing frame. If none is present,
  the shot is tagged **beam off** even when a valid rate is reported.

---

## Quick Reference — `caget` Inspection

```bash
# Inspect enum states of a mode or destination PV
caget -d DBR_GR_ENUM MPS:UNDH:2850:HXRSS_MODE
caget -d DBR_GR_ENUM MPS:UNDS:3500:SXRSS_MODE
caget -d DBR_GR_ENUM PHYS:SYS0:1:CU_HXR
caget -d DBR_GR_ENUM PHYS:SYS0:1:CU_SXR

# Current beam-rate split
caget EVNT:SYS0:1:NC_HARD_INJRATE EVNT:SYS0:1:NC_SOFT_INJRATE
