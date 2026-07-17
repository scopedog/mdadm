/* raidkm-dcl.c — raidkm declustered-parity create-time support.
 *
 * Map core + rebuild-distribution scoring + acceptance search, ported from
 * the reference simulator (md-kmec tools/declustered-sim.c) — the function
 * bodies must stay IDENTICAL to the simulator and to the kernel's
 * km/raid_km_dcl.h so that seed -> permutation-set generation is
 * bit-reproducible across all three.  tools/raidkm-test-declustered-create.sh
 * pins mdadm's accepted seed against an independent simulator run.
 */
#include "mdadm.h"
#include "xmalloc.h"
#include "raidkm-dcl.h"
#include <math.h>

/* crc32 from mdadm's bundled crc32.c.  NOTE: unlike zlib, this copy has
 * the standard pre/post inversions COMMENTED OUT — it is the raw table
 * loop, so the caller must seed with ~0 and xor the result with ~0 to get
 * the standard CRC-32 (== the kernel's crc32_le(~0,..)^~0 and the
 * simulator's crc32_buf). */
extern unsigned long crc32(unsigned long crc, const unsigned char *buf,
			    unsigned int len);
#define rkdcl_crc32(buf, len) \
	((uint32_t)(crc32(0xffffffffUL, (const unsigned char *)(buf), (len)) \
		    ^ 0xffffffffUL))

/* ---- map core (verbatim from declustered-sim.c KERNEL-CORE) -------------- */

static inline uint64_t dcl_splitmix64(uint64_t *state)
{
	uint64_t z = (*state += 0x9e3779b97f4a7c15ULL);

	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return z ^ (z >> 31);
}

static void dcl_gen_base_perm(uint32_t n, uint64_t seed, uint32_t *perm)
{
	uint32_t i, j, tmp;
	uint64_t st = seed;

	for (i = 0; i < n; i++)
		perm[i] = i;
	for (i = n - 1; i > 0; i--) {
		j = (uint32_t)(dcl_splitmix64(&st) % (uint64_t)(i + 1));
		tmp = perm[i]; perm[i] = perm[j]; perm[j] = tmp;
	}
}

struct dcl_geom {
	uint32_t N, g, m, k, s, ngroups, nbase;
	uint64_t seed;
	uint32_t *base, *ibase;
};

static inline uint64_t dcl_seed_for_base(uint64_t seed, uint32_t b)
{
	uint64_t st = seed + 0x100000001b3ULL * (uint64_t)(b + 1);

	return dcl_splitmix64(&st);
}

static void dcl_geom_tables(struct dcl_geom *ge)
{
	uint32_t b, c;

	for (b = 0; b < ge->nbase; b++) {
		dcl_gen_base_perm(ge->N, dcl_seed_for_base(ge->seed, b),
				  ge->base + (size_t)b * ge->N);
		for (c = 0; c < ge->N; c++)
			ge->ibase[(size_t)b * ge->N +
				  ge->base[(size_t)b * ge->N + c]] = c;
	}
}

static inline uint32_t dcl_disk(const struct dcl_geom *ge, uint64_t row,
				uint32_t lcol)
{
	uint32_t pidx = (uint32_t)(row % ((uint64_t)ge->nbase * ge->N));
	uint32_t b = pidx / ge->N, t = pidx % ge->N;

	return (ge->base[(size_t)b * ge->N + lcol] + t) % ge->N;
}

static inline uint32_t dcl_lcol(const struct dcl_geom *ge, uint64_t row,
				uint32_t disk)
{
	uint32_t pidx = (uint32_t)(row % ((uint64_t)ge->nbase * ge->N));
	uint32_t b = pidx / ge->N, t = pidx % ge->N;

	return ge->ibase[(size_t)b * ge->N + (disk + ge->N - t) % ge->N];
}

#define DCL_ROLE_DATA	0
#define DCL_ROLE_PARITY	1
#define DCL_ROLE_SPARE	2

static inline uint32_t dcl_role(const struct dcl_geom *ge, uint32_t lcol,
				uint32_t *group, uint32_t *slot)
{
	if (lcol < ge->ngroups * ge->g) {
		*group = lcol / ge->g;
		*slot  = lcol % ge->g;
		return *slot < ge->k ? DCL_ROLE_DATA : DCL_ROLE_PARITY;
	}
	*group = 0;
	*slot  = lcol - ge->ngroups * ge->g;
	return DCL_ROLE_SPARE;
}

