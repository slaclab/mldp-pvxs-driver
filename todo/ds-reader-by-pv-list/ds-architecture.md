# Directory Service — Architecture Overview

## Summary

SLAC Directory Service (DS) is an **EPICS V4 PVAccess RPC service** that maps accelerator names across multiple namespaces. Clients query via `eget`/pvAccess RPC using a PV named `ds` (configurable via `SERVICE_NAME` env var). Given one or more name types as input (e.g., tag, device name, IOC name), the service resolves them to a target namespace (e.g., channel names) using a chained execution plan model. Results are returned as `NTTable` PV structures.

Data comes from two sources:
1. **Redis** — live IOC PV lists (`IOC.pvlists`, `Services.pvlists` keys) polled every 60 s
2. **Filesystem** — MAD deck export `.dat` files (accelerator lattice/element data) and user-defined tag files, polled every 5 min

---

## Request Flow

```
Client (eget / pvAccess RPC)
        │
        │  PVStructure{query: {show, name/tag/dname/..., filter, sort, regex}}
        ▼
┌───────────────────────────────────┐
│  DSMain  (RPCServer, port default)│  ← Apache Commons Daemon lifecycle
│  registerService("ds", ...)       │
└────────────────┬──────────────────┘
                 │ RPCService.request(PVStructure)
                 ▼
┌───────────────────────────────────┐
│  DSServiceImpl  (RPCService)      │
│                                   │
│  1. Parse "command" field?        │──► executeCommand()  (reload / help)
│  2. Parse "show" field → destType │
│     default: CHANNEL_NAME         │
│  3. Parse input args per DSNameType│
│     (tag, dname, ename, lname,    │
│      ioc, name/channelname, regex)│
│  4. executePlans()                │
│     a. getMatchingNames()         │──► ExecutionPlans.hop(src→dest, names)
│     b. applyPatternOnFinalResults │──► regex filter OR external :filter PV
│     c. sortResultsAccordingly()   │──► Z-position sort (optional)
│  5. convertResults() → NTTable    │
└────────────────┬──────────────────┘
                 │
                 ▼
┌───────────────────────────────────────────────────────────┐
│  ExecutionPlans  (static registry: src×dest → plan chain) │
│                                                           │
│  hop(srcType, destType, names, dsContainer)               │
│    → looks up List<ExecutionPlan> chain                   │
│    → executes each hop in sequence,                       │
│      output of step N = input to step N+1                 │
│                                                           │
│  Missing pairs → empty set (no error, no warning)        │
└────────────────┬──────────────────────────────────────────┘
                 │ reads in-memory indexes
                 ▼
┌───────────────────────────────────────────────────────────┐
│  DSContainer  (wires all components, owns executor)       │
│                                                           │
│  ┌─────────────┐  ┌──────────────┐  ┌─────────────────┐  │
│  │  PVNames    │  │ IOC2PVNames  │  │  TagsFromDeck   │  │
│  │             │  │              │  │                 │  │
│  │ ConcurrentS │  │ IOC path →   │  │ MAD deck .dat   │  │
│  │ kipListSet  │  │ Set<PVName>  │  │ + user tags     │  │
│  │ + trie idx  │  │              │  │ tag→device map  │  │
│  └──────┬──────┘  └──────┬───────┘  └────────┬────────┘  │
│         │                │                    │            │
│  ┌──────▼────────────────▼────────────────────▼────────┐  │
│  │  Scheduled Executor (single-thread)                 │  │
│  │  IOCPVListWatcher  every 60 s  ──► Redis poll       │  │
│  │  TagsFromDeck      every 5 min ──► FS poll          │  │
│  └─────────────────────────────────────────────────────┘  │
└────────────────┬──────────────────────────────────────────┘
                 │
        ┌────────┴────────┐
        ▼                 ▼
┌──────────────┐   ┌────────────────────┐
│    Redis     │   │  Filesystem (SDF)  │
│  IOC.pvlists │   │  cu_hxr_lines.dat  │
│  per-IOC     │   │  sc_*_lines.dat    │
│  pvNames +   │   │  user tag files    │
│  lastModified│   └────────────────────┘
└──────────────┘
```

---

## Name Types (`DSNameType`)

