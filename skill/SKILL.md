---
name: cow-clone
description: Use cow-clone to copy a large folder or file (node_modules, build outputs, caches, a checkout, a simulator or VM image) as a copy-on-write clone that takes no extra disk space and finishes in a fraction of a copy's time, on macOS (APFS), Linux (btrfs, XFS with reflink, bcachefs) and Windows (ReFS, Dev Drive). Use it instead of `cp -R`, `cp -Rc`, `cp --reflink`, `robocopy` or `fs.cp` when both paths are on the same volume.
---

# cow-clone

A clone shares every block with its source until one side changes, so cloning a 4 GB `node_modules` uses almost no space. `cow-clone` clones files and whole trees and says when it could not, so you can copy instead.

```bash
npx cow-clone <source> <destination> [<source> <destination>]...
npx cow-clone --json <source> <destination>...   # { results: [{ cloned, reason?, code?, message?, path? }] }
```

On Windows in cmd.exe or PowerShell the command is `cow-clone-windows` (`npx --package cow-clone cow-clone-windows <source> <destination>`).

- Each destination must not exist; its parent must.
- Exit status 0: every pair cloned. 1: at least one did not; stderr names each with its errno code, and the other pairs still ran. 2: a usage error.
- A failed pair leaves no partial destination behind.

When a pair is not cloned, read the reason (with `--json`, the `reason` field):

- `unsupported`: the file system cannot clone (ext4, tmpfs, NTFS, HFS+, FAT, most network shares). Copy instead with `cp -R` (`cp -a` to keep modes and times), `robocopy` or `fs.cp`, and say the copy takes real space.
- `cross-device`: the two paths are on different volumes. No tool can clone across volumes; copy, or pick a destination on the source's volume.
- `failed`: another error, such as `EEXIST` (the destination exists), `ENOENT` or `EACCES`. Fix the cause; do not delete an existing destination to make room unless the user asked for that.

Symbolic links and Windows junctions are cloned as links, never followed. Modes, times and Windows attributes are kept; ownership, ACLs and Linux extended attributes are not.
