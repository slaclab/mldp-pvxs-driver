# SLAC Calendar → MLDP Configuration / Activation Mapping

Proposed mapping of the JSON fields returned by the calendar API
(`https://aosd.slac.stanford.edu/program_calendar/<accel>/events.json`)
to the MLDP payloads produced by `SlacCalendarReader::pushEvent`
(`src/reader/impl/slac_calendar/SlacCalendarReader.cpp`).

## Sample event

```json
{
  "url": "https://www.google.com/calendar/event?eid=ZzYyZG9tbHQwaGk3ZDYyc3Y3NGU0N2J2ajQgNnNyNDE0bmVoYnVnbjJrNjdlZnE3MW1wcGdAZw",
  "program_name": "XPP 1020896 Halavanau",
  "description": "Deliver to XPP",
  "note": "10.7 GeV",
  "calendar": "NC-XPP",
  "details": "https://pswww.slac.stanford.edu/questionnaire_slac/proposal_questionnaire/run27/1020896/#xray\nActual start: 09/28/2026 18:06",
  "start": "2026-09-28T18:00:00-07:00",
  "end": "2026-09-29T06:00:00-07:00",
  "tags": null,
  "poc": "Sun",
  "config": "9.831 keV",
  "hutch": { "name": "XPP", "color": "#00a53e", "line": "HXR", "text_color": "white" },
  "machine": "NC"
}
```

The event has no `location` field: the location is given by the `hutch`
block (`name`, `line`).

## Proposed mapping

| JSON field | Configuration | Activation |
|---|---|---|
| `program_name` | `configurationName` (and `category` by default) | `configurationName` |
| `description` | `description` | `description` |
| `tags` | `tags` | `tags` |
| `calendar` | `attributes.calendar` | `attributes.calendar` |
| `machine` | `attributes.machine` | — |
| `hutch.name` | `attributes.hutch_name` | `attributes.hutch_name` |
| `hutch.line` | `attributes.hutch_line` | `attributes.hutch_line` |
| `hutch.color` | `attributes.hutch_color` | — |
| `hutch.text_color` | — (dropped) | — |
| `note` (electron beam energy) | — | `attributes.note` |
| `config` (photon energy) | — | `attributes.config` |
| `poc` (person on shift) | — | `attributes.poc` |
| `details` (HTML stripped) | — | `attributes.details` |
| `start` | — | `startTime` |
| `end` | — | `endTime` |
| `url` | — | `clientActivationId` (`url\|start\|end`) |
| *(reader config)* `accel` | `attributes.accel` | `attributes.accel` |

## Changes from today

- Today `note`, `config`, `poc` and `details` go on the **configuration**.
  The proposal moves them to the **activation**, because they change every
  shift: on the configuration (one record per `program_name`) they go stale
  or overwrite each other.
- `hutch_name` and `hutch_line` are also copied to the activation, so shifts
  can be filtered by location.

## Optional

- Extract the proposal number (`1020896`) from `details` (or
  `program_name`) as the configuration's `attributes.proposal`.
- Extract the "Actual start" line from `details` as the activation's
  `attributes.actual_start`.

## Compatibility

Anyone reading `note` / `config` / `poc` / `details` from configurations
today will need to read them from activations. They can be written to both
for one release to ease the transition.
