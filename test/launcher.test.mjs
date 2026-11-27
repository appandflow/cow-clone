import assert from "node:assert/strict";
import { execFileSync, spawnSync } from "node:child_process";
import {
  chmodSync,
  copyFileSync,
  mkdirSync,
  symlinkSync,
  writeFileSync,
} from "node:fs";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { test } from "node:test";
import { scratch, write } from "./helpers.mjs";

const launcher = fileURLToPath(new URL("../bin/cow-clone", import.meta.url));
const posix = process.platform !== "win32";

/** A PATH with only the tools the launcher needs, and a `node` that fails, so any use of Node shows. */
function toolsOnly(root) {
  const tools = join(root, "tools");
  mkdirSync(tools);
  for (const tool of ["sh", "uname", "readlink", "dirname"]) {
    const found = execFileSync("/bin/sh", ["-c", `command -v ${tool}`], {
      encoding: "utf8",
    }).trim();
    symlinkSync(found, join(tools, tool));
  }
  writeFileSync(
    join(tools, "node"),
    "#!/bin/sh\necho node was started >&2\nexit 99\n",
    { mode: 0o755 },
  );
  return tools;
}

test(
  "the launcher runs the binary through symbolic links without Node",
  { skip: !posix },
  (t) => {
    const root = scratch(t);
    write(join(root, "tree", "file"), 10);
    const bin = join(root, "node modules", ".bin");
    mkdirSync(bin, { recursive: true });
    symlinkSync(launcher, join(bin, "cow-clone"));
    mkdirSync(join(root, "global bin"));
    symlinkSync(join(bin, "cow-clone"), join(root, "global bin", "cow-clone"));
    const tools = toolsOnly(root);
    for (const [index, command] of [
      join(bin, "cow-clone"),
      join(root, "global bin", "cow-clone"),
    ].entries()) {
      const run = spawnSync(
        command,
        ["--json", join(root, "tree"), join(root, `clone-${index}`)],
        {
          encoding: "utf8",
          env: { PATH: tools },
        },
      );
      assert.ok(
        run.status === 0 || run.status === 1,
        `${command}: ${run.stderr}`,
      );
      assert.equal(JSON.parse(run.stdout).results.length, 1);
    }
    const failed = spawnSync(
      join(bin, "cow-clone"),
      [join(root, "missing"), join(root, "x")],
      {
        encoding: "utf8",
        env: { PATH: tools },
      },
    );
    assert.equal(failed.status, 1);
  },
);

test("the launcher is valid sh and valid JavaScript", { skip: !posix }, () => {
  execFileSync("sh", ["-n", launcher]);
  execFileSync(process.execPath, ["--check", launcher]);
});

test("Node runs the launcher the way npm's Windows shims do", (t) => {
  const root = scratch(t);
  write(join(root, "tree", "file"), 10);
  const run = spawnSync(
    process.execPath,
    [launcher, "--json", join(root, "tree"), join(root, "clone")],
    { encoding: "utf8" },
  );
  assert.ok(run.status === 0 || run.status === 1, run.stderr);
  assert.equal(JSON.parse(run.stdout).results.length, 1);
  const failed = spawnSync(
    process.execPath,
    [launcher, join(root, "missing"), join(root, "x")],
    { encoding: "utf8" },
  );
  assert.equal(failed.status, 1);
});

test("without a prebuilt binary the launcher prints one line and exits 2", (t) => {
  const root = scratch(t);
  mkdirSync(join(root, "bin"));
  copyFileSync(launcher, join(root, "bin", "cow-clone"));
  const runs = [
    spawnSync(process.execPath, [join(root, "bin", "cow-clone"), "a", "b"], {
      encoding: "utf8",
    }),
  ];
  if (posix) {
    chmodSync(join(root, "bin", "cow-clone"), 0o755);
    runs.push(
      spawnSync(join(root, "bin", "cow-clone"), ["a", "b"], {
        encoding: "utf8",
      }),
    );
  }
  for (const run of runs) {
    assert.equal(run.status, 2, run.stderr);
    assert.equal(run.stdout, "");
    assert.match(run.stderr, /^cow-clone: no prebuilt binary for .+\n$/);
  }
});
