# Changelog

## 0.1.0

- `clone` and `cloneMany` clone files, symbolic links and directory trees
  copy-on-write, in one child process, and report per pair whether it
  cloned and why not: `unsupported`, `cross-device` or `failed` with an
  errno name.
- macOS: one `clonefile(2)` call per pair. Linux: `ioctl(FICLONE)` per file
  on btrfs, XFS and bcachefs. Windows: `FSCTL_DUPLICATE_EXTENTS_TO_FILE` per
  file on ReFS and Dev Drive.
- The `cow-clone` command, with `--json` and `--stdin`.
