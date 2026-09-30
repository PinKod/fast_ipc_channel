# logbook

Thoughts and log of them from v1 to c_v22_3
#### about namings
- versons incremented from 1
- new vesrion number kinda big change
- version with additonal number, for example `v7_2` or `v7_3` more evoltuion, then changing(small tunning) 
- no prefix working version with no compression
- prefix `nr_` - broken version no need to tests(for example live lock)
- prefix  `c_` - version with compression

## 0) v1-v8

- v1 no comperssion, no lose, single treaded. ~2.2s baseline.

- v2 added - improved 21x times vs v1 version. maped file on prod side, local buffer in cos side. 0.107s.

- v3 added slight improment, 0.056s.

- v4 with passign size worse, v5 using dbule buffering on consumer side is done better. 0.048s vs
v3's 0.06s.

- v6/v7/v7_2/v7_3 mostly the same shape, small tweaks, nothing that moved the needle much.

- v7_4 better on avg+median system with no copression to send via 256 bytes shm. current realization
of copression on LZ4 is worse due to random distribution cuasuing more cpu cycles for nothing and
in general sending the raw file as is. Still single threaded.

## 1) v9-v21, raw transfer, no compression yet

- v9: state+wr_bytes were two separate atomics on the same cache line, lshaz flagged it proven.
merged into one packed ctrl word, one atomic store instead of two per chunk. 3/3 identical,
lshaz finding gone. real sudo numbers put v9 and v7_4 within 2% of each other though -
SCHED_FIFO alone wasn't doing much.
```
found the real SCHED_FIFO livelock: consumer setted itself to SCHED_FIFO before
pthread_create the writer thread. new pthread inherits the parent's cpu affinity + scheduling policy at creation, so write_thread was born already pinned to the same core, same priority, as a busy-spinning parent. This lead to situaiton where write_thread COULD NEVER RUN. !!! LIVELOCK !!!.
```

#### fix: 
```
child pins+escalates+then signals ready, parent waits on the (still normal-scheduler) flag before
escalating itself.
applied to v5/v6/v7/v7_3/v7_4/v9. No more hangs.
```

- v10: producer side prefetch, __builtin_prefetch ahead of the memcpy. regressed, 21% slower
under sudo. Files alredy have advice MAP_POPULATE && MADV_WILLNEED - extra branch+prefetch per
248-byte chunk cost more than it saved. Tried diffent PREFETCH_AHEAD values, all regressed
about the same, not a tuning problem. kept the code.

- v11/v12: round-robin 4 lanes instead of 1, real win. shm_ring holds SHM_LANES=4 slots, producer
round-robins chunk i into lanes[i % 4], consumer runs 4 reader threads (main + 3 extra) each on
its own physical core, each memcpy straight into its own offset of the
output mmap - no locking needed - ZONES NEVER OVERLAP.

## from here started to use 1 GB file, cause optmization was going to well and startup/exit seqwence(file mmap, smh open and mmap, pthread creation, thread join, munmap, descriptors close)get the near most of running time - this lead for no ability no understand the speed diffrence betwen version of actual data transfer through shm region

- v12 is the same as v11 but moved out the v10 prefetch
code (wanted to know if v11 won despite it or because of it - turned out neutral either way, v12
kept as the cleaner version). sudo/1GB: v11 1.279s, v12 1.282s, both 35% faster than v9's 1.97s.

- v13: fixed a real bug while in there - madvise(file_mapping, size, MADV_SEQUENTIAL | MADV_WILLNEED)
was OR two enum values together, advice isn't a bitmask, that call had always just been
MADV_WILLNEED, MADV_SEQUENTIAL never actually applied in any version up to this point. split into
two calls. also tried SSE2 non-temporal stores (_mm_stream_si128) for the shm chunk copy - v12 on speed.

- v14: pushed SHM_LANES 4->7 to use the 3 idle E-cores too. regressed ~36%, workload was already
bandwidth-saturated at 4 lanes and the extra coordination (7-way round robin, more ready-signals,
weaker e-core throughput) cost more than it bought. not used fro next evoltuion.

