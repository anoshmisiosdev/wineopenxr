#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later

"""Wine OpenXR thunk generator for macOS.

Parses xr.xml and emits four files.

  src/include/loader_thunks.h   enum unix_call, per-command PARAMS structs
  src/include/openxr_thunks.h   struct openxr_instance_funcs, ALL_XR_INSTANCE_FUNCS
  src/pe/loader_thunks.c        PE thunks, xr_instance_dispatch_table, proc-addr lookup
  src/unix/openxr_thunks.c      Unix thunks, __wine_unix_call_funcs[]
"""

import copy
import os
import sys
import xml.etree.ElementTree as ET
from collections import OrderedDict

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.dirname(SCRIPT_DIR)

XR_XML = os.path.join(PROJECT_DIR, "extern", "OpenXR-SDK",
                      "specification", "registry", "xr.xml")

LOADER_THUNKS_H = os.path.join(PROJECT_DIR, "src", "include", "loader_thunks.h")
OPENXR_THUNKS_H = os.path.join(PROJECT_DIR, "src", "include", "openxr_thunks.h")
LOADER_THUNKS_C = os.path.join(PROJECT_DIR, "src", "pe", "loader_thunks.c")
OPENXR_THUNKS_C = os.path.join(PROJECT_DIR, "src", "unix", "openxr_thunks.c")


# Handle types that live in a PE-side wrapper. Tuple is (accessor function,
# host handle field). Unwrapped handles pass through as native OpenXR handles,
# no registry
WRAPPED_HANDLES = {
    "XrInstance":  ("wine_instance_from_handle",  "host_instance"),
    "XrSession":   ("wine_session_from_handle",   "host_session"),
    "XrSwapchain": ("wine_swapchain_from_handle", "host_swapchain"),
}

# Parameter-type sets that trigger a platform guard around a PARAMS struct and
# the auto-generated PE thunk
D3D11_TYPES = {
    "XrGraphicsRequirementsD3D11KHR",
    "XrGraphicsBindingD3D11KHR",
}
METAL_TYPES = {
    "XrGraphicsBindingMetalKHR",
    "XrSwapchainImageMetalKHR",
    "XrGraphicsRequirementsMetalKHR",
}
WIN32_TYPES = {
    "LARGE_INTEGER",
}

# Platform-guard macros the generator is allowed to emit. Anything else is a
# policy bug
ALLOWED_PROTECTS = {
    "XR_USE_GRAPHICS_API_D3D11",
    "XR_USE_GRAPHICS_API_METAL",
    "XR_USE_PLATFORM_WIN32",
}

# Per-command overrides on the default dispatch and export policy. Default is
# dispatch=True, no_pe_export=False, no_unix_export=False.
#
#   dispatch         emit a slot in openxr_instance_funcs and a USE_XR_FUNC
#                    entry in ALL_XR_INSTANCE_FUNCS. Skip only for functions
#                    the Unix side does not invoke through the table.
#   no_pe_export     skip PE-side entirely, so no PE thunk, no PARAMS struct,
#                    no enum unix_call slot, no xr_instance_dispatch_table
#                    entry. Intended for Metal's xrGetMetalGraphicsRequirements
#                    which the Unix side calls via the dispatch table but
#                    which the app never names (the app sees the D3D11
#                    equivalent through extension substitution)
#   no_unix_export   skip Unix-side entirely, so no PARAMS struct, no enum
#                    unix_call slot, no openxr_instance_funcs slot, no
#                    __wine_unix_call_funcs entry. Implies the PE thunk is
#                    hand-written in MANUAL_LOADER_THUNKS and does not route
#                    through UNIX_CALL. Intended for xrGetInstanceProcAddr,
#                    which resolves locally against xr_instance_dispatch_table
FUNCTION_OVERRIDES = {
    "xrCreateInstance":                           {"dispatch": False},
    "xrGetInstanceProcAddr":                      {"dispatch": False, "no_unix_export": True},
    "xrEnumerateInstanceExtensionProperties":     {"dispatch": False},

    "xrGetD3D11GraphicsRequirementsKHR":          {"dispatch": False},
    "xrConvertTimeToWin32PerformanceCounterKHR":  {"dispatch": False},
    "xrConvertWin32PerformanceCounterToTimeKHR":  {"dispatch": False},

    "xrGetMetalGraphicsRequirementsKHR": {"dispatch": True, "no_pe_export": True},
}

