# Bådblæser-controller — Handover

Projekt: DIY-styring af 5× 12V PWM-blæsere på en sejlbåd via standard IR-fjernbetjening, ingen ledninger mellem enhederne. Dette dokument er status og kontekst til videre udvikling — ikke en spec, der skal følges slavisk.

## TL;DR for hurtig orientering

Fem identiske ESP32-noder. Hver styrer én 12V 4-pin PWM-blæser (Arctic P14 Pro PST). Alle noder kører **samme firmware** og finder selv deres ID via hardware-jumpere. Enhver node, der modtager et IR-signal fra fjernbetjeningen, udfører kommandoen og videresender den trådløst (ESP-NOW broadcast) til de øvrige noder — uden hub, uden ledninger, uden kode-ændring pr. board.

Den endelige firmware-fil er `baadblaeser_mesh_hw_id.ino` (vedhæftet/i samme mappe som dette dokument). Den er ikke testet på hardware endnu — se "Status og næste skridt".

## Arkitekturbeslutninger og hvorfor

Disse valg er truffet efter at have overvejet alternativer. Bevar dem, medmindre nye informationer fra hardwaretest tilsiger andet.

**IR, ikke RF, til selve fjernbetjeningen.** Brugeren har en sejlbåd på ca. 30 fod, hvor IR-bounce mellem skotter vurderes tilstrækkeligt til praktisk brug. RF (433 MHz) blev overvejet og fravalgt for selve fjernbetjenings-leddet, da IR-løsningen er simplere og bruger en almindelig fjernbetjening.

**ESP-NOW mesh i stedet for hub.** Første idé var én central IR-hub, der distribuerer til "dumme" RF-noder. Det blev forkastet til fordel for at lade *alle* noder være ligeværdige IR-modtagere og repeatere — det fjerner single-point-of-failure og løser elegant evt. desync, fordi redundansen betyder at selv hvis én node misser et tryk pga. skot/afstand, fanger en anden det og spreder det videre.

**Tre regler holder meshet stabilt** (implementeret i firmwaren, se kommentarer i `loop()` og `accept()`):
- **R1 — Ét hop:** kun IR-modtagne kommandoer videresendes via ESP-NOW. Kommandoer modtaget via ESP-NOW udføres, men sendes ALDRIG videre. Forhindrer broadcast-storm/loop.
- **R2 — Ingen repeat-frames, undtaget VOL+/-:** IR repeat-frames (`IRDATA_FLAGS_IS_REPEAT`) ignoreres, MEN `CMD_VOL_UP` og `CMD_VOL_DOWN` accepteres når man holder tasten nede — cirka 3 trin/sek (`VOL_REPEAT_MS = 333`). Alle andre kommandoer forbliver "ét fysisk tryk = ét diskret skridt". Accepterede VOL-gentagelser går gennem den normale sti (dedup + applyCommand + meshBroadcast) så resten af meshet ramper synkront.
- **R3 — Dedup-vindue (150 ms):** Da flere noder typisk hører samme IR-tryk samtidig, ville de hver isæ udføre + videresende uden denne regel, hvilket ville give dobbelt-trin eller uforudsigelig power-toggle. Samme kommando inden for 150 ms efter sidste udførsel ignoreres.

**Hardware-baseret node-ID, ikke kode pr. board.** Tre GPIO-pins med interne pullups (`INPUT_PULLUP`) og jumpere til GND giver 5 mulige ID'er (se tabel i kildekoden). Al fem noder flashes med identisk binary. Dette var et eksplicit krav fra brugeren — undgå at skulle ændre kildekode/genkompilere pr. enhed.

**4-pin PWM, ikke 3-pin + MOSFET.** Tidligt i projektet blev en MOSFET-baseret low-side-styring til 3-pin-blæsere designet i detalje (findes i ældre filer som `baadblaeser_node.ino` for Arduino Nano — **forældet, ikke i brug**). Da det viste sig at brugerens faktiske blæsere (Arctic P14 Pro PST) er ægte 4-pin PWM-enheder med 25 kHz-signal og maks. 5,25V/5mA på signalbenet, blev MOSFET-grenen droppet helt — ESP32 driver PWM-benet direkte. Dette er bekræftet med produktdokumentation, ikke kun antagelse.

**CPU throttlet til 80 MHz for lavere idle-forbrug.** Firmwaren kalder `setCpuFrequencyMhz(80)` som første handling i `setup()`. 80 MHz er ESP32-klassikkens laveste indstilling der stadig holder WiFi-radioen aktiv (nødvendig for ESP-NOW) — 40 MHz og derunder slukker radioen. APB-clocken forbliver 80 MHz, så LEDC PWM (25 kHz) og seriel kører uændret; IRremote skalerer sine timere automatisk. Idle-strøm halveres cirka i forhold til 240 MHz-standarden — relevant på en båd med begrænset batteri. Ingen af arbejdsopgaverne (IR-decode, ESP-NOW, PWM-output) er CPU-bundne, så der er intet at vinde ved at hæve igen. Sænkes yderligere kræver alternativ mesh-transport.

