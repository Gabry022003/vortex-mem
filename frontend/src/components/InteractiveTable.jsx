import React, { useState, useMemo } from 'react';
import { Search, ArrowUpDown } from 'lucide-react';

export default function InteractiveTable({ columns, data, rowKey }) {
  const [sortConfig, setSortConfig] = useState({ key: null, direction: 'asc' });
  const [filterText, setFilterText] = useState('');
  const [openStacks, setOpenStacks] = useState({});

  const toggleStack = (id) => setOpenStacks(p => ({ ...p, [id]: !p[id] }));

  const sortedAndFilteredData = useMemo(() => {
    let filtered = data;
    if (filterText) {
      const lower = filterText.toLowerCase();
      filtered = data.filter(item =>
        Object.values(item).some(val => String(val).toLowerCase().includes(lower))
      );
    }
    if (sortConfig.key) {
      filtered = [...filtered].sort((a, b) => {
        let valA = a[sortConfig.key];
        let valB = b[sortConfig.key];
        if (typeof valA === 'string') valA = valA.toLowerCase();
        if (typeof valB === 'string') valB = valB.toLowerCase();

        if (valA < valB) return sortConfig.direction === 'asc' ? -1 : 1;
        if (valA > valB) return sortConfig.direction === 'asc' ? 1 : -1;
        return 0;
      });
    }
    return filtered;
  }, [data, sortConfig, filterText]);

  const requestSort = (key) => {
    let direction = 'asc';
    if (sortConfig.key === key && sortConfig.direction === 'asc') direction = 'desc';
    setSortConfig({ key, direction });
  };

  return (
    <div>
      <div style={{ marginBottom: '15px', position: 'relative' }}>
        <Search style={{ position: 'absolute', left: '10px', top: '10px', color: '#00d2ff', width: '18px' }} />
        <input
          type="text"
          placeholder="Filter results..."
          value={filterText}
          onChange={(e) => setFilterText(e.target.value)}
          style={{ width: '100%', padding: '10px 10px 10px 35px', borderRadius: '8px', border: '1px solid rgba(0, 210, 255, 0.3)', background: 'rgba(0,0,0,0.2)', color: '#fff', outline: 'none' }}
        />
      </div>
      <div style={{ overflowX: 'auto' }}>
        <table>
          <thead>
            <tr>
              {columns.map(col => (
                <th key={col.key} onClick={() => col.sortable !== false && requestSort(col.key)} style={{ cursor: col.sortable !== false ? 'pointer' : 'default' }}>
                  {col.label} {col.sortable !== false && <ArrowUpDown size={12} style={{ marginLeft: '5px', opacity: 0.5 }} />}
                </th>
              ))}
            </tr>
          </thead>
          <tbody>
            {sortedAndFilteredData.map((row, i) => {
              const id = rowKey ? row[rowKey] : i;
              return (
                <tr key={id}>
                  {columns.map(col => (
                    <td key={col.key}>
                      {col.key === 'stacktrace' ? (
                        row.stacktrace ? (
                          <>
                            <button className="btn-view" onClick={() => toggleStack(id)}>Toggle Trace</button>
                            {openStacks[id] && <div className="stack-block">{row.stacktrace}</div>}
                          </>
                        ) : 'No stack trace'
                      ) : (
                        col.render ? col.render(row[col.key], row) : row[col.key]
                      )}
                    </td>
                  ))}
                </tr>
              );
            })}
            {sortedAndFilteredData.length === 0 && (
              <tr><td colSpan={columns.length} style={{ textAlign: 'center', color: '#a9b7c6' }}>No results found.</td></tr>
            )}
          </tbody>
        </table>
      </div>
    </div>
  );
}
