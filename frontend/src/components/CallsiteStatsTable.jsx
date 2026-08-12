import React from 'react';
import InteractiveTable from './InteractiveTable';
import { formatBytes, formatDuration } from '../utils';

function CallsiteStatsTable({ data }) {
  if (!data || data.length === 0) return null;

  const columns = [
    {
      key: 'stacktrace',
      label: 'Callsite',
      sortable: false,
      render: (val, row) => {
        return null;
      }
    },
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
    { key: 'full_stacktrace', label: 'Stack Trace', sortable: false }
  ];

  const modifiedColumns = [
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
    let firstLine = 'Unknown';
    if (item.stacktrace) {
      firstLine = item.stacktrace.split(/[\n;]/)[0];
    }
    return {
      ...item,
      id: idx,
      callsite: firstLine,
    };
  });

  return (
    <div className="glass-card">
      <h2 style={{ color: '#00d2ff', marginTop: 0 }}>Callsite Stats</h2>
      <InteractiveTable
        rowKey="id"
        data={transformedData}
        columns={modifiedColumns}
      />
    </div>
  );
}

export default CallsiteStatsTable;
