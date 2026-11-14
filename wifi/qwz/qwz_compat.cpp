/*
 * qwz_compat.cpp - OpenBSD kernel services for qwz(4) on macOS.
 * See qwz_compat.h for the concurrency and DMA model.
 *
 * Copyright (c) 2026 razvanccr. GPL-2.0-or-later (see LICENSE).
 */
#include "qwz_compat.h"
#include "qwz_fw.h"

#include <IOKit/IOLocks.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <kern/clock.h>
#include <kern/thread.h>

#define QWZLOG(fmt, ...)	qwz_log("qwz: " fmt "\n", ##__VA_ARGS__)

/* ---- malloc(9) -------------------------------------------------------- */

/* free(9) may be called with size 0, so every block records its own size. */
struct qwz_mhdr {
	size_t	size;
	size_t	magic;
};
#define QWZ_MAGIC	0x71777a6dUL	/* "qwzm" */

/*
 * M0 (C26): live malloc(9) blocks and bytes, the leak detector the 15th-wake
 * failure lacked (health line "mem=<blocks>/<KB>"). Relaxed atomics: the
 * numbers are read by a sampler, nothing orders against them.
 */
volatile uint32_t qwz_mem_blocks;
volatile uint64_t qwz_mem_bytes;

void *
qwz_malloc(size_t sz, int type, int flags)
{
	size_t tot = sz + sizeof(struct qwz_mhdr);
	struct qwz_mhdr *h;

	if (tot < sz)
		return NULL;
	h = (struct qwz_mhdr *)IOMalloc(tot);
	if (h == NULL) {
		if (!(flags & M_NOWAIT))
			QWZLOG("malloc(%zu) failed with M_WAITOK", sz);
		return NULL;
	}
	h->size = tot;
	h->magic = QWZ_MAGIC;
	__atomic_add_fetch(&qwz_mem_blocks, 1u, __ATOMIC_RELAXED);
	__atomic_add_fetch(&qwz_mem_bytes, (uint64_t)tot, __ATOMIC_RELAXED);
	if (flags & M_ZERO)
		bzero(h + 1, sz);
	return h + 1;
}

void *
qwz_mallocarray(size_t n, size_t sz, int type, int flags)
{
	if (sz != 0 && n > SIZE_MAX / sz)
		return NULL;
	return qwz_malloc(n * sz, type, flags);
}

void
qwz_free(void *p, int type, size_t sz)
{
	struct qwz_mhdr *h;

	if (p == NULL)
		return;
	h = (struct qwz_mhdr *)p - 1;
	if (h->magic != QWZ_MAGIC)
		panic("qwz: free(%p) of a block malloc() did not return", p);
	h->magic = 0;
	__atomic_sub_fetch(&qwz_mem_blocks, 1u, __ATOMIC_RELAXED);
	__atomic_sub_fetch(&qwz_mem_bytes, (uint64_t)h->size, __ATOMIC_RELAXED);
	IOFree(h, h->size);
}

/*
 * itl80211's ieee80211_var.h has "#define explicit_bzero bzero", which
 * turned this definition into a bzero() that called itself: every bzero()
 * in the kext bound to it and overflowed the kernel stack (a double fault,
 * so the machine froze or reset with no panic). Undo the macro here.
 */
#undef explicit_bzero
void
explicit_bzero(void *p, size_t n)
{
	bzero(p, n);
	__asm__ volatile("" ::: "memory");
}

uint64_t
getnsecuptime(void)
{
	uint64_t abs, ns;

	clock_get_uptime(&abs);
	absolutetime_to_nanoseconds(abs, &ns);
	return ns;
}

/* ---- sleep / wakeup --------------------------------------------------- */

static IOCommandGate *qwz_gate;

void
qwz_compat_set_gate(IOCommandGate *gate)
{
	qwz_gate = gate;
}

IOCommandGate *
qwz_compat_gate(void)
{
	return qwz_gate;
}

/*
 * M0 (C01): the life word of the softc (sc_life) is reached through a pointer
 * that ItlQwz sets at attach and clears BEFORE the softc is freed. NULL means
 * "not dead". Every writer and every reader runs under the gate (a task wakes
 * inside commandSleep with the gate held), so the NULL store is ordered before
 * any later read.
 */
static volatile uint32_t *qwz_life_word;
static unsigned int qwz_refused_total;
static char qwz_dead_yield_chan;			/* nobody ever wakes this channel: a pure 1 ms yield */

void
qwz_compat_set_life(volatile uint32_t *life)
{
	qwz_life_word = life;
}

/* M0 (C26): the refused-sleep total for the health line ("refused="); qwz_refused_total itself stays file-static */
unsigned int
qwz_refused_count(void)
{
	return __atomic_load_n(&qwz_refused_total, __ATOMIC_RELAXED);
}

static inline bool
qwz_dead_now(void)
{
	return qwz_dead_aware && qwz_life_word != NULL &&
	    (__atomic_load_n(qwz_life_word, __ATOMIC_ACQUIRE) & QWZ_LIFE_DEAD) != 0;
}

/*
 * A DEAD return yields the gate for 1 ms first, so an unaudited retry loop
 * degrades into a slow loop, never a gate-held spin.
 */
static void
qwz_dead_yield(void)
{
	uint64_t interval, deadline;

	nanoseconds_to_absolutetime(1000000ULL, &interval);	/* 1 ms */
	deadline = mach_absolute_time() + interval;
	qwz_gate->commandSleep((void *)&qwz_dead_yield_chan, deadline, THREAD_UNINT);
}

/*
 * Sleep on `ident`, releasing the gate while asleep. Only legal from a thread
 * that holds the gate and is not the workloop thread itself; OpenBSD forbids
 * the equivalent (sleeping in interrupt context) too. A refusal returns
 * EDEADLK (not EWOULDBLOCK, which is a real timeout). With dead_aware, a DEAD
 * firmware ends the wait with ESHUTDOWN; waits that are not about the firmware
 * (delay and poll loops) use qwz_tsleep_delay, which passes dead_aware = false.
 */
