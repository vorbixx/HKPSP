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
# Release check (tools/psp/check_release_tree.py companion): with HKPSP_EXEC_LOG set, every program
# a Python step starts is appended to that file with the path it resolves to, so the check can
# list programs taken from outside the bundle and Windows (a test PC may have them, a player not).
if os.environ.get('HKPSP_EXEC_LOG'):
    import shutil as _shutil
    import sys as _sys

    def _log_exec(event, args, _path=os.environ['HKPSP_EXEC_LOG']):
        if event != 'subprocess.Popen':
            return
        executable, argv = args[0], args[1]
        name = executable or (argv if isinstance(argv, (str, bytes, os.PathLike)) else (argv or [''])[0])
        name = os.fsdecode(name) if not isinstance(name, str) else name
        if isinstance(argv, str) and not executable:
            name = name.strip().split('"')[1] if name.strip().startswith('"') else name.split()[0]
        env = args[3] if len(args) > 3 and args[3] else os.environ
        found = name if os.path.isabs(name) else (_shutil.which(name, path=env.get('PATH')) or '?' + name)
        try:
            with open(_path, 'a', encoding='utf-8') as stream:
                stream.write(found + '\n')
        except OSError:
            pass

    _sys.addaudithook(_log_exec)
