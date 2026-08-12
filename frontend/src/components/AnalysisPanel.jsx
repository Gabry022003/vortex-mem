import React from 'react';
import { AlertTriangle, AlertCircle, Info, Cpu } from 'lucide-react';
import { formatBytes } from '../utils';

function AnalysisPanel({ analysis }) {
  if (!analysis || analysis.length === 0) return null;

  return (
    <div className="glass-card">
      <h2 style={{ color: '#00d2ff', marginTop: 0, display: 'flex', alignItems: 'center', gap: '10px' }}>
        <Cpu /> Smart Analysis
      </h2>
      <div style={{ display: 'flex', flexDirection: 'column', gap: '15px', marginTop: '20px' }}>
        {analysis.map((item, idx) => {
          let Icon = Info;
          let color = '#00d2ff';
          if (item.severity === 'critical') {
            Icon = AlertCircle;
            color = '#ff4757';
          } else if (item.severity === 'warning') {
            Icon = AlertTriangle;
            color = '#ffa502';
          }

          return (
            <div key={idx} className={`analysis-card ${item.severity}`}>
              <div className="analysis-header">
                <Icon color={color} size={24} />
                <h3 className="analysis-title" style={{ color }}>{item.title}</h3>
                <div style={{ flex: 1 }} />
                <div className="badge">
                  <span>{item.alloc_count} allocs</span>
                  {item.total_bytes && <span>• {formatBytes(item.total_bytes)}</span>}
                </div>
              </div>
              <p style={{ margin: '0 0 10px 0', color: '#f8f9fa', fontSize: '0.95em' }}>
                {item.description}
              </p>
              {item.suggestion && (
                <p style={{ margin: '0 0 10px 0', color: '#2ecc71', fontSize: '0.9em', fontWeight: 600 }}>
                  Suggestion: {item.suggestion}
                </p>
              )}
              {item.stacktrace && (
                <div className="stack-block" style={{ marginTop: '10px' }}>
                  {item.stacktrace}
                </div>
              )}
            </div>
          );
        })}
      </div>
    </div>
  );
}

export default AnalysisPanel;