**Boot-failsafe er bevidst accepteret som ufuldstændig.** 4-pin PWM-spec definerer at floating/GND-signal = fuld hastighed (fail-safe for PC-køling). Det er uønsket på en båd ved boot. Løsningen er en 10 kΩ pullup til 3,3V på PWM-pinen + at firmwaren sætter pinen lav som digital output, før LEDC initialiserer (se `pwmBegin()`). Brugeren har eksplicit accepteret, at blæseren kan køre fuld hastighed i det korte boot-vindue (under et sekund), og afvist en MOSFET-baseret hård afbryder som overkill "for nu". **Hvis dette skal løses fuldt ud senere, er det en bevidst efterladt opgave, ikke en fejl.**

## Hardware

**Pr. node (× 5):**
- ESP32 (Arduino-core 2.x eller 3.x — koden har conditional compilation for LEDC API-forskelle)
- Arctic P14 Pro PST 12V 4-pin PWM-blæser — 12V/GND direkte til forsyning, kun signalben (pin 4) til ESP32
- IR-modtager: VS1838B eller TSOP38238 (38 kHz), med 100 Ω serie på Vcc + 100 µF/10V + 100 nF afkobling lokalt ved modtageren
- MP1584 buck-konverter 12V→3,3V/5V
- 10 kΩ pullup-modstand fra PWM-pin til 3,3V
- LED + 330 Ω serie-modstand på SIG_LED_PIN (blinker ved gyldigt modtaget signal — bruges til feltdebugging af IR-dækning)
- 3× jumpere til ID-pins (GPIO 32/33/25 — alle general-purpose, virker på både plain ESP32 og WROVER/PSRAM-varianter)
- 12V hanstik (cigarettænder-type), inline-sikring 1-2A

**Fælles:**
- Fjernbetjening: endnu ikke endeligt valgt. Diskuteret: ONE FOR ALL URC1210 (universal/lærende), men afvist pga. usikker/ikke-dokumenteret protokol indtil testet. Strategi nu: sæt URC1210 til en TV-enhedskode og brug `ir_diagnostik.ino` til at verificere empirisk, at protokollen rapporteres som NEC af IRremote-biblioteket. Samsung BN59-01315B blev også overvejet og **frarådet** — det er en SolarCell/Bluetooth-hybrid-fjernbetjening, hvor IR-fallback for taltaster ikke er garanteret pålideligt.

## Firmware-filer i denne mappe

| Fil | Status | Beskrivelse |
|---|---|---|
| `baadblaeser_mesh_hw_id.ino` | **Aktuel/endelig** | ESP32, IR+ESP-NOW mesh, hardware-ID via jumperpins, 4-pin PWM, signal-LED, boot-pullup-håndtering |
| `ir_diagnostik.ino` | Værktøj | Separat sketch til at identificere IR-protokol/koder fra en ukendt fjernbetjening før koderne indsættes i mesh-firmwaren |
| `baadblaeser_mesh_esp32.ino` | Forældet | Mellemtrin uden hardware-ID (kode-baseret `FAN_ID`) — erstattet af `_hw_id`-varianten |
| `baadblaeser_node_esp32.ino` | Forældet | Tidligere alt-IR-variant uden mesh/ESP-NOW (hver node sin egen IR-modtager, ingen videresendelse) |
| `baadblaeser_node.ino` | Forældet/forkert hardware-antagelse | Arduino Nano + MOSFET til 3-pin-blæsere. Skrevet før det blev afklaret at blæserne er 4-pin. Behold kun som reference. |

**Anbefaling:** ryd de forældede filer ud af repoet, eller flyt dem til en `archive/`-mappe, så der ikke er tvivl om hvilken fil der er aktiv.

## Kommandosæt (fjernbetjening → funktion)