static int
qwz_tsleep_common(const volatile void *ident, const char *wmesg, uint64_t nsecs, bool dead_aware, void *ra)
{
	IOWorkLoop *wl;
	IOReturn ret;

	if (qwz_gate == NULL || (wl = qwz_gate->getWorkLoop()) == NULL) {		/* unchanged */
		QWZLOG("tsleep(%s) before the gate exists", wmesg);
		IOSleep(nsecs == INFSLP ? 1000 : (uint32_t)(nsecs / 1000000) + 1);
		return EWOULDBLOCK;
	}
	if (wl->onThread() || !wl->inGate()) {
		unsigned int n = __atomic_add_fetch(&qwz_refused_total, 1u, __ATOMIC_RELAXED);

		if (n <= 8 || (n % 256) == 0)
			QWZLOG("tsleep(%s) refused #%u: %s; caller +0x%lx from qwz_tsleep_nsec", wmesg, n,
			    wl->onThread() ? "workloop thread" : "outside the gate",
			    (unsigned long)((uintptr_t)ra - (uintptr_t)&qwz_tsleep_nsec));
		return EDEADLK;
	}
	if (dead_aware && qwz_dead_now()) {
		qwz_dead_yield();
		return ESHUTDOWN;
	}
	if (nsecs == INFSLP) {
		ret = qwz_gate->commandSleep((void *)ident, THREAD_UNINT);
	} else {
		uint64_t interval, deadline;

		nanoseconds_to_absolutetime(nsecs, &interval);
		deadline = mach_absolute_time() + interval;
		ret = qwz_gate->commandSleep((void *)ident, deadline, THREAD_UNINT);
	}
	if (dead_aware && qwz_dead_now())	/* woken by qwz_wake_all_waiters, or by a real event that no longer matters */
		return ESHUTDOWN;
	switch (ret) {
	case THREAD_AWAKENED:			/* == kIOReturnSuccess == 0 */
		return 0;
	case THREAD_TIMED_OUT:			/* 1: what commandSleep really returns (kern_types.h:53) */
	case kIOReturnTimeout:
		return EWOULDBLOCK;
	case THREAD_INTERRUPTED:
	case kIOReturnAborted:
		return EINTR;
	default:
		QWZLOG("tsleep(%s): commandSleep returned 0x%x", wmesg, ret);
		return EWOULDBLOCK;
	}
}

int
qwz_tsleep_nsec(const volatile void *ident, int prio, const char *wmesg, uint64_t nsecs)
{
	return qwz_tsleep_common(ident, wmesg, nsecs, true, __builtin_return_address(0));
}

int
qwz_tsleep_delay(const volatile void *ident, int prio, const char *wmesg, uint64_t nsecs)
{
	return qwz_tsleep_common(ident, wmesg, nsecs, false, __builtin_return_address(0));
}

void
qwz_wakeup(const volatile void *ident)
{
	if (qwz_gate != NULL)
		qwz_gate->commandWakeup((void *)ident, false);
}

void
qwz_wakeup_one(const volatile void *ident)
{
	if (qwz_gate != NULL)
		qwz_gate->commandWakeup((void *)ident, true);
}

/* ---- rwlock(9) / refcnt(9) -------------------------------------------- */

void
rw_init(struct rwlock *rwl, const char *name)
{
	rwl->rwl_name = name;
	rwl->rwl_owner = NULL;
}

/*
 * M0 (C02): a waiter sleeps in 100 ms slices with qwz_tsleep_delay (not
 * DEAD-aware: it must really release the gate so the owner can finish), never
 * spins on a refusal, and gives up after QWZ_RWL_WAIT_MS (above the longest
 * legitimate hold: a healthy qwz_init is about 1 s). Every caller handles a
 * failure; nobody may rw_exit a lock it did not get.
 */
#define QWZ_RWL_WAIT_MS		20000
#define QWZ_REFCNT_WAIT_MS	15000

int
rw_enter(struct rwlock *rwl, int flags)
{
	void *self = current_thread();
	uint64_t dl = getnsecuptime() + MSEC_TO_NSEC(QWZ_RWL_WAIT_MS);
	int err;

	while (rwl->rwl_owner != NULL) {
		if (rwl->rwl_owner == self) {
			QWZLOG("rwlock %s: recursive acquire refused (EDEADLK)", rwl->rwl_name);
			return EDEADLK;
		}
		if (flags & RW_NOSLEEP)
			return EBUSY;
		err = qwz_tsleep_delay(rwl, 0, rwl->rwl_name, MSEC_TO_NSEC(100));
		if (err == EDEADLK) {		/* this thread cannot sleep: the owner cannot run while we spin */
			QWZLOG("rwlock %s busy (owner %p) and this thread cannot sleep: EBUSY, no spin", rwl->rwl_name, rwl->rwl_owner);
			return EBUSY;
		}
		if (err == EINTR && (flags & RW_INTR))
			return EINTR;
		if (getnsecuptime() >= dl) {
			QWZLOG("rwlock %s not acquired in %u ms (owner %p): ETIMEDOUT", rwl->rwl_name, QWZ_RWL_WAIT_MS, rwl->rwl_owner);
			return ETIMEDOUT;
		}
	}
	rwl->rwl_owner = self;
	return 0;
}

int
rw_enter_write(struct rwlock *rwl)
{
	return rw_enter(rwl, RW_WRITE);
}

/*
 * R1-05: the lock once more, after qwz_declare_dead ended the stuck holder's firmware waits (its next
 * wait fails at once, so it unwinds and releases within a few slices). Waits at most `ms`, in 100 ms
 * slices of qwz_tsleep_delay: unlike a DEAD-aware sleep (which returns at once while DEAD) it really
 * releases the gate, which the woken holder needs to run. Never spins: a refusal (workloop thread, or
 * outside the gate) returns EBUSY, a recursive acquire EDEADLK, a lapsed grace ETIMEDOUT; the caller then
 * goes on without the lock (C02). The owner is tested before the deadline: a lock released in the last
 * slice is taken.
 */
int
rw_enter_write_grace(struct rwlock *rwl, unsigned int ms)
{
	void *self = current_thread();
	uint64_t dl = getnsecuptime() + MSEC_TO_NSEC(ms);

	while (rwl->rwl_owner != NULL) {
		if (rwl->rwl_owner == self)
			return EDEADLK;
		if (qwz_tsleep_delay(rwl, 0, rwl->rwl_name, MSEC_TO_NSEC(100)) == EDEADLK)
			return EBUSY;
		if (rwl->rwl_owner != NULL && getnsecuptime() >= dl)
			return ETIMEDOUT;
	}
	rwl->rwl_owner = self;
	return 0;
}

void
rw_exit(struct rwlock *rwl)
{
	if (rwl->rwl_owner != NULL && rwl->rwl_owner != current_thread()) {
		static unsigned int not_owner;

		if (qwz_log_ratelimit(&not_owner, 4, 64))
			QWZLOG("rw_exit(%s) by a thread that is not the owner: ignored", rwl->rwl_name);
		return;
	}
	rwl->rwl_owner = NULL;
	qwz_wakeup(rwl);
}

void
rw_exit_write(struct rwlock *rwl)
{
	rw_exit(rwl);
}

void
refcnt_init(struct refcnt *r)
{
	SInt32 stale = r->r_gaveup && r->r_refs > 0 ? r->r_refs : 0;	/* tasks a give-up left running */

	r->r_gaveup = 0;
	r->r_refs = 1 + stale;			/* 1 exactly as today unless a give-up happened; the stale tasks release into this count */
	if (stale)
		QWZLOG("refcnt: %d task reference(s) left by an earlier give-up carried into the new epoch", (int)stale);
}

void
refcnt_take(struct refcnt *r)
{
	OSIncrementAtomic(&r->r_refs);
}

int
refcnt_rele(struct refcnt *r)
{
	return OSDecrementAtomic(&r->r_refs) == 1;	/* returns the old value */
}

void
refcnt_rele_wake(struct refcnt *r)
{
	if (refcnt_rele(r))
		qwz_wakeup(r);
}