| Enum | Query arg(s) | Description |
|------|-------------|-------------|
| `CHANNEL_NAME` | `name`, `channelname` | EPICS PV/channel name |
| `TAG` | `tag` | Logical tag from MAD deck or user-defined |
| `DEVICE_NAME` | `dname`, `devicename` | Accelerator device name |
| `ELEMENT_NAME` | `ename`, `elementname` | MAD lattice element name |
| `ELEMENT_TYPE` | `elementtype`, `etype` | Lattice element type |
| `LINE_NAME` | `lname`, `linename` | Beam line name |
| `IOC_NAME` | `ioc` | IOC name |
| `Z` | `z` | Longitudinal position along beamline |
| `IOC_SCHEME` | `scheme` | CA or PVA protocol scheme |

---

## Key Components

| Class | Role |
|-------|------|
| `DSMain` | Entry point; Apache Daemon lifecycle; owns `RPCServer` |
| `DSServiceImpl` | RPC handler; parses query args; drives execution plans; builds response |
| `DSContainer` | IoC container; wires components; owns scheduled executor |
| `ExecutionPlans` | Static registry mapping `(src,dest)` → ordered `ExecutionPlan` chain |
| `ExecutionPlan` | Interface: `hop(Set<String> names, DSContainer) → Set<String>` |
| `PVNames` | Thread-safe global PV name list with colon-split trie index |
| `IOC2PVNames` | Maps IOC pvlist path → owned PV names; handles add/remove diffs |
| `IOCPVListWatcher` | Polls Redis for `IOC.pvlists` / `Services.pvlists`; triggers `IOC2PVNames` updates |
| `TagsFromDeck` | Loads MAD deck `.dat` files + user tag files; builds tag→device index |
| `JedisExternalState` | Redis client (Jedis pool); reads `REDIS_URL` env var |

---

## Execution Plans

### Interface

```java
Set<String> hop(Set<String> inputNames, DSContainer container);
```

Each implementation reads one in-memory index inside `DSContainer` and translates names from one namespace to another:

| Class | Translates |
|-------|-----------|
| `ChannelNames2ChannelNames` | channel regex → matching channel names (trie search) |
| `ChannelNames2DeviceNames` | channel names → device names |
| `DeviceNames2ElementNames` | device names → element names |
| `ElementNames2Z` | element names → Z positions |
| `Tags2ChannelNames` | tag names → channel names |
| `IOCNames2ChannelNames` | IOC names → channel names |

### Plan Selection

`ExecutionPlans` holds a static `HashMap<(srcType, destType), List<Class<? extends ExecutionPlan>>>` built at class-load time.

When DS receives a request:
1. `DSServiceImpl` parses query args → determines `srcType` per input field (`name=` → `CHANNEL_NAME`, `tag=` → `TAG`, etc.)
2. Reads `show=` → determines `destType` (default: `CHANNEL_NAME`)
3. For each input arg calls `ExecutionPlans.hop(srcType, destType, names, container)`
4. `hop()` does `executionPlanRegistry.get(new Name2NameDefinition(srcType, destType))`
5. **No entry** → returns empty `HashSet` immediately (no error, no client warning)
6. **Entry found** → instantiates each class via `action.getConstructor().newInstance()` and runs in sequence

### Chain Execution

```
srcNames
  └──► plan[0].hop(srcNames,        container) → intermediate0
  └──► plan[1].hop(intermediate0,   container) → intermediate1
       ...
  └──► plan[N].hop(intermediateN-1, container) → finalSet
```

Output of each step is input to the next. If any step returns empty set, all subsequent steps also return empty.

### Plan Registry