# Unix-side thunks hand-written in src/unix/*. Generator skips body emission
# but still assigns a slot in enum unix_call and __wine_unix_call_funcs
MANUAL_UNIX_THUNKS = {
    "xrCreateInstance",
    "xrDestroyInstance",
    "xrEnumerateInstanceExtensionProperties",
    "xrEnumerateSwapchainFormats",
    "xrCreateSession",
    "xrCreateSwapchain",
    "xrEndFrame",
    "xrGetD3D11GraphicsRequirementsKHR",
    "xrConvertTimeToWin32PerformanceCounterKHR",
    "xrConvertWin32PerformanceCounterToTimeKHR",
}

# PE-side thunks hand-written in src/pe/openxr_loader.c. Generator skips thunk
# body emission but still adds an xr_instance_dispatch_table entry (unless also
# in SKIP_FUNCTIONS) so the loader can resolve via xrGetInstanceProcAddr
MANUAL_LOADER_THUNKS = {
    "xrCreateInstance",
    "xrDestroyInstance",
    "xrCreateSession",
    "xrDestroySession",
    "xrCreateSwapchain",
    "xrDestroySwapchain",
    "xrEnumerateSwapchainImages",
    "xrReleaseSwapchainImage",
    "xrEndFrame",
    "xrPollEvent",
    "xrGetInstanceProcAddr",
    "xrGetD3D11GraphicsRequirementsKHR",
    "xrConvertTimeToWin32PerformanceCounterKHR",
    "xrConvertWin32PerformanceCounterToTimeKHR",
    "xrNegotiateLoaderRuntimeInterface",
}

# PARAMS structs maintained by hand in src/include/openxr_loader.h. Generator
# skips their emission in loader_thunks.h so the hand-written definition is
# authoritative
HAND_MAINTAINED_PARAMS = {
    "xrEndFrame",
}

# Bridge-specific unix calls not backed by xr.xml. PARAMS structs for these
# live in openxr_loader.h and the Unix handlers are hand-written
CUSTOM_UNIX_CALLS = [
    "create_d3d11_session",
    "release_metal_session",
    "export_metal_textures",
]

# Commands excluded from every thunk path. Loader-level negotiation and
# api-layer stubs. Implemented in PE where needed and never dispatched via
# unix_call
SKIP_FUNCTIONS = {
    "xrNegotiateLoaderRuntimeInterface",
    "xrNegotiateLoaderApiLayerInterface",
    "xrCreateApiLayerInstance",
    "xrEnumerateApiLayerProperties",
}

# Extensions whose commands are eligible for thunk generation
THUNKED_EXTENSIONS = {
    "XR_KHR_D3D11_enable",
    "XR_KHR_metal_enable",
    "XR_KHR_win32_convert_performance_counter_time",
    "XR_KHR_convert_timespec_time",
    "XR_KHR_visibility_mask",
    "XR_KHR_composition_layer_depth",
    "XR_KHR_locate_spaces",
    "XR_KHR_extended_struct_name_lengths",
    "XR_EXT_hand_tracking",
    "XR_FB_display_refresh_rate",
}


def platform_guard(cmd):
    for param in cmd.params:
        if param.type_name in D3D11_TYPES:
            return "XR_USE_GRAPHICS_API_D3D11"
        if param.type_name in METAL_TYPES:
            return "XR_USE_GRAPHICS_API_METAL"
        if param.type_name in WIN32_TYPES:
            return "XR_USE_PLATFORM_WIN32"
    return None


