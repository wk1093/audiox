(function (root) {
  'use strict';

  const horizontalGap = 60;
  const verticalGap = 50;
  const patchGap = 70;

  function createCanvasResolution(graphCanvas) {
    let width = 1, height = 1, scaleX = 1, scaleY = 1;
    // LiteGraph 0.7 renders in CSS pixels and copies its background at native size.
    // Keep its viewport/input coordinates logical, but rasterize both layers at display density.
    for (const [method, contextKey, canvasKey] of [
      ['drawBackCanvas', 'bgctx', 'bgcanvas'], ['drawFrontCanvas', 'ctx', 'canvas'],
    ]) {
      const draw = graphCanvas[method];
      graphCanvas[method] = function (...args) {
        const ctx = this[contextKey] || this[canvasKey].getContext('2d');
        this[contextKey] = ctx;
        ctx.save();
        ctx.setTransform(scaleX, 0, 0, scaleY, 0, 0);
        const drawImage = ctx.drawImage;
        if (method === 'drawFrontCanvas') {
          ctx.drawImage = function (image, ...coordinates) {
            if (image === graphCanvas.bgcanvas && coordinates.length === 2)
              return drawImage.call(this, image, coordinates[0], coordinates[1], width, height);
            return drawImage.call(this, image, ...coordinates);
          };
        }
        try { return draw.apply(this, args); }
        finally {
          if (method === 'drawFrontCanvas') ctx.drawImage = drawImage;
          ctx.restore();
        }
      };
    }
    return {
      resize(cssWidth, cssHeight, pixelRatio = 1) {
        width = Math.max(1, Math.floor(cssWidth));
        height = Math.max(1, Math.floor(cssHeight));
        const ratio = Number.isFinite(pixelRatio) && pixelRatio > 0 ? pixelRatio : 1;
        const backingWidth = Math.max(1, Math.round(width * ratio));
        const backingHeight = Math.max(1, Math.round(height * ratio));
        graphCanvas.resize(backingWidth, backingHeight);
        for (const canvas of [graphCanvas.canvas, graphCanvas.bgcanvas]) {
          canvas.style.width = `${width}px`;
          canvas.style.height = `${height}px`;
        }
        scaleX = backingWidth / width;
        scaleY = backingHeight / height;
        graphCanvas.viewport = [0, 0, width, height];
        graphCanvas.ds.viewport = graphCanvas.viewport;
        graphCanvas.ds.computeVisibleArea(graphCanvas.viewport);
        graphCanvas.setDirty(true, true);
      },
      size() { return [width, height]; },
    };
  }

  function columnForNode(node) {
    if (node.kind === 'routing_helper') return 1;
    if (node.kind === 'effect') return 2;
    return node.outputs > 0 ? 0 : 3;
  }

  function columnLayout(nodes) {
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
        y += node.height + verticalGap;
        width = Math.max(width, node.width);
      }
      x += width + horizontalGap;
    }
    return positions;
  }

  function layoutNodes(nodes, routes = []) {
    const byId = new Map(nodes.map(node => [node.id, node]));
    const compare = (a, b) => a < b ? -1 : a > b ? 1 : 0;
    const outgoing = new Map(nodes.map(node => [node.id, new Set()]));
    const neighbors = new Map(nodes.map(node => [node.id, new Set()]));
    for (const route of routes) {
      if (!byId.has(route.src) || !byId.has(route.dst) || route.src === route.dst) continue;
      // Stereo/parallel channel wires are one relationship for node placement.
      outgoing.get(route.src).add(route.dst);
      neighbors.get(route.src).add(route.dst);
      neighbors.get(route.dst).add(route.src);
    }
    if (!nodes.some(node => neighbors.get(node.id).size)) return columnLayout(nodes);

    const positions = {};
    const visited = new Set();
    let top = 60;
    for (const id of [...byId.keys()].sort(compare)) {
      if (visited.has(id) || !neighbors.get(id).size) continue;
      const component = [];
      const pending = [id];
      visited.add(id);
      while (pending.length) {
        const current = pending.pop();
        component.push(current);
        for (const neighbor of neighbors.get(current)) {
          if (visited.has(neighbor)) continue;
          visited.add(neighbor);
          pending.push(neighbor);
        }
      }
      component.sort(compare);

      // Collapse cycles for ranking; every edge between groups then points right.
      const index = new Map(), low = new Map(), groupFor = new Map();
      const stack = [], stacked = new Set(), groups = [];
      const visit = current => {
        index.set(current, index.size);
        low.set(current, index.get(current));
        stack.push(current);
        stacked.add(current);
        for (const next of [...outgoing.get(current)].sort(compare)) {
          if (!index.has(next)) {
            visit(next);
            low.set(current, Math.min(low.get(current), low.get(next)));
          } else if (stacked.has(next)) {
            low.set(current, Math.min(low.get(current), index.get(next)));
          }
        }
        if (low.get(current) !== index.get(current)) return;
        const group = [];
        let member;
        do {
          member = stack.pop();
          stacked.delete(member);
          groupFor.set(member, groups.length);
          group.push(member);
        } while (member !== current);
        groups.push(group.sort(compare));
      };
      for (const current of component) if (!index.has(current)) visit(current);
      const rank = groups.map(() => 0);
      const predecessors = groups.map(() => new Set());
      for (const src of component) {
        for (const dst of outgoing.get(src)) {
          if (groupFor.get(src) !== groupFor.get(dst))
            predecessors[groupFor.get(dst)].add(groupFor.get(src));
        }
      }
      const ranked = new Set();
      const rankGroup = group => {
        if (ranked.has(group)) return rank[group];
        for (const predecessor of predecessors[group])
          rank[group] = Math.max(rank[group], rankGroup(predecessor) + 1);
        ranked.add(group);
        return rank[group];
      };
      groups.forEach((_, group) => rankGroup(group));
      const flowRank = [...rank];
      const folded = groups.map(() => false);
      for (const group of groups.map((_, i) => i).sort((a, b) => flowRank[a] - flowRank[b])) {
        const previous = [...predecessors[group]];
        const predecessor = previous[0];
        const canFold = previous.length === 1 && groups[group].length === 1 &&
          groups[predecessor].length === 1 && !folded[predecessor] &&
          byId.get(groups[group][0]).kind === 'effect' &&
          byId.get(groups[predecessor][0]).kind === 'effect' &&
          outgoing.get(groups[predecessor][0]).size === 1;
        rank[group] = previous.length ?
          Math.max(...previous.map(p => rank[p] + (canFold ? 0 : 1))) : 0;
        folded[group] = canFold;
      }
      const earlyRank = [...rank];
      for (const group of groups.map((_, i) => i).sort((a, b) => flowRank[b] - flowRank[a])) {
        const successors = new Set(groups[group].flatMap(id => [...outgoing.get(id)])
          .map(id => groupFor.get(id)).filter(next => next !== group));
        if (successors.size)
          rank[group] = Math.min(...[...successors].map(next =>
            rank[next] - (earlyRank[next] === earlyRank[group] ? 0 : 1)));
      }
      const layers = Array.from({ length: Math.max(...rank) + 1 }, () => []);
      const vertices = component.map(current => ({
        node: byId.get(current), rank: rank[groupFor.get(current)], before: [], after: [],
      }));
      const vertexFor = new Map(vertices.map(vertex => [vertex.node.id, vertex]));
      for (const vertex of vertices) layers[vertex.rank].push(vertex);
      const segments = [];
      for (const src of component) {
        for (const dst of [...outgoing.get(src)].sort(compare)) {
          let previous = vertexFor.get(src);
          const target = vertexFor.get(dst);
          if (previous.rank === target.rank) continue;
          // Virtual vertices let long routes influence every intervening layer.
          for (let level = previous.rank + 1; level <= target.rank; level++) {
            const next = level === target.rank ? target : {
              rank: level, before: [], after: [],
            };
            if (next !== target) layers[level].push(next);
            previous.after.push(next);
            next.before.push(previous);
            segments.push([previous, next]);
            previous = next;
          }
        }
      }
      const order = () => layers.forEach(layer => layer.forEach((vertex, i) => { vertex.order = i; }));
      const crossings = () => {
        order();
        let count = 0;
        for (let i = 0; i < segments.length; i++) {
          const [a, b] = segments[i];
          for (let j = i + 1; j < segments.length; j++) {
            const [c, d] = segments[j];
            if (a.rank === c.rank && (a.order - c.order) * (b.order - d.order) < 0) count++;
          }
        }
        return count;
      };
      let best = layers.map(layer => [...layer]), bestCrossings = crossings();
      for (let pass = 0; pass < 12; pass++) {
        const forward = pass % 2 === 0;
        const levels = layers.map((_, level) => level);
        if (!forward) levels.reverse();
        for (const level of levels) {
          order();
          const scores = new Map(layers[level].map(vertex => {
            const adjacent = forward ? vertex.before : vertex.after;
            return [vertex, adjacent.length ?
              adjacent.reduce((sum, neighbor) => sum + neighbor.order, 0) / adjacent.length :
              vertex.order];
          }));
          layers[level].sort((a, b) => scores.get(a) - scores.get(b) || a.order - b.order);
        }
        const count = crossings();
        if (count < bestCrossings) {
          bestCrossings = count;
          best = layers.map(layer => [...layer]);
        }
        if (bestCrossings === 0) break;
      }
      const realLayers = best.map(layer => layer.filter(vertex => vertex.node));
      for (let level = 0; level < realLayers.length; level++) {
        const layer = realLayers[level];
        // Keep vertically folded serial pairs in signal order.
        const pairs = [];
        const assigned = new Set();
        for (const vertex of layer) {
          if (assigned.has(vertex)) continue;
          const group = groupFor.get(vertex.node.id);
          const next = folded[group] ? vertexFor.get(groups[[...predecessors[group]][0]][0]) :
            [...outgoing.get(vertex.node.id)].map(id => vertexFor.get(id))
              .find(candidate => folded[groupFor.get(candidate.node.id)] && candidate.rank === level);
          const pair = next && !assigned.has(next) ? [vertex, next] : [vertex];
          pair.sort((a, b) => flowRank[groupFor.get(a.node.id)] - flowRank[groupFor.get(b.node.id)]);
          pair.forEach(member => assigned.add(member));
          pairs.push(pair);
        }
        realLayers[level] = pairs.flat();
      }
      const heights = realLayers.map(layer =>
        layer.reduce((sum, vertex) => sum + vertex.node.height, 0) +
        Math.max(0, layer.length - 1) * verticalGap);
      const height = Math.max(...heights);
      let x = 40;
      realLayers.forEach((layer, level) => {
        let y = top + (height - heights[level]) / 2;
        for (const { node } of layer) {
          positions[node.id] = { x, y };
          y += node.height + verticalGap;
        }
        x += Math.max(0, ...layer.map(vertex => vertex.node.width)) + horizontalGap;
      });
      top += height + patchGap;
    }
    const isolated = columnLayout(nodes.filter(node => !visited.has(node.id)));
    for (const [id, position] of Object.entries(isolated))
      positions[id] = { x: position.x, y: position.y + top - 60 };
    return positions;
  }

  function wireCurve(a, b) {
    const c = Math.max(40, Math.min(220, Math.abs(b[0] - a[0]) * .45));
    return [a, [a[0] + c, a[1]], [b[0] - c, b[1]], b];
  }

  function wirePath(a, b) {
    const [start, first, second, end] = wireCurve(a, b);
    return Array.from({ length: 49 }, (_, i) => {
      const t = i / 48, u = 1 - t;
      return [u * u * u * start[0] + 3 * u * u * t * first[0] +
        3 * u * t * t * second[0] + t * t * t * end[0],
      u * u * u * start[1] + 3 * u * u * t * first[1] +
        3 * u * t * t * second[1] + t * t * t * end[1]];
    });
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

  function channelLabel(channel, count) {
    if (count === 1) return 'Mono';
    if (count === 2) return channel === 0 ? 'Left' : 'Right';
    return `Channel ${channel + 1}`;
  }

  const exported = { createCanvasResolution, columnForNode, layoutNodes, wireCurve, wirePath, fitNodes, channelLabel };
  if (typeof module !== 'undefined' && module.exports) module.exports = exported;
  else root.AudioxRouting = exported;
})(globalThis);
