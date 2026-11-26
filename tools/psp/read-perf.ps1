# Copy a completed hardware run without changing the stick. -Drive also accepts
# a directory containing PSP/GAME/HKPSPPERF/out for an offline collection check.
# -Path validates an already collected directory (psplink.ps1 uses it) without copying.
param([Parameter(ParameterSetName='Drive',Mandatory=$true)][string]$Drive,
      [Parameter(ParameterSetName='Path',Mandatory=$true)][string]$Path,
      [switch]$Gpu)
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$root=if ($Drive -match '^[A-Za-z]:$') { $Drive+'\' } else { $Drive }
$game=if ($Gpu) { 'HKPSPGPU' } else { 'HKPSPPERF' }
$source=if ($Path) { (Resolve-Path -LiteralPath $Path).Path } else { Join-Path $root "PSP/GAME/$game/out" }
$required=if ($Gpu) { @('gpu.csv','summary.csv','summary.txt','vram.csv','done.txt') } else { @('perf.csv','summary.txt','done.txt','cpu.csv','cpu-summary.csv','cpu-done.txt') }
$details=@('simulation_us','spatial_checks','spatial_rejects','room_candidates','owner_checks','owner_rejects',
    'room_tests','reject_pass','reject_split','reject_alpha','reject_clip','reject_opacity','reject_frustum','room_quads',
    'sim_hero_us','sim_enemies_us','sim_flow_us','sim_decor_us','sim_ambient_us','sim_particles_us','vblank_delta','page_loads','page_open_us','page_read_us','page_verify_us','page_load_us','page_evictions','resident_bytes','blocking_miss','texture_read_bytes','ge_scene_us','ge_grade_us','ge_hud_us','cpu_scene_us','cpu_grade_us','cpu_hud_us','arena_bytes','largest_free_block','audio_read_bytes','audio_stream_underruns','audio_tail_underruns','event_nail_hits','event_decor_cuts','event_damage_taken','event_pickup_geo')
$details+=@('audio_read_calls','audio_read_us','audio_read_cpu_us','audio_pump_wakeups','audio_cpu_probe_errors')
$details+=@('game_run_us','mixer_run_us','audio_io_run_us','idle_run_us','kernel_interrupt_us','runtime_excess_us','runtime_probe_errors','simulation_run_us','draw_list_run_us','gu_run_us','audio_submit_us','perf_bookkeeping_us','swap_us','runtime_probe_us','runtime_probe_calls')
$details+=@('page_overflow_frames','page_ge_waits','page_ge_wait_us')
$details+=@('clip_storage_us','clip_triangles_us','clip_initialized_bytes',
    'upload_pages_us','upload_heap_us','upload_bookkeeping_us','submit_batches','page_prepare_batches')
$details+=@('audio_open_calls','audio_open_us','audio_open_cpu_us','audio_close_calls','audio_close_us','audio_close_cpu_us',
    'audio_seek_calls','audio_seek_us','audio_seek_cpu_us','audio_clip_changes',
    'audio_reads_le_8192','audio_reads_le_16384','audio_reads_le_32768','audio_reads_le_65536','audio_reads_gt_65536',
    'audio_ring_low_frames','audio_tail_low_pages')
$details+=@('page_vram_bytes','page_vram_free_bytes','page_pool_slots','page_fixed_pages','page_pass_evictions','clut_loads')
$details+=@('gu_state_commands','texture_flushes')
$simulationDetails=@('sim_schedule_us','sim_physics_callbacks_us','sim_contacts_us','sim_combat_us','sim_snapshot_us',
    'sim_world_us','sim_interactions_us','sim_session_us','sim_observe_us')
$details+=$simulationDetails+@('sim_bodies','sim_collider_visits','sim_combat_tests','sim_decor_tests','sim_decor_particles','sim_ambient_slots','sim_decor_emission_slots')
$details+=@('sim_state_hash_lo','sim_state_hash_hi','sim_state_leaves')
$details+=@('sim_decor_terrain_tests','sim_decor_terrain_queries')
function Check-Details($r,$label) {
    if ($r.page_loaded_ids -cnotmatch '^(?:(?:0|[1-9][0-9]*)(?:;(?:0|[1-9][0-9]*))*)?$') { throw "Invalid page IDs at $label" }
    $ids=if ($r.page_loaded_ids -ceq '') { @() } else { @($r.page_loaded_ids -split ';') }
    if ($ids.Count -ne [double]$r.page_loads -or
        [double]$r.blocking_miss -ne [int]($ids.Count -gt 0)) { throw "Page counters disagree at $label" }
    if ([double]$r.page_overflow_frames -ne [int]([double]$r.page_ge_waits -gt 0) -or
        ([double]$r.page_ge_waits -eq 0 -and [double]$r.page_ge_wait_us -ne 0)) { throw "Page overflow counters disagree at $label" }
    if ($ids.Count -eq 0 -and ([double]$r.page_open_us+[double]$r.page_read_us+[double]$r.page_verify_us) -ne 0) { throw "Page time without load at $label" }
    foreach ($name in $details) {
        if ($r.$name -cnotmatch '^(0|[1-9][0-9]*)$' -or [double]$r.$name -gt [uint32]::MaxValue) { throw "Invalid perf detail $name at $label" }
    }
    $rejected=0.0;foreach ($name in @('reject_pass','reject_split','reject_alpha','reject_clip','reject_opacity','reject_frustum')) { $rejected+=[double]$r.$name }
    if ([double]$r.room_tests -ne $rejected+[double]$r.room_quads -or
        [double]$r.spatial_rejects -gt [double]$r.spatial_checks -or [double]$r.owner_rejects -gt [double]$r.owner_checks) { throw "Invalid rejection attribution at $label" }
    $sim=0.0;foreach ($name in (@('sim_hero_us','sim_enemies_us','sim_flow_us','sim_decor_us','sim_ambient_us','sim_particles_us')+$simulationDetails)) { $sim+=[double]$r.$name }
    if ($sim -gt [double]$r.simulation_us) { throw "Invalid simulation attribution at $label" }
}
foreach ($name in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $source $name) -PathType Leaf)) {
        throw "Incomplete perf run: missing $source/$name"
    }
}
if (Test-Path -LiteralPath (Join-Path $source 'error.txt')) { throw 'Run contains error.txt' }
$marker=(Get-Content -LiteralPath (Join-Path $source 'done.txt') -Raw) -replace "`r",''
$pagePattern='page_loads=(0|[1-9][0-9]*)\npage_open_us=(0|[1-9][0-9]*)\npage_read_us=(0|[1-9][0-9]*)\npage_verify_us=(0|[1-9][0-9]*)\npage_evictions=(0|[1-9][0-9]*)\npage_resident_bytes=(0|[1-9][0-9]*)\npage_blocking_miss=(0|[1-9][0-9]*)\npage_loaded_ids=((?:(?:0|[1-9][0-9]*)(?:;(?:0|[1-9][0-9]*))*)?)\n\z'
$pagePattern=$pagePattern.Substring(0,$pagePattern.Length-2)+'page_overflow_frames=(0|[1-9][0-9]*)\npage_ge_waits=(0|[1-9][0-9]*)\npage_ge_wait_us=(0|[1-9][0-9]*)\n\z'
$pagePattern=$pagePattern.Substring(0,$pagePattern.Length-2)+'page_vram_bytes=(0|[1-9][0-9]*)\npage_vram_free_bytes=(0|[1-9][0-9]*)\npage_pool_slots=(0|[1-9][0-9]*)\npage_fixed_pages=(0|[1-9][0-9]*)\npage_pass_evictions=(0|[1-9][0-9]*)\n\z'
if ($marker -cnotmatch $pagePattern) { throw 'Incomplete page counters in done marker' }
$marker=($marker -creplace $pagePattern,'') -replace "animation_missing_plays=0`n$",''
if ($Gpu) {
    $configs=@('full','grade_off','lights_off','grade_lights_off','nearest','far_off','near_off','hero_hud','vram','clut_cached','pass_sync',
        'grade_copy_only','grade_one_sweep','grade_stale','grade_direct32','grade_vram','grade_target','target_strip64','target_strip32','target_strip16','target_sprites','target_noblend0','fill_x3','fill_clut8','target_strip16_cached')
    $passes=@('far','near','actors','lights','grade','hud')
    if ($marker -cne "configs=25`nframes_per_config=150`nframes=3750`ncontrol_steps=7500`n") {
        throw 'Incomplete GPU run: expected 25 configs / 3750 frames / 7500 control steps'
    }
    $rows=@(Import-Csv -LiteralPath (Join-Path $source 'gpu.csv'))
    if ($rows.Count -ne 3750) { throw "Incomplete GPU CSV: $($rows.Count) rows, expected 3750" }
    $columns=@('config','frame','ge_us','total_us','gu_us','sync_us','draw_list_us','clipping_us','quads','triangles','texture_binds','clut_loads','clut_skips','segments','vram_binds')
    $columns+=@($passes | ForEach-Object { $_+'_us' });$columns+=@($passes | ForEach-Object { $_+'_quads' })
    if (($rows[0].PSObject.Properties.Name -join ',') -cne ($columns -join ',')) { throw 'Invalid GPU CSV columns' }
    $summary=@(Import-Csv -LiteralPath (Join-Path $source 'summary.csv'))
    if ($summary.Count -ne $configs.Count) { throw 'Incomplete GPU summary' }
    for ($c=0;$c -lt $configs.Count;$c++) {
        $ge=@();$total=@();$grade=@()
        for ($f=0;$f -lt 150;$f++) {
            $r=$rows[$c*150+$f]
            if ($r.config -cne $configs[$c] -or $r.frame -cne [string]$f) { throw "Invalid GPU frame $c/$f" }
            foreach ($column in $columns | Select-Object -Skip 2) {
                if ($r.$column -cnotmatch '^(0|[1-9][0-9]*)$' -or [double]$r.$column -gt [uint32]::MaxValue) { throw "Invalid GPU counter $column at $c/$f" }
            }
            $ge+= [double]$r.ge_us;$total+= [double]$r.total_us;$grade+= [double]$r.grade_us
        }
        $s=$summary[$c]
        if ($s.config -cne $configs[$c] -or $s.frames -cne '150') { throw "Invalid GPU summary config $c" }
        $ge=@($ge | Sort-Object);$total=@($total | Sort-Object);$grade=@($grade | Sort-Object)
        $expected=@{mean_ge_ms=($ge | Measure-Object -Average).Average/1000;p95_ge_ms=$ge[142]/1000;
            mean_total_ms=($total | Measure-Object -Average).Average/1000;p95_total_ms=$total[142]/1000;
            mean_grade_ms=($grade | Measure-Object -Average).Average/1000;p95_grade_ms=$grade[142]/1000}
        foreach ($key in $expected.Keys) {
            $value=0.0
            if (-not [double]::TryParse($s.$key,[Globalization.NumberStyles]::Float,[Globalization.CultureInfo]::InvariantCulture,[ref]$value) -or
                [double]::IsNaN($value) -or [double]::IsInfinity($value) -or [Math]::Abs($value-$expected[$key]) -gt 0.000000501) {
                throw "GPU summary mismatch: $($configs[$c]) $key"
            }
        }
    }
} else {
# The game appends animation_missing_plays (anim-facing guard); a clean run reports 0.
if ($marker -cne "frames=600`ncontrol_steps=1200`n") {
    throw 'Incomplete perf run: expected 600 frames / 1200 control steps'
}
$rows=@(Import-Csv -LiteralPath (Join-Path $source 'perf.csv'))
if ($rows.Count -ne 600) { throw "Incomplete perf CSV: $($rows.Count) rows, expected 600" }
# Preserve collection of main's older 1/11-config runs and the first GE A/B.
# Once a schema group is present, require and validate the entire group.
$roomDetails=@('room_cpu_projected','room_runs_drawn','room_ge_vertices','room_vertex_bytes',
    'room_alpha_fallback','room_ge_mode','game_heap_bytes','room_scene_bytes')
$roomCostDetails=@('room_ge_matrix_loads','room_ge_matrix_bytes','room_ge_draw_bytes','gu_list_bytes',
    'room_ge_stream_visits','room_ge_key_searches','room_ge_order_tests','room_ge_span_visits')
if (@($roomDetails | Where-Object { $_ -cin $rows[0].PSObject.Properties.Name }).Count) { $details+=$roomDetails }
if (@($roomCostDetails | Where-Object { $_ -cin $rows[0].PSObject.Properties.Name }).Count) { $details+=$roomCostDetails }
# A capture older than these columns lacks them; name the cause instead of a per-value failure.
$missingColumns=@($details | Where-Object { $_ -cnotin $rows[0].PSObject.Properties.Name })
if ($missingColumns.Count -gt 0) { throw "Stale PPSSPP perf capture $source (missing columns: $($missingColumns -join ', ')); refresh with: python -I tests/psp/test_perf_hw.py" }
for ($i=0;$i -lt $rows.Count;$i++) {
    if ($rows[$i].frame -cne [string]$i) { throw "Invalid perf frame at row $i" }
    Check-Details $rows[$i] "baseline/$i"
}
if (Test-Path -LiteralPath (Join-Path $source 'cpu.csv')) {
    foreach ($name in @('cpu-summary.csv','cpu-done.txt')) {
        if (-not (Test-Path -LiteralPath (Join-Path $source $name))) { throw "Incomplete CPU run: missing $name" }
    }
    # N = 1 (shipped), 2 (exact/GE A/B), or 11 (PERF_SWEEP).
    $cpuMarker=(Get-Content -LiteralPath (Join-Path $source 'cpu-done.txt') -Raw) -replace "`r",''
    $cpuMatch=[regex]::Match($cpuMarker,'\Aconfigs=([0-9]+)\nframes_per_config=600\nframes=([0-9]+)\ncontrol_steps=([0-9]+)\n\z')
    if (-not $cpuMatch.Success) { throw 'Incomplete CPU marker' }
    $cpuConfigs=[int]$cpuMatch.Groups[1].Value
    if ($cpuConfigs -notin @(1,2,11)) { throw "Unsupported CPU config count: $cpuConfigs (expected 1, 2 or 11)" }
    if ($cpuMatch.Groups[2].Value -cne [string]($cpuConfigs*600) -or $cpuMatch.Groups[3].Value -cne [string]($cpuConfigs*1200)) { throw 'Incomplete CPU marker' }
    $cpu=@(Import-Csv -LiteralPath (Join-Path $source 'cpu.csv'))
    if ($cpu.Count -ne $cpuConfigs*600) { throw "Incomplete CPU CSV: $($cpu.Count) rows, expected $($cpuConfigs*600)" }
    # @(...) keeps a single name an array (a one-element if-result would unroll to a string).
    $configs=@(if ($cpuConfigs -eq 1) { 'shipped' } elseif ($cpuConfigs -eq 2) { 'room_exact','room_ge' } else { 'baseline','c1','c2','c3','shipped','strips','page_fix','quad_cache','all','io_22','io_30' })
    $phaseNames=@('prepare_us','plants_us','ambient_us','decor_us','particles_us','motion_us','index_us','sort_us','filter_us','projection_us','lights_us','actors_us','grade_build_us','hud_us','owner_table_us','hud_text_us','other_us','clip_us')
    $guPhases=@('upload_us','commands_us','finish_us')
    $columns=@('config','frame','draw_list_us','gu_us','total_us','quads','triangles','texture_binds','blend_changes','draw_calls')+$phaseNames+$guPhases+@('sync_us','vblank_us','simulation_us','ge_wait_us','input_us')+$details[1..($details.Count-1)]+@('page_loaded_ids')
    if (($cpu[0].PSObject.Properties.Name -join ',') -cne ($columns -join ',')) { throw 'Invalid CPU CSV columns' }
    for ($c=0;$c -lt $configs.Count;$c++) {
        for ($f=0;$f -lt 600;$f++) {
            $r=$cpu[$c*600+$f]
            if ($r.config -cne $configs[$c] -or $r.frame -cne [string]$f) { throw "Invalid CPU frame $c/$f" }
            Check-Details $r "$c/$f"
            foreach ($property in $r.PSObject.Properties | Select-Object -Skip 2 | Where-Object Name -ne 'page_loaded_ids') {
                if ($property.Value -cnotmatch '^(0|[1-9][0-9]*)$' -or [double]$property.Value -gt [uint32]::MaxValue) { throw 'Invalid CPU counter' }
            }
            $sum=0.0;foreach ($name in $phaseNames) { $sum+=[double]$r.$name }
            if ([Math]::Abs($sum-[double]$r.draw_list_us) -gt [double]$r.draw_list_us*0.05) { throw "CPU phases exceed 5% at $c/$f" }
            if ([double]$r.ge_wait_us -ne [double]$r.finish_us+[double]$r.sync_us) { throw "Invalid GE wait at $c/$f" }
        }
    }
    $table=@(Import-Csv -LiteralPath (Join-Path $source 'cpu-summary.csv'))
    if ($table.Count -ne $configs.Count -or ($table.config -join ',') -cne ($configs -join ',')) { throw 'Incomplete CPU summary' }
    for ($c=0;$c -lt $configs.Count;$c++) {
        $s=$table[$c];$group=@($cpu | Select-Object -Skip ($c*600) -First 600)
        if ($s.frames -cne '600') { throw 'Invalid CPU summary frames' }
        $expected=@{}
        foreach ($name in $phaseNames+$guPhases+@('draw_calls','input_us')+$details) {
            $expected['mean_'+$name]=($group | Measure-Object -Property $name -Average).Average
        }
        foreach ($name in @('draw_list','gu','total')) {
            $expected['mean_'+$name+'_ms']=($group | Measure-Object -Property ($name+'_us') -Average).Average/1000
        }
        $sorted=@($group | ForEach-Object { [double]$_.draw_list_us } | Sort-Object)
        $expected['p95_draw_list_ms']=$sorted[569]/1000
        foreach ($key in $expected.Keys) {
            $value=0.0
            if (-not [double]::TryParse($s.$key,[Globalization.NumberStyles]::Float,[Globalization.CultureInfo]::InvariantCulture,[ref]$value) -or
                [double]::IsNaN($value) -or [double]::IsInfinity($value) -or [Math]::Abs($value-$expected[$key]) -gt 0.000000501) {
                throw "CPU summary mismatch: $($configs[$c]) $key"
            }
        }
    }
}
}
# Each configuration includes cold setup, measurement and preview in its wall time.
$secondsFile=Join-Path $source 'config-seconds.csv'
if (-not (Test-Path -LiteralPath $secondsFile)) { throw 'Missing config-seconds.csv' }
$seconds=@(Import-Csv -LiteralPath $secondsFile)
if ($seconds.Count -ne $configs.Count -or ($seconds[0].PSObject.Properties.Name -join ',') -cne 'config,seconds') { throw 'Invalid config seconds columns/count' }
for ($c=0;$c -lt $configs.Count;$c++) {
    $value=0.0
    if ($seconds[$c].config -cne $configs[$c] -or
        -not [double]::TryParse($seconds[$c].seconds,[Globalization.NumberStyles]::Float,[Globalization.CultureInfo]::InvariantCulture,[ref]$value) -or
        [double]::IsNaN($value) -or [double]::IsInfinity($value) -or $value -le 0) { throw "Invalid config seconds at $c" }
}
if (-not $Gpu) {
    & python -I (Join-Path $PSScriptRoot 'perf_run3.py') $source
    if ($LASTEXITCODE -ne 0) { throw 'Run-3 validation failed' }
}
if ($Path) {
    $destination=$source
    "Perf validated (not copied): $destination"
} else {
    $prefix=if ($Gpu) { 'gpu-' } else { 'perf-' }
    $destination=Join-Path $repo ('build/psp/hw/'+$prefix+(Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
    if (Test-Path -LiteralPath $destination) { throw "Collection already exists: $destination" }
    New-Item -ItemType Directory -Force $destination | Out-Null
    Get-ChildItem -LiteralPath $source -File | Copy-Item -Destination $destination
    "Perf copied: $destination"
}
if (-not $Gpu -and (Test-Path -LiteralPath (Join-Path $destination 'cpu-summary.csv'))) {
    'CPU phases use microseconds; draw-list/GU/total columns use ms. PPSSPP is shape only.'
    Get-Content -LiteralPath (Join-Path $destination 'cpu-summary.csv')
} else { Get-Content -LiteralPath (Join-Path $destination 'summary.txt') }
