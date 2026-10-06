/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Virtual swap space
 *
 * Copyright (C) 2026 Nhat Pham
 */
#ifndef _MM_VSWAP_H
#define _MM_VSWAP_H

#include <linux/jump_label.h>
#include <linux/swap.h>
#include "swap.h"

struct zswap_entry;

/*
 * VSWAP_ZERO and VSWAP_FOLIO are return-only values synthesized from
 * swap_table state; the rest are stored in the vtable per slot.
 */
enum vswap_backing_type {
	VSWAP_NONE	= 0,
	VSWAP_ZSWAP	= 1,
	VSWAP_SWAPFILE	= 2,
	VSWAP_ZERO,
	VSWAP_FOLIO,
};

#ifdef CONFIG_SWAP

#include "swap_table.h"
DECLARE_STATIC_KEY_FALSE(vswap_key);

/*
 * Only true once vswap_init() has published vswap_si, so callers never
 * see the device half built.
 */
static inline bool vswap_is_enabled(void)
{
	return static_branch_unlikely(&vswap_key);
}

static inline bool is_vswap_entry(swp_entry_t entry)
{
	return swap_is_vswap(__swap_entry_to_info(entry));
}

/*
 * Rmap cache-only helpers for physical cluster Pointer-tagged entries.
 * SWP_RMAP_CACHE_ONLY records, inline on the physical swap_table entry,
 * that the backing vswap entry has swap_count == 0 (swap-cache-only, so
 * reclaimable). The physical reclaim scanner reads it directly instead of
 * chasing the rmap into the vswap layer and paying the cluster-lookup
 * indirection.
 *
 * Callers hold the vswap cluster lock, not the physical one. The rmap is
 * only touched while the vtable holds the slot as SWAPFILE, and that
 * window is opened and closed under the vswap cluster lock, so the
 * allocator has finished writing the entry by then.
 */
static inline void swap_rmap_mark_cache_only(struct swap_cluster_info *ci,
					     unsigned int off)
{
	atomic_long_t *table;

	table = rcu_dereference_protected(ci->table, true);
	atomic_long_or(SWP_RMAP_CACHE_ONLY, &table[off]);
}

static inline void swap_rmap_clear_cache_only(struct swap_cluster_info *ci,
					      unsigned int off)
{
	atomic_long_t *table;

	table = rcu_dereference_protected(ci->table, true);
	atomic_long_and(~SWP_RMAP_CACHE_ONLY, &table[off]);
}

/*
 * Virtual table entry encoding for vswap clusters.
 *
 * Each entry in ci->virtual_table stores the backing type and
 * pointer for a virtual swap slot. Tag in low 3 bits, payload in
 * upper 61 bits.
 *
 *   NONE:     |----- 0000 ------|000|  - no separate backend pointer
 *   ZSWAP:    |--- zswap_entry* |001|  - compressed in zswap (tag in low bits)
 *   SWAPFILE: |- type:5,off:56 -|010|  - on a physical swapfile
 *
 * SWAPFILE packs swp_type in the top MAX_SWAPFILES_SHIFT bits and swp_offset in
 * the middle VTABLE_PHYS_OFF_BITS bits, both above the tag, so the type is
 * not shifted off the word. Pointer payloads (ZSWAP) are stored directly with
 * the tag OR'd into the low bits (kernel pointers are >= 8-byte aligned, same
 * approach as xarray).
 *
 * vtable[i] = NONE does not by itself mean "free". The swap_table entry
 * and the per-slot zero flag carry the rest of the state. The full
 * per-slot state table is:
 *
 *   vtable[i] | swap_table[i] | zero  | meaning
 *   ----------+---------------+-------+--------------------------------
 *   NONE      | NULL          | clear | truly free / unbacked
 *   NONE      | PFN           | clear | folio cached, no backing
 *   NONE      | shadow        | clear | evicted, no backing: data lost
 *   NONE      | *             | set   | zero-backed; cached if PFN set
 *   ZSWAP     | PFN           | clear | folio cached + zswap entry
 *   ZSWAP     | shadow / NULL | clear | evicted, only in zswap
 *   SWAPFILE  | PFN           | clear | folio cached + physical slot
 *   SWAPFILE  | shadow / NULL | clear | evicted, only on the swapfile
 *
 * Locking: a slot's vtable entry (the vswap entry's backend) is only
 * stable while the caller owns and holds the lock on that entry's swap
 * cache folio. The cluster lock (ci->lock) only makes an individual
 * vtable read atomic, and by itself does not give the caller the right to
 * change the backend. A backend read without the folio lock is
 * best-effort and must be re-validated under the folio lock before
 * being acted on.
 *
 * Zero-backed slots use the swap_table per-slot zero flag (same as
 * direct-mapped physical swap), via __swap_table_test_zero() and friends,
 * which fall back to ci->zero_bitmap where the flag does not fit. Cached
 * folios are read out of the swap_table PFN entry; there is no separate FOLIO
 * vtable type because the folio pointer would duplicate that PFN and
 * would go stale on folio migration / split.
 */

