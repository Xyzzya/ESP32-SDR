Import("env")
import os

"""
check_wifi.py — Hard-disable WiFi for low-noise SDR reception.

ESP-IDF 5.x Kconfig has:
    config ESP_WIFI_ENABLED
        default y if SOC_WIFI_SUPPORTED

'sdkconfig.defaults' with =n is ignored because Kconfig re-syncs the
config every build and the 'default y' clause wins.

We fix this at the source: patch the framework's Kconfig file so the
default becomes 'n'.  This survives CMake re-syncs and produces a
clean WiFi=OFF build on the first attempt.
"""

KCONFIG_MARKER = "## PATCHED by SDR check_wifi.py: WiFi default changed y->n"


def patch_framework_kconfig():
    """Change 'default y' to 'default n' in ESP-IDF's esp_wifi Kconfig."""
    framework_dir = env.PioPlatform().get_package_dir("framework-espidf")

    kconfig_path = os.path.join(
        framework_dir, "components", "esp_wifi", "Kconfig"
    )

    if not os.path.exists(kconfig_path):
        print("check_wifi.py: WARNING: esp_wifi Kconfig not found at %s" % kconfig_path)
        return False

    with open(kconfig_path, "r") as f:
        content = f.read()

    if KCONFIG_MARKER in content:
        print("check_wifi.py: Kconfig already patched (WiFi default OFF)")
        return True

    if "default y if SOC_WIFI_SUPPORTED" in content:
        content = content.replace(
            "default y if SOC_WIFI_SUPPORTED",
            "default n if SOC_WIFI_SUPPORTED  " + KCONFIG_MARKER,
        )
        with open(kconfig_path, "w") as f:
            f.write(content)
        print("check_wifi.py: PATCHED %s  (default y -> n)" % kconfig_path)
        return True

    print("check_wifi.py: Kconfig patch pattern not found (ESP-IDF version mismatch?)")
    return False


# Execute patch
patch_framework_kconfig()
