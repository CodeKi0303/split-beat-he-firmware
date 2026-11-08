import { resolve } from "node:path";
import { pathToFileURL } from "node:url";
const studioModule = (filename) => import(pathToFileURL(resolve(process.env.SPLIT_BEAT_STUDIO, "web", filename)));
import fs from "node:fs";
import assert from "node:assert/strict";
const {
  unpackPacket,
  packet,
  decodeInfo,
  decodeStatus,
  decodeKey,
  decodeBinding,
  encodeKey,
  encodeBinding,
} = await studioModule("protocol.mjs");
const vectors = JSON.parse(
  fs.readFileSync("build/host/bridge-vectors.json", "utf8"),
);
let kinds = new Set();
for (const v of vectors) {
  const request = unpackPacket(Buffer.from(v.request, "hex")),
    response = unpackPacket(Buffer.from(v.response, "hex"));
  assert.equal(response.seq, request.seq);
  assert.equal(response.op, request.op | 128);
  assert.equal(
    Buffer.from(
      packet(response.op, response.seq, response.payload, response.status),
    ).toString("hex"),
    v.response,
  );
  assert.equal(response.status, 0);
  kinds.add(request.op);
  switch (request.op) {
    case 1:
      assert.equal(decodeInfo(response.payload).board_rev, 77);
      break;
    case 2:
      assert.equal(decodeStatus(response.payload).raw.length, 7);
      break;
    case 3:
    case 4:
      assert.deepEqual(
        encodeKey(decodeKey(response.payload)),
        response.payload,
      );
      break;
    case 9:
    case 10:
      assert.deepEqual(
        encodeBinding(decodeBinding(response.payload)),
        response.payload,
      );
      break;
  }
}
for (const op of [1, 2, 3, 4, 5, 7, 9, 10]) assert.ok(kinds.has(op));
console.log(
  `C core -> Python -> JS golden codec exchanges PASS (${vectors.length})`,
);