#define VTABLE_TAG_BITS		3
#define VTABLE_TAG_MASK		((1UL << VTABLE_TAG_BITS) - 1)

static inline enum vswap_backing_type vtable_type(unsigned long vt)
{
	return vt & VTABLE_TAG_MASK;
}

/* swp_offset field width in a physical backend slot; layout described above. */
#define VTABLE_PHYS_OFF_BITS	(BITS_PER_LONG - VTABLE_TAG_BITS - MAX_SWAPFILES_SHIFT)

static inline unsigned long vtable_mk_phys(swp_entry_t entry)
{
	VM_WARN_ON_ONCE(swp_offset(entry) >> VTABLE_PHYS_OFF_BITS);
	return ((unsigned long)swp_type(entry) << (VTABLE_TAG_BITS + VTABLE_PHYS_OFF_BITS)) |
	       (swp_offset(entry) << VTABLE_TAG_BITS) | VSWAP_SWAPFILE;
}

static inline swp_entry_t vtable_to_phys(unsigned long vt)
{
	VM_WARN_ON(vtable_type(vt) != VSWAP_SWAPFILE);
	return swp_entry(vt >> (VTABLE_TAG_BITS + VTABLE_PHYS_OFF_BITS),
			 (vt >> VTABLE_TAG_BITS) & ((1UL << VTABLE_PHYS_OFF_BITS) - 1));
}

static inline struct zswap_entry *vtable_to_zswap(unsigned long vt)
{
	VM_WARN_ON(vtable_type(vt) != VSWAP_ZSWAP);
	return (struct zswap_entry *)(vt & ~VTABLE_TAG_MASK);
}

/* Virtual table accessors */

static inline unsigned long __vtable_get(struct swap_cluster_info *ci,
					 unsigned int off)
{
	VM_WARN_ON_ONCE(off >= SWAPFILE_CLUSTER);
	return atomic_long_read(&ci->virtual_table[off]);
}

static inline void __vtable_set(struct swap_cluster_info *ci,
				unsigned int off, unsigned long vt)
{
	VM_WARN_ON_ONCE(off >= SWAPFILE_CLUSTER);
	atomic_long_set(&ci->virtual_table[off], vt);
}

/**
 * vswap_lock_cluster - look up and lock the vswap cluster for an entry
 * @entry: the virtual swap entry
 * @voff: out param, receives @entry's slot offset within the cluster
 *
 * Return: the locked vswap cluster.
 */
static inline struct swap_cluster_info *
vswap_lock_cluster(swp_entry_t entry, unsigned int *voff)
{
	*voff = swp_cluster_offset(entry);
	return swap_cluster_lock(__swap_entry_to_info(entry), swp_offset(entry));
}

