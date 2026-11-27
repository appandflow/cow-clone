import { spawn } from "node:child_process";
import { existsSync } from "node:fs";
import { fileURLToPath } from "node:url";

export class CowCloneError extends Error {
  /**
   * @param {"no-binary" | "timeout" | "failed"} code
   * @param {string} message
   * @param {{ cause?: unknown; exitCode?: number | null }} [options]
   */
  constructor(code, message, options = {}) {
    super(message, { cause: options.cause });
    this.name = "CowCloneError";
    this.code = code;
    this.exitCode = options.exitCode ?? null;
  }
}

/** @returns {string | null} */
export function binaryPath() {
  const name = process.platform === "win32" ? "cow-clone.exe" : "cow-clone";
  const file = fileURLToPath(
    new URL(
      `./prebuilds/${process.platform}-${process.arch}/${name}`,
      import.meta.url,
    ),
  );
  return existsSync(file) ? file : null;
}

/**
 * @param {string} source
 * @param {string} destination
 * @param {import("./index.d.mts").CloneOptions} [options]
 * @returns {Promise<import("./index.d.mts").CloneResult>}
 */
export async function clone(source, destination, options) {
  const [result] = await cloneMany([[source, destination]], options);
  return /** @type {import("./index.d.mts").CloneResult} */ (result);
}

/**
 * @param {Iterable<readonly [string, string]>} pairs
 * @param {import("./index.d.mts").CloneOptions} [options]
 * @returns {Promise<import("./index.d.mts").CloneResult[]>}
 */
export function cloneMany(pairs, options = {}) {
  const { signal, timeoutMs } = options;
  const list = [...pairs];
  for (const pair of list)
    if (
      !Array.isArray(pair) ||
      pair.length !== 2 ||
      pair.some(
        (path) => typeof path !== "string" || !path || path.includes("\0"),
      )
    )
      return Promise.reject(
        new TypeError(
          "cloneMany takes [source, destination] pairs of non-empty paths without NUL characters",
        ),
      );
  return new Promise((resolve, reject) => {
    if (signal?.aborted) return reject(signal.reason);
    if (list.length === 0) return resolve([]);
    const binary = binaryPath();
    if (!binary)
      return reject(
        new CowCloneError(
          "no-binary",
          `cow-clone has no prebuilt binary for ${process.platform}-${process.arch}`,
        ),
      );
    const child = spawn(binary, ["--json", "--stdin"], {
      stdio: ["pipe", "pipe", "pipe"],
      windowsHide: true,
    });
    let stdout = "";
    let stderr = "";
    child.stdout.setEncoding("utf8").on("data", (chunk) => (stdout += chunk));
    child.stderr.setEncoding("utf8").on("data", (chunk) => (stderr += chunk));
    child.stdin.on("error", () => {});
    child.stdin.end(list.map(([from, to]) => `${from}\0${to}\0`).join(""));
    let settled = false;
    /** @type {NodeJS.Timeout | undefined} */
    let timer;
    const onAbort = () => {
      child.kill("SIGKILL");
      finish(() => reject(signal?.reason));
    };
    /** @param {() => void} settle */
    const finish = (settle) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      signal?.removeEventListener("abort", onAbort);
      settle();
    };
    if (timeoutMs !== undefined)
      timer = setTimeout(() => {
        child.kill("SIGKILL");
        finish(() =>
          reject(
            new CowCloneError(
              "timeout",
              `cow-clone did not finish within ${timeoutMs} ms`,
            ),
          ),
        );
      }, timeoutMs);
    signal?.addEventListener("abort", onAbort, { once: true });
    child.on("error", (error) =>
      finish(() =>
        reject(new CowCloneError("failed", error.message, { cause: error })),
      ),
    );
    child.on("close", (exitCode, exitSignal) =>
      finish(() => {
        /** @type {{ results?: Array<Record<string, unknown>> } | null} */
        let parsed = null;
        try {
          parsed = JSON.parse(stdout);
        } catch {}
        const results = parsed?.results;
        if (
          (exitCode === 0 || exitCode === 1) &&
          Array.isArray(results) &&
          results.length === list.length
        )
          resolve(
            results.map((result, index) => {
              const [source, destination] = /** @type {[string, string]} */ (
                list[index]
              );
              return /** @type {import("./index.d.mts").CloneResult} */ ({
                source,
                destination,
                ...result,
              });
            }),
          );
        else
          reject(
            new CowCloneError(
              "failed",
              stderr
                .trim()
                .split(/\r?\n/)
                .at(0)
                ?.replace(/^cow-clone: /, "") ||
                `cow-clone exited with ${exitCode ?? exitSignal}`,
              { exitCode },
            ),
          );
      }),
    );
  });
}
