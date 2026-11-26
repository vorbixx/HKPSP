# Regenerates every PSP asset in this checkout's _private/converted-psp (assets are not in git,
# so each checkout and each merge needs this). Every new PSP converter MUST be added here
# (docs/psp/agent-rules.md "Paths"). Runs each converter under tools/validation-lock.ps1 -Write.
#   tools/psp/regen-assets.ps1 [-Only room,hero,actors,lighting,motes,hud,fonts,audio]
param([string[]]$Only = @(), [string]$RepackTable, [string]$RepackProfile,
      [string]$RepackOutput, [switch]$MultiClut, [string]$SharedTrace,
      [string]$SharedManifest, [string]$SharedLayout, [string[]]$SharedBank = @(),
      [string]$SharedOutput, [string]$ShippingBase, [string]$ShippingRoom,
      [string]$ShippingShared, [string]$ShippingOutput, [switch]$ListStale,
      # Steam builds: keep Tutorial_01 unpacked when the measured repack recipe does not fit.
      [switch]$AllowUnpackedTutorial)
$ErrorActionPreference = 'Stop'
# 2026-11-18: `pwsh -File regen-assets.ps1 -Only a,b` passes ONE string "a,b"; no group matched and the
# script exited 0 having regenerated nothing (merge check then failed on a missing asset). Split and validate.
$Only = @($Only | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $PSScriptRoot 'build-config.ps1')
$python = Get-HkpspPython
$steps = [ordered]@{
    room     = @('tools/psp/transcode_room.py', 'Tutorial_01')
    rooms    = @('tools/psp/room_packages.py')
    dreamscenes = @('tools/psp/regen_dream_scenes.py')
    routerooms = @('tools/psp/room_packages.py', '--stage', (Join-Path $repo 'tools/psp/route-itinerary.md'))
    enemies  = @('tools/psp/scene_enemy_metadata.py')
    hero     = @('tools/psp/transcode_room.py', 'hero')
    abilityhero = @('tools/ability_hero.py')
    actors   = @('tools/psp/transcode_actors.py')
    townlift = @('tools/psp/transcode_town_lift.py')
    shamanpillar = @('tools/psp/transcode_shaman_pillar.py')
    citylift = @('tools/psp/transcode_city_lift_art.py')
    mantis   = @('tools/psp/transcode_mantis.py')
    worldswitch = @('tools/psp/transcode_world_switch.py')
    waterways = @('tools/psp/transcode_waterways.py')
    citycrest = @('tools/psp/transcode_city_crest.py')
    stagstation = @('tools/psp/transcode_stag_station.py')
    cornifer = @('tools/psp/transcode_cornifer.py')
    elderbug = @('tools/psp/transcode_elderbug.py')
    quirrel  = @('tools/psp/transcode_quirrel.py')
    seer     = @('tools/psp/transcode_seer.py')
    lemm     = @('tools/psp/transcode_lemm.py')
    tiso = @('tools/psp/transcode_tiso.py')
    slyshop  = @('tools/psp/transcode_sly_shop.py')
    facing   = @('tools/export_actor_facing.py', '--extract-components', '--extract-libraries', '--header', 'include/actor_facing_data.hpp')
    libraries = @('tools/psp/transcode_anim_libraries.py')
    uumuu    = @('tools/psp/transcode_uumuu.py')
    lighting = @('tools/psp/transcode_lighting.py')
    motes    = @('tools/psp/transcode_motes.py')
    hud      = @('tools/psp/transcode_hud.py', 'tools/psp/area_title_art.py', 'tools/convert_area_title.py')
    inventory = @('tools/psp/transcode_inventory.py')
    map      = @('tools/psp/transcode_map.py')
    ability  = @('tools/ability_panel.py')
    spirit   = @('tools/psp/transcode_spirit.py')
    fonts    = @('tools/psp/transcode_fonts.py')
    # Original text pack (data/text.bin) and data format (data/format.txt).
    text     = @('tools/psp/pack_text.py')
    menus    = @('tools/psp/transcode_menus.py')
    # Includes movement, room music and room atmosphere extraction before conversion.
    audio    = @('tools/psp/transcode_audio.py')
}
# 2026-11-19 (recurring): merges changed converters and their output stayed old until someone
# remembered the group. Every run stamps its group (tools/psp/regen_stamp.py); `-Only stale`
# regenerates exactly the groups whose converter code changed since their stamp, `-ListStale` lists them.
# routerooms is packaging-only and slow, so it is never picked as stale.
$stampable = @($steps.Keys | Where-Object { $_ -ne 'routerooms' })
$stampTool = Join-Path $repo 'tools/psp/regen_stamp.py'
if ($ListStale -or 'stale' -in $Only) {
    $stale = @(& $python -I $stampTool stale @($stampable | ForEach-Object { "$_=$($steps[$_][0])" }))
    if ($LASTEXITCODE) { throw "regen_stamp.py stale failed (exit $LASTEXITCODE)" }
    if ($ListStale) { if ($stale.Count) { "stale: $($stale -join ',')" } else { 'no stale groups' }; exit 0 }
    $Only = @(@($Only | Where-Object { $_ -ne 'stale' }) + $stale | Select-Object -Unique)
    # The text pack also follows the tracked text_spec.json and converter fragments; it takes a second.
    if ('text' -notin $Only) { $Only += 'text' }
    if (-not $Only.Count) { 'no stale groups'; exit 0 }
    # 2026-11-19: the seed run regenerated every owner but skipped ship-repack, so Tutorial_01 shipped
    # unpacked (30 pages, 214 evictions in 600 combat frames on the PSP-3000). The shipping repack must
    # follow any regeneration; a stale recipe then fails loudly (owner SHA256 mismatch) instead of silently.
    if ($stale.Count -and 'ship-repack' -notin $Only) { $Only += 'ship-repack' }
    # 2026-11-19 (area-titles merge): ship-repack --install rewrites Tutorial_01 and the eleven owner tables in
    # place (pooled PSA1 + SCL1/PPG1), so running it again over them fails ("Invalid room tail size"). It needs
    # every input regenerated as a baseline: room (Tutorial_01), hero, actors (buzzer, crawler, decor, fury, geo,
    # great-door, shade), hud, motes, ability.
    if ('ship-repack' -in $Only) { $Only = @($Only + @('room', 'hero', 'actors', 'hud', 'motes', 'ability') | Select-Object -Unique) }
    # 2026-11-19 (area-titles merge): abilityhero reads hero/hero.psp.bin, which ship-repack rewrote as a
    # pooled table (SCL1/PPG1 tail) -> "Invalid animation event header". Regenerate the hero baseline first.
    if ('abilityhero' -in $Only -and 'hero' -notin $Only) { $Only += 'hero' }
    "stale groups: $($stale -join ',')"
}
# Repacking requires an explicit hash-bound owner/use profile and a separate output.
# Example: -Only repack -RepackTable <table> -RepackProfile <json> -RepackOutput <new-dir> -MultiClut
$repackPaths = @($RepackTable, $RepackProfile, $RepackOutput | Where-Object { $_ })
if ($repackPaths.Count -and $repackPaths.Count -ne 3) { throw 'Repacking requires table, profile and output paths' }
if (($MultiClut -or 'repack' -in $Only) -and $repackPaths.Count -ne 3) { throw 'Repacking requires explicit paths' }
if ($repackPaths.Count) {
    $steps['repack'] = @('tools/psp/repack_pages.py', $RepackTable, $RepackProfile, '--output', $RepackOutput)
    if ($MultiClut) { $steps['repack'] += '--multi-clut' }
}
# Shared candidates retain one common page namespace and map owner table names
# in repack.json. Shipping staging must consume that mapping before adoption.
$sharedPaths = @($SharedTrace, $SharedManifest, $SharedLayout, $SharedOutput | Where-Object { $_ })
if (($sharedPaths.Count -or $SharedBank.Count -or 'shared-repack' -in $Only) -and
    ($sharedPaths.Count -ne 4 -or $SharedBank.Count -lt 2)) {
    throw 'Shared repacking requires trace, manifest, layout, output and at least two banks'
}
if ($sharedPaths.Count) {
    $steps['shared-repack'] = @('tools/psp/shared_page_repack.py', '--trace', $SharedTrace,
        '--manifest', $SharedManifest, '--layout', $SharedLayout, '--output', $SharedOutput)
    foreach ($bank in $SharedBank) { $steps['shared-repack'] += @('--bank', $bank) }
}
$shippingPaths = @($ShippingBase, $ShippingRoom, $ShippingShared, $ShippingOutput | Where-Object { $_ })
if ($shippingPaths.Count -and $shippingPaths.Count -ne 4) {
    throw 'Shipping repack requires explicit base, room, shared and output paths'
}
if ($shippingPaths.Count) {
    $steps['ship-repack'] = @('tools/psp/ship_page_repack.py', '--base', $ShippingBase,
        '--room', $ShippingRoom, '--shared', $ShippingShared, '--output', $ShippingOutput)
} else {
    # Run last after the baseline converters; the recipe verifies every source hash.
    $steps['ship-repack'] = @('tools/psp/ship_page_repack.py', '--base',
        (Join-Path $repo '_private/converted-psp'), '--recipe',
        (Join-Path $repo 'tools/psp/page-repack-tutorial.json'), '--install')
    # The same layout measured for the Steam-converted tables (tools/psp/build_tutorial_recipe.py).
    $steamRecipe = Join-Path $repo 'tools/psp/page-repack-tutorial-steam.json'
    if (Test-Path -LiteralPath $steamRecipe) { $steps['ship-repack'] += @('--recipe', $steamRecipe) }
    if ($AllowUnpackedTutorial) { $steps['ship-repack'] += '--if-matching' }
}
$unknown = @($Only | Where-Object { $_ -notin $steps.Keys -and $_ -notin @('repack', 'shared-repack') })
if ($unknown.Count) { throw "Unknown regen group(s): $($unknown -join ', '); known: $($steps.Keys -join ', ')" }
New-Item -ItemType Directory -Force (Join-Path $repo 'build/psp') | Out-Null
foreach ($name in $steps.Keys) {
    if ($Only.Count -and $name -notin $Only -and -not ($name -eq 'abilityhero' -and 'hero' -in $Only)) { continue }
    if ($name -eq 'routerooms' -and 'routerooms' -notin $Only) { continue }  # explicit only (slow, 116 rooms)
    $log = Join-Path $repo "build/psp/regen-$name.log"
    $args = @('-NoProfile', '-File', (Join-Path $repo 'tools/validation-lock.ps1'), '-Write', '--', $python, '-I') +
        @($steps[$name] | ForEach-Object { if ($_ -like 'tools/*') { Join-Path $repo $_ } else { $_ } })
    # 2026-11-19 (fixture-api-b): a converter's DeprecationWarning on stderr became a NativeCommandError
    # under 'Stop' and aborted `-Only stale` at lighting. Only the exit code decides.
    $ErrorActionPreference = 'Continue'
    & pwsh @args *> $log
    $code = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($code) { throw "regen $name failed (exit $code): $log" }
    if ($name -in $stampable) {
        & $python -I $stampTool record "$name=$($steps[$name][0])"
        if ($LASTEXITCODE) { throw "regen_stamp.py record $name failed (exit $LASTEXITCODE)" }
    }
    "{0,-9} ok  {1}" -f $name, ((Get-Content $log -Tail 1) -replace '\s+', ' ')
}