/*
 * M0 (C02): waits in 100 ms slices; after 5 s logs how many references are
 * still held; gives up after QWZ_REFCNT_WAIT_MS with a log and returns; returns
 * at once on a refusal. A give-up leaves tasks that still hold a reference, so
 * r_gaveup is set and the next refcnt_init carries their references over: the
 * count stays exact and the stale tasks release into it when they finish.
 */
void
refcnt_finalize(struct refcnt *r, const char *wmesg)
{
	uint64_t t0 = getnsecuptime(), now;
	bool warned = false;

	refcnt_rele(r);
	while (r->r_refs > 0) {
		if (qwz_tsleep_delay(r, 0, wmesg, MSEC_TO_NSEC(100)) == EDEADLK) {
			QWZLOG("refcnt %s: %d held and this thread cannot sleep: not waiting", wmesg, (int)r->r_refs);
			r->r_gaveup = 1;
			return;
		}
		now = getnsecuptime();
		if (!warned && now - t0 > SEC_TO_NSEC(5)) {
			warned = true;
			QWZLOG("refcnt %s: %d task(s) did not finish in 5 s", wmesg, (int)r->r_refs);
		}
		if (now - t0 > MSEC_TO_NSEC(QWZ_REFCNT_WAIT_MS)) {
			QWZLOG("refcnt %s: giving up after %u ms with %d held", wmesg, QWZ_REFCNT_WAIT_MS, (int)r->r_refs);
			r->r_gaveup = 1;			/* the next refcnt_init carries the still-running tasks over */
			return;
		}
	}
}

/* ---- soft asserts (C03) ------------------------------------------------- */

/*
 * QWZ_RBAD(bad) is true when the BAD condition holds. With debug.qwz.softassert=0
 * this panics like KASSERT (the message names the assertion that failed, !(bad)).
 * Otherwise it counts, logs (the first 8, then every 64th) and returns 1, and the
 * site runs its fix instead of panicking. A panic cannot be shown or saved on
 * this machine, so it looks like a hard freeze.
 */
int
qwz_rassert_hit(const char *expr, const char *file, int line)
{
	static unsigned int n;

	if (!qwz_softassert)
		panic("qwz: KASSERT !(%s) at %s:%d", expr, file, line);
	if (qwz_log_ratelimit(&n, 8, 64))
		qwz_log("qwz: soft-assert #%u: !(%s) at %s:%d: continuing (debug.qwz.softassert=0 panics)\n", n, expr, file, line);
	return 1;
}

/* ---- process-context timeouts ----------------------------------------- */

struct qwz_proc_timeout {
	struct task	task;
};

static void
qwz_proc_timeout_fire(void *arg)
{
	struct qwz_proc_timeout *pt = (struct qwz_proc_timeout *)arg;

	task_add(systq, &pt->task);
}

void
qwz_timeout_set_proc(CTimeout **to, void (*fn)(void *), void *arg)
{
	/* Lives as long as the driver: one per timeout_set_proc() at attach. */
	struct qwz_proc_timeout *pt = (struct qwz_proc_timeout *)
	    qwz_malloc(sizeof(*pt), M_DEVBUF, M_WAITOK | M_ZERO);

	task_set(&pt->task, fn, arg, "qwz_proc_timeout");
	timeout_set(to, qwz_proc_timeout_fire, pt);
}

void
qwz_timeout_free_proc(CTimeout **to)
{
	struct qwz_proc_timeout *pt;

	if (to == NULL || *to == NULL)
		return;
	pt = (struct qwz_proc_timeout *)(*to)->to_arg;
	task_del(systq, &pt->task);
	timeout_free(to);
	qwz_free(pt, M_DEVBUF, sizeof(*pt));
}

/* ---- PCI ---------------------------------------------------------------- */

pcireg_t
pci_conf_read(pci_chipset_tag_t pc, pcitag_t tag, int reg)
{
	return tag->configRead32(reg);
}

void
pci_conf_write(pci_chipset_tag_t pc, pcitag_t tag, int reg, pcireg_t val)
{
	tag->configWrite32(reg, val);
}

int
pci_get_capability(pci_chipset_tag_t pc, pcitag_t tag, int capid, int *offp, pcireg_t *valp)
{
	UInt8 off = 0;

	if (tag->findPCICapability((UInt8)capid, &off) == 0)
		return 0;
	if (offp)
		*offp = (int)off;
	if (valp)
		*valp = tag->configRead32(off);
	return 1;
}

pcireg_t
pci_mapreg_type(pci_chipset_tag_t pc, pcitag_t tag, int reg)
{
	return tag->configRead32(reg) & 0x7;	/* I/O-vs-memory and 32/64-bit bits */
}

static IOMemoryMap *qwz_bar_map;

int
pci_mapreg_map(struct pci_attach_args *pa, int reg, pcireg_t type, int flags,
    bus_space_tag_t *tagp, bus_space_handle_t *handlep, bus_addr_t *basep,
    bus_size_t *sizep, bus_size_t maxsize)
{
	IOMemoryMap *map = pa->pa_tag->mapDeviceMemoryWithRegister(reg);

	if (map == NULL)
		return ENOMEM;
	OSSafeReleaseNULL(qwz_bar_map);	/* a retried bring-up maps again */
	qwz_bar_map = map;
	if (tagp)
		*tagp = 0;
	if (handlep)
		*handlep = (bus_space_handle_t)map->getVirtualAddress();
	if (basep)
		*basep = map->getPhysicalAddress();
	if (sizep)
		*sizep = map->getLength();
	QWZLOG("BAR 0x%x: phys 0x%llx, %llu bytes", reg,
	    (unsigned long long)map->getPhysicalAddress(),
	    (unsigned long long)map->getLength());
	return 0;
}

void
qwz_pci_unmap_bar(void)
{
	OSSafeReleaseNULL(qwz_bar_map);
}

int
pci_set_powerstate(pci_chipset_tag_t pc, pcitag_t tag, int state)
{
	UInt8 off = 0;
	uint16_t pmcsr;

	if (tag->findPCICapability(kIOPCIPowerManagementCapability, &off) == 0)
		return EOPNOTSUPP;
	pmcsr = tag->configRead16(off + 4);
	if ((pmcsr & 0x3) != (state & 0x3)) {
		tag->configWrite16(off + 4, (pmcsr & ~0x3) | (state & 0x3));
		IOSleep(10);	/* D3hot -> D0 needs 10 ms */
	}
	return 0;
}

/* Multiple MSI vectors are not set up yet; qwz falls back to one vector. */
int
pci_intr_enable_msivec(struct pci_attach_args *pa, int num_vec)
{
	return EOPNOTSUPP;
}

static int
qwz_find_intr_index(IOPCIDevice *dev, bool msi)
{
	int index, type;

	for (index = 0; dev->getInterruptType(index, &type) == kIOReturnSuccess; index++) {
		if (((type & kIOInterruptTypePCIMessaged) != 0) == msi)
			return index;
	}
	return -1;
}

