# Make the Director the single decision maker for volumes and devices

* Status: proposed
* Deciders: [Philipp Storz](https://github.com/pstorz)
* Date: 2026-10-30

## Context and Problem Statement

Knowledge about volumes, drives and autochangers is split between the
Director and the Storage Daemon, and both take decisions:

* The Director returns *candidate* volumes (`FindNextVolumeForAppend()` in
  `core/src/dird/next_vol.cc`). The Storage Daemon iterates over them,
  skips volumes it considers in use and reserves one locally
  (`DirFindNextAppendableVolume()` in `core/src/stored/askdir.cc`,
  `reserve_volume()` in `core/src/stored/vol_mgr.cc`).
* The Director sends a *list* of acceptable devices; the Storage Daemon
  picks the actual drive (`core/src/stored/reserve.cc`).
* The Storage Daemon loads, unloads and swaps volumes between drives on its
  own (`core/src/stored/autochanger.cc`, `core/src/stored/mount.cc`). The
  catalog fields `Slot` and `InChanger` are refreshed only by `update slots`
  or as a side effect of `UpdateMedia`.
* Both daemons change `VolStatus`: the Director on expiry, pruning and
  recycling; the Storage Daemon on end of medium (`Full`), I/O errors
  (`Error`) and labeling (`Append`). It sends the status back from a cached
  copy (`dev->VolCatInfo`).
* The Storage Daemon decides to rewrite a label when it sees a `Recycle` or
  pre-labeled volume.

The two daemons use separate lock domains (`DbLocker` and
`scratch_volume_mutex` in the Director; `vol_list_lock`,
`reservation_mutex`, `changer_lock` and `mount_mutex` in the Storage
Daemon). No decision is atomic across both, which leads to races: several
jobs receive the same candidate volume, a stale cached status overwrites a
newer catalog status, the catalog slot information drifts from the real
changer, and the Director chooses volumes without knowing which drive holds
which volume.

How can volume and device handling be made consistent and free of races?

## Decision Drivers

* Remove races between the catalog state and the Storage Daemon state
* One place that implements volume, drive and changer policy
* Make volume handling testable without running daemons
* Better decisions, e.g. prefer a volume that is already loaded
* Operator commands (`mount`, `label`, `update slots`, ...) must use the
  same state and locks as jobs

## Considered Options

* Director is the single decision maker; the Storage Daemon only executes
  commands and reports events
* Storage Daemon is the single decision maker and owns its own volume
  database
* Keep the split and add more cross-daemon locking and status checks

## Decision Outcome

Chosen option: "Director is the single decision maker", because the
Director already owns the catalog and the policy (pools, retention,
recycling, scratch), so only the Storage Daemon's state needs to move. A
single lock domain removes the races.

The Director gets a *storage manager* (`core/src/dird/storage_manager.h`).
For every Storage Daemon it keeps an in-memory model of the drives, the
changer slots and the volume leases. It makes each decision, including the
matching catalog update, in one critical section. It must not hold that
lock during network I/O. Commands in flight are marked as *pending* in the
model and committed when the Storage Daemon reports their result.

The Storage Daemon becomes an executor and sensor:

* Director to Storage Daemon commands: `reserve` (one exact device), `load`,
  `unload`, `transfer`, `mount` (volume, drive, mode), `write label`,
  `release` and `inventory`.
* Storage Daemon to Director events and requests: `NeedVolume`, volume
  events (`EOM`, write/read error, label mismatch, not labeled), drive
  events (loaded, unloaded, error) and counters. These report facts only.
  The Storage Daemon never sets `VolStatus`, `Slot` or `InChanger` and never
  chooses a volume, slot or drive.
* Job attribute and JobMedia records (`UpdCat`, `BlastAttr`,
  `CreateJobMedia`) are unchanged.

A persistent control connection per Storage Daemon carries inventory,
operator commands and events that happen outside a job. Whenever the
Director starts, the Storage Daemon connects or the Storage Daemon reports
a new boot id, the Director runs `inventory` and reconciles the result with
the catalog. `update slots` becomes a forced reconcile.

The new protocol is incompatible with the old one: Director and Storage
Daemon must have the same version. Several Directors sharing one Storage
Daemon is not supported.

The migration is done in phases, each in its own pull request:

0. This ADR, a protocol version check and a concurrency stress system test.
1. Storage manager model in the Director, filled from the Storage Daemon as
   a shadow without changing behaviour. Divergences are logged.
2. The Director assigns the exact drive.
3. Volume selection and leases move to the Director. This replaces the
   candidate loop and the decision logic in `vol_mgr.cc`.
4. The Director makes volume status transitions.
5. The Director takes all autochanger load, unload and transfer decisions.
6. Labeling and relabeling decisions move to the Director.
7. The Director sequences read volumes for restore, copy, migrate and verify.
8. Remove legacy code and update the documentation.

Implemented so far (DIR-SD protocol version 7):

* The job command carries `SdProtocolVersion`; the Storage Daemon rejects
  other versions.
* The storage model is filled from the changer `listall` and the new
  `inventory` command and shown by `status storage=<name> model`.
* The Director orders the concrete devices for a reservation; the Storage
  Daemon reserves the first usable one in that order.
* The Director leases append volumes per drive, prefers the volume already
  loaded in the requesting drive and ignores stale volume status reports.
* Reading jobs lease the volume they read, so no other job selects it for
  appending on another drive meanwhile.
* The Director decides whether a recycled volume gets a new label.
* The Storage Daemon reports volume events (labeled, full, error) instead
  of a volume status, slot and `InChanger` flag. The Director derives the
  volume status from the event and keeps slot and `InChanger` itself.
* The Storage Daemon reports autochanger loads and unloads of jobs as
  drive events. The Director keeps the storage model current with them,
  sends the slot where the volume really is and corrects `Slot` and
  `InChanger` in the catalog.
* When the slots of an autochanger are unknown (Director start, operator
  commands like `label`, `import` or `export`, Storage Daemon restart detected by its epoch in the
  inventory), the Director lists the changer before reserving a device
  and corrects `Slot` and `InChanger` of all volumes of the storage in
  the catalog. It uses a short lived control connection instead of a
  persistent one.
* The `file-autochanger` system test `volume-race` runs six concurrent
  jobs in three pools on four drives with small volumes and checks that
  no volume conflicts, wrong volumes or catalog divergences occur.

* Reading jobs reserve the free drive that already holds the first
  volume of their bootstrap. The bootstrap, written by the Director,
  defines the read order of the volumes.
* The Storage Daemon no longer searches for another appendable volume
  when it autoloads; the Director chose the volume and its slot.
* The Storage Daemon still asks again with the volumes it could not
  reserve locally as `unwanted_volumes`. This is a report, the Director
  chooses the next volume and its lease. Moving a volume from an idle
  drive into the reserved one is only possible when the Director leased
  the volume to that drive.
* Every volume the Director sends carries a changer action planned from
  the storage model: `mounted` (already in the drive), `load` (from its
  slot), `transfer` (from another, idle drive), `busy` (in a drive used
  by another job) or `unknown` (slots unknown or no autochanger drive).
  The Storage Daemon takes a volume out of another drive only on
  `transfer` from exactly that drive, otherwise it waits for the
  Director.

* The console commands `mount`, `unmount` and `release` are the
  Director's load and unload commands. The Storage Daemon reports the
  loads and unloads it runs for them as drive events, so the Director
  keeps the slots of its model instead of listing the changer again.
* The Storage Daemon has no timers of its own that unload idle drives;
  it only loads and unloads for a changer action of the Director or for
  one of these console commands.

Still open: the Storage Daemon runs the changer commands that belong to
a changer action within the job instead of receiving each `load` and
`unload` as a separate command. This keeps one round trip per volume.
None of this has been tested with real tape libraries yet.

### Positive Consequences

* Each decision is atomic, which removes the races between the catalog and
  the Storage Daemon
* A single policy implementation that can be unit tested
* The Director knows the drive contents and can avoid unnecessary changer
  operations
* Consistent slot and `InChanger` information without manual `update slots`

### Negative Consequences

* The Director and Storage Daemon protocols are incompatible, so both
  daemons must be upgraded together
* Each mount adds a Director round trip
* After a Director restart during running jobs, the model must be rebuilt
  from the Storage Daemon
* NDMP code paths need a separate review

## Pros and Cons of the Options

### Director is the single decision maker

* Good, because the catalog, policy and state live in one process and one
  lock domain
* Good, because the policy can be tested without daemons
* Bad, because it needs a new protocol and a large refactoring

### Storage Daemon is the single decision maker

* Good, because the Storage Daemon is close to the hardware
* Bad, because the pool, retention and recycling policy and the catalog
  live in the Director and would have to be duplicated or moved
* Bad, because a Storage Daemon cannot see jobs on other Storage Daemons

### Keep the split, add locking

* Good, because it needs the smallest change
* Bad, because it cannot make one decision atomic across two processes and
  only narrows the race windows
