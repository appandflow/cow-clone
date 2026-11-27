import assert from "node:assert/strict";
import { existsSync, readFileSync } from "node:fs";
import { join } from "node:path";
import { test } from "node:test";
import { cli, cloneTargets, scratch, write } from "./helpers.mjs";

const [cloneable] = cloneTargets();

const { version } = JSON.parse(
  readFileSync(new URL("../package.json", import.meta.url), "utf8"),
);

test("--version prints the package version and the platform", () => {
  const run = cli(["--version"]);
  assert.equal(run.status, 0);
  assert.equal(
    run.stdout,
    `${version} (${process.platform}-${process.arch})\n`,
  );
});

test("usage errors exit 2 without cloning", (t) => {
  const root = scratch(t);
  write(join(root, "file"));
  for (const args of [
    [],
    [join(root, "file")],
    ["--json", join(root, "file"), join(root, "a"), join(root, "b")],
    ["--bogus", join(root, "file"), join(root, "a")],
    ["--stdin", join(root, "file"), join(root, "a")],
  ]) {
    const run = cli(args);
    assert.equal(run.status, 2, `${args.join(" ")}: ${run.stderr}`);
    assert.equal(run.stdout, "");
    assert.match(run.stderr, /usage: cow-clone/);
  }
  assert.equal(existsSync(join(root, "a")), false);
});

test(
  "plain output is silent on success and names each failure on stderr",
  { skip: !cloneable && "no clone-capable directory" },
  (t) => {
    const root = scratch(t, cloneable?.parent);
    write(join(root, "file"));
    const ok = cli([join(root, "file"), join(root, "a")]);
    assert.equal(ok.status, 0, ok.stderr);
    assert.equal(ok.stdout + ok.stderr, "");
    const failed = cli([
      join(root, "file"),
      join(root, "a"),
      join(root, "file"),
      join(root, "b"),
    ]);
    assert.equal(failed.status, 1);
    assert.match(failed.stderr, /^cow-clone: .*a: .+ \(EEXIST\)\n$/);
    assert.equal(existsSync(join(root, "b")), true);
  },
);

test(
  "--stdin reads NUL-separated pairs, and -- ends the options",
  { skip: !cloneable && "no clone-capable directory" },
  (t) => {
    const root = scratch(t, cloneable?.parent);
    write(join(root, "-file"));
    const run = cli(["--json", "--stdin"], {
      input: `${join(root, "-file")}\0${join(root, "a")}\0${join(root, "-file")}\0${join(root, "b")}`,
    });
    assert.equal(run.status, 0, run.stderr);
    assert.deepEqual(JSON.parse(run.stdout), {
      results: [{ cloned: true }, { cloned: true }],
    });
    const dashed = cli(["--", "-file", "-copy"], { cwd: root });
    assert.equal(dashed.status, 0, dashed.stderr);
    assert.equal(existsSync(join(root, "-copy")), true);
  },
);

test("JSON escapes quotes, backslashes and control characters in paths", (t) => {
  const root = scratch(t);
  const name =
    process.platform === "win32" ? "quote'and space" : 'quote"back\\slash\ttab';
  const run = cli(["--json", join(root, name), join(root, "x")]);
  assert.equal(run.status, 1);
  const [result] = JSON.parse(run.stdout).results;
  assert.equal(result.path, join(root, name));
  assert.equal(result.code, "ENOENT");
});
