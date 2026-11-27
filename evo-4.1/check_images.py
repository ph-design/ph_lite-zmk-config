#!/usr/bin/env python3
"""Compare two matching Evo west build directories (or their zephyr/ directories).

Run with the Zephyr build Python (pyelftools) and Zephyr's python-devicetree.
The latter can be located through --zephyr-base, ZEPHYR_BASE, or CMakeCache.txt.
Each invocation checks one variant: default keyboard, radio keyboard, or receiver.
The output describes static evidence. A settings-table ID is assigned on the device.

Example: python3 evo-4.1/check_images.py build-3.5-default build/evo-4.1-default
"""

import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import struct
import sys

try:
    from elftools.elf.elffile import ELFFile
except ImportError:
    sys.exit("Use a Zephyr build Python with pyelftools installed.")


FAMILY = 0xADA52840
PARTITIONS = {
    "code": (0x26000, 0xC6000),
    "storage": (0xEC000, 0x8000),
    "boot": (0xF4000, 0xC000),
}
# User-visible identity, persisted layout, transport selection, runtime features.
CONFIG_KEYS = """
    BOARD SOC_NRF52840 USB_DEVICE_VID USB_DEVICE_PID USB_DEVICE_PRODUCT
    USB_DEVICE_MANUFACTURER ZMK_KEYBOARD_NAME ZMK_DEVICE_FAMILY ZMK_DEVICE_MANUFACTURER ZMK_DEVICE_BOARD_ID
    BT_DEVICE_NAME BT_MAX_PAIRED BT_ID_MAX BT_SETTINGS BT_SIGNING
    BT_SMP_SC_PAIR_ONLY BT_KEYS_OVERWRITE_OLDEST BT_DIS_PNP_VID BT_DIS_PNP_PID BT_DIS_PNP_VID_SRC
    ZMK_USB ZMK_BLE ZMK_HID_REPORT_TYPE_NKRO ZMK_KEYMAP_SETTINGS_STORAGE
    ZMK_KEYMAP_LAYER_REORDERING ZMK_KEYMAP_LAYER_NAME_MAX_LEN
    ZMK_BEHAVIOR_LOCAL_IDS ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS
    ZMK_BEHAVIOR_LOCAL_ID_TYPE_SETTINGS_TABLE ZMK_BEHAVIOR_LOCAL_ID_TYPE_CRC16
    ZMK_2G4 ZMK_2G4_DONGLE ZMK_STUDIO ZMK_STUDIO_LOCKING
    ZMK_STUDIO_TRANSPORT_BLE ZMK_STUDIO_TRANSPORT_UART ZMK_STUDIO_TRANSPORT_2G4
    ZMK_ESB ZMK_2G4_DONGLE_EXTERNAL ZMK_2G4_RF_CHANNEL ZMK_2G4_TX_POWER ZMK_2G4_ADDR_PREFIX
    ZMK_2G4_AES_KEY
    ZMK_2G4_ADDR_BASE_0 ZMK_2G4_ADDR_BASE_1 ZMK_2G4_ADDR_BASE_2 ZMK_2G4_ADDR_BASE_3
    ZMK_RGB_UNDERGLOW ZMK_RGB_UNDERGLOW_FADE ZMK_RGB_UNDERGLOW_FADE_MS
    EXPERIMENTAL_RGB_LAYER ZMK_HID_INDICATORS ZMK_MOTION
    ZMK_POINTING ZMK_PM_SOFT_OFF ZMK_SLEEP ZMK_UNRESPONSE_FIX
    SETTINGS SETTINGS_NVS SETTINGS_NVS_SECTOR_COUNT SETTINGS_NVS_SECTOR_SIZE_MULT
    NVS_DATA_CRC FLASH_BASE_ADDRESS FLASH_LOAD_OFFSET FLASH_LOAD_SIZE
    ZMK_SETTINGS_RESET_ON_START ZMK_BLE_CLEAR_BONDS_ON_START
""".split()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def build_path(path):
    return path if (path / ".config").is_file() else path / "zephyr"


def dt_library(base, roots):
    locations = [base, os.getenv("ZEPHYR_BASE")]
    for root in roots:
        cache = root.parent / "CMakeCache.txt"
        if cache.is_file():
            for line in cache.read_text().splitlines():
                if line.startswith("ZEPHYR_BASE:PATH="):
                    locations.append(line.split("=", 1)[1])
    for location in locations:
        if location:
            src = Path(location) / "scripts/dts/python-devicetree/src"
            if (src / "devicetree/dtlib.py").is_file():
                sys.path.insert(0, str(src))
                break
    try:
        from devicetree import dtlib
        return dtlib
    except ImportError as exc:
        raise ValueError("Supply --zephyr-base pointing at a Zephyr source tree for dtlib") from exc


