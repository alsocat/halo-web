"""Ninja rules for the browser build (``ninja web``).

It compiles the game sources with Emscripten for 32-bit WebAssembly, adds
the platform layer of the Linux build (``port/linux/src``) with the pieces
the browser does not have replaced from ``port/web/src``, and links
``build/web/site``: the page, its scripts and ``halo.wasm``. See
port/web/README.md for the design.

The browser build is a sibling of the Android one: 32-bit code that is not
x86, with OpenGL ES 3 (WebGL 2) for graphics and SDL3 for input and sound.
"""

import os
import shutil
from pathlib import Path
from typing import Any, List

from .embed_assets import hud_assets_build
from .linux_build import (KCP_DIR, PORT_CONFIG, TOML_DIR, XDK_INCLUDE, _load_port_config, _quote,
                          compile_launcher, game_defines_and_includes, game_sources, musl_math_cflags,
                          musl_math_sources, xdk_headers)
from .ninja_syntax import Writer

LINUX_DIR = Path("port/linux")
PORT_DIR = Path("port/web")

# The ABI the game was written against (tools/linux_build.py LINUX_ABI_FLAGS),
# as far as WebAssembly has it: wasm32 already aligns 64-bit members to 8
# bytes as MSVC does, and returns structures through memory.
WEB_ABI_FLAGS = [
    "-DHALO_WEB=1",
    "-pthread",
    "-fms-extensions",
    "-fshort-wchar",
    "-fcommon",
    "-fno-strict-aliasing",
    "-fwrapv",
    "-fno-delete-null-pointer-checks",
    "-ffp-contract=off",
    "-O2",
    *(f"-fno-builtin-{name}" for name in (
        "wcslen", "wcsnlen", "wcschr", "wcsrchr", "wcscmp", "wcsncmp", "wcscpy",
        "wcsncpy", "wcscat", "wcsncat", "wmemchr", "wmemcmp", "wmemcpy",
        "wmemmove", "wmemset",
    )),
]

GAME_FLAGS = [
    "-std=gnu89",
    "-D__STRICT_ANSI__",
    "-w",
    "-Wno-error=incompatible-pointer-types",
    "-Wno-error=incompatible-function-pointer-types",
    "-Wno-error=int-conversion",
    "-Wno-error=implicit-function-declaration",
    "-Wno-error=implicit-int",
    "-Wno-error=return-type",
]

PLATFORM_FLAGS = [
    "-std=gnu11",
    "-D_GNU_SOURCE",
    "-DHALO_LINUX_PLATFORM_LAYER",
    "-w",
]

# posix_*.c talk to the C library only, with its own ABI (32-bit wchar_t)
POSIX_FLAGS = ["-DHALO_WEB=1", "-pthread", "-std=gnu11", "-D_GNU_SOURCE", "-O2", "-Wall"]

# the Linux platform units the browser leaves out: the self-updater, UPnP,
# page-fault write tracking, sockets and disc images
# (port/web/src has the browser's versions of the ones it needs)
LEFT_OUT = {
    "posix_update.c", "posix_upnp.c", "updater.c", "memory_watch.c",
    "posix_net.c", "xiso.c",
}

LINK_FLAGS = [
    "-pthread",
    "--use-port=sdl3",
    "-sMIN_WEBGL_VERSION=2",
    "-sMAX_WEBGL_VERSION=2",
    "-sFULL_ES3",
    # the Xbox's memory window at 0x80000000 (port/linux/src/platform.h)
    # lies past 2 GB
    "-sALLOW_MEMORY_GROWTH",
    "-sMAXIMUM_MEMORY=4GB",
    "-sINITIAL_MEMORY=256MB",
    "-sSTACK_SIZE=4MB",
    "-sDEFAULT_PTHREAD_STACK_SIZE=1MB",
    # the game's loop blocks: it runs on a thread of its own, with the
    # canvas handed to it
    "-sPROXY_TO_PTHREAD",
    "-sOFFSCREENCANVAS_SUPPORT",
    "-sOFFSCREENCANVASES_TO_PTHREAD=#canvas",
    "-sPTHREAD_POOL_SIZE=16",
    "-sPTHREAD_POOL_SIZE_STRICT=0",
    "-sWASMFS",
    "-sFORCE_FILESYSTEM",
    "-sEXIT_RUNTIME=0",
    "-sENVIRONMENT=web,worker",
    # SDL_GL_GetProcAddress (port/linux/src/gl_functions.c)
    "-sGL_ENABLE_GET_PROC_ADDRESS",
    # what the page uses (port/web/shell/page.js)
    "-sEXPORTED_FUNCTIONS=_main,_malloc,_free",
    "-sEXPORTED_RUNTIME_METHODS=HEAP32,HEAPU8,UTF8ToString",
]


def web_configure_inputs() -> List[Path]:
    return [Path(__file__), PORT_DIR / "src", PORT_DIR / "shell"]