class XrParam:
    def __init__(self, elem):
        type_elem = elem.find("type")
        self.type_name = type_elem.text if type_elem is not None else ""
        name_elem = elem.find("name")
        self.param_name = name_elem.text if name_elem is not None else ""
        self.full_decl = self._reconstruct_decl(elem)
        self.is_pointer = "*" in self.full_decl
        self.is_const = "const " in self.full_decl

        self.array_suffix = ""
        if name_elem is not None:
            parts = []
            after_name = False
            for child in elem:
                if child is name_elem:
                    after_name = True
                    if child.tail:
                        parts.append(child.tail.strip())
                    continue
                if after_name:
                    if child.text:
                        parts.append(child.text.strip())
                    if child.tail:
                        parts.append(child.tail.strip())
            joined = "".join(parts)
            if "[" in joined:
                self.array_suffix = joined

    @staticmethod
    def _reconstruct_decl(elem):
        parts = []
        if elem.text:
            parts.append(elem.text.strip())
        for child in elem:
            if child.text:
                parts.append(child.text.strip())
            if child.tail:
                parts.append(child.tail.strip())
        return " ".join(p for p in parts if p)

    @property
    def is_wrapped_handle(self):
        return self.type_name in WRAPPED_HANDLES and not self.is_pointer

    def c_type(self):
        # Arrays decay to pointers because Wine's unixlib convention indirects
        # variable-length payloads through the PARAMS struct
        decl = self.full_decl
        idx = decl.rfind(self.param_name)
        t = decl[:idx].strip() if idx >= 0 else decl
        if self.array_suffix:
            t = t.replace(self.array_suffix, "").strip() + " *"
        return t

    def struct_field(self):
        return f"    {self.c_type()} {self.param_name};"

    def unix_call_arg(self):
        # Wrapped handles chase their accessor, unwrapped handles forward
        # verbatim from the PARAMS struct
        if self.is_wrapped_handle:
            func, field = WRAPPED_HANDLES[self.type_name]
            return f"{func}(params->{self.param_name})->{field}"
        return f"params->{self.param_name}"


class XrCommand:
    def __init__(self, elem):
        proto = elem.find("proto")
        self.return_type = proto.find("type").text
        self.name = proto.find("name").text
        self.params = [XrParam(p) for p in elem.findall("param")]
        self.alias_target = None

    @classmethod
    def alias_of(cls, alias_name, target):
        # Registry aliases share a signature with their target but resolve to a
        # distinct host dispatch slot. Monado gates xrLocateSpaces on OpenXR 1.1
        # while keeping xrLocateSpacesKHR available on OpenXR 1.0 when
        # XR_KHR_locate_spaces is enabled, see
        # src/monado/src/xrt/state_trackers/oxr/oxr_api_negotiate.c:446-450.
        # Clone the target and rename it so codegen emits an independent PE
        # thunk, PARAMS struct, Unix thunk, and dispatch slot per name
        inst = copy.copy(target)
        inst.name = alias_name
        inst.alias_target = target.name
        return inst

    @property
    def needs_dispatch(self):
        return FUNCTION_OVERRIDES.get(self.name, {}).get("dispatch", True)

    @property
    def no_pe_export(self):
        return FUNCTION_OVERRIDES.get(self.name, {}).get("no_pe_export", False)

    @property
    def no_unix_export(self):
        return FUNCTION_OVERRIDES.get(self.name, {}).get("no_unix_export", False)


def parse_commands(xml_path):
    root = ET.parse(xml_path).getroot()
    commands = OrderedDict()
    deferred_aliases = []
    for commands_elem in root.findall("commands"):
        for cmd_elem in commands_elem.findall("command"):
            alias_target = cmd_elem.get("alias")
            if alias_target:
                alias_name = cmd_elem.get("name")
                if alias_name:
                    deferred_aliases.append((alias_name, alias_target))
                continue
            proto = cmd_elem.find("proto")
            if proto is None or proto.find("name") is None:
                continue
            cmd = XrCommand(cmd_elem)
            commands[cmd.name] = cmd
    for alias_name, target_name in deferred_aliases:
        target = commands.get(target_name)
        if target is None:
            continue
        commands[alias_name] = XrCommand.alias_of(alias_name, target)
    return commands


def get_feature_commands(xml_path, feature_names):
    root = ET.parse(xml_path).getroot()
    result = set()
    for feature in root.findall("feature"):
        if feature.get("api", "") != "openxr":
            continue
        if feature.get("name", "") not in feature_names:
            continue
        for req in feature.findall("require"):
            for cmd in req.findall("command"):
                result.add(cmd.get("name"))
    return result


