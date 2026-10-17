/* rfetch: system information like fastfetch, with the memory that is really in use.
 *
 * One process, no forks, no libraries beyond libc: it reads /proc, /sys, the
 * DRM driver and CPUID directly and writes the result with a single write().
 *
 * Memory "used" here = MemTotal - MemFree - Buffers - Cached - SReclaimable + Shmem
 *                      - pages on the per-CPU free lists - the GPU driver's free page pool.
 * The first part is what neofetch and htop count. The two subtractions are free
 * memory that /proc/meminfo doesn't report as free: the kernel keeps a few free
 * pages per CPU for fast allocation (zoneinfo "count:"), and GPU drivers keep a
 * pool of freed pages (meminfo GPUReclaim) that the kernel takes back under
 * pressure. fastfetch and free(1) use MemTotal - MemAvailable, which also
 * counts the kernel's watermark reserve as used.
 *
 * Disk "used" on XFS leaves out the blocks the filesystem reserves in every
 * allocation group for its own btrees, which statfs (df) counts as used; see
 * xfs_reserved_blocks().
 *
 * Usage: rfetch [-m] [-n]   -m: memory breakdown   -n: no logo
 */
#define _GNU_SOURCE
#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#define HAVE_CPUID 1
#endif
#include <dirent.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <limits.h>
#include <math.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <unistd.h>

#define MAXLINES 32
#define LINELEN 320

static char lines[MAXLINES][LINELEN];
static int nlines;
static int color = 1;
static char out[32768];
static size_t olen;

static void emit(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(out + olen, sizeof out - olen, fmt, ap);
	va_end(ap);
	if (n > 0) olen += (size_t)n < sizeof out - olen ? (size_t)n : sizeof out - olen - 1;
}

/* Read a whole (small) file into buf, NUL-terminated, trailing newline removed. */
static ssize_t rd(const char *path, char *buf, size_t sz)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) return -1;
	size_t n = 0;
	ssize_t r;
	while (n < sz - 1 && (r = read(fd, buf + n, sz - 1 - n)) > 0) n += (size_t)r;
	close(fd);
	while (n && (buf[n - 1] == '\n' || buf[n - 1] == ' ')) n--;
	buf[n] = 0;
	return (ssize_t)n;
}

static long rdl(const char *path)
{
	char b[64];
	return rd(path, b, sizeof b) > 0 ? strtol(b, NULL, 0) : -1;
}

/* key: value line; key is printed bold in the logo colour */
static void field(const char *key, const char *fmt, ...)
{
	if (nlines >= MAXLINES) return;
	char *l = lines[nlines++];
	int n = color ? snprintf(l, LINELEN, "\033[1;35m%s\033[0m: ", key) : snprintf(l, LINELEN, "%s: ", key);
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(l + n, LINELEN - (size_t)n, fmt, ap);
	va_end(ap);
}

static void size_str(char *b, size_t n, double bytes)
{
	static const char *u[] = { "B", "KiB", "MiB", "GiB", "TiB" };
	int i = 0;
	while (bytes >= 1024 && i < 4) bytes /= 1024, i++;
	if (i == 0) snprintf(b, n, "%.0f B", bytes);
	else snprintf(b, n, "%.2f %s", bytes, u[i]);
}

/* ---- memory ---- */

struct mem {
	long total, free, avail, buffers, cached, shmem, srecl, sunrecl, kstack, pt, spt, percpu, vmalloc,
		anon, swtotal, swfree, gpuact, gpurecl, pcp; /* kB */
};

static void meminfo(struct mem *m)
{
	static const struct { const char *k; size_t off; } keys[] = {
#define K(name, f) { name, offsetof(struct mem, f) }
		K("MemTotal", total), K("MemFree", free), K("MemAvailable", avail), K("Buffers", buffers),
		K("Cached", cached), K("Shmem", shmem), K("SReclaimable", srecl), K("SUnreclaim", sunrecl),
		K("KernelStack", kstack), K("PageTables", pt), K("SecPageTables", spt), K("Percpu", percpu),
		K("VmallocUsed", vmalloc), K("AnonPages", anon), K("SwapTotal", swtotal), K("SwapFree", swfree),
		K("GPUActive", gpuact), K("GPUReclaim", gpurecl),
#undef K
	};
	char b[8192];
	memset(m, 0, sizeof *m);
	m->gpuact = m->gpurecl = -1;
	if (rd("/proc/meminfo", b, sizeof b) < 0) return;
	for (char *p = b; p && *p;) {
		char *c = strchr(p, ':');
		if (!c) break;
		size_t kl = (size_t)(c - p);
		for (size_t i = 0; i < sizeof keys / sizeof *keys; i++)
			if (strlen(keys[i].k) == kl && !memcmp(keys[i].k, p, kl)) {
				*(long *)((char *)m + keys[i].off) = strtol(c + 1, NULL, 10);
				break;
			}
		p = strchr(c, '\n');
		if (p) p++;
	}
	/* free pages sitting on the per-CPU lists (not in MemFree) */
	static char z[262144];
	ssize_t n = rd("/proc/zoneinfo", z, sizeof z);
	long pages = 0;
	for (char *p = z; n > 0 && (p = strstr(p, "count:")); p += 6) pages += strtol(p + 6, NULL, 10);
	m->pcp = pages * (sysconf(_SC_PAGESIZE) / 1024);
}

static long real_used(const struct mem *m)
{
	long u = m->total - m->free - m->buffers - m->cached - m->srecl + m->shmem - m->pcp - (m->gpurecl > 0 ? m->gpurecl : 0);
	return u < 0 ? 0 : u;
}

static void row(const char *label, long kb, const char *desc)
{
	emit("  %-17s%8.1f MiB%s%s\n", label, kb / 1024.0, desc[0] ? "  " : "", desc);
}

