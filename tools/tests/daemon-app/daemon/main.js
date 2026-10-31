// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// The fixture daemon tools/test_daemon.py drives: each route exercises one part of the contract.
import {Archives, Daemon, Downloads, FileSystem, Platform} from '@ps5-react/core';

const {version, stateDir} = Daemon.args;
const startedAt = Date.now();
FileSystem.mkdir(stateDir, {recursive: true});
console.log(`fixture daemon ${version} serving from ${stateDir}`);
let timers = 0;
setInterval(() => { timers += 1; }, 50);

const json = (status, value) => ({status, headers: {'content-type': 'application/json'}, body: JSON.stringify(value)});
const wait = ms => new Promise(resolve => setTimeout(resolve, ms));

Daemon.serve(async ({method, path, query, headers, body}) => {
  if (path === '/hello') return json(200, {protocol: 1, version, startedAt});
  if (path === '/echo') {
    return json(200, {method, path, query, headers, body, isDaemon: Daemon.isDaemon, supported: Daemon.supported,
      platform: Platform.OS, args: Daemon.args, timers, modules: Object.keys(globalThis.__ps5ReactNative).sort()});
  }
  if (path === '/download' && method === 'POST') {
    const {url, destination} = JSON.parse(body);
    FileSystem.mkdir(destination.slice(0, destination.lastIndexOf('/')), {recursive: true});
    const result = await Downloads.enqueue({url, destination, connections: 4, resume: false}).done;
    return json(200, {state: result.state, size: FileSystem.stat(destination).size});
  }
  if (path === '/extract' && method === 'POST') {
    const {source, destination} = JSON.parse(body);
    const result = await Archives.extract({sources: [source], destination}).done;
    return json(200, {state: result.state, files: FileSystem.readDir(destination).map(entry => entry.name).filter(name => !name.startsWith('.')).sort()});
  }
  if (path === '/wait') {
    await wait(Number(query.ms));
    return json(200, {waited: Number(query.ms)});
  }
  if (path === '/throw') throw new Error('fixture failure');
  if (path === '/bad') return {status: 'nope'};
  if (path === '/empty') return {status: 204};
  if (path === '/exit' && method === 'POST') {
    setTimeout(() => Daemon.exit(Number(query.code ?? 0)), 0);
    return json(200, {});
  }
  return json(404, {error: {message: `no route ${method} ${path}`}});
});