/* ---- P2 rebuild-distribution metric (verbatim from the simulator) -------- */

struct p2_metrics {
	double read_cv, write_cv, speedup;
	uint64_t rebuilt;
	uint64_t max_read, max_write, max_combined;
};

static void measure_p2(const struct dcl_geom *ge, uint32_t X,
		       struct p2_metrics *pm)
{
	uint64_t period = (uint64_t)ge->nbase * ge->N;
	uint64_t *reads = xcalloc(ge->N, sizeof(*reads));
	uint64_t *writes = xcalloc(ge->N, sizeof(*writes));
	uint64_t row, rebuilt = 0;
	uint32_t Y, group, slot, role, j;
	double rsum = 0, wsum = 0, rss = 0, wss = 0, rmean, wmean;
	uint32_t nsurv = ge->N - 1;

	for (row = 0; row < period; row++) {
		uint32_t lcol = dcl_lcol(ge, row, X);

		role = dcl_role(ge, lcol, &group, &slot);
		if (role == DCL_ROLE_SPARE)
			continue;
		for (j = 0; j < ge->g; j++) {
			uint32_t d = dcl_disk(ge, row, group * ge->g + j);

			if (d != X)
				reads[d]++;
		}
		writes[dcl_disk(ge, row, ge->ngroups * ge->g + 0)]++;
		rebuilt++;
	}

	pm->rebuilt = rebuilt;
	pm->max_read = pm->max_write = pm->max_combined = 0;
	for (Y = 0; Y < ge->N; Y++) {
		if (Y == X)
			continue;
		rsum += reads[Y]; wsum += writes[Y];
		if (reads[Y] > pm->max_read)   pm->max_read = reads[Y];
		if (writes[Y] > pm->max_write) pm->max_write = writes[Y];
		if (reads[Y] + writes[Y] > pm->max_combined)
			pm->max_combined = reads[Y] + writes[Y];
	}
	rmean = rsum / nsurv; wmean = wsum / nsurv;
	for (Y = 0; Y < ge->N; Y++) {
		if (Y == X)
			continue;
		rss += (reads[Y] - rmean) * (reads[Y] - rmean);
		wss += (writes[Y] - wmean) * (writes[Y] - wmean);
	}
	pm->read_cv  = rmean > 0 ? sqrt(rss / (nsurv - 1)) / rmean : 0.0;
	pm->write_cv = wmean > 0 ? sqrt(wss / (nsurv - 1)) / wmean : 0.0;
	pm->speedup  = pm->max_combined ?
			(double)rebuilt / pm->max_combined : 0.0;
	free(reads); free(writes);
}

/* ---- public API ----------------------------------------------------------- */

unsigned int rkdcl_default_spares(unsigned int N, unsigned int g)
{
	unsigned int s = N % g;

	return s ? s : g;
}

int rkdcl_validate_geometry(unsigned int N, unsigned int g, unsigned int m,
			    unsigned int s, unsigned int *ngroups)
{
	if (g <= m || g - m < 1) {
		pr_err("declustered group width %u must exceed parity count %u (k = g - m >= 1)\n",
		       g, m);
		return -1;
	}
	if (s < 1) {
		pr_err("declustered layout needs at least one spare column per row (--spare-columns)\n");
		return -1;
	}
	if (N < g + s) {
		pr_err("pool of %u disks cannot hold one group of %u plus %u spare column(s)\n",
		       N, g, s);
		return -1;
	}
	if ((N - s) % g) {
		pr_err("declustered geometry violates (N - s) %% g == 0: (%u - %u) %% %u = %u\n",
		       N, s, g, (N - s) % g);
		pr_err("for N=%u, g=%u the smallest legal spare-column count is %u (s must be N mod g, plus any multiple of g)\n",
		       N, g, rkdcl_default_spares(N, g));
		return -1;
	}
	*ngroups = (N - s) / g;
	return 0;
}