static void mem_breakdown(void)
{
	struct mem m;
	meminfo(&m);
	long used = real_used(&m);
	long prog = m.anon + m.shmem;
	long vm = m.vmalloc - m.kstack; /* vmalloc'd kernel stacks are in both fields */
	if (vm < 0) vm = 0;
	long kern = m.sunrecl + m.kstack + m.pt + m.spt + m.percpu + vm;
	long gpu = m.gpuact > 0 ? m.gpuact : 0;
	long drv = used - prog - kern - gpu;
	if (drv < 0) drv = 0;
	char t[32], kd[160];
	size_str(t, sizeof t, m.total * 1024.0);
	const char *B = color ? "\033[1;35m" : "", *R = color ? "\033[0m" : "";
	emit("%sReally in use%s: %.1f MiB of %s\n", B, R, used / 1024.0, t);
	row("programs", prog, "anonymous memory of all processes, plus tmpfs and shared memory");
	snprintf(kd, sizeof kd, "slab %.1f, stacks %.1f, page tables %.1f, per-CPU %.1f, vmalloc %.1f", m.sunrecl / 1024.0,
		 m.kstack / 1024.0, (m.pt + m.spt) / 1024.0, m.percpu / 1024.0, vm / 1024.0);
	row("kernel", kern, kd);
	if (m.gpuact >= 0) row("GPU", gpu, "buffers the GPU driver holds in system RAM");
	row("drivers/other", drv, "pages drivers allocate directly: DMA rings, SSD host memory buffer, ...");
	emit("%sNot counted%s (free, or freed on demand):\n", B, R);
	row("page cache", m.cached - m.shmem + m.buffers, "file data, dropped when memory is needed");
	row("reclaimable slab", m.srecl, "dentry and inode caches");
	row("per-CPU free", m.pcp, "free pages each CPU keeps for fast allocation");
	if (m.gpurecl >= 0) row("GPU page pool", m.gpurecl, "freed GPU pages, returned under memory pressure");
	emit("%sOther tools%s:\n", B, R);
	row("fastfetch/free", m.total - m.avail, "MemTotal - MemAvailable (counts the watermark reserve too)");
	row("neofetch/htop", m.total + m.shmem - m.free - m.buffers - m.cached - m.srecl, "rfetch without the two free-page subtractions");
}

/* ---- fields ---- */

static void f_os(const struct utsname *u)
{
	char b[2048], name[128] = "Linux";
	if (rd("/etc/os-release", b, sizeof b) > 0 || rd("/usr/lib/os-release", b, sizeof b) > 0) {
		char *p = strstr(b, "PRETTY_NAME=");
		if (p) {
			p += 12;
			if (*p == '"' || *p == '\'') p++;
			size_t n = strcspn(p, "\"'\n");
			if (n >= sizeof name) n = sizeof name - 1;
			memcpy(name, p, n);
			name[n] = 0;
		}
	}
	field("OS", "%s %s", name, u->machine);
}

static void f_host(void)
{
	char n[128], v[128];
	if (rd("/sys/class/dmi/id/product_name", n, sizeof n) <= 0) return;
	if (rd("/sys/class/dmi/id/product_version", v, sizeof v) > 0 && strcmp(v, "None") && !strstr(v, "To be filled") &&
	    !strstr(v, "Type1") && strcmp(v, "System Version"))
		field("Host", "%s (%s)", n, v);
	else
		field("Host", "%s", n);
}

static void f_uptime(void)
{
	struct sysinfo si;
	if (sysinfo(&si)) return;
	long s = si.uptime, d = s / 86400, h = s / 3600 % 24, mi = s / 60 % 60;
	char b[96];
	int n = 0;
	if (d) n += snprintf(b + n, sizeof b - (size_t)n, "%ld day%s, ", d, d == 1 ? "" : "s");
	if (h) n += snprintf(b + n, sizeof b - (size_t)n, "%ld hour%s, ", h, h == 1 ? "" : "s");
	if (mi) n += snprintf(b + n, sizeof b - (size_t)n, "%ld min%s, ", mi, mi == 1 ? "" : "s");
	if (!n) n = snprintf(b, sizeof b, "%ld secs, ", s);
	b[n - 2] = 0;
	field("Uptime", "%s", b);
}

struct dent { uint64_t ino; int64_t off; unsigned short reclen; unsigned char type; char name[]; };

/* Entries of an open directory that don't start with '.' or '-' (portage's -MERGING-). */
static long count_entries(int fd)
{
	char buf[16384];
	long n = 0, r;
	while ((r = syscall(SYS_getdents64, fd, buf, sizeof buf)) > 0)
		for (long off = 0; off < r;) {
			struct dent *d = (struct dent *)(buf + off);
			off += d->reclen;
			if (d->name[0] != '.' && d->name[0] != '-') n++;
		}
	return n;
}

