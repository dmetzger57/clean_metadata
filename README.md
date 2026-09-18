# clean_metadata

A fast, multi-threaded C command-line tool designed to clean up macOS and Windows hidden metadata, clutter, and housekeeping files across connected storage drives.

It performs a parallel recursive scan using POSIX threads (`pthread`), presents matched items for user confirmation, and safely deletes them — also in parallel.

---

## Features

* **Parallel Recursive Scan:** A `pthread` work-queue of worker threads (sized to the number of CPU cores) walks directory trees concurrently, without invoking sub-shell commands or redundant system loops.
* **Fast Directory Detection:** Uses the directory-entry type reported by `readdir()` to tell files from directories, falling back to `lstat()` only when the filesystem doesn't provide it — avoiding an extra syscall per entry on most filesystems.
* **O(1) Pattern Matching:** Target patterns are indexed in a hash set, so matching scales with directory size regardless of how many patterns are configured.
* **Targeted Cleanup:** Specifically searches for known macOS and Windows metadata files, leaving user data untouched.
* **Interactive Confirmation:** Lists all matching paths and prompts for user permission before executing any delete operations.
* **Parallel Recursive Deletion:** Matched files and folders (such as `.Trashes` or `.fseventsd`) are removed concurrently across worker threads via POSIX calls (`unlink`/`rmdir`), so large matched trees don't serialize deletion on a single thread.
* **Index Inhibition:** Automatically creates a `.metadata_never_index` marker file at the root of the targeted drive to prevent macOS Spotlight from re-indexing the volume.
* **Customizable Patterns:** Optionally override the built-in target list with your own, via `~/.clean_metadata_patterns`.

---

## Targeted Patterns

By default, `clean_metadata` matches and cleans the following hidden metadata files and directories:

| Category | Targeted File / Folder Patterns |
| :--- | :--- |
| **macOS System & Finder** | `.DS_Store`, `.AppleDouble`, `.Trashes`, `.fseventsd`, `.TemporaryItems`, `.VolumeIcon.icns`, `.localized` |
| **Spotlight & Indexing** | `.Spotlight-V100`, `.DocumentRevisions-V100`, `VolumeConfiguration.plist` |
| **Time Machine & Backup** | `.com.apple.timemachine.donotpresent`, `.com.apple.timemachine.supported`, `.apdisk`, `.MobileBackups`, `.MobileBackups.trash` |
| **Windows Explorer** | `Thumbs.db`, `ehthumbs.db` |

### Custom Patterns

If `~/.clean_metadata_patterns` exists and contains at least one valid entry, it replaces the built-in list entirely. Format is one pattern per line:

```
# Lines starting with # are comments and are ignored
.DS_Store
.Trashes
my-custom-cache-folder
```

Blank lines and comment lines (`#`) are skipped. If the file is missing or has no valid entries, the built-in patterns above are used instead.

---

## Installation

### Prerequisites

* C Compiler (`gcc` or `clang`)
* POSIX Threads support (`pthread`)
* macOS or Linux operating system

### Build

Clone the repository and build with `make`:

```bash
git clone https://github.com/dmetzger57/clean_metadata.git
cd clean_metadata

# Build (uses -O2 -pthread, see Makefile)
make

# Optional: install to ~/bin
make install

# Remove the built binary
make clean
```

Or compile directly without `make`:

```bash
gcc -O2 -pthread clean_metadata.c -o clean_metadata
```

---

## Usage

```bash
./clean_metadata <path-to-scan>
```

The tool scans the given path, prints every matched file/folder it finds, and asks for confirmation before deleting anything:

```
Scanning: /Volumes/MyDrive

The following 3 item(s) were found:
  /Volumes/MyDrive/.DS_Store
  /Volumes/MyDrive/photos/.DS_Store
  /Volumes/MyDrive/.Spotlight-V100

Delete these items? [y/N]: y

Deleting...
  Removing: /Volumes/MyDrive/.DS_Store
  Removing: /Volumes/MyDrive/photos/.DS_Store
  Removing: /Volumes/MyDrive/.Spotlight-V100
Created index inhibition marker: /Volumes/MyDrive/.metadata_never_index

Cleanup complete.
```

Answering anything other than `y`/`Y` cancels the operation without deleting or modifying anything.

**Warning:** Deletion is permanent (no trash/recycle bin involved) and matched directories are removed recursively. Review the listed items before confirming, especially if you're using a custom pattern file.

### Scripting / non-interactive use

```bash
./clean_metadata --scan <path-to-scan>
```

Performs the same scan with no banner, no prompt, and no deletion — it just prints every matched path to stdout, NUL-delimited (like `find -print0`), and exits. This is what the [GUI app](#gui-app-macos) uses under the hood; it's also handy for scripting, e.g.:

```bash
./clean_metadata --scan ~/Desktop | xargs -0 -n1 echo "would remove:"
```

---

## GUI App (macOS)

A native SwiftUI app wraps the CLI for a point-and-click workflow: choose a folder, scan it, review/uncheck matched items, and remove only what you approve.

1. **Choose Folder…** opens a standard folder picker.
2. **Scan** runs `clean_metadata --scan` against it and lists every match, each with a checkbox (checked by default).
3. Uncheck anything you want to keep, or use **Select All** / **Select None**.
4. **Remove Selected (N)** asks for confirmation, then deletes the approved items — concurrently — and shows a ✓ or ✗ next to each as it finishes. On success it also creates the `.metadata_never_index` marker at the scanned root, same as the CLI.

The app doesn't reimplement scanning — it shells out to the compiled `clean_metadata` binary (bundled alongside it) so scans get the same fast, multithreaded scan engine documented below; deletion of the items you approve happens natively in the app, concurrently across the selected items.

### Prerequisites

* Everything under [Installation](#installation) above (to build the CLI binary the app bundles)
* Swift toolchain (Xcode or Xcode Command Line Tools — `xcode-select --install`); this repo was built/tested against Swift 6.4
* macOS 13 (Ventura) or later

### Build & install

```bash
make install-gui-app
```

This builds the CLI, builds the SwiftUI app (`swift build -c release` in `gui/`), assembles `Clean Metadata.app` (CLI binary bundled in `Contents/Resources`), and copies it to `~/Applications/Clean Metadata.app`. `make gui-app` builds the bundle in the repo directory without installing it.

The app isn't code-signed (beyond the ad-hoc signature the linker adds automatically). If macOS refuses to open it, right-click → **Open** once, or:

```bash
xattr -dr com.apple.quarantine "$HOME/Applications/Clean Metadata.app"
```

As with the CLI, scanning a protected location (Desktop, Documents, Downloads, an external volume, etc.) may prompt for permission under **System Settings → Privacy & Security → Files and Folders** the first time — this is normal macOS sandboxing, not specific to this app.

---

## Performance

`clean_metadata` is built to stay fast on large directory trees and external drives:

* Scanning and deletion both use a worker-thread pool sized to the host's CPU count (`sysconf(_SC_NPROCESSORS_ONLN)`).
* Directory vs. file checks prefer `d_type` from `readdir()` over an `lstat()` syscall per entry.
* Pattern matching is a hash-set lookup rather than a linear scan, so large custom pattern files don't slow down scanning.
* Output is fully buffered, so large result listings don't incur a syscall per printed line.