- v15/v16: two hugepage attempts - both failed.
v15 = MAP_HUGETLB can't back a real file, so gave up zero-copy mmap entirely on both sides,
explicit read()/write() into an anonymous hugetlb buffer instead. needs sysctl vm.nr_hugepages
reserved first, fails loud + fast if it's not (good). with a real reservation in place it's still
34% slower under sudo - the explicit full-file copy costs more than the TLB win saves, and
consumer's single serial write() at the end kills the incremental async writeback v12/v13 got
for free.
v16 = dedicated prefault thread on a spare e-core, touching every output page before the real
writers get there so the fault happens ahead of time. backfired hard - 5x slower. checked why
with perf stat: v13 baseline 8,347 page faults, v16 258,128 - 31x more not less.
```
mechanism: v13's
normal path lets whoever writes a page first take one fault and it's resident+writable for everyone after; the prefault thread maps every page read-only first, so the real writer needs a separate upgrade fault, and since one 4kb page holds 17 round-robin chunks spread across all 4 lanes this repeats way more than once per page. not used for next versions.
```

- v16_2: same prefault idea as v16 but done in different way. Instead of a manual read-touch loop, one call to madvise(out_mapping, size, MADV_POPULATE_WRITE) - populates the page table for write access
directly, can't corrupt data so no sync needed with the real writers.
155 page faults vs v13's 8,347, 54x fewer. real win: producer -26%, consumer -17% at 250MB, +9%
faster than v13 at real 1GB/sudo scale. First improvemnt from v12.

- v16_3: deep-profiled with perf(kernel also) v16_2 and found a 5-6% chunk of consumer cycles going into ext4 (ext4_da_get_block_prep, mark_buffer_dirty, etc) - direct result of ftruncate() only setting the logical size, leaving a sparse file with no real blocks committed. swapped ftruncate() for fallocate(fd, 0, 0, size) - one line, actually reserves the
disk blocks up front. *BIGGETS* win: consumer ~33% faster than v16_2 at
250MB, and at real 1GB/sudo scale: 0.858s total vs v16_2's 1.089s vs v13's 1.314s vs v12's 1.272s.
context-switches *DROPPED* from the 1K down to an 18 average. **this is the raw-transfer best version - v16_3.**

- v19: tried TPAUSE/UMWAIT instead of the plain pause-spin on every wait point (producer waiting for
a lane to empty, readers waiting for it to fill). IPC was down around 0.10 at this point so most
cores are mostly waiting, umwait/umonitor should let a waiting core back off instead of burning the
core. regressed instead - 65% slower depending on timeout value. Tried diffrent time values 500 and 50 cycles, *all was worse*. umwait setup cost is bigger than a single pause instruction, raw spinlock on the atomic for how this workload actually waits is the better fit. not used fro evoltuion.

### tmpfs experiment: copied a data file into /dev/shm (already tmpfs) and pointed DATA_FILE at it
zero code change - wanted to see if it can kill the remaining kernel_init_pages/ext4 tail. results - *mixed*
- slower on one repeated 250MB run
- 1GB deep-profile run had diffent unstable times

### come back to default fs
- v20/v21: memfd_create instead of mmap(MAP_ANONYMOUS|MAP_HUGETLB) for the consumer's intermediate
buffer, v20 with MFD_HUGETLB|MFD_HUGE_2MB, v21 plain. same structural problem as v17 below though -
memfd_create makes a path-less file, can never *be* the real output file, so you still need the
same parallel pwrite-at-the-end flush regardless of what backs the buffer. wall-clock confirms it:
both lose to v16_3, v20 -70% and v21 -70% at 250MB. the perf-stat numbers are intresting
though - v20 gets *206x* fewer dTLB-store-misses and *900x* significatly less page-faults than v16_3 - *WINS EVERY*
low-level metric on each rep - reproducaly(not noise), *BUT* it still doesn't translate to a actaul run win
because the extra buffer + flush-at-the-end costs more than what the TLB reduction buys back. not used fro evoltuion, but a good data point: hugepages *do* deliver exactly what they promise at the TLB level,
every single time this session measured it directly, it just never once was worth the allocation
overhead on this workload.