```
Scope (farvetaster — sætter enhedstype OG nulstiller valg til "alle i scope"):
  BLÅ   : blæsere (SCOPE_FAN — denne firmware)
  GUL   : LED-strips (SCOPE_LED — planlagt, kræver separat binary)
  RØD   : reserveret til fremtidig enhedstype
  GRØN  : reserveret til fremtidig enhedstype

Valg (inden for aktiv scope):
  Tast 1-5 : vælg en specifik instans
  Tast 0   : vælg ALLE instanser

Handling (kun aktiv på noder hvis THIS_NODE_SCOPE matcher activeScope):
  + / -    : ét trin op/ned på det valgte (tænder hvis slukket)
  POWER    : tænd/sluk det valgte
  MUTE     : toggle — første tryk gemmer nuværende tilstand og tvinger
             slukket + niveau 0; andet tryk gendanner den gemte tilstand.
             VOL+/-, POWER eller endnu et OFF-cycle imellem nulstiller
             det gemte "bookmark".
```

Scope er "sticky": tryk fx GUL → MUTE slukker alle LED-strips, uden mellemliggende "0"-tryk, fordi et farvetryk nulstiller `selectedFan` til `SELECT_ALL`. Vil man narrow'e ned bagefter, trykker man et taltryk 1-5. Valg-tilstand er persistent indtil næste farve- eller taltryk.

`SEL_LED` på en blæser-node lyser når `activeScope == SCOPE_FAN` OG (`selectedFan == fanID` eller "alle") — altså kun når noden faktisk er adresserbar lige nu.

Niveau-skalaen er 0..10 (elleve diskrete trin) for alle scopes — hver enhedstype mapper 0..10 til sin egen output-range. For blæserne: `{25, 33, 40, 48, 55, 63, 70, 78, 85, 93, 100}` % duty, lineær fra 25% (niveau 0) til 100% (niveau 10), ca. 7,5%-per-trin. Boot-værdi er indeks 5 (63%). 25% er under det typiske stall-gulv for PC-blæsere — det skal verificeres på fysisk hardware om blæserne rent faktisk starter fra niveau 0 eller 1. LED-firmwaren (fremtidig) mapper 0..10 til sin egen lysstyrke-range.

### IR-koder (Samsung, address 0x0E)

Hentet fra URC1210 med `ir_diagnostic.ino`. De koder der ikke bruges lige nu er også taget med — de kan indsættes uden at flashe fjernbetjeningen om igen, hvis firmwaren senere udvides.

| Command | Tast | Rolle i fan-firmwaren |
|---|---|---|
| `0x00` | 0     | `CMD_SELECT_ALL` — vælg alle blæsere |
| `0x01` | 1     | `CMD_SELECT[0]` — vælg blæser 1 |
| `0x02` | 2     | `CMD_SELECT[1]` — vælg blæser 2 |
| `0x03` | 3     | `CMD_SELECT[2]` — vælg blæser 3 |
| `0x04` | 4     | `CMD_SELECT[3]` — vælg blæser 4 |
| `0x05` | 5     | `CMD_SELECT[4]` — vælg blæser 5 |
| `0x06` | 6     | *reserveret — evt. fremtidig CMD_SELECT eller anden enhedstype* |
| `0x07` | 7     | *reserveret* |
| `0x08` | 8     | *reserveret* |
| `0x09` | 9     | *reserveret* |
| `0x0C` | POWER | `CMD_POWER` — tænd/sluk det valgte |
| `0x0D` | MUTE  | `CMD_OFF` — sluk det valgte |
| `0x12` | CH+   | *reserveret — evt. sekundær navigation* |
| `0x13` | CH-   | *reserveret* |
| `0x14` | VOL+  | `CMD_VOL_UP` — ét hastighedstrin op |
| `0x15` | VOL-  | `CMD_VOL_DOWN` — ét hastighedstrin ned |
| `0xA0` | RED   | `CMD_SCOPE_RED` — scope-vælger, reserveret enhedstype |
| `0xA1` | GREEN | `CMD_SCOPE_GREEN` — scope-vælger, reserveret enhedstype |
| `0xA2` | YELLOW| `CMD_SCOPE_LED` — scope-vælger, LED-strips (kræver fremtidig LED-firmware) |
| `0xA3` | BLUE  | `CMD_SCOPE_FAN` — scope-vælger, blæsere (denne firmware) |

## Kendte risici / ting at validere før eller under videre udvikling

1. **IR-protokol bekræftet som Samsung32.** Firmwaren har `#define DECODE_SAMSUNG` aktiveret (skiftet fra NEC efter test med `ir_diagnostic.ino` — den valgte enhedskode på fjernbetjeningen sender Samsung32-frames). Hvis fjernbetjeningen senere ændres eller en anden enhedskode vælges, kør diagnostik-sketchen igen — er protokollen ikke længere Samsung, skal enten enhedskoden justeres eller et yderligere `#define DECODE_*` tilføjes.

2. **Boot-failsafe er delvis, ikke komplet** (se Arkitekturbeslutninger ovenfor) — accepteret af brugeren, men bør nævnes hvis projektet skal "hærdes" senere.

