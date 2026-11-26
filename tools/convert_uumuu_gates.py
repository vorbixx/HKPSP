#!/usr/bin/env python3
"""Validate and convert the three Archives battle gates from the saved scene export."""
from __future__ import annotations

import json
import converted_files
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "_private/minimum-bosses/uumuu/level229-source.json"
OBJECTS = ROOT / "_private/minimum-bosses/uumuu/level229-objects.json"
HEADER = ROOT / "include/uumuu_gates_data.hpp"
EXPECTED = {3: False, 6: True, 13: True}
GATE_NAMES = {3: "Battle Gate Archives (2)", 6: "Battle Gate Archives", 13: "Battle Gate Archives (1)"}
EXPECTED_LAYER = 8


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def vec3(value):
    return tuple(float(value[k]) for k in ("x", "y", "z"))


def _actions(state):
    return [(a["type"].rsplit(".", 1)[-1], int(a["enabled"]), a.get("parameters", {}))
            for a in state.get("actions", [])]


def _plain_bool(value, expected):
    return isinstance(value, bool) and value is expected


def _validate_active_action(action, expected, owner, state_name):
    active = action.get("parameters", {}).get("active")
    require(isinstance(active, dict) and _plain_bool(active.get("useVariable"), False) and
            _plain_bool(active.get("value"), expected),
            f"gate {owner}: {state_name} active collider/renderer value changed")


def validate_fsm(fsm, owner, expected_start_closed):
    require(fsm.get("name") == "BG Control" and fsm.get("start") == "Init",
            f"gate {owner}: unsupported FSM identity/start")
    closed = [x.get("value") for x in fsm.get("variables", {}).get("boolVariables", [])
              if x.get("name") == "Start Closed"]
    require(len(closed) == 1 and type(closed[0]) is int and closed[0] == int(expected_start_closed),
            f"gate {owner}: Start Closed changed")
    gt = [(x.get("fsmEvent", {}).get("name"), x.get("toState")) for x in fsm.get("globalTransitions", [])]
    require(gt == [("BG DESTROY", "Destroy"), ("BG QUICK OPEN", "Quick Open")],
            f"gate {owner}: unsupported global transitions")
    states = {s["name"]: s for s in fsm.get("states", [])}
    required = {"Init", "Close 1", "Open", "Quick Close", "Quick Open", "Opened"}
    require(required <= states.keys(), f"gate {owner}: missing gate states")
    trans = lambda name: [(x.get("fsmEvent", {}).get("name"), x.get("toState"))
                          for x in states[name].get("transitions", [])]
    expected_transitions = {
        "Init": [("BG CLOSE", "Close 1"), ("BG QUICK CLOSE", "Quick Close")],
        "Close 1": [("BG OPEN", "Open")],
        "Open": [("BG CLOSE", "Close 1"), ("BG QUICK CLOSE", "Quick Close"), ("FINISHED", "Opened")],
        "Quick Close": [("BG OPEN", "Open")],
        "Quick Open": [("BG CLOSE", "Close 1"), ("BG QUICK CLOSE", "Quick Close")],
        "Opened": [("BG CLOSE", "Close 1"), ("BG QUICK CLOSE", "Quick Close")],
        "Destroy": [],
    }
    require(set(states) == set(expected_transitions), f"gate {owner}: unexpected state set")
    for name, expected in expected_transitions.items():
        require(trans(name) == expected, f"gate {owner}: {name} transitions changed")
    # Validate ordered state actions, including disabled legacy actions: Open's disabled
    # SetCollider must not be mistaken for an active collider change.
    expected = {
        "Init": [("GetOwner", 1), ("BoolTest", 1), ("SetCollider", 1), ("SetMeshRenderer", 1)],
        "Close 1": [("SetMeshRenderer", 1), ("Tk2dPlayAnimationWithEvents", 1), ("SetCollider", 1)],
        "Open": [("Tk2dPlayAnimation", 0), ("PlayParticleEmitter", 0), ("SetCollider", 0), ("Tk2dPlayAnimationWithEvents", 1)],
        "Quick Close": [("SetCollider", 1), ("Tk2dPlayAnimation", 1), ("SetMeshRenderer", 1)],
        "Quick Open": [("Tk2dPlayAnimation", 1), ("SetCollider", 1)],
        "Opened": [("SetCollider", 1)],
    }
    for name, sequence in expected.items():
        actions = _actions(states[name])
        require([(n, enabled) for n, enabled, _ in actions] == sequence,
                f"gate {owner}: {name} action order/enabled flags changed")
    active_values = {
        "Init": {2: False, 3: False}, "Close 1": {0: True, 2: True},
        "Open": {2: False}, "Quick Close": {0: True, 2: True},
        "Quick Open": {1: False}, "Opened": {0: False},
    }
    for name, indices in active_values.items():
        actions = states[name].get("actions", [])
        for index, value in indices.items():
            _validate_active_action(actions[index], value, owner, name)
    require(_actions(states["Open"])[2][2].get("active", {}).get("value") is False,
            f"gate {owner}: disabled Open SetCollider action changed")
    bool_test = states["Init"].get("actions", [])[1].get("parameters", {})
    require(bool_test.get("boolVariable") == {"variable": "Start Closed"} and
            bool_test.get("isTrue") == "BG QUICK CLOSE" and bool_test.get("isFalse") == "" and
            _plain_bool(bool_test.get("everyFrame"), False),
            f"gate {owner}: Init BoolTest parameters changed")
    destroy_actions = states["Destroy"].get("actions", [])
    require(len(destroy_actions) == 1 and
            destroy_actions[0].get("type", "").rsplit(".", 1)[-1] == "DestroySelf" and
            type(destroy_actions[0].get("enabled")) is int and destroy_actions[0].get("enabled") == 1 and
            destroy_actions[0].get("parameters", {}).get("detachChildren") ==
            {"value": False, "useVariable": False},
            f"gate {owner}: DestroySelf parameters changed")
    require(_actions(states["Init"])[1][2].get("isTrue") == "BG QUICK CLOSE" and
            _actions(states["Close 1"])[1][2].get("animationCompleteEvent") == "FINISHED" and
            _actions(states["Open"])[3][2].get("animationCompleteEvent") == "FINISHED",
            f"gate {owner}: start/animation completion event changed")
    require(_actions(states["Open"])[3][2].get("clipName", {}).get("value") == "Open" and
            _actions(states["Close 1"])[1][2].get("clipName", {}).get("value") == "Close" and
            _actions(states["Quick Close"])[1][2].get("clipName", {}).get("value") == "Closed" and
            _actions(states["Quick Open"])[0][2].get("clipName", {}).get("value") == "Opened",
            f"gate {owner}: animation clip names changed")


