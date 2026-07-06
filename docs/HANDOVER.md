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
- **R2 — Ingen repeat-frames:** IR repeat-frames (`IRDATA_FLAGS_IS_REPEAT`, afsendt når man holder en tast nede) ignoreres helt. Hvert fysisk tryk giver præcis ét diskret skridt. Det gør hastighedsregulering til "tryk pr. trin" snarere end "hold for at ramp'e" — bevidst valg, ikke en begrænsning der skal rettes.
- **R3 — Dedup-vindue (150 ms):** Da flere noder typisk hører samme IR-tryk samtidig, ville de hver isæ udføre + videresende uden denne regel, hvilket ville give dobbelt-trin eller uforudsigelig power-toggle. Samme kommando inden for 150 ms efter sidste udførsel ignoreres.

**Hardware-baseret node-ID, ikke kode pr. board.** Tre GPIO-pins med interne pullups (`INPUT_PULLUP`) og jumpere til GND giver 5 mulige ID'er (se tabel i kildekoden). Al fem noder flashes med identisk binary. Dette var et eksplicit krav fra brugeren — undgå at skulle ændre kildekode/genkompilere pr. enhed.

**4-pin PWM, ikke 3-pin + MOSFET.** Tidligt i projektet blev en MOSFET-baseret low-side-styring til 3-pin-blæsere designet i detalje (findes i ældre filer som `baadblaeser_node.ino` for Arduino Nano — **forældet, ikke i brug**). Da det viste sig at brugerens faktiske blæsere (Arctic P14 Pro PST) er ægte 4-pin PWM-enheder med 25 kHz-signal og maks. 5,25V/5mA på signalbenet, blev MOSFET-grenen droppet helt — ESP32 driver PWM-benet direkte. Dette er bekræftet med produktdokumentation, ikke kun antagelse.

**Boot-failsafe er bevidst accepteret som ufuldstændig.** 4-pin PWM-spec definerer at floating/GND-signal = fuld hastighed (fail-safe for PC-køling). Det er uønsket på en båd ved boot. Løsningen er en 10 kΩ pullup til 3,3V på PWM-pinen + at firmwaren sætter pinen lav som digital output, før LEDC initialiserer (se `pwmBegin()`). Brugeren har eksplicit accepteret, at blæseren kan køre fuld hastighed i det korte boot-vindue (under et sekund), og afvist en MOSFET-baseret hård afbryder som overkill "for nu". **Hvis dette skal løses fuldt ud senere, er det en bevidst efterladt opgave, ikke en fejl.**

## Hardware

**Pr. node (× 5):**
- ESP32 (Arduino-core 2.x eller 3.x — koden har conditional compilation for LEDC API-forskelle)
- Arctic P14 Pro PST 12V 4-pin PWM-blæser — 12V/GND direkte til forsyning, kun signalben (pin 4) til ESP32
- IR-modtager: VS1838B eller TSOP38238 (38 kHz), med 100 Ω serie på Vcc + 100 µF/10V + 100 nF afkobling lokalt ved modtageren
- MP1584 buck-konverter 12V→3,3V/5V
- 10 kΩ pullup-modstand fra PWM-pin til 3,3V
- LED + 330 Ω serie-modstand på SIG_LED_PIN (blinker ved gyldigt modtaget signal — bruges til feltdebugging af IR-dækning)
- 3× jumpere til ID-pins (GPIO 16/17/18 — **tjek for PSRAM-konflikt**, se Kendte risici)
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
Tast 1–5 : vælg en specifik blæser (gælder for efterfølgende kommandoer)
Tast 0   : vælg ALLE blæsere (gælder for efterfølgende kommandoer)
+ / -    : ét hastighedstrin op/ned på det valgte (tænder hvis slukket)
POWER    : tænd/sluk det valgte
MUTE     : sluk det valgte uafhængigt af nuværende status
```

Valg-tilstand er persistent: har man valgt "alle" med tast 0, gælder efterfølgende +/-/POWER/MUTE alle blæsere, indtil man vælger en specifik igen med 1-5. `SEL_LED` lyser på en node når `selectedFan == fanID` eller når "vælg alle" er aktiv.

Hastighedstrin: `{40, 55, 70, 85, 100}` % duty — 5 trin, indeks 2 (70%) er startværdi efter boot. Disse tal er ikke verificeret mod faktiske blæsere endnu; 40% blev valgt som et gæt på et fornuftigt stall-gulv og bør justeres efter test.

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
| `0xA0` | RED   | *reserveret — fremtidig enhedstype (fx pumper)* |
| `0xA1` | GREEN | *reserveret — fremtidig enhedstype* |
| `0xA2` | YELLOW| *reserveret — planlagt: LED-strips (se afsnit længere nede)* |
| `0xA3` | BLUE  | *reserveret — planlagt: blæsere som eksplicit scope-vælger* |

## Kendte risici / ting at validere før eller under videre udvikling

1. **GPIO-konflikt på PSRAM-boards.** ID-pins (16/17/18) og blæser-pins (4/5/19) er valgt for et "almindeligt" ESP32-board uden PSRAM. På WROVER-varianter er GPIO 16/17 optaget af SPI til PSRAM. Hvis det konkrete board har PSRAM, skal ID-pins flyttes (forslag: GPIO 32/33/34 — bemærk at 34+ er input-only uden intern pullup og kræver eksterne 10 kΩ-modstande).

2. **IR-protokol bekræftet som Samsung32.** Firmwaren har `#define DECODE_SAMSUNG` aktiveret (skiftet fra NEC efter test med `ir_diagnostic.ino` — den valgte enhedskode på fjernbetjeningen sender Samsung32-frames). Hvis fjernbetjeningen senere ændres eller en anden enhedskode vælges, kør diagnostik-sketchen igen — er protokollen ikke længere Samsung, skal enten enhedskoden justeres eller et yderligere `#define DECODE_*` tilføjes.

