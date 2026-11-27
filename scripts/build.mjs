import { spawnSync } from "node:child_process";
import { chmodSync, mkdirSync, readFileSync } from "node:fs";

const { version } = JSON.parse(readFileSync("package.json", "utf8"));
const target = `${process.platform}-${process.arch}`;
const directory = `prebuilds/${target}`;
mkdirSync(directory, { recursive: true });

function run(command, args) {
  console.log([command, ...args].join(" "));
  const result = spawnSync(command, args, { stdio: "inherit" });
  if (result.status !== 0) process.exit(result.status ?? 1);
}

const warnings = ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror"];
const defines = [
  `-DCOW_CLONE_VERSION=${version}`,
  `-DCOW_CLONE_TARGET=${target}`,
];
if (process.platform === "darwin") {
  const sources = ["src/main.c", "src/darwin.c", "src/walk.c", "src/posix.c"];
  run("cc", [
    ...warnings,
    "-mmacosx-version-min=11.0",
    ...defines,
    ...sources,
    "-o",
    `${directory}/cow-clone`,
  ]);
  // The walker Linux uses, built for macOS so the tests can run it on APFS.
  mkdirSync("build", { recursive: true });
  run("cc", [
    ...warnings,
    "-mmacosx-version-min=11.0",
    ...defines,
    "-DCOW_CLONE_WALK",
    ...sources,
    "-o",
    "build/cow-clone-walk",
  ]);
} else if (process.platform === "linux") {
  run("cc", [
    ...warnings,
    "-D_GNU_SOURCE",
    "-static",
    "-s",
    ...defines,
    "src/main.c",
    "src/linux.c",
    "src/walk.c",
    "src/posix.c",
    "-o",
    `${directory}/cow-clone`,
  ]);
} else if (process.platform === "win32") {
  mkdirSync("build", { recursive: true });
  run("cl", [
    "/nologo",
    "/std:c11",
    "/O2",
    "/W4",
    "/WX",
    "/MT",
    "/utf-8",
    `/DCOW_CLONE_VERSION=${version}`,
    `/DCOW_CLONE_TARGET=${target}`,
    "src\\main.c",
    "src\\windows.c",
    "/Fobuild\\",
    `/Fe${directory}\\cow-clone.exe`,
  ]);
} else {
  console.error(`cow-clone does not build on ${process.platform}`);
  process.exit(1);
}
if (process.platform !== "win32") chmodSync(`${directory}/cow-clone`, 0o755);
