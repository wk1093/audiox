'use strict';

// UI scripts reach the device through api.js; load it into each sandbox so a
// stubbed sandbox `fetch` keeps receiving the requests.
const fs = require('node:fs');
const vm = require('node:vm');

const apiSource = fs.readFileSync(require.resolve('../../../web/embedded/api.js'), 'utf8');

function apiContext(sandbox = {}) {
  const context = vm.createContext(sandbox);
  vm.runInContext(apiSource, context);
  return context;
}

module.exports = { apiContext };
