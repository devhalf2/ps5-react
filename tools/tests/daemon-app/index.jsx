// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// tools/test_daemon.py: starts the daemon from the app, twice (the second finds it serving), reports on
// stdout and closes; the test then talks to the daemon the app left running.
import {useEffect} from 'react';
import {AppRegistry, BackHandler, Daemon, View} from '@ps5-react/core';

export const TOKEN = '0123456789abcdef0123456789abcdef';

async function run() {
  const report = (name, value) => console.log(`daemon-test ${name} ${JSON.stringify(value)}`);
  report('app', {supported: Daemon.supported, isDaemon: Daemon.isDaemon, args: Daemon.args});
  report('random', [Daemon.randomToken(), Daemon.randomToken()]);
  try { Daemon.serve(() => ({status: 200})); report('serve', 'accepted'); }
  catch (error) { report('serve', error.message); }
  report('invalid', await Daemon.start({token: 'two\nlines', stateDir: '/download0/daemon-state'}));
  report('start', await Daemon.start({token: TOKEN, stateDir: '/download0/daemon-state'}));
  report('again', await Daemon.start({token: TOKEN, stateDir: '/download0/daemon-state'}));
  BackHandler.exitApp();
}

function App() {
  useEffect(() => { run().catch(error => { console.log(`daemon-test failed ${error.stack}`); BackHandler.exitApp(); }); }, []);
  return <View style={{width: 1280, height: 720, backgroundColor: '#101820'}} />;
}

AppRegistry.registerComponent('daemon-app', () => App);
