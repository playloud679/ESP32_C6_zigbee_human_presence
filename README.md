# zigbee_presence_probe_c6

Mini progetto ESP-IDF per validare il trigger Zigbee presenza su `ESP32-C6`
prima di integrarlo nel firmware del display e-ink.

## Obiettivo

Il probe fa tre cose:

1. Avvia l'`ESP32-C6` come coordinator Zigbee.
2. Accetta il join di un sensore presenza.
3. Cerca un endpoint che esponga il cluster `Occupancy Sensing` (`0x0406`),
   prova a fare bind + configure reporting e stampa a seriale i cambi di stato.

Non scarica Blynk e non aggiorna il display: serve solo a validare il trigger.

## Struttura

- `main/app_main.c`: bootstrap NVS + piattaforma Zigbee.
- `main/zb_coordinator.c`: formazione rete, endpoint locale, segnali stack.
- `main/zb_presence.c`: discovery endpoint, bind, configure reporting, log occupancy.

## Build

Prerequisiti:

- `ESP-IDF` installato.
- accesso Internet al primo build per scaricare `esp-zboss-lib` e `esp-zigbee-lib`.

Esempio:

```sh
. "$HOME/esp/esp-idf/export.sh"
cd zigbee_presence_probe_c6
idf.py set-target esp32c6
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

## Flusso atteso a seriale

Dopo il flash dovresti vedere:

- formazione rete coordinator
- `permit join`
- `device announce`
- ricerca endpoint
- individuazione cluster `0x0406`
- log tipo `OCCUPANCY ... state=PRESENT` o `state=CLEAR`

## CLI seriale

Il probe espone anche una piccola CLI con prompt `zb>`.

Comandi utili:

- `help`
- `zb_status`
- `zb_open 180`
- `zb_close`
- `zb_sensor`
- `zb_discover 0x1234`
- `zb_bind`
- `zb_report`
- `zb_factory_reset`
- `tg_status`
- `tg_scan`
- `tg_wifi "SSID" "PASSWORD"`
- `tg_wifi_pick <index> "<PASSWORD>"`
- `tg_chat "<BOT_TOKEN>" "<CHAT_ID>"`
- `tg_test`
- `tg_reset`

Uso tipico:

1. fai partire il coordinator
2. apri la rete con `zb_open 180`
3. fai joinare il sensore
4. se serve, rilancia discovery manuale con `zb_discover 0xSHORT`
5. forza `zb_bind` e `zb_report`
6. osserva i log `OCCUPANCY ...`

## Telegram

Il probe puo` anche inoltrare a Telegram i cambi di stato presenza.

Configurazione minima:

1. opzionale: fai `tg_scan` per vedere gli SSID rilevati dal probe
2. imposta il Wi-Fi con `tg_wifi "SSID" "PASSWORD"` oppure `tg_wifi_pick <index> "<PASSWORD>"`
3. imposta bot e destinazione con `tg_chat "<BOT_TOKEN>" "<CHAT_ID>"`
4. verifica con `tg_test`
5. controlla lo stato con `tg_status`

Comportamento:

- invia solo sui veri cambi di stato: `PRESENT` e `CLEAR`
- le credenziali restano in NVS, quindi sopravvivono al reboot
- il Wi-Fi viene usato on-demand per scan o invio, poi viene disconnesso per non
  interferire stabilmente con Zigbee sul radio shared del `ESP32-C6`
- se l'orario di sistema non e` valido usa come fallback il tempo dal boot
- `tg_reset` cancella tutta la configurazione Telegram/Wi-Fi del notifier

## Changelog

- aggiunto notifier Telegram con configurazione persistente in NVS
- aggiunti comandi `tg_scan` e `tg_wifi_pick` per scegliere il Wi-Fi dal radio del probe
- abilitata la coesistenza `Wi-Fi + Zigbee` con `esp_coex_wifi_i154_enable()`
- stabilizzato il notifier usando Wi-Fi on-demand, senza tenere la STA sempre associata
- validato il flusso completo `PRESENT/CLEAR -> Telegram` su hardware reale

## Note

- Il progetto usa un endpoint locale con cluster `Occupancy Sensing` lato client,
  perche` l'SDK inoltra il `REPORT_ATTR` al callback solo se il cluster esiste
  sul nostro endpoint.
- Su `ESP32-C6`, la coesistenza `Wi-Fi + Zigbee` va abilitata esplicitamente:
  senza `esp_coex_wifi_i154_enable()` il Wi-Fi puo` risultare instabile oppure
  restituire scan vuoti con Zigbee gia` attivo.
- In alcuni reboot o dopo variazioni di rete puo` essere necessario rilanciare
  `zb_bind` per riallineare il reporting del sensore.
- Il sensore reale potrebbe richiedere ritocchi su `bind` o `configure reporting`
  a seconda del vendor. Questo scaffold e` pensato per essere la base del test.
