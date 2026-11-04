# SPDX-License-Identifier: GPL-3.0+
# Copyright (c) 2026 SUSE LLC and contributors
"""Look up target and channel implementations by name."""

import importlib
from typing import Any


class PluginError(Exception):
    """A plugin can't be found or loaded."""


def load(kind: str, name: str, builtin: dict[str, str]) -> Any:
    """Return the class for name.

    name is either a key of builtin or 'package.module:Class', to load an
    implementation from outside this package.
    """
    path = builtin.get(name, name)
    module_name, sep, class_name = path.partition(':')
    if not sep or not module_name or not class_name:
        known = ', '.join(sorted(builtin))
        raise PluginError(
            f"unknown {kind} '{name}', use one of {known} or 'module:Class'"
        )
    try:
        module = importlib.import_module(module_name)
    except ImportError as e:
        raise PluginError(f"can't load {kind} '{name}': {e}") from e
    try:
        return getattr(module, class_name)
    except AttributeError as e:
        raise PluginError(
            f"{kind} '{name}': {module_name} has no {class_name}"
        ) from e
