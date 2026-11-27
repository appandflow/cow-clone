export interface Cloned {
  source: string;
  destination: string;
  cloned: true;
}

export interface NotCloned {
  source: string;
  destination: string;
  cloned: false;
  /**
   * `unsupported`: the file system cannot clone (ext4, tmpfs, NTFS, HFS+, a FAT drive, most network shares).
   * `cross-device`: the source and the destination are on different volumes. `failed`: any other error, for example
   * a missing source, an existing destination (`code` EEXIST), a permission error or a full disk. In every case the
   * destination is left as it was before the call: a clone that fails part-way removes what it created.
   */
  reason: "unsupported" | "cross-device" | "failed";
  /** An errno name such as `ENOTSUP`, `EXDEV`, `EEXIST` or `EACCES`. Windows errors are mapped to the same names. */
  code: string;
  message: string;
  /** The path the failing call was about: the destination, or an entry below the source or the destination. */
  path: string;
}

export type CloneResult = Cloned | NotCloned;

export interface CloneOptions {
  /** Aborting kills the child process and rejects with the signal's reason. A partial destination can remain. */
  signal?: AbortSignal;
  /**
   * Kills the child process and rejects with a `timeout` error after this many milliseconds. A partial destination
   * can remain.
   */
  timeoutMs?: number;
}

export type CowCloneErrorCode = "no-binary" | "timeout" | "failed";

export declare class CowCloneError extends Error {
  constructor(
    code: CowCloneErrorCode,
    message: string,
    options?: { cause?: unknown; exitCode?: number | null },
  );
  readonly name: "CowCloneError";
  /**
   * `no-binary`: the package has no prebuilt binary for this platform. `timeout`: `timeoutMs` ran out. `failed`: the
   * binary could not run or printed no result.
   */
  readonly code: CowCloneErrorCode;
  /** The binary's exit status, null when it did not exit on its own. */
  readonly exitCode: number | null;
}

/**
 * Clones `source`, a file, a directory tree or a symbolic link, to `destination`, which must not exist; its parent
 * must. Resolves with whether it cloned and, when not, why, so the caller can copy instead.
 */
export declare function clone(
  source: string,
  destination: string,
  options?: CloneOptions,
): Promise<CloneResult>;

/**
 * Clones each `[source, destination]` pair in order, in one child process. Resolves with one result per pair, in the
 * same order; a pair that cannot be cloned does not stop the others. Resolves with `[]` for no pairs.
 */
export declare function cloneMany(
  pairs: Iterable<readonly [string, string]>,
  options?: CloneOptions,
): Promise<CloneResult[]>;

/** The absolute path of the prebuilt binary for this platform, or null when the package has none. */
export declare function binaryPath(): string | null;