static void f_packages(void)
{
	int fd = open("/var/db/pkg", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0) return;
	static char buf[16384]; /* categories */
	long n = 0, r;
	while ((r = syscall(SYS_getdents64, fd, buf, sizeof buf)) > 0)
		for (long off = 0; off < r;) {
			struct dent *d = (struct dent *)(buf + off);
			off += d->reclen;
			if (d->name[0] == '.' || d->type != DT_DIR) continue;
			int c = openat(fd, d->name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
			if (c >= 0) n += count_entries(c), close(c);
		}
	close(fd);
	field("Packages", "%ld (emerge)", n);
}

static int pcomm(pid_t pid, char *comm, size_t n)
{
	char p[64];
	snprintf(p, sizeof p, "/proc/%d/comm", pid);
	return rd(p, comm, n) > 0;
}

static pid_t ppid_of(pid_t pid)
{
	char p[64], b[512];
	snprintf(p, sizeof p, "/proc/%d/stat", pid);
	if (rd(p, b, sizeof b) <= 0) return 0;
	char *c = strrchr(b, ')'); /* comm may contain spaces */
	return c ? (pid_t)strtol(c + 4, NULL, 10) : 0;
}

static int in_list(const char *s, const char *const *l)
{
	for (; *l; l++)
		if (!strcmp(s, *l)) return 1;
	return 0;
}

static const char *const shells[] = { "bash", "zsh", "fish", "dash", "sh", "ksh", "mksh", "ash", "tcsh", "csh", "nu", "elvish", "xonsh", "yash", "oksh", NULL };
static const char *const skip[] = { "doas", "sudo", "su", "rfetch", "env", "time", "strace", NULL };
static const char *const wms[] = {
	"dwm", "i3", "sway", "bspwm", "openbox", "xmonad", "awesome", "herbstluftwm", "qtile", "Hyprland", "river", "labwc",
	"niri", "kwin_x11", "kwin_wayland", "mutter", "xfwm4", "fluxbox", "icewm", "spectrwm", "leftwm", "berry", "cwm", "2bwm",
	"jwm", "fvwm", "fvwm3", "ratpoison", "stumpwm", "wayfire", "weston", "gnome-shell", "marco", "muffin", "enlightenment",
	"dwl", "hikari", "wmaker", "WindowMaker", "evilwm", "windowchef", "frankenwm", "sowm", "tinywm", "ctwm", "twm", "pekwm",
	"xmonad-x86_64-l", "notion", "budgie-wm", "cosmic-comp", NULL };

/* Version of the installed shell package, e.g. app-shells/bash-5.3_p20 -> 5.3.20 (no exec). */
static void shell_version(const char *name, char *v, size_t n)
{
	v[0] = 0;
	DIR *d = opendir("/var/db/pkg/app-shells");
	if (!d) return;
	size_t nl = strlen(name);
	struct dirent *e;
	while ((e = readdir(d)))
		if (!strncmp(e->d_name, name, nl) && e->d_name[nl] == '-' && e->d_name[nl + 1] >= '0' && e->d_name[nl + 1] <= '9') {
			size_t j = 0;
			for (const char *q = e->d_name + nl + 1; *q && j < n - 1; q++) {
				if (q[0] == '-' && q[1] == 'r') break;          /* -r1: ebuild revision */
				if (q[0] == '_' && q[1] == 'p') { v[j++] = '.'; q++; continue; } /* _p20: patch level */
				v[j++] = *q;
			}
			v[j] = 0;
			break;
		}
	closedir(d);
}

/* Shell, terminal and (when it is the terminal's parent) window manager from the process tree. */
static void f_shell_term(char *wm, size_t wmn)
{
	char c[64], ver[32];
	pid_t pid = getppid();
	for (int i = 0; i < 4 && pcomm(pid, c, sizeof c) && in_list(c, skip); i++) pid = ppid_of(pid);
	pid_t shell = 0;
	if (pcomm(pid, c, sizeof c) && in_list(c, shells)) {
		shell = pid;
		shell_version(c, ver, sizeof ver);
		field("Shell", ver[0] ? "%s %s" : "%s", c, ver);
	} else {
		const char *s = getenv("SHELL");
		if (s && *s) field("Shell", "%s", strrchr(s, '/') ? strrchr(s, '/') + 1 : s);
	}
	if (!shell) return;
	pid_t t = ppid_of(shell);
	for (int i = 0; i < 6 && t > 1 && pcomm(t, c, sizeof c) && (in_list(c, shells) || in_list(c, skip)); i++) t = ppid_of(t);
	if (t <= 1 || !pcomm(t, c, sizeof c)) return;
	if (!strcmp(c, "login") || !strcmp(c, "agetty") || !strcmp(c, "getty")) {
		char *tty = ttyname(0);
		field("Terminal", "%s", tty ? tty : c);
		return;
	}
	field("Terminal", "%s", c);
	char w[64];
	if (pcomm(ppid_of(t), w, sizeof w) && in_list(w, wms)) snprintf(wm, wmn, "%s", w);
}

static void f_wm(char *wm)
{
	if (!wm[0]) { /* terminal not started by the WM: look for one among our processes */
		DIR *d = opendir("/proc");
		uid_t me = getuid();
		struct dirent *e;
		while (d && (e = readdir(d))) {
			if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
			struct stat st;
			if (fstatat(dirfd(d), e->d_name, &st, 0) || st.st_uid != me) continue;
			char c[64];
			if (pcomm((pid_t)atoi(e->d_name), c, sizeof c) && in_list(c, wms)) {
				snprintf(wm, 64, "%s", c);
				break;
			}
		}
		if (d) closedir(d);
	}
	if (!wm[0]) return;
	const char *wl = getenv("WAYLAND_DISPLAY"), *x = getenv("DISPLAY");
	field("WM", "%s%s", wm, wl && *wl ? " (Wayland)" : x && *x ? " (X11)" : "");
}

/* ---- display: active modes straight from the DRM driver ---- */

static const char *conn_type(uint32_t t)
{
	static const char *n[] = { "Unknown", "VGA", "DVI-I", "DVI-D", "DVI-A", "Composite", "SVIDEO", "LVDS", "Component",
				   "DIN", "DP", "HDMI-A", "HDMI-B", "TV", "eDP", "Virtual", "DSI", "DPI", "Writeback", "SPI", "USB" };
	return t < sizeof n / sizeof *n ? n[t] : "Unknown";
}

static void edid_name(const unsigned char *e, size_t n, char *name, size_t nn, double *inch)
{
	name[0] = 0;
	*inch = 0;
	if (n < 128) return;
	for (int d = 54; d <= 108; d += 18) /* monitor name descriptor */
		if (!e[d] && !e[d + 1] && e[d + 3] == 0xfc) {
			size_t l = 0;
			while (l < 13 && e[d + 5 + l] != '\n' && l < nn - 1) name[l] = (char)e[d + 5 + l], l++;
			while (l && name[l - 1] == ' ') l--;
			name[l] = 0;
		}
	if (!name[0]) { /* PNP id + product code, like "SDC4215" */
		unsigned v = (unsigned)e[8] << 8 | e[9];
		snprintf(name, nn, "%c%c%c%04X", '@' + (v >> 10 & 31), '@' + (v >> 5 & 31), '@' + (v & 31), (unsigned)(e[11] << 8 | e[10]));
	}
	double w = e[66] | (e[68] >> 4) << 8, h = e[67] | (e[68] & 15) << 8; /* mm, first detailed timing */
	if (!(e[54] | e[55]) || w < 10) w = e[21] * 10.0, h = e[22] * 10.0;
	if (w > 0 && h > 0) *inch = sqrt(w * w + h * h) / 25.4;
}

static void f_display(void)
{
	/* Only next to a running display server: on a bare console, opening the card
	 * node would make us DRM master for a moment and fbcon would redraw on close. */
	const char *x = getenv("DISPLAY"), *wl = getenv("WAYLAND_DISPLAY");
	if (!(x && *x) && !(wl && *wl)) return;
	int shown = 0;
	for (int card = 0; card < 8 && !shown; card++) {
		char path[64];
		snprintf(path, sizeof path, "/dev/dri/card%d", card);
		int fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0) continue;
		struct drm_mode_card_res res = { 0 };
		uint32_t conns[32];
		if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) || !res.count_connectors) { close(fd); continue; }
		if (res.count_connectors > 32) res.count_connectors = 32;
		struct drm_mode_card_res r2 = { .connector_id_ptr = (uintptr_t)conns, .count_connectors = res.count_connectors };
		if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &r2)) { close(fd); continue; }
		for (uint32_t i = 0; i < r2.count_connectors && i < 32; i++) {
			/* count_modes = 1: return the cached state, never trigger a probe */
			struct drm_mode_modeinfo dummy;
			struct drm_mode_get_connector gc = { .connector_id = conns[i], .count_modes = 1, .modes_ptr = (uintptr_t)&dummy };
			if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &gc) || gc.connection != 1 || !gc.encoder_id) continue;
			struct drm_mode_get_encoder ge = { .encoder_id = gc.encoder_id };
			if (ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &ge) || !ge.crtc_id) continue;
			struct drm_mode_crtc cr = { .crtc_id = ge.crtc_id };
			if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &cr) || !cr.mode_valid) continue;
			const struct drm_mode_modeinfo *md = &cr.mode;
			double hz = md->htotal && md->vtotal ? md->clock * 1000.0 / ((double)md->htotal * md->vtotal) : 0;
			if (md->flags & DRM_MODE_FLAG_INTERLACE) hz *= 2;
			if (md->flags & DRM_MODE_FLAG_DBLSCAN) hz /= 2;
			const char *tn = conn_type(gc.connector_type);
			char ep[128], name[32] = "", key[64], hzs[16];
			unsigned char edid[512];
			double inch = 0;
			snprintf(ep, sizeof ep, "/sys/class/drm/card%d-%s-%u/edid", card, tn, gc.connector_type_id);
			int efd = open(ep, O_RDONLY | O_CLOEXEC);
			if (efd >= 0) {
				ssize_t n = read(efd, edid, sizeof edid);
				close(efd);
				if (n > 0) edid_name(edid, (size_t)n, name, sizeof name, &inch);
			}
			snprintf(key, sizeof key, name[0] ? "Display (%s)" : "Display", name);
			if (fabs(hz - round(hz)) < 0.01) snprintf(hzs, sizeof hzs, "%.0f", hz);
			else snprintf(hzs, sizeof hzs, "%.2f", hz);
			int builtin = gc.connector_type == DRM_MODE_CONNECTOR_eDP || gc.connector_type == DRM_MODE_CONNECTOR_LVDS ||
				      gc.connector_type == DRM_MODE_CONNECTOR_DSI;
			if (inch > 0)
				field(key, "%ux%u @ %s Hz in %.0f\" [%s]", md->hdisplay, md->vdisplay, hzs, inch, builtin ? "Built-in" : "External");
			else
				field(key, "%ux%u @ %s Hz [%s]", md->hdisplay, md->vdisplay, hzs, builtin ? "Built-in" : "External");
			shown++;
		}
		close(fd);
	}
}

