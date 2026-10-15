# rfetch

A system information tool in the style of fastfetch, in one C file, that reports the memory that is really in use. fastfetch, free and neofetch count some memory that is actually free as "used": the free pages each CPU keeps for fast allocation, the GPU driver's pool of freed pages and, in fastfetch's case, the kernel's watermark reserve. rfetch subtracts them. `rfetch -m` shows where the memory goes. It shows the same fields as fastfetch's default output and runs in about a third of its time, because it doesn't start other programs or load libraries. It reads `/proc`, `/sys`, the DRM driver and the CPU's CPUID instruction directly.

**Status:** in daily use on an HP OmniBook 7 (Ryzen AI 9 HX 475, Radeon 890M) with Gentoo and dwm, where it replaced fastfetch. Linux only; the package count is Gentoo's (`/var/db/pkg`).

![Terminal output on a black background. "rfetch" prints the Gentoo logo in magenta and white, and next to it: rec@gentoo; OS: Gentoo Linux x86_64; Host: HP OmniBook 7 Laptop 14-kh0xxx; Kernel: Linux 7.2.10-gentoo-min; Uptime: 50 mins; Packages: 214 (emerge); Shell: bash 5.3.20; Terminal: script; WM: dwm (X11); Display (SDC4215): 2880x1800 @ 120 Hz in 14" [Built-in]; CPU: AMD Ryzen AI 9 HX 475 (8+16) @ 5.30 GHz; GPU: AMD Radeon 890M Graphics [Integrated]; Memory: 1.45 GiB / 54.58 GiB (3%); Swap: 0 B / 16.00 GiB (0%); Disk (/): 25.13 GiB / 936.41 GiB (3%) - xfs; Local IP (wlan0): 192.168.0.240/24; Battery (Primary): 87% [Discharging, 4.5 W]; Power plan: balanced (auto); Locale: en_US.utf8; and two rows of colour blocks. "rfetch -m" then prints "Really in use: 1484.8 MiB of 54.58 GiB", split into programs 918.4 MiB, kernel 257.2 MiB (slab 173.6, stacks 6.8, page tables 15.7, per-CPU 11.6, vmalloc 49.5), GPU 22.0 MiB and drivers/other 287.1 MiB; then what isn't counted: page cache 1245.9 MiB, reclaimable slab 304.3 MiB, per-CPU free 42.2 MiB, GPU page pool 20.4 MiB; and what other tools would show: fastfetch/free 1594.0 MiB, neofetch/htop 1547.4 MiB. Claude Code was running, which is most of the 918 MiB of programs; the terminal shows as "script" because the screenshot was rendered from output captured through script(1).](docs/images/rfetch.png)

## Features

- **Memory as it really is.** "Used" is `MemTotal - MemFree - Buffers - Cached - SReclaimable + Shmem` (what neofetch and htop count), minus two kinds of free memory that `/proc/meminfo` doesn't call free (see How it works). On the laptop it was 110–135 MiB lower than fastfetch with Claude Code running; the gap shrinks when the system is quieter, because fewer free pages wait on the per-CPU lists.
- **`rfetch -m`: where the memory goes.** It shows four parts: programs (anonymous memory plus tmpfs), the kernel (slab, stacks, page tables, per-CPU areas, vmalloc), GPU buffers in system RAM, and memory drivers took directly. Then what isn't counted and why, and what fastfetch and neofetch would show for the same moment.
- **fastfetch's fields:** user and host, OS, host model, kernel, uptime, package count, shell with version, terminal, window manager, each display's resolution, refresh rate and size, CPU with core types (`8+16` = 8 threads on full cores and 16 on compact ones) and top clock, GPU with its marketing name, memory, swap, root disk, local IP, battery with the draw in watts while discharging, and locale.
- **The power plan** from the `powerprofile` script's state file (`/run/powerprofile`), when it exists.
- **Fast and small:** about 1 ms of work and 1 MiB of memory; one static binary, nothing to configure.

## How to install

Requires a C compiler, make, Linux kernel headers (for the DRM structures) and glibc's static libraries (or build it dynamically with `make LDFLAGS=`).

```sh
git clone https://github.com/r3clusionn/rfetch
cd rfetch
make install            # to ~/.local/bin; PREFIX=/usr/local for system-wide
```

## How to use

```sh
rfetch          # logo and information
rfetch -n       # without the logo
rfetch -m       # where the memory goes
```

Colours are on when the output is a terminal. `NO_COLOR=1` turns them off and `CLICOLOR_FORCE=1` keeps them in a pipe. To show it in every new terminal, put `rfetch` at the end of `~/.bashrc`.

## How it works

- **Memory.** One read of `/proc/meminfo` and one of `/proc/zoneinfo`. Two amounts are free memory that meminfo doesn't count as free:
  - The kernel keeps a few free pages on a list per CPU so allocations don't need a lock. They are taken out of `MemFree` while they wait there. rfetch adds up `count:` under each CPU's pageset in zoneinfo; on a 24-thread laptop it was 30–70 MiB.
  - GPU drivers that use TTM (amdgpu, radeon, nouveau, xe) keep freed pages in a pool for the next buffer; the kernel takes them back under memory pressure. Newer kernels report them as `GPUReclaim` in meminfo.
  - fastfetch and free use `MemTotal - MemAvailable`. MemAvailable also leaves out the reserve the kernel keeps for emergencies (the watermarks), so their "used" includes it.
  - Not subtracted: memory a driver allocated on its own, such as an SSD's host memory buffer (64 MiB on a DRAM-less SSD) or network DMA rings. It is really in use, so it shows under drivers/other.
- **No other programs.** The shell version comes from its package (`app-shells/bash-5.3_p20` gives 5.3.20). The terminal and window manager come from the process tree: the shell's parent is the terminal, and the terminal's parent is usually the window manager. Only when it isn't does rfetch look through your own processes for a known window manager.
- **Display** from the DRM driver: the connector, encoder and CRTC ioctls give the mode that is actually on the screen. The connector query passes a non-zero mode count, so the kernel returns its cached state and never probes the monitor. The name and size come from the EDID in sysfs. Without a running X or Wayland session the display line is skipped, so that rfetch never briefly becomes DRM master on a text console.
- **CPU** name from CPUID. Core types are grouped by each CPU's maximum clock in cpufreq.
- **GPU** name from libdrm's `amdgpu.ids` (AMD's marketing names, matched by device and revision) or from `pci.ids`. An AMD GPU counts as integrated when the platform security processor sits in the same PCI slot, which is true of every APU and never of a graphics card. Opening the GPU device to ask the driver costs 0.6 ms on amdgpu, because the driver sets up a GPU address space for each opener.
- **Packages** by counting `/var/db/pkg/*/*` with raw `getdents64` calls.
- **Output** is assembled in memory and written with one `write()`.

