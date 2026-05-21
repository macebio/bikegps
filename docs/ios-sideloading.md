# Installare BikeGPS su iPhone — Guida al sideloading

> **TL;DR — sì, è un po' rognoso.** Apple impone restrizioni severe sulle app non distribuite tramite l'App Store: con un account sviluppatore gratuito, l'app smette di funzionare dopo **7 giorni** e va reinstallata da Xcode. Non c'è modo di aggirarlo senza pagare 99€/anno (Apple Developer Program) o fare jailbreak. Mi dispiace — non è colpa nostra.
>
> Una **versione Android** è in lavorazione e non avrà questo problema.

---

## Cosa ti serve

- Un Mac con **Xcode 15 o superiore** (scaricabile gratis dall'App Store)
- Un **Apple ID** (anche quello gratuito va benissimo)
- Il tool **XcodeGen**: `brew install xcodegen`
- Il cavo Lightning/USB-C per collegare l'iPhone al Mac

---

## Setup iniziale (una volta sola)

### 1. Genera il progetto Xcode

```bash
cd iphone-app
xcodegen generate
open BikeGPS.xcodeproj
```

### 2. Collega il tuo iPhone al Mac

Usa il cavo. Xcode lo rileverà in pochi secondi.

### 3. Configura firma e Bundle ID

In Xcode:
1. Clicca su **BikeGPS** nel pannello a sinistra (il progetto, non una cartella)
2. Seleziona il target **BikeGPS** → tab **Signing & Capabilities**
3. In **Team**, scegli il tuo Apple ID dal menu a tendina
   - Se non compare, vai su **Xcode → Settings → Accounts** e aggiungi il tuo Apple ID
4. Cambia il **Bundle Identifier** da `com.yourname.bikegps` a qualcosa di unico, es. `com.tuonome.bikegps`
   - Deve essere unico a livello mondiale — aggiungi il tuo nome/nickname

Xcode dovrebbe mostrare "Signing Certificate: Apple Development" senza errori rossi.

### 4. Installa l'app sul telefono

1. In alto a sinistra, seleziona il tuo iPhone come destinazione (al posto del simulatore)
2. Premi ▶ (Run) oppure `Cmd+R`
3. Xcode compila e installa l'app — ci vuole 1-3 minuti la prima volta

---

## Autorizzare l'app sull'iPhone (prima installazione)

Dopo aver premuto Run, sul telefono comparirà un avviso **"Developer non autorizzato"** e l'app non si aprirà. È normale.

1. Vai su **Impostazioni** (l'app grigia con l'ingranaggio)
2. Scorri fino a **Generali** → **VPN e gestione dispositivo**
3. Cerca il tuo Apple ID sotto **App per sviluppatori**
4. Toccalo → **Autorizza "tuonome@email.com"** → conferma

Ora l'app si aprirà normalmente.

---

## Il problema dei 7 giorni

Con un account gratuito, ogni app sideloaded scade dopo **7 giorni esatti**. Quando scade, sul telefono compare l'errore *"Impossibile verificare l'app"* e smette di funzionare.

**Come reinstallarla:**

1. Tieni Xcode aperto sul Mac con il progetto caricato
2. Collega l'iPhone con il cavo
3. Premi ▶ (Run) — Xcode reinstalla in ~30 secondi
4. Non serve riautorizzare su iPhone (rimane autorizzata)

> **Consiglio**: metti un promemoria sul calendario ogni 6 giorni. Oppure, se usi BikeGPS spesso, considera l'**Apple Developer Program** a 99€/anno — la firma dura 1 anno e puoi distribuire l'app a chi vuoi senza limiti.

---

## Permessi richiesti all'avvio

La prima volta che apri BikeGPS, iOS chiederà due permessi:

| Permesso | Risposta corretta | Motivo |
|---|---|---|
| **Localizzazione** | "Sempre" | Serve per inviare la posizione GPS all'ESP32 anche quando il telefono è in tasca |
| **Bluetooth** | "Consenti" | Serve per comunicare con il modulo ESP32 |

Se scegli "Solo durante l'uso" per la localizzazione, la navigazione si interromperà quando blocchi lo schermo.

---

## Problemi comuni

| Problema | Soluzione |
|---|---|
| Xcode non vede il telefono | Sblocca il telefono e tocca "Autorizza" sul popup che compare |
| "Provisioning profile not found" | Signing & Capabilities → clicca "Try Again" o cambia bundle ID |
| L'app crasha all'avvio | Controlla che i permessi Bluetooth e Localizzazione siano impostati correttamente |
| "Unable to install" dopo 7 giorni | Riconnetti il cavo e premi Run da Xcode |
| L'app non trova il modulo ESP32 | Assicurati che il modulo sia acceso e che il Bluetooth del telefono sia attivo |

---

## Versione Android (in arrivo)

Sto lavorando a una versione Android dell'app. Android permette di installare APK direttamente senza Xcode, senza limiti di scadenza, e senza bisogno di un Mac. Quando sarà pronta troverete il file APK nella sezione [Releases](https://github.com/macebio/bikegps/releases) di questo repository.
