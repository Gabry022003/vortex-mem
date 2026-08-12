import React, { useState, useMemo } from 'react';
import { formatBytes } from '../utils';

function buildFlameTree(data) {
  const root = { name: 'all', value: 0, children: [], depth: 0 };
  for (const entry of data) {
    let node = root;
    for (let i = 0; i < entry.frames.length; i++) {
      const frame = entry.frames[i];
      let child = node.children.find(c => c.name === frame);
      if (!child) {
        child = { name: frame, value: 0, children: [], depth: i + 1 };
        node.children.push(child);
      }
      child.value += entry.bytes;
      node = child;
    }
  }
  root.value = root.children.reduce((sum, c) => sum + c.value, 0);
  return root;
}

function FlameNode({ node, totalValue, onZoom, maxDepth }) {
  if (node.value / totalValue < 0.005) return null;

  const width = (node.value / totalValue) * 100;
  
  const hue = Math.max(0, 60 - (node.depth * 10));
  const depthHue = Math.min(220, 30 + (node.depth * 15));
  const bgColor = `hsl(${depthHue}, 70%, 50%)`;

  return (
    <div style={{ display: 'flex', flexDirection: 'column', width: `${width}%` }}>
      <div 
        className="flame-bar"
        style={{ backgroundColor: bgColor }}
        onClick={(e) => { e.stopPropagation(); onZoom(node); }}
        title={`${node.name}\n${formatBytes(node.value)} (${width.toFixed(2)}%)`}
      >
        {width > 2 ? node.name : ''}
      </div>
      {node.children && node.children.length > 0 && (
        <div className="flame-row" style={{ width: '100%' }}>
          {node.children.sort((a,b) => b.value - a.value).map((child, i) => (
            <FlameNode 
              key={i} 
              node={child} 
              totalValue={node.value} 
              onZoom={onZoom}
              maxDepth={maxDepth}
            />
          ))}
        </div>
      )}
    </div>
  );
}

function MemoryFlameGraph({ data }) {
  const [zoomPath, setZoomPath] = useState([]);

  const tree = useMemo(() => {
    if (!data || data.length === 0) return null;
    return buildFlameTree(data);
  }, [data]);

  if (!tree) return null;

  let currentNode = tree;
  for (const step of zoomPath) {
    const next = currentNode.children.find(c => c.name === step);
    if (next) currentNode = next;
  }

  const handleZoom = (node) => {
    if (node.name === 'all') return;
    setZoomPath(prev => [...prev, node.name]);
  };

  const handleReset = () => setZoomPath([]);
  const handleCrumbClick = (index) => {
    setZoomPath(prev => prev.slice(0, index + 1));
  };

  return (
    <div className="glass-card">
      <h2 style={{ color: '#00d2ff', marginTop: 0 }}>Memory Flame Graph</h2>
      
      <div className="breadcrumb">
        <button className="breadcrumb-btn" onClick={handleReset}>Root</button>
        {zoomPath.map((crumb, idx) => (
          <React.Fragment key={idx}>
            <span style={{ color: '#666' }}>/</span>
            <button className="breadcrumb-btn" onClick={() => handleCrumbClick(idx)}>
              {crumb}
            </button>
          </React.Fragment>
        ))}
      </div>

      <div className="flame-graph-container">
        <div className="flame-row" style={{ width: '100%' }}>
          <FlameNode 
            node={currentNode} 
            totalValue={currentNode.value} 
            onZoom={handleZoom} 
          />
        </div>
      </div>
    </div>
  );
}

export default MemoryFlameGraph;