def settings(path):
    result = {}
    for line in path.read_text().splitlines():
        if line.startswith("CONFIG_"):
            key, value = line[7:].split("=", 1)
            if value.startswith('"'):
                value = json.loads(value)
            elif value not in ("y", "m"):
                value = int(value, 0)
            result[key] = value
    for key, old in (("BT_DIS_MODEL_NUMBER_STR", "BT_DIS_MODEL"),
                     ("BT_DIS_MANUF_NAME_STR", "BT_DIS_MANUF")):
        result[key] = result.get(key, result.get(old, "n"))
    return result


def bindings(prop, dt, cells_property):
    """Convert phandle cells into stable target paths and arguments."""
    values = struct.iter_unpack(">I", prop.value)
    result = []
    for (phandle,) in values:
        node = dt.phandle2node[phandle]
        count = node.props[cells_property].to_num()
        args = tuple(next(values)[0] for _ in range(count))
        result.append((node.path, args))
    return result


def devicetree(dtlib, path, keyboard, config):
    try:
        dt = dtlib.DT(str(path))
    except dtlib.DTError as exc:
        raise ValueError(f"{path}: invalid generated devicetree: {exc}") from exc
    chosen = dt.get_node("/chosen").props
    code = chosen["zephyr,code-partition"].to_path()
    parts = {"code": tuple(code.props["reg"].to_nums())}
    storage = (chosen["zephyr,settings-partition"].to_path()
               if "zephyr,settings-partition" in chosen else dt.label2node["storage_partition"])
    parts["storage"] = tuple(storage.props["reg"].to_nums())
    parts["boot"] = tuple(dt.label2node["boot_partition"].props["reg"].to_nums())
    require(parts == PARTITIONS, f"{path}: unexpected flash partitions {parts}")
    info = {"partitions": parts}
    if keyboard:
        boot_behavior = dt.label2node["bootloader"]
        if "zephyr,boot-mode" in chosen:
            require(config.get("RETENTION_BOOT_MODE") == "y" and
                    config.get("ZMK_BOOTMODE_TO_MAGIC_VALUE_MAPPER") == "y",
                    f"{path}: retained bootloader selection is not enabled")
            backing = chosen["zmk,magic-boot-mode"].to_path()
            require("bootloader" in boot_behavior.props and
                    backing.parent.props["reg"].to_nums() == [0x4000051C, 1] and
                    backing.props["reg"].to_nums() == [0, 1],
                    f"{path}: bootloader behavior does not target GPREGRET1")
            info["bootloader_magic"] = config["ZMK_BOOTMODE_BOOTLOADER_MAGIC_VALUE"]
        else:
            info["bootloader_magic"] = boot_behavior.props["type"].to_num()
        require(info["bootloader_magic"] == 0x57, f"{path}: unexpected UF2 bootloader request")
        layout = chosen["zmk,physical-layout"].to_path()
        transform = layout.props["transform"].to_node()
        info["matrix"] = {p: transform.props[p].to_nums() for p in ("rows", "columns", "map")}
        info["geometry"] = bindings(layout.props["keys"], dt, "#key-cells")
        info["layers"] = []
        for node in dt.get_node("/keymap").nodes.values():
            props = node.props
            status = props["status"].to_string() if "status" in props else "okay"
            if config.get("ZMK_STUDIO") == "y" or status == "okay":
                keys = bindings(props["bindings"], dt, "#binding-cells") if "bindings" in props else []
                info["layers"].append((node.name, status, keys))
        combos = dt.get_node("/combos").nodes.values()
        # Product combo indices sort by the number of positions, then DTS declaration order.
        info["combos"] = sorted(
            [(n.name, tuple(n.props["key-positions"].to_nums())) for n in combos],
            key=lambda combo: len(combo[1]),
        )
    return info


