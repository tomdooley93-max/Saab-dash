# Saab Dash – protokoll v2 (Wi-Fi)

Kontraktet mellan dongeln (ESP32, firmware) och appen (`web/app.html`).
Alla flerbytesfält är **little-endian** om inget annat anges.

## Översikt

```
Bil ─CAN─ Dongel (ESP32) ══ Wi-Fi (telefonens hotspot) ══ iPhone, Safari
           ├ GET  /            → appen (index.html, lagrad i dongeln)
           ├ WS   /ws          → värden 10–20 Hz, felkoder, logg  ←→  kommandon
           ├ GET  /api/info    → versioner och status (JSON)
           ├ POST /api/app     → ny app (index.html)
           └ POST /api/ota     → ny firmware (.bin)
```

- Dongeln ansluter som klient till telefonens personliga hotspot (station-läge).
  SSID och lösenord ställs in en gång med konsolkommandot `wifi`. Utan sparad hotspot,
  eller om ingen hittas på 30 s, öppnar dongeln ett eget nät `Saab-Dash` (appen på `http://192.168.4.1`).
- Dongeln annonserar sig med mDNS som **`saab-dash.local`**. Fungerar inte mDNS
  används dongelns IP-adress på hotspot-nätet.
- Appen laddas från dongeln, så sidan och WebSocket har samma ursprung. (En sida från
  GitHub Pages, https, får inte öppna `ws://` mot dongeln – därför ligger appen i dongeln.)

## WebSocket `/ws`

Binära meddelanden. Första byten anger typ, resten är nyttolasten.

### Dongel → app

| Typ | Nyttolast | När |
|---|---|---|
| `0x01` DATA | 20 byte, se nedan | 10–20 Hz |
| `0x02` DTC | felkodslista, se nedan | vid anslutning och vid ändring |
| `0x03` LOG | UTF-8-text, en eller flera rader | speglar firmwarens `printf` |

Dongeln skickar minst ett DATA-paket per sekund. Hör appen inget på 3 s stänger den
anslutningen och ansluter igen.

### App → dongel (kommandon)

| Byte 0 | Data | Betydelse |
|---|---|---|
| `0x01` | – | Radera felkoder (KWP ClearDiagnosticInformation) |
| `0x02` | – | Läs om felkoderna |
| `0x10` | u8 sid-id | Aktiv sida. Dongeln håller diagnossessionen för dynsym bara när sidan är LUFTMASSA. |
| `0x30` | UTF-8-text | Konsolkommando: `v` (versioner), `wifi SSID\|LÖSENORD` (spara hotspot), `reboot`. Svaret kommer som LOG. |

Sid-id: 0 ÖVERSIKT, 1 LADDTRYCK, 2 FART, 3 MOTORTEMP, 4 BATTERI, 5 LUFTMASSA, 6 FELKODER.

### DATA (20 byte)

| Offset | Typ | Fält | Enhet / kodning | Okänt värde |
|---|---|---|---|---|
| 0 | u8 | version | = 1 | – |
| 1 | u8 | flags | b0 can_alive, b1 adapt_active, b2 cel_on, b3 diag_session | – |
| 2 | i16 | boost | mbar relativt (−1000…+2000) | 0x7FFF |
| 4 | u16 | rpm | varv/min | 0xFFFF |
| 6 | u8 | speed | km/h | 0xFF |
| 7 | u8 | coolant | °C + 40 | 0xFF |
| 8 | u8 | iat | °C + 40 | 0xFF |
| 9 | u8 | batt | 0,1 V | 0xFF |
| 10 | u16 | airm_req | mg/c | 0xFFFF |
| 12 | u16 | airm_act | mg/c | 0xFFFF |
| 14 | u16 | afr | AFR × 100 | 0xFFFF |
| 16 | u8 | ethanol | % | 0xFF |
| 17 | u8 | fuel | % | 0xFF |
| 18 | u8 | dtc_count | antal | – |
| 19 | u8 | seq | löpnummer, räknar upp per paket | – |

### DTC

```
u8 version (=1)
u8 count
count × { u16 code (BIG-endian, SAE-format), u8 status }
```

SAE-format: bit 15–14 = bokstav (0 P, 1 C, 2 B, 3 U), bit 13–12 = första siffran,
bit 11–0 = tre hexsiffror. Exempel: `0x0171` → P0171. Högst 16 koder.

## HTTP

| Metod och sökväg | Svar / innehåll |
|---|---|
| `GET /` | Appen. Lagras som fil i dongelns lagringsyta (`/www/index.html`). Saknas filen visas en enkel uppladdningssida. |
| `GET /icons/*`, `/manifest.json` | Ikoner och manifest för hemskärmen, från samma lagringsyta. |
| `GET /api/info` | `{"device":"saab-dash","fw":"1.0.0","app":"<build>","ip":"172.20.10.5","rssi":-61,"uptime":1234,"vbat":14.1}` |
| `POST /api/app` | Brödtext = ny `index.html`. Skrivs till en temporär fil och byter plats först när allt kommit fram. Svar `{"ok":true}`. |
| `POST /api/ota` | Brödtext = firmware `.bin`. Skrivs till den lediga OTA-platsen; dongeln startar om. Svar `{"ok":true}` innan omstart. |

### Uppdatering och säkerhet

- **Firmware:** två OTA-platser (A/B) med automatisk återgång. Startar den nya versionen
  inte korrekt (markerar sig inte som giltig inom 30 s) går dongeln tillbaka till den gamla.
- **App:** byts atomiskt, så en avbruten uppladdning lämnar den gamla appen kvar.
- **Från telefonen:** appens Logg-panel kan hämta senaste app och firmware från GitHub Pages
  (`version.json`, `index.html`, `fw/saab-dash.bin`) över hotspotens internet och skicka dem
  till dongeln, eller ladda upp en fil från iPhonens Filer-app.
- **Nödläge:** startar dongeln inte alls flashas den med USB-kabel enligt WiCANs manual.
