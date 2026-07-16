/* raidkm-dcl.h — raidkm declustered-parity create-time support (mdadm side).
 *
 * See md-kmec notes/declustered-parity-design.md.  mdadm owns the acceptance
 * search (float scoring is userspace-only): at --create it picks the
 * permutation seed, packs the geometry into the layout word
 * (m | RAIDKM_LAYOUT_DCL | g<<16 | s<<24) and writes one per-member on-disk
 * metadata block carrying what the layout word cannot (nbase, the 64-bit
 * seed; later the Phase-3 spare-assignment table).  The kernel regenerates
 * the identical permutation set from the seed (km/raid_km_dcl.h) — the map
 * core here must stay bit-identical with tools/declustered-sim.c (the
 * reference) and the kernel header; tools/raidkm-test-declustered-create.sh
 * pins mdadm's accepted seed against the simulator's.
 */
#ifndef RAIDKM_DCL_H
#define RAIDKM_DCL_H

#include <stdint.h>

/* On-disk declustered metadata block: 4 KiB at the start of the reserved
 * tail chunk (data_offset + data_size), one per member, all identical at
 * create.  Every multi-byte field is stored explicitly little-endian
 * (rkdcl_build_sb uses __cpu_to_le*); the kernel reads with le*_to_cpu.
 * Versioned for the Phase-3 spare-assignment table. */
#define RKDCL_MAGIC		"RKDCLMD1"
#define RKDCL_SB_VERSION	1
#define RKDCL_SB_BYTES		4096

struct rkdcl_sb {
	char		magic[8];	/* RKDCL_MAGIC, no NUL		*/
	uint32_t	version;	/* RKDCL_SB_VERSION		*/
	uint32_t	hdr_crc;	/* crc32-le of the 4 KiB block
					 * with this field zeroed	*/
	uint32_t	pool_disks;	/* N — cross-check vs SB	*/
	uint32_t	group_width;	/* g = k + m			*/
	uint32_t	parity;		/* m				*/
	uint32_t	spare_cols;	/* s				*/
	uint32_t	ngroups;	/* (N - s) / g			*/
	uint32_t	nbase;		/* base permutations		*/
	uint64_t	seed;		/* accepted permutation seed	*/
	uint64_t	flags;		/* 0				*/
	/* pad to RKDCL_SB_BYTES */
};

/* Geometry validation (C1: (N-s) % g == 0, k >= 1, s >= 1, ngroups >= 1).
 * Returns 0 and sets *ngroups on success; on failure prints the reason and
 * a legal nearby spare-column suggestion, returns -1. */
int rkdcl_validate_geometry(unsigned int N, unsigned int g, unsigned int m,
			    unsigned int s, unsigned int *ngroups);

/* Smallest legal spare-column count (>= 1) for N, g: s == N mod g (or g). */
unsigned int rkdcl_default_spares(unsigned int N, unsigned int g);

/* Acceptance search: try `tries` seeds from `seed0`, score each by the
 * rebuild bottleneck (max per-survivor read+write load; CV tiebreak), return
 * the winner and print its rebuild stats.  Deterministic.  Returns 0, or -1
 * on allocation failure / invalid geometry. */
int rkdcl_accept_seed(unsigned int N, unsigned int g, unsigned int m,
		      unsigned int s, unsigned int nbase,
		      uint64_t seed0, unsigned int tries, uint64_t *seed_out);

/* Fill a 4 KiB metadata block (caller provides RKDCL_SB_BYTES buffer). */
void rkdcl_build_sb(void *buf, unsigned int N, unsigned int g,
		    unsigned int m, unsigned int s, unsigned int nbase,
		    uint64_t seed);

#endif /* RAIDKM_DCL_H */
