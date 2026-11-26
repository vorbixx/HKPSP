# Route itinerary

Scene order of the first-ending route. The builder reads this table
(tools/psp/room_packages.py `census_itinerary`) to choose which rooms to convert and package.
Each arrow is one scene change. Codes: C converted room, M room without converted output yet,
U special package, T tutorial; `/F` marks a room whose terrain conversion is incomplete.

## Ordered itinerary

| Leg | Objective and scene chain |
| --- | --- |
| 01 | **Fresh save; reach False Knight.** `Tutorial_01`[T] → `Town`[C] → `Crossroads_01`[C] → `Crossroads_07`[C] → `Crossroads_33`[C] → `Crossroads_06`[C] → `Crossroads_10`[C] |
| 02 | **Vengeful Spirit.** `Crossroads_10`[C] → `Crossroads_06`[C] → `Crossroads_ShamanTemple`[C] |
| 03 | **Rescue Sly after Gruz Mother.** `Crossroads_ShamanTemple`[C] → `Crossroads_06`[C] → `Crossroads_10`[C] → `Crossroads_21`[C] → `Crossroads_03`[C] → `Crossroads_19`[C] → `Crossroads_04`[C] → `Room_ruinhouse`[C] |
| 04 | **Reach Greenpath.** `Room_ruinhouse`[C] → `Crossroads_04`[C] → `Crossroads_19`[C] → `Crossroads_03`[C] → `Crossroads_16`[C] → `Crossroads_40`[C] → `Crossroads_05`[C] → `Crossroads_07`[C] → `Crossroads_11_alt`[C] → `Fungus1_01`[C] |
| 05 | **Lower Greenpath to Hornet.** `Fungus1_01`[C] → `Fungus1_01b`[C] → `Fungus1_02`[C] → `Fungus1_06`[C] → `Fungus1_07`[C] → `Fungus1_19`[C] → `Fungus1_10`[C] → `Fungus1_30`[C] → `Fungus1_22`[C] → `Fungus1_21`[C] → `Fungus1_04`[C] |
| 06 | **Dash to Mantis Claw.** `Fungus1_04`[C] → `Fungus1_21`[C] → `Fungus1_22`[C] → `Fungus1_30`[C] → `Fungus1_10`[C] → `Fungus1_19`[C] → `Fungus1_11`[C] → `Fungus3_01`[C] → `Fungus3_02`[C] → `Fungus2_01`[C] → `Fungus2_03`[M] → `Fungus2_18`[C] → `Fungus2_11`[C] → `Fungus2_17`[C] → `Fungus2_14`[M] |
| 07 | **Enter City with Crest.** `Fungus2_14`[M] → `Fungus2_17`[C] → `Fungus2_11`[C] → `Fungus2_10`[M] → `Fungus2_21`[M] → `Ruins1_01`[C] → `Ruins1_02`[M] → `Ruins1_03`[M] → `Ruins1_05b`[M] |
| 08 | **Free City Simple Key and exit lift.** `Ruins1_05b`[M] → `Ruins1_03`[M] → `Ruins1_02`[M] → `Ruins1_01`[C] → `Ruins1_17`[C] → `Ruins1_28`[M/F] → `Crossroads_49b`[C] → `Crossroads_49`[C] |
| 09 | **Return to Sly; buy Lantern.** `Crossroads_49`[C] → `Crossroads_43`[C] → `Crossroads_19`[C] → `Crossroads_03`[C] → `Crossroads_16`[C] → `Crossroads_40`[C] → `Crossroads_05`[C] → `Crossroads_07`[C] → `Crossroads_01`[C] → `Town`[C] → `Room_shop`[C] |
| 10 | **Lantern entrance to Crystal Heart.** `Room_shop`[C] → `Town`[C] → `Crossroads_01`[C] → `Crossroads_02`[C] → `Crossroads_39`[C] → `Crossroads_14`[C] → `Crossroads_16`[C] → `Crossroads_03`[C] → `Mines_33`[C] → `Mines_02`[C] → `Mines_03`[C] → `Mines_05`[C] → `Mines_19`[C] → `Mines_20`[C] → `Mines_31`[C] |
| 11 | **Crystal Peak drop to Dreamers memorial.** `Mines_31`[C] → `Mines_20`[C] → `Mines_37`[C] → `Mines_04`[C] → `Mines_07`[C] → `Mines_28`[C] → `RestingGrounds_02`[C] → `RestingGrounds_04`[C] |
| 12 | **Dream Nail acquisition.** `RestingGrounds_04`[C] → `Dream_Nailcollection`[U] → `RestingGrounds_07`[C] |
| 13 | **Return through Blue Lake and the opened City lift.** `RestingGrounds_07`[C] → `RestingGrounds_05`[C] → `RestingGrounds_04`[C] → `RestingGrounds_02`[C] → `RestingGrounds_06`[C] → `Crossroads_50`[C] → `Crossroads_04`[C] → `Crossroads_19`[C] → `Crossroads_43`[C] → `Crossroads_49`[C] → `Crossroads_49b`[C] → `Ruins1_28`[M/F] → `Ruins1_17`[C] → `Ruins1_01`[C] → `Ruins1_02`[M] → `Ruins1_03`[M] → `Ruins1_05b`[M] |
| 14 | **Unlock Waterways, Dung Defender, pump.** `Ruins1_05b`[M] → `Waterways_01`[C] → `Waterways_02`[C] → `Waterways_05`[C] |
| 15 | **Ancient Basin to Broken Vessel.** `Waterways_05`[C] → `Abyss_01`[C] → `Abyss_02`[C] → `Abyss_03`[C] → `Abyss_04`[C] → `Abyss_18`[C] → `Abyss_19`[C] |
| 16 | **Monarch Wings.** `Abyss_19`[C] → `Abyss_21`[C] |
| 17 | **Back to Isma's Grove.** `Abyss_21`[C] → `Abyss_19`[C] → `Abyss_18`[C] → `Abyss_04`[C] → `Abyss_03`[C] → `Abyss_02`[C] → `Abyss_01`[C] → `Waterways_07`[C] → `Waterways_13`[C] |
| 18 | **Ascend eastern City; Watcher Knights and Lurien.** `Waterways_13`[C] → `Waterways_07`[C] → `Abyss_01`[C] → `Ruins2_04`[M] → `Ruins2_01_b`[M] → `Ruins2_01`[M/F] → `Ruins2_03b`[M] → `Ruins2_03`[M/F] → `Ruins2_Watcher_Room`[M] |
| 19 | **First Dreamer: Lurien.** `Ruins2_Watcher_Room`[M] → `Dream_Guardian_Lurien`[U] → `Cutscene_Boss_Door`[M] → `Ruins2_Watcher_Room`[M] |
| 20 | **Return through Waterways to Mantis village.** `Ruins2_Watcher_Room`[M] → `Ruins2_03`[M/F] → `Ruins2_03b`[M] → `Ruins2_01`[M/F] → `Ruins2_01_b`[M] → `Ruins2_04`[M] → `Abyss_01`[C] → `Waterways_05`[C] → `Waterways_02`[C] → `Waterways_01`[C] → `Ruins1_05b`[M] → `Ruins1_03`[M] → `Ruins1_02`[M] → `Ruins1_01`[C] → `Ruins1_17`[C] → `Ruins1_28`[M/F] → `Crossroads_49b`[C] → `Crossroads_49`[C] → `Crossroads_43`[C] → `Crossroads_19`[C] → `Crossroads_42`[C] → `Crossroads_13`[C] → `Crossroads_08`[C] → `Crossroads_18`[C] → `Fungus2_06`[M] → `Fungus2_07`[M] → `Fungus2_08`[M] → `Fungus2_09`[C] → `Fungus2_10`[M] → `Fungus2_11`[C] → `Fungus2_17`[C] → `Fungus2_14`[M] |
| 21 | **Mantis Lords and Deepnest.** `Fungus2_14`[M] → `Fungus2_15`[M] → `Fungus2_25`[C] → `Deepnest_16`[C] → `Deepnest_17`[C] → `Deepnest_30`[C] → `Deepnest_03`[C] → `Deepnest_34`[C] → `Deepnest_39`[C] → `Deepnest_41`[C] → `Deepnest_10`[C] → `Deepnest_Spider_Town`[C] |
| 22 | **Second Dreamer: Herrah.** `Deepnest_Spider_Town`[C] → `Dream_Guardian_Hegemol`[U] → `Cutscene_Boss_Door`[M] → `Deepnest_Spider_Town`[C] |
| 23 | **Retrace Deepnest to village.** `Deepnest_Spider_Town`[C] → `Deepnest_10`[C] → `Deepnest_41`[C] → `Deepnest_39`[C] → `Deepnest_34`[C] → `Deepnest_03`[C] → `Deepnest_30`[C] → `Deepnest_17`[C] → `Deepnest_16`[C] → `Fungus2_25`[C] → `Fungus2_15`[M] → `Fungus2_14`[M] |
| 24 | **Fungal entrance to Fog Canyon; Teachers Archives.** `Fungus2_14`[M] → `Fungus2_17`[C] → `Fungus2_11`[C] → `Fungus2_18`[C] → `Fungus2_03`[M] → `Fungus2_01`[C] → `Fungus3_02`[C] → `Fungus3_47`[C] → `Fungus3_archive`[C] → `Fungus3_archive_02`[C] |
| 25 | **Third Dreamer: Monomon.** `Fungus3_archive_02`[C] → `Dream_Guardian_Monomon`[M] → `Cutscene_Boss_Door`[M] → `Fungus3_archive_02`[C] |
| 26 | **Retrace known path to the Black Egg.** `Fungus3_archive_02`[C] → `Fungus3_archive`[C] → `Fungus3_47`[C] → `Fungus3_02`[C] → `Fungus2_01`[C] → `Fungus2_03`[M] → `Fungus2_04`[M] → `Fungus2_05`[M/F] → `Fungus2_06`[M] → `Crossroads_18`[C] → `Crossroads_08`[C] → `Crossroads_33`[C] → `Crossroads_07`[C] → `Crossroads_01`[C] → `Crossroads_02`[C] → `Room_temple`[C] |
| 27 | **Open Black Egg; defeat and seal Hollow Knight.** `Room_temple`[C] → `Room_Final_Boss_Atrium`[C] → `Room_Final_Boss_Core`[C] → `Cinematic_Ending_A`[U] |
