/*
 * Host test for qwz/qwz_awdl_cell.c (K5, L2-C1). No kernel, no hackintosh: tests/run-awdl-cell-test.sh builds and runs it with the system clang.
 *
 * The vectors are built here from the AWDL layout the kext already uses (qwz.c tests f[24] == 127, OUI 00:17:f2, f[28] == 8; the engine fills
 * phy_tx_time and target_tx_time at offsets 32 to 39, AirportItlwmV2.cpp): a PSF of 300 bytes (subtype 0), a MIF of 935 bytes (subtype 3) and a
 * unicast QoS data frame. The sizes are the ones seen in the M2a AD-0 session; the bytes are not a raw dump (no ring dump with frame bytes was
 * kept), the addresses are made up.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "qwz_awdl_cell.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static const uint8_t bssid[6] = { 0x00, 0x25, 0x00, 0xff, 0x94, 0x73 };
static const uint8_t peer[6] = { 0x6e, 0x11, 0x22, 0x33, 0x44, 0x55 };
static const uint8_t me[6] = { 0x6e, 0xaa, 0xbb, 0xcc, 0xdd, 0xee };
static const uint8_t self[6] = { 0x02, 0x01, 0x02, 0x03, 0x04, 0x05 };
static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

/* an AWDL action frame: header, category 127, OUI, type 8, version 0x10, subtype, reserved, phy and target tx time, then filler TLV bytes */
static size_t
mk_action(uint8_t *f, size_t len, uint8_t subtype, const uint8_t *a1)
{
	size_t i;

	memset(f, 0, len);
	f[0] = 0xd0;
	memcpy(f + 4, a1, 6);
	memcpy(f + 10, peer, 6);
	memcpy(f + 16, bssid, 6);
	f[22] = 0x40; f[23] = 0x03;
	f[24] = 127; f[25] = 0x00; f[26] = 0x17; f[27] = 0xf2; f[28] = 0x08; f[29] = 0x10; f[30] = subtype; f[31] = 0;
	f[32] = 0x11; f[33] = 0x22; f[34] = 0x33; f[35] = 0x44;
	f[36] = 0x55; f[37] = 0x66; f[38] = 0x77; f[39] = 0x88;
	for (i = 40; i < len; i++)
		f[i] = (uint8_t)(i * 7);
	return len;
}

/* a unicast QoS data frame (type 2, subtype 8) to `a1` in the AWDL BSS with an LLC/SNAP header */
static size_t
mk_data(uint8_t *f, size_t len, const uint8_t *a1, const uint8_t *a3)
{
	size_t i;

	memset(f, 0, len);
	f[0] = 0x88; f[1] = 0x00;
	memcpy(f + 4, a1, 6);
	memcpy(f + 10, peer, 6);
	memcpy(f + 16, a3, 6);
	f[26] = 0xaa; f[27] = 0xaa; f[28] = 0x03; f[32] = 0x08; f[33] = 0x00;
	for (i = 34; i < len; i++)
		f[i] = (uint8_t)(i * 3);
	return len;
}