```
(src, dest)               Plan chain
──────────────────────────────────────────────────────────────────────────────
CH   → CH                 ChannelNames2ChannelNames
CH   → DEVICE             CH2CH → ChannelNames2DeviceNames
CH   → ELEMENT            CH2CH → CH2Dev → DeviceNames2ElementNames
CH   → ELEM_TYPE          CH2CH → CH2Dev → Dev2Elem → ElementNames2ElementType
CH   → Z                  CH2CH → ChannelNames2Z
CH   → LINE               CH2CH → CH2Dev → DeviceNames2LineNames
CH   → IOC                CH2CH → ChannelNames2IOCNames
CH   → SCHEME             CH2CH → ChannelNames2CAOrPVAScheme
TAG  → CH                 Tags2ChannelNames
TAG  → DEVICE             Tags2DeviceNames
TAG  → ELEMENT            Tags2Dev → Dev2Elem
TAG  → ELEM_TYPE          Tags2Dev → Dev2Elem → Elem2Type
TAG  → Z                  Tags2Dev → Dev2Elem → ElementNames2Z
TAG  → LINE               Tags2Dev → DeviceNames2LineNames
TAG  → IOC                Tags2CH → CH2IOC
DEV  → CH                 DeviceNames2DeviceNames → Dev2ChannelNames
DEV  → ELEMENT            Dev2Dev → Dev2Elem
DEV  → ELEM_TYPE          Dev2Dev → Dev2Elem → Elem2Type
DEV  → Z                  Dev2Dev → Dev2Elem → Elem2Z
DEV  → LINE               Dev2Dev → DeviceNames2LineNames
DEV  → IOC                Dev2Dev → Dev2CH → CH2IOC
ELEM → DEVICE             ElementNames2DeviceNames
ELEM → ELEM_TYPE          ElementNames2ElementType
ELEM → Z                  ElementNames2Z
ELEM → LINE               Elem2Dev → DeviceNames2LineNames
ETYPE→ ELEMENT            ElementTypes2ElementNames
ETYPE→ DEVICE             EType2Elem → Elem2Dev
ETYPE→ Z                  EType2Elem → Elem2Z
LINE → DEVICE             LineNames2DeviceNames
LINE → ELEMENT            Line2Dev → Dev2Elem
LINE → ELEM_TYPE          Line2Dev → Dev2Elem → Elem2Type
LINE → Z                  Line2Dev → Dev2Elem → Elem2Z
IOC  → CH                 IOCNames2ChannelNames
──────────────────────────────────────────────────────────────────────────────
Any unregistered pair     → empty set, no error
```

### Multiple Input Args → Set Intersection

When the query has multiple input fields (e.g. `name=XCOR:*` AND `tag=QUAD`), `getMatchingNames()` runs `hop()` for each independently then intersects:

```java
// First arg seeds the accumulator:
matchingNames.addAll(hop(firstArg.nameType, destType, firstNames, container));

// Each subsequent arg narrows via retainAll:
matchingNames.retainAll(hop(nextArg.nameType, destType, nextNames, container));
```

Order affects performance (most-selective arg first); correctness is order-independent.

### Optimisation: `destType == CHANNEL_NAME` with `name=` pattern

When output is `CHANNEL_NAME` AND a `name=` pattern is present alongside other input args (e.g. `tag=QUAD&name=XCOR:.*`):

1. Resolve non-channel args (tags, devices, etc.) into a candidate channel set via `hop()`
2. Apply the `name=` regex directly on that (usually small) candidate set

Avoids a full `ChannelNames2ChannelNames` trie search over 100k+ PVs followed by `retainAll`.

---

## Data Sources

### Redis
- Key `IOC.pvlists` → JSON array of IOC pvlist file paths
- Key `Services.pvlists` → JSON array of service pvlist file paths
- Key `<path>` → hash with fields `pvNames` (JSON array) and `lastModifiedTime`

### MAD Deck (filesystem)
Default path: `/sdf/data/ad/accel/prod/lcls/` (override: `DS_MAD_EXPORT_FOLDER`)
Files: `cu_hxr_lines.dat`, `cu_sxr_lines.dat`, `sc_*_lines.dat`
Contains: element names, types, Z positions, line membership, device names

### User Tags (filesystem)
Path: `DS_USER_DEFINED_TAGS_FOLDER` env var
Format: flat files mapping tag names to device names

---

## Configuration

| Variable | Default | Purpose |
|----------|---------|---------|
| `SERVICE_NAME` | `ds` | PVAccess RPC service name |
| `REDIS_URL` | — | Redis host:port |
| `DS_IOC_PVLIST_CONTENTS_POLLING_PERIOD_IN_SECONDS` | `60` | IOC list poll interval |
| `DS_TAG_CONTENTS_POLLING_PERIOD_IN_SECONDS` | `300` | MAD/tag poll interval |
| `DS_MAD_EXPORT_FOLDER` | `/sdf/data/ad/accel/prod/lcls/` | MAD deck files location |
| `DS_USER_DEFINED_TAGS_FOLDER` | — | User tag files location |
| `DS_FILTER_SERVICES` | — | Comma-separated external filter service names |
| `helpFolder` | `.` | Location of `help.txt` |

---

## RPC Call Structure (`runWorker()`)

`EpicsDSMetadataReader::runWorker()` builds an NTURI struct and issues a single pvAccess RPC.

