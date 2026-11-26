#!/bin/sh
# ilspycmd for the Linux bundle: the NuGet tool (net8.0, any OS) run by the bundled .NET
# runtime. Installed as runtime/ilspy/ilspycmd next to ilspycmd.dll.
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
dotnet="${DOTNET_ROOT:-$here/../dotnet}/dotnet"
exec "$dotnet" "$here/ilspycmd.dll" "$@"
