export function formatBytes(bytes) {
  if (bytes === null || bytes === undefined || isNaN(bytes) || bytes <= 0) return '0 B';
  const k = 1024;
  const sizes = ['B', 'KB', 'MB', 'GB', 'TB'];
  const i = Math.min(Math.floor(Math.log(bytes) / Math.log(k)), sizes.length - 1);
  return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
}

export function formatDuration(ms) {
  if (ms === null || ms === undefined || isNaN(ms) || ms < 0) return 'never freed';
  if (ms < 1) return (ms * 1000).toFixed(0) + ' μs';
  if (ms < 1000) return ms.toFixed(1) + ' ms';
  return (ms / 1000).toFixed(2) + ' s';
}
