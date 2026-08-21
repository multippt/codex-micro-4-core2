"""Allow repeated characteristic UUIDs in Arduino-ESP32's NimBLE service map.

HOGP identifies each Report characteristic with a 0x2908 Report Reference
descriptor, so input and output reports intentionally share UUID 0x2A4D. The
Arduino-ESP32 3.3.9 compatibility BLE library suppresses the second UUID under
NimBLE, leaving Windows without an output endpoint. Patch the pinned framework
source before PlatformIO builds that library. The replacement is idempotent and
fails loudly if the pinned source no longer matches either known form.
"""

from pathlib import Path
from SCons.Script import COMMAND_LINE_TARGETS

Import("env")  # type: ignore[name-defined]  # Provided by PlatformIO/SCons.


original = """#if defined(CONFIG_NIMBLE_ENABLED)
  if (pExisting != nullptr) {
    pExisting->m_removed = 0;
  } else
#endif
  {
    // Remember this characteristic in our map of characteristics.  At this point, we can lookup by UUID
    // but not by handle.  The handle is allocated to us on the ESP_GATTS_ADD_CHAR_EVT.
    m_characteristicMap.setByUUID(pCharacteristic, pCharacteristic->getUUID());
  }
"""

patched = """  // HOGP Report characteristics intentionally reuse UUID 0x2A4D; their
  // Report Reference descriptors distinguish input, output, and feature reports.
  // Store every object instead of suppressing repeated UUIDs under NimBLE.
  m_characteristicMap.setByUUID(pCharacteristic, pCharacteristic->getUUID());
"""

def apply_patch() -> None:
    framework_package = env.PioPlatform().get_package_dir("framework-arduinoespressif32")
    if framework_package is None:
        raise RuntimeError("The pinned Tab5 Arduino framework package is not installed")

    service_source = (
        Path(framework_package) / "libraries" / "BLE" / "src" / "BLEService.cpp"
    )
    source = service_source.read_text(encoding="utf-8")
    if patched in source:
        print("Tab5 NimBLE duplicate-characteristic patch already applied")
    elif original in source:
        service_source.write_text(source.replace(original, patched, 1), encoding="utf-8")
        print("Applied Tab5 NimBLE duplicate-characteristic patch")
    else:
        raise RuntimeError(
            f"Pinned BLEService.cpp does not match the expected source: {service_source}"
        )


if env.GetOption("clean") or "clean" in COMMAND_LINE_TARGETS:
    print("Skipping Tab5 NimBLE patch while cleaning")
else:
    apply_patch()