int
pci_intr_map_msi(struct pci_attach_args *pa, pci_intr_handle_t *ih)
{
	ih->vec = qwz_find_intr_index(pa->pa_tag, true);
	return ih->vec < 0 ? ENXIO : 0;
}

int
pci_intr_map_msivec(struct pci_attach_args *pa, int vec, pci_intr_handle_t *ih)
{
	return vec == 0 ? pci_intr_map_msi(pa, ih) : EOPNOTSUPP;
}

int
pci_intr_map(struct pci_attach_args *pa, pci_intr_handle_t *ih)
{
	ih->vec = qwz_find_intr_index(pa->pa_tag, false);
	return ih->vec < 0 ? ENXIO : 0;
}

const char *
pci_intr_string(pci_chipset_tag_t pc, pci_intr_handle_t ih)
{
	return "msi";
}

/* An interrupt handler: the filter always claims (MSI is never shared) and
 * the action runs the qwz handler on the workloop, holding the gate. */
class QwzIntr : public OSObject {
	OSDeclareDefaultStructors(QwzIntr)
public:
	int (*fn)(void *);
	void *arg;
	IOFilterInterruptEventSource *src;
	IOWorkLoop *workloop;

	bool
	init() override
	{
		return OSObject::init();
	}
	static bool
	filter(OSObject *owner, IOFilterInterruptEventSource *)
	{
		return true;
	}
	static void
	action(OSObject *owner, IOInterruptEventSource *, int)
	{
		QwzIntr *ih = (QwzIntr *)owner;
		ih->fn(ih->arg);
	}
};
OSDefineMetaClassAndStructors(QwzIntr, OSObject)

void *
qwz_pci_intr_establish(struct pci_attach_args *pa, pci_intr_handle_t ih, int level,
    int (*fn)(void *), void *arg, const char *name)
{
	QwzIntr *h = new QwzIntr;

	if (h == NULL || !h->init()) {
		OSSafeReleaseNULL(h);
		return NULL;
	}
	h->fn = fn;
	h->arg = arg;
	h->workloop = pa->pa_workloop;
	h->src = IOFilterInterruptEventSource::filterInterruptEventSource(h,
	    &QwzIntr::action, &QwzIntr::filter, pa->pa_tag, ih.vec);
	if (h->src == NULL || h->workloop->addEventSource(h->src) != kIOReturnSuccess) {
		QWZLOG("%s: cannot attach interrupt index %d", name, ih.vec);
		OSSafeReleaseNULL(h->src);
		h->release();
		return NULL;
	}
	h->src->enable();
	QWZLOG("%s: interrupt index %d established", name, ih.vec);
	return h;
}

void
pci_intr_disestablish(pci_chipset_tag_t pc, void *cookie)
{
	QwzIntr *h = (QwzIntr *)cookie;

	if (h == NULL)
		return;
	if (h->src) {
		h->src->disable();
		h->workloop->removeEventSource(h->src);
		OSSafeReleaseNULL(h->src);
	}
	h->release();
}

/* ---- bus_dma ------------------------------------------------------------ */

/*
 * Physically contiguous memory below 4 GB. Every qwz allocation and bounce
 * buffer comes from here. Page-granular masks keep alignment >= 4 KB.
 */
static IOBufferMemoryDescriptor *
qwz_alloc_low(size_t size, bus_size_t align, bus_addr_t high, bus_addr_t *pa)
{
	IOBufferMemoryDescriptor *md;
	mach_vm_address_t mask;

	if (align < PAGE_SIZE)
		align = PAGE_SIZE;
	mask = (high & 0xffffffffULL) & ~((mach_vm_address_t)align - 1);
	md = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
	    kIODirectionInOut | kIOMemoryPhysicallyContiguous, size, mask);
	if (md == NULL)
		return NULL;
	if (md->prepare() != kIOReturnSuccess) {
		md->release();
		return NULL;
	}
	*pa = md->getPhysicalSegment(0, NULL, kIOMemoryMapperNone);
	bzero(md->getBytesNoCopy(), size);
	return md;
}

/*
 * MS-1: the DMA hold. A no-link teardown cannot clear the bus-master bit (a config write to a downed link is
 * lost) and cannot reset the chip, so a firmware that survived the outage may DMA into anything the host frees
 * until the next power-up has verified the chip in reset (qwz_pci_late_reset). While the hold is on, qwz_free_low
 * parks the descriptor (the pages stay allocated and keep their contents, so the firmware keeps seeing its own
 * rings) and qwz_dma_hold(0) frees them. qwz_dma_hold(0) is called only on verified paths (late-reset success,
 * a healthy power-down with a live link). A teardown returns only descriptors. DETACH also returns the MHI,
 * event and cmd rings, the context arrays, the amss buffers, fwmem and m3 (qwz_pci_detach, qwz_detach), and
 * qwz_compat_fini returns the pool chunks (the bounce slots, the CE rx buffers among them). With the hold on,
 * detach parks the first group and qwz_compat_fini leaves all of them allocated on purpose (HF-6). The lock is
 * the one of the bounce pools: nothing calls qwz_free_low with it held.
 */
static IOLock *qwz_dma_lock;
struct qwz_dma_parked {
	struct qwz_dma_parked	*next;
	IOBufferMemoryDescriptor *md;
	size_t			 len;
};
static int			qwz_dma_hold_on;
static struct qwz_dma_parked	*qwz_dma_parked_head;
static uint32_t			qwz_dma_parked_n, qwz_dma_leaked_n;
static uint64_t			qwz_dma_parked_bytes;

static void
qwz_free_low(IOBufferMemoryDescriptor *md)
{
	if (qwz_dma_lock != NULL) {
		IOLockLock(qwz_dma_lock);
		if (qwz_dma_hold_on) {
			struct qwz_dma_parked *p = (struct qwz_dma_parked *)qwz_malloc(sizeof(*p), M_DEVBUF, M_NOWAIT | M_ZERO);

			if (p != NULL) {
				p->md = md;
				p->len = md->getLength();
				p->next = qwz_dma_parked_head;
				qwz_dma_parked_head = p;
				qwz_dma_parked_n++;
				qwz_dma_parked_bytes += p->len;
			} else
				qwz_dma_leaked_n++;	/* no node: leak the block, never free a page the firmware may still write */
			IOLockUnlock(qwz_dma_lock);
			return;				/* no complete(): the block stays prepared until it is released */
		}
		IOLockUnlock(qwz_dma_lock);
	}
	md->complete();
	md->release();
}

