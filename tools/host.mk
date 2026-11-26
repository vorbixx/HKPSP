# Native GNU Make (w64devkit); invoked by test-host.ps1 from the repository root.
.DEFAULT_GOAL := all
.DELETE_ON_ERROR:
.SUFFIXES:
# Keep GNU ld import-archive member timestamps stable as well as the PE header.
export SOURCE_DATE_EPOCH := 0

CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Werror -pedantic -Iinclude
CORE := hero focus physics animation combat enemy terrain contour_physics contact_solver breakable contact_effects room room_entities room_visual room_dynamics room_ambient sector_cache tutorial great_door audio wind_audio save geo geo_visual fury_chest health_cocoon health_cocoon_fx health_scuttler plant grass grass_scene ambient_particles hud tutorial_prompts lore_tablets shade stalactite_visual conditional_events
PROBES := contact_probe edge_probe corner_probe speed_probe physics_probe recoil_probe encounter_probe timing_probe tutorial_probe decor_effect_probe decor_motion_probe geo_capture_probe hero_effect_probe conditional_particle_probe map_deepnest_route
PROBES += cast_save_fixture scene_save_fixture uumuu_fight_probe
PROBES += monomon_room_probe
PROBES += map_coverage_probe
UNITS := contact_effects_tests hero_tests combat_tests enemy_tests terrain_tests breakable_tests timing_tests room_visual_tests room_dynamics_tests sector_cache_tests ccd_tests room_tests room_entities_tests great_door_tests geo_tests geo_visual_tests fury_chest_tests health_cocoon_tests health_scuttler_tests save_tests audio_tests tutorial_tests tutorial_room_tests plant_tests grass_tests grass_scene_tests lighting_tests console_wire_tests hud_tests conditional_events_tests greenpath_route_tests
CORE += ui
# Original game text from the converted text pack (data/text.bin).
CORE += text_pack
CORE += shop_menu
UNITS += ui_tests
UNITS += wind_audio_player_tests
UNITS += cartographer_audio_tests
UNITS += heap_guard_tests
CORE += hero_death hero_effects focus_effects surface_swim
CORE += cartographer
CORE += nail_interactions
CORE += greenpath_enemy
CORE += enemy_c
CORE += enemy_b
CORE += cpu_worker tutorial_render_state
CORE += scene
CORE += scene_activation
CORE += scene_walls
CORE += scene_enemies
CORE += elder_baldur
CORE += enemy_rewards
CORE += resting
CORE += dream_nail
CORE += collectibles
CORE += world_quests
UNITS += world_quests_tests
CORE += crossroads_boss
CORE += watcher_knights mantis_lords broken_vessel
CORE += uumuu
CORE += monomon
CORE += monomon_room
CORE += uumuu_attacks
CORE += uumuu_multizaps
CORE += uumuu_zap
CORE += uumuu_death
CORE += uumuu_motion
CORE += uumuu_encounter
CORE += uumuu_quirrel
CORE += uumuu_quirrel_parts
CORE += uumuu_body
CORE += uumuu_colliders
CORE += uumuu_gates
CORE += uumuu_room
CORE += uumuu_wake
CORE += super_dash
UNITS += frame_pipeline_tests
UNITS += cornifer_map_tests cartographer_tests
OBJECTS := $(addprefix build/host/obj/,$(addsuffix .o,$(CORE)))
TEST_OBJECTS := $(addprefix build/host/obj/,$(addsuffix .o,$(UNITS) $(PROBES)))
PROGRAMS := $(addprefix build/host/,$(addsuffix .exe,$(UNITS) $(PROBES)))
CORE_DLL := build/host/runtime.dll
CORE_IMPORT := build/host/runtime.dll.a
PYTHON ?= python

.PHONY: all
all: $(addprefix build/host/,$(addsuffix .exe,$(UNITS)))

$(OBJECTS) $(TEST_OBJECTS): tools/host.mk build/host/compiler-config.txt

build/host/obj/%.o: src/%.cpp
	@echo CXX $<
	@"$(CXX)" $(CXXFLAGS) -MMD -MP -c $< -o $@

