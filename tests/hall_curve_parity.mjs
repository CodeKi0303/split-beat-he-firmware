import { resolve } from "node:path";
import { pathToFileURL } from "node:url";
const studioModule = (filename) => import(pathToFileURL(resolve(process.env.SPLIT_BEAT_STUDIO, "web", filename)));
// Exhaustive comparison against actual C-core output, never HID.
import assert from "node:assert/strict";
import fs from "node:fs";
const { rawToTravel } = await studioModule("hall-curve.mjs");
const { rawToEstimatedMm } = await studioModule("telemetry.mjs");
const vectors = JSON.parse(fs.readFileSync(process.argv[2], "utf8"));
let count = 0;
for (const v of vectors) {
  const key = { released: v.released, pressed: v.pressed, curve_id: v.curve_id, flags: 7 };
  for (let raw = 0; raw <= 4095; raw++) {
    assert.equal(rawToTravel(raw, key), v.travel[raw], `curve ${v.curve_id}, raw ${raw}, endpoints ${v.released}/${v.pressed}`);
    if (v.curve_id === 1) {
      const at34 = rawToEstimatedMm(raw, key, 3.4);
      const at40 = rawToEstimatedMm(raw, key, 4.0);
      assert.ok(Math.abs(at40 - at34 * 4 / 3.4) < 1e-12);
    }
    count++;
  }
}
console.log(`C -> JS Hall curve parity PASS: ${count} samples; proportional 3.4/4.0 mm checked`);
