# Directory Service — Architecture

## What problem does it solve?

The SLAC accelerator control system describes the same physical machine
through several different naming systems, each natural to a different
group of people.  Operators and EPICS infrastructure see **PV names**
like `BPMS:LI21:201:X`.  Hardware engineers see **IOCs** that serve those
PVs.  Accelerator physicists see **devices**, **elements**, and
**beamlines** as defined by the MAD deck.  Application authors invent
their own **tags** to group whatever they care about — BSA channels,
PBLM channels, machine modes, and so on.

Each of these vocabularies is correct, and each is incomplete on its
own.  The Directory Service exists to translate between them.  If you
hand it any name in any vocabulary, it can give you back the
corresponding names in the others, and it can do so with set-style
operations: *"give me every BPM X-channel in BSY that is also being
archived,"* *"give me every PV in IOC `SIOC:SYS0:ML00` that maps to a
quadrupole element,"* *"give me every device on linac line `LI21`
sorted by position along the beamline."*

A useful mental model is that DS is the **librarian** of the control
system.  It does not own the books — the IOCs, the MAD deck, the tag
files, and the various inventory crawlers do.  But it knows where each
book lives and how the catalogues cross-reference one another, and it
will hand you exactly the slice of the catalogue you ask for.

## How DS fits with the rest of the system

The catalogue itself is built by a separate daemon called **ARBO**.
ARBO walks the IOC data folders, reads `IOC.pvlist` files and
V4-service inventory files, and writes the resulting PV/IOC inventory
into Redis.  Redis acts as the shared blackboard between ARBO and DS:
ARBO is the only writer, DS is the only reader.  This split is
deliberate — it lets the discovery side scale and evolve
independently of the query side, and it lets DS restart without
losing any inventory.

DS does not, however, get *everything* from Redis.  Two important data
sources are read directly from disk by DS itself:

1. The **MAD-deck export files** (`*_lines.dat`) produced by the
   accelerator-modelling group.  These give DS its understanding of
   accelerator physics: which devices drive which elements, what type
   each element is, what longitudinal position it occupies, and which
   beamline (`LI21`, `BSY`, `LCLS2cuH`, …) it belongs to.
2. The **user-defined tag files** (`*.tag`).  These are plain-text
   files maintained by accelerator experts and operators.  Each file
   defines a single named tag and lists the PV names, device names, or
   regex patterns that belong to it.

In other words, ARBO answers *"what PVs exist?"* while DS additionally
answers *"what do those PVs mean?"*  The first question is mechanical;
the second is human.

DS finally has one more trick: it can call **outbound** to other PVA
services to refine its results.  If you ask DS for *"all `:BDES` PVs
that are currently being archived,"* DS resolves the names locally and
then makes an RPC call to the archiver's `hist:filter` PV, hands it the
candidate list, and returns the intersection.  This pattern lets DS
participate in queries that depend on data it does not own without
having to embed that domain knowledge.

## Anatomy of the daemon

DS is a single Java process.  It is normally started by `runDS.sh`,
which selects per-facility environment variables (LCLS, FACET,
TESTFAC, DEV, CODING) and then launches `edu.stanford.slac.simpleDS.DSMain`
under Apache Commons Daemon (`jsvc`) in production, or directly under
`java -jar` for local development.  The daemon registers exactly one
PVA RPC service — by default the service PV is named `ds`, although
the name is overridable via the `SERVICE_NAME` environment variable.

Inside the daemon, three layers cooperate:

* **`DSMain`** is the lifecycle skeleton.  It implements the
  `Daemon` interface (`init`, `start`, `stop`, `destroy`) so jsvc can
  manage it, and it owns a JVM shutdown hook so a plain `kill -INT`
  also stops cleanly.  Its job is to construct the container, hand
  it to the RPC server, and stay out of the way.

* **`DSContainer`** is the in-memory state.  It owns the Redis
  connection pool, the in-memory PV index, the IOC-to-PV mapping, the
  MAD-deck/tag data, and the scheduled executor that keeps everything
  fresh.  Every component reaches the others through the container,
  not directly, so the entire container can be rebuilt and swapped in
  atomically when the operator issues a `command=reload`.  Think of
  the container as a snapshot of the world at one moment in time;
  rebuilding it is how DS recovers from corrupted state without
  taking the daemon down.

* **`DSServiceImpl`** is the request handler.  Each PVA RPC arrives
  here, gets parsed, dispatched through the execution-plan engine,
  filtered, sorted, and packaged as a normative-type response.

These three layers map roughly onto the classic *router → state →
controller* split — `DSMain` brings the daemon up and accepts
connections, `DSContainer` is the stateful core, `DSServiceImpl`
implements the policy that turns a query into a response.

## Where the data lives, and how fresh it stays

Two scheduled tasks run in the background and keep the in-memory
state synchronised with the outside world.  Both are driven by a
single-threaded scheduled executor inside the container, which means
their work is serialized; if one is busy the other waits.  This is
adequate for current workloads but is worth knowing about — a long
MAD-deck reload will briefly delay an inventory refresh, and vice
versa.

