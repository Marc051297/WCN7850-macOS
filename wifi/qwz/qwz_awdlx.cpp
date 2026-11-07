/*
 * qwz_awdlx.cpp - the AWDL evidence layer of the M2a build (docs/research-airdrop-2026-11-06.md, A.3 I1 to I10 and
 * 3.5 Y0). See qwz_awdlx.h for the contract: LOG ONLY, nothing here changes a frame, a decision or a timer.
 *
 * Every ring line is built in a buffer of at most 352 bytes and printed with one qwz_log call (qwz_vlog cuts a line
 * at 383 bytes, the head "[ssss.mmm tid] " included), and every site that can fire in a loop has a limiter
 * (qwz_log_ratelimit) or a per-boot budget. Nothing here sleeps, allocates, takes a lock or touches the BAR, so it is
 * safe in every state of the chip (DEAD, MMIO block, powered down).
 *
 * Copyright (c) 2026. GPL-2.0-or-later (see LICENSE).
 */
#include "qwz_compat.h"
#include "qwz_log.h"
#include "qwz_awdlx.h"

struct qwz_awdlx qwz_awdlx;

#define QX_TU_NS	1024000ULL
#define QX_INC(f)	((void)__atomic_fetch_add(&(f), 1u, __ATOMIC_RELAXED))
#define QX_LD(f)	__atomic_load_n(&(f), __ATOMIC_RELAXED)
#define QX_ST(f, v)	__atomic_store_n(&(f), (v), __ATOMIC_RELAXED)
#define QX_FNV0		2166136261u
#define QX_LINE		352	/* one ring line's text; qwz_vlog keeps 383 bytes including its own head */

static const char *const qx_cls_name[QX_NCLS] = { "psf", "mif", "uc", "mc", "oth" };
static const char *const qx_err_name[QX_NERR] = { "nospc", "nobufs", "noent", "notconn", "cancel", "inval", "nxio", "other" };

/* ---- small helpers ------------------------------------------------------------------------------------------- */

static uint32_t
qx_now_ms(void)
{
	return (uint32_t)(getnsecuptime() / 1000000ULL);
}

static uint32_t
qx_fnv(const uint8_t *p, size_t n, uint32_t h)
{
	while (n--) {
		h ^= *p++;
		h *= 16777619u;
	}
	return h;
}

/* bounded appender, as qwz_hl_add in qwz_health.cpp */
static size_t qx_add(char *buf, size_t size, size_t off, const char *fmt, ...) __attribute__((format(__printf__, 4, 5)));

static size_t
qx_add(char *buf, size_t size, size_t off, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (off + 1 >= size)
		return off;
	va_start(ap, fmt);
	n = vsnprintf(buf + off, size - off, fmt, ap);
	va_end(ap);
	if (n < 0)
		return off;
	if ((size_t)n >= size - off)
		return size - 1;
	return off + (size_t)n;
}

/*
 * Every ring line that is not built by qx_add goes through here: it is formatted into a QX_LINE (352-byte) buffer first, so
 * that no line can exceed 351 characters plus the ring's head of about 20 (qwz_vlog keeps 383 bytes and loses the newline of
 * a longer line). A cut line keeps its newline. The bound holds for any argument values, not only for the counters seen so far.
 */
static void qx_log(const char *fmt, ...) __attribute__((format(__printf__, 1, 2)));

static void
qx_log(const char *fmt, ...)
{
	char buf[QX_LINE];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if ((size_t)n >= sizeof(buf)) {
		buf[sizeof(buf) - 2] = '\n';
		buf[sizeof(buf) - 1] = '\0';
	}
	qwz_log("%s", buf);
}

/* "xx xx xx" for n bytes (no trailing space); returns the length */
static size_t
qx_hex(char *out, size_t outlen, const uint8_t *p, size_t n)
{
	static const char d[] = "0123456789abcdef";
	size_t i, o = 0;

	for (i = 0; i < n && o + 4 < outlen; i++) {
		out[o++] = d[p[i] >> 4];
		out[o++] = d[p[i] & 15];
		out[o++] = ' ';
	}
	if (o > 0)
		o--;
	out[o] = '\0';
	return o;
}

static void
qx_wall(unsigned *h, unsigned *m, unsigned *s)
{
	struct timeval tv;
	uint64_t t;

	microtime(&tv);
	t = (uint64_t)tv.tv_sec % 86400ULL;
	*h = (unsigned)(t / 3600);
	*m = (unsigned)(t / 60 % 60);
	*s = (unsigned)(t % 60);
}

/* days since 1970-01-01 to y-m-d (H. Hinnant's civil_from_days) */
static void
qx_civil(uint64_t secs, int *y, unsigned *mo, unsigned *d)
{
	int64_t z = (int64_t)(secs / 86400ULL) + 719468;
	int64_t era = z / 146097;
	unsigned doe = (unsigned)(z - era * 146097);
	unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	unsigned mp = (5 * doy + 2) / 153;

	*d = doy - (153 * mp + 2) / 5 + 1;
	*mo = mp < 10 ? mp + 3 : mp - 9;
	*y = (int)((int64_t)yoe + era * 400 + (*mo <= 2));
}

/* the AW clock mirror: where "now" sits in the 64-TU period and in the 16-slot sequence */
struct qx_pos {
	uint32_t	aw;		/* sequence number of the window in progress */
	uint8_t		slot, tu, bin, valid;
};

static void
qx_pos_at(uint64_t now, struct qx_pos *p)
{
	struct qwz_awdlx *x = &qwz_awdlx;
	uint64_t next, per, last, el;

	p->valid = 0;
	p->aw = 0;
	p->slot = 0xff;
	p->tu = 0;
	p->bin = QX_NPHASE - 1;
	if (!QX_LD(x->clk_on))
		return;
	next = QX_LD(x->clk_next_ns);
	per = QX_LD(x->clk_period_ns);
	if (per == 0 || next < per)
		return;
	last = next - per;
	el = now >= last ? now - last : 0;
	if (el >= per)
		el = per - 1;
	p->tu = (uint8_t)(el / QX_TU_NS > 63 ? 63 : el / QX_TU_NS);
	p->aw = QX_LD(x->clk_seq) - 4;
	p->slot = (uint8_t)((p->aw >> 2) & 15);
	p->bin = p->tu >> 2;
	p->valid = 1;
}

/*
 * The peer table holds QX_NPEER transmitters. When it is full, a new transmitter replaces the one heard longest ago (counted in
 * peer_full as an eviction), so that the peers being heard now are never hidden by the first eight that were ever heard. The
 * evicted record restarts from zero (its counters, hashes and log budgets).
 */
static struct qwz_awdlx_peer *
qx_peer(const uint8_t *mac, int create)
{
	struct qwz_awdlx *x = &qwz_awdlx;
	struct qwz_awdlx_peer *v = NULL;
	uint32_t now, age, best = 0;
	int i;

	for (i = 0; i < QX_NPEER; i++)
		if (x->peer[i].used && memcmp(x->peer[i].mac, mac, 6) == 0)
			return &x->peer[i];
	if (!create)
		return NULL;
	now = qx_now_ms();
	for (i = 0; i < QX_NPEER; i++) {
		if (!x->peer[i].used) {
			v = &x->peer[i];
			break;
		}
		age = now - x->peer[i].last_ms;
		if (v == NULL || age > best) {
			v = &x->peer[i];
			best = age;
		}
	}
	if (v->used)
		QX_INC(x->peer_full);
	memset(v, 0, sizeof(*v));
	memcpy(v->mac, mac, 6);
	v->used = 1;
	v->first_ms = now;
	v->last_ms = now;
	return v;
}

static unsigned
qx_lat_bucket(uint64_t ns)
{
	return ns < 2000000ULL ? 0 : ns < 5000000ULL ? 1 : ns < 20000000ULL ? 2 : ns < 100000000ULL ? 3 : 4;
}

static unsigned
qx_len_bucket(size_t len)
{
	return len <= 128 ? 0 : len <= 512 ? 1 : len <= 1024 ? 2 : 3;
}

static unsigned
qx_status_col(uint32_t st)
{
	return st < 4 ? st : 4;
}

/*
 * A time cap under the count limiters, for the three lines that can run at the frame rate (a failed completion, a received
 * action, a received data frame): at most `per_sec` lines in any one second, however many frames arrive, so a flood of frames
 * cannot wrap the ring. Called from the interrupt action, under the main gate, only.
 */
static int
qx_rate_ok(int site, uint32_t per_sec)
{
	struct qwz_awdlx *x = &qwz_awdlx;
	uint32_t now = qx_now_ms();

	if (now - x->rate[site].sec_ms >= 1000u) {
		x->rate[site].sec_ms = now;
		x->rate[site].n = 0;
	}
	return x->rate[site].n++ < per_sec;
}

/* ---- the session ------------------------------------------------------------------------------------------- */

