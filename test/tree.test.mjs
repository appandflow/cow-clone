import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import {
  chmodSync,
  closeSync,
  fsyncSync,
  openSync,
  existsSync,
  lutimesSync,
  mkdirSync,
  readFileSync,
  symlinkSync,
  utimesSync,
  writeFileSync,
} from "node:fs";
import { join } from "node:path";
import { test } from "node:test";
import {
  cloneJson,
  cloneTargets,
  freeBytes,
  MiB,
  scratch,
  snapshot,
  windows,
  write,
} from "./helpers.mjs";

/** A tree with what a checkout or node_modules holds: nesting, empty entries, modes, links, old times and odd names. */
function fill(source) {
  write(join(source, "package.json"), 300);
  write(join(source, "lib", "deep", "deeper", "index.js"), 5000);
  write(join(source, "bin", "tool"), 100);
  writeFileSync(join(source, "empty"), "");
  mkdirSync(join(source, "empty-dir"));
  write(join(source, "names", "space in name"), 10);
  write(join(source, "names", "accentué ü 日本"), 10);
  write(join(source, "big"), 3 * MiB + 123);
  symlinkSync(
    "lib/deep",
    join(source, "relative-link"),
    windows ? "dir" : undefined,
  );
  symlinkSync(
    join(source, "package.json"),
    join(source, "absolute-link"),
    windows ? "file" : undefined,
  );
  symlinkSync(
    "missing",
    join(source, "dangling-link"),
    windows ? "file" : undefined,
  );
  if (!windows) {
    chmodSync(join(source, "bin", "tool"), 0o755);
    chmodSync(join(source, "package.json"), 0o600);
    chmodSync(join(source, "lib", "deep", "deeper", "index.js"), 0o444);
  }
  const old = new Date("2020-02-03T04:05:06.789Z");
  for (const path of [
    "big",
    "empty",
    "lib/deep/deeper/index.js",
    "lib/deep/deeper",
    "lib/deep",
    "lib",
    "empty-dir",
  ])
    utimesSync(join(source, path), old, old);
  lutimesSync(join(source, "relative-link"), old, old);
  if (!windows) chmodSync(join(source, "lib", "deep"), 0o555);
  utimesSync(source, old, old);
}

for (const { label, binary, parent } of cloneTargets()) {
  test(`${label}: a directory tree clones with its modes, times and links`, (t) => {
    const root = scratch(t, parent);
    const source = join(root, "source");
    mkdirSync(source);
    fill(source);
    const before = snapshot(source);
    const [result] = cloneJson([[source, join(root, "clone")]], binary);
    assert.deepEqual(result, { cloned: true });
    assert.deepEqual(snapshot(source), before);
    const cloned = snapshot(join(root, "clone"));
    if (label === "clonefile")
      // clonefile(2) gives each cloned directory the time of the clone; files and links keep theirs.
      for (const entries of [cloned, before])
        for (const entry of Object.values(entries))
          if (entry.type === "directory") delete entry.mtime;
    assert.deepEqual(cloned, before);
  });

  test(`${label}: a file, a symbolic link and many pairs in one call`, (t) => {
    const root = scratch(t, parent);
    write(join(root, "file"), 2000);
    symlinkSync("file", join(root, "link"), windows ? "file" : undefined);
    const pairs = [];
    for (let i = 0; i < 50; i++)
      pairs.push([join(root, "file"), join(root, `copy-${i}`)]);
    pairs.push([join(root, "link"), join(root, "link-copy")]);
    const results = cloneJson(pairs, binary);
    assert.ok(
      results.every((result) => result.cloned),
      JSON.stringify(results),
    );
    assert.deepEqual(
      readFileSync(join(root, "copy-49")),
      readFileSync(join(root, "file")),
    );
    assert.deepEqual(
      snapshot(join(root, "link-copy"))["."],
      snapshot(join(root, "link"))["."],
    );
  });

  test(`${label}: clones share blocks with their source`, (t) => {
    const root = scratch(t, parent);
    write(join(root, "source", "big"), 64 * MiB);
    // btrfs and XFS allocate on writeback, so flush the source before the baseline.
    const fd = openSync(join(root, "source", "big"), "r+");
    fsyncSync(fd);
    closeSync(fd);
    const free = freeBytes(root);
    const results = cloneJson(
      [
        [join(root, "source"), join(root, "a")],
        [join(root, "source"), join(root, "b")],
      ],
      binary,
    );
    assert.ok(
      results.every((result) => result.cloned),
      JSON.stringify(results),
    );
    const used = free - freeBytes(root);
    assert.ok(used < 32 * MiB, `two 64 MiB clones used ${used} bytes`);
  });

  test(`${label}: an existing destination is left alone and later pairs still run`, (t) => {
    const root = scratch(t, parent);
    write(join(root, "source", "file"));
    write(join(root, "taken", "mine"), 10);
    const before = snapshot(join(root, "taken"));
    const results = cloneJson(
      [
        [join(root, "source"), join(root, "taken")],
        [join(root, "missing"), join(root, "nothing")],
        [join(root, "source"), join(root, "fresh")],
      ],
      binary,
    );
    assert.equal(results[0].cloned, false);
    assert.equal(results[0].reason, "failed");
    assert.equal(results[0].code, "EEXIST");
    assert.equal(results[1].code, "ENOENT");
    assert.equal(results[1].path, join(root, "missing"));
    assert.equal(results[2].cloned, true);
    assert.deepEqual(snapshot(join(root, "taken")), before);
    assert.equal(existsSync(join(root, "nothing")), false);
  });

  test(
    `${label}: a failure part-way removes the partial destination`,
    { skip: windows && "a file the owner cannot read needs an ACL on Windows" },
    (t) => {
      const root = scratch(t, parent);
      const source = join(root, "source");
      write(join(source, "a", "first"));
      write(join(source, "z", "unreadable"));
      write(join(source, "z", "zz"));
      chmodSync(join(source, "z", "unreadable"), 0);
      if (process.getuid?.() === 0) return t.skip("root reads every file");
      const [result] = cloneJson([[source, join(root, "clone")]], binary);
      if (binary.endsWith("cow-clone-walk") || process.platform === "linux") {
        assert.equal(result.cloned, false);
        assert.equal(result.code, "EACCES");
        assert.equal(result.path, join(source, "z", "unreadable"));
      }
      // clonefile(2) clones a file its owner cannot read, since cloning reads no data.
      if (!result.cloned) assert.equal(existsSync(join(root, "clone")), false);
    },
  );

  test(
    `${label}: a FIFO stops the pair`,
    {
      skip:
        windows ||
        (process.platform === "darwin" && !binary.endsWith("cow-clone-walk")),
    },
    (t) => {
      const root = scratch(t, parent);
      write(join(root, "source", "file"));
      const fifo = spawnSync("mkfifo", [join(root, "source", "pipe")]);
      assert.equal(fifo.status, 0);
      const [result] = cloneJson(
        [[join(root, "source"), join(root, "clone")]],
        binary,
      );
      assert.equal(result.cloned, false);
      assert.equal(result.code, "ESPECIAL");
      assert.equal(existsSync(join(root, "clone")), false);
    },
  );
}
