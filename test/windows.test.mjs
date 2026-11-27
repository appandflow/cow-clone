import assert from "node:assert/strict";
import { existsSync, readlinkSync, symlinkSync, statSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { test } from "node:test";
import {
  cloneJson,
  cloneTargets,
  scratch,
  snapshot,
  write,
} from "./helpers.mjs";

const windows = process.platform === "win32";

test("Windows: NTFS cannot clone", { skip: !windows }, (t) => {
  const root = scratch(t, tmpdir());
  write(join(root, "tree", "file"));
  const [result] = cloneJson([[join(root, "tree"), join(root, "clone")]]);
  assert.equal(result.reason, "unsupported", JSON.stringify(result));
  assert.equal(existsSync(join(root, "clone")), false);
});

for (const { parent } of windows ? cloneTargets() : []) {
  test("Windows ReFS: another volume is cross-device", (t) => {
    const from = scratch(t, parent);
    const to = scratch(t, tmpdir());
    write(join(from, "tree", "file"));
    const [result] = cloneJson([[join(from, "tree"), join(to, "clone")]]);
    assert.equal(result.reason, "cross-device", JSON.stringify(result));
    assert.equal(existsSync(join(to, "clone")), false);
  });

  test("Windows ReFS: junctions are cloned as junctions", (t) => {
    const root = scratch(t, parent);
    write(join(root, "tree", "target", "file"));
    symlinkSync(
      join(root, "tree", "target"),
      join(root, "tree", "junction"),
      "junction",
    );
    const [result] = cloneJson([[join(root, "tree"), join(root, "clone")]]);
    assert.equal(result.cloned, true, JSON.stringify(result));
    assert.equal(
      readlinkSync(join(root, "clone", "junction")),
      readlinkSync(join(root, "tree", "junction")),
    );
  });

  test("Windows ReFS: paths longer than MAX_PATH clone", (t) => {
    const root = scratch(t, parent);
    let deep = join(root, "tree");
    while (deep.length < 400)
      deep = join(deep, "a-long-directory-name-to-pass-max-path");
    write(join(deep, "file"), 100000);
    const [result] = cloneJson([[join(root, "tree"), join(root, "clone")]]);
    assert.equal(result.cloned, true, JSON.stringify(result));
    assert.deepEqual(
      snapshot(join(root, "clone")),
      snapshot(join(root, "tree")),
    );
  });

  test("Windows ReFS: files of odd sizes clone byte for byte", (t) => {
    const root = scratch(t, parent);
    for (const size of [1, 4095, 4096, 4097, 65535, 65537, 1048577])
      write(join(root, "tree", `size-${size}`), size);
    const [result] = cloneJson([[join(root, "tree"), join(root, "clone")]]);
    assert.equal(result.cloned, true, JSON.stringify(result));
    assert.deepEqual(
      snapshot(join(root, "clone")),
      snapshot(join(root, "tree")),
    );
    assert.equal(statSync(join(root, "clone", "size-4097")).size, 4097);
  });
}
