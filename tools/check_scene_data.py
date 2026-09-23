#!/usr/bin/env python3
"""Static scene metadata/dependency checks. Does not load the game or decode models."""
import argparse
from native_data import read as read_toml, SCENE_VERSION
import math
import re
from pathlib import Path
from parameter_values import cpp_number

SCENE_HEADER = "paper/scenes/scene.hpp"
RESOURCE_HEADER = "paper/resources/resource_store.hpp"
FILE_BYTES = cpp_number(RESOURCE_HEADER, "fileBytes")
ID_BYTES = cpp_number(SCENE_HEADER, "idBytes")
COORDINATE_METERS = cpp_number(SCENE_HEADER, "coordinateMeters")
HIERARCHY_DEPTH = cpp_number(SCENE_HEADER, "hierarchyDepth")
SCALE_MINIMUM = cpp_number(SCENE_HEADER, "scaleMinimum")
SCALE_MAXIMUM = cpp_number(SCENE_HEADER, "scaleMaximum")
VECTOR_COMPONENTS, QUAD_VERTICES = 3, 4


def merge(base, patch):
    result = dict(base) if isinstance(base, dict) else {}
    for key, value in patch.items():
        result[key] = merge(result.get(key), value) if isinstance(value, dict) else value
    return result


def check(root, manifest):
    root = root.resolve()
    files = set()

    def path(relative):
        p = (root / relative).resolve()
        if Path(relative).is_absolute() or root not in p.parents or not p.is_file():
            raise ValueError(f"missing dependency or path outside assets: {relative}")
        if p.stat().st_size > FILE_BYTES:
            raise ValueError(f"file exceeds 64 MiB: {relative}")
        files.add(p)
        return p

    def read(relative, field):
        if Path(relative).suffix != '.dc' + field:
            raise ValueError(f"expected .dc{field} document: {relative}")
        data = read_toml(path(relative))
        if set(data) != {"format", "version", field} or data["format"] != "dcmo." + field:
            raise ValueError(f"invalid native envelope: {relative}")
        if type(data["version"]) is not int or data["version"] != SCENE_VERSION:
            raise ValueError(f"unsupported version: {relative}")
        if not isinstance(data[field], dict) or "version" in data[field]:
            raise ValueError(f"invalid native payload: {relative}")
        return dict(data[field], version=SCENE_VERSION)

    def identifier(value):
        if not isinstance(value, str) or len(value.encode("utf-8")) > ID_BYTES or not re.fullmatch(r"[A-Za-z0-9_./-]+", value):
            raise ValueError(f"invalid ID: {value!r}")
        return value

    def vector(value, positive=False):
        if not isinstance(value, list) or len(value) != VECTOR_COMPONENTS or any(
            type(v) not in (float, int) or not math.isfinite(v) or abs(v) > COORDINATE_METERS
            or (positive and v <= 0) for v in value
        ):
            raise ValueError(f"invalid vector: {value!r}")

    def box(value):
        vector(value["center"])
        vector(value["half"], True)

    m = read(manifest, "world")
    resources, templates, nodes, rooms, spawns, scenes, ids = {}, {}, {}, {}, {}, set(), set()
    for filename in m["resources"]:
        for resource in read(filename, "resources")["resources"]:
            rid = identifier(resource["id"])
            if rid in resources:
                raise ValueError(f"duplicate resource: {rid}")
            resources[rid] = resource
            if resource["type"] not in ("procedural", "sprite", "gltf"):
                raise ValueError(f"unknown resource type: {rid}")
            for field in ("path", "altered"):
                if field in resource:
                    path(resource[field])
            for primitive in resource.get("primitives", []):
                if "box" in primitive:
                    box(primitive["box"])
                if "quad" in primitive:
                    if len(primitive["quad"]) != QUAD_VERTICES:
                        raise ValueError(f"invalid quad in {rid}")
                    for point in primitive["quad"]:
                        vector(point)
    for filename in m["templates"]:
        for template in read(filename, "templates")["templates"]:
            tid = identifier(template["id"])
            if tid in templates:
                raise ValueError(f"duplicate template: {tid}")
            templates[tid] = template
    zones = []
    for filename in m["scenes"]:
        scene = read(filename, "scene")
        sid = identifier(scene["id"])
        if sid in scenes:
            raise ValueError(f"duplicate scene: {sid}")
        scenes.add(sid)
        for collection in ("nodes", "rooms", "spawns", "zones", "lights"):
            for item in scene.get(collection, []):
                iid = identifier(item["id"])
                if iid in ids:
                    raise ValueError(f"duplicate scene element: {iid}")
                ids.add(iid)
                if "position" in item:
                    vector(item["position"])
                if "bounds" in item:
                    box(item["bounds"])
                if collection == "nodes":
                    base = templates[item["template"]] if "template" in item else {}
                    overrides = dict(item)
                    base = dict(base)
                    removed = overrides.pop("remove", [])
                    if not isinstance(removed, list):
                        raise ValueError(f"remove must be an array: {iid}")
                    for field in removed:
                        if not isinstance(field, str) or field in {"id", "template", "remove"} or field not in base or field in overrides:
                            raise ValueError(f"invalid inherited field removal in {iid}: {field}")
                        del base[field]
                    nodes[iid] = (sid, merge(base, overrides))
                if collection == "rooms":
                    rooms[iid] = sid
                if collection == "spawns":
                    spawns[iid] = sid
                if collection == "zones":
                    zones.append(item)
    for nid, (sid, node) in nodes.items():
        if "room" in node and rooms.get(node["room"]) != sid:
            raise ValueError(f"unknown room in {nid}")
        if "visibleWhen" in node and node["visibleWhen"] not in nodes:
            raise ValueError(f"unknown visibility dependency in {nid}")
        current, seen = nid, set()
        while "parent" in nodes[current][1]:
            if current in seen or len(seen) >= HIERARCHY_DEPTH:
                raise ValueError(f"cyclic/deep hierarchy: {nid}")
            seen.add(current)
            current = nodes[current][1]["parent"]
            if current not in nodes or nodes[current][0] != sid:
                raise ValueError(f"unknown parent in scene: {nid}")
        for field in ("resource", "activeResource", "inspectResource"):
            if field in node and node[field] not in resources:
                raise ValueError(f"unknown resource in {nid}: {node[field]}")
        for pose in node.get("poses", []):
            if pose not in resources:
                raise ValueError(f"unknown pose: {pose}")
        if not SCALE_MINIMUM <= node.get("scale", 1) <= SCALE_MAXIMUM:
            raise ValueError(f"invalid scale: {nid}")
    if m["entrySpawn"] not in spawns:
        raise ValueError("unknown entry spawn")
    for zone in zones:
        if zone.get("targetSpawn") and zone["targetSpawn"] not in spawns:
            raise ValueError(f"unknown target spawn: {zone['id']}")
    print(f"Scene metadata OK: {len(scenes)} scenes, {len(nodes)} nodes, "
          f"{len(templates)} templates, {len(resources)} resources, {len(files)} source files")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1] / "examples/boxes/assets")
    parser.add_argument("--manifest", default="world.dcworld")
    args = parser.parse_args()
    try:
        check(args.root, args.manifest)
    except (ValueError, KeyError, TypeError, OSError) as error:
        parser.exit(1, f"Scene metadata error: {error}\n")
