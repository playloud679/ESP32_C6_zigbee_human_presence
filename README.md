# ESP32_C6_zigbee_human_presence

Questo non e` un firmware applicativo. E` un backend minimale travestito da
probe embedded.

L'`ESP32-C6` gira come coordinator Zigbee e osserva un solo problema: capire
quando un sensore di presenza HU entra e esce dallo stato umano rilevato,
senza il rumore di tutto il resto. Niente cloud. Niente display finale. Niente
automazioni decorative. Solo rete, stato, log e un LED locale che dice la
verita` se il software la conosce.

## Idea

Il sensore non va trattato come un device elegante che espone un modello ZCL
pulito e ortodosso. Va trattato come un nodo che puo`:

- parlare standard quando gli conviene
- parlare vendor quando gli conviene di piu`
- cambiare `short_addr`
- ignorare discovery ZDO mentre continua a trasmettere burst utili

Quindi il software non parte da "che specifica dichiara il device", ma da
"qual e` la fonte di verita` piu` affidabile in questo istante".

In questo progetto la fonte di verita` e` questa, in ordine pratico:

1. burst reali `EF00`
2. notifiche `IAS Zone`
3. report standard `Occupancy`
4. discovery e metadata, ma solo come contesto, non come verita` operativa

## Modello mentale

Il sistema ha tre piani distinti.

`Rete`

Il coordinator forma o riapre la rete Zigbee, registra un endpoint locale e si
mette in condizione di ricevere traffico che altrimenti lo stack scarterebbe.

`Contesto device`

Il software mantiene in RAM un piccolo modello del sensore corrente:
indirizzo corto, endpoint, tipo logico, cluster osservato, presenza del vendor
cluster `0xEF00`, qualche metadato IAS.

Questo contesto e` persistito solo nella sua parte stabile. Non per comodita`,
ma per non mentire a se stesso al reboot.

`Stato presenza`

`PRESENT` e `CLEAR` non sono configurazione. Sono runtime puro.
Se li salvi, introduci memoria dove dovrebbe esserci osservazione.
Appena fai questo errore il sistema puo` rialzarsi gia` in `PRESENT`,
che e` esattamente la cosa che un sensore di presenza non deve mai inventare.

## Invarianti

Questo repo vive su poche regole dure.

- La presenza non si persiste.
- La configurazione del sensore si puo` persistere.
- I burst reali valgono piu` della discovery fallita.
- Il bootstrap non deve generare traffico inutile che altera il sensore.
- Il LED locale e` solo una vista dello stato interno, non una sorgente di stato.

Se rompi una di queste regole, il progetto continua a compilare ma smette di
essere affidabile.

## Perche` esiste l'endpoint locale

Lo stack Zigbee inoltra certi callback solo se sul coordinator esiste davvero
il cluster lato giusto. Per questo il probe registra un endpoint locale con i
cluster client necessari.

Non e` estetica architetturale. E` una tassa d'ingresso imposta dallo stack.
Se non lo fai, il traffico puo` esistere in aria ma non esistere per il tuo
software.

## Il caso HU presence

La milestone congelata in questo repo e` quella del sensore HU presence Zigbee.

Le osservazioni importanti sono state queste:

- i burst `EF00` arrivano davvero
- il device puo` cambiare `short_addr`
- `zb_discover` puo` fallire anche quando il sensore e` vivo
- `PRESENT` e `CLEAR` si vedono bene solo se il software resta passivo e non
  reinventa lo stato al boot

Il backend ha quindi smesso di inseguire un device ideale e ha iniziato a
seguire il traffico reale.

## Persistenza: cosa si salva, cosa no

Si salva:

- `short_addr`
- `endpoint`
- `kind`
- `cluster_id`
- `has_vendor_cluster`
- campi stabili del contesto IAS

Non si salva:

- `occupancy_known`
- `occupied`
- `zone_status`

Questa separazione e` il cuore del progetto. Senza questa separazione il probe
diventa un autore di fiction.

## LED come strumento di debug

Il LED onboard della `ESP32-C6-Zero` non e` un effetto UI. E` uno strumento di
debug immediato.

- `unknown` -> spento
- `CLEAR` -> bianco
- `PRESENT` -> viola

La board e` stata verificata con mapping `RGB`, non `GRB`. Questo dettaglio
sembra banale finche` non chiedi il viola e ottieni turchese. In quel momento
capisci che l'hardware ti sta dicendo la verita` sul formato byte prima ancora
dei log.

Board reference: [Waveshare ESP32-C6-Zero](https://www.waveshare.com/wiki/ESP32-C6-Zero?srsltid=AfmBOopFxMjO_4ma8wf0lXDpBSiPipDySdWLvjq7NPo_uu-O3YDNXeze)

## File che contano

- `main/app_main.c`
  bootstrap NVS, piattaforma Zigbee, avvio del task principale
- `main/zb_coordinator.c`
  rete, segnali dello stack, raw handler `EF00`, wiring dei callback Zigbee
- `main/zb_presence.c`
  modello del sensore, discovery, bind, parsing dei DP vendor, persistenza del
  contesto, transizioni di stato
- `main/presence_led.c`
  traduzione dello stato software in feedback locale sul LED

## Cosa non e`

Non e` ancora il firmware del display e-ink.
Non e` un prodotto finito.
Non e` una libreria generica per qualunque sensore Zigbee.

E` una base isolata, utile proprio perche` riduce il mondo a una sola domanda:

"questo sensore sta dicendo che c'e` una persona, oppure no?"

Finche` questa domanda non e` banale e affidabile qui, non ha senso integrarla
nel resto.