/**
 * vswap_to_phys - resolve a vswap entry's physical swap backing
 * @entry: the virtual swap entry
 *
 * Context: takes and drops the vswap cluster lock internally.
 * Return: the backing physical swp_entry_t, or the null entry (.val == 0)
 * when @entry has no physical backing (NONE/ZSWAP/ZERO).
 */
static inline swp_entry_t vswap_to_phys(swp_entry_t entry)
{
	struct swap_cluster_info *ci;
	unsigned int voff;
	unsigned long vt;

	ci = vswap_lock_cluster(entry, &voff);
	vt = __vtable_get(ci, voff);
	swap_cluster_unlock(ci);

	if (vtable_type(vt) != VSWAP_SWAPFILE)
		return (swp_entry_t){};

	return vtable_to_phys(vt);
}

void __vswap_release_backing(struct swap_cluster_info *ci,
			     unsigned int ci_start, unsigned int nr);

/**
 * vswap_zswap_store - record a zswap entry as the backing for a vswap entry.
 * @entry: the vswap entry
 * @ze: the zswap entry now holding @entry's compressed data
 *
 * Releases @entry's previous backing, and sets the zswap entry @ze as the new
 * backing.
 *
 * Context: takes and drops the vswap cluster lock internally.
 */
static inline void vswap_zswap_store(swp_entry_t entry,
				     struct zswap_entry *ze)
{
	struct swap_cluster_info *ci;
	unsigned int voff;

	ci = vswap_lock_cluster(entry, &voff);
	__vswap_release_backing(ci, voff, 1);
	__vtable_set(ci, voff, (unsigned long)ze | VSWAP_ZSWAP);
	swap_cluster_unlock(ci);
}

/**
 * vswap_zswap_load - return the zswap entry backing a vswap entry
 * @entry: the virtual swap entry
 *
 * Context: takes and drops the vswap cluster lock internally.
 * Return: the backing zswap entry, or NULL if @entry is not zswap-backed.
 */
static inline struct zswap_entry *vswap_zswap_load(swp_entry_t entry)
{
	struct swap_cluster_info *ci;
	unsigned int voff;
	unsigned long vt;

	ci = vswap_lock_cluster(entry, &voff);
	vt = __vtable_get(ci, voff);
	swap_cluster_unlock(ci);

	if (vtable_type(vt) != VSWAP_ZSWAP)
		return NULL;
	return vtable_to_zswap(vt);
}

void folio_release_vswap_backing(struct folio *folio);
swp_entry_t folio_realloc_swap(struct folio *folio);
void folio_release_non_phys_swap_backing(struct folio *folio);

/*
 * Walk nr vtable slots starting at voff in ci. Returns the prefix
 * length of slots sharing one effective backing type. For SWAPFILE,
 * the prefix is also restricted to contiguous offsets in the same
 * swapfile.
 *
 * Effective type per slot:
 *   vtable=NONE + zero flag set       -> VSWAP_ZERO
 *   vtable=NONE + swap_table PFN tag  -> VSWAP_FOLIO
 *   vtable=NONE + neither             -> VSWAP_NONE
 *   vtable=SWAPFILE                   -> VSWAP_SWAPFILE
 *   vtable=ZSWAP                      -> VSWAP_ZSWAP
 *
 * *typep returns the effective type of slot 0. Caller holds
 * ci->lock.
 */
static inline int __vswap_check_backing(struct swap_cluster_info *ci,
					unsigned int voff, int nr,
					enum vswap_backing_type *typep)
{
	enum vswap_backing_type first_type = VSWAP_NONE;
	enum vswap_backing_type slot_type;
	swp_entry_t first_phys = {};
	unsigned long vt, swap_tb;
	int i;

	lockdep_assert_held(&ci->lock);