def uf2(path):
    data = path.read_bytes()
    require(data and len(data) % 512 == 0, f"{path}: incomplete UF2 blocks")
    count = len(data) // 512
    start, capacity = PARTITIONS["code"]
    flash = bytearray(capacity)
    present = bytearray(capacity)
    ids = set()
    low, high = start + capacity, start
    for pos in range(0, len(data), 512):
        block = data[pos:pos + 512]
        m0, m1, flags, addr, length, index, total, family = struct.unpack_from("<8I", block)
        tail, = struct.unpack_from("<I", block, 508)
        require((m0, m1, tail) == (0x0A324655, 0x9E5D5157, 0x0AB16F30),
                f"{path}: corrupt UF2 header at {pos}")
        require(flags == 0x2000 and family == FAMILY,
                f"{path}: unexpected flags/family {flags:#x}/{family:#x}")
        require(total == count and index < count and index not in ids, f"{path}: invalid block index {index}")
        ids.add(index)
        offset = addr - start
        require(0 < length <= 476 and offset >= 0 and offset + length <= capacity,
                f"{path}: UF2 write outside application partition at {addr:#x} with {length} bytes")
        region = slice(offset, offset + length)
        require(not any(present[region]), f"{path}: overlapping UF2 payload at {addr:#x}")
        flash[region] = block[32:32 + length]
        present[region] = bytes([1]) * length
        low, high = min(low, addr), max(high, addr + length)
    require(low == start, f"{path}: wrong start address {low:#x}")
    return flash, present, {"family": hex(FAMILY), "start": hex(low), "end_exclusive": hex(high),
                            "payload_bytes": sum(present), "free_after_image": start + capacity - high}


def crc16_ansi(name):
    crc = 0xFFFF
    for byte in name.encode():
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0xA001 if crc & 1 else 0)
    return crc


def storage_layouts(elf):
    """Read field offsets for persistent records from the compiled types, when available."""
    targets = {
        b"bluetooth/host/keys.c": b"bt_keys",
        b"app/src/ble.c": b"zmk_ble_profile",
        b"app/src/keymap.c": b"zmk_behavior_binding_setting",
        b"app/src/combo.c": b"combo_settings_blob",
        b"app/src/2g4_crypto.c": b"pairing_record",
    }
    layouts = {}
    if not elf.has_dwarf_info():
        return layouts
    for unit in elf.get_dwarf_info().iter_CUs():
        name = unit.get_top_DIE().attributes.get("DW_AT_name")
        target = next((kind for suffix, kind in targets.items()
                       if name and name.value.endswith(suffix)), None)
        if not target:
            continue
        for die in unit.iter_DIEs():
            attrs = die.attributes
            if (die.tag != "DW_TAG_structure_type" or "DW_AT_byte_size" not in attrs
                    or getattr(attrs.get("DW_AT_name"), "value", b"") != target):
                continue
            members = {
                child.attributes["DW_AT_name"].value.decode():
                    child.attributes["DW_AT_data_member_location"].value
                for child in die.iter_children() if child.tag == "DW_TAG_member"
            }
            # bt_keys_store serializes the tail starting at storage_start.
            start = members["storage_start"] if target == b"bt_keys" else 0
            layouts[target.decode()] = {
                "record_bytes": attrs["DW_AT_byte_size"].value - start,
                "field_offsets": {k: v - start for k, v in members.items()
                                  if v >= start and k != "storage_start"},
            }
            break
    return layouts


def host_interfaces(elf, symtab, read_at):
    descriptors = {
        hashlib.sha256(read_at(symbol["st_value"])[:symbol["st_size"]]).hexdigest()
        for symbol in symtab.get_symbol_by_name("zmk_hid_report_desc") or []
    }
    gatt = elf.get_section_by_name("bt_gatt_service_static_area")
    services = []
    if gatt:
        def uuid_at(address):
            data = read_at(address)
            if data[0] == 0:
                return f"{struct.unpack_from('<H', data, 2)[0]:04x}"
            require(data[0] == 2, f"unexpected GATT UUID type {data[0]}")
            return data[1:17][::-1].hex()

        # Both ARM32 ABIs use two words per static service, five per attribute.
        # Zephyr assigns static ATT handles from 1 in linker order.
        handle = 1
        for attrs, count in struct.iter_unpack("<II", gatt.data()):
            entries = []
            for index in range(count):
                uuid, _, _, user, _, _ = struct.unpack_from("<IIIIHH", read_at(attrs + 20 * index))
                attr = uuid_at(uuid)
                detail = None
                if attr == "2800":  # Primary service
                    detail = uuid_at(user)
                elif attr == "2803":  # Characteristic declaration, UUID and properties
                    value_uuid, _, properties = struct.unpack_from("<IHB", read_at(user))
                    detail = [uuid_at(value_uuid), properties]
                elif attr == "2908":  # HID report reference, report ID and type
                    detail = read_at(user)[:2].hex()
                entries.append([attr, detail])
            digest = hashlib.sha256(json.dumps(entries).encode()).hexdigest()
            services.append({"uuid": entries[0][1], "first_handle": handle,
                             "attributes": count, "sha256": digest})
            handle += count
    return {"hid_report_descriptor_sha256": sorted(descriptors), "gatt_services": services}


