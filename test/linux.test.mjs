import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { existsSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { test } from "node:test";
import { cloneJson, cloneTargets, scratch, write } from "./helpers.mjs";

const linux = process.platform === "linux";
const fileSystem = (path) =>
  execFileSync("stat", ["-f", "-c", "%T", path], { encoding: "utf8" }).trim();

test(
  "Linux: ext4 and tmpfs cannot clone, and the partial tree is removed",
  { skip: !linux },
  (t) => {
    const candidates = [tmpdir(), "/dev/shm"].filter(
      (dir) =>
        existsSync(dir) && ["ext2/ext3", "tmpfs"].includes(fileSystem(dir)),
    );
    if (process.env.CI)
      assert.ok(candidates.length > 0, "no ext4 or tmpfs directory");
    for (const dir of candidates) {
      const root = scratch(t, dir);
      write(join(root, "tree", "a", "file"));
      const [result] = cloneJson([[join(root, "tree"), join(root, "clone")]]);
      assert.equal(result.cloned, false, dir);
      assert.equal(
        result.reason,
        "unsupported",
        `${dir}: ${JSON.stringify(result)}`,
      );
      assert.equal(existsSync(join(root, "clone")), false);
    }
  },
);

test("Linux: two reflink mounts are cross-device", { skip: !linux }, (t) => {
  const mounts = cloneTargets().map(({ parent }) => parent);
  if (mounts.length < 2) return t.skip("needs two reflink mounts");
  const from = scratch(t, mounts[0]);
  const to = scratch(t, mounts[1]);
  write(join(from, "tree", "file"));
  const [result] = cloneJson([[join(from, "tree"), join(to, "clone")]]);
  assert.equal(result.reason, "cross-device", JSON.stringify(result));
  assert.equal(result.code, "EXDEV");
  assert.equal(existsSync(join(to, "clone")), false);
});