def convert(source, objects):
    require(source.get("build_id") == 22529139 and source.get("scene") == "level229",
            "unsupported Uumuu source build/scene")
    actors = {int(a["path_id"]): a for a in source.get("actors", [])}
    gates = []
    for owner, start_closed in EXPECTED.items():
        actor = actors.get(owner)
        require(actor is not None and actor.get("name") == GATE_NAMES[owner] and
                _plain_bool(actor.get("active"), True), f"missing, renamed, or inactive gate {owner}")
        comps = actor.get("components", [])
        transforms = [c for c in comps if c.get("type") == "Transform"]
        colliders = [c for c in comps if c.get("type") == "BoxCollider2D"]
        renderers = [c for c in comps if c.get("type") == "MeshRenderer"]
        fsms = [c for c in comps if c.get("type") == "PlayMakerFSM"]
        require(len(transforms) == len(colliders) == len(renderers) == len(fsms) == 1,
                f"gate {owner}: component set changed")
        transform, collider, renderer, fsm = transforms[0], colliders[0], renderers[0], fsms[0]
        t, c = transform["data"], collider["data"]
        require(t["m_GameObject"]["m_PathID"] == owner and c["m_GameObject"]["m_PathID"] == owner and
                renderer["data"]["m_GameObject"]["m_PathID"] == owner and
                fsm["data"]["m_GameObject"]["m_PathID"] == owner,
                f"gate {owner}: component owner changed")
        require(t.get("m_Father", {}).get("m_PathID") == 0, f"gate {owner}: parent transform unsupported")
        require(type(c.get("m_Enabled")) is int and c.get("m_Enabled") == 1 and
                _plain_bool(c.get("m_IsTrigger"), False) and
                _plain_bool(renderer["data"].get("m_Enabled"), True),
                f"gate {owner}: collider must be enabled and non-trigger")
        collider_flags = {
            "m_UsedByEffector": False, "m_CompositeOperation": 0, "m_CompositeOrder": 0,
            "m_AutoTiling": False, "m_IncludeLayers": {"m_Bits": 0},
            "m_ExcludeLayers": {"m_Bits": 0}, "m_LayerOverridePriority": 0,
            "m_ForceSendLayers": {"m_Bits": 4294967295},
            "m_ForceReceiveLayers": {"m_Bits": 4294967295},
            "m_ContactCaptureLayers": {"m_Bits": 4294967295},
            "m_CallbackLayers": {"m_Bits": 4294967295},
        }
        require(all(type(c.get(k)) is type(v) and c.get(k) == v for k, v in collider_flags.items()),
                f"gate {owner}: unsupported collider flags")
        require(c.get("m_EdgeRadius") == 0, f"gate {owner}: rounded box unsupported")
        position, scale = vec3(t["m_LocalPosition"]), vec3(t["m_LocalScale"])
        require(all(math.isfinite(v) for v in position + scale) and all(v > 0 for v in scale),
                f"gate {owner}: invalid transform values")
        q = t["m_LocalRotation"]
        require(abs(q["x"]) < 1e-6 and abs(q["y"]) < 1e-6, f"gate {owner}: non-planar rotation unsupported")
        norm = math.sqrt(sum(float(q[k]) ** 2 for k in ("x", "y", "z", "w")))
        require(abs(norm - 1.0) < 1e-5, f"gate {owner}: non-unit rotation unsupported")
        angle = 2.0 * math.atan2(float(q["z"]), float(q["w"]))
        quarter = round(angle / (math.pi / 2.0))
        require(abs(angle - quarter * math.pi / 2.0) < 1e-5, f"gate {owner}: rotated AABB unsupported")
        offset = c["m_Offset"]
        cx, cy = float(offset["x"]) * scale[0], float(offset["y"]) * scale[1]
        co, si = math.cos(angle), math.sin(angle)
        center_x = position[0] + co * cx - si * cy
        center_y = position[1] + si * cx + co * cy
        half_x, half_y = float(c["m_Size"]["x"]) * scale[0] / 2, float(c["m_Size"]["y"]) * scale[1] / 2
        ex, ey = abs(co) * half_x + abs(si) * half_y, abs(si) * half_x + abs(co) * half_y
        require(ex > 0 and ey > 0 and all(math.isfinite(v) for v in (center_x, center_y, ex, ey)),
                f"gate {owner}: invalid collider bounds")
        sidecar = objects.get(owner)
        require(sidecar is not None and sidecar.get("m_Name") == GATE_NAMES[owner] and
                type(sidecar.get("m_Layer")) is int and sidecar["m_Layer"] == EXPECTED_LAYER and
                _plain_bool(sidecar.get("m_IsActive"), True),
                f"gate {owner}: sidecar name, active state, or layer changed")
        validate_fsm(fsm["decoded"], owner, start_closed)
        gates.append(dict(owner=owner, collider=collider["path_id"], layer=sidecar["m_Layer"],
                          bounds=(center_x-ex, center_y-ey, center_x+ex, center_y+ey),
                          startClosed=start_closed, position=position[:2], scale=scale[:2], rotation=angle))
    return gates


