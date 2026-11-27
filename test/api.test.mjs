import assert from "node:assert/strict";
import { existsSync } from "node:fs";
import { join } from "node:path";
import { test } from "node:test";
import { clone, cloneMany, CowCloneError } from "../index.mjs";
import { scratch, write } from "./helpers.mjs";

test("clone resolves with a result that names the pair", async (t) => {
  const root = scratch(t);
  write(join(root, "file"));
  const { message, ...result } = await clone(
    join(root, "file"),
    join(root, "missing", "x"),
  );
  assert.deepEqual(result, {
    source: join(root, "file"),
    destination: join(root, "missing", "x"),
    cloned: false,
    reason: "failed",
    code: "ENOENT",
    path: join(root, "missing", "x"),
  });
  assert.ok(message.length > 0);
});

test("cloneMany keeps the order of its pairs and passes them on stdin", async (t) => {
  const root = scratch(t);
  write(join(root, "file"));
  // Far more than a Windows command line holds (32,767 characters).
  const pairs = Array.from({ length: 2000 }, (_, i) => [
    join(root, "file"),
    join(root, `a-long-destination-name-to-fill-the-command-line-${i}`),
  ]);
  pairs.splice(1000, 0, [join(root, "nope"), join(root, "never")]);
  const results = await cloneMany(pairs);
  assert.equal(results.length, 2001);
  assert.equal(results[1000].cloned, false);
  assert.equal(results[1000].source, join(root, "nope"));
  if (process.platform === "darwin") {
    assert.ok(results.filter((result) => result.cloned).length === 2000);
    assert.equal(existsSync(pairs[2000][1]), true);
  }
});

test("cloneMany resolves [] for no pairs and rejects bad pairs", async () => {
  assert.deepEqual(await cloneMany([]), []);
  for (const pairs of [[["a"]], [["a", ""]], [["a", "b\0c"]], [[1, "b"]]])
    await assert.rejects(cloneMany(/** @type {any} */ (pairs)), TypeError);
});

test("an aborted signal rejects with its reason", async (t) => {
  const root = scratch(t);
  write(join(root, "file"));
  const controller = new AbortController();
  controller.abort(new Error("stop"));
  await assert.rejects(
    clone(join(root, "file"), join(root, "x"), { signal: controller.signal }),
    /stop/,
  );
  assert.equal(existsSync(join(root, "x")), false);
});

test("CowCloneError carries a code and an exit status", () => {
  const error = new CowCloneError("timeout", "late");
  assert.equal(error.code, "timeout");
  assert.equal(error.exitCode, null);
  assert.equal(error.name, "CowCloneError");
});