def get_extension_commands(xml_path, ext_names):
    root = ET.parse(xml_path).getroot()
    result = set()
    for ext in root.findall(".//extension"):
        if ext.get("name") in ext_names:
            for req in ext.findall("require"):
                for cmd in req.findall("command"):
                    result.add(cmd.get("name"))
    return result


def filter_thunked(all_commands, allowed_names):
    result = OrderedDict()
    for name in sorted(allowed_names):
        if name in SKIP_FUNCTIONS:
            continue
        if name in all_commands:
            result[name] = all_commands[name]
    return result


def validate_policy(commands):
    for name, overrides in FUNCTION_OVERRIDES.items():
        if name not in commands and name not in SKIP_FUNCTIONS:
            raise RuntimeError(
                f"FUNCTION_OVERRIDES references unknown command {name!r}")
        for key in overrides:
            if key not in ("dispatch", "no_pe_export", "no_unix_export"):
                raise RuntimeError(
                    f"FUNCTION_OVERRIDES[{name!r}] has unknown key {key!r}")

    for name in MANUAL_UNIX_THUNKS:
        if name not in commands:
            raise RuntimeError(
                f"MANUAL_UNIX_THUNKS references unknown command {name!r}")
    for name in MANUAL_LOADER_THUNKS:
        if name in SKIP_FUNCTIONS:
            continue
        if name not in commands:
            raise RuntimeError(
                f"MANUAL_LOADER_THUNKS references unknown command {name!r}")
    for name in HAND_MAINTAINED_PARAMS:
        if name not in commands:
            raise RuntimeError(
                f"HAND_MAINTAINED_PARAMS references unknown command {name!r}")

    for name, cmd in commands.items():
        if cmd.alias_target:
            # Aliases emit as independent full-codegen entries that share a
            # signature with their target. If the target ever moves into a
            # special-policy table, the auto-generated alias thunks would
            # collide with the hand-written path. Fail fast so someone adds
            # explicit policy instead of shipping a silently wrong alias
            target = cmd.alias_target
            if (target in MANUAL_LOADER_THUNKS
                    or target in MANUAL_UNIX_THUNKS
                    or target in HAND_MAINTAINED_PARAMS
                    or target in SKIP_FUNCTIONS
                    or target in FUNCTION_OVERRIDES):
                raise RuntimeError(
                    f"alias {name!r} targets {target!r} which needs special "
                    f"handling (present in a special-policy table); add "
                    f"explicit policy for this alias")
        if cmd.no_pe_export and name in MANUAL_LOADER_THUNKS:
            raise RuntimeError(
                f"{name!r} is no_pe_export but also in MANUAL_LOADER_THUNKS")
        if cmd.no_unix_export:
            if cmd.no_pe_export:
                raise RuntimeError(
                    f"{name!r} cannot be both no_pe_export and no_unix_export")
            if name not in MANUAL_LOADER_THUNKS:
                raise RuntimeError(
                    f"{name!r} is no_unix_export but not in MANUAL_LOADER_THUNKS")
            if name in MANUAL_UNIX_THUNKS:
                raise RuntimeError(
                    f"{name!r} is no_unix_export but also in MANUAL_UNIX_THUNKS")
        guard = platform_guard(cmd)
        if guard is not None and guard not in ALLOWED_PROTECTS:
            raise RuntimeError(
                f"{name!r} needs unexpected platform guard {guard!r}")


def _group_by_guard(commands):
    groups = {None: []}
    for g in ALLOWED_PROTECTS:
        groups[g] = []
    for name in sorted(commands):
        cmd = commands[name]
        groups.setdefault(platform_guard(cmd), []).append((name, cmd))
    return groups


