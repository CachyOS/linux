// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <stdio.h>
#include <stdbool.h>
#include <linux/kernel.h>
#include <linux/mman.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>

#include "../kselftest.h"

static const char * const dev_files[] = {
	"/dev/zero", "/dev/null", "/dev/urandom",
	"/proc/version", "/proc"
};

/*
 * Open/create the file at filename, (optionally) write random data to it
 * (exactly num_pages), then test the cachestat syscall on this file.
 *
 * If test_fsync == true, fsync the file, then check the number of dirty
 * pages.
 */
bool test_cachestat(const char *filename, bool write_random, bool create,
	bool test_fsync, int num_pages, int open_flags, mode_t open_mode)
{
	int cachestat_nr = 451;
	size_t PS = sysconf(_SC_PAGESIZE);
	int filesize = num_pages * PS;
	bool ret = true;
	int random_fd;
	long syscall_ret;
	struct cachestat cs;

	int fd = open(filename, open_flags, open_mode);

	if (fd == -1) {
		ksft_print_msg("Unable to create/open file.\n");
		goto out;
	} else {
		ksft_print_msg("Create/open %s\n", filename);
	}

	if (write_random) {
		char data[filesize];

		random_fd = open("/dev/urandom", O_RDONLY);

		if (random_fd < 0) {
			ksft_print_msg("Unable to access urandom.\n");
			ret = false;
			goto out1;
		} else {
			int remained = filesize;
			char *cursor = data;

			while (remained) {
				ssize_t read_len = read(random_fd, cursor, remained);

				if (read_len <= 0) {
					ksft_print_msg("Unable to read from urandom.\n");
					ret = false;
					goto out2;
				}

				remained -= read_len;
				cursor += read_len;
			}

			/* write random data to fd */
			remained = filesize;
			cursor = data;
			while (remained) {
				ssize_t write_len = write(fd, cursor, remained);

				if (write_len <= 0) {
					ksft_print_msg("Unable write random data to file.\n");
					ret = false;
					goto out2;
				}

				remained -= write_len;
				cursor += write_len;
			}
		}
	}

	syscall_ret = syscall(cachestat_nr, fd, 0, filesize, &cs);

	ksft_print_msg("Cachestat call returned %ld\n", syscall_ret);

	if (syscall_ret) {
		ksft_print_msg("Cachestat returned non-zero.\n");
		ret = false;

		if (write_random)
			goto out2;
		else
			goto out1;

	} else {
		ksft_print_msg(
			"Using cachestat: Cached: %lu, Dirty: %lu, Writeback: %lu, Evicted: %lu, Recently Evicted: %lu\n",
			cs.nr_cache, cs.nr_dirty, cs.nr_writeback,
			cs.nr_evicted, cs.nr_recently_evicted);

		if (write_random) {
			if (cs.nr_cache + cs.nr_evicted != num_pages) {
				ksft_print_msg(
					"Total number of cached and evicted pages is off.\n");
				ret = false;
			}
		}
	}

	if (test_fsync) {
		if (fsync(fd)) {
			ksft_print_msg("fsync fails.\n");
			ret = false;
		} else {
			syscall_ret = syscall(cachestat_nr, fd, 0, filesize, &cs);

			ksft_print_msg("Cachestat call (after fsync) returned %ld\n",
				syscall_ret);

			if (!syscall_ret) {
				ksft_print_msg(
					"Using cachestat: Cached: %lu, Dirty: %lu, Writeback: %lu, Evicted: %lu, Recently Evicted: %lu\n",
					cs.nr_cache, cs.nr_dirty, cs.nr_writeback,
					cs.nr_evicted, cs.nr_recently_evicted);

				if (cs.nr_dirty) {
					ret = false;
					ksft_print_msg(
						"Number of dirty should be zero after fsync.\n");
				}
			} else {
				ksft_print_msg("Cachestat (after fsync) returned non-zero.\n");
				ret = false;

				if (write_random)
					goto out2;
				else
					goto out1;
			}
		}
	}

out2:
	if (write_random)
		close(random_fd);
out1:
	close(fd);

	if (create)
		remove(filename);
out:
	return ret;
}

int main(void)
{
	for (int i = 0; i < 5; i++) {
		const char *dev_filename = dev_files[i];

		if (test_cachestat(dev_filename, false, false, false,
			4, O_RDONLY, 0400))
			ksft_test_result_pass("cachestat works with %s\n", dev_filename);
		else
			ksft_test_result_fail("cachestat fails with %s\n", dev_filename);
	}

	if (test_cachestat("tmpfilecachestat", true, true,
		true, 4, O_CREAT | O_RDWR, 0400 | 0600))
		ksft_test_result_pass("cachestat works with a normal file\n");
	else
		ksft_test_result_fail("cachestat fails with normal file\n");

	return 0;
}
