(function (root) {
  'use strict';

  function columnForNode(node) {
    if (node.kind === 'routing_helper') return 1;
    if (node.kind === 'effect') return 2;
    return node.outputs > 0 ? 0 : 3;
  }

  function layoutNodes(nodes) {
    const columns = [[], [], [], []];
    for (const node of nodes) columns[columnForNode(node)].push(node);
    const positions = {};
    let x = 40;
    for (const column of columns) {
      if (!column.length) continue;
      let y = 60;
      let width = 0;
      for (const node of column) {
        positions[node.id] = { x, y };
        y += node.height + 70;
        width = Math.max(width, node.width);
      }
      x += width + 110;
    }
    return positions;
  }

  function fitNodes(nodes, width, height) {
    if (!nodes.length || width <= 0 || height <= 0) return null;
    const left = Math.min(...nodes.map((node) => node.x));
    const top = Math.min(...nodes.map((node) => node.y - 30));
    const right = Math.max(...nodes.map((node) => node.x + node.width));
    const bottom = Math.max(...nodes.map((node) => node.y + node.height));
    const padding = Math.min(24, width / 4, height / 4);
    const scale = Math.min(1, (width - padding * 2) / Math.max(1, right - left),
      (height - padding * 2) / Math.max(1, bottom - top));
    return {
      scale,
      offset: [(width / scale - (right - left)) / 2 - left, (height / scale - (bottom - top)) / 2 - top],
    };
  }

  const exported = { columnForNode, layoutNodes, fitNodes };
  if (typeof module !== 'undefined' && module.exports) module.exports = exported;
  else root.AudioxRouting = exported;
})(globalThis);
