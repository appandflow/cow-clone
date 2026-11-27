import { binaryPath, clone, cloneMany, CowCloneError } from "cow-clone";
import type { CloneResult } from "cow-clone";

const results: CloneResult[] = await cloneMany([["a", "b"]], {
  timeoutMs: 1000,
  signal: AbortSignal.timeout(2000),
});
const one = await clone("a", "b");
if (!one.cloned) {
  const reason: "unsupported" | "cross-device" | "failed" = one.reason;
  const code: string = one.code;
  void reason;
  void code;
}
const binary: string | null = binaryPath();
try {
  await clone("a", "b");
} catch (error) {
  if (error instanceof CowCloneError) {
    const code: "no-binary" | "timeout" | "failed" = error.code;
    void code;
  }
}
void results;
void binary;
