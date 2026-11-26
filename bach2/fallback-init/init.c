/* bach2 fallback /init: the only file in the CI kernel's built-in initramfs.
 *
 * Test images get their real initramfs as an external initrd (/chosen linux,initrd-start/end, set at
 * pack time; it overwrites this /init when the kernel unpacks it). If that initrd is missing or
 * unreadable, the kernel runs this file instead. It logs to kmsg and pmsg (both survive in pstore),
 * then restarts, which on bach2 boots the untouched stock kernel. No buttons needed.
 * Loop guard (same policy as the real init): if pmsg already holds 2 fallback boots for this attempt
 * id (/chosen/bach2,attempt), the restart is re-entering erecovery, so it parks instead of looping.
 * Static, no shell, no other files. Build: gcc -static -Os -s -o init init.c
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <unistd.h>

static void put(const char *path, const char *s)
{
	int fd = open(path, O_WRONLY | O_NOCTTY);

	if (fd >= 0) {
		(void)!write(fd, s, strlen(s));
		close(fd);
	}
}

static int slurp(const char *path, char *buf, size_t n)
{
	int fd = open(path, O_RDONLY), len = 0, r;

	if (fd < 0)
		return 0;
	while (len < (int)n - 1 && (r = read(fd, buf + len, n - 1 - len)) > 0)
		len += r;
	close(fd);
	buf[len] = 0;
	return len;
}

static char pm[131072 + 1];

int main(void)
{
	char id[64] = "noid", tag[128], msg[256];
	int n = 0, len, i;
	char *p;

	mkdir("/dev", 0755); mkdir("/proc", 0555); mkdir("/sys", 0555);
	mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
	mount("proc", "/proc", "proc", 0, NULL);
	mount("sysfs", "/sys", "sysfs", 0, NULL);
	mount("pstore", "/sys/fs/pstore", "pstore", 0, NULL);

	len = slurp("/proc/device-tree/chosen/bach2,attempt", id, sizeof(id));
	for (i = 0; i < len; i++)
		if (id[i] < 0x21 || id[i] > 0x7e)
			id[i] = 0;
	if (!id[0])
		strcpy(id, "noid");
	snprintf(tag, sizeof(tag), "BACH2ML-FALLBACK-INIT %s", id);
	len = slurp("/sys/fs/pstore/pmsg-ramoops-0", pm, sizeof(pm));
	for (i = 0; i < len; i++)
		if (!pm[i])
			pm[i] = '\n';
	/* pmsg holds the previous boot only: carry the count forward ("<tag> n=<k>"), like the real init */
	for (p = pm; (p = strstr(p, tag)); p += strlen(tag)) {
		int k = 0;

		if (sscanf(p + strlen(tag), " n=%d", &k) == 1 && k > n)
			n = k;
	}
	n++;

	snprintf(msg, sizeof(msg), "%s n=%d: no external initramfs (initrd missing/unreadable); %s\n", tag, n,
		 n >= 3 ? "loop guard: parking" : "kernel restart into stock in 3 s");
	put("/dev/pmsg0", msg);
	snprintf(msg, sizeof(msg), "<2>BACH2ML: FALLBACK-INIT attempt=%s n=%d: no external initramfs; %s\n", id, n,
		 n >= 3 ? "loop guard: parking" : "restart in 3 s");
	put("/dev/kmsg", msg);
	sync();
	if (n >= 3)
		for (;;)
			pause();
	sleep(3);
	sync();
	reboot(RB_AUTOBOOT);
	for (;;)
		pause();
}
