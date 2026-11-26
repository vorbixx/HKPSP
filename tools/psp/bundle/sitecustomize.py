"""HKPSP builder runtime: temporary folders keep the normal inherited permissions on Windows.

The bundled Python 3.12 gives tempfile.mkdtemp() folders an owner-only, non-inherited ACL
(mode 0o700). Converters build their output in such folders and move them into _private/,
so a player account that is not an administrator later loses read access to its own build
("Access to the path ... is denied", laptop 2026-11-27). make_builder_bundle.py copies this
file into runtime/python/Lib/site-packages, where Python imports it even with -I.
"""
import os
import tempfile

if os.name == 'nt':
    class _InheritingOs:
        """tempfile's view of os, with mkdir ignoring the owner-only mode."""
        def __getattr__(self, name):
            return getattr(os, name)

        @staticmethod
        def mkdir(path, mode=0o777, *, dir_fd=None):
            if dir_fd is None:
                os.mkdir(path)
            else:
                os.mkdir(path, dir_fd=dir_fd)

    tempfile._os = _InheritingOs()