static void
qx_start(void)
{
	struct qwz_awdlx *x = &qwz_awdlx;
	struct timeval tv;
	int y;
	unsigned mo, d, h, m, s;

	if (__atomic_exchange_n(&x->started, 1u, __ATOMIC_ACQ_REL) != 0)
		return;
	x->start_ms = qx_now_ms();
	microtime(&tv);
	qx_civil((uint64_t)tv.tv_sec, &y, &mo, &d);
	qx_wall(&h, &m, &s);
	qx_log("qwz: awdlx: start wall=%04d-%02u-%02uT%02u:%02u:%02uZ (UTC; SAST is +2 h). Orders: classes psf/mif/uc/mc/oth, status ok/discard/inspect/noack/other, "
	    "errno nospc/nobufs/noent/notconn/cancel/inval/nxio/other, phase bins 4 TU of the 64-TU period + nc (no clock)\n", y, mo, d, h, m, s);
}

void
qwz_awdlx_link(int up)
{
	struct qwz_awdlx *x = &qwz_awdlx;
	uint32_t was;

	if (up) {
		qx_start();
		QX_ST(x->tcl_left[0], x->tcl_n[0] < 16 ? 16 - x->tcl_n[0] : 0);
		QX_ST(x->tcl_left[1], x->tcl_n[1] < 8 ? 8 - x->tcl_n[1] : 0);
	}
	was = __atomic_exchange_n(&x->link, up ? 1u : 0u, __ATOMIC_ACQ_REL);
	if ((was != 0) != (up != 0))
		qx_log("qwz: awdlx: awdl0 link %s\n", up ? "up" : "down");
}

void
qwz_awdlx_clk(int on, uint64_t next_ns, uint64_t period_ns, uint32_t seq, int self)
{
	struct qwz_awdlx *x = &qwz_awdlx;

	QX_ST(x->clk_next_ns, next_ns);
	QX_ST(x->clk_period_ns, period_ns);
	QX_ST(x->clk_seq, seq);
	QX_ST(x->clk_self, self ? 1u : 0u);
	QX_ST(x->clk_on, on ? 1u : 0u);
}

static int
qx_heard_within(uint32_t ms)
{
	struct qwz_awdlx *x = &qwz_awdlx;
	uint32_t now = qx_now_ms();
	int i, n = 0;

	for (i = 0; i < QX_NPEER; i++)
		if (x->peer[i].used && now - x->peer[i].last_ms < ms)
			n++;
	return n;
}

void
qwz_awdlx_master(const uint8_t *master, int self)
{
	struct qwz_awdlx *x = &qwz_awdlx;
	int changed = (self != 0) != (QX_LD(x->clk_self) != 0) || memcmp(master, x->clk_master, 6) != 0;

	memcpy(x->clk_master, master, 6);
	QX_ST(x->clk_self, self ? 1u : 0u);
	if (!changed)
		return;
	QX_INC(x->flips);
	if (qwz_log_ratelimit(&x->flip_logs, 64, 4))
		qx_log("qwz: awdlx: role %s, top master %02x:%02x:%02x:%02x:%02x:%02x | flips %u, peers heard in 5 s %d, follow samples %u (last %d us), clock samples %u, clock %s\n",
		    self ? "MASTER (free-running)" : "SLAVE (following)", master[0], master[1], master[2], master[3], master[4], master[5],
		    QX_LD(x->flips), qx_heard_within(5000), QX_LD(x->fr_total), x->fr_last_us, QX_LD(x->ce_total), QX_LD(x->clk_on) ? "on" : "off");
}

void
qwz_awdlx_late(int64_t late_ns)
{
	struct qwz_awdlx *x = &qwz_awdlx;
	int64_t us = late_ns / 1000;
	uint32_t n;

	if (us > 1000000)
		us = 1000000;
	if (us < -1000000)
		us = -1000000;
	n = QX_LD(x->late_n);
	x->late_us[n % QX_LATE_N] = (int32_t)us;
	QX_ST(x->late_n, n + 1);
	QX_INC(x->late_total);
}

void
qwz_awdlx_chanseq(const uint32_t *channels, const uint32_t *flags)
{
	static uint32_t last_hash, calls, logs;
	char buf[QX_LINE];
	size_t off;
	uint32_t h = QX_FNV0;
	int i;

	calls++;
	h = qx_fnv((const uint8_t *)channels, 16 * sizeof(uint32_t), h);
	h = qx_fnv((const uint8_t *)flags, 16 * sizeof(uint32_t), h);
	if (calls > 1 && h == last_hash)
		return;
	last_hash = h;
	if (logs >= 32)
		return;
	logs++;
	off = qx_add(buf, sizeof(buf), 0, "slot:ch/flags");
	for (i = 0; i < 16; i++)
		off = qx_add(buf, sizeof(buf), off, " %d:%u/%x", i, channels[i] & 0xffffu, flags[i] & 0xffffu);
	qx_log("qwz: awdlx: engine channel sequence #%u (call %u): %s\n", logs, calls, buf);
}

int
qwz_awdlx_errcls(int err)
{
	switch (err) {
	case ENOSPC:	return QX_E_NOSPC;
	case ENOBUFS:	return QX_E_NOBUFS;
	case ENOENT:	return QX_E_NOENT;
	case ENOTCONN:	return QX_E_NOTCONN;
	case ECANCELED:	return QX_E_CANCEL;
	case EINVAL:	return QX_E_INVAL;
	case ENXIO:	return QX_E_NXIO;
	}
	return QX_E_OTHER;
}

/* ---- TX bookkeeping (AirportItlwm side) ----------------------------------------------------------------------- */

void
qwz_awdlx_tx_result(int cls, int ret)
{
	struct qwz_awdlx *x = &qwz_awdlx;

	if (cls < 0 || cls >= QX_NCLS)
		cls = QX_OTH;
	QX_INC(x->sub[cls]);
	if (ret == 0)
		QX_INC(x->acc[cls]);
	else
		QX_INC(x->ref[cls][qwz_awdlx_errcls(ret)]);
}

void
qwz_awdlx_drop_pre(int why)
{
	QX_INC(qwz_awdlx.o_pre[why < 0 || why > 2 ? 2 : why]);
}

void
qwz_awdlx_bridge(int result)
{
	struct qwz_awdlx *x = &qwz_awdlx;

	QX_INC(x->b_in);
	if (result == ENOSPC)
		QX_INC(x->b_full);
	else if (result != 0)
		QX_INC(x->b_bad);
}

/* ---- the management-TX lane (qwz side): submit, completion, flush -------------------------------------------- */

static int
qx_dst_index(const uint8_t *mac)
{
	struct qwz_awdlx *x = &qwz_awdlx;
	int i;

	for (i = 0; i < QX_NDST; i++) {
		if (x->dst[i].used && memcmp(x->dst[i].mac, mac, 6) == 0)
			return i;
		if (!x->dst[i].used) {
			memcpy(x->dst[i].mac, mac, 6);
			x->dst[i].used = 1;
			return i;
		}
	}
	return 0xff;
}

void
qwz_awdlx_sub(int slot, const uint8_t *frame, size_t len)
{
	struct qwz_awdlx *x = &qwz_awdlx;
	struct qwz_awdlx_slot *s;
	struct qx_pos pos;
	uint64_t now = getnsecuptime();
	int cls;

	if (slot < 0 || slot >= QX_SLOTS)
		return;
	s = &x->slot[slot];
	if (s->valid)				/* a purge freed the mbuf without a completion: it counts as flushed */
		QX_INC(x->flush[s->cls < QX_NCLS ? s->cls : QX_OTH]);
	cls = qwz_awdlx_class(frame, len);
	qx_pos_at(now, &pos);
	s->t_ns = now;
	s->len = (uint32_t)len;
	s->cls = (uint8_t)cls;
	s->lb = (uint8_t)qx_len_bucket(len);
	s->ph = pos.bin;
	s->sl = pos.slot;
	s->dst = 0xff;
	s->age = 3;
	if (cls == QX_UC && len >= 10) {
		struct qwz_awdlx_peer *p = qx_peer(frame + 4, 0);

		s->dst = (uint8_t)qx_dst_index(frame + 4);
		if (p != NULL) {
			uint32_t age = qx_now_ms() - p->last_ms;

			s->age = age < 30 ? 0 : age < 100 ? 1 : age < 1000 ? 2 : 3;
		}
	}
	s->valid = 1;
	x->slotmask |= 1ULL << slot;
}

void
qwz_awdlx_unsub(int slot)
{
	struct qwz_awdlx *x = &qwz_awdlx;

	if (slot < 0 || slot >= QX_SLOTS)
		return;
	x->slot[slot].valid = 0;
	x->slotmask &= ~(1ULL << slot);
}