def generate_web_build(n: Writer, sln: Any) -> None:
    if not PORT_CONFIG.is_file() or not PORT_DIR.is_dir():
        return
    emsdk = Path(os.environ.get("EMSDK", Path.home() / "emsdk"))
    emcc = shutil.which("emcc") or str(emsdk / "upstream" / "emscripten" / "emcc")
    if not Path(emcc).exists():
        return
    config = _load_port_config()
    build_dir: Path = sln.build_dir / "web"
    obj_dir = build_dir / "obj"
    site = build_dir / "site"
    prefix_header = LINUX_DIR / "include" / "halo_linux_prefix.h"
    semantics_header = sln.build_dir / "linux" / "halo_msvc_semantics.h"
    platform_semantics_header = sln.build_dir / "linux" / "platform_msvc_semantics.h"
    release = getattr(sln, "port_release", False)

    n.comment("Browser build (ninja web)")
    n.variable("web_cc", emcc)
    n.variable("web_cxx", str(Path(emcc).with_name("em++")))
    n.rule(
        name="web_cc",
        command=f"{compile_launcher(sln)}$web_cc -MMD -MF $out.d $cflags -c $in -o $out",
        description="WEB CC $out",
        depfile="$out.d",
        deps="gcc",
    )
    n.rule(
        name="web_cxx",
        command=f"{compile_launcher(sln)}$web_cxx -MMD -MF $out.d $cflags -c $in -o $out",
        description="WEB CXX $out",
        depfile="$out.d",
        deps="gcc",
    )
    n.rule(
        name="web_link",
        command="$web_cxx $ldflags -o $out @$out.rsp",
        description="WEB LINK $out",
        rspfile="$out.rsp",
        rspfile_content="$in_newline",
    )
    n.rule(name="web_copy", command="cp $in $out", description="WEB COPY $out")

    abi = " ".join(WEB_ABI_FLAGS + (["-DHALO_RELEASE"] if release else []))
    port_include = LINUX_DIR / "include"
    sdk_flags = f"-idirafter {XDK_INCLUDE}"
    objects: List[Path] = []
    implicit = [*xdk_headers(), prefix_header, semantics_header, platform_semantics_header]

    def add_object(source: Path, cflags: str) -> None:
        obj = obj_dir / source.with_suffix(".o")
        objects.append(obj)
        rule = "web_cxx" if source.suffix == ".cpp" else "web_cc"
        n.build(outputs=obj, rule=rule, inputs=source, implicit=implicit, variables={"cflags": cflags})

    game_cflags = " ".join([
        abi, " ".join(GAME_FLAGS), f"-include {prefix_header}", f"-include {semantics_header}",
        f"-I{port_include}", game_defines_and_includes(config), sdk_flags,
    ])
    for source in game_sources(config):
        add_object(source, game_cflags)
    for source in sorted(Path(config["game_sources"]).glob("*.c")):
        add_object(source, game_cflags)

    platform_dir = Path(config["platform_sources"])
    platform_cflags = " ".join([
        abi, " ".join(PLATFORM_FLAGS), f"-include {prefix_header}", f"-include {platform_semantics_header}",
        f"-I{PORT_DIR / 'src'}", f"-I{platform_dir}", f"-I{port_include}", f"-I{TOML_DIR}", f"-I{KCP_DIR}",
        "-Isource -Isource/cseries", "--use-port=sdl3", sdk_flags,
    ])
    posix_cflags = " ".join(POSIX_FLAGS + [f"-I{PORT_DIR / 'src'}", f"-I{platform_dir}"])
    for source in sorted(platform_dir.glob("*.c")):
        if source.name in LEFT_OUT:
            continue
        add_object(source, posix_cflags if source.name.startswith("posix_") else platform_cflags)
    for source in sorted((PORT_DIR / "src").glob("*.c")):
        add_object(source, posix_cflags if source.name.startswith("posix_") else platform_cflags)
    # the maps' WasmFS backend, on WasmFS's own classes
    wasmfs_dir = Path(emcc).parent / "system" / "lib" / "wasmfs"
    for source in sorted((PORT_DIR / "src").glob("*.cpp")):
        add_object(source, " ".join(["-DHALO_WEB=1", "-pthread", "-std=c++17", "-O2", "-Wall",
                                     f"-I{_quote(wasmfs_dir)}"]))
    for source in hud_assets_build(n, "web", build_dir / "generated" / "hud_hires_assets.c"):
        add_object(source, platform_cflags)
    add_object(TOML_DIR / "tomlc17.c", " ".join([abi, "-std=gnu11", "-w"]))
    add_object(KCP_DIR / "ikcp.c", " ".join([abi, "-std=gnu11", "-w"]))
    for source in musl_math_sources():
        add_object(source, musl_math_cflags(abi))

    shell_dir = PORT_DIR / "shell"
    pre_js = shell_dir / "pre.js"
    ldflags = [*LINK_FLAGS, "-O2" if release else "-O1", "-g" if not release else "-g0"]
    if pre_js.is_file():
        ldflags.append(f"--pre-js {_quote(pre_js)}")
    output = site / "halo.js"
    n.build(outputs=[output], rule="web_link", inputs=objects,
            implicit=[pre_js] if pre_js.is_file() else [],
            variables={"ldflags": " ".join(ldflags)})
    site_files = [output]
    for item in sorted(shell_dir.glob("*")):
        if item.name == "pre.js" or not item.is_file():
            continue
        target = site / item.name
        n.build(outputs=target, rule="web_copy", inputs=item)
        site_files.append(target)
    n.build(outputs="web", rule="phony", inputs=site_files)
    n.newline()
