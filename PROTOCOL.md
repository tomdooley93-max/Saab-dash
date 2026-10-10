# Saab Gauge BLE-protokoll v1

Gemensamt kontrakt mellan dongeln (ESP32, firmware) och webbappen (`app.html`).
Alla flerbytesfält är **little-endian** om inget annat anges.

## GATT

| Roll | UUID | Egenskaper |
|---|---|---|
| Service | `7a5a0001-5341-4142-9b3e-0c2a1d4e8f01` | – |
| DATA | `7a5a0002-5341-4142-9b3e-0c2a1d4e8f01` | notify, read |
| DTC | `7a5a0003-5341-4142-9b3e-0c2a1d4e8f01` | notify, read |
| CMD | `7a5a0004-5341-4142-9b3e-0c2a1d4e8f01` | write, write-without-response |
| LOG | `7a5a0005-5341-4142-9b3e-0c2a1d4e8f01` | notify (UTF-8-text) |

Annonseringsnamn: börjar med `SAAB` (t.ex. `SAAB-T8`). Servicens UUID ska finnas i annonseringen.

## DATA (20 byte, 10–20 Hz)

Ryms i standard-MTU (20 byte nyttolast), så det fungerar även om MTU-förhandlingen uteblir.

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

## DTC (skickas vid ändring, och kan läsas)

```
u8 version (=1)
u8 count
count × { u16 code (BIG-endian, SAE-format), u8 status }
```

SAE-format: bit 15–14 = bokstav (0 P, 1 C, 2 B, 3 U), bit 13–12 = första siffran, bit 11–0 = tre hexsiffror.
Exempel: `0x0171` → P0171. Firmware begränsar listan till 16 koder.

## CMD (app → dongel)

| Byte 0 | Data | Betydelse |
|---|---|---|
| 0x01 | – | Radera felkoder (KWP ClearDiagnosticInformation) |
| 0x02 | – | Läs om felkoderna |
| 0x10 | u8 sid-id | Aktiv sida i appen (se nedan). Dongeln håller diagnossessionen för dynsym bara när sidan är LUFTMASSA, precis som displayen gjorde. |
| 0x30 | ASCII-text | Konsolkommando, samma som det seriella (t.ex. `v`). Svaret kommer på LOG. |

Sid-id: 0 ÖVERSIKT, 1 BOOST, 2 FART, 3 TEMP, 4 VOLT, 5 LUFTMASSA, 6 FELKODER.

## LOG (dongel → app)

Fri UTF-8-text, en eller flera rader per notifiering. Speglar firmwarens `printf`. Används för felsökning.