void
qwz_awdlx_cmpl_tlv(uint32_t tlv_len, const uint8_t *ev)
{
	struct qwz_awdlx *x = &qwz_awdlx;
	uint32_t extra[9];
	size_t n, i, off = 0;
	char buf[QX_LINE];

	if (tlv_len >= 24)
		memcpy(&x->ctlv_x0, ev + 20, 4);		/* the dword at +20 of the latest completion, for the D line */
	if (tlv_len == x->ctlv_len && x->ctlv_nlog != 0)
		return;
	x->ctlv_len = tlv_len;
	buf[0] = '\0';				/* also for 20 < tlv_len < 24, where no whole dword follows (refuter nice_to_fix) */
	if (tlv_len > 20) {
		n = (tlv_len - 20) / 4;
		if (n > 9)
			n = 9;
		for (i = 0; i < n; i++)
			memcpy(&extra[i], ev + 20 + 4 * i, 4);
		for (i = 0; i < n; i++)
			off = qx_add(buf, sizeof(buf), off, "%s%08x", i ? " " : "", extra[i]);
	}
	if (x->ctlv_nlog++ < 4)
		qx_log("qwz: awdlx: completion TLV is %u bytes (the port reads 20: desc_id status pdev_id ppdu_id ack_rssi)%s%s\n", tlv_len,
		    tlv_len >= 24 ? "; dwords from +20: " : "", buf);
}

/* the completion of one slot: matrix, latency, ack_rssi, per destination, phase, slot, age */
void
qwz_awdlx_cmpl(int slot, uint32_t status, int32_t ack_rssi)
{
	struct qwz_awdlx *x = &qwz_awdlx;
	struct qwz_awdlx_slot *s;
	uint64_t lat;
	unsigned col = qx_status_col(status), ok = status == 0, c;
	int log = 0;

	if (slot < 0 || slot >= QX_SLOTS || !x->slot[slot].valid) {
		QX_INC(x->orphan);
		return;
	}
	s = &x->slot[slot];
	s->valid = 0;
	x->slotmask &= ~(1ULL << slot);
	c = s->cls < QX_NCLS ? s->cls : QX_OTH;
	lat = getnsecuptime() - s->t_ns;
	QX_INC(x->cm[c][col]);
	QX_INC(x->cl[c][s->lb][ok ? 0 : 1]);
	QX_INC(x->lat[c][qx_lat_bucket(lat)]);
	if (c == QX_UC) {
		unsigned col2 = ok ? 0 : 1;

		QX_INC(x->ph[s->ph < QX_NPHASE ? s->ph : QX_NPHASE - 1][col2]);
		QX_INC(x->sl[s->sl < 16 ? s->sl : 0][col2]);
		QX_INC(x->ag[s->age < 4 ? s->age : 3][col2]);
		if (s->dst < QX_NDST) {
			if (ok)
				QX_INC(x->dst[s->dst].ok);
			else
				QX_INC(x->dst[s->dst].fail);
		} else
			QX_INC(x->dst_other[col2]);
		if (ok) {
			QX_INC(x->ack_n);
			if (ack_rssi == 0)
				QX_INC(x->ack_zero);
			x->ack_sum += ack_rssi;
			if (x->ack_n == 1 || ack_rssi < x->ack_min)
				x->ack_min = ack_rssi;
		}
	}
	/* the per-completion line: every failure for a while, the first few successes of each class */
	if (!ok)
		log = qwz_log_ratelimit(&x->nfail_log, 24, 8) && (QX_LD(x->nfail_log) <= 24 || qx_rate_ok(QX_RATE_CMPL, 2));
	else if (QX_LD(x->nok_log[c]) < (c == QX_UC ? 4u : 2u)) {
		QX_INC(x->nok_log[c]);
		log = 1;
	}
	if (log)
		qx_log("qwz: awdlx: cmpl slot=%d cls=%s st=%u lat=%lluus len=%u ack=%d phase=%d slot16=%d age=%d dst=%d\n", slot, qx_cls_name[c],
		    status, (unsigned long long)(lat / 1000), s->len, ack_rssi, s->ph < QX_NPHASE - 1 ? s->ph * 4 : -1,
		    s->sl < 16 ? s->sl : -1, c == QX_UC ? s->age : -1, s->dst < QX_NDST ? s->dst : -1);
}

/*
 * A teardown (qwz_fw_epoch_reset) clears the masks of the slots the firmware will never complete: they leave the
 * books as "flushed", per class, so completions + outstanding + flushed keep equal to accepted.
 */
void
qwz_awdlx_flush(void)
{
	struct qwz_awdlx *x = &qwz_awdlx;
	int i;

	for (i = 0; i < QX_SLOTS; i++)
		if (x->slot[i].valid) {
			QX_INC(x->flush[x->slot[i].cls < QX_NCLS ? x->slot[i].cls : QX_OTH]);
			x->slot[i].valid = 0;
		}
	x->slotmask = 0;
}

/* ---- receive (qwz side) ------------------------------------------------------------------------------------------- */

static void
qx_rx_common(struct qwz_awdlx_peer *p, const uint8_t *f, int mgmt, int ours, int grp, const struct qx_pos *pos, int rssi)
{
	struct qwz_awdlx *x = &qwz_awdlx;
	uint16_t sc = (uint16_t)(f[22] | (f[23] << 8));

	if (f[1] & 0x08) {
		QX_INC(mgmt ? x->rx_m_retry : x->rx_d_retry);
		if (p != NULL)
			p->retry++;
	}
	if (p != NULL) {
		p->last_ms = qx_now_ms();
		p->rssi = (int8_t)rssi;
		if (pos->valid)
			p->hist[pos->slot & 15]++;
		if (p->have_sc && p->last_sc == sc) {
			QX_INC(mgmt ? x->rx_m_dup : x->rx_d_dup);
			p->dup++;
		}
		p->last_sc = sc;
		p->have_sc = 1;
		if (mgmt)
			p->rx_m++;
		else if (ours)
			p->rx_du++;
		else if (grp)
			p->rx_dm++;
	}
}

void
qwz_awdlx_rx_mgmt(const uint8_t *f, size_t len, int rssi, int ours)
{
	struct qwz_awdlx *x = &qwz_awdlx;
	struct qwz_awdlx_peer *p;
	struct qx_pos pos;
	uint64_t now;
	int grp, st;
	char hex[3 * 16 + 4];
	uint32_t n;

	if (len < 31 || f[28] != 0x08)
		return;
	qx_start();
	now = getnsecuptime();
	grp = (f[4] & 1) != 0;
	st = f[30];
	QX_INC(x->rx_m[ours ? 0 : grp ? 1 : 2]);
	QX_INC(x->rx_m_st[st == 0 ? 0 : st == 3 ? 1 : 2]);
	p = qx_peer(f + 10, 1);
	qx_pos_at(now, &pos);
	qx_rx_common(p, f, 1, ours, grp, &pos, rssi);
	n = __atomic_add_fetch(&x->rx_log_m, 1u, __ATOMIC_RELAXED);
	if (n <= 16 || ((n % 128) == 0 && qx_rate_ok(QX_RATE_RXA, 1)))
		qx_log("qwz: awdlx: rx act #%u ta=%02x:%02x:%02x:%02x:%02x:%02x st=%d len=%zu a1=%s retry=%d rssi=%d aw=%u slot=%d tu=%d\n", n,
		    f[10], f[11], f[12], f[13], f[14], f[15], st, len, ours ? "us" : grp ? "grp" : "other", (f[1] & 8) != 0, rssi,
		    pos.aw, pos.valid ? pos.slot : -1, pos.valid ? pos.tu : -1);
	if (ours) {					/* a unicast action to us: rare, and the one at 2271.924 of M0 was never decoded */
		n = __atomic_add_fetch(&x->rx_log_u, 1u, __ATOMIC_RELAXED);
		if (n <= 8 || (n % 16) == 0) {
			qx_hex(hex, sizeof(hex), f + 24, len - 24 < 16 ? len - 24 : 16);
			qx_log("qwz: awdlx: rx act to us #%u ta=%02x:%02x:%02x:%02x:%02x:%02x st=%d len=%zu body16: %s\n", n,
			    f[10], f[11], f[12], f[13], f[14], f[15], st, len, hex);
		}
	}
}