def emit_loader_thunks_h(commands):
    out = []
    out.append("/* Generated by make_openxr.py. Do not edit manually */")
    out.append("#ifndef __WINE_OPENXR_LOADER_THUNKS_H")
    out.append("#define __WINE_OPENXR_LOADER_THUNKS_H")
    out.append("")
    out.append('#include "openxr/openxr.h"')
    out.append("#include <stdint.h>")
    out.append("#include <time.h>")
    out.append("")
    out.append("/* Portable NTSTATUS, skip if windows.h or wine already defined it */")
    out.append("#if !defined(_WINNT_) && !defined(__WINE_WINTERNL_H)")
    out.append("typedef int32_t NTSTATUS;")
    out.append("#endif")
    out.append("")

    out.append("enum unix_call")
    out.append("{")
    out.append("    unix_init,")
    out.append("    unix_is_available_instance_function,")
    for name in CUSTOM_UNIX_CALLS:
        out.append(f"    unix_{name},")
    for name in sorted(commands):
        cmd = commands[name]
        if cmd.no_pe_export or cmd.no_unix_export:
            continue
        out.append(f"    unix_{name},")
    out.append("    unix_count,")
    out.append("};")
    out.append("")

    def emit_params(name, cmd):
        out.append(f"struct {name}_params")
        out.append("{")
        for p in cmd.params:
            out.append(p.struct_field())
        if cmd.return_type == "XrResult":
            out.append("    XrResult result;")
        out.append("};")
        out.append("")

    for name in sorted(commands):
        if name in HAND_MAINTAINED_PARAMS:
            continue
        cmd = commands[name]
        if cmd.no_pe_export or cmd.no_unix_export or platform_guard(cmd):
            continue
        emit_params(name, cmd)

    # Guarded PARAMS blocks. Only D3D11 and WIN32 reach PE. Metal is
    # no_pe_export so there are no Metal-guarded PARAMS
    for guard in ("XR_USE_GRAPHICS_API_D3D11", "XR_USE_PLATFORM_WIN32"):
        guarded = [(n, commands[n]) for n in sorted(commands)
                   if n not in HAND_MAINTAINED_PARAMS
                   and not commands[n].no_pe_export
                   and not commands[n].no_unix_export
                   and platform_guard(commands[n]) == guard]
        if not guarded:
            continue
        out.append(f"#ifdef {guard}")
        out.append('#include "openxr/openxr_platform.h"')
        out.append("")
        for name, cmd in guarded:
            emit_params(name, cmd)
        out.append(f"#endif /* {guard} */")
        out.append("")

    out.append("#endif /* __WINE_OPENXR_LOADER_THUNKS_H */")
    return "\n".join(out) + "\n"


def emit_openxr_thunks_h(commands):
    out = []
    out.append("/* Generated by make_openxr.py. Do not edit manually */")
    out.append("#ifndef __WINE_OPENXR_THUNKS_H")
    out.append("#define __WINE_OPENXR_THUNKS_H")
    out.append("")
    out.append("/* openxr_instance_funcs and ALL_XR_INSTANCE_FUNCS are Unix-only.")
    out.append(" * The PE side of the bridge does not dispatch through this table.")
    out.append(" * Include after openxr/openxr_platform.h so the platform-typed")
    out.append(" * slots have their parameter types defined */")
    out.append("")

    groups = _group_by_guard({n: c for n, c in commands.items()
                              if c.needs_dispatch})

    def slot_line(name, cmd):
        param_types = ", ".join(p.c_type() for p in cmd.params)
        return f"    {cmd.return_type} (*p_{name})({param_types});"

    out.append("struct openxr_instance_funcs")
    out.append("{")
    for name, cmd in groups[None]:
        out.append(slot_line(name, cmd))
    for guard in ("XR_USE_GRAPHICS_API_D3D11",
                  "XR_USE_GRAPHICS_API_METAL",
                  "XR_USE_PLATFORM_WIN32"):
        guarded = groups.get(guard, [])
        if not guarded:
            continue
        out.append(f"#ifdef {guard}")
        for name, cmd in guarded:
            out.append(slot_line(name, cmd))
        out.append(f"#endif /* {guard} */")
    out.append("};")
    out.append("")

    # ALL_XR_INSTANCE_FUNCS composed from per-guard sub-macros. The C
    # preprocessor forbids #ifdef inside a macro body, so we split
    def emit_submacro(macro_name, entries, guard=None):
        if not entries:
            # Empty expansion regardless of guard, nothing to conditionalize
            out.append(f"#define {macro_name}()")
            out.append("")
            return
        def populated_body():
            body = [f"#define {macro_name}() \\"]
            for i, (name, _) in enumerate(entries):
                suffix = " \\" if i < len(entries) - 1 else ""
                body.append(f"    USE_XR_FUNC({name}){suffix}")
            return body
        if guard is None:
            out.extend(populated_body())
        else:
            out.append(f"#ifdef {guard}")
            out.extend(populated_body())
            out.append("#else")
            out.append(f"#define {macro_name}()")
            out.append(f"#endif /* {guard} */")
        out.append("")

    emit_submacro("ALL_XR_INSTANCE_FUNCS_CORE",  groups[None])
    emit_submacro("ALL_XR_INSTANCE_FUNCS_D3D11", groups.get("XR_USE_GRAPHICS_API_D3D11", []),
                  guard="XR_USE_GRAPHICS_API_D3D11")
    emit_submacro("ALL_XR_INSTANCE_FUNCS_METAL", groups.get("XR_USE_GRAPHICS_API_METAL", []),
                  guard="XR_USE_GRAPHICS_API_METAL")
    emit_submacro("ALL_XR_INSTANCE_FUNCS_WIN32", groups.get("XR_USE_PLATFORM_WIN32", []),
                  guard="XR_USE_PLATFORM_WIN32")

    out.append("#define ALL_XR_INSTANCE_FUNCS() \\")
    out.append("    ALL_XR_INSTANCE_FUNCS_CORE() \\")
    out.append("    ALL_XR_INSTANCE_FUNCS_D3D11() \\")
    out.append("    ALL_XR_INSTANCE_FUNCS_METAL() \\")
    out.append("    ALL_XR_INSTANCE_FUNCS_WIN32()")
    out.append("")

    out.append("#endif /* __WINE_OPENXR_THUNKS_H */")
    return "\n".join(out) + "\n"


