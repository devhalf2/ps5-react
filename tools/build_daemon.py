# Copyright (C) 2026 half144 and PS5 React contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional attribution term: see LICENSE-ATTRIBUTION.
"""pandorad.elf: the app's daemon entry (app.json "daemon") in the headless host (native/daemon), built as
a payload for the loader on 9021, like rar-extract.elf. It links the payload SDK's libc and the PacBrew
ports directly, which target payloads, so none of the title's compatibility shims are needed."""
from concurrent.futures import ThreadPoolExecutor
import subprocess

from common import ROOT


def sources(hui, generated):
    native = ROOT / "native"
    return [*(native / "daemon" / f"{name}.cpp" for name in ("main", "js_runtime", "http_server", "headless", "archive_hooks", "ps5")),
            *(native / "ps5" / f"{name}.cpp" for name in ("host_platform", "async_log", "payload_loader", "package_installer", "system_platform")),
            *(native / "shared" / f"{name}.cpp" for name in ("host_api", "removals", "network", "network_api", "archives",
                                                             "archive_preflight", "rar5_password", "archive_writer",
                                                             "archive_api", "package_api", "system_state",
                                                             "notifications_api")),
            hui / "src/platform/ps5/system.cpp", generated / "daemon_bundle.c"]


def build(output, sdk, env, hui, er, quickjs, generated, qjs_library, port_includes, libraries):
    """Compiles and links `output`; `libraries` are the network and archive ports, then compiler builtins."""
    objects = output.parent / (output.name + ".obj")
    objects.mkdir(parents=True, exist_ok=True)
    includes = [ROOT / "native/shared", ROOT / "native/ps5", ROOT / "native/daemon", generated, hui / "src",
                er / "bridges/quickjs", quickjs, *port_includes]
    flags = ["-O2", "-DPROSPERO=1", "-Wall", "-Wextra", "-Werror", *[f for path in includes for f in ("-I", str(path))]]

    def compile_one(item):
        index, source = item
        obj = objects / f"{index}-{source.stem}.o"
        cpp = source.suffix == ".cpp"
        command = [str(sdk / "bin" / ("prospero-clang++" if cpp else "prospero-clang")),
                   *(["-std=c++20", "-fno-exceptions", "-fno-rtti"] if cpp else ["-std=c11"]), *flags,
                   "-c", str(source), "-o", str(obj)]
        result = subprocess.run(command, capture_output=True, text=True, env=env)
        if result.returncode: raise RuntimeError(f"pandorad {source.name}: {result.stderr[-4000:]}")
        return obj

    with ThreadPoolExecutor(8) as pool:
        built = list(pool.map(compile_one, enumerate(sources(hui, generated))))
    command = [str(sdk / "bin/prospero-clang++"), *map(str, built), str(qjs_library),
               "-Wl,--start-group", *map(str, libraries), "-Wl,--end-group",
               "-lSceUserService", "-lSceSystemService", "-lkernel_sys", "-o", str(output)]
    result = subprocess.run(command, capture_output=True, text=True, env=env)
    if result.returncode: raise RuntimeError(f"pandorad link: {result.stderr[-4000:]}")
    return output