void
qwz_dma_hold(int on)
{
	struct qwz_dma_parked *list, *p;
	uint32_t n, leaked;
	uint64_t bytes;

	if (qwz_dma_lock == NULL)
		return;
	IOLockLock(qwz_dma_lock);
	if (on) {
		int was = qwz_dma_hold_on;

		qwz_dma_hold_on = 1;
		IOLockUnlock(qwz_dma_lock);
		if (!was)
			QWZLOG("dma: hold on: DMA memory freed from now on is parked until the chip is verified in reset");
		return;
	}
	if (!qwz_dma_hold_on) {
		IOLockUnlock(qwz_dma_lock);
		return;
	}
	qwz_dma_hold_on = 0;
	list = qwz_dma_parked_head;
	n = qwz_dma_parked_n;
	leaked = qwz_dma_leaked_n;
	bytes = qwz_dma_parked_bytes;
	qwz_dma_parked_head = NULL;
	qwz_dma_parked_n = qwz_dma_leaked_n = 0;
	qwz_dma_parked_bytes = 0;
	IOLockUnlock(qwz_dma_lock);
	while ((p = list) != NULL) {
		list = p->next;
		p->md->complete();
		p->md->release();
		qwz_free(p, M_DEVBUF, sizeof(*p));
	}
	QWZLOG("dma: hold released: %u block(s), %llu KB freed, %u leaked (no list node)", n, (unsigned long long)(bytes / 1024), leaked);
}

/*
 * Bounce buffers: fixed-size slots carved out of 256 KB low chunks, one
 * free list per size class. Maps keep their slot until they are destroyed,
 * so the ring buffers that qwz reloads constantly never touch the allocator.
 */
struct qwz_bounce {
	struct qwz_bounce	*next;
	uint8_t			*kva;
	bus_addr_t		 pa;
	size_t			 size;
	IOBufferMemoryDescriptor *own;	/* set when not from a pool */
};

#define QWZ_POOL_CHUNK	(256 * 1024)
static const size_t qwz_pool_sizes[] = { 2048, 4096, 16384, 65536 };
#define QWZ_NPOOLS	(sizeof(qwz_pool_sizes) / sizeof(qwz_pool_sizes[0]))

struct qwz_pool_chunk {
	struct qwz_pool_chunk	*next;
	IOBufferMemoryDescriptor *md;
};

static struct qwz_bounce *qwz_pool_free[QWZ_NPOOLS];
static struct qwz_pool_chunk *qwz_pool_chunks;

static int
qwz_pool_grow(int cls)
{
	struct qwz_pool_chunk *c;
	size_t sz = qwz_pool_sizes[cls], i;
	bus_addr_t pa;
	uint8_t *kva;

	c = (struct qwz_pool_chunk *)qwz_malloc(sizeof(*c), M_DEVBUF, M_NOWAIT | M_ZERO);
	if (c == NULL)
		return ENOMEM;
	c->md = qwz_alloc_low(QWZ_POOL_CHUNK, PAGE_SIZE, 0xffffffffULL, &pa);
	if (c->md == NULL) {
		qwz_free(c, M_DEVBUF, sizeof(*c));
		return ENOMEM;
	}
	kva = (uint8_t *)c->md->getBytesNoCopy();
	for (i = 0; i + sz <= QWZ_POOL_CHUNK; i += sz) {
		struct qwz_bounce *b = (struct qwz_bounce *)
		    qwz_malloc(sizeof(*b), M_DEVBUF, M_NOWAIT | M_ZERO);
		if (b == NULL)
			break;
		b->kva = kva + i;
		b->pa = pa + i;
		b->size = sz;
		b->next = qwz_pool_free[cls];
		qwz_pool_free[cls] = b;
	}
	c->next = qwz_pool_chunks;
	qwz_pool_chunks = c;
	return 0;
}

static struct qwz_bounce *
qwz_bounce_get(size_t size)
{
	struct qwz_bounce *b = NULL;
	unsigned int cls;

	for (cls = 0; cls < QWZ_NPOOLS; cls++)
		if (size <= qwz_pool_sizes[cls])
			break;

	if (cls == QWZ_NPOOLS) {
		/* Larger than any pool: a dedicated allocation. */
		b = (struct qwz_bounce *)qwz_malloc(sizeof(*b), M_DEVBUF, M_NOWAIT | M_ZERO);
		if (b == NULL)
			return NULL;
		b->own = qwz_alloc_low(size, PAGE_SIZE, 0xffffffffULL, &b->pa);
		if (b->own == NULL) {
			qwz_free(b, M_DEVBUF, sizeof(*b));
			return NULL;
		}
		b->kva = (uint8_t *)b->own->getBytesNoCopy();
		b->size = size;
		return b;
	}

	IOLockLock(qwz_dma_lock);
	if (qwz_pool_free[cls] == NULL)
		qwz_pool_grow(cls);
	b = qwz_pool_free[cls];
	if (b != NULL)
		qwz_pool_free[cls] = b->next;
	IOLockUnlock(qwz_dma_lock);
	return b;
}

static void
qwz_bounce_put(struct qwz_bounce *b)
{
	unsigned int cls;

	if (b->own != NULL) {
		qwz_free_low(b->own);
		qwz_free(b, M_DEVBUF, sizeof(*b));
		return;
	}
	for (cls = 0; cls < QWZ_NPOOLS; cls++)
		if (b->size == qwz_pool_sizes[cls])
			break;
	IOLockLock(qwz_dma_lock);
	b->next = qwz_pool_free[cls];
	qwz_pool_free[cls] = b;
	IOLockUnlock(qwz_dma_lock);
}

/* kva -> bus address for memory from bus_dmamem_map(), for bus_dmamap_load(). */
struct qwz_dmamem_ent {
	struct qwz_dmamem_ent	*next;
	uint8_t			*kva;
	size_t			 size;
	bus_addr_t		 pa;
};
static struct qwz_dmamem_ent *qwz_dmamem_list;

void
qwz_compat_init(void)
{
	if (qwz_dma_lock == NULL)
		qwz_dma_lock = IOLockAlloc();
}

