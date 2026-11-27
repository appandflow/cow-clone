import assert from "node:assert/strict";
import { existsSync } from "node:fs";
import { join } from "node:path";
import { test } from "node:test";
import {
  binary,
  cloneJson,
  hfsVolume,
  scratch,
  walkBinary,
  write,
} from "./helpers.mjs";

const darwin = process.platform === "darwin";

for (const [label, executable] of [
  ["clonefile", binary],
  ["walker", walkBinary],
])
  test(
    `macOS ${label}: HFS+ cannot clone and another volume is cross-device`,
    { skip: !darwin },
    (t) => {
      const volume = hfsVolume(t);
      if (!volume) return t.skip("hdiutil could not attach an image");
      const root = scratch(t);
      write(join(root, "tree", "file"));
      write(join(volume, "tree", "file"));
      const [unsupported, crossDevice] = cloneJson(
        [
          [join(volume, "tree"), join(volume, "clone")],
          [join(root, "tree"), join(volume, "from-apfs")],
        ],
        executable,
      );
      assert.equal(unsupported.cloned, false);
      assert.equal(unsupported.reason, "unsupported");
      assert.equal(unsupported.code, "ENOTSUP");
      assert.equal(crossDevice.reason, "cross-device");
      assert.equal(crossDevice.code, "EXDEV");
      assert.equal(existsSync(join(volume, "clone")), false);
      assert.equal(existsSync(join(volume, "from-apfs")), false);
    },
  );
