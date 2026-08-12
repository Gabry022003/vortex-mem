import React, { useState, useEffect } from 'react';
import { UploadCloud } from 'lucide-react';

import InteractiveTable from './components/InteractiveTable';
import AnalysisPanel from './components/AnalysisPanel';
import MemoryFlameGraph from './components/MemoryFlameGraph';
import CallsiteStatsTable from './components/CallsiteStatsTable';
import LifetimeChart from './components/LifetimeChart';
import TimelineChart from './components/TimelineChart';

import { formatBytes } from './utils';

function App() {
  const [data, setData] = useState(null);
  const [dragActive, setDragActive] = useState(false);

  const loadReport = () => {
    fetch('/vortex_report.json?t=' + Date.now())
      .then(res => {
        if (!res.ok) throw new Error('Not found');
        return res.json();
      })
      .then(json => {
        setData(prev => ({ ...(prev || {}), ...json }));
      })
      .catch(err => {});
  };

  useEffect(() => {
    loadReport();
    
    const evtSource = new EventSource('/live');
    evtSource.onmessage = (e) => {
      try {
        const msg = JSON.parse(e.data);
        if (msg.action === 'reload') {
          loadReport();
          return;
        }
        
        setData(prev => {
          const base = prev || { timeline: [], summary: {}, leaks: [], errors: [], callsite_stats: [] };
          return { ...base, timeline: [...(base.timeline || []), msg] };
        });
      } catch(err) {}
    };
      
    return () => {
      evtSource.close();
    };
  }, []);

  const handleDrag = (e) => {
    e.preventDefault(); e.stopPropagation();
    if (e.type === "dragenter" || e.type === "dragover") setDragActive(true);
    else if (e.type === "dragleave") setDragActive(false);
  };

  const processFile = (file) => {
    if (!file) return;
    const reader = new FileReader();
    reader.onload = (e) => {
      try {
        const json = JSON.parse(e.target.result);
        setData(json);
      } catch (err) {
        alert("Invalid JSON file");
      }
    };
    reader.readAsText(file);
  };

  const handleDrop = (e) => {
    e.preventDefault(); e.stopPropagation();
    setDragActive(false);
    if (e.dataTransfer.files && e.dataTransfer.files[0]) processFile(e.dataTransfer.files[0]);
  };

  const hasContent = data && (data.summary || (data.timeline && data.timeline.length > 0));

  if (!hasContent) {
    return (
      <div style={{ maxWidth: '800px', margin: '100px auto', padding: '20px' }}>
        <h1 style={{ textAlign: 'center', fontSize: '3em', fontWeight: 800, marginBottom: '40px' }}>
          <span style={{ color: '#fff' }}>Vortex</span> <span className="text-gradient">Memory UI</span>
        </h1>
        <div 
          className={`glass-card drop-zone ${dragActive ? "active" : ""}`}
          onDragEnter={handleDrag} onDragLeave={handleDrag} onDragOver={handleDrag} onDrop={handleDrop}
          onClick={() => document.getElementById("file-upload").click()}
        >
          <input type="file" id="file-upload" accept=".json" onChange={(e) => processFile(e.target.files[0])} style={{ display: 'none' }} />
          <UploadCloud className="drop-icon" />
          <h2 style={{ color: '#fff', marginBottom: '10px' }}>Waiting for Vortex...</h2>
          <p style={{ color: '#a9b7c6' }}>Run <code>./vortex run ./your_program</code> or drag a report here.</p>
        </div>
      </div>
    );
  }

  const totalCallsites = data.callsite_stats?.length || 0;
  const totalAllocs = data.callsite_stats?.reduce((sum, c) => sum + (c.alloc_count || 0), 0) || 0;

  return (
    <div style={{ maxWidth: '1200px', margin: '0 auto', padding: '20px' }}>
      {}
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: '40px' }}>
        <h1 style={{ fontSize: '2.5em', fontWeight: 800, margin: 0 }}>
          <span style={{ color: '#fff' }}>Vortex</span> <span className="text-gradient">Memory UI</span>
        </h1>
        <button className="btn-view" onClick={() => setData(null)}>Load Another File</button>
      </div>
      
      {}
      <div style={{ display: 'grid', gridTemplateColumns: 'repeat(5, 1fr)', gap: '20px', marginBottom: '30px' }}>
        <div className="glass-card" style={{ marginBottom: 0 }}>
          <h3 style={{ color: '#ff4757', marginTop: 0 }}>Memory Errors</h3>
          <p style={{ fontSize: '2em', margin: '10px 0', fontWeight: 'bold', color: '#ff4757' }}>{data.summary?.errors || 0}</p>
          <p style={{ color: '#a9b7c6', margin: 0 }}>Double frees, overflows, etc.</p>
        </div>
        <div className="glass-card" style={{ marginBottom: 0 }}>
          <h3 style={{ color: '#ffa502', marginTop: 0 }}>Total Leaks</h3>
          <p style={{ fontSize: '2em', margin: '10px 0', fontWeight: 'bold' }}>{data.summary?.leaks || 0}</p>
          <p style={{ color: '#a9b7c6', margin: 0 }}>{formatBytes(data.summary?.leaked_bytes || 0)} lost</p>
        </div>
        <div className="glass-card" style={{ marginBottom: 0 }}>
          <h3 style={{ color: '#00d2ff', marginTop: 0 }}>Callsite Stats</h3>
          <p style={{ fontSize: '2em', margin: '10px 0', fontWeight: 'bold' }}>{totalCallsites}</p>
          <p style={{ color: '#a9b7c6', margin: 0 }}>{totalAllocs.toLocaleString()} total allocs</p>
        </div>
        <div className="glass-card" style={{ marginBottom: 0 }}>
          <h3 style={{ color: '#eccc68', marginTop: 0 }}>Minor Faults</h3>
          <p style={{ fontSize: '2em', margin: '10px 0', fontWeight: 'bold' }}>{data.summary?.page_faults?.minor?.toLocaleString() || 0}</p>
          <p style={{ color: '#a9b7c6', margin: 0, fontSize: '0.9em' }}>RAM pages mapped</p>
        </div>
        <div className="glass-card" style={{ marginBottom: 0, borderLeft: (data.summary?.page_faults?.major > 0) ? '4px solid #ff4757' : 'none' }}>
          <h3 style={{ color: (data.summary?.page_faults?.major > 0) ? '#ff4757' : '#a9b7c6', marginTop: 0 }}>Major Faults</h3>
          <p style={{ fontSize: '2em', margin: '10px 0', fontWeight: 'bold', color: (data.summary?.page_faults?.major > 0) ? '#ff4757' : 'inherit' }}>{data.summary?.page_faults?.major?.toLocaleString() || 0}</p>
          <p style={{ color: '#a9b7c6', margin: 0, fontSize: '0.9em' }}>Disk I/O swap</p>
        </div>
      </div>

      {}
      <AnalysisPanel analysis={data.analysis} />

      {}
      <MemoryFlameGraph data={data.flame_graph} />

      {}
      <TimelineChart timeline={data.timeline} events={data.timeline_events} />

      {}
      <LifetimeChart data={data.lifetime_distribution} />

      {}
      <CallsiteStatsTable data={data.callsite_stats} />

      {}
      {data.leaks?.length > 0 && (
        <div className="glass-card">
          <h2 style={{ color: '#00d2ff', marginTop: 0 }}>Memory Leaks</h2>
          <InteractiveTable 
            rowKey="address" data={data.leaks}
            columns={[
              { key: 'address', label: 'Address' },
              { key: 'size', label: 'Size', render: v => formatBytes(v) },
              { key: 'thread_id', label: 'Thread ID' },
              { key: 'stacktrace', label: 'Stack Trace', sortable: false }
            ]}
          />
        </div>
      )}

      {}
      {data.errors?.length > 0 && (
        <div className="glass-card">
          <h2 style={{ color: '#ff4757', marginTop: 0 }}>Errors</h2>
          <InteractiveTable 
            rowKey="address" data={data.errors}
            columns={[
              { key: 'type', label: 'Type', render: v => <span style={{color: '#ff4757'}}>{v}</span> },
              { key: 'address', label: 'Address' },
              { key: 'size', label: 'Size', render: v => formatBytes(v) },
              { key: 'stacktrace', label: 'Stack Trace', sortable: false }
            ]}
          />
        </div>
      )}

    </div>
  );
}

export default App;
