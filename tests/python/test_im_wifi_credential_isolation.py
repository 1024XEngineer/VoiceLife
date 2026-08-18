import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
ESP_RUNTIME_ROOT = ROOT / "components/voicelife_runtime_esp/src"
WIFI_SOURCE = (ESP_RUNTIME_ROOT / "linx_ota_bootstrap.cc").read_text()
IM_SOURCE = (ESP_RUNTIME_ROOT / "im_runtime_bootstrap.cc").read_text()
RUNTIME_SOURCE = (ESP_RUNTIME_ROOT / "esp_runtime.cc").read_text()


class ImWifiCredentialIsolationTest(unittest.TestCase):
    def test_wifi_and_im_use_distinct_encrypted_nvs_namespaces(self):
        wifi = re.search(r'kWifiNamespace\[\] = "([^"]+)"', WIFI_SOURCE)
        im = re.search(r'kImNamespace\[\] = "([^"]+)"', IM_SOURCE)
        self.assertIsNotNone(wifi)
        self.assertIsNotNone(im)
        self.assertNotEqual(wifi.group(1), im.group(1))
        self.assertEqual(wifi.group(1), "wifi")
        self.assertEqual(im.group(1), "im")

    def test_network_provisioning_cannot_overwrite_or_erase_im_credentials(self):
        wifi_storage = WIFI_SOURCE[
            WIFI_SOURCE.index("Status StoreWifiCredentials") : WIFI_SOURCE.index(
                "Result<WifiCredentials> LoadWifiCredentials"
            )
        ]
        for im_key in ("gateway_origin", "device_id", "device_token", "user_id", "kImNamespace"):
            self.assertNotIn(im_key, wifi_storage)
        self.assertNotIn("nvs_erase_all", wifi_storage)

    def test_usb_serial_jtag_reads_use_the_driver_api_without_fcntl(self):
        read_function = IM_SOURCE[
            IM_SOURCE.index("bool ReadConsoleBytes") : IM_SOURCE.index("Status StoreProvisioningRequest")
        ]
        usb_branch = read_function.split("#else", 1)[0]
        self.assertIn("usb_serial_jtag_read_bytes", usb_branch)
        self.assertNotIn("fcntl", usb_branch)

    def test_im_usb_provisioning_starts_before_wifi_bootstrap_can_fail(self):
        startup = RUNTIME_SOURCE[
            RUNTIME_SOURCE.index("Status Runtime::Start(PlatformAssembly& assembly)") : RUNTIME_SOURCE.index(
                "Status Runtime::RequestInterrupt()"
            )
        ]
        self.assertLess(startup.index("StartImProvisioningTask()"), startup.index("BootstrapLinxOtaConfig("))

    def test_im_provisioning_writes_all_four_credentials_only_in_im_namespace(self):
        im_storage = IM_SOURCE[
            IM_SOURCE.index("Status StoreProvisioningRequest") : IM_SOURCE.index(
                "ConsoleCommandResult ReadImConsoleCommand"
            )
        ]
        for im_key in ("gateway_origin", "device_id", "device_token", "user_id", "kImNamespace"):
            self.assertIn(im_key, im_storage)
        self.assertNotIn("kWifiNamespace", im_storage)


if __name__ == "__main__":
    unittest.main()
