import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import { magelight } from '@magelight/vite-plugin';

export default defineConfig({
  plugins: [react(), magelight({ entries: { config: 'index.html' } })],
  // A --local scaffold links @magelight/* by folder; without this Vite loads a second React from there and the page renders blank.
  resolve: { dedupe: ['react', 'react-dom'] },
});