void
qwz_awdlx_rx_data(const uint8_t *f, size_t len, int rssi, int ours)
{
	struct qwz_awdlx *x = &qwz_awdlx;
	struct qwz_awdlx_peer *p;
	struct qx_pos pos;
	int grp;
	uint32_t n;

	if (len < 24)
		return;
	qx_start();
	grp = (f[4] & 1) != 0;
	QX_INC(x->rx_d[ours ? 0 : grp ? 1 : 2]);
	p = qx_peer(f + 10, 1);
	qx_pos_at(getnsecuptime(), &pos);
	qx_rx_common(p, f, 0, ours, grp, &pos, rssi);
	n = __atomic_add_fetch(&x->rx_log_d, 1u, __ATOMIC_RELAXED);
	if (n <= 16 || ((n % 64) == 0 && qx_rate_ok(QX_RATE_RXD, 1)))
		qx_log("qwz: awdlx: rx data #%u ta=%02x:%02x:%02x:%02x:%02x:%02x a1=%s fc=%02x%02x seq=%u len=%zu rssi=%d aw=%u slot=%d tu=%d\n", n,
		    f[10], f[11], f[12], f[13], f[14], f[15], ours ? "us" : grp ? "grp" : "other", f[0], f[1],
		    (unsigned)((f[22] | (f[23] << 8)) >> 4), len, rssi, pos.aw, pos.valid ? pos.slot : -1, pos.valid ? pos.tu : -1);
}

/* ---- I4 and I7: TLV evidence and the clock, from the frame bytes ---------------------------------------------- */

static void
qx_hex_line(const char *what, const char *dir, const uint8_t *ta, int st, const uint8_t *tlv, size_t tlen)
{
	char hex[3 * 88 + 4];
	size_t n = tlen < 88 ? tlen : 88;

	qx_hex(hex, sizeof(hex), tlv, n);
	qx_log("qwz: awdlx: %s %s ta=%02x:%02x:%02x:%02x:%02x:%02x st=%d %zuB%s: %s\n", what, dir, ta[0], ta[1], ta[2], ta[3], ta[4], ta[5], st, tlen,
	    tlen > n ? "+" : "", hex);
}

/* whole-frame dump, 96 bytes a line, at most 512 bytes: the first PSF and MIF of each direction */
static void
qx_frame_dump(const char *dir, const char *kind, const uint8_t *f, size_t len)
{
	char hex[3 * 96 + 4];
	size_t off, n;

	for (off = 0; off < len && off < 512; off += 96) {
		n = len - off < 96 ? len - off : 96;
		qx_hex(hex, sizeof(hex), f + off, n);
		qx_log("qwz: awdlx: frame %s %s %zuB +%03zu: %s\n", dir, kind, len, off, hex);
	}
}

static uint32_t
qx_tlv4_hash(const uint8_t *tlv, size_t tlen)
{
	uint8_t tmp[128];

	if (tlen > sizeof(tmp))
		tlen = sizeof(tmp);
	memcpy(tmp, tlv, tlen);
	if (tlen > 3 + 30) {			/* tx_counter (value 1..2) and aw_seq_number (29..30) change in every frame */
		tmp[3 + 1] = tmp[3 + 2] = 0;
		tmp[3 + 29] = tmp[3 + 30] = 0;
	}
	return qx_fnv(tmp, tlen, QX_FNV0);
}

static void
qx_clock_sample(const uint8_t *f, const uint8_t *v, size_t vlen, uint64_t now)
{
	struct qwz_awdlx *x = &qwz_awdlx;
	struct qx_pos pos;
	uint64_t next, per;
	uint32_t seq, pseq, pseq16;
	int32_t k;
	int64_t err;
	uint32_t tc, a;
	int from_master;

	if (vlen < 33 || !QX_LD(x->clk_on))
		return;
	next = QX_LD(x->clk_next_ns);
	per = QX_LD(x->clk_period_ns);
	seq = QX_LD(x->clk_seq);
	if (per == 0)
		return;
	tc = (uint32_t)(v[1] | (v[2] << 8));
	pseq16 = (uint32_t)(v[29] | (v[30] << 8));
	pseq = (((seq & 0xffff0000u) | pseq16) & ~3u) + 4;		/* the next window, as awdlAwFollow numbers it */
	if ((int32_t)(pseq - seq) < -0x8000)
		pseq += 0x10000;
	else if ((int32_t)(pseq - seq) > 0x8000)
		pseq -= 0x10000;
	k = (int32_t)(pseq - seq) / 4;					/* windows apart: one window is 4 in sequence units */
	err = (int64_t)(now + (uint64_t)tc * QX_TU_NS) - (int64_t)next - (int64_t)k * (int64_t)per;
	a = (uint32_t)((err < 0 ? -err : err) / 1000);			/* microseconds */
	QX_INC(x->ce_n);			/* the interval counters (restarted at every K line) and the cumulative ones */
	QX_INC(x->ce_total);
	QX_INC(x->ce[a < 1024 ? 0 : a < 3072 ? 1 : a < 10240 ? 2 : a < 32768 ? 3 : 4]);
	if ((int32_t)a > x->ce_maxabs_us)
		x->ce_maxabs_us = (int32_t)(a > 0x7fffffff ? 0x7fffffff : a);
	from_master = !QX_LD(x->clk_self) && memcmp(f + 10, x->clk_master, 6) == 0;
	if (from_master) {
		QX_INC(x->fr_n);
		QX_INC(x->fr_total);
		x->fr_last_us = (int32_t)(err / 1000);
		if ((int32_t)a > x->fr_maxabs_us)
			x->fr_maxabs_us = (int32_t)(a > 0x7fffffff ? 0x7fffffff : a);
	}
	qx_pos_at(now, &pos);
	if (qwz_log_ratelimit(&x->clock_logs, 12, 256))		/* a cumulative count: the first 12 of the boot, then every 256th (ce_n restarts at every K line and cannot be the limiter) */
		qx_log("qwz: awdlx: clock ta=%02x:%02x:%02x:%02x:%02x:%02x peer aw_seq %u next_in %u TU (its slot %u, %d TU in) | ours: next in %lld TU, k=%d, slot %d, %d TU in | error %lld us%s\n",
		    f[10], f[11], f[12], f[13], f[14], f[15], pseq16, tc, (pseq16 >> 2) & 15, tc <= 64 ? 64 - (int)tc : -1,
		    (long long)(((int64_t)next - (int64_t)now) / (int64_t)QX_TU_NS), k, pos.valid ? pos.slot : -1, pos.valid ? pos.tu : -1,
		    (long long)(err / 1000), from_master ? " (the master we follow)" : "");
}

/*
 * One AWDL action frame, whole 802.11 frame, final bytes: dir 0 = received (call it BEFORE the clock follow, so the
 * error is the clock's prediction error), 1 = about to be transmitted. Returns 1 when (direction, transmitter,
 * subtype) shows a TLV signature or length not seen before.
 */
