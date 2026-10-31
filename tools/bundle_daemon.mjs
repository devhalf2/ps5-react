// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// Bundles an app's daemon entry (app.json "daemon") into one classic script for pandorad, the headless
// host (native/daemon). `@ps5-react/core` resolves to runtime/js/headless.js there: no React UI, screen or
// input, so a daemon that imports them fails here rather than on the console.
//   node tools/bundle_daemon.mjs <embedded-react> <entry.js> <out.js>
import {createRequire} from 'node:module';
import {fileURLToPath} from 'node:url';
import {dirname, resolve} from 'node:path';

const [embeddedReact, entry, outfile] = process.argv.slice(2);
const here = resolve(embeddedReact, 'bridges/quickjs/js');
const require = createRequire(resolve(here, 'package.json'));
const {build} = require('esbuild');
const runtime = resolve(dirname(fileURLToPath(import.meta.url)), '../runtime/js');

await build({
  entryPoints: [resolve(entry)],
  bundle: true,
  format: 'iife',
  outfile: resolve(outfile),
  platform: 'neutral',
  target: 'es2020',
  // One React copy, as tools/bundle.mjs pins it; native.js imports hooks the daemon never calls.
  alias: {
    '@ps5-react/core': resolve(runtime, 'headless.js'),
    react: resolve(here, 'node_modules/react'),
  },
  nodePaths: [resolve(here, 'node_modules')],
  define: {'process.env.NODE_ENV': '"production"'},
  legalComments: 'none',
  logLevel: 'info',
}).catch(() => process.exit(1));
