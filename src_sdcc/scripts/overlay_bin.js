const fs = require('fs');

const [targetPath, overlayPath, offsetText] = process.argv.slice(2);
if (!targetPath || !overlayPath || !offsetText) process.exit(2);

const target = fs.readFileSync(targetPath);
const overlay = fs.readFileSync(overlayPath);
const offset = Number(offsetText);
if (!Number.isInteger(offset) || offset < 0 || offset + overlay.length > target.length)
  process.exit(1);

overlay.copy(target, offset);
fs.writeFileSync(targetPath, target);
console.log(`  overlay: ${overlay.length} bytes at +0x${offset.toString(16)}`);