int rkdcl_accept_seed(unsigned int N, unsigned int g, unsigned int m,
		      unsigned int s, unsigned int nbase,
		      uint64_t seed0, unsigned int tries, uint64_t *seed_out)
{
	struct dcl_geom ge;
	struct p2_metrics pm, best_pm;
	uint64_t best_seed = seed0;
	double best_score = 1e300;
	unsigned int i, ngroups;

	if (rkdcl_validate_geometry(N, g, m, s, &ngroups))
		return -1;

	memset(&ge, 0, sizeof(ge));
	ge.N = N; ge.g = g; ge.m = m; ge.k = g - m;
	ge.s = s; ge.ngroups = ngroups; ge.nbase = nbase;
	ge.base  = xcalloc((size_t)nbase * N, sizeof(uint32_t));
	ge.ibase = xcalloc((size_t)nbase * N, sizeof(uint32_t));

	memset(&best_pm, 0, sizeof(best_pm));
	for (i = 0; i < tries; i++) {
		double score;

		ge.seed = seed0 + i;
		dcl_geom_tables(&ge);
		measure_p2(&ge, 0, &pm);
		/* score: bottleneck first (maximise speedup), CV as tiebreak
		 * — MUST match the simulator's accept_search exactly */
		score = 1.0 / (pm.speedup > 0 ? pm.speedup : 1e-9)
			+ 0.01 * (pm.read_cv + pm.write_cv);
		if (score < best_score) {
			best_score = score;
			best_seed = ge.seed;
			best_pm = pm;
		}
	}
	free(ge.base); free(ge.ibase);

	printf("mdadm: declustered acceptance search: seed 0x%016llx (%u tries), single-failure rebuild ~%.2fx parallel (ceiling %.2fx), read_cv %.3f\n",
	       (unsigned long long)best_seed, tries, best_pm.speedup,
	       (double)(N - 1) / g, best_pm.read_cv);
	*seed_out = best_seed;
	return 0;
}

void rkdcl_build_sb(void *buf, unsigned int N, unsigned int g,
		    unsigned int m, unsigned int s, unsigned int nbase,
		    uint64_t seed)
{
	struct rkdcl_sb *sb = buf;

	memset(buf, 0, RKDCL_SB_BYTES);
	memcpy(sb->magic, RKDCL_MAGIC, 8);
	/* every multi-byte field is stored explicitly little-endian */
	sb->version	= __cpu_to_le32(RKDCL_SB_VERSION);
	sb->pool_disks	= __cpu_to_le32(N);
	sb->group_width	= __cpu_to_le32(g);
	sb->parity	= __cpu_to_le32(m);
	sb->spare_cols	= __cpu_to_le32(s);
	sb->ngroups	= __cpu_to_le32((N - s) / g);
	sb->nbase	= __cpu_to_le32(nbase);
	sb->seed	= __cpu_to_le64(seed);
	sb->flags	= 0;
	sb->hdr_crc	= 0;
	sb->hdr_crc	= __cpu_to_le32(rkdcl_crc32(buf, RKDCL_SB_BYTES));
}

/* Parse + validate an on-disk rkdcl metadata block: magic/version/crc, then
 * cross-check the geometry against the caller's (from the SB layout word).
 * The inverse of rkdcl_build_sb.  Returns 0 and sets the nbase/seed
 * out-params, -1 if the block is not a valid rkdcl block for this geometry. */
int rkdcl_parse_sb(const void *buf, unsigned int N, unsigned int g,
		   unsigned int m, unsigned int s,
		   unsigned int *nbase, uint64_t *seed)
{
	const struct rkdcl_sb *sb = buf;
	unsigned char tmp[RKDCL_SB_BYTES];

	if (memcmp(sb->magic, RKDCL_MAGIC, 8) != 0 ||
	    __le32_to_cpu(sb->version) < RKDCL_SB_VERSION ||
	    __le32_to_cpu(sb->version) > RKDCL_SB_VERSION3)
		return -1;
	/* crc covers the whole block with hdr_crc zeroed */
	memcpy(tmp, buf, RKDCL_SB_BYTES);
	((struct rkdcl_sb *)tmp)->hdr_crc = 0;
	if (rkdcl_crc32(tmp, RKDCL_SB_BYTES) != __le32_to_cpu(sb->hdr_crc))
		return -1;
	if (__le32_to_cpu(sb->pool_disks) != N ||
	    __le32_to_cpu(sb->group_width) != g ||
	    __le32_to_cpu(sb->parity) != m ||
	    __le32_to_cpu(sb->spare_cols) != s ||
	    __le32_to_cpu(sb->ngroups) != (N - s) / g)
		return -1;
	if (!__le32_to_cpu(sb->nbase) || !__le64_to_cpu(sb->seed))
		return -1;
	*nbase = __le32_to_cpu(sb->nbase);
	*seed = __le64_to_cpu(sb->seed);
	return 0;
}