def elf_info(path, flash, present, config):
    with path.open("rb") as stream:
        elf = ELFFile(stream)
        require(elf.elfclass == 32 and elf.little_endian and elf["e_machine"] == "EM_ARM",
                f"{path}: expected a little-endian ARM32 ELF")
        segments = [s for s in elf.iter_segments() if s["p_type"] == "PT_LOAD" and s["p_filesz"]]
        chunks = [(s["p_vaddr"], s.data()) for s in segments]
        def read_at(addr):
            for base, payload in chunks:
                if base <= addr < base + len(payload):
                    return payload[addr - base:]
            raise ValueError(f"{path}: unmapped pointer {addr:#x}")

        # Ignore linker and file alignment gaps. Compare every file-backed allocated section.
        sections = 0
        for section in elf.iter_sections():
            if not section["sh_flags"] & 2 or section["sh_type"] == "SHT_NOBITS" or not section["sh_size"]:
                continue
            parent = next((s for s in segments if s.section_in_segment(section)), None)
            require(parent is not None, f"{path}: no load segment for {section.name}")
            offset = parent["p_paddr"] + section["sh_addr"] - parent["p_vaddr"] - PARTITIONS["code"][0]
            end = offset + section["sh_size"]
            require(offset >= 0 and end <= len(flash) and all(present[offset:end])
                    and flash[offset:end] == section.data(), f"{path}: UF2 differs from ELF in {section.name}")
            sections += 1
        require(sections > 0, f"{path}: empty application")
        layouts = storage_layouts(elf)
        symtab = elf.get_section_by_name(".symtab")
        require(symtab is not None, f"{path}: unstripped ELF required")
        interfaces = host_interfaces(elf, symtab, read_at)
        symbols = {s.name: s["st_value"] for s in symtab.iter_symbols()}
        for key, symbol in (("ZMK_2G4", "zmk_2g4_send_keyboard_report"),
                            ("ZMK_2G4_DONGLE", "zmk_2g4_dongle_start")):
            require((config.get(key) == "y") == (symbol in symbols),
                    f"{path}: CONFIG_{key} does not match the linked image")
        if config.get("ZMK_BEHAVIOR_LOCAL_IDS") != "y":
            return sections, [], layouts, interfaces
        if config.get("ZMK_BEHAVIOR_LOCAL_ID_TYPE_SETTINGS_TABLE") == "y":
            require({"behavior_handle_set", "behavior_handle_commit"} <= symbols.keys(),
                    f"{path}: behavior settings callbacks missing")
        start = symbols["_zmk_behavior_local_id_map_list_start"]
        end = symbols["_zmk_behavior_local_id_map_list_end"]
        require(0 < end - start and (end - start) % 8 == 0, f"{path}: invalid behavior ID map")
        names = []
        for addr in range(start, end, 8):
            device, = struct.unpack_from("<I", read_at(addr))
            nameptr, = struct.unpack_from("<I", read_at(device))
            namebytes, sep, _ = read_at(nameptr)[:128].partition(b"\0")
            require(namebytes and sep, f"{path}: invalid behavior name at {nameptr:#x}")
            names.append(namebytes.decode("utf-8"))
        duplicates = [name for name, count in Counter(names).items() if count > 1]
        require(not duplicates, f"{path}: ambiguous behavior names {duplicates}")
        return sections, names, layouts, interfaces


