(function (root) {
  'use strict';
  const copy = (value) => JSON.parse(JSON.stringify(value));
  const signature = (edges) => JSON.stringify(edges.map((edge) => [
    edge.src, edge.dst, edge.srcChannel, edge.dstChannel,
  ]).sort((a, b) => JSON.stringify(a).localeCompare(JSON.stringify(b))));

  function create({ apply, changed, limit = 100 }) {
    let entries = [[]], cursor = 0, saved = signature([]);
    const notify = () => changed({
      undo: cursor > 0, redo: cursor < entries.length - 1,
      dirty: signature(entries[cursor]) !== saved,
    });
    return {
      reset(edges) {
        entries = [copy(edges)];
        cursor = 0;
        saved = signature(edges);
        notify();
      },
      record(edges) {
        if (signature(edges) === signature(entries[cursor])) return;
        entries = entries.slice(0, cursor + 1);
        entries.push(copy(edges));
        if (entries.length > limit) entries.shift();
        cursor = entries.length - 1;
        notify();
      },
      saved(edges) {
        saved = signature(edges);
        notify();
      },
      undo() {
        if (!cursor) return false;
        apply(copy(entries[--cursor]));
        notify();
        return true;
      },
      redo() {
        if (cursor >= entries.length - 1) return false;
        apply(copy(entries[++cursor]));
        notify();
        return true;
      },
    };
  }
  const api = { create };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.AudioxRoutingHistory = api;
})(typeof window !== 'undefined' ? window : globalThis);