def render(gates):
    def f32(value):
        result = f"{value:.9g}"
        return result if "." in result or "e" in result.lower() else result + ".0"

    lines = ["#pragma once", "", "#include <array>", "#include <cstdint>", '#include "physics.hpp"', "",
             "namespace hk3ds::uumuu_gates_data {", "struct Gate { std::uint32_t owner, collider, layer; Rect bounds; bool startClosed; };",
             "inline constexpr std::array<Gate, 3> gates{{"]
    for gate in gates:
        l, b, r, t = gate["bounds"]
        lines.append(f"    {{{gate['owner']}u, {gate['collider']}u, {gate['layer']}u, {{{f32(l)}f, {f32(b)}f, {f32(r)}f, {f32(t)}f}}, {'true' if gate['startClosed'] else 'false'}}},")
    lines += ["}};", 'inline constexpr const char* closeClip = "Close";',
              'inline constexpr const char* closedClip = "Closed";', 'inline constexpr const char* openClip = "Open";',
              'inline constexpr const char* openedClip = "Opened";',
              'inline constexpr float openDuration = 7.0f/12.0f;', "} // namespace hk3ds::uumuu_gates_data", ""]
    visuals = ['struct Visual { Vec2 position, scale; float rotation; };',
               'inline constexpr std::array<Visual, 3> visuals{{'] + [
        '    {{%sf, %sf}, {%sf, %sf}, %sf},' % tuple(f32(v) for v in (*g['position'], *g['scale'], g['rotation']))
        for g in gates] + ['}};']
    lines[-2:-2] = visuals
    return "\n".join(lines)


def main():
    source = json.loads(SOURCE.read_text(encoding="utf-8"))
    for actor in source['actors']:
        if actor.get('name') == 'Battle Gate Archives':
            animators = [c for c in actor['components'] if c.get('type') == 'tk2dSpriteAnimator']
            clip = next(c for c in animators[0]['animation_library']['clips'] if c['name'] == 'Open')
            require(clip['fps'] == 12 and len(clip['frames']) == 7, 'gate Open duration changed')
    objects = json.loads(OBJECTS.read_text(encoding="utf-8"))
    by_path = {int(k): v for k, v in objects.items()}
    HEADER.write_text(render(convert(source, by_path)), encoding="utf-8", newline="\n")
    print(f"converted {len(EXPECTED)} Uumuu battle gates")


if __name__ == "__main__":
    main()