def inspect(root, dtlib):
    conf = settings(root / ".config")
    board = conf.get("BOARD", "").split("/")[0]
    require(board in ("ph_lite", "ph_lite_evo", "ph_lite_receiver"),
            f"{root}: not an Evo or receiver board ({board})")
    keyboard = board != "ph_lite_receiver"
    require(not keyboard or conf.get("SHIELD_PH60_SC_V2_EVO") == "y", f"{root}: expected the Evo shield")
    conf.setdefault("ZMK_DEVICE_BOARD_ID", conf["BOARD"])
    conf["BOARD"] = "ph_lite_evo" if keyboard else board
    require(conf.get("SOC_NRF52840") == "y" and conf.get("BUILD_OUTPUT_UF2") == "y",
            f"{root}: expected an nRF52840 UF2 build")
    info = devicetree(dtlib, root / "zephyr.dts", keyboard, conf)
    flash, present, image = uf2(root / "zmk.uf2")
    sections, names, layouts, interfaces = elf_info(root / "zmk.elf", flash, present, conf)
    mode = "disabled"
    if names:
        if conf.get("ZMK_BEHAVIOR_LOCAL_ID_TYPE_SETTINGS_TABLE") == "y":
            mode = "settings table"
            require(all(len(n.encode()) <= 31 for n in names),
                    f"{root}: behavior name exceeds persisted buffer")
        else:
            require(conf.get("ZMK_BEHAVIOR_LOCAL_ID_TYPE_CRC16") == "y", f"{root}: unknown local ID scheme")
            mode = "CRC16"
            ids = [crc16_ansi(n) for n in names]
            require(len(set(ids)) == len(ids) and 0xFFFF not in ids, f"{root}: collision or sentinel CRC16")
    keys = CONFIG_KEYS + ["BT_DIS_MODEL_NUMBER_STR", "BT_DIS_MANUF_NAME_STR"]
    compare_config = {key: conf.get(key, "n") for key in keys}
    info["config"] = compare_config
    image.update({
        "build_board": board, "elf_sections_matching_uf2": sections,
        "registered_behaviors": len(names), "id_scheme": mode,
        "compiled_settings_records": layouts, **interfaces,
    })
    return info, image, names


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path, help="matching product 3.5 west build")
    parser.add_argument("candidate", type=Path, help="ported 4.1 west build of the same SKU and config")
    parser.add_argument("--zephyr-base", type=Path, help="Zephyr source tree containing dtlib")
    args = parser.parse_args()
    roots = [build_path(args.baseline), build_path(args.candidate)]
    lib = dt_library(args.zephyr_base, roots)
    old, old_image, old_names = inspect(roots[0], lib)
    new, new_image, new_names = inspect(roots[1], lib)
    errors = []
    for key in sorted(old.keys() | new.keys()):
        if key == "config":
            for name in old[key]:
                if old[key][name] != new[key][name]:
                    if name == "ZMK_2G4_AES_KEY":
                        errors.append("CONFIG_ZMK_2G4_AES_KEY: factory pairing key differs")
                    else:
                        errors.append(f"CONFIG_{name}: {old[key][name]!r} -> {new[key][name]!r}")
        elif old.get(key) != new.get(key):
            errors.append(f"{key}: product devicetree differs")
    missing = sorted(set(old_names) - set(new_names))
    if missing:
        errors.append(f"unresolvable saved behaviors: {', '.join(missing)}")
    old_layouts = old_image["compiled_settings_records"]
    new_layouts = new_image["compiled_settings_records"]
    for key in ("hid_report_descriptor_sha256", "gatt_services"):
        if old_image[key] != new_image[key]:
            errors.append(f"compiled {key} differs")
    for name in sorted(old_layouts.keys() & new_layouts.keys()):
        if old_layouts[name] != new_layouts[name]:
            errors.append(f"compiled {name} record layout differs")
    evidence = {
        "baseline": old_image, "candidate": new_image,
        "board": old["config"]["BOARD"],
        "keyboard_radio": old["config"]["ZMK_2G4"], "receiver": old["config"]["ZMK_2G4_DONGLE"],
        "studio_locking": old["config"]["ZMK_STUDIO_LOCKING"],
        "rgb_fade": new["config"]["ZMK_RGB_UNDERGLOW_FADE"],
        "studio_board_id": new["config"]["ZMK_DEVICE_BOARD_ID"],
        "storage": {k: [hex(x) for x in v] for k, v in new["partitions"].items()},
        "bootloader_magic": hex(new["bootloader_magic"]) if "bootloader_magic" in new else None,
        "config_fields_compared": len(old["config"]),
        "keys": len(new.get("geometry", [])), "layers": len(new.get("layers", [])),
        "combo_slots": len(new.get("combos", [])),
        "retained_behavior_names": sorted(set(old_names) & set(new_names)),
        "added_behavior_names": sorted(set(new_names) - set(old_names)),
        "record_types_compared": sorted(old_layouts.keys() & new_layouts.keys()),
        "record_types_with_one_sided_debug_info": sorted(old_layouts.keys() ^ new_layouts.keys()),
        "differences": errors,
    }
    if old_image["id_scheme"] == new_image["id_scheme"] == "settings table":
        evidence["id_evidence"] = ("Saved behavior/local_id/<id> records match device names. "
                                   "Actual assigned IDs require the device's settings.")
        evidence["linker_registration_order_changed"] = old_names != new_names
    if new_image["id_scheme"] == "CRC16":
        evidence["crc16"] = {name: crc16_ansi(name) for name in sorted(new_names)}
    print(json.dumps(evidence, indent=2))
    return int(bool(errors))


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, KeyError, OSError, StopIteration, struct.error) as exc:
        sys.exit(f"Verification failed: {exc}")
