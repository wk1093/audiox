'use strict';

const fs = require('node:fs');
const path = require('node:path');

const files = [
  ['litegraph.js/build/litegraph.js', 'litegraph.js'],
  ['litegraph.js/css/litegraph.css', 'litegraph.css'],
  ['litegraph.js/LICENSE', 'litegraph-LICENSE.txt'],
  ['@fontsource/space-grotesk/LICENSE', 'space-grotesk-LICENSE.txt'],
  ...[400, 500, 600, 700].map((weight) => [
    `@fontsource/space-grotesk/files/space-grotesk-latin-${weight}-normal.woff2`,
    `space-grotesk-${weight}.woff2`,
  ]),
];

function stageVendor(destination) {
  fs.mkdirSync(destination, { recursive: true });
  for (const [source, target] of files) {
    fs.copyFileSync(path.join(__dirname, 'node_modules', source), path.join(destination, target));
  }
}

if (require.main === module) {
  stageVendor(path.resolve(process.argv[2] || path.join(__dirname, 'vendor')));
}

module.exports = { stageVendor };