```cpp
// struct when hasShow == true:
TypeDef(TypeCode::Struct, "epics:nt/NTURI:1.0", {
    Member(TypeCode::String, "scheme"),
    Member(TypeCode::String, "path"),
    Member(TypeCode::Struct, "query", {
        Member(TypeCode::String, "name"),
        Member(TypeCode::String, "show"),   // ← present only when showColumns non-empty
    }),
})

arg["scheme"]     = "pva";
arg["path"]       = config_.service();      // e.g. "ds"
arg["query.name"] = config_.query();        // e.g. "XCOR:.*"
arg["query.show"] = config_.showColumns();  // e.g. "dname"
```

DS maps this to:
- `query.name` → input namespace = `CHANNEL_NAME`, matched as regex (after `%`→`.*` substitution)
- `query.show` → output namespace (`DSNameType`); defaults to `CHANNEL_NAME` when absent
- Returns `NTTable` with one `value.name` string array

### Available `show` values

| `config_.showColumns()` | Returns |
|------------------------|---------|
| `""` (empty/absent) | channel names (EPICS PV names) — default |
| `"dname"` | accelerator device names |
| `"ename"` | MAD lattice element names |
| `"etype"` | element type strings |
| `"lname"` | beam line names |
| `"ioc"` | IOC names |
| `"scheme"` | `ca` or `pva` |
| `"z"` | longitudinal Z position (float as string) |

### Pattern syntax in `config_.query()`

| Input | Transformed to | Behaviour |
|-------|---------------|-----------|
| `XCOR:%` | `XCOR:.*` | `%`→`.*`; bash-safe wildcard |
| `XCOR:.*` | `XCOR:.*` | already valid regex, unchanged |
| `XCOR:*,YCOR:*` | `["XCOR:.*","YCOR:.*"]` | `,` → OR split; results unioned |

---

## Fundamental Limitation: One Column Per RPC Call

DS has **no multi-column mode**. One call = one namespace. No `show=all`.

```
query.name="XCOR:.*"  (no show)   →  ["XCOR:LI21:201:BACT", ...]  ← channel names
query.name="XCOR:.*"  show=dname  →  ["XCOR:LI21:201", ...]        ← device names
query.name="XCOR:.*"  show=ename  →  ["XC21201", ...]              ← element names
```

### Result sets across calls are NOT positionally aligned

DS runs the full execution plan **independently** for each call. Result size and ordering differ even with the same `query.name`. **Row N from `show=dname` does NOT correspond to row N from `show=ename`.** Never zip columns by position.

`show=dname` runs `CH2CH→CH2Dev`; `show=ename` runs `CH2CH→CH2Dev→Dev2Elem`. Coverage gaps in `Dev2Elem` produce different-sized output than `CH2Dev`.

---

## Getting All Attributes Per PV

To collect all attributes, run `runWorker()` N times with different `showColumns` then join by channel name.

### Step 1 — get canonical channel names (join key)

```cpp
// config_.showColumns() = ""   (empty → no show field sent)
// config_.query()       = "XCOR:.*"
// result: ["XCOR:LI21:201:BACT", "XCOR:LI21:203:BACT", ...]
```

### Step 2 — one call per attribute column

```cpp
// config_.showColumns() = "dname"   → device names
// config_.showColumns() = "ename"   → element names
// config_.showColumns() = "etype"   → element types
// config_.showColumns() = "lname"   → line names
// config_.showColumns() = "ioc"     → IOC names
// config_.showColumns() = "scheme"  → ca/pva
// config_.showColumns() = "z"       → Z positions (float strings)
```

### Step 3 — join strategies

Attribute columns return destination-namespace names only — no corresponding channel name column in the NTTable.

#### Option A — per-channel exact queries (correct, O(N × M))

For each channel from Step 1, repeat with the exact channel name as `query.name`:

```cpp
// config_.query()       = "XCOR:LI21:201:BACT"  // exact — no wildcards
// config_.showColumns() = "dname"               // → ["XCOR:LI21:201"] (1 result)
// config_.showColumns() = "ioc"                 // → ["SIOC:SYS0:AL00"] (1 result)
```

Exact 1:1 join guaranteed. Viable for small sets (< 200 channels).

#### Option B — reverse lookup (correct, O(M + N_unique_attr))

Use each attribute result as input to query back to channel names. DS accepts `dname=`, `ename=`, `ioc=` etc. as input namespaces:

```cpp
// After bulk dname result: ["XCOR:LI21:201", "XCOR:LI21:203", ...]
TypeDef queryDef = TypeDef(TypeCode::Struct, "epics:nt/NTURI:1.0", {
    Member(TypeCode::String, "scheme"),
    Member(TypeCode::String, "path"),
    Member(TypeCode::Struct, "query", {
        Member(TypeCode::String, "dname"),  // ← input namespace = DEVICE_NAME
        // no "show" → output = CHANNEL_NAME
    }),
});
arg["query.dname"] = "XCOR:LI21:201";
// → returns ["XCOR:LI21:201:BACT", ...] — channel names owned by that device
```