## Measurements

On the laptop (Ryzen AI 9 HX 475, Gentoo, 7.2 kernel), on battery with the balanced plan:

| | rfetch | fastfetch 2.65.2 |
|---|---|---|
| Time per run, including starting the process | 1.6 ms | 4.3 ms |
| Starting an empty program (`/bin/true`), for reference | 0.6–0.7 ms | |
| Peak memory | 1.0 MiB | 4.2 MiB |
| Binary | 721 KiB, static | 1.6 MiB, plus libyyjson and the libraries it loads at run time |
| Memory "used" at the same moment | 1484.8 MiB | 1594.0 MiB |

A dynamic build of rfetch takes 2.0 ms per run, so the static one is the default. Time per field, measured inside the program: memory 180 µs (most of it the kernel generating zoneinfo), display 230–265 µs (opening the DRM device), packages 150 µs, CPU 125 µs, everything else under 90 µs each.

## Verification

- Package count against `ls -d /var/db/pkg/*/* | wc -l`: both 214.
- Every field compared with fastfetch's output on the same machine: OS, host, kernel, uptime, packages, shell version, display (SDC4215, 2880x1800, 120 Hz, 14"), CPU (8+16, 5.30 GHz), GPU name, swap, disk, IP, battery and locale match.
- Memory: the "fastfetch/free" line of `rfetch -m` against `free -k` run right after it, three times: 1567.4/1570.1, 1570.9/1571.2, 1571.2/1571.6 MiB (the difference is what changed between the two commands). drivers/other is the remainder after the parts meminfo names.
- GPU names for other cards through the same lookup: NVIDIA GeForce RTX 4090, Intel Arc A770, Intel Arc Graphics, AMD Radeon RX 7900 XTX, AMD Radeon RX 6800 XT.

## Limits

- Linux with glibc. CPUID needs x86; elsewhere the CPU name comes from `/proc/cpuinfo`.
- Packages are counted for Portage only.
- The terminal is whatever the shell's parent process is called: a terminal that starts the shell through a helper shows the helper's name.
- Fields are fixed in the source; there is no configuration file.

## License

MIT (see `LICENSE`).