int
qwz_awdlx_tlvs(int dir, const uint8_t *f, size_t len)
{
	struct qwz_awdlx *x = &qwz_awdlx;
	const uint8_t *t4 = NULL, *t12 = NULL, *t0c = NULL;
	size_t l4 = 0, l12 = 0, l0c = 0, off, l;
	uint32_t sig = QX_FNV0, n2 = 0, n10 = 0, nt = 0, h;
	char types[3 * 24 + 1];
	size_t tn = 0;
	struct qwz_awdlx_key *k = NULL;
	struct qwz_awdlx_peer *p = NULL;
	const char *dn = dir ? "tx" : "rx";
	int st, i, changed = 0, arpa = 0, kind;

	if (len < 40 || f[24] != 0x7f || f[25] != 0x00 || f[26] != 0x17 || f[27] != 0xf2 || f[28] != 0x08)
		return 0;
	st = f[30];
	types[0] = '\0';
	for (off = 40; off + 3 <= len; off += 3 + l) {
		uint8_t t = f[off];

		l = (size_t)(f[off + 1] | (f[off + 2] << 8));
		if (off + 3 + l > len)
			break;
		sig = qx_fnv(&t, 1, sig);
		nt++;
		if (nt <= 24)
			tn = qx_add(types, sizeof(types), tn, "%s%02x", tn ? " " : "", t);
		if (t == 4 && t4 == NULL) { t4 = f + off; l4 = 3 + l; }
		else if (t == 0x12 && t12 == NULL) { t12 = f + off; l12 = 3 + l; }
		else if (t == 0x0c && t0c == NULL) { t0c = f + off; l0c = 3 + l; }
		else if (t == 0x02) {
			n2++;
			if (dir == 1 && st == 3 && l >= 4) {		/* "arpa" as text inside a service record of the MIF we transmit */
				size_t j;

				for (j = 0; j + 4 <= l; j++)
					if (f[off + 3 + j] == 'a' && f[off + 4 + j] == 'r' && f[off + 5 + j] == 'p' && f[off + 6 + j] == 'a')
						arpa = 1;
			}
		}
		else if (t == 0x10)
			n10++;
	}
	kind = st == 0 ? 0 : st == 3 ? 1 : -1;
	if (dir == 1 && st == 3) {
		x->mi_len = (uint32_t)len;
		x->mi_t2 = n2;
		x->mi_t10 = n10;
		x->mi_arpa = (uint32_t)arpa;
	}
	/* the TLV-change log */
	for (i = 0; i < QX_NKEY; i++)
		if (x->key[i].used && x->key[i].dir == dir && x->key[i].st == st && memcmp(x->key[i].ta, f + 10, 6) == 0) {
			k = &x->key[i];
			break;
		}
	if (k == NULL) {
		for (i = 0; i < QX_NKEY && x->key[i].used; i++)
			;
		if (i == QX_NKEY) {
			i = (int)(x->key_next++ % QX_NKEY);
			x->key_evict++;
		}
		k = &x->key[i];
		memset(k, 0, sizeof(*k));
		k->used = 1;
		k->dir = (uint8_t)dir;
		k->st = (uint8_t)st;
		memcpy(k->ta, f + 10, 6);
		changed = 1;
	} else if (k->sig != sig || k->len != (uint16_t)len)
		changed = 1;
	if (changed) {
		uint32_t osig = k->sig;
		uint16_t olen = k->len;
		int first = k->nlog == 0, budget = k->nlog < 8;

		k->sig = sig;
		k->len = (uint16_t)len;
		if (k->nlog < 255)
			k->nlog++;
		if (budget && qwz_log_ratelimit(&x->key_logs, 96, 8)) {
			if (first)
				qx_log("qwz: awdlx: tlvs %s ta=%02x:%02x:%02x:%02x:%02x:%02x st=%d len=%zu n=%u sig=%08x types=%s (first)\n", dn,
				    f[10], f[11], f[12], f[13], f[14], f[15], st, len, nt, sig, types);
			else
				qx_log("qwz: awdlx: tlvs %s ta=%02x:%02x:%02x:%02x:%02x:%02x st=%d len=%zu n=%u sig=%08x types=%s (was len=%u sig=%08x)\n", dn,
				    f[10], f[11], f[12], f[13], f[14], f[15], st, len, nt, sig, types, olen, osig);
		}
	}
	/* hex of TLV 4 (clock fields masked for the comparison), 0x12 and 0x0c: first sight and every change */
	if (dir == 1) {
		if (t4 != NULL) {
			h = qx_tlv4_hash(t4, l4);
			if (h != x->tx4) {
				if (x->tx4 != 0)
					QX_INC(x->tx_flips);
				x->tx4 = h;
				if (qwz_log_ratelimit(&x->tx_hex_logs, 64, 4))
					qx_hex_line("tlv04", dn, f + 10, st, t4, l4);
			}
		}
		if (t12 != NULL) {
			h = qx_fnv(t12, l12, QX_FNV0);
			if (h != x->tx12) {
				if (x->tx12 != 0)
					QX_INC(x->tx_flips);
				x->tx12 = h;
				if (qwz_log_ratelimit(&x->tx_hex_logs, 64, 4))
					qx_hex_line("tlv12", dn, f + 10, st, t12, l12);
			}
		}
	} else {
		p = qx_peer(f + 10, 1);
		if (p != NULL) {
			if (t4 != NULL && (h = qx_tlv4_hash(t4, l4)) != p->h4) {
				p->h4 = h;
				if (p->n4 < 8 && (p->n4++, qwz_log_ratelimit(&x->hex_logs, 128, 4)))
					qx_hex_line("tlv04", dn, f + 10, st, t4, l4);
			}
			if (t12 != NULL && (h = qx_fnv(t12, l12, QX_FNV0)) != p->h12) {
				p->h12 = h;
				if (p->n12 < 8 && (p->n12++, qwz_log_ratelimit(&x->hex_logs, 128, 4)))
					qx_hex_line("tlv12", dn, f + 10, st, t12, l12);
			}
			if (t0c != NULL && (h = qx_fnv(t0c, l0c, QX_FNV0)) != p->h0c) {
				p->h0c = h;
				if (p->n0c < 8 && (p->n0c++, qwz_log_ratelimit(&x->hex_logs, 128, 4)))
					qx_hex_line("tlv0c", dn, f + 10, st, t0c, l0c);
			}
		}
		if (t4 != NULL && l4 >= 3 + 33)
			qx_clock_sample(f, t4 + 3, l4 - 3, getnsecuptime());
	}
	/*
	 * One whole PSF and one whole MIF per direction. The first frame we transmit may predate the engine's TLV 4 and TLV 0x12 (it then
	 * lacks them), so a transmitted PSF or MIF is dumped once more when it first carries both (bits 4 to 7 of frame_dumped).
	 */
	if (kind >= 0) {
		uint32_t bit = 1u << (dir * 2 + kind), bit2 = bit << 4, dumped = QX_LD(x->frame_dumped);
		int full = dir == 1 && t4 != NULL && t12 != NULL;

		if (!(dumped & bit) || (full && !(dumped & bit2))) {
			__atomic_fetch_or(&x->frame_dumped, full ? (bit | bit2) : bit, __ATOMIC_RELAXED);
			qx_frame_dump(dn, kind ? "mif" : "psf", f, len);
		}
	}
	return changed;
}

/* ---- I6: the L3/L4 tracer (awdl0's Ethernet frames, both directions) ---------------------------------------- */

static unsigned
qx_be16(const uint8_t *p)
{
	return (unsigned)(p[0] << 8 | p[1]);
}

static uint32_t
qx_be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* the first three labels of the DNS name at p, printable characters only; returns the 16-bit field that follows the name, or 0 */
static unsigned
qx_dns_name(const uint8_t *p, size_t avail, char *out, size_t outlen)
{
	size_t off = 0, o = 0;
	int labels = 0;

	out[0] = '\0';
	while (off < avail && labels < 16) {
		unsigned l = p[off];
		size_t i;

		if (l == 0) {
			off++;
			return off + 2 <= avail ? qx_be16(p + off) : 0;
		}
		if (l & 0xc0) {				/* a compression pointer: the name goes on elsewhere */
			if (labels < 3)
				o = qx_add(out, outlen, o, "%s<ptr>", o ? "." : "");
			return 0;
		}
		if (off + 1 + l > avail)
			return 0;
		if (labels < 3) {
			if (o)
				o = qx_add(out, outlen, o, ".");
			for (i = 0; i < l && i < 24 && o + 2 < outlen; i++)
				out[o++] = (p[off + 1 + i] >= 0x20 && p[off + 1 + i] < 0x7f) ? (char)p[off + 1 + i] : '.';
			out[o] = '\0';
		}
		off += 1 + l;
		labels++;
	}
	return 0;
}