def emit_loader_thunks_c(commands, core_names):
    out = []
    out.append("/* Generated by make_openxr.py. Do not edit manually */")
    out.append("")
    out.append("#include <stddef.h>")
    out.append("#include <string.h>")
    out.append("#include <windows.h>")
    out.append("")
    out.append("/* Platform guards so platform-typed PARAMS structs and thunks compile */")
    out.append("#define XR_USE_GRAPHICS_API_D3D11")
    out.append("#define XR_USE_PLATFORM_WIN32")
    out.append("")
    out.append('#include "openxr_loader.h"')
    out.append('#include "loader_thunks.h"')
    out.append("")

    for name in sorted(commands):
        cmd = commands[name]
        if name in MANUAL_LOADER_THUNKS or cmd.no_pe_export:
            continue
        param_list = ", ".join(f"{p.c_type()} {p.param_name}" for p in cmd.params)
        out.append(f"static {cmd.return_type} WINAPI wine_{name}({param_list})")
        out.append("{")
        out.append(f"    struct {name}_params params;")
        out.append("    NTSTATUS status;")
        out.append("")
        for p in cmd.params:
            cast = f"({p.c_type()})" if p.is_const else ""
            out.append(f"    params.{p.param_name} = {cast}{p.param_name};")
        out.append("")
        out.append(f"    status = UNIX_CALL({name}, &params);")
        out.append("    if (status) return XR_ERROR_RUNTIME_FAILURE;")
        if cmd.return_type == "XrResult":
            out.append("    return params.result;")
        out.append("}")
        out.append("")

    # Forward-declare extension functions that are manual on the PE side.
    # openxr_platform.h only exposes KHR prototypes when XR_EXTENSION_PROTOTYPES
    # is set, which we never set. The dispatch table needs the symbol so the
    # cast to void * resolves
    ext_manual = sorted(name for name in MANUAL_LOADER_THUNKS
                        if name not in SKIP_FUNCTIONS
                        and name in commands
                        and name not in core_names)
    if ext_manual:
        for name in ext_manual:
            cmd = commands[name]
            param_list = ", ".join(f"{p.c_type()} {p.param_name}" for p in cmd.params)
            out.append(f"{cmd.return_type} WINAPI {name}({param_list});")
        out.append("")

    out.append("struct openxr_func")
    out.append("{")
    out.append("    const char *name;")
    out.append("    void *func;")
    out.append("};")
    out.append("")

    dispatch_funcs = []
    for name in sorted(commands):
        cmd = commands[name]
        if name in SKIP_FUNCTIONS or cmd.no_pe_export:
            continue
        if name in MANUAL_LOADER_THUNKS:
            dispatch_funcs.append((name, f"(void *){name}"))
        else:
            dispatch_funcs.append((name, f"wine_{name}"))

    out.append("static const struct openxr_func xr_instance_dispatch_table[] =")
    out.append("{")
    for func_name, impl_name in sorted(dispatch_funcs, key=lambda x: x[0]):
        out.append(f'    {{"{func_name}", {impl_name}}},')
    out.append("};")
    out.append("")

    out.append("void *wine_xr_get_instance_proc_addr(const char *name)")
    out.append("{")
    out.append("    unsigned int lo = 0, hi = sizeof(xr_instance_dispatch_table) / sizeof(xr_instance_dispatch_table[0]);")
    out.append("    while (lo < hi)")
    out.append("    {")
    out.append("        unsigned int mid = (lo + hi) / 2;")
    out.append("        int cmp = strcmp(xr_instance_dispatch_table[mid].name, name);")
    out.append("        if (cmp == 0)")
    out.append("            return xr_instance_dispatch_table[mid].func;")
    out.append("        if (cmp < 0)")
    out.append("            lo = mid + 1;")
    out.append("        else")
    out.append("            hi = mid;")
    out.append("    }")
    out.append("    return NULL;")
    out.append("}")
    out.append("")

    return "\n".join(out) + "\n"