void
qwz_compat_fini(void)
{
	struct qwz_dma_parked *plist = NULL, *pn;
	struct qwz_pool_chunk *clist = NULL, *cn;
	uint32_t parked_n = 0, leaked_n = 0, chunk_n = 0;
	uint64_t parked_bytes = 0;
	unsigned int cls;
	int unverified = 0;

	/*
	 * MS-1 (HF-6): the hold is still on here when the chip was never verified in reset (a no-link teardown set it, and no power-up with a
	 * live link has run the late reset since). Detach has just freed the MHI and event rings, the amss buffers, fwmem and m3 into the
	 * parked list, and the CE rx descriptors still point at the pool chunks: a firmware that survived the outage, with bus mastering
	 * on again, may write into all of them, and nothing here can reset it. So they are left allocated, on purpose: no release of the
	 * parked list, no chunk free. Only the host-side bookkeeping goes (list nodes, counters, the flag): the descriptors stay retained
	 * and prepared with nothing pointing at them, and a later attach starts from clean statics. The cost is a few MB of wired low
	 * memory, once, in a state the chip has to be recovered from anyway; the alternative is a stray DMA write into reused pages.
	 * Everything is taken and reset in ONE critical section, the log and the node frees come after the unlock.
	 */
	if (qwz_dma_lock != NULL) {
		IOLockLock(qwz_dma_lock);
		if (qwz_dma_hold_on) {
			unverified = 1;
			plist = qwz_dma_parked_head;
			parked_n = qwz_dma_parked_n;
			leaked_n = qwz_dma_leaked_n;
			parked_bytes = qwz_dma_parked_bytes;
			clist = qwz_pool_chunks;
			for (cn = clist; cn != NULL; cn = cn->next)
				chunk_n++;
			qwz_dma_hold_on = 0;
			qwz_dma_parked_head = NULL;
			qwz_dma_parked_n = qwz_dma_leaked_n = 0;
			qwz_dma_parked_bytes = 0;
			qwz_pool_chunks = NULL;
		}
		IOLockUnlock(qwz_dma_lock);
	}
	if (unverified) {
		QWZLOG("dma: fini with the hold on: %u parked block(s), %llu KB (%u more without a list node), and %u pool chunk(s), %u KB, are left allocated on purpose: the chip was never verified in reset",
		    parked_n, (unsigned long long)(parked_bytes / 1024), leaked_n, chunk_n, chunk_n * (QWZ_POOL_CHUNK / 1024));
		while ((pn = plist) != NULL) {		/* the list nodes only; never pn->md */
			plist = pn->next;
			qwz_free(pn, M_DEVBUF, sizeof(*pn));
		}
		while ((cn = clist) != NULL) {		/* the list nodes only; never cn->md */
			clist = cn->next;
			qwz_free(cn, M_DEVBUF, sizeof(*cn));
		}
	}
	qwz_dma_hold(0);		/* the hold is off here (dropped above when it was on, so nothing is released): a no-op that keeps the invariant that no block is parked once fini runs, and the chunk frees below are never parked */
	for (cls = 0; cls < QWZ_NPOOLS; cls++) {
		while (qwz_pool_free[cls] != NULL) {
			struct qwz_bounce *b = qwz_pool_free[cls];
			qwz_pool_free[cls] = b->next;
			qwz_free(b, M_DEVBUF, sizeof(*b));
		}
	}
	while (qwz_pool_chunks != NULL) {
		struct qwz_pool_chunk *c = qwz_pool_chunks;
		qwz_pool_chunks = c->next;
		qwz_free_low(c->md);
		qwz_free(c, M_DEVBUF, sizeof(*c));
	}
	qwz_pci_unmap_bar();
	qwz_life_word = NULL;		/* the softc is gone (C01) */
	if (qwz_dma_lock != NULL) {
		IOLockFree(qwz_dma_lock);
		qwz_dma_lock = NULL;
	}
}

int
qwz_bus_dmamap_create(bus_dma_tag_t t, bus_size_t size, int nsegs, bus_size_t maxsegsz,
    bus_size_t boundary, int flags, bus_dmamap_t *mapp)
{
	bus_dmamap_t map;

	map = (bus_dmamap_t)qwz_malloc(sizeof(*map), M_DEVBUF, M_ZERO |
	    ((flags & BUS_DMA_NOWAIT) ? M_NOWAIT : M_WAITOK));
	if (map == NULL)
		return ENOMEM;
	map->_size = size;
	*mapp = map;
	return 0;
}

void
qwz_bus_dmamap_destroy(bus_dma_tag_t t, bus_dmamap_t map)
{
	if (map == NULL)
		return;
	if (map->_bounce)
		qwz_bounce_put(map->_bounce);
	qwz_free(map, M_DEVBUF, sizeof(*map));
}

static int
qwz_map_bounce(bus_dmamap_t map, bus_size_t len)
{
	if (len > map->_size)
		return EFBIG;
	if (map->_bounce == NULL && (map->_bounce = qwz_bounce_get(map->_size)) == NULL)
		return ENOMEM;
	map->dm_nsegs = 1;
	map->dm_mapsize = len;
	map->dm_segs[0].ds_addr = map->_bounce->pa;
	map->dm_segs[0].ds_len = len;
	return 0;
}

int
qwz_bus_dmamap_load(bus_dma_tag_t t, bus_dmamap_t map, void *buf, bus_size_t len,
    void *proc, int flags)
{
	struct qwz_dmamem_ent *e;
	uint8_t *kva = (uint8_t *)buf;

	/* Memory from bus_dmamem_map() is already low and contiguous. */
	IOLockLock(qwz_dma_lock);
	for (e = qwz_dmamem_list; e != NULL; e = e->next) {
		if (kva >= e->kva && kva + len <= e->kva + e->size) {
			map->dm_nsegs = 1;
			map->dm_mapsize = len;
			map->dm_segs[0].ds_addr = e->pa + (kva - e->kva);
			map->dm_segs[0].ds_len = len;
			IOLockUnlock(qwz_dma_lock);
			return 0;
		}
	}
	IOLockUnlock(qwz_dma_lock);

	/*
	 * Not from bus_dmamem_map(): bounce it. Nothing is copied unless the
	 * caller syncs (PREWRITE/POSTREAD), so say so: every qwz caller today
	 * passes dmamem, and a new one that does not would fail silently.
	 */
	QWZLOG("bus_dmamap_load: %zu bytes at %p are not dmamem, bouncing (flags 0x%x)",
	    (size_t)len, buf, flags);
	map->_kva = buf;
	return qwz_map_bounce(map, len);
}

int
qwz_bus_dmamap_load_mbuf(bus_dma_tag_t t, bus_dmamap_t map, mbuf_t m, int flags)
{
	size_t len = 0;
	mbuf_t n;
	int err;

	/* OpenBSD sums m_len over the chain; the pkthdr length may be stale. */
	for (n = m; n != NULL; n = mbuf_next(n))
		len += mbuf_len(n);
	err = qwz_map_bounce(map, len);
	if (err)
		return err;
	map->_mbuf = m;
	map->_flags = flags;
	map->_postread = 0;
	/* Transmit paths may not PREWRITE-sync (see the header): copy in now. */
	if (flags & BUS_DMA_WRITE)
		mbuf_copydata(m, 0, map->dm_mapsize, map->_bounce->kva);
	return 0;
}

int
qwz_bus_dmamap_load_raw(bus_dma_tag_t t, bus_dmamap_t map, bus_dma_segment_t *segs,
    int nsegs, bus_size_t size, int flags)
{
	if (nsegs != 1)
		return EINVAL;
	map->dm_nsegs = 1;
	map->dm_mapsize = size;
	map->dm_segs[0].ds_addr = segs[0].ds_addr;
	map->dm_segs[0].ds_len = size;
	return 0;
}

void
qwz_bus_dmamap_unload(bus_dma_tag_t t, bus_dmamap_t map)
{
	/* Receive paths may unload without POSTREAD-syncing: copy out now. */
	if (map->_mbuf != NULL && (map->_flags & BUS_DMA_READ) && !map->_postread)
		qwz_bus_dmamap_sync(t, map, 0, map->dm_mapsize, BUS_DMASYNC_POSTREAD);
	map->_flags = 0;
	map->dm_nsegs = 0;
	map->dm_mapsize = 0;
	map->_mbuf = NULL;
	map->_kva = NULL;
}

/*
 * PCIe on x86 is cache-coherent, so syncing only matters for bounced maps:
 * copy into the bounce buffer before the device reads it, and back out after
 * the device wrote it.
 */