void
qwz_awdlx_trace(int dir, const uint8_t *dst, const uint8_t *src, unsigned ethertype, const uint8_t *p, size_t len)
{
	struct qwz_awdlx *x = &qwz_awdlx;
	const uint8_t *peer = dir ? dst : src;
	const char *dn = dir ? "tx" : "rx";
	unsigned nh;
	size_t off = 40;
	int ext;

	dir = dir ? 1 : 0;
	if (ethertype != 0x86dd) {
		QX_INC(x->l4_other[dir]);
		if (qwz_log_ratelimit(&x->l4_logs_eth, 8, 1u << 30))
			qx_log("qwz: awdlx: l3 %s ethertype %04x len=%zu peer=%02x:%02x:%02x:%02x:%02x:%02x (first 8 non-IPv6 frames)\n", dn, ethertype, len,
			    peer[0], peer[1], peer[2], peer[3], peer[4], peer[5]);
		return;
	}
	if (len < 40 || (p[0] >> 4) != 6) {
		QX_INC(x->l4_other[dir]);
		return;
	}
	nh = p[6];
	for (ext = 0; ext < 3 && (nh == 0 || nh == 43 || nh == 60); ext++) {	/* hop-by-hop, routing, destination options */
		size_t elen;

		if (off + 8 > len)
			return;
		elen = ((size_t)p[off + 1] + 1) * 8;
		if (off + elen > len)
			return;
		nh = p[off];
		off += elen;
	}
	if (nh == 6 && len >= off + 20) {
		unsigned fl = p[off + 13], sp = qx_be16(p + off), dp = qx_be16(p + off + 2), doff = (p[off + 12] >> 4) * 4u;
		size_t plen = qx_be16(p + 4) + 40 > off + doff ? qx_be16(p + 4) + 40 - off - doff : 0;
		char s[8];
		size_t n = 0;

		QX_INC(x->l4_tcp[dir]);
		if (fl & 2) QX_INC(x->l4_syn[dir]);
		if (fl & 4) QX_INC(x->l4_rst[dir]);
		if (fl & 1) QX_INC(x->l4_fin[dir]);
		if (fl & 7) {				/* control segments: SYN, FIN, RST */
			if (fl & 2) s[n++] = 'S';
			if (fl & 1) s[n++] = 'F';
			if (fl & 4) s[n++] = 'R';
			if (fl & 0x10) s[n++] = 'A';
			s[n] = '\0';
			if (qwz_log_ratelimit(&x->l4_logs_tcp, 200, 1u << 30))
				qx_log("qwz: awdlx: tcp %s peer=%02x:%02x:%02x ports %u>%u fl=%s seq=%u ack=%u len=%zu win=%u\n", dn, peer[3], peer[4], peer[5],
				    sp, dp, s, qx_be32(p + off + 4), qx_be32(p + off + 8), plen, qx_be16(p + off + 14));
		}
		if ((sp == 8770 || dp == 8770) && plen > 0 && doff >= 20 && len >= off + doff + 1 && !QX_LD(x->l4_8770[dir])) {
			char hex[3 * 40 + 4];
			size_t avail = len - off - doff, nb = plen < avail ? plen : avail;

			QX_ST(x->l4_8770[dir], 1u);
			qx_hex(hex, sizeof(hex), p + off + doff, nb < 40 ? nb : 40);
			qx_log("qwz: awdlx: tcp %s port 8770 first payload %zuB (first %zu shown): %s\n", dn, plen, nb < 40 ? nb : (size_t)40, hex);
		}
		return;
	}
	if (nh == 58 && len >= off + 4) {
		unsigned type = p[off], code = p[off + 1];

		QX_INC(x->l4_icmp[dir]);
		if ((type >= 130 && type <= 136) || type == 143) {	/* MLD query/report/done, RS, RA, NS, NA, MLDv2 report */
			if (qwz_log_ratelimit(&x->l4_logs_icmp, 128, 16)) {
				static const char *const nm[] = { "mld-query", "mld-report", "mld-done", "rs", "ra", "ns", "na" };
				const char *tn = type == 143 ? "mldv2-report" : nm[type - 130];
				if ((type == 135 || type == 136) && len >= off + 24)
					qx_log("qwz: awdlx: icmp6 %s %s code=%u peer=%02x:%02x:%02x target ..%02x%02x:%02x%02x:%02x%02x len=%zu\n", dn, tn, code, peer[3], peer[4], peer[5],
					    p[off + 18], p[off + 19], p[off + 20], p[off + 21], p[off + 22], p[off + 23], len);
				else
					qx_log("qwz: awdlx: icmp6 %s %s code=%u peer=%02x:%02x:%02x len=%zu\n", dn, tn, code, peer[3], peer[4], peer[5], len);
			}
		}
		return;
	}
	if (nh == 17 && len >= off + 8) {
		unsigned sp = qx_be16(p + off), dp = qx_be16(p + off + 2);

		if (sp == 5353 || dp == 5353) {
			QX_INC(x->l4_mdns[dir]);
			if (qwz_log_ratelimit(&x->l4_logs_mdns, 128, 16)) {
				char nm[64];
				unsigned qt = 0, id = 0, fl = 0, qd = 0, an = 0, ns = 0, ar = 0;

				nm[0] = '\0';
				if (len >= off + 8 + 12) {
					const uint8_t *d = p + off + 8;

					id = qx_be16(d); fl = qx_be16(d + 2); qd = qx_be16(d + 4); an = qx_be16(d + 6); ns = qx_be16(d + 8); ar = qx_be16(d + 10);
					qt = qx_dns_name(d + 12, len - off - 8 - 12, nm, sizeof(nm));
				}
				qx_log("qwz: awdlx: mdns %s peer=%02x:%02x:%02x id=%u fl=%04x qd=%u an=%u ns=%u ar=%u first name \"%s\" type=%u len=%zu\n", dn,
				    peer[3], peer[4], peer[5], id, fl, qd, an, ns, ar, nm, qt, len);
			}
			return;
		}
	}
	QX_INC(x->l4_other[dir]);
}

/* ---- I9: TCL descriptors, log only ---------------------------------------------------------------------------- */

void
qwz_awdlx_tcl(int awdl, int slot, unsigned vdev, unsigned bank, const uint32_t *info, unsigned dlen, unsigned tid, const uint8_t *wh, size_t wlen)
{
	struct qwz_awdlx *x = &qwz_awdlx;
	static const uint8_t zero[10] = { 0 };
	int w = awdl ? 0 : 1;
	uint32_t left = QX_LD(x->tcl_left[w]), n;

	if (wh == NULL || wlen < 10)
		wh = zero;

	if (left == 0)
		return;
	QX_ST(x->tcl_left[w], left - 1);
	n = ++x->tcl_n[w];
	qx_log("qwz: awdlx: tcl %s #%u slot=%d vdev=%u bank=%u info0=%08x info1=%08x info2=%08x info3=%08x info4=%08x len=%u tid=%u fc=%02x%02x da=%02x:%02x:%02x:%02x:%02x:%02x\n",
	    awdl ? "awdl" : "infra", n, slot, vdev, bank, info[0], info[1], info[2], info[3], info[4], dlen, tid, wh[0], wh[1],
	    wh[4], wh[5], wh[6], wh[7], wh[8], wh[9]);
}

/* ---- I8: the summary (qwz_health_log, under the main gate) --------------------------------------------------- */

static unsigned
qx_cm_total(int c)
{
	struct qwz_awdlx *x = &qwz_awdlx;
	unsigned t = 0;
	int s;

	for (s = 0; s < QX_NST; s++)
		t += QX_LD(x->cm[c][s]);
	return t;
}

static unsigned
qx_ref_total(int c)
{
	struct qwz_awdlx *x = &qwz_awdlx;
	unsigned t = 0;
	int e;

	for (e = 0; e < QX_NERR; e++)
		t += QX_LD(x->ref[c][e]);
	return t;
}

static unsigned
qx_out(int c)
{
	struct qwz_awdlx *x = &qwz_awdlx;
	unsigned t = 0;
	int i;

	for (i = 0; i < QX_SLOTS; i++)
		if (x->slot[i].valid && x->slot[i].cls == c)
			t++;
	return t;
}

/* the age of the oldest slot still waiting for its completion (a slot the firmware never completes shows here) */
static unsigned
qx_oldest_ms(void)
{
	struct qwz_awdlx *x = &qwz_awdlx;
	uint64_t now = getnsecuptime(), best = 0;
	int i;

	for (i = 0; i < QX_SLOTS; i++)
		if (x->slot[i].valid && now > x->slot[i].t_ns && now - x->slot[i].t_ns > best)
			best = now - x->slot[i].t_ns;
	return (unsigned)(best / 1000000ULL);
}

/* ascending insertion sort of up to QX_LATE_N values (no qsort to rely on in a kext) */
static void
qx_sort(int32_t *v, uint32_t n)
{
	uint32_t i, j;

	for (i = 1; i < n; i++) {
		int32_t t = v[i];

		for (j = i; j > 0 && v[j - 1] > t; j--)
			v[j] = v[j - 1];
		v[j] = t;
	}
}

static int
qx_clamp(long long v)
{
	return v > 99999 ? 99999 : v < -99999 ? -99999 : (int)v;
}

/*
 * The detail lines: a line prints when something it shows changed and at least 15 s passed since its last print, and
 * in any case after 60 s. The first print of each is immediate. The books (A, E) and the clock (K) print at every
 * health sample (5 s while AWDL is up).
 */
#define QX_DETAIL_MS	15000u

static int
qx_detail_due(int k, uint32_t sig, uint32_t now_ms)
{
	struct qwz_awdlx *x = &qwz_awdlx;
	uint32_t last = x->sum_ms[k];

	if (last != 0 && now_ms - last < QX_DETAIL_MS)
		return 0;
	if (last != 0 && sig == x->sum_sig[k] && now_ms - last < 60000u)
		return 0;
	x->sum_ms[k] = now_ms ? now_ms : 1;
	x->sum_sig[k] = sig;
	return 1;
}

