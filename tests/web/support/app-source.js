'use strict';

const fs = require('node:fs');
const path = require('node:path');

const embedded = path.resolve(__dirname, '../../../web/embedded');

function appSource() {
  const html = fs.readFileSync(path.join(embedded, 'index.html'), 'utf8');
  return [...html.matchAll(/<script src="\/(app\/[^"]+\.js|app\.js)"><\/script>/g)]
    .map(([, name]) => fs.readFileSync(path.join(embedded, name), 'utf8')).join('\n');
}

module.exports = { appSource };