int
main(void)
{
	uint8_t psf[300], mif[935], uni[180], out[2500], kind;
	uint8_t neg[300];
	struct qwz_montx_hdr h;
	size_t n;

	mk_action(psf, sizeof(psf), 0, bcast);
	mk_action(mif, sizeof(mif), 3, bcast);
	mk_data(uni, sizeof(uni), me, bssid);

	/* classification */
	CHECK(qwz_awdl_cell_classify(psf, sizeof(psf)) == QWZ_AWDL_CELL_ACTION_AWDL);
	CHECK(qwz_awdl_cell_classify(mif, sizeof(mif)) == QWZ_AWDL_CELL_ACTION_AWDL);
	CHECK(qwz_awdl_cell_classify(uni, sizeof(uni)) == QWZ_AWDL_CELL_DATA_AWDL);
	memcpy(neg, psf, sizeof(psf)); neg[0] = 0xe0;
	CHECK(qwz_awdl_cell_classify(neg, sizeof(neg)) == QWZ_AWDL_CELL_ACTION_AWDL);	/* action no-ack */
	memcpy(neg, psf, sizeof(psf)); neg[28] = 0x07;
	CHECK(qwz_awdl_cell_classify(neg, sizeof(neg)) == QWZ_AWDL_CELL_OTHER);		/* wrong OUI type */
	memcpy(neg, psf, sizeof(psf)); neg[27] = 0xf3;
	CHECK(qwz_awdl_cell_classify(neg, sizeof(neg)) == QWZ_AWDL_CELL_OTHER);		/* wrong OUI */
	memcpy(neg, psf, sizeof(psf)); neg[24] = 4;
	CHECK(qwz_awdl_cell_classify(neg, sizeof(neg)) == QWZ_AWDL_CELL_OTHER);		/* public action, not vendor */
	CHECK(qwz_awdl_cell_classify(psf, 31) == QWZ_AWDL_CELL_OTHER);			/* short */
	CHECK(qwz_awdl_cell_classify(psf, 32) == QWZ_AWDL_CELL_ACTION_AWDL);
	memcpy(neg, psf, sizeof(psf)); neg[0] = 0x80;
	CHECK(qwz_awdl_cell_classify(neg, sizeof(neg)) == QWZ_AWDL_CELL_OTHER);		/* a beacon */
	memcpy(neg, psf, sizeof(psf)); neg[0] = 0xd1;
	CHECK(qwz_awdl_cell_classify(neg, sizeof(neg)) == QWZ_AWDL_CELL_OTHER);		/* protocol version 1 */
	mk_data(neg, sizeof(neg), me, peer);
	CHECK(qwz_awdl_cell_classify(neg, 180) == QWZ_AWDL_CELL_OTHER);			/* data in another BSS, no awdl0 address given */
	CHECK(qwz_awdl_cell_classify_to(neg, 180, me) == QWZ_AWDL_CELL_DATA_AWDL);		/* ... but to awdl0's own address */
	CHECK(qwz_awdl_cell_classify_to(neg, 180, self) == QWZ_AWDL_CELL_OTHER);
	CHECK(qwz_awdl_cell_classify(uni, 23) == QWZ_AWDL_CELL_OTHER);
	CHECK(qwz_awdl_cell_classify(NULL, 100) == QWZ_AWDL_CELL_OTHER);
	CHECK(qwz_awdl_cell_classify(psf, 0) == QWZ_AWDL_CELL_OTHER);
	memcpy(neg, psf, 30); neg[0] = 0xd4;	/* control frame (ACK) */
	CHECK(qwz_awdl_cell_classify(neg, 30) == QWZ_AWDL_CELL_OTHER);

	/* tx_prepare: PSF (broadcast) */
	memset(out, 0xa5, sizeof(out)); memset(&h, 0x77, sizeof(h)); kind = 9;
	n = qwz_awdl_cell_tx_prepare(out, sizeof(out), psf, sizeof(psf), self, &h, &kind);
	CHECK(n == sizeof(psf));
	CHECK(kind == 1);
	CHECK(h.flags == QWZ_MONTX_F_NO_ACK);
	CHECK(memcmp(out + 10, self, 6) == 0);
	CHECK(memcmp(out + 16, bssid, 6) == 0);
	CHECK(memcmp(out + 32, psf + 32, 4) == 0);			/* phy_tx_time untouched */
	CHECK(memcmp(out + 36, psf + 36, 4) == 0);			/* target_tx_time untouched */
	CHECK(memcmp(out, psf, 10) == 0 && memcmp(out + 22, psf + 22, n - 22) == 0);	/* everything else unchanged */
	CHECK(out[n] == 0xa5);						/* no write past the frame */
	CHECK(qwz_awdl_cell_classify(out, n) == QWZ_AWDL_CELL_ACTION_AWDL);

	/* MIF (broadcast, 935 bytes) */
	n = qwz_awdl_cell_tx_prepare(out, sizeof(out), mif, sizeof(mif), self, &h, &kind);
	CHECK(n == sizeof(mif) && kind == 1 && h.flags == QWZ_MONTX_F_NO_ACK);
	CHECK(memcmp(out + 32, mif + 32, 8) == 0);

	/* a unicast MIF keeps ACK */
	mk_action(neg, 200, 3, peer);
	n = qwz_awdl_cell_tx_prepare(out, sizeof(out), neg, 200, self, &h, &kind);
	CHECK(n == 200 && kind == 1 && h.flags == 0);

	/* unicast data: kind 2, no NO_ACK, A2 and A3 rewritten */
	memset(&h, 0x77, sizeof(h));
	n = qwz_awdl_cell_tx_prepare(out, sizeof(out), uni, sizeof(uni), self, &h, &kind);
	CHECK(n == sizeof(uni));
	CHECK(kind == 2);
	CHECK(h.flags == 0 && h.rate == 0 && h.pad == 0);
	CHECK(memcmp(out + 4, me, 6) == 0);				/* A1 kept */
	CHECK(memcmp(out + 10, self, 6) == 0);
	CHECK(memcmp(out + 16, bssid, 6) == 0);
	CHECK(memcmp(out + 24, uni + 24, n - 24) == 0);

	/* data with another BSSID is forced into the AWDL BSS */
	mk_data(neg, 100, me, peer);
	n = qwz_awdl_cell_tx_prepare(out, sizeof(out), neg, 100, self, &h, &kind);
	CHECK(n == 100 && memcmp(out + 16, bssid, 6) == 0);
	/* a group-addressed data frame is NO_ACK */
	mk_data(neg, 100, bcast, bssid);
	n = qwz_awdl_cell_tx_prepare(out, sizeof(out), neg, 100, self, &h, &kind);
	CHECK(n == 100 && kind == 2 && h.flags == QWZ_MONTX_F_NO_ACK);

	/* in place */
	memcpy(out, psf, sizeof(psf));
	n = qwz_awdl_cell_tx_prepare(out, sizeof(out), out, sizeof(psf), self, &h, &kind);
	CHECK(n == sizeof(psf) && memcmp(out + 10, self, 6) == 0 && memcmp(out + 32, psf + 32, 8) == 0);

	/* rejects */
	CHECK(qwz_awdl_cell_tx_prepare(out, sizeof(out), psf, 23, self, &h, &kind) == 0);		/* management shorter than 24 */
	CHECK(qwz_awdl_cell_tx_prepare(out, sizeof(out), psf, 24, self, &h, &kind) == 24);		/* exactly 24 */
	CHECK(qwz_awdl_cell_tx_prepare(out, sizeof(out), uni, 25, self, &h, &kind) == 0);		/* data shorter than 26 */
	CHECK(qwz_awdl_cell_tx_prepare(out, sizeof(out), uni, 26, self, &h, &kind) == 26);
	CHECK(qwz_awdl_cell_tx_prepare(out, sizeof(out), psf, 1, self, &h, &kind) == 0);
	{
		static uint8_t big[QWZ_MONTX_MAXFRAME + 1], bigout[QWZ_MONTX_MAXFRAME + 1];

		mk_action(big, sizeof(big), 3, bcast);
		CHECK(qwz_awdl_cell_tx_prepare(bigout, sizeof(bigout), big, QWZ_MONTX_MAXFRAME + 1, self, &h, &kind) == 0);	/* too long */
		CHECK(qwz_awdl_cell_tx_prepare(bigout, sizeof(bigout), big, QWZ_MONTX_MAXFRAME, self, &h, &kind) == QWZ_MONTX_MAXFRAME);
	}
	CHECK(qwz_awdl_cell_tx_prepare(out, 299, psf, sizeof(psf), self, &h, &kind) == 0);		/* out too small */
	memcpy(neg, psf, sizeof(psf)); neg[0] = 0xd4;
	CHECK(qwz_awdl_cell_tx_prepare(out, sizeof(out), neg, 40, self, &h, &kind) == 0);		/* control frame */
	CHECK(qwz_awdl_cell_tx_prepare(NULL, sizeof(out), psf, sizeof(psf), self, &h, &kind) == 0);
	CHECK(qwz_awdl_cell_tx_prepare(out, sizeof(out), NULL, sizeof(psf), self, &h, &kind) == 0);
	CHECK(qwz_awdl_cell_tx_prepare(out, sizeof(out), psf, sizeof(psf), NULL, &h, &kind) == 0);

	printf("%d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
