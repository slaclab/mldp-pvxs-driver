# Plan: C++ EPICS PV Metadata Client

## Goal

CLI tool: query the Directory Service (`ds`) with a wildcard pattern, receive all matching PVs, and print full metadata for each PV as a structured block. Single RPC call. No live PV get. No monitoring.

---

## Input

| Mode | Flag | Example | Query sent to service |
|---|---|---|---|
| Scan all PVs | *(no arg)* | *(none)* | `name=%` |
| Device prefix | `-d` | `VPIO:IN20:111` | `name=VPIO:IN20:111:%` |
| PV name (exact) | *(default)* | `VPIO:IN20:111:PRES` | `name=VPIO:IN20:111:PRES` |

| Option | Flag | Default | Description |
|---|---|---|---|
| Service name | `-s` | `ds` | pvAccess channel name of the directory service |

---

## Flow

```
pvmetadata [-w timeout] [-s service] [-d] [<name>]
        |
        v
If no <name>: query = "%"  (all PVs)
If -d <name>: query = <name> + "%"
Else:         query = <name>
        |
        v
Build NTURI: scheme=pva, path=<service>, query.name=<query>
        |
        v
Single RPC call to <service> channel via pvAccess ChannelRPC
        |
        v
Receive NTTable response
        |
        v
For each row in NTTable:
  print PV name as header
  print each metadata field: label = value
        |
        v
Print total count
        |
        v
Exit 0 (or 1 on timeout/error)
```

---

## Output (per-PV block format)

```
Querying ds for: VPIO:IN20:111:%

PV: VPIO:IN20:111:PRES
  hostName  : cpu-li20-vac1
  iocName   : ioc-li20-vac1
  owner     : vacuum
  archived  : true
  tags      : fast,survey

PV: VPIO:IN20:111:STATUS
  hostName  : cpu-li20-vac1
  iocName   : ioc-li20-vac1
  owner     : vacuum
  archived  : false
  tags      : survey

2 PV(s) found
```

All fields dynamic — no hardcoded column list. Whatever `ds` returns gets printed per PV.

---

## pvAccess / pvData API (pvAccessCPP + pvDataCPP)

These are the exact headers and types needed. The codebase uses `pvAccessCPP` / `pvDataCPP` (pvAccess v7, **not** pvxs). Use these.

### Headers

```cpp
#include <pv/pvAccess.h>       // ChannelProvider, Channel, ChannelRPC, ChannelRPCRequester
#include <pv/pvData.h>         // PVStructure, PVString, PVStringArray, PVScalarArray, getFieldCreate, getPVDataCreate
#include <pv/lock.h>           // Mutex, Lock
#include <pv/event.h>          // Event (binary semaphore)
#include <epicsStdlib.h>
#include <epicsGetopt.h>
#include <epicsThread.h>
#include <epicsExit.h>
```

Namespace aliases used in eget.cpp and throughout this codebase:
```cpp
namespace TR1 = std::tr1;
using namespace epics::pvData;
using namespace epics::pvAccess;
```

---

## RPC call lifecycle (pvAccess pattern)

### 1. Start provider

```cpp
ClientFactory::start();
ChannelProvider::shared_pointer provider =
    ChannelProviderRegistry::clients()->getProvider("pva");
```

### 2. Create channel

```cpp
Channel::shared_pointer channel =
    provider->createChannel(serviceName,          // serviceName = argv[-s], default "ds"
                            DefaultChannelRequester::build(),
                            ChannelProvider::PRIORITY_DEFAULT);
```

### 3. Create RPC handle

`pvRequest` is an empty request structure — just signals no field filtering:
```cpp
PVStructure::shared_pointer pvRequest =
    CreateRequest::create()->createRequest("");

TR1::shared_ptr<RpcRequester> req(new RpcRequester(serviceName));
ChannelRPC::shared_pointer rpc = channel->createChannelRPC(req, pvRequest);
```

### 4. Wait for RPC connection, send request

`waitConnect()` blocks on `m_connectionEvent`; `waitDone()` blocks on `m_event`:
```cpp
if (req->waitConnect(timeout)) {
    rpc->lastRequest();      // tells pvAccess this is the only request
    rpc->request(uri);       // uri = the NTURI PVStructure built below
    req->waitDone(timeout);
}
```

### 5. Destroy and stop

```cpp
channel->destroy();
epicsThreadSleep(0.1);   // flush async teardown
ClientFactory::stop();
```

---

## Building the NTURI request argument

The directory service expects an `epics:nt/NTURI:1.0` structure with a single query field `name`:

```
epics:nt/NTURI:1.0
  string scheme  = "pva"
  string path    = <serviceName>   // -s flag, default "ds"
  structure query
    string name  = <queryName>     // e.g. "VPIO:IN20:111:%" or "%"
```

Construction using pvDataCPP field factory:

