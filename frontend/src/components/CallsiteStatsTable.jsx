import React from 'react';
import InteractiveTable from './InteractiveTable';
import { formatBytes, formatDuration } from '../utils';

function CallsiteStatsTable({ data }) {
  if (!data || data.length === 0) return null;

  const columns = [
    { key: 'callsite', label: 'Callsite' },
    { key: 'alloc_count', label: 'Allocs' },
    { key: 'free_count', label: 'Frees' },
    {
      key: 'leak_rate',
      label: 'Leak Rate %',
      render: (val) => {
        let colorClass = 'green';
        if (val > 50) colorClass = 'red';
        else if (val >= 10) colorClass = 'yellow';
        return <span className={`pill ${colorClass}`}>{val.toFixed(1)}%</span>;
      }
    },
    { key: 'total_bytes', label: 'Total Bytes', render: (val) => formatBytes(val) },
    { key: 'current_bytes', label: 'Current Held', render: (val) => formatBytes(val) },
    { key: 'avg_lifetime_ms', label: 'Avg Lifetime', render: (val) => formatDuration(val) },
    { key: 'stacktrace', label: 'Trace', sortable: false }
  ];

  const transformedData = data.map((item, idx) => {
    let callsiteDisplay = 'Unknown';
    if (item.stacktrace) {
      const lines = item.stacktrace.split('\n').map(l => l.trim()).filter(Boolean);
      const symbol = lines[0] || 'Unknown';
      const fileInfoLine = lines.find(l => l.startsWith('at ') && !l.includes('??:'));
      if (fileInfoLine) {
        const fileInfo = fileInfoLine.replace(/^at\s+/, '');
        callsiteDisplay = `${symbol} (${fileInfo})`;
      } else {
        callsiteDisplay = symbol;
      }
    }
    return {
      ...item,
      id: idx,
      callsite: callsiteDisplay,
    };
  });

  return (
    <div className="glass-card">
      <h2 style={{ color: '#00d2ff', marginTop: 0 }}>Callsite Stats</h2>
      <InteractiveTable
        rowKey="id"
        data={transformedData}
        columns={columns}
      />
    </div>
  );
}

export default CallsiteStatsTable;