3. **Boot-failsafe er delvis, ikke komplet** (se Arkitekturbeslutninger ovenfor) — accepteret af brugeren, men bør nævnes hvis projektet skal "hærdes" senere.

4. **Hastighedstrin og stall-gulv (40%) er ikke valideret mod fysisk blæser.** Bør justeres efter første hardwaretest.

5. **Adressefiltrering findes ikke.** Firmwaren filtrerer kun på NEC `command`-byte, ikke `address`. Brugeren har vurderet risikoen for interferens fra andre både/fjernbetjeninger som teoretisk og lav i praksis (IR kræver sigtelinje/nær refleksion) — bevidst fravalgt, ikke overset.

6. **`ledc`-API har forskellig signatur mellem ESP32 Arduino-core 2.x og 3.x.** Firmwaren håndterer dette med `#if ESP_ARDUINO_VERSION_MAJOR >= 3` — vigtigt at bevare ved fremtidige ændringer i PWM-relateret kode.

## Fremtidig udvidelse (besluttet udskudt, men arkitektur forberedt for den)

Brugeren ønsker senere at udvide samme mesh til at styre LED-strips i båden med samme fjernbetjening. Plan, ikke implementeret endnu:

- Brug fjernbetjeningens 4 farvetaster (rød/gul/grøn/blå) som **enhedstype-vælger**, adskilt fra 1-5/+/-/power, som forbliver "lokale" kommandoer inden for den valgte enhedstype. Foreslået: blå = blæsere, gul = LED-strips, rød/grøn reserveret til fremtidige enhedstyper (fx pumper, cockpit-lys).
- Dette løser navnerum-kollision (uden farvetast-præfiks ville "tryk 1" tvetydigt ramme både en blæser-node og en fremtidig LED-node).
- Mesh-laget (IR-modtagelse, ESP-NOW-distribution, R1-R3-reglerne) er allerede enhedsagnostisk og kan genbruges uændret. Kun `applyCommand()`/`applyOutput()` og et nyt `activeDeviceType`-felt (sat af farvetasterne, kompileret ind pr. node-type ligesom `FAN_ID` i dag) skal tilføjes.
- LED-strips vil sandsynligvis kræve anden output-elektronik end blæsernes direkte PWM-ben (enten MOSFET-PWM til simple 12V-strips, eller WS2812/NeoPixel-protokol til adresserbare strips) — dette er ikke designet endnu.
- Åbent spørgsmål, ikke besluttet: skal `activeDeviceType` huskes ved genstart, eller altid boote til en default (sandsynligvis blæser-mode er det rigtige fail-safe-valg, men ikke bekræftet med brugeren).

**Eksplicit brugerbeslutning:** dette er bevidst lagt på is — byg det ikke proaktivt, før brugeren beder om det igen.

## Status og næste skridt

Intet er testet på fysisk hardware endnu. Rækkefølgen brugeren har lagt op til:

1. Vælg og anskaf endelig fjernbetjening (eller test URC1210 med en TV-enhedskode).
2. Kør `ir_diagnostik.ino` på én node, log faktiske koder for alle taster (1-5, +, -, power, 0).
3. Opdatér `CMD_SELECT[]`/`CMD_VOL_UP`/`CMD_VOL_DOWN`/`CMD_POWER`/`CMD_ALL_OFF` i `baadblaeser_mesh_hw_id.ino` med de fundne værdier.
4. Bekræft konkret ESP32-boardvariant (PSRAM eller ej) og justér ID-/blæser-pins om nødvendigt.
5. Flash + test én node solo (IR-modtagelse, PWM-output mod en faktisk blæser).
6. Flash alle 5, test mesh-distribution (signal-LED på hver node bekræfter modtagelse), test fra forskellige positioner i båden for at vurdere IR-dækning gennem skotter.
7. Justér hastighedstrin/stall-gulv efter behov.