## 2) c_v22 family - compression finally goes in!!

- c_v22: base = v16_3. producer restructured from single-threaded to 4 worker threads, one per lane,
each compressing+sending its own disjoint block range with LZ4_compress_default, 128kb blocks.
consumer's existing reader threads extended to reassemble + LZ4_decompress_safe each block. links
liblz4.a static(use cutom local builded lz4 lib https://github.com/lz4/lz4.git)
250MB no sudo: 0.441s vs v16_3's 0.465s, c_v22 already wins - but the producer's own stdout tells
the real story: `262144000 bytes in, 263173610 bytes compressed, ratio 0.39%`. LZ4 does not shrink
this float32 data at all, matches exactly what the old pre-session commit already found. the win
is producer-side parallelism spreading the same per-chunk overhead across 4 cores, not the
compression doing anything. real sudo/1GB: 1.006s vs v16_3's 1.230s, 18% faster, and way more
consistent run to run (v16_3 keeps spiking to 1s+ on random reps, c_v22 stays 0.46-0.55s).

- c_v22_2/c_v22_3: each of c_v22's 4 threads was doing its own malloc(132kb) at thread start. asked
for one allocation up front before any thread exists, sliced per lane, instead. tried two backings:
mmap(MAP_HUGETLB) for c_v22_2, plain malloc() for c_v22_3. malloc one time in c_v22_3 is the
winner - (probably could also just do plain mmap with no hugepages to shave libc malloc
instructions but who cares about this at this point, diminishing returns). mmap+hugetlb regressed
again, same story as literally everywhere else hugetlb got tried this whole session. the data
between c_v22/c_v22_2/c_v22_3 was honestly noisy as hell though - ranking flipped between back to
back sudo runs with zero code changes in between (~5-10% swings, page-fault baselines drifting
2082 -> 2872 -> 8832 across consecutive sessions, looks like thermal/cache-state drift not a code
effect). went with c_v22_3 anyway since it's the cleanest of the three (no hugetlb reservation).

## 3) compresion actullly happens but ddiint give anything..

### 3.1) c_v23 - xor-delta

- c_v23 - Each block XOR's every float against the previous one
in the same block, drops the delimiter bytes entirely (always 0x0a, reconstructed on decode), then
LZ4's the packed xor stream. real near 20% ratio this time, lossless, confirmed byte-identical
output - first actual compression win. also
switched fallocate() to FALLOC_FL_ZERO_RANGE while in there.

250MB no sudo: c_v23 clearly fastest of the four compared (0.386s vs c_v22_3's 0.410s). but decode
cost eats the savings at 1GB scale - real sudo/1GB comparison came back basically tied with
c_v22_3 (0.970s vs 0.965s, c_v23 actually ahead by median, behind by average, noise-level either
way). perfomamce profile explation: consumer IPC nearly doubles (0.169 -> 0.295) doing real
productive xor-decode work, but instructions executed go up 110% decoding 200M individual float
records for a 1GB file - eats most of what the 20% payload reduction should have bought back.
FALLOC_FL_ZERO_RANGE turned out to do nothing measurable either, kernel_init_pages still 20% of
consumer cycles same as before the flag, matches what v16_2->v16_3 already found - this is a
page-cache first-touch cost, not an extent/block-allocation thing, no flag fixes it. left behind,
tied with c_v22_3 isn't good enough to justify the extra complexity

### 3.2) final numbers

c_v22_3 vs v16_3 sudo/1GB: 0.95s vs 1.23s, 18% faster.

**c_v22_3 is the final compression version. v16_3 is best raw transfer**