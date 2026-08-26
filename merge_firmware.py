# Post-build packaging for BLSmartFlow.
#
# Produces, next to the normal PlatformIO output:
#   .firmware/BLSmartflow_V<version>.bin.ota   plain app image for OTA uploads
#   .firmware/BLSmartflow_V<version>.bin       full flash image (bootloader +
#                                              partitions + app), offset 0
#   firmware/esp32dev/BLSmartflow_<version>.bin  copy of the merged image that
#                                              firmware/manifest.json points at
#
# The manifest is what the web flasher reads, so it is rewritten here to keep
# its version and part path in step with custom_version in platformio.ini.

Import("env")

import json
import os
import shutil

# Set to False to skip producing the merged full-flash image (the OTA image is
# always produced).
ENABLE_MERGE_BIN = True

PROJECT_DIR = env.subst("$PROJECT_DIR")
BUILD_DIR = env.subst("$BUILD_DIR")
PROGNAME = env.subst("${PROGNAME}")
APP_BIN = os.path.normpath(os.path.join(BUILD_DIR, f"{PROGNAME}.bin"))
MERGED_BIN = os.path.normpath(os.path.join(BUILD_DIR, f"{PROGNAME}_merged.bin"))
BOARD_CONFIG = env.BoardConfig()

project_name = env.GetProjectOption("custom_project_name") or "Firmware"
version = env.GetProjectOption("custom_version") or "0.0.0"

firmware_path_merged = os.path.normpath(
    os.path.join(PROJECT_DIR, ".firmware", f"{project_name}_V{version}.bin")
)
firmware_path_ota = os.path.normpath(
    os.path.join(PROJECT_DIR, ".firmware", f"{project_name}_V{version}.bin.ota")
)

# Path inside the repo that firmware/manifest.json references.
RELEASE_REL_PATH = f"esp32dev/{project_name}_{version}.bin"
release_path = os.path.normpath(os.path.join(PROJECT_DIR, "firmware", RELEASE_REL_PATH))
MANIFEST = os.path.normpath(os.path.join(PROJECT_DIR, "firmware", "manifest.json"))


def copy_bin_as_ota(source, target, env):
    os.makedirs(os.path.dirname(firmware_path_ota), exist_ok=True)
    shutil.copyfile(APP_BIN, firmware_path_ota)
    print(f"Firmware (OTA, uncompressed) copied to: {firmware_path_ota}")


def update_manifest():
    """Point firmware/manifest.json at the image this build just produced."""
    try:
        with open(MANIFEST, "r", encoding="utf-8") as fh:
            manifest = json.load(fh)
    except (OSError, ValueError) as exc:
        print(f"Could not read {MANIFEST}: {exc} - skipping manifest update")
        return

    manifest["version"] = version
    builds = manifest.get("builds") or []
    if not builds:
        builds = [{"chipFamily": "ESP32", "parts": []}]
        manifest["builds"] = builds
    parts = builds[0].get("parts") or []
    if not parts:
        parts = [{}]
        builds[0]["parts"] = parts
    # The merged image starts at offset 0 because it contains the bootloader.
    parts[0]["path"] = RELEASE_REL_PATH
    parts[0]["offset"] = 0

    with open(MANIFEST, "w", encoding="utf-8") as fh:
        json.dump(manifest, fh, indent=2)
        fh.write("\n")
    print(f"Manifest updated: version {version}, part {RELEASE_REL_PATH}")


def merge_bin(source, target, env):
    flash_images = env.Flatten(env.get("FLASH_EXTRA_IMAGES", []))
    app_offset = env.subst("$ESP32_APP_OFFSET") or "0x10000"
    flash_images += [app_offset, APP_BIN]

    flash_args = [
        os.path.normpath(env.subst(str(x))) if not str(x).startswith("0x") else str(x)
        for x in flash_images
    ]

    cmd = [
        env.subst("$PYTHONEXE"),
        env.subst("$OBJCOPY"),
        "--chip", BOARD_CONFIG.get("build.mcu", "esp32"),
        "merge-bin",
        "--pad-to-size", BOARD_CONFIG.get("upload.flash_size", "4MB"),
        "--output", MERGED_BIN,
    ] + flash_args

    command_str = " ".join(
        f'"{c}"' if " " in c and not c.startswith('"') else c for c in cmd
    )
    print("Running merge-bin:\n  ", command_str)
    result = env.Execute(command_str)
    if result:
        print("merge-bin failed - manifest and release copy left untouched")
        return result

    os.makedirs(os.path.dirname(firmware_path_merged), exist_ok=True)
    shutil.copyfile(MERGED_BIN, firmware_path_merged)
    print(f"Firmware (merged) copied to: {firmware_path_merged}")

    os.makedirs(os.path.dirname(release_path), exist_ok=True)
    shutil.copyfile(MERGED_BIN, release_path)
    print(f"Firmware (merged) copied to: {release_path}")

    update_manifest()
    return result


# The OTA image is always produced.
env.AddPostAction(APP_BIN, copy_bin_as_ota)

if ENABLE_MERGE_BIN:
    env.AddPostAction(APP_BIN, merge_bin)
