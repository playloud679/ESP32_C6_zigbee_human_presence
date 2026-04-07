# Changelog

## Current

- Added Telegram notifications for real `PRESENT` and `CLEAR` state changes.
- Added persistent NVS storage for Wi-Fi credentials, bot token, and chat ID.
- Added CLI commands for Telegram setup and diagnostics:
  `tg_status`, `tg_scan`, `tg_wifi`, `tg_wifi_pick`, `tg_chat`, `tg_test`, `tg_reset`.
- Enabled explicit `Wi-Fi + Zigbee` coexistence on `ESP32-C6` with
  `esp_coex_wifi_i154_enable()`.
- Switched the Telegram notifier to on-demand Wi-Fi usage so the STA is not kept
  permanently associated while Zigbee is running.
- Validated end-to-end delivery on hardware:
  `sensor -> Zigbee -> probe -> Telegram`.
- Documented the practical note that `zb_bind` may need to be reissued in some
  reboot or network-change scenarios to restore reporting.