Cost: O(N_unique_devices) calls — usually much less than N_channels. Best for large populations.

#### Option C — positional alignment (fragile, O(M))

When the same pattern produces identical sets in every call, row positions align. Holds only when all intermediate maps have 100% coverage. **Any missing mapping silently breaks alignment.** Do not use in production without verified coverage.

### Summary

| Option | RPC calls | Correctness | Use when |
|--------|-----------|-------------|----------|
| A: per-channel exact | O(N×M) | Exact | N < 200, exact join required |
| B: reverse lookup | O(M + N_attr) | Exact | N large, performance matters |
| C: positional | O(M) | Fragile | 100% map coverage guaranteed |

### `sort=z`

Add `query.sort = "z"` to re-sort by MAD deck Z position. Requires `Member(TypeCode::String, "sort")` in the NTURI TypeDef and `arg["query.sort"] = "z"`.

| `show=` | `sort=z` result |
|---------|----------------|
| *(omit)* | sorted by channel Z |
| `"dname"` | sorted by device Z |
| `"ename"` | sorted by element Z |
| **any other** | **empty set silently** — not an error |

---

## Filter Mechanics

The `query.filter` field supports two behaviours. DS checks whether the value **exactly matches** (case-sensitive, full string) a name in `DS_FILTER_SERVICES` first; otherwise treats it as a regex.

Current `runWorker()` does **not** send `filter`. To enable, add `Member(TypeCode::String, "filter")` to the `query` TypeDef and set `arg["query.filter"] = config_.filter()`.

### Regex filter (default)

Applied **after** `ExecutionPlans.hop()` resolves all candidate names. Supports same `%`→`.*` and `,`-OR processing as `query.name`.

```
query.name="XCOR:.*"  →  hop()  →  candidateSet (e.g. 500 device names if show=dname)
                                          │
                                   filter="XCOR:LI21:.*"
                                   isFilterService("XCOR:LI21:.*") == false
                                          │
                                   Pattern.compile("XCOR:LI21:.*")
                                   .matcher(name).matches()  ← runs against candidateSet
                                          │
                                     filteredSet → NTTable
```

- Filter runs on the **output namespace** — if `show=dname`, regex matches device names, not channel names
- Multiple `,`-separated patterns → OR (union, not AND)
- `%`→`.*` substitution applies to `filter=` same as `name=`

### External service filter

Triggered when `filter=<name>` exactly matches (case-sensitive) an entry in `DS_FILTER_SERVICES`. DS calls `<name>:filter` via pvAccess RPC:

```
query.name="XCOR:.*"  →  hop()  →  candidateSet
                                          │
                                   filter="hist"
                                   isFilterService("hist") == true
                                          │
                         RPCClientFactory.create("hist:filter")
                         ┌──────────────────────────────────────┐
                         │ send:  PVStructure{                   │
                         │          names: String[]              │
                         │          (= full candidateSet)        │
                         │        }                              │
                         │ recv:  PVStructure{                   │
                         │          value: String[]              │
                         │          (subset service knows)       │
                         │        }                              │
                         └──────────────────────────────────────┘
                                          │
                         intersection(candidateSet, serviceResult)
                                          │
                                     filteredSet → NTTable
```

DS sends the full candidate set; external service returns the subset it recognises. DS returns the intersection.

**Timeout:** default 30 s server-side. Override: add `Member(TypeCode::String, "timeout")` to `query` TypeDef and set `arg["query.timeout"] = "60.0"`.

**External service contract:**
- PV name: `<filterName>:filter`
- Input:  `PVStructure{ names: String[] }`
- Output: `PVStructure{ value: String[] }`

Partial match or wrong case in `filter` value → treated as regex silently, no error.

Both filter types work with any `show=` value — filtering happens on resolved output-namespace names after `hop()` completes.

---

## Hot Reload

Send `eget -s ds -a command=reload` → `DSServiceImpl.executeCommand("reload")`:
1. Build brand new `DSContainer` (re-reads all data sources from scratch)
2. Atomically swap reference in `DSMain`
3. Shutdown old container (stops its scheduled executor)

No downtime — in-flight requests complete against old container; new requests use new container after swap.