void
qwz_bus_dmamap_sync(bus_dma_tag_t t, bus_dmamap_t map, bus_addr_t off, bus_size_t len, int ops)
{
	uint8_t *bounce;

	__asm__ volatile("mfence" ::: "memory");
	if (map->_bounce == NULL || (map->_mbuf == NULL && map->_kva == NULL))
		return;
	if (off >= map->dm_mapsize)
		return;
	if (off + len > map->dm_mapsize)
		len = map->dm_mapsize - off;
	bounce = map->_bounce->kva + off;

	/*
	 * PREREAD hands the buffer back to the device, so a copy-out done by
	 * an earlier POSTREAD is stale: unload must copy out again. (qwz's CE
	 * rx enqueue POSTREAD-syncs the new buffer before posting it, and the
	 * receive path then unloads without a sync; with the flag left set,
	 * the firmware's HTC ready message was never copied into the mbuf.)
	 */
	if (ops & BUS_DMASYNC_PREREAD)
		map->_postread = 0;
	if (ops & BUS_DMASYNC_PREWRITE) {
		if (map->_mbuf)
			mbuf_copydata(map->_mbuf, off, len, bounce);
		else
			memcpy(bounce, (uint8_t *)map->_kva + off, len);
	}
	if (ops & BUS_DMASYNC_POSTREAD) {
		map->_postread = 1;
		if (map->_mbuf) {
			if (mbuf_next(map->_mbuf) == NULL && off + len <= mbuf_len(map->_mbuf))
				memcpy((uint8_t *)mbuf_data(map->_mbuf) + off, bounce, len);
			else
				mbuf_copyback(map->_mbuf, off, len, bounce, MBUF_DONTWAIT);
#ifdef QWZ_DEBUG
			/* Diagnostic: the mbuf must now match what the device wrote. */
			{
				uint8_t chk[64];
				size_t o;
				for (o = 0; o < len; o += sizeof(chk)) {
					size_t n = len - o < sizeof(chk) ? len - o : sizeof(chk);
					mbuf_copydata(map->_mbuf, off + o, n, chk);
					if (memcmp(chk, bounce + o, n) != 0) {
						QWZLOG("bounce copy-out MISMATCH at %zu of %zu (mbuf len %zu, chain %d)",
						    (size_t)(off + o), (size_t)len, (size_t)mbuf_len(map->_mbuf),
						    mbuf_next(map->_mbuf) != NULL);
						break;
					}
				}
			}
#endif
		} else {
			memcpy((uint8_t *)map->_kva + off, bounce, len);
		}
	}
	__asm__ volatile("mfence" ::: "memory");
}

int
qwz_bus_dmamem_alloc_range(bus_dma_tag_t t, bus_size_t size, bus_size_t align,
    bus_size_t boundary, bus_dma_segment_t *segs, int nsegs, int *rsegs, int flags,
    bus_addr_t low, bus_addr_t high)
{
	bus_addr_t pa;
	IOBufferMemoryDescriptor *md = qwz_alloc_low(size, align, high, &pa);

	if (md == NULL) {
		QWZLOG("cannot allocate %llu contiguous bytes below 4 GB",
		    (unsigned long long)size);
		return ENOMEM;
	}
	segs[0].ds_addr = pa;
	segs[0].ds_len = size;
	segs[0]._ds_md = md;
	*rsegs = 1;
	return 0;
}

int
qwz_bus_dmamem_alloc(bus_dma_tag_t t, bus_size_t size, bus_size_t align, bus_size_t boundary,
    bus_dma_segment_t *segs, int nsegs, int *rsegs, int flags)
{
	/* The device only reaches 32-bit addresses (see the header). */
	return qwz_bus_dmamem_alloc_range(t, size, align, boundary, segs, nsegs, rsegs,
	    flags, 0, 0xffffffffULL);
}

void
qwz_bus_dmamem_free(bus_dma_tag_t t, bus_dma_segment_t *segs, int nsegs)
{
	if (segs[0]._ds_md != NULL) {
		qwz_free_low(segs[0]._ds_md);
		segs[0]._ds_md = NULL;
	}
}

int
qwz_bus_dmamem_map(bus_dma_tag_t t, bus_dma_segment_t *segs, int nsegs, size_t size,
    caddr_t *kvap, int flags)
{
	struct qwz_dmamem_ent *e;

	if (segs[0]._ds_md == NULL)
		return EINVAL;
	e = (struct qwz_dmamem_ent *)qwz_malloc(sizeof(*e), M_DEVBUF, M_WAITOK | M_ZERO);
	if (e == NULL)
		return ENOMEM;
	e->kva = (uint8_t *)segs[0]._ds_md->getBytesNoCopy();
	e->size = size;
	e->pa = segs[0].ds_addr;
	IOLockLock(qwz_dma_lock);
	e->next = qwz_dmamem_list;
	qwz_dmamem_list = e;
	IOLockUnlock(qwz_dma_lock);
	*kvap = (caddr_t)e->kva;
	return 0;
}

void
qwz_bus_dmamem_unmap(bus_dma_tag_t t, caddr_t kva, size_t size)
{
	struct qwz_dmamem_ent **pp, *e;

	IOLockLock(qwz_dma_lock);
	for (pp = &qwz_dmamem_list; (e = *pp) != NULL; pp = &e->next) {
		if (e->kva == (uint8_t *)kva) {
			*pp = e->next;
			break;
		}
	}
	IOLockUnlock(qwz_dma_lock);
	if (e != NULL)
		qwz_free(e, M_DEVBUF, sizeof(*e));
}

/* ---- mbuf(9) ------------------------------------------------------------ */

static void
qwz_extfree(caddr_t buf, u_int size, caddr_t arg)
{
	IOFree(buf, size);
}

mbuf_t
qwz_mclgetl(mbuf_t m, int how, size_t len)
{
	caddr_t buf;

	if (m == NULL)
		return NULL;
	if (len <= MCLBYTES) {
		errno_t e = mbuf_mclget((mbuf_how_t)how, MBUF_TYPE_DATA, &m);
		if (e != 0 || !(mbuf_flags(m) & MBUF_EXT))
			QWZLOG("mbuf_mclget(%zu) failed: %d", len, e);
		return m;
	}
	/*
	 * Bigger than a 2 KB cluster: attach our own buffer so the data stays
	 * contiguous for mtod(), whatever the size (MHI uses 64 KB).
	 */
	buf = (caddr_t)IOMalloc(len);
	if (buf == NULL)
		return m;
	errno_t e = mbuf_attachcluster((mbuf_how_t)how, MBUF_TYPE_DATA, &m, buf,
	    qwz_extfree, len, NULL);
	if (e != 0) {
		QWZLOG("mbuf_attachcluster(%zu) failed: %d", len, e);
		IOFree(buf, len);
	}
	return m;
}

