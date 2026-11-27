# Changelog

## 0.1.1

- Linux and Windows clone files on several threads, with fewer system calls
  per file: modes and times are set on the open file, and Windows only touches
  integrity streams when the source or destination uses them. On the CI bench
  (40,014 files) XFS went from 1.62x to 0.98x the time of `fs.cpSync` and ReFS
  from 1.58x to 1.17x, while cloning where `fs.cpSync` copies.

## 0.1.0

- `clone` and `cloneMany` clone files, symbolic links and directory trees
  copy-on-write, in one child process, and report per pair whether it
  cloned and why not: `unsupported`, `cross-device` or `failed` with an
  errno name.
- macOS: one `clonefile(2)` call per pair. Linux: `ioctl(FICLONE)` per file
  on btrfs, XFS and bcachefs. Windows: `FSCTL_DUPLICATE_EXTENTS_TO_FILE` per
  file on ReFS and Dev Drive.
- The `cow-clone` command, with `--json` and `--stdin`.