def emit_openxr_thunks_c(commands):
    out = []
    out.append("/* Generated by make_openxr.py. Do not edit manually */")
    out.append("")
    out.append("#define XR_USE_GRAPHICS_API_METAL")
    out.append("#define XR_USE_GRAPHICS_API_D3D11")
    out.append("#define XR_USE_PLATFORM_WIN32")
    out.append("")
    out.append('#include "ntstatus.h"')
    out.append("#define WIN32_NO_STATUS")
    out.append('#include "windef.h"')
    out.append('#include "winnt.h"')
    out.append('#include "winternl.h"')
    out.append('#include "wine/unixlib.h"')
    out.append("")
    # openxr_loader.h first so its forward-declarations of opaque D3D11
    # types (ID3D11Device etc.) land before openxr_platform.h references
    # them under XR_USE_GRAPHICS_API_D3D11. loader_thunks.h conditionally
    # pulls openxr_platform.h via its platform-guarded PARAMS blocks, and
    # the explicit include below makes Metal types visible too
    out.append('#include "openxr_loader.h"')
    out.append('#include "openxr/openxr_platform.h"')
    out.append('#include "loader_thunks.h"')
    out.append('#include "openxr_thunks.h"')
    out.append("")
    out.append("extern struct openxr_instance_funcs g_xr_host_instance_dispatch_table;")
    out.append("")

    out.append("extern NTSTATUS wine_init(void *args);")
    out.append("extern NTSTATUS wine_is_available_instance_function(void *args);")
    for name in CUSTOM_UNIX_CALLS:
        out.append(f"extern NTSTATUS wine_{name}(void *args);")
    for name in sorted(MANUAL_UNIX_THUNKS):
        if name in commands:
            out.append(f"extern NTSTATUS wine_{name}(void *args);")
    out.append("")

    for name in sorted(commands):
        cmd = commands[name]
        if name in MANUAL_UNIX_THUNKS or cmd.no_pe_export or cmd.no_unix_export:
            continue
        if not cmd.params:
            continue

        out.append(f"static NTSTATUS thunk_{name}(void *args)")
        out.append("{")
        out.append(f"    struct {name}_params *params = args;")
        out.append("    struct openxr_instance_funcs *funcs = &g_xr_host_instance_dispatch_table;")
        if cmd.return_type == "XrResult":
            # An app that holds a handle past xrDestroyInstance hits a zeroed
            # table. UNSUPPORTED is the honest answer, not a NULL deref
            out.append(f"    if (!funcs->p_{name})")
            out.append("    {")
            out.append("        params->result = XR_ERROR_FUNCTION_UNSUPPORTED;")
            out.append("        return STATUS_SUCCESS;")
            out.append("    }")

        call_args = [p.unix_call_arg() for p in cmd.params]
        if cmd.return_type == "XrResult":
            out.append(f"    params->result = funcs->p_{name}(")
        else:
            out.append(f"    funcs->p_{name}(")
        for i, arg in enumerate(call_args):
            comma = "," if i < len(call_args) - 1 else ");"
            out.append(f"        {arg}{comma}")
        out.append("    return STATUS_SUCCESS;")
        out.append("}")
        out.append("")

    # Dispatch table order must match enum unix_call exactly
    out.append("const unixlib_entry_t __wine_unix_call_funcs[] =")
    out.append("{")
    out.append("    wine_init,")
    out.append("    wine_is_available_instance_function,")
    for name in CUSTOM_UNIX_CALLS:
        out.append(f"    wine_{name},")
    for name in sorted(commands):
        cmd = commands[name]
        if cmd.no_pe_export or cmd.no_unix_export:
            continue
        if name in MANUAL_UNIX_THUNKS:
            out.append(f"    wine_{name},")
        else:
            out.append(f"    thunk_{name},")
    out.append("};")
    out.append("")

    out.append("_Static_assert(sizeof(__wine_unix_call_funcs) / sizeof(__wine_unix_call_funcs[0]) == unix_count,")
    out.append('    "Unix call table size mismatch");')
    out.append("")

    return "\n".join(out) + "\n"


