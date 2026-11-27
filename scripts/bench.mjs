import { spawnSync } from "node:child_process";
import {
  constants,
  cpSync,
  existsSync,
  mkdirSync,
  mkdtempSync,
  readdirSync,
  rmSync,
  statfsSync,
  writeFileSync,
} from "node:fs";
import { cpus } from "node:os";
import { join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { parseArgs } from "node:util";
import { binaryPath } from "../index.mjs";

const { values } = parseArgs({
  options: {
    source: { type: "string" },
    generate: { type: "string" },
    files: { type: "string", default: "40000" },
    scratch: { type: "string" },
    runs: { type: "string", default: "5" },
    "skip-copy": { type: "boolean", default: false },
  },
});
const runs = Number(values.runs);
const binary = binaryPath();
if (!binary) throw new Error("run npm run build first");
const walker = fileURLToPath(
  new URL("../build/cow-clone-walk", import.meta.url),
);

/** A tree shaped like an npm `node_modules`: packages of 8 to 47 small files in a few folders, a quarter of them with
 * nested dependencies. Seeded, so every runner builds the same tree. */
function nodeModules(root, target) {
  let seed = 1;
  const random = (n) => {
    seed = (Math.imul(seed, 1103515245) + 12345) & 0x7fffffff;
    return (seed >>> 16) % n;
  };
  const content = Buffer.alloc(32 * 1024, "x");
  let files = 0;
  const pkg = (dir, depth) => {
    const folders = ["", "lib", join("lib", "internal"), "dist"];
    for (const folder of folders)
      mkdirSync(join(dir, folder), { recursive: true });
    writeFileSync(join(dir, "package.json"), `{"name":"${files}"}\n`);
    const count = 8 + random(40);
    for (let f = 0; f < count; f++) {
      const size = 1 + (random(8) === 0 ? random(32 * 1024) : random(2048));
      writeFileSync(
        join(dir, folders[random(4)], `file-${f}.js`),
        content.subarray(0, size),
      );
    }
    files += count + 1;
    if (depth < 5 && random(4) === 0)
      for (let n = 0, nested = 1 + random(5); n < nested && files < target; n++)
        pkg(join(dir, "node_modules", `dep-${n}`), depth + 1);
  };
  for (let i = 0; files < target; i++) pkg(join(root, `package-${i}`), 0);
  return files;
}

function countFiles(dir) {
  let files = 0;
  for (const entry of readdirSync(dir, {
    withFileTypes: true,
    recursive: true,
  }))
    if (entry.isFile()) files++;
  return files;
}

let source;
if (values.generate) {
  source = join(resolve(values.generate), "node_modules");
  if (!existsSync(source)) nodeModules(source, Number(values.files));
} else if (values.source) source = resolve(values.source);
else throw new Error("pass --source <dir> or --generate <dir>");
const scratchParent = resolve(
  values.scratch ?? (values.generate ? values.generate : join(source, "..")),
);
const scratch = mkdtempSync(join(scratchParent, "cow-clone-bench-"));
const entries = readdirSync(source);
const pairsFor = (destination) =>
  entries.map((name) => [join(source, name), join(destination, name)]);

function run(command, args, input) {
  const result = spawnSync(command, args, {
    input,
    encoding: "utf8",
    maxBuffer: 1 << 28,
  });
  if (result.status !== 0)
    throw new Error(
      `${command} exited with ${result.status}: ${result.stderr.slice(0, 500)}`,
    );
}

function cloneAll(executable, pairs) {
  run(
    executable,
    ["--stdin"],
    pairs
      .flat()
      .map((path) => `${path}\0`)
      .join(""),
  );
}

const variants = [
  {
    name: "every top-level entry, one process",
    run: (destination) => {
      mkdirSync(destination);
      cloneAll(binary, pairsFor(destination));
    },
  },
  {
    name: "every top-level entry, one process per entry",
    run: (destination) => {
      mkdirSync(destination);
      for (const [from, to] of pairsFor(destination)) run(binary, [from, to]);
    },
  },
  {
    name: "the whole tree as one pair",
    run: (destination) => run(binary, [source, destination]),
  },
];
if (process.platform !== "darwin")
  variants.push({
    name: "the whole tree as one pair, one thread",
    run: (destination) => {
      const result = spawnSync(binary, [source, destination], {
        env: { ...process.env, COW_CLONE_THREADS: "1" },
      });
      if (result.status !== 0)
        throw new Error(`cow-clone exited with ${result.status}`);
    },
  });
if (process.platform === "darwin" && existsSync(walker))
  variants.push({
    name: "walker build (file by file, as on Linux), one process",
    run: (destination) => {
      mkdirSync(destination);
      cloneAll(walker, pairsFor(destination));
    },
  });
if (process.platform === "darwin")
  variants.push({
    name: "cp -Rc, whole tree",
    run: (destination) => run("cp", ["-Rc", source, destination]),
  });
if (process.platform === "linux")
  variants.push({
    name: "cp -a --reflink=always, whole tree",
    run: (destination) =>
      run("cp", ["-a", "--reflink=always", source, destination]),
  });
if (!values["skip-copy"])
  variants.push({
    name: "fs.cpSync with COPYFILE_FICLONE",
    run: (destination) =>
      cpSync(source, destination, {
        recursive: true,
        verbatimSymlinks: true,
        mode: constants.COPYFILE_FICLONE,
      }),
  });

/** Free bytes on the scratch volume, after a sync so btrfs and XFS have accounted for what was written. ReFS frees
 * deleted clusters later, so its numbers are not shown. */
function freeBytes() {
  if (process.platform !== "win32") spawnSync("sync");
  const { bavail, bsize } = statfsSync(scratch);
  return bavail * bsize;
}

const median = (list) =>
  [...list].sort((a, b) => a - b)[Math.floor(list.length / 2)];
console.log(
  `${process.platform}-${process.arch}, ${cpus()[0]?.model ?? "unknown CPU"}, Node ${process.version}\n` +
    `source: ${source} (${entries.length} top-level entries, ${countFiles(source)} files), median of ${runs} runs\n`,
);
console.log(
  "| variant | median ms | min ms | max ms | MiB used |\n| --- | ---: | ---: | ---: | ---: |",
);
try {
  for (const variant of variants) {
    const times = [];
    let used = 0;
    for (let i = 0; i < runs; i++) {
      const destination = join(scratch, `run-${i}`);
      const free = freeBytes();
      const start = process.hrtime.bigint();
      try {
        variant.run(destination);
      } catch (error) {
        console.log(
          `| ${variant.name} | ${error.message.split("\n")[0]} | | | |`,
        );
        times.length = 0;
        break;
      }
      times.push(Number(process.hrtime.bigint() - start) / 1e6);
      if (i === 0) used = free - freeBytes();
      rmSync(destination, { recursive: true, force: true });
    }
    if (times.length)
      console.log(
        `| ${variant.name} | ${median(times).toFixed(0)} | ${Math.min(...times).toFixed(0)} | ${Math.max(...times).toFixed(0)} | ${process.platform === "win32" ? "n/a" : (used / 2 ** 20).toFixed(0)} |`,
      );
  }
} finally {
  rmSync(scratch, { recursive: true, force: true });
}
