import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

export default defineConfig({
  plugins: [react()],
  server: {
    port: 5173,
    host: true,
    proxy: {
      '/live': {
        target: 'http://localhost:8000',
        changeOrigin: true
      },
      '/vortex_report.json': {
        target: 'http://localhost:8000',
        changeOrigin: true
      },
      '/resolve': {
        target: 'http://localhost:8000',
        changeOrigin: true
      }
    }
  }
})
