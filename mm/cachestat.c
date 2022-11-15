// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * The cachestat() system call.
 */
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/syscalls.h>
#include <linux/shmem_fs.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <uapi/linux/mman.h>

#include "swap.h"

/*
 * The cachestat(3) system call.
 *
 * cachestat() returns the page cache status of a file in the
 * bytes specified by `off` and `len`: number of cached pages,
 * number of dirty pages, number of pages marked for writeback,
 * number of (recently) evicted pages.
 *
 * If `off` + `len` >= `off`, the queried range is [`off`, `off` + `len`].
 * Otherwise, we will query in the range from `off` to the end of the file.
 *
 * Because the status of a page can change after cachestat() checks it
 * but before it returns to the application, the returned values may
 * contain stale information.
 *
 * return values:
 *  zero    - success
 *  -EFAULT - cstat points to an illegal address
 *  -EINVAL - invalid arguments
 *  -EBADF	- invalid file descriptor
 */
SYSCALL_DEFINE4(cachestat, unsigned int, fd, off_t, off, size_t, len,
	struct cachestat __user *, cstat)
{
	struct fd f;
	struct cachestat cs;

	memset(&cs, 0, sizeof(struct cachestat));

	if (off < 0)
		return -EINVAL;

	if (!access_ok(cstat, sizeof(struct cachestat)))
		return -EFAULT;

	f = fdget(fd);
	if (f.file) {
		struct address_space *mapping = f.file->f_mapping;
		pgoff_t first_index = off >> PAGE_SHIFT;
		XA_STATE(xas, &mapping->i_pages, first_index);
		struct folio *folio;
		pgoff_t last_index = (off + len - 1) >> PAGE_SHIFT;

		if (last_index < first_index)
			last_index = ULONG_MAX;

		rcu_read_lock();
		xas_for_each(&xas, folio, last_index) {
			if (xas_retry(&xas, folio) || !folio)
				continue;

			if (xa_is_value(folio)) {
				/* page is evicted */
				void *shadow;
				bool workingset; /* not used */

				cs.nr_evicted += 1;

				if (shmem_mapping(mapping)) {
					/* shmem file - in swap cache */
					swp_entry_t swp = radix_to_swp_entry(folio);

					shadow = get_shadow_from_swap_cache(swp);
				} else {
					/* in page cache */
					shadow = (void *) folio;
				}

				if (workingset_test_recent(shadow, true, &workingset))
					cs.nr_recently_evicted += 1;

				continue;
			}

			/* page is in cache */
			cs.nr_cache += 1;

			if (folio_test_dirty(folio))
				cs.nr_dirty += 1;

			if (folio_test_writeback(folio))
				cs.nr_writeback += 1;
		}
		rcu_read_unlock();
		fdput(f);

		if (copy_to_user(cstat, &cs, sizeof(struct cachestat)))
			return -EFAULT;

		return 0;
	}

	return -EBADF;
}
