# clean_metadata

A fast, multi-threaded C command-line tool designed to clean up macOS and Windows hidden metadata, clutter, and housekeeping files across connected storage drives.

It performs a single-pass recursive scan using POSIX threads (`pthread`), presents matched items for user confirmation, and safely deletes them.

---

## Features

* **Single-Pass Recursive Scan:** Evaluates file patterns across directory trees without invoking sub-shell commands or redundant system loops.
* **Multi-Threaded Work Queue:** Leverages a dynamic `pthread` task pool to scan large directory trees and external storage drives efficiently.
* **Targeted Cleanup:** Specifically searches for known macOS and Windows metadata files, leaving user data untouched.
* **Interactive Confirmation:** Lists all matching paths and prompts for user permission before executing any delete operations.
* **Recursive Folder Deletion:** Safely cleans out directory structures (such as `.Trashes` or `.fseventsd`) via POSIX calls (`unlink`/`rmdir`).
* **Index Inhibition:** Automatically creates a `.metadata_never_index` marker file at the root of the targeted drive to prevent macOS Spotlight from re-indexing the volume.

---

## Targeted Patterns

`clean_metadata` matches and cleans the following hidden metadata files and directories:

| Category | Targeted File / Folder Patterns |
| :--- | :--- |
| **macOS System & Finder** | `.DS_Store`, `.AppleDouble`, `.Trashes`, `.fseventsd`, `.TemporaryItems`, `.VolumeIcon.icns`, `.localized` |
| **Spotlight & Indexing** | `.Spotlight-V100`, `.DocumentRevisions-V100`, `VolumeConfiguration.plist` |
| **Time Machine & Backup** | `.com.apple.timemachine.donotpresent`, `.com.apple.timemachine.supported`, `.apdisk`, `.MobileBackups`, `.MobileBackups.trash` |
| **Windows Explorer** | `Thumbs.db`, `ehthumbs.db` |

---

## Installation

### Prerequisites

* C Compiler (`gcc` or `clang`)
* POSIX Threads support (`pthread`)
* macOS or Linux operating system

### Compilation

Clone the repository and compile using `gcc` or `clang`:

```bash
# Clone repository
git clone [https://github.com/your-username/clean_metadata.git](https://github.com/your-username/clean_metadata.git)
cd clean_metadata

# Compile with optimization and pthread support
gcc -O2 -pthread clean_metadata.c -o clean_metadata