3. **Hastighedstrin og laveste duty (25%) er ikke valideret mod fysisk blæser.** Starter fanen fra 25% duty? Skal bekræftes ved første hardwaretest — hæv til fx 30-40% hvis fanen ikke starter pålideligt fra niveau 0.

4. **Adressefiltrering findes ikke.** Firmwaren filtrerer kun på Samsung `command`-byte, ikke `address`. Brugeren har vurderet risikoen for interferens fra andre både/fjernbetjeninger som teoretisk og lav i praksis (IR kræver sigtelinje/nær refleksion) — bevidst fravalgt, ikke overset.

5. **`ledc`-API har forskellig signatur mellem ESP32 Arduino-core 2.x og 3.x.** Firmwaren håndterer dette med `#if ESP_ARDUINO_VERSION_MAJOR >= 3` — vigtigt at bevare ved fremtidige ændringer i PWM-relateret kode.

## Fremtidig udvidelse (scope-infrastruktur bygget — LED-node udestår)

Brugeren har bekræftet at farvetasterne skal fungere som **scope-præfiks**, og at det næste tryk efter en farve skal gælde alle enheder i den scope indtil et taltryk indsnævrer. Dette er implementeret i den nuværende firmware:

- Farvetaster tracked på alle noder: `activeScope` opdateres på både IR- og mesh-siden, så meshet forbliver synkron. Fordeling: blå = blæsere (`SCOPE_FAN`), gul = LED-strips (`SCOPE_LED`), rød/grøn = reserveret til fremtidige enhedstyper.
- Kommandoer der ikke er scope- eller select-taster kun aktive når `activeScope == THIS_NODE_SCOPE`. Blæser-firmwaren har `THIS_NODE_SCOPE = SCOPE_FAN` som compile-time konstant; en fremtidig LED-firmware sætter `SCOPE_LED`.
- Farvetryk nulstiller `selectedFan` til `SELECT_ALL`, så "gul → mute" slukker alle LED-strips uden et mellemliggende "0"-tryk.
- `SEL_LED` lyser når noden er adresserbar nu: `activeScope == THIS_NODE_SCOPE` og `selectedFan` matcher (eller er "alle").

Boot-default er `SCOPE_FAN` (blæsermode). Det er stadig ubekræftet om `activeScope` skal huskes på tværs af genstart — nuværende valg er nej, for at genstart altid lander i den sikreste tilstand på en båd.

Firmwaren er nu opdelt i common-kode (`control_mesh_hw_id.ino`) og en device-specifik output-stage (`device_fan.cpp`) med et lille interface (`device.h`) — `deviceBegin()`, `deviceApply(on, levelIndex)`, og `THIS_NODE_SCOPE`. En LED-node arves ved at kopiere `.ino` + `device.h` til en ny sketch-mappe (`control_mesh_led/`) og skrive et `device_led.cpp` der implementerer de tre symboler. Common-koden røres ikke.

Hvad der stadig udestår før LED-strips kan bruges:

- Et nyt sketch-directory `control_mesh_led/` med kopi af `.ino` og `device.h` plus et `device_led.cpp`. Driver-valg (WS2812/NeoPixel-protokol vs simpel MOSFET-PWM til analoge 12V-strips) og hardware-pinout er ikke besluttet.
- LED-node hardware (buck-konverter dimensioneret til strips, evt. logic-level shifter til 5V-data hvis WS2812).

**Byg ikke LED-binary'en proaktivt** — vent på at brugeren beder om det. Både scope-infrastruktur og device-lags-abstraktion er dog klar, så det bliver reelt bare implementering af `device_led.cpp`.

## Status og næste skridt

Intet er testet på fysisk hardware endnu. Rækkefølgen brugeren har lagt op til:

1. Vælg og anskaf endelig fjernbetjening (eller test URC1210 med en TV-enhedskode).
2. Kør `ir_diagnostik.ino` på én node, log faktiske koder for alle taster (1-5, +, -, power, 0).
3. Opdatér `CMD_SELECT[]`/`CMD_VOL_UP`/`CMD_VOL_DOWN`/`CMD_POWER`/`CMD_ALL_OFF` i `baadblaeser_mesh_hw_id.ino` med de fundne værdier.
4. Bekræft konkret ESP32-boardvariant (PSRAM eller ej) og justér ID-/blæser-pins om nødvendigt.
5. Flash + test én node solo (IR-modtagelse, PWM-output mod en faktisk blæser).
6. Flash alle 5, test mesh-distribution (signal-LED på hver node bekræfter modtagelse), test fra forskellige positioner i båden for at vurdere IR-dækning gennem skotter.
7. Justér hastighedstrin/stall-gulv efter behov.
