import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
WIFI_SOURCE = (ROOT / "components/voicelife_runtime/src/linx_ota_bootstrap.cc").read_text()
IM_SOURCE = (ROOT / "components/voicelife_runtime/src/im_runtime_bootstrap.cc").read_text()
RUNTIME_SOURCE = (ROOT / "components/voicelife_runtime/src/runtime.cc").read_text()
SERIAL_VOICE_SOURCE = (ROOT / "components/voicelife_runtime/src/serial_voice_test.cc").read_text()
USB_ROUTER_SOURCE = (ROOT / "components/voicelife_runtime/src/usb_serial_frame_router.cc").read_text()


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

    def test_runtime_usb_input_has_one_driver_reader_and_protocol_specific_consumers(self):
        self.assertIn("usb_serial_jtag_read_bytes", USB_ROUTER_SOURCE)
        self.assertIn("ReceiveImUsbSerialFrame", IM_SOURCE)
        self.assertIn("ReceiveSerialVoiceUsbFrame", SERIAL_VOICE_SOURCE)
        self.assertNotIn("usb_serial_jtag_read_bytes", IM_SOURCE)
        self.assertNotIn("usb_serial_jtag_read_bytes", SERIAL_VOICE_SOURCE)

    def test_im_usb_provisioning_starts_after_wifi_bootstrap_releases_console(self):
        startup = RUNTIME_SOURCE[
            RUNTIME_SOURCE.index("Status Start(PlatformAssembly& assembly)") : RUNTIME_SOURCE.index(
                "void StopEventLoop()"
            )
        ]
        self.assertLess(startup.index("BootstrapLinxOtaConfig("), startup.index("StartImProvisioningTask()"))

    def test_audio_dma_is_prepared_before_network_and_im_startup(self):
        startup = RUNTIME_SOURCE[
            RUNTIME_SOURCE.index("Status Start(PlatformAssembly& assembly)") : RUNTIME_SOURCE.index(
                "void StopEventLoop()"
            )
        ]
        self.assertIn('ESP_LOGI(kTag, "AUDIO_PREPARED=1")', startup)
        self.assertLess(startup.index('ESP_LOGI(kTag, "AUDIO_PREPARED=1")'), startup.index("BootstrapLinxOtaConfig("))
        self.assertLess(startup.index("ReserveImRuntimeTask();"), startup.index("BootstrapLinxOtaConfig("))
        self.assertLess(startup.index("session_->Start(config)"), startup.index("StartImRuntime();"))
        self.assertIn("xTaskCreate(&Runtime::ImLifecycleTaskEntry", RUNTIME_SOURCE)
        self.assertNotIn("xTaskCreateWithCaps(&Runtime::ImLifecycleTaskEntry", RUNTIME_SOURCE)
        self.assertIn("ulTaskNotifyTake(pdTRUE, portMAX_DELAY)", RUNTIME_SOURCE)
        self.assertIn("xTaskNotifyGive(im_lifecycle_task_)", RUNTIME_SOURCE)

    def test_reminder_action_worker_reserves_an_internal_stack_before_storage(self):
        startup = RUNTIME_SOURCE[
            RUNTIME_SOURCE.index("Status Start(PlatformAssembly& assembly)") : RUNTIME_SOURCE.index(
                "void StopEventLoop()"
            )
        ]
        worker = RUNTIME_SOURCE[
            RUNTIME_SOURCE.index("bool StartReminderActionWorker()") : RUNTIME_SOURCE.index(
                "static void ReminderActionTaskEntry"
            )
        ]
        self.assertLess(startup.index("StartReminderActionWorker()"), startup.index("storage_.Start()"))
        self.assertIn("xTaskCreateWithCaps(&Runtime::ReminderActionTaskEntry", worker)
        self.assertIn("MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT", worker)
        self.assertIn("kReminderActionWorkerStackBytes = 16 * 1024", worker)
        self.assertNotIn("MALLOC_CAP_SPIRAM", worker)

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