The first task is **`IOCPVListWatcher`**.  Once a minute (by default),
it asks Redis for the list of `IOC.pvlist` paths and `*.xdb` paths
that ARBO currently knows about, compares that list against the paths
DS has already loaded, and reconciles the differences.  New paths
trigger a fresh load of their PV names.  Removed paths trigger a
deregister, which removes the contributing PVs from the global index
*unless* another IOC is still serving the same names.  This last
detail matters — multiple IOCs can legitimately serve the same PV
during commissioning or failover, and the watcher uses set-difference
across all known IOCs to avoid over-deleting.  For unchanged paths it
checks the per-path `lastModifiedTime` field in Redis and only re-reads
if ARBO has marked the file as updated.

The second task is **`TagsFromDeck`**.  By default it runs every five
minutes.  It checks the modification times of the MAD-deck files and
the `.tag` files, and reloads only when something has actually
changed.  Reloads happen into a *temporary* `TagsFromDeckData` object
that is then assigned over the live one in a single field-assignment
step — readers either see the old snapshot or the new one, never a
half-built one.  This is the simplest possible form of MVCC and it is
sufficient because tag data is read-mostly.

Together these two watchers give DS a self-healing property: if Redis
goes away briefly, DS keeps serving from cache and resumes consuming
from Redis when it returns; if a tag file is accidentally edited and
the operator wants to re-pull immediately rather than wait for the
next polling tick, they can issue `command=reload` over RPC and the
container will rebuild itself end-to-end.

## What happens during a query

A user types something like:

```
pvcall ds query.lname=BSY query.name=BPMS:%:X query.show=ename query.sort=z
```

This becomes a `PVStructure` with a `query` sub-structure containing
the named arguments.  The lifetime of that structure inside DS looks
like this in plain words:

1. **Special commands first.**  If the request carries `command=` or
   `help`, that is handled immediately and the rest of the pipeline is
   skipped.  `command=reload` rebuilds the container; `command=help`
   reads `docs/help.txt` from disk and returns it as a string.  This
   is how operators recover from inconsistent state and how new users
   discover the query language.

2. **The destination type is chosen.**  The `show=` argument tells DS
   what *kind* of name the user wants back.  In our example,
   `show=ename` means *"give me element names."*  When `show=` is
   absent, the destination defaults to channel names — the most
   common case.

3. **Each source predicate is parsed.**  Every argument other than
   `show`, `sort`, `filter`, `timeout`, `command`, and `help` is a
   *source predicate*: it constrains the result by a different
   vocabulary.  In our example, `lname=BSY` is a line-name predicate
   and `name=BPMS:%:X` is a channel-name predicate.  Each predicate
   becomes a `(NameType, value)` pair.  Comma-separated values within
   one predicate are turned into alternatives (a logical OR);
   `name=` arguments have `%` rewritten to `.*` for shell-friendly
   syntax, while `regex=` is passed through verbatim for cases where
   the regex itself contains commas or percent signs.

4. **Each predicate is hopped to the destination type.**  This is the
   heart of the system.  For every supported `(srcType, destType)` pair,
   DS has a registered chain of one or more *execution plans* — small
   classes that each implement a single hop.  For example, going from
   line-name to element-name is implemented as
   `LineNames2DeviceNames → DeviceNames2ElementNames`: first translate
   the line into the devices that belong to it, then translate those
   devices into their elements.  The dispatcher
   (`ExecutionPlans.hop`) instantiates each plan reflectively and
   threads the output of one as the input of the next.

5. **The per-predicate result sets are intersected.**  If you ask
   *"give me everything that is on line `BSY` AND matches `BPMS:%:X`"*,
   DS computes the BSY elements and the BPMS-X elements separately,
   then keeps only the elements present in both.  Set-intersection is
   the default combinator across predicates; this is how DS implements
   AND semantics without a SQL-style join planner.

6. **There is a small optimisation for channel-name destinations.**
   If the destination is `CHANNEL_NAME` and the user supplied a
   `name=`/`regex=` predicate alongside other predicates, DS does not
   include the channel-name predicate in the intersection.  Instead,
   it computes the intersection of the *other* predicates first, then
   applies the channel-name regex as a *filter* over the survivors.
   This is dramatically faster than expanding the regex against the
   whole PV namespace and then intersecting — there are usually
   millions of PVs but only thousands of devices.

7. **`filter=` is applied last.**  The filter is comma-separated.
   Each value is either treated as a regex applied locally to the
   final result set, or — if the value matches a name in the
   `DS_FILTER_SERVICES` allow-list — DS opens an outbound PVA RPC to
   `<value>:filter`, ships it the candidate names, and uses the
   response as the new working set.  This is how DS composes with
   the archiver, save-restore, and similar services.  The default
   timeout is 30 seconds, overridable via `query.timeout`.

