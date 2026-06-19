import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

// Renderer-Build. base: './' damit Electron die Assets per file:// laden kann.
export default defineConfig({
  plugins: [react()],
  base: './',
  build: {
    outDir: 'dist',
    emptyOutDir: true
  },
  server: {
    port: 5173,
    strictPort: true
  }
})