void
qwz_awdlx_health(const struct qwz_awdlx_env *env)
{
	struct qwz_awdlx *x = &qwz_awdlx;
	char buf[QX_LINE];
	size_t off;
	unsigned wh, wm, ws, c, e, i, now = qx_now_ms();
	int link = (int)QX_LD(x->link);
	long long chk, chkx;
	unsigned handed, pre, d_ref = 0, d_cm = 0, d_out = 0, d_fl = 0, d_rq = 0, d_held = 0, d_wd = 0;
	unsigned o_out[QX_NCLS], o_cm[QX_NCLS], o_ref[QX_NCLS];

	if (!link && !env->listening)
		return;
	qx_start();
	qx_wall(&wh, &wm, &ws);
	for (c = 0; c < QX_NCLS; c++) {
		o_out[c] = qx_out((int)c);
		o_cm[c] = qx_cm_total((int)c);
		o_ref[c] = qx_ref_total((int)c);
	}
	handed = QX_LD(x->o_handed);
	pre = QX_LD(x->o_pre[0]) + QX_LD(x->o_pre[1]) + QX_LD(x->o_pre[2]);
	for (c = QX_UC; c <= QX_MC; c++) {
		d_ref += o_ref[c];
		d_cm += o_cm[c];
		d_out += o_out[c];
		d_fl += QX_LD(x->flush[c]);
		d_rq += QX_LD(x->rq[c]);
		d_held += QX_LD(x->held[c]);
		d_wd += QX_LD(x->wdrop[c]);
	}
	/* completions + refusals + drops (+ outstanding + flushed) = frames handed to awdl0; TCL completions count as completions */
	chk = (long long)handed - ((long long)pre + d_ref + d_cm + d_out + d_fl + env->tcl_done + env->tcl_inflight - d_rq + d_held + d_wd);
	chkx = 0;				/* PSF, MIF, other: submits = refusals + completions + outstanding + flushed */
	for (c = 0; c < QX_NCLS; c++)
		if (c != QX_UC && c != QX_MC)
			chkx += (long long)QX_LD(x->sub[c]) - o_ref[c] - o_cm[c] - o_out[c] - QX_LD(x->flush[c]);

	/* A: the books of awdl0's data frames (classes uc/mc), every sample */
	qx_log("qwz: awdlx: A wall=%02u:%02u:%02uZ up=%us link=%d lis=%d q=%d dead=%d probe=%d | handed=%u pre=%u/%u/%u | sub=%u/%u ref=%u/%u cmpl=%u/%u out=%u/%u flush=%u/%u"
	    " | tcl=%u+%u rq=%u held=%u wd=%u orph=%u mask=%u old=%ums | chk=%d\n", wh, wm, ws, (now - x->start_ms) / 1000, link, env->listening, env->quiesce, env->dead,
	    env->probe, handed, QX_LD(x->o_pre[0]), QX_LD(x->o_pre[1]), QX_LD(x->o_pre[2]),
	    QX_LD(x->sub[QX_UC]), QX_LD(x->sub[QX_MC]), o_ref[QX_UC], o_ref[QX_MC], o_cm[QX_UC], o_cm[QX_MC], o_out[QX_UC], o_out[QX_MC],
	    QX_LD(x->flush[QX_UC]), QX_LD(x->flush[QX_MC]), env->tcl_done, env->tcl_inflight, d_rq, d_held, d_wd, QX_LD(x->orphan),
	    (unsigned)__builtin_popcount(env->txmask), qx_oldest_ms(), qx_clamp(chk));

	/* E: the books of the engine's frames (classes psf/mif/oth), every sample */
	qx_log("qwz: awdlx: E wall=%02u:%02u:%02uZ psf/mif/oth sub=%u/%u/%u ref=%u/%u/%u cmpl=%u/%u/%u out=%u/%u/%u flush=%u/%u/%u | bridge in=%u full=%u bad=%u | chkx=%d\n",
	    wh, wm, ws, QX_LD(x->sub[QX_PSF]), QX_LD(x->sub[QX_MIF]), QX_LD(x->sub[QX_OTH]), o_ref[QX_PSF], o_ref[QX_MIF], o_ref[QX_OTH],
	    o_cm[QX_PSF], o_cm[QX_MIF], o_cm[QX_OTH], o_out[QX_PSF], o_out[QX_MIF], o_out[QX_OTH],
	    QX_LD(x->flush[QX_PSF]), QX_LD(x->flush[QX_MIF]), QX_LD(x->flush[QX_OTH]), QX_LD(x->b_in), QX_LD(x->b_full), QX_LD(x->b_bad), qx_clamp(chkx));

	/* K: the clock, every sample; the interval counters restart here */
	{
		static int32_t v[QX_LATE_N];		/* only this function, under the main gate, uses it: not on the stack */
		uint32_t n = QX_LD(x->late_n), tot = n, k, r;
		int32_t p50 = 0, p99 = 0, mx = 0;

		if (n > QX_LATE_N)
			n = QX_LATE_N;
		for (k = 0; k < n; k++)
			v[k] = x->late_us[k];
		qx_sort(v, n);
		if (n) {
			r = (n * 99 + 99) / 100;		/* nearest rank */
			p50 = v[(n - 1) / 2];
			p99 = v[r - 1 < n ? r - 1 : n - 1];
			mx = v[n - 1];
		}
		qx_log("qwz: awdlx: K wall=%02u:%02u:%02uZ clk=%s self=%d | error of every peer frame since the last K (<1,<3,<10,<32,>=32 TU) %u/%u/%u/%u/%u n=%u max=%dus"
		    " | follow n=%u last=%dus max=%dus | aw timer lateness us p50=%d p99=%d max=%d n=%u (total %u) | role flips=%u\n", wh, wm, ws,
		    QX_LD(x->clk_on) ? "on" : "off", (int)QX_LD(x->clk_self), QX_LD(x->ce[0]), QX_LD(x->ce[1]), QX_LD(x->ce[2]), QX_LD(x->ce[3]), QX_LD(x->ce[4]),
		    QX_LD(x->ce_n), x->ce_maxabs_us, QX_LD(x->fr_n), x->fr_last_us, x->fr_maxabs_us, p50, p99, mx, n, tot, QX_LD(x->flips));
		for (k = 0; k < 5; k++)
			QX_ST(x->ce[k], 0u);
		QX_ST(x->ce_n, 0u);
		x->ce_maxabs_us = 0;
		QX_ST(x->fr_n, 0u);
		x->fr_maxabs_us = 0;
		QX_ST(x->late_n, 0u);
	}

	/* R: refusals by errno, non-zero cells only */
	{
		unsigned sig = QX_LD(x->b_in) ^ (QX_LD(x->b_full) << 7) ^ (QX_LD(x->b_bad) << 13);

		for (c = 0; c < QX_NCLS; c++)
			sig = sig * 31 + o_ref[c];
		if (qx_detail_due(1, sig, now)) {
			off = qx_add(buf, sizeof(buf), 0, "qwz: awdlx: R wall=%02u:%02u:%02uZ refusals (class:errno=count)", wh, wm, ws);
			for (c = 0; c < QX_NCLS; c++)
				for (e = 0; e < QX_NERR; e++)
					if (QX_LD(x->ref[c][e]) != 0)
						off = qx_add(buf, sizeof(buf), off, " %s:%s=%u", qx_cls_name[c], qx_err_name[e], QX_LD(x->ref[c][e]));
			qwz_log("%s\n", buf);
		}
	}

	/* C: completions by status */
	{
		unsigned sig = 0;

		for (c = 0; c < QX_NCLS; c++)
			sig = sig * 31 + o_cm[c];
		if (qx_detail_due(2, sig, now)) {
			off = qx_add(buf, sizeof(buf), 0, "qwz: awdlx: C wall=%02u:%02u:%02uZ completions ok/discard/inspect/noack/other", wh, wm, ws);
			for (c = 0; c < QX_NCLS; c++)
				off = qx_add(buf, sizeof(buf), off, " %s=%u/%u/%u/%u/%u", qx_cls_name[c], QX_LD(x->cm[c][0]), QX_LD(x->cm[c][1]),
				    QX_LD(x->cm[c][2]), QX_LD(x->cm[c][3]), QX_LD(x->cm[c][4]));
			qwz_log("%s\n", buf);
		}
	}

	/* D: submit-to-completion time and the acknowledgement RSSI */
	{
		unsigned sig = 0;

		for (c = 0; c < QX_NCLS; c++)
			for (i = 0; i < QX_NLAT; i++)
				sig = sig * 31 + QX_LD(x->lat[c][i]);
		if (qx_detail_due(3, sig, now))
			qx_log("qwz: awdlx: D wall=%02u:%02u:%02uZ latency (<2,<5,<20,<100,>=100 ms) psf+mif=%u/%u/%u/%u/%u uc=%u/%u/%u/%u/%u mc=%u/%u/%u/%u/%u"
			    " | uc ack_rssi n=%u zero=%u min=%d mean=%d | completion TLV %u bytes, dword at +20 %08x\n", wh, wm, ws,
			    QX_LD(x->lat[0][0]) + QX_LD(x->lat[1][0]), QX_LD(x->lat[0][1]) + QX_LD(x->lat[1][1]), QX_LD(x->lat[0][2]) + QX_LD(x->lat[1][2]),
			    QX_LD(x->lat[0][3]) + QX_LD(x->lat[1][3]), QX_LD(x->lat[0][4]) + QX_LD(x->lat[1][4]),
			    QX_LD(x->lat[2][0]), QX_LD(x->lat[2][1]), QX_LD(x->lat[2][2]), QX_LD(x->lat[2][3]), QX_LD(x->lat[2][4]),
			    QX_LD(x->lat[3][0]), QX_LD(x->lat[3][1]), QX_LD(x->lat[3][2]), QX_LD(x->lat[3][3]), QX_LD(x->lat[3][4]),
			    QX_LD(x->ack_n), QX_LD(x->ack_zero), x->ack_n ? x->ack_min : 0, x->ack_n ? (int)(x->ack_sum / (int64_t)x->ack_n) : 0,
			    x->ctlv_len, x->ctlv_x0);
	}

	/* U: unicast completions (ok/fail) by frame length, destination and how long ago the destination was last heard */
	{
		unsigned sig = 0;

		for (i = 0; i < 4; i++)
			sig = sig * 31 + QX_LD(x->ag[i][0]) + QX_LD(x->ag[i][1]);
		if (qx_detail_due(4, sig, now)) {
			off = qx_add(buf, sizeof(buf), 0, "qwz: awdlx: U wall=%02u:%02u:%02uZ uc ok/fail by length <=128 %u/%u <=512 %u/%u <=1024 %u/%u >1024 %u/%u"
			    " | by destination", wh, wm, ws, QX_LD(x->cl[QX_UC][0][0]), QX_LD(x->cl[QX_UC][0][1]), QX_LD(x->cl[QX_UC][1][0]), QX_LD(x->cl[QX_UC][1][1]),
			    QX_LD(x->cl[QX_UC][2][0]), QX_LD(x->cl[QX_UC][2][1]), QX_LD(x->cl[QX_UC][3][0]), QX_LD(x->cl[QX_UC][3][1]));
			for (i = 0; i < QX_NDST; i++)
				if (x->dst[i].used)
					off = qx_add(buf, sizeof(buf), off, " ..%02x:%02x:%02x=%u/%u", x->dst[i].mac[3], x->dst[i].mac[4], x->dst[i].mac[5],
					    QX_LD(x->dst[i].ok), QX_LD(x->dst[i].fail));
			off = qx_add(buf, sizeof(buf), off, " rest=%u/%u | by age of its last frame <30ms %u/%u <100ms %u/%u <1s %u/%u older %u/%u",
			    QX_LD(x->dst_other[0]), QX_LD(x->dst_other[1]), QX_LD(x->ag[0][0]), QX_LD(x->ag[0][1]), QX_LD(x->ag[1][0]), QX_LD(x->ag[1][1]),
			    QX_LD(x->ag[2][0]), QX_LD(x->ag[2][1]), QX_LD(x->ag[3][0]), QX_LD(x->ag[3][1]));
			qwz_log("%s\n", buf);
		}
	}

	/* V and W: unicast ok/fail by the 4-TU phase (V, "nc" = no clock) and by the 16-slot position (W) at submit; non-empty bins only */
	{
		unsigned sig = 0;

		for (i = 0; i < QX_NPHASE; i++)
			sig = sig * 31 + QX_LD(x->ph[i][0]) + QX_LD(x->ph[i][1]);
		if (qx_detail_due(5, sig, now)) {
			off = qx_add(buf, sizeof(buf), 0, "qwz: awdlx: V wall=%02u:%02u:%02uZ uc fail/total by phase (bin*4 TU)", wh, wm, ws);
			for (i = 0; i < QX_NPHASE; i++)
				if (QX_LD(x->ph[i][0]) + QX_LD(x->ph[i][1]) != 0) {
					if (i == QX_NPHASE - 1)
						off = qx_add(buf, sizeof(buf), off, " nc:%u/%u", QX_LD(x->ph[i][1]), QX_LD(x->ph[i][0]) + QX_LD(x->ph[i][1]));
					else
						off = qx_add(buf, sizeof(buf), off, " %u:%u/%u", i * 4, QX_LD(x->ph[i][1]), QX_LD(x->ph[i][0]) + QX_LD(x->ph[i][1]));
				}
			qwz_log("%s\n", buf);
			off = qx_add(buf, sizeof(buf), 0, "qwz: awdlx: W wall=%02u:%02u:%02uZ uc fail/total by slot (0-15)", wh, wm, ws);
			for (i = 0; i < 16; i++)
				if (QX_LD(x->sl[i][0]) + QX_LD(x->sl[i][1]) != 0)
					off = qx_add(buf, sizeof(buf), off, " %u:%u/%u", i, QX_LD(x->sl[i][1]), QX_LD(x->sl[i][0]) + QX_LD(x->sl[i][1]));
			qwz_log("%s\n", buf);
		}
	}

	/* X: the receive split */
	{
		unsigned sig = QX_LD(x->rx_m[0]) + QX_LD(x->rx_m[1]) + QX_LD(x->rx_m[2]) + QX_LD(x->rx_d[0]) * 3 + QX_LD(x->rx_d[1]) * 5 + QX_LD(x->rx_d[2]) * 7;

		if (qx_detail_due(6, sig, now))
			qx_log("qwz: awdlx: X wall=%02u:%02u:%02uZ rx mgmt actions to_us=%u grp=%u oth=%u psf=%u mif=%u ost=%u retry=%u dup=%u"
			    " | rx data to_us=%u grp=%u oth=%u retry=%u dup=%u | peers=%d evicted=%u\n", wh, wm, ws,
			    QX_LD(x->rx_m[0]), QX_LD(x->rx_m[1]), QX_LD(x->rx_m[2]), QX_LD(x->rx_m_st[0]), QX_LD(x->rx_m_st[1]), QX_LD(x->rx_m_st[2]),
			    QX_LD(x->rx_m_retry), QX_LD(x->rx_m_dup), QX_LD(x->rx_d[0]), QX_LD(x->rx_d[1]), QX_LD(x->rx_d[2]), QX_LD(x->rx_d_retry),
			    QX_LD(x->rx_d_dup), qx_heard_within(0xffffffffu), QX_LD(x->peer_full));
	}

	/* L: the L3/L4 tracer counters (rx/tx) */
	{
		unsigned sig = QX_LD(x->l4_tcp[0]) + QX_LD(x->l4_tcp[1]) + QX_LD(x->l4_icmp[0]) + QX_LD(x->l4_icmp[1]) + QX_LD(x->l4_mdns[0]) + QX_LD(x->l4_mdns[1]) +
		    QX_LD(x->l4_other[0]) + QX_LD(x->l4_other[1]);

		if (qx_detail_due(7, sig, now))
			qx_log("qwz: awdlx: L wall=%02u:%02u:%02uZ l4 rx/tx tcp=%u/%u syn=%u/%u rst=%u/%u fin=%u/%u icmp6=%u/%u mdns=%u/%u other=%u/%u\n", wh, wm, ws,
			    QX_LD(x->l4_tcp[0]), QX_LD(x->l4_tcp[1]), QX_LD(x->l4_syn[0]), QX_LD(x->l4_syn[1]), QX_LD(x->l4_rst[0]), QX_LD(x->l4_rst[1]),
			    QX_LD(x->l4_fin[0]), QX_LD(x->l4_fin[1]), QX_LD(x->l4_icmp[0]), QX_LD(x->l4_icmp[1]), QX_LD(x->l4_mdns[0]), QX_LD(x->l4_mdns[1]),
			    QX_LD(x->l4_other[0]), QX_LD(x->l4_other[1]));
	}

	/* T: what we transmit (hashes of the clock-masked TLV 4 and of TLV 0x12) and the last MIF */
	{
		unsigned sig = QX_LD(x->tx4) ^ (QX_LD(x->tx12) << 1) ^ QX_LD(x->mi_len) ^ (QX_LD(x->tx_flips) << 9);

		if (qx_detail_due(0, sig, now))
			qx_log("qwz: awdlx: T wall=%02u:%02u:%02uZ transmitted tlv4=%08x tlv12=%08x flips=%u | last MIF len=%u tlv2=%u tlv10=%u arpa=%c | keys evicted=%u\n", wh, wm, ws,
			    QX_LD(x->tx4), QX_LD(x->tx12), QX_LD(x->tx_flips), QX_LD(x->mi_len), QX_LD(x->mi_t2), QX_LD(x->mi_t10), QX_LD(x->mi_arpa) ? 'y' : 'n',
			    QX_LD(x->key_evict));
	}

	/* P: one line per peer, at most every 15 s and only while its counters move */
	for (i = 0; i < QX_NPEER; i++) {
		struct qwz_awdlx_peer *p = &x->peer[i];
		uint32_t sig;

		if (!p->used)
			continue;
		sig = p->rx_m + p->rx_du + p->rx_dm + p->dup + p->retry;
		if (p->pr_ms != 0 && (now - p->pr_ms < QX_DETAIL_MS || (sig == p->pr_sig && now - p->pr_ms < 60000u)))
			continue;
		p->pr_ms = now ? now : 1;
		p->pr_sig = sig;
		off = qx_add(buf, sizeof(buf), 0, "qwz: awdlx: P wall=%02u:%02u:%02uZ peer %02x:%02x:%02x:%02x:%02x:%02x act=%u data_to_us=%u data_grp=%u dup=%u retry=%u age=%ums rssi=%d slots",
		    wh, wm, ws, p->mac[0], p->mac[1], p->mac[2], p->mac[3], p->mac[4], p->mac[5], p->rx_m, p->rx_du, p->rx_dm, p->dup, p->retry,
		    now - p->last_ms, p->rssi);
		for (e = 0; e < 16; e++)
			off = qx_add(buf, sizeof(buf), off, " %u", p->hist[e]);
		qwz_log("%s\n", buf);
	}
}