	for (i = 0; i < nr; i++) {
		vt = __vtable_get(ci, voff + i);
		if (vtable_type(vt) == VSWAP_NONE) {
			swap_tb = __swap_table_get(ci, voff + i);
			if (__swap_table_test_zero(ci, voff + i))
				slot_type = VSWAP_ZERO;
			else if (swp_tb_is_folio(swap_tb))
				slot_type = VSWAP_FOLIO;
			else
				slot_type = VSWAP_NONE;
		} else {
			slot_type = vtable_type(vt);
		}

		if (!i) {
			first_type = slot_type;
			if (first_type == VSWAP_SWAPFILE)
				first_phys = vtable_to_phys(vt);
		} else if (slot_type != first_type) {
			break;
		} else if (first_type == VSWAP_SWAPFILE &&
			   vtable_to_phys(vt).val != first_phys.val + i) {
			break;
		}
	}

	if (typep)
		*typep = first_type;
	return i;
}

static inline int vswap_check_backing(swp_entry_t entry, int nr,
				      enum vswap_backing_type *typep)
{
	struct swap_cluster_info *ci;
	unsigned int voff;
	int ret;

	ci = vswap_lock_cluster(entry, &voff);
	ret = __vswap_check_backing(ci, voff, nr, typep);
	swap_cluster_unlock(ci);
	return ret;
}

/**
 * folio_phys_swap_backed - test whether a folio is backed by a contiguous
 *                          range of physical swap slots.
 * @folio: a swap-cache resident folio
 *
 * Return: %true if @folio->swap is not a vswap entry, or if these vswap
 * entries are backed by a contiguous range of physical slots.
 */
static inline bool folio_phys_swap_backed(struct folio *folio)
{
	swp_entry_t entry = folio->swap;
	int nr = folio_nr_pages(folio);
	enum vswap_backing_type type;

	return !is_vswap_entry(entry) ||
	       (vswap_check_backing(entry, nr, &type) == nr &&
		type == VSWAP_SWAPFILE);
}

static inline int vswap_cluster_alloc_vtable(struct swap_cluster_info *ci,
					     gfp_t gfp)
{
	ci->virtual_table = kcalloc(SWAPFILE_CLUSTER,
				    sizeof(*ci->virtual_table), gfp);
	return ci->virtual_table ? 0 : -ENOMEM;
}

static inline void vswap_cluster_free_vtable(struct swap_cluster_info *ci)
{
	kfree(ci->virtual_table);
	ci->virtual_table = NULL;
}

#else /* !CONFIG_SWAP */

static inline bool vswap_is_enabled(void)
{
	return false;
}

static inline bool is_vswap_entry(swp_entry_t entry)
{
	return false;
}

static inline swp_entry_t vswap_to_phys(swp_entry_t entry)
{
	return (swp_entry_t){};
}

static inline bool folio_phys_swap_backed(struct folio *folio)
{
	return true;
}

#endif /* CONFIG_SWAP */

/*
 * Test a per-backend swap flag (SWP_SYNCHRONOUS_IO, SWP_STABLE_WRITES, ...)
 * for @entry. For a vswap entry the property belongs to the current
 * physical backing rather than vswap_si itself; resolve to the backing
 * and test there. Returns false for zswap/zero/unbacked vswap entries
 * as they don't have a backing bdev.
 */
static inline bool swap_entry_backend_has_flag(struct swap_info_struct *si,
					       swp_entry_t entry,
					       unsigned long flag)
{
	struct swap_info_struct *phys_si;
	swp_entry_t phys;
	bool has_flag;

	if (!swap_is_vswap(si))
		return data_race(si->flags & flag);

	phys = vswap_to_phys(entry);
	if (!phys.val)
		return false;

	phys_si = get_swap_device(phys);
	if (IS_ERR_OR_NULL(phys_si))
		return false;

	has_flag = data_race(phys_si->flags & flag);
	put_swap_device(phys_si);
	return has_flag;
}

#endif /* _MM_VSWAP_H */