```cpp
// query sub-structure: { string name }
StringArray qnames;   qnames.push_back("name");
FieldConstPtrArray qfields;
qfields.push_back(getFieldCreate()->createScalar(pvString));
Structure::const_shared_ptr qstruct =
    getFieldCreate()->createStructure(qnames, qfields);

// top-level NTURI: { string scheme, string path, query }
StringArray unames;
unames.push_back("scheme");
unames.push_back("path");
unames.push_back("query");
FieldConstPtrArray ufields;
ufields.push_back(getFieldCreate()->createScalar(pvString));
ufields.push_back(getFieldCreate()->createScalar(pvString));
ufields.push_back(qstruct);
Structure::const_shared_ptr ustruct =
    getFieldCreate()->createStructure("epics:nt/NTURI:1.0", unames, ufields);

// instantiate and fill
PVStructure::shared_pointer uri = getPVDataCreate()->createPVStructure(ustruct);
uri->getSubField<PVString>("scheme")->put("pva");
uri->getSubField<PVString>("path")->put(serviceName);   // serviceName = argv[-s], default "ds"
uri->getSubField<PVStructure>("query")->getSubField<PVString>("name")->put(queryName);
```

Pass the **full NTURI** (`uri`) — not just the query sub-structure — to `rpc->request()`.

---

## Parsing the NTTable response

`ds` returns `epics:nt/NTTable:1.0`:

```
epics:nt/NTTable:1.0
  string[]  labels          // column display names, same order as value subfields
  structure value
    string[] channelName    // (or first column) — one entry per PV row
    string[] hostName
    string[] iocName
    string[] owner
    ...                     // any additional columns ds returns
```

**Exact parsing idiom** (mirrors `formatNTTable` in `eget.cpp:323`):

```cpp
// 1. Get labels array
PVStringArrayPtr labelsField = result->getSubField<PVStringArray>("labels");
PVStringArray::const_svector labels;
labelsField->get(labels);                 // shared_vector<const string>

// 2. Get value structure; iterate its sub-fields in declaration order
PVStructurePtr valueStruct = result->getSubField<PVStructure>("value");
PVFieldPtrArray fields = valueStruct->getPVFields();  // one PVScalarArray per column

// fields[i] corresponds to labels[i]
// Number of rows = fields[0]->getLength()

// 3. Cast each column to PVScalarArray
PVScalarArrayPtr col = TR1::dynamic_pointer_cast<PVScalarArray>(fields[i]);

// 4a. For string columns cast further:
PVStringArrayPtr scol = TR1::dynamic_pointer_cast<PVStringArray>(col);
PVStringArray::const_svector sv;
scol->get(sv);           // sv[r] = string value at row r

// 4b. For non-string columns use dumpValue():
ostringstream oss;
col->dumpValue(oss, r);  // appends text representation of element r
```

**Identify the PV name column**: check `labels[i]` for `"channelName"` first; fall back to column 0 if not found.

**Per-PV print loop**:
```
nrows = fields[0]->getLength()
for r in 0..nrows-1:
    print "PV: " + pvNameCol[r]
    for each i where labels[i] != pvNameLabel:
        print "  " + labels[i] + " : " + cols[i][r]
    print blank line
print nrows + " PV(s) found"
```

---

## Requester class (pvAccess callback pattern)

Implement `ChannelRPCRequester`. Two `Event` semaphores gate connection and response.

Key callbacks and their contract:

| Callback | When fired | What to do |
|---|---|---|
| `channelRPCConnect(status, rpc)` | pvAccess connected RPC sub-channel | set `m_connected = status.isSuccess()`, signal `m_connEvent` |
| `requestDone(status, rpc, response)` | server replied | if success: store `m_response`, set `m_done=true`; always signal `m_doneEvent` |

`waitConnect(timeout)` calls `m_connEvent.wait(timeout)` then returns `m_connected`.  
`waitDone(timeout)` calls `m_doneEvent.wait(timeout)` then returns `m_done`.

---

## Files

- `src/pvmetadata.cpp` — binary (already exists, needs `printNTTable` → `printPerPV` refactor)
- `src/Makefile` — `PROD_HOST += pvmetadata` (already added)

---

## Steps Remaining

- [ ] Replace `printNTTable()` with `printPerPV()`: per-PV block output using `getPVFields()` + `dumpValue()`
- [ ] Handle no-arg case: when `optind >= argc`, use `queryName = "%"` instead of printing usage and exiting
- [ ] Identify PV name column: scan `labels` for `"channelName"`, fall back to index 0

---

## Dependencies

- `pvAccessCPP` — `ChannelProvider`, `Channel`, `ChannelRPC`, `ChannelRPCRequester`, `ClientFactory`, `DefaultChannelRequester`, `CreateRequest`
- `pvDataCPP` — `PVStructure`, `PVString`, `PVStringArray`, `PVScalarArray`, `getFieldCreate()`, `getPVDataCreate()`
