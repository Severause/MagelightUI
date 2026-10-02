import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import { magelight } from '@magelight/vite-plugin';

export default defineConfig({
  plugins: [react(), magelight({ entries: { config: 'index.html' } })],
});
