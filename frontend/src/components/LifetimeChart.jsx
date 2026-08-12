import React from 'react';
import { Chart as ChartJS, CategoryScale, LinearScale, BarElement, LineElement, PointElement, BarController, LineController, Title, Tooltip, Legend } from 'chart.js';
import { Chart } from 'react-chartjs-2';
import { formatBytes } from '../utils';

ChartJS.register(CategoryScale, LinearScale, BarElement, LineElement, PointElement, BarController, LineController, Title, Tooltip, Legend);

function LifetimeChart({ data }) {
  if (!data || !data.buckets) return null;

  const chartData = {
    labels: data.buckets,
    datasets: [
      {
        type: 'line',
        label: 'Bytes',
        data: data.bytes,
        borderColor: '#ffa502',
        backgroundColor: '#ffa502',
        borderWidth: 2,
        yAxisID: 'y1',
        tension: 0.3,
        pointRadius: 4,
      },
      {
        type: 'bar',
        label: 'Allocations',
        data: data.counts,
        backgroundColor: data.buckets.map(b => b === 'never_freed' ? 'rgba(255, 71, 87, 0.7)' : 'rgba(0, 210, 255, 0.7)'),
        borderColor: data.buckets.map(b => b === 'never_freed' ? '#ff4757' : '#00d2ff'),
        borderWidth: 1,
        yAxisID: 'y',
      }
    ]
  };

  const options = {
    responsive: true,
    maintainAspectRatio: false,
    interaction: { mode: 'index', intersect: false },
    scales: {
      x: {
        grid: { color: 'rgba(255,255,255,0.05)' },
        ticks: { color: '#a9b7c6' }
      },
      y: {
        type: 'linear',
        display: true,
        position: 'left',
        grid: { color: 'rgba(255,255,255,0.05)' },
        ticks: { color: '#a9b7c6' },
        title: { display: true, text: 'Allocations Count', color: '#a9b7c6' }
      },
      y1: {
        type: 'linear',
        display: true,
        position: 'right',
        grid: { drawOnChartArea: false },
        ticks: {
          color: '#ffa502',
          callback: function (value) { return formatBytes(value); }
        },
        title: { display: true, text: 'Bytes Allocated', color: '#ffa502' }
      }
    },
    plugins: {
      tooltip: {
        callbacks: {
          label: function (context) {
            let label = context.dataset.label || '';
            if (label) label += ': ';
            if (context.dataset.yAxisID === 'y1') {
              label += formatBytes(context.raw);
            } else {
              label += context.raw;
            }
            return label;
          }
        }
      }
    }
  };

  return (
    <div className="glass-card">
      <h2 style={{ color: '#00d2ff', marginTop: 0 }}>Lifetime Distribution</h2>
      <div style={{ height: '350px' }}>
        <Chart type='bar' data={chartData} options={options} />
      </div>
    </div>
  );
}

export default LifetimeChart;