/* Only the append case (skip == packet length) is used: making room for a MIC. */
mbuf_t
m_makespace(mbuf_t m0, int skip, int hlen, int *off)
{
	mbuf_t m, last = m0, n;

	if (skip != (int)mbuf_pkthdr_len(m0)) {
		QWZLOG("m_makespace: insertion at %d not supported", skip);
		return NULL;
	}
	for (m = m0; m != NULL; m = mbuf_next(m))
		last = m;
	if (mbuf_trailingspace(last) >= (size_t)hlen) {
		*off = (int)mbuf_len(last);
		mbuf_setlen(last, mbuf_len(last) + hlen);
		n = last;
	} else {
		if (mbuf_get(MBUF_DONTWAIT, MBUF_TYPE_DATA, &n) != 0)
			return NULL;
		mbuf_setlen(n, hlen);
		mbuf_setnext(last, n);
		*off = 0;
	}
	mbuf_pkthdr_setlen(m0, mbuf_pkthdr_len(m0) + hlen);
	return n;
}

int
m_copyback(mbuf_t m, int off, int len, const void *src, int how)
{
	return mbuf_copyback(m, off, len, src, (mbuf_how_t)how);
}

/* ---- firmware / autoconf ----------------------------------------------- */

int
loadfirmware(const char *name, u_char **bufp, size_t *lenp)
{
	int i;

	for (i = 0; i < qwz_fw_count; i++) {
		if (strcmp(qwz_fw_table[i].name, name) != 0)
			continue;
		*bufp = (u_char *)qwz_malloc(qwz_fw_table[i].size, M_DEVBUF, M_WAITOK);
		if (*bufp == NULL)
			return ENOMEM;
		memcpy(*bufp, qwz_fw_table[i].data, qwz_fw_table[i].size);
		*lenp = qwz_fw_table[i].size;
		return 0;
	}
	QWZLOG("firmware %s is not built in", name);
	return ENOENT;
}

static struct device *qwz_mountroot_dev;
static void (*qwz_mountroot_fn)(struct device *);

void
config_mountroot(struct device *dev, void (*fn)(struct device *))
{
	qwz_mountroot_dev = dev;
	qwz_mountroot_fn = fn;
}

void
qwz_compat_run_mountroot(void)
{
	void (*fn)(struct device *) = qwz_mountroot_fn;

	qwz_mountroot_fn = NULL;
	if (fn != NULL)
		fn(qwz_mountroot_dev);
}

/* ---- debug -------------------------------------------------------------- */

static IOService *qwz_controller;

void
qwz_compat_set_controller(IOService *controller)
{
	qwz_controller = controller;
}

void
qwz_compat_publish_country(const char *requested, const char *firmware,
    int pending, int error, uint32_t status, uint32_t flags149, uint32_t frequency149)
{
	if (qwz_controller == nullptr)
		return;
	OSDictionary *d = OSDictionary::withCapacity(7);
	if (d == nullptr)
		return;
	auto number = [d](const char *key, uint32_t value) {
		if (OSNumber *n = OSNumber::withNumber(value, 32)) {
			d->setObject(key, n);
			n->release();
		}
	};
	if (OSString *s = OSString::withCString(requested[0] ? requested : "<unset>")) {
		d->setObject("requested", s); s->release();
	}
	if (OSString *s = OSString::withCString(firmware[0] ? firmware : "<unset>")) {
		d->setObject("firmware", s); s->release();
	}
	number("pending", pending != 0); number("requestError", error);
	number("firmwareStatus", status); number("channel149Flags", flags149);
	number("channel149Frequency", frequency149);
	qwz_controller->setProperty("QwzRegulatory", d);
	d->release();
}

int
qwz_compat_publish_blob(const char *key, const void *buf, size_t len)
{
	OSData *d;

	if (qwz_controller == NULL)
		return ENXIO;
	d = OSData::withBytes(buf, (unsigned int)len);
	if (d == NULL)
		return ENOMEM;
	qwz_controller->setProperty(key, d);
	d->release();
	QWZLOG("published %zu bytes as IORegistry property %s", len, key);
	return 0;
}

void
qwz_hexdump(const char *label, const void *p, size_t len, size_t base)
{
	const uint8_t *b = (const uint8_t *)p;
	char line[16 * 3 + 1];
	size_t i, j;

	QWZLOG("hexdump %s: %zu bytes", label, len);
	for (i = 0; i < len; i += 16) {
		size_t n = len - i < 16 ? len - i : 16;
		for (j = 0; j < n; j++)
			snprintf(line + j * 3, 4, "%02x ", b[i + j]);
		line[n * 3] = 0;
		QWZLOG("  %04zx: %s", base + i, line);
	}
}

/* ---- bring-up checkpoints ------------------------------------------------ */

int qwz_stage_sysctl = -1;	/* debug.qwz.stage: -1 = use the boot-time limit */

static int
qwz_stage_limit(void)
{
	static int limit = -1;

	IORegistryEntry *opts;
	int v;

	if (qwz_stage_sysctl >= 0)
		return qwz_stage_sysctl;
	if (limit >= 0)
		return limit;
	/*
	 * Full bring-up unless qwz_stage=<n> (boot-args; NVRAM writes do not
	 * persist on some boards) limits it. -qwzoff disables the driver.
	 */
	limit = 1000;
	if (PE_parse_boot_argn("qwz_stage", &v, sizeof(v))) {
		limit = v;
	} else if ((opts = IORegistryEntry::fromPath("/options", gIODTPlane)) != NULL) {
		OSObject *o = opts->copyProperty("qwz-stage");
		OSData *d = OSDynamicCast(OSData, o);
		OSString *str = OSDynamicCast(OSString, o);
		if (d != NULL && d->getLength() > 0 && d->getLength() < 8) {
			char buf[8] = { 0 };
			memcpy(buf, d->getBytesNoCopy(), d->getLength());
			limit = (int)strtoul(buf, NULL, 10);
		} else if (str != NULL) {
			limit = (int)strtoul(str->getCStringNoCopy(), NULL, 10);
		}
		OSSafeReleaseNULL(o);
		opts->release();
	}
	QWZLOG("bring-up stage limit: %d", limit);
	return limit;
}

static bool qwz_stage_limited;

int
qwz_stage_hit_limit(void)
{
	return qwz_stage_limited;
}

void
qwz_stage_reset(void)
{
	qwz_stage_limited = false;
}

/* Set once a stage >= 15 was refused and the bring-up was parked (qwz.c). */
static bool qwz_stage_is_parked;

int
qwz_stage_parked(void)
{
	return qwz_stage_is_parked;
}

void
qwz_stage_set_parked(void)
{
	qwz_stage_is_parked = true;
	qwz_stage_limited = true;
}

int
qwz_stage_ok(int n, const char *what)
{
	char stage[160];

	if (n > qwz_stage_limit()) {
		snprintf(stage, sizeof(stage), "%d %s: not run (qwz_stage limit)", n, what);
		qwz_log_stage(stage);
		qwz_stage_limited = true;
		return 0;
	}
	snprintf(stage, sizeof(stage), "%d %s", n, what);
	qwz_log_stage(stage);
	return 1;
}