def write_if_changed(path, content):
    try:
        with open(path, "r") as f:
            if f.read() == content:
                return False
    except FileNotFoundError:
        pass
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(content)
    return True


def check_files(files):
    ok = True
    for path, content in files:
        rel = os.path.relpath(path, PROJECT_DIR)
        try:
            with open(path, "r") as f:
                existing = f.read()
            if existing == content:
                print(f"  {rel}: ok")
            else:
                print(f"  {rel}: DIFFERS", file=sys.stderr)
                ok = False
        except FileNotFoundError:
            print(f"  {rel}: MISSING", file=sys.stderr)
            ok = False
    return ok


def diff_files(files):
    import difflib
    for path, content in files:
        rel = os.path.relpath(path, PROJECT_DIR)
        try:
            with open(path, "r") as f:
                existing = f.read()
        except FileNotFoundError:
            existing = ""
        if existing == content:
            continue
        for line in difflib.unified_diff(existing.splitlines(keepends=True),
                                         content.splitlines(keepends=True),
                                         fromfile=f"a/{rel}",
                                         tofile=f"b/{rel}"):
            sys.stdout.write(line)


def main():
    check_mode = "--check" in sys.argv
    diff_mode = "--diff" in sys.argv

    if not os.path.exists(XR_XML):
        print(f"Error: {XR_XML} not found", file=sys.stderr)
        sys.exit(1)

    all_commands = parse_commands(XR_XML)
    core_names = get_feature_commands(XR_XML,
                                      ("XR_VERSION_1_0",
                                       "XR_VERSION_1_1",
                                       "XR_LOADER_VERSION_1_0"))
    ext_names = get_extension_commands(XR_XML, THUNKED_EXTENSIONS)

    commands = filter_thunked(all_commands, core_names | ext_names)

    validate_policy(commands)

    print(f"Parsed {len(all_commands)} commands, "
          f"{len(commands)} thunked "
          f"({len(core_names)} core + {len(ext_names)} extension)")

    files = [
        (LOADER_THUNKS_H, emit_loader_thunks_h(commands)),
        (OPENXR_THUNKS_H, emit_openxr_thunks_h(commands)),
        (LOADER_THUNKS_C, emit_loader_thunks_c(commands, core_names)),
        (OPENXR_THUNKS_C, emit_openxr_thunks_c(commands)),
    ]

    if check_mode:
        if check_files(files):
            print("All generated files are up to date.")
        else:
            print("Generated files are out of date. Run make_openxr.py to regenerate.",
                  file=sys.stderr)
            sys.exit(1)
        return

    if diff_mode:
        diff_files(files)
        return

    for path, content in files:
        rel = os.path.relpath(path, PROJECT_DIR)
        print(f"  {rel}: {'updated' if write_if_changed(path, content) else 'unchanged'}")


if __name__ == "__main__":
    main()
