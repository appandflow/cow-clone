import assert from "node:assert/strict";
import { execFileSync, spawnSync } from "node:child_process";
import { createHash, randomBytes } from "node:crypto";
import {
  existsSync,
  lstatSync,
  mkdirSync,
  mkdtempSync,
  readdirSync,
  readFileSync,
  readlinkSync,
  realpathSync,
  rmSync,
  statfsSync,
  writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { delimiter, dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { binaryPath } from "../index.mjs";

export const MiB = 1024 * 1024;
export const windows = process.platform === "win32";

export const binary = binaryPath();
assert.ok(binary, "run npm run build first");

/** The macOS build of the per-file walker that Linux uses, made by `npm run build`. */
export const walkBinary = fileURLToPath(
  new URL("../build/cow-clone-walk", import.meta.url),
);

/**
 * The binaries to run the tree tests with, each with a directory where it must clone. macOS: the release binary and
 * the walker build, both on APFS under the temporary directory. Linux: btrfs and XFS mounts named by
 * COW_CLONE_REFLINK_DIRS. Windows: the ReFS volume named by COW_CLONE_REFS_DIR. CI must provide them.
 */
export function cloneTargets() {
  if (process.platform === "darwin") {
    assert.ok(existsSync(walkBinary), "run npm run build first");
    return [
      { label: "clonefile", binary, parent: tmpdir() },
      { label: "walker", binary: walkBinary, parent: tmpdir() },
    ];
  }
  const variable =
    process.platform === "linux"
      ? "COW_CLONE_REFLINK_DIRS"
      : "COW_CLONE_REFS_DIR";
  const dirs = (process.env[variable] ?? "").split(delimiter).filter(Boolean);
  if (process.env.CI)
    assert.ok(
      dirs.length > 0,
      `CI must set ${variable} to a clone-capable directory`,
    );
  return dirs.map((parent) => ({ label: parent, binary, parent }));
}

/** A fresh directory under `parent` (the system temporary directory by default), removed after the test. */
export function scratch(t, parent = tmpdir()) {
  const directory = realpathSync(mkdtempSync(join(parent, "cow-clone-")));
  t.after(() => removeAll(directory));
  return directory;
}

/** Removes a tree that the tests may have made read-only. */
export function removeAll(directory) {
  if (!windows) spawnSync("chmod", ["-R", "u+rwx", directory]);
  else spawnSync("attrib", ["-R", join(directory, "*"), "/S", "/D"]);
  rmSync(directory, { recursive: true, force: true });
}

/** Writes `size` random bytes, so no file system can compress or deduplicate them. */
export function write(file, size = 1024) {
  mkdirSync(dirname(file), { recursive: true });
  writeFileSync(file, randomBytes(size));
}

export function cli(args, options = {}, executable = binary) {
  return spawnSync(executable, args, {
    encoding: "utf8",
    timeout: 120000,
    ...options,
  });
}

/** Runs the binary with --json and returns its results. */
export function cloneJson(pairs, executable = binary) {
  const run = cli(["--json", "--", ...pairs.flat()], {}, executable);
  assert.ok(run.status === 0 || run.status === 1, run.stderr);
  const { results } = JSON.parse(run.stdout);
  assert.equal(results.length, pairs.length);
  return results;
}

/** Every entry below `root` with its type, mode, times, size, content hash and link target. */
export function snapshot(root) {
  const entries = {};
  const visit = (relative) => {
    const path = relative ? join(root, relative) : root;
    const stat = lstatSync(path, { bigint: true });
    const entry = {
      type: stat.isSymbolicLink()
        ? "link"
        : stat.isDirectory()
          ? "directory"
          : "file",
      mode: windows ? undefined : Number(stat.mode & 0o7777n),
      mtime: stat.mtimeNs,
    };
    if (entry.type === "link") entry.target = readlinkSync(path);
    else if (entry.type === "file") {
      entry.size = stat.size;
      entry.hash = createHash("sha256")
        .update(readFileSync(path))
        .digest("hex");
    }
    entries[relative || "."] = entry;
    if (entry.type === "directory")
      for (const name of readdirSync(path).sort())
        visit(relative ? join(relative, name) : name);
  };
  visit("");
  return entries;
}

/** Free bytes on the volume that holds `path`. */
export function freeBytes(path) {
  const { bavail, bsize } = statfsSync(path);
  return bavail * bsize;
}

function attached(mount) {
  return execFileSync("hdiutil", ["info"], { encoding: "utf8" })
    .split("\n")
    .some((line) => line.endsWith(`\t${mount}`));
}

function detach(mount, device) {
  for (let attempt = 0; attempt < 5 && attached(mount); attempt++) {
    if (attempt > 0)
      Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 1000);
    spawnSync("hdiutil", ["detach", "-force", device ?? mount], {
      stdio: "ignore",
    });
  }
  if (attached(mount))
    throw new Error(`could not detach the image at ${mount}`);
}

/**
 * Attaches a fresh HFS+ image, a file system without clones, for macOS tests, and returns its mount point, or null
 * when it cannot. One hook detaches it after the test, retrying, and only then removes its directory.
 */
export function hfsVolume(t) {
  const root = realpathSync(mkdtempSync(join(tmpdir(), "cow-clone-")));
  const image = join(root, "hfs.dmg");
  const mount = join(root, "mount");
  let device;
  let mounted = false;
  t.after(() => {
    if (mounted) detach(mount, device);
    rmSync(root, { recursive: true, force: true });
  });
  try {
    execFileSync("hdiutil", [
      "create",
      "-quiet",
      "-size",
      "8m",
      "-fs",
      "HFS+",
      "-volname",
      "cow-clone",
      image,
    ]);
    mkdirSync(mount);
    mounted = true;
    const output = execFileSync(
      "hdiutil",
      ["attach", "-nobrowse", "-mountpoint", mount, image],
      { encoding: "utf8" },
    );
    device = output.match(/^\/dev\/disk\d+/m)?.[0];
    return mount;
  } catch {
    return null;
  }
}