build/host/obj/%.o: tests/%.cpp
	@echo CXX $<
	@"$(CXX)" $(CXXFLAGS) -MMD -MP -c $< -o $@

# Resolve every runtime object, including otherwise unused code. An unchanged
# export interface keeps the import library timestamp, so suites need no relink.
$(CORE_DLL) $(CORE_IMPORT) &: $(OBJECTS) tools/host.mk tools/stable_file.py
	@echo LINK $(CORE_DLL)
	@"$(CXX)" -shared $(OBJECTS) -Wl,--export-all-symbols,--no-undefined,--no-insert-timestamp,--out-implib,$(CORE_IMPORT).next -o $(CORE_DLL)
	@$(PYTHON) tools/stable_file.py $(CORE_IMPORT).next $(CORE_IMPORT)

$(PROGRAMS): build/host/%.exe: build/host/obj/%.o $(CORE_IMPORT) tools/host.mk | $(CORE_DLL)
	@echo LINK $@
	@"$(CXX)" $(filter %.o,$^) $(CORE_IMPORT) -o $@

# Compile the actual platform adapter against a small host NDSP implementation.
build/host/obj/wind_audio_player_host.o: src/wind_audio_player.cpp
	@"$(CXX)" $(CXXFLAGS) -Itests/audio_platform -DHK3DS_DIAGNOSTICS=0 -MMD -MP -c $< -o $@
build/host/obj/wind_audio_player_tests.o: CXXFLAGS += -Itests/audio_platform -DHK3DS_DIAGNOSTICS=0
build/host/wind_audio_player_tests.exe: build/host/obj/wind_audio_player_host.o
build/host/obj/wind_audio_player_host.o: tools/host.mk build/host/compiler-config.txt
-include build/host/obj/wind_audio_player_host.d

build/host/obj/cartographer_audio_host.o: src/cartographer_audio.cpp tools/host.mk build/host/compiler-config.txt
	@"$(CXX)" $(CXXFLAGS) -Itests/audio_platform -DHK3DS_DIAGNOSTICS=0 -MMD -MP -c $< -o $@
build/host/obj/cartographer_audio_tests.o: CXXFLAGS += -Itests/audio_platform -DHK3DS_DIAGNOSTICS=0
build/host/cartographer_audio_tests.exe: build/host/obj/cartographer_audio_host.o
-include build/host/obj/cartographer_audio_host.d

build/host/ccd_tests.exe: build/host/obj/contact_exhaustive.o
build/host/obj/contact_exhaustive.o: src/contact_solver.cpp include/physics.hpp tools/host.mk
	@"$(CXX)" $(CXXFLAGS) -DHK3DS_CCD_EXHAUSTIVE -DsolveContacts=solveContactsExhaustive -MMD -MP -c $< -o $@

build/host/terrain_tests.exe: build/host/obj/contour_exhaustive.o
build/host/obj/contour_exhaustive.o: src/contour_physics.cpp include/physics.hpp tools/host.mk
	@"$(CXX)" $(CXXFLAGS) -DHK3DS_RAY_EXHAUSTIVE -DrayContours=rayContoursExhaustive -DmoveContours=moveContoursExhaustive -MMD -MP -c $< -o $@

# Route audit: compare every query's result bits with the exhaustive implementation.
build/host/tutorial_query_probe.exe: build/host/obj/tutorial_probe.o build/host/obj/terrain_query_verify.o build/host/obj/contour_optimized.o build/host/obj/contour_exhaustive.o $(filter-out build/host/obj/contour_physics.o,$(OBJECTS))
	@"$(CXX)" $(filter %.o,$^) -o $@
build/host/obj/terrain_query_verify.o: tests/terrain_query_verify.cpp include/physics.hpp tools/host.mk
	@"$(CXX)" $(CXXFLAGS) -MMD -MP -c $< -o $@
build/host/obj/contour_optimized.o: src/contour_physics.cpp include/physics.hpp tools/host.mk
	@"$(CXX)" $(CXXFLAGS) -DrayContours=rayContoursOptimized -MMD -MP -c $< -o $@

-include $(OBJECTS:.o=.d) $(TEST_OBJECTS:.o=.d)