/* ---- CPU / GPU ---- */

static void f_cpu(void)
{
	char brand[49] = "";
#ifdef HAVE_CPUID
	unsigned r[12];
	if (__get_cpuid_max(0x80000000, NULL) >= 0x80000004) {
		for (unsigned i = 0; i < 3; i++) __get_cpuid(0x80000002 + i, &r[i * 4], &r[i * 4 + 1], &r[i * 4 + 2], &r[i * 4 + 3]);
		memcpy(brand, r, 48);
		brand[48] = 0;
	}
#endif
	if (!brand[0]) {
		char b[4096];
		if (rd("/proc/cpuinfo", b, sizeof b) > 0) {
			char *p = strstr(b, "model name");
			if (p && (p = strchr(p, ':'))) {
				p += 2;
				size_t n = strcspn(p, "\n");
				if (n > 48) n = 48;
				memcpy(brand, p, n);
				brand[n] = 0;
			}
		}
	}
	/* tidy: drop " w/ Radeon ...", "(R)", "(TM)", " CPU", "n-Core Processor", extra spaces */
	char *p;
	if ((p = strstr(brand, " w/ "))) *p = 0;
	if ((p = strstr(brand, " with Radeon"))) *p = 0;
	if ((p = strstr(brand, "-Core Processor"))) { while (p > brand && p[-1] != ' ') p--; *p = 0; }
	static const char *const junk[] = { "(R)", "(TM)", " CPU", " Processor", NULL };
	for (const char *const *j = junk; *j; j++)
		while ((p = strstr(brand, *j))) memmove(p, p + strlen(*j), strlen(p + strlen(*j)) + 1);
	char *s = brand;
	while (*s == ' ') s++;
	size_t l = strlen(s);
	while (l && s[l - 1] == ' ') s[--l] = 0;

	/* threads per max-frequency group, e.g. (8+16) for big + compact cores */
	long nc = sysconf(_SC_NPROCESSORS_ONLN);
	long fq[8] = { 0 }, cnt[8] = { 0 }, maxf = 0;
	int ng = 0;
	int cdir = open("/sys/devices/system/cpu", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	for (long c = 0; c < nc && c < 1024 && cdir >= 0; c++) {
		char pth[64], b[32];
		snprintf(pth, sizeof pth, "cpu%ld/cpufreq/cpuinfo_max_freq", c);
		int fd = openat(cdir, pth, O_RDONLY | O_CLOEXEC);
		ssize_t n = fd >= 0 ? read(fd, b, sizeof b - 1) : -1;
		if (fd >= 0) close(fd);
		long f = n > 0 ? (b[n] = 0, strtol(b, NULL, 10)) : -1;
		if (f <= 0) { ng = -1; break; }
		if (f > maxf) maxf = f;
		int g = 0;
		while (g < ng && labs(fq[g] - f) > f / 50) g++; /* within 2%: same core type */
		if (g == ng) {
			if (ng == 8) { ng = -1; break; }
			fq[ng++] = f;
		}
		cnt[g]++;
	}
	if (cdir >= 0) close(cdir);
	if (cdir < 0) ng = -1;
	char cores[64];
	if (ng > 1 && ng <= 3) { /* biggest cores first */
		int o[3] = { 0, 1, 2 };
		for (int a = 0; a < ng; a++)
			for (int b = a + 1; b < ng; b++)
				if (fq[o[b]] > fq[o[a]]) { int t = o[a]; o[a] = o[b]; o[b] = t; }
		int n = snprintf(cores, sizeof cores, "(");
		for (int a = 0; a < ng; a++) n += snprintf(cores + n, sizeof cores - (size_t)n, a ? "+%ld" : "%ld", cnt[o[a]]);
		snprintf(cores + n, sizeof cores - (size_t)n, ")");
	} else
		snprintf(cores, sizeof cores, "(%ld)", nc);
#ifdef HAVE_CPUID
	/* Fewer CPUs online than the chip has (nr_cpus=, maxcpus=, offlined): count the
	 * hardware instead. AMD leaf 0x80000026 gives this CPU's core type and the size
	 * of its core complex; with two core types, the rest of the chip is the other. */
	unsigned ea, eb, ec, ed;
	if (__get_cpuid_max(0x80000000, NULL) >= 0x80000026 && __get_cpuid(0x80000008, &ea, &eb, &ec, &ed) &&
	    (long)(ec & 0xff) + 1 > nc) {
		long total = (long)(ec & 0xff) + 1, cx = 0;
		int hetero = 0, type = 0;
		for (unsigned l = 0; l < 8; l++) {
			__cpuid_count(0x80000026, l, ea, eb, ec, ed);
			unsigned lt = (ec >> 8) & 0xff; /* 1 core, 2 complex, 3 die, 4 socket */
			if (!lt) break;
			if (l == 0) { hetero = ea >> 30 & 1; type = (int)(eb >> 28); }
			if (lt == 2) cx = eb & 0xffff;
		}
		if (hetero && cx > 0 && cx < total) /* performance cores first */
			snprintf(cores, sizeof cores, "(%ld+%ld)", type ? total - cx : cx, type ? cx : total - cx);
		else
			snprintf(cores, sizeof cores, "(%ld)", total);
	}
#endif
	if (maxf > 0) field("CPU", "%s %s @ %.2f GHz", s, cores, maxf / 1e6);
	else field("CPU", "%s %s", s, cores);
}

/* Name from a "ID,\tREV,\tName" line (libdrm amdgpu.ids) or pci.ids. */
static int gpu_name(unsigned ven, unsigned dev, unsigned rev, char *name, size_t n)
{
	int found = 0;
	if (ven == 0x1002) {
		int fd = open("/usr/share/libdrm/amdgpu.ids", O_RDONLY | O_CLOEXEC);
		struct stat st;
		if (fd >= 0 && !fstat(fd, &st) && st.st_size > 0) {
			char *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
			if (m != MAP_FAILED) {
				char key[24];
				int kl = snprintf(key, sizeof key, "\n%04X,\t%02X,\t", dev, rev);
				char *h = memmem(m, (size_t)st.st_size, key, (size_t)kl);
				if (h) {
					h += kl;
					size_t l = 0;
					while (h + l < m + st.st_size && h[l] != '\n' && l < n - 1) l++;
					memcpy(name, h, l);
					name[l] = 0;
					found = 1;
				}
				munmap(m, (size_t)st.st_size);
			}
		}
		if (fd >= 0) close(fd);
		if (found) return 1;
	}
	static const char *const ids[] = { "/usr/share/hwdata/pci.ids", "/usr/share/misc/pci.ids", "/usr/share/pci.ids", NULL };
	for (const char *const *f = ids; *f && !found; f++) {
		int fd = open(*f, O_RDONLY | O_CLOEXEC);
		struct stat st;
		if (fd < 0) continue;
		if (!fstat(fd, &st) && st.st_size > 0) {
			char *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
			if (m != MAP_FAILED) {
				char key[16];
				int kl = snprintf(key, sizeof key, "\n%04x  ", ven);
				char *v = memmem(m, (size_t)st.st_size, key, (size_t)kl);
				if (v) {
					char *vend = v + kl, *e = m + st.st_size;
					char vn[64];
					size_t vl = 0;
					while (vend + vl < e && vend[vl] != '\n' && vl < sizeof vn - 1) vn[vl] = vend[vl], vl++;
					vn[vl] = 0;
					char *nextv = v + 1; /* the vendor block ends at the next line that starts with a hex digit */
					while ((nextv = memchr(nextv, '\n', (size_t)(e - nextv))) && nextv + 1 < e && (nextv[1] == '\t' || nextv[1] == '#'))
						nextv++;
					kl = snprintf(key, sizeof key, "\n\t%04x  ", dev);
					char *d = memmem(v, (size_t)((nextv ? nextv : e) - v), key, (size_t)kl);
					if (d) {
						d += kl;
						size_t l = 0;
						while (d + l < e && d[l] != '\n' && l < 200) l++;
						/* short vendor: "NVIDIA Corporation" -> NVIDIA, "Advanced Micro Devices, Inc. [AMD/ATI]" -> AMD */
						const char *vs = vn;
						char *vb = strchr(vn, '[');
						if (vb) vs = vb + 1;
						int vsl = (int)strcspn(vs, vb ? "/]" : " ,");
						/* "AD102 [GeForce RTX 4090]" -> keep the bracket part when there is one */
						const char *dn = d;
						int dl = (int)l;
						char *lb = memchr(d, '[', l), *rb = lb ? memchr(lb, ']', (size_t)(d + l - lb)) : NULL;
						if (lb && rb) dn = lb + 1, dl = (int)(rb - lb - 1);
						if (dl >= vsl && !strncmp(dn, vs, (size_t)vsl)) snprintf(name, n, "%.*s", dl, dn);
						else snprintf(name, n, "%.*s %.*s", vsl, vs, dl, dn);
						found = 1;
					}
				}
				munmap(m, (size_t)st.st_size);
			}
		}
		close(fd);
	}
	return found;
}

/* An AMD APU's GPU shares its PCI slot with the platform security processor
 * (class 0x1080, "encryption controller"); a discrete card never does. Read from
 * sysfs: opening the DRM device would make amdgpu build a GPU address space. */
static int amd_apu(const char *path)
{
	char dev[PATH_MAX];
	snprintf(dev, sizeof dev, "%s", path);
	size_t l = strlen(dev);
	if (l < 2 || dev[l - 2] != '.') return -1;
	for (char fn = '0'; fn <= '7'; fn++) {
		dev[l - 1] = fn;
		char c[300];
		snprintf(c, sizeof c, "%s/class", dev);
		long cls = rdl(c);
		if (cls >= 0 && (cls >> 8) == 0x1080) return 1;
	}
	return 0;
}

static void f_gpu(void)
{
	for (int card = 0; card < 8; card++) {
		char p[96], drv[256];
		snprintf(p, sizeof p, "/sys/class/drm/card%d/device/vendor", card);
		long ven = rdl(p);
		if (ven < 0) continue;
		snprintf(p, sizeof p, "/sys/class/drm/card%d/device/device", card);
		long dev = rdl(p);
		snprintf(p, sizeof p, "/sys/class/drm/card%d/device/revision", card);
		long rev = rdl(p);
		snprintf(p, sizeof p, "/sys/class/drm/card%d/device/driver", card);
		ssize_t dl = readlink(p, drv, sizeof drv - 1);
		drv[dl > 0 ? dl : 0] = 0;
		const char *dn = strrchr(drv, '/') ? strrchr(drv, '/') + 1 : drv;
		char name[160], dpath[PATH_MAX];
		snprintf(p, sizeof p, "/sys/class/drm/card%d/device", card);
		if (!realpath(p, dpath)) dpath[0] = 0;
		size_t pl = strlen(dpath);
		if (!gpu_name((unsigned)ven, (unsigned)dev, (unsigned)(rev < 0 ? 0 : rev), name, sizeof name))
			snprintf(name, sizeof name, "%04lx:%04lx", ven, dev);
		const char *kind = "";
		if (!strcmp(dn, "amdgpu") && pl) {
			int apu = amd_apu(dpath);
			kind = apu == 1 ? " [Integrated]" : apu == 0 ? " [Discrete]" : "";
		} else if ((!strcmp(dn, "i915") || !strcmp(dn, "xe")) && pl > 12) /* Intel iGPUs are always 00:02.0 */
			kind = !strcmp(dpath + pl - 12, "0000:00:02.0") ? " [Integrated]" : " [Discrete]";
		else if (!strcmp(dn, "nvidia") || !strcmp(dn, "nouveau"))
			kind = " [Discrete]";
		field("GPU", "%s%s", name, kind);
	}
}

static void f_memory(void)
{
	struct mem m;
	meminfo(&m);
	char u[32], t[32];
	long used = real_used(&m);
	size_str(u, sizeof u, used * 1024.0);
	size_str(t, sizeof t, m.total * 1024.0);
	field("Memory", "%s / %s (%.0f%%)", u, t, m.total ? 100.0 * used / m.total : 0);
	if (m.swtotal > 0) {
		size_str(u, sizeof u, (m.swtotal - m.swfree) * 1024.0);
		size_str(t, sizeof t, m.swtotal * 1024.0);
		field("Swap", "%s / %s (%.0f%%)", u, t, 100.0 * (m.swtotal - m.swfree) / m.swtotal);
	}
}

/* XFS geometry (XFS_IOC_FSGEOMETRY, v5 layout; any user may ask). */
struct xfs_geom {
	uint32_t blocksize, rtextsize, agblocks, agcount, logblocks, sectsize, inodesize, imaxpct;
	uint64_t datablocks, rtblocks, rtextents, logstart;
	unsigned char uuid[16];
	uint32_t sunit, swidth;
	int32_t version;
	uint32_t flags, logsectsize, rtsectsize, dirblocksize, logsunit, sick, checked, rgextents, rgcount;
	uint64_t rtstart, rtreserved, reserved[14];
};
#define XFS_IOC_FSGEOMETRY _IOR('X', 126, struct xfs_geom)
#define XFS_GEOM_V5SB (1u << 15)
#define XFS_GEOM_FINOBT (1u << 17)
#define XFS_GEOM_RMAPBT (1u << 19)
#define XFS_GEOM_REFLINK (1u << 20)

/* Blocks of a btree holding `recs` records at minimum fill (the kernel's xfs_btree_calc_size). */
static uint64_t btree_blocks(uint64_t recs, unsigned leafmin, unsigned nodemin)
{
	uint64_t level = recs, blocks = 0;
	for (int l = 0; level > 1; l++) {
		unsigned m = l ? nodemin : leafmin;
		level = (level + m - 1) / m;
		blocks += level;
	}
	return blocks;
}

/* Blocks XFS holds back in every allocation group so its reverse-mapping, refcount
 * and free-inode btrees can always grow (the kernel's per-AG reservations). statfs
 * counts them as used; on a 937 GiB disk they are 18 GiB. Same formulas as
 * xfs_rmapbt/refcountbt/finobt_calc_reserves; checked against GETFSMAP. */
static uint64_t xfs_reserved_blocks(const char *path)
{
	int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	struct xfs_geom g;
	memset(&g, 0, sizeof g);
	int ok = fd >= 0 && !ioctl(fd, XFS_IOC_FSGEOMETRY, &g);
	if (fd >= 0) close(fd);
	if (!ok || !g.agcount || !g.agblocks || !g.blocksize || !g.inodesize) return 0;
	unsigned room = g.blocksize - ((g.flags & XFS_GEOM_V5SB) ? 56 : 16); /* short btree block header */
	unsigned rmap_leaf = room / 24 / 2, rmap_node = room / 44 / 2;   /* record; low+high key and pointer */
	unsigned refc_leaf = room / 12 / 2, refc_node = room / 8 / 2;
	unsigned ino_leaf = room / 16 / 2, ino_node = room / 8 / 2;
	uint64_t inopblock = g.blocksize / g.inodesize, total = 0;
	unsigned agbits = 0; /* AG number of the internal log: fsblock = agno << log2(agblocks rounded up) | agbno */
	while ((1ull << agbits) < g.agblocks) agbits++;
	for (uint32_t ag = 0; ag < g.agcount; ag++) {
		uint64_t len = ag + 1 < g.agcount ? g.agblocks : g.datablocks - (uint64_t)(g.agcount - 1) * g.agblocks;
		if (g.logstart && (g.logstart >> agbits) == ag && len > g.logblocks) len -= g.logblocks;
		if (g.flags & XFS_GEOM_RMAPBT) {
			uint64_t r = btree_blocks(len, rmap_leaf, rmap_node);
			total += r > len / 100 ? r : len / 100;
		}
		if (g.flags & XFS_GEOM_REFLINK) total += btree_blocks(len, refc_leaf, refc_node);
		if (g.flags & XFS_GEOM_FINOBT) total += btree_blocks(len * inopblock / 64, ino_leaf, ino_node);
	}
	return total;
}

static void f_disk(void)
{
	struct statvfs v;
	struct statfs f;
	if (statvfs("/", &v) || statfs("/", &f)) return;
	double tot = (double)v.f_blocks * v.f_frsize, used = (double)(v.f_blocks - v.f_bfree) * v.f_frsize;
	if ((unsigned long)f.f_type == 0x58465342) { /* XFS: leave out its metadata reservations */
		double resv = (double)xfs_reserved_blocks("/") * v.f_frsize;
		used = used > resv ? used - resv : 0;
	}
	static const struct { unsigned long m; const char *n; } fs[] = {
		{ 0x58465342, "xfs" }, { 0xEF53, "ext4" }, { 0x9123683E, "btrfs" }, { 0xF2F52010, "f2fs" }, { 0x2FC12FC1, "zfs" },
		{ 0xCA451A4E, "bcachefs" }, { 0x01021994, "tmpfs" }, { 0x794C7630, "overlay" }, { 0x6E736673, "nilfs2" } };
	const char *fn = "";
	for (size_t i = 0; i < sizeof fs / sizeof *fs; i++)
		if ((unsigned long)f.f_type == fs[i].m) fn = fs[i].n;
	char u[32], t[32];
	size_str(u, sizeof u, used);
	size_str(t, sizeof t, tot);
	field("Disk (/)", fn[0] ? "%s / %s (%.0f%%) - %s" : "%s / %s (%.0f%%)", u, t, tot > 0 ? 100.0 * used / tot : 0, fn);
}

static void f_ip(void)
{
	struct ifaddrs *ifa, *i;
	if (getifaddrs(&ifa)) return;
	for (i = ifa; i; i = i->ifa_next) {
		if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_UP)) continue;
		char a[INET_ADDRSTRLEN], key[48];
		inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, a, sizeof a);
		uint32_t mask = i->ifa_netmask ? ntohl(((struct sockaddr_in *)i->ifa_netmask)->sin_addr.s_addr) : 0;
		snprintf(key, sizeof key, "Local IP (%s)", i->ifa_name);
		field(key, "%s/%d", a, __builtin_popcount(mask));
		break;
	}
	freeifaddrs(ifa);
}

