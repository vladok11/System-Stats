# system_stats

A lightweight, highly optimized C-based system resource monitor for Linux. Designed with a focus on minimal CPU and RAM footprint.

---

## Features

- Lightweight: Zero dependencies on heavy third-party libraries, directly interfaces with Linux `/proc` filesystem.
- Low Footprint: Uses integer arithmetic and adaptive timers to minimize CPU and memory overhead.
- Nix Support: Includes `flake.nix` for building and running on NixOS/Nix.
- Flexible: Interactive real-time watch mode or single-shot telemetry output.

---

## Building and Installation

### Option 1: Via Make (Standard)

Requirements: gcc or clang, make.

1. Build the binary:
   make

2. Install system-wide (default `/usr/local/bin`):
   sudo make install

3. Uninstall:
   sudo make uninstall

---

### Option 2: Direct GCC Compilation

gcc -Wall -Wextra -O2 system_stats.c -o system_stats

---

### Option 3: Using Nix / NixOS

Build and run using Nix Flakes:

# Build the project
nix build

# Run without installing
nix run

---

## Usage

### Interactive Real-Time Mode (Watch mode)

Run with specified refresh delay (in milliseconds):

# Update every 1000 ms (1 second)
./system_stats -w 1000

# Update every 500 ms (0.5 seconds)
./system_stats -w 500

### Single-Shot Mode

Output current system status once and exit:

# Output core metrics (CPU, RAM, Processes)
./system_stats

# Output all available metrics
./system_stats -a

---

## Flags and Options

-w <ms>, --watch <ms>   : Interactive mode with update delay in milliseconds
-a, --all              : Enable all available sections
-c, --cpu              : CPU usage section
-r, --ram              : RAM and Swap usage section
-p, --procs            : Process list
-t, --threads          : Thread count information
-d, --disk             : Disk I/O stats
-n, --net              : Network traffic stats
-T, --temps            : Temperature sensors
-i, --info             : System info (kernel, uptime)
-l <num>, --limit      : Process list limit (default: 15)
--sort-cpu             : Sort process list by CPU usage
--sort-mem             : Sort process list by RAM usage
--sort-io              : Sort process list by Disk I/O
-h, --help             : Print help information

---

## Navigation in Interactive Mode (-w)

- Arrow Up / Down : Scroll list up / down
- PgUp / PgDn     : Scroll page up / page down
- Home / End      : Jump to top / bottom of list
- Mouse Scroll    : Scroll list
- Q               : Quit application

---

## License

This project is licensed under the GNU General Public License v3.0 (GPL-3.0). See the [LICENSE](LICENSE) file for details.