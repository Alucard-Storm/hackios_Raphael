/*
 * hakios-img: an image file on the phone's Android /data that the loader can use as storage.
 * Static; built for the phone by tools/android/build.sh, run as root (su) on Android.
 *
 *   hakios-img create <path> <bytes>   an empty file, pinned so f2fs never moves its blocks, then
 *                                      all its blocks allocated (nothing is written into them)
 *   hakios-img extents <path>          where its blocks are on the partition, in 4096-byte blocks:
 *                                      "<file block> <partition block> <count> <flags>" a line each
 *   hakios-img pinned <path>           its f2fs GC-failure count
 *
 * /data is encrypted below the filesystem (metadata encryption), so the file's contents are not
 * readable from here and are not meant to be: the loader reads and writes these blocks raw.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define F2FS_IOC_SET_PIN_FILE _IOW(0xf5, 13, uint32_t)
#define F2FS_IOC_GET_PIN_FILE _IOR(0xf5, 14, uint32_t)

static int
fail(const char *what)
{
	fprintf(stderr, "hakios-img: %s: %s\n", what, strerror(errno));
	return 1;
}

static int
extents(const char *path)
{
	int fd = open(path, O_RDONLY);
	enum { N = 256 };
	struct fiemap *fm = calloc(1, sizeof(*fm) + N * sizeof(struct fiemap_extent));
	uint64_t at = 0, blocks = 0, n = 0;
	int last = 0;

	if (fd < 0) return fail(path);
	while (!last) {
		fm->fm_start = at;
		fm->fm_length = ~0ULL - at;
		fm->fm_flags = FIEMAP_FLAG_SYNC;
		fm->fm_extent_count = N;
		fm->fm_mapped_extents = 0;
		if (ioctl(fd, FS_IOC_FIEMAP, fm) < 0) return fail("FIEMAP");
		if (fm->fm_mapped_extents == 0) break;
		for (uint32_t i = 0; i < fm->fm_mapped_extents; i++) {
			struct fiemap_extent *e = &fm->fm_extents[i];

			printf("%llu %llu %llu 0x%x\n", (unsigned long long)(e->fe_logical / 4096),
			    (unsigned long long)(e->fe_physical / 4096), (unsigned long long)(e->fe_length / 4096), e->fe_flags);
			blocks += e->fe_length / 4096;
			n++;
			at = e->fe_logical + e->fe_length;
			if (e->fe_flags & FIEMAP_EXTENT_LAST) last = 1;
		}
	}
	fprintf(stderr, "hakios-img: %llu extents, %llu blocks\n", (unsigned long long)n, (unsigned long long)blocks);
	return 0;
}

int
main(int argc, char **argv)
{
	if (argc >= 4 && strcmp(argv[1], "create") == 0) {
		char dir[512];
		uint64_t bytes = strtoull(argv[3], NULL, 0);
		uint32_t pin = 1;
		int fd;

		if (bytes == 0 || bytes % 4096) { fprintf(stderr, "hakios-img: a multiple of 4096 bytes\n"); return 1; }
		strncpy(dir, argv[2], sizeof(dir) - 1);
		dir[sizeof(dir) - 1] = 0;
		char *slash = strrchr(dir, '/');
		if (slash) { *slash = 0; mkdir(dir, 0700); }
		fd = open(argv[2], O_RDWR | O_CREAT | O_EXCL, 0600);
		if (fd < 0) return fail(argv[2]);
		if (ioctl(fd, F2FS_IOC_SET_PIN_FILE, &pin) < 0) { fail("pin (f2fs?)"); unlink(argv[2]); return 1; }
		if (fallocate(fd, 0, 0, (off_t)bytes) < 0) { fail("fallocate"); unlink(argv[2]); return 1; }
		fsync(fd);
		close(fd);
		return extents(argv[2]);
	}
	if (argc >= 3 && strcmp(argv[1], "extents") == 0) {
		return extents(argv[2]);
	}
	if (argc >= 3 && strcmp(argv[1], "pin") == 0) {
		uint32_t pin = 1;
		int fd = open(argv[2], O_RDWR);

		if (fd < 0) return fail(argv[2]);
		if (ioctl(fd, F2FS_IOC_SET_PIN_FILE, &pin) < 0) return fail("set pin");
		pin = 0;
		if (ioctl(fd, F2FS_IOC_GET_PIN_FILE, &pin) < 0) return fail("get pin");
		printf("pin set; gc failures %u\n", pin);
		return 0;
	}
	if (argc >= 3 && strcmp(argv[1], "pinned") == 0) {
		uint32_t pin = 0;
		int fd = open(argv[2], O_RDONLY);

		if (fd < 0) return fail(argv[2]);
		if (ioctl(fd, F2FS_IOC_GET_PIN_FILE, &pin) < 0) return fail("get pin");
		printf("gc failures counted for it: %u (this kernel does not report the pin itself; dump.f2fs -i shows i_inline 0x40)\n", pin);
		return 0;
	}
	fprintf(stderr, "usage: hakios-img create <path> <bytes> | extents <path> | pinned <path>\n");
	return 1;
}
