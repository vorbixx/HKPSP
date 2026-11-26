"""Recognize private converter output, including the assigned converted junction."""
from pathlib import Path

from assetripper_client import ROOT


def is_private_output(path, root=ROOT):
    target = Path(path).resolve()
    # Converted assets may live on another drive. Approve that exact target,
    # rather than allowing every junction beneath _private to escape its root.
    return any(target.is_relative_to(base.resolve()) for base in
               (root / '_private', root / '_private/converted'))


def is_private_source(path, root=ROOT):
    target = Path(path).resolve()
    # Research is shared from the main checkout. Admit it as input without
    # admitting it as an output location.
    return any(target.is_relative_to(base.resolve()) for base in
               (root / '_private', root / '_private/research'))