static void f_battery(void)
{
	DIR *d = opendir("/sys/class/power_supply");
	struct dirent *e;
	while (d && (e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		char p[300], b[64], model[64] = "", st[32] = "";
		snprintf(p, sizeof p, "/sys/class/power_supply/%s/type", e->d_name);
		if (rd(p, b, sizeof b) <= 0 || strcmp(b, "Battery")) continue;
		snprintf(p, sizeof p, "/sys/class/power_supply/%s/scope", e->d_name);
		if (rd(p, b, sizeof b) > 0 && !strcmp(b, "Device")) continue; /* mice, headsets */
		snprintf(p, sizeof p, "/sys/class/power_supply/%s/capacity", e->d_name);
		long cap = rdl(p);
		snprintf(p, sizeof p, "/sys/class/power_supply/%s/model_name", e->d_name);
		rd(p, model, sizeof model);
		snprintf(p, sizeof p, "/sys/class/power_supply/%s/status", e->d_name);
		rd(p, st, sizeof st);
		snprintf(p, sizeof p, "/sys/class/power_supply/%s/power_now", e->d_name);
		long pw = !strcmp(st, "Discharging") ? rdl(p) : -1;
		char key[96];
		snprintf(key, sizeof key, model[0] ? "Battery (%s)" : "Battery", model);
		if (pw > 0) field(key, "%ld%% [%s, %.1f W]", cap, st, pw / 1e6);
		else field(key, "%ld%% [%s]", cap, st);
	}
	if (d) closedir(d);
}

static void f_plan(void)
{
	char b[64]; /* the owner's power plans (powerprofile): "plan pinned|auto" */
	if (rd("/run/powerprofile", b, sizeof b) <= 0) return;
	char *sp = strchr(b, ' ');
	if (sp) *sp = 0;
	field("Power plan", sp ? "%s (%s)" : "%s", b, sp ? sp + 1 : "");
}

static void f_locale(void)
{
	const char *l = getenv("LC_ALL");
	if (!l || !*l) l = getenv("LC_CTYPE");
	if (!l || !*l) l = getenv("LANG");
	if (l && *l) field("Locale", "%s", l);
}

/* Gentoo logo (same art as neofetch/fastfetch); \1 = magenta, \2 = white */
static const char *const logo[] = {
	"\1         -/oyddmdhs+:.",
	"\1     -o\2dNMMMMMMMMNNmhy+\1-`",
	"\1   -y\2NMMMMMMMMMMMNNNmmdhy\1+-",
	"\1 `o\2mMMMMMMMMMMMMNmdmmmmddhhy\1/`",
	"\1 om\2MMMMMMMMMMMN\1hhyyyo\2hmdddhhhd\1o`",
	"\1.y\2dMMMMMMMMMMd\1hs++so/s\2mdddhhhhdm\1+`",
	"\1 oy\2hdmNMMMMMMMN\1dyooy\2dmddddhhhhyhN\1d.",
	"\1  :o\2yhhdNNMMMMMMMNNNmmdddhhhhhyym\1Mh",
	"\1    .:\2+sydNMMMMMNNNmmmdddhhhhhhmM\1my",
	"\1       /m\2MMMMMMNNNmmmdddhhhhhmMNh\1s:",
	"\1    `o\2NMMMMMMMNNNmmmddddhhdmMNhs\1+`",
	"\1  `s\2NMMMMMMMMNNNmmmdddddmNMmhs\1/.",
	"\1 /N\2MMMMMMMMNNNNmmmdddmNMNdso\1:`",
	"\1+M\2MMMMMMNNNNNmmmmdmNMNdso\1/-",
	"\1yM\2MNNNNNNNmmmmmNNMmhs+/\1-`",
	"\1/h\2MMNNNNNNNNMNdhs++/\1-`",
	"\1`/\2ohdmmddhys+++/:\1.`",
	"\1  `-//////:--.",
};

int main(int argc, char **argv)
{
	int brk = 0, nologo = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-m")) brk = 1;
		else if (!strcmp(argv[i], "-n")) nologo = 1;
		else {
			static const char u[] = "usage: rfetch [-m] [-n]\n  -m  memory breakdown\n  -n  no logo\n";
			(void)!write(2, u, sizeof u - 1);
			return 2;
		}
	}
	const char *nc = getenv("NO_COLOR"), *cf = getenv("CLICOLOR_FORCE");
	color = (isatty(1) || (cf && *cf && strcmp(cf, "0"))) && !(nc && *nc);
	if (brk) {
		mem_breakdown();
		return write(1, out, olen) == (ssize_t)olen ? 0 : 1;
	}

	struct utsname u;
	uname(&u);
	const char *user = getenv("USER");
	if (!user || !*user) user = getenv("LOGNAME");
	if (!user) user = "?";
	char title[160];
	int tl = snprintf(title, sizeof title, "%s@%s", user, u.nodename);
	if (color) snprintf(lines[nlines++], LINELEN, "\033[1;35m%s\033[0m@\033[1;35m%s\033[0m", user, u.nodename);
	else snprintf(lines[nlines++], LINELEN, "%s", title);
	memset(lines[nlines], '-', (size_t)(tl < LINELEN - 1 ? tl : LINELEN - 1));
	nlines++;

	char wm[64] = "";
	f_os(&u);
	f_host();
	field("Kernel", "Linux %s", u.release);
	f_uptime();
	f_packages();
	f_shell_term(wm, sizeof wm);
	f_wm(wm);
	f_display();
	f_cpu();
	f_gpu();
	f_memory();
	f_disk();
	f_ip();
	f_battery();
	f_plan();
	f_locale();
	if (color && nlines + 3 <= MAXLINES) {
		nlines++; /* blank */
		char *a = lines[nlines++], *b = lines[nlines++];
		int na = 0, nb = 0;
		for (int i = 0; i < 8; i++) {
			na += snprintf(a + na, LINELEN - (size_t)na, "\033[4%dm   ", i);
			nb += snprintf(b + nb, LINELEN - (size_t)nb, "\033[10%dm   ", i);
		}
		snprintf(a + na, LINELEN - (size_t)na, "\033[0m");
		snprintf(b + nb, LINELEN - (size_t)nb, "\033[0m");
	}

	int nl = nologo ? 0 : (int)(sizeof logo / sizeof *logo), logo_w = 0;
	for (int r = 0; r < nl; r++) {
		int w = 0;
		for (const char *p = logo[r]; *p; p++) w += *p > 2;
		if (w > logo_w) logo_w = w;
	}
	int rows = nl > nlines ? nl : nlines;
	for (int r = 0; r < rows; r++) {
		if (nl) {
			int w = 0;
			if (r < nl)
				for (const char *p = logo[r]; *p; p++) {
					if (*p == 1 || *p == 2) {
						if (color) emit(*p == 1 ? "\033[35m" : "\033[37m");
					} else {
						out[olen < sizeof out - 1 ? olen++ : olen] = *p;
						w++;
					}
				}
			if (color && r < nl) emit("\033[0m");
			emit("%*s", logo_w - w + 3, "");
		}
		if (r < nlines) emit("%s", lines[r]);
		emit("\n");
	}
	return write(1, out, olen) == (ssize_t)olen ? 0 : 1;
}
