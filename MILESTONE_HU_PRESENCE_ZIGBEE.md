# HU Presence Zigbee Milestone

Questo repository congela la milestone del probe Zigbee per il sensore HU
presence prima dell'integrazione nel firmware principale del display e-ink.

## Stato consolidato

- Probe ESP-IDF su `ESP32-C6` operativo come coordinator Zigbee.
- Ricezione dei burst vendor `EF00` confermata.
- Parsing dei payload Tuya-like con identificazione dei DP rilevanti.
- Rilevazione runtime di `PRESENT` e `CLEAR` verificata.
- Evitata la regressione del falso stato `sempre PRESENT`.
- Persistenza limitata alla configurazione stabile del sensore, non allo stato
  di presenza.
- LED onboard della `ESP32-C6-Zero` usato come indicatore locale:
  - `CLEAR` -> bianco
  - `PRESENT` -> viola
  - `unknown` -> spento
- Mapping colore LED validato in formato `RGB` per questa board.

## Vincoli emersi

- Il sensore puo` cambiare `short_addr`, quindi il codice non deve assumere che
  un vecchio indirizzo resti valido.
- La discovery ZDO puo` fallire anche quando il sensore e` attivo; i burst reali
  `EF00` sono la fonte piu` affidabile per aggiornare il contesto del device.
- Lo stato `PRESENT/CLEAR` non va persistito su flash.
- Rebind o reconfigure automatici al boot possono alterare il comportamento del
  sensore e vanno evitati finche` non servono davvero.

## Base consigliata per i passi successivi

- Usare questo repo come base isolata per test e refactor.
- Integrare nel firmware principale solo dopo verifica dei burst reali su lunga
  durata.
- Mantenere separati:
  - configurazione persistente del sensore
  - stato runtime della presenza
  - feedback locale LED
