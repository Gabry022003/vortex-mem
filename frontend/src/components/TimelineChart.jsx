import React, { useMemo } from 'react';
import { Chart as ChartJS, CategoryScale, LinearScale, PointElement, LineElement, LineController, Title, Tooltip, Legend, Filler } from 'chart.js';
import { Line } from 'react-chartjs-2';
import { formatBytes } from '../utils';

ChartJS.register(CategoryScale, LinearScale, PointElement, LineElement, LineController, Title, Tooltip, Legend, Filler);

function TimelineChart({ timeline, events }) {
  if (!timeline || timeline.length === 0) return null;

  const chartData = useMemo(() => {
    const data = {
      labels: timeline.map(t => t.time_ms),
      datasets: [
        {
          label: 'Memory Used (Bytes)',
          data: timeline.map(t => t.bytes),
          borderColor: '#00d2ff',
          backgroundColor: 'rgba(0, 210, 255, 0.15)',
          fill: true,
          borderWidth: 2,
          pointRadius: 0,
          pointHitRadius: 10,
          tension: 0.4
        }
      ]
    };

    if (events && events.length > 0) {
      const eventData = timeline.map(t => null);
      const eventLabels = timeline.map(t => null);

      events.forEach(evt => {
        let closestIdx = 0;
        let minDiff = Infinity;
        timeline.forEach((t, idx) => {
          const diff = Math.abs(t.time_ms - evt.time_ms);
          if (diff < minDiff) {
            minDiff = diff;
            closestIdx = idx;
          }
        });

        eventData[closestIdx] = timeline[closestIdx].bytes;
        eventLabels[closestIdx] = evt.label || evt.type;
      });

      data.datasets.push({
        label: 'Events',
        data: eventData,
        borderColor: '#ff4757',
        backgroundColor: '#ff4757',
        pointStyle: 'triangle',
        pointRadius: 8,
        pointHoverRadius: 12,
        showLine: false,
        customLabels: eventLabels
      });
    }

    return data;
  }, [timeline, events]);

  const options = {
    responsive: true,
    maintainAspectRatio: false,
    interaction: { mode: 'index', intersect: false },
    scales: {
      x: {
        grid: { color: 'rgba(255,255,255,0.05)' },
        title: { display: true, text: 'Time (ms)', color: '#00d2ff' },
        ticks: { color: '#a9b7c6' }
      },
      y: {
        grid: { color: 'rgba(255,255,255,0.05)' },
        beginAtZero: true,
        ticks: {
          color: '#a9b7c6',
          callback: function (value) { return formatBytes(value); }
        }
      }
    },
    plugins: {
      tooltip: {
        callbacks: {
          label: function (context) {
            if (context.dataset.label === 'Events') {
              const lbl = context.dataset.customLabels[context.dataIndex];
              return lbl ? `Event: ${lbl}` : 'Event';
            }
            return `Memory: ${formatBytes(context.raw)}`;
          }
        }
      }
    }
  };

  return (
    <div className="glass-card">
      <h2 style={{ color: '#00d2ff', marginTop: 0 }}>Memory Usage Timeline</h2>
      <div style={{ height: '350px' }}>
        <Line data={chartData} options={options} />
      </div>
    </div>
  );
}

export default TimelineChart;
