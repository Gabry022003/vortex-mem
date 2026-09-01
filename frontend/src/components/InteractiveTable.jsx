import React, { useState, useMemo, useEffect } from 'react';
import { Search, ArrowUpDown, ChevronLeft, ChevronRight } from 'lucide-react';

export default function InteractiveTable({ columns, data, rowKey }) {
  const [sortConfig, setSortConfig] = useState({ key: null, direction: 'asc' });
  const [filterText, setFilterText] = useState('');
  const [openStacks, setOpenStacks] = useState({});
  const [page, setPage] = useState(1);
  const [pageSize, setPageSize] = useState(50);

  const toggleStack = (id) => setOpenStacks(p => ({ ...p, [id]: !p[id] }));

  useEffect(() => {
    setPage(1);
  }, [filterText, sortConfig, pageSize]);

  const sortedAndFilteredData = useMemo(() => {
    let filtered = (data || []).map((item, idx) => ({
      ...item,
      _vx_uid: item.id !== undefined ? item.id : (rowKey && item[rowKey] !== undefined ? `${item[rowKey]}_${idx}` : `row_${idx}`)
    }));
    if (filterText) {
      const lower = filterText.toLowerCase();
      filtered = filtered.filter(item =>
        Object.entries(item).some(([k, val]) => k !== '_vx_uid' && String(val).toLowerCase().includes(lower))
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
  }, [data, sortConfig, filterText, rowKey]);

  const totalItems = sortedAndFilteredData.length;
  const effectivePageSize = pageSize === -1 ? totalItems : pageSize;
  const totalPages = Math.max(1, Math.ceil(totalItems / (effectivePageSize || 1)));
  const currentPage = Math.min(page, totalPages);

  const paginatedData = useMemo(() => {
    if (pageSize === -1) return sortedAndFilteredData;
    const start = (currentPage - 1) * pageSize;
    return sortedAndFilteredData.slice(start, start + pageSize);
  }, [sortedAndFilteredData, currentPage, pageSize]);

  const requestSort = (key) => {
    let direction = 'asc';
    if (sortConfig.key === key && sortConfig.direction === 'asc') direction = 'desc';
    setSortConfig({ key, direction });
  };

  const startIdx = totalItems === 0 ? 0 : (currentPage - 1) * (pageSize === -1 ? totalItems : pageSize) + 1;
  const endIdx = pageSize === -1 ? totalItems : Math.min(currentPage * pageSize, totalItems);

  return (
    <div>
      <div style={{ marginBottom: '15px', display: 'flex', gap: '10px', alignItems: 'center' }}>
        <div style={{ position: 'relative', flex: 1 }}>
          <Search style={{ position: 'absolute', left: '10px', top: '10px', color: '#00d2ff', width: '18px' }} />
          <input
            type="text"
            placeholder="Filter results..."
            value={filterText}
            onChange={(e) => setFilterText(e.target.value)}
            style={{ width: '100%', padding: '10px 10px 10px 35px', borderRadius: '8px', border: '1px solid rgba(0, 210, 255, 0.3)', background: 'rgba(0,0,0,0.2)', color: '#fff', outline: 'none' }}
          />
        </div>
        <div style={{ display: 'flex', alignItems: 'center', gap: '8px', color: '#a9b7c6', fontSize: '0.9em' }}>
          <span>Show:</span>
          <select
            value={pageSize}
            onChange={(e) => setPageSize(Number(e.target.value))}
            style={{ padding: '8px', borderRadius: '6px', background: 'rgba(0,0,0,0.3)', color: '#fff', border: '1px solid rgba(0, 210, 255, 0.3)', outline: 'none' }}
          >
            <option value={25}>25</option>
            <option value={50}>50</option>
            <option value={100}>100</option>
            <option value={-1}>All</option>
          </select>
        </div>
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
            {paginatedData.map((row) => {
              const id = row._vx_uid;
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
      {totalItems > 0 && pageSize !== -1 && totalPages > 1 && (
        <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginTop: '15px', color: '#a9b7c6', fontSize: '0.9em' }}>
          <div>Showing {startIdx}-{endIdx} of {totalItems}</div>
          <div style={{ display: 'flex', gap: '8px', alignItems: 'center' }}>
            <button
              className="btn-view"
              onClick={() => setPage(p => Math.max(1, p - 1))}
              disabled={currentPage <= 1}
              style={{ opacity: currentPage <= 1 ? 0.4 : 1, cursor: currentPage <= 1 ? 'not-allowed' : 'pointer' }}
            >
              <ChevronLeft size={14} style={{ verticalAlign: 'middle' }} /> Prev
            </button>
            <span>Page {currentPage} of {totalPages}</span>
            <button
              className="btn-view"
              onClick={() => setPage(p => Math.min(totalPages, p + 1))}
              disabled={currentPage >= totalPages}
              style={{ opacity: currentPage >= totalPages ? 0.4 : 1, cursor: currentPage >= totalPages ? 'not-allowed' : 'pointer' }}
            >
              Next <ChevronRight size={14} style={{ verticalAlign: 'middle' }} />
            </button>
          </div>
        </div>
      )}
    </div>
  );
}
