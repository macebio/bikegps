# BikeGPS — Handlebar Mount

> ⚠️ **Prototipo v1** — questo è il primo prototipo funzionante. Probabilmente ci sono molte cose da migliorare: ergonomia, angolo del display, resistenza alle vibrazioni, facilità di montaggio. Ogni suggerimento, modifica o versione alternativa è benvenuta — aprire una PR o un Issue su GitHub.

## File

| File | Descrizione |
|---|---|
| `bikegps_mount_v1.step` | Primo prototipo — mount per manubrio da bici con fissaggio a C-clamp |

File in formato STEP AP214, apribile con FreeCAD, Fusion 360, SolidWorks, KiCad 3D Viewer o qualsiasi altro tool CAD.

## Hardware necessario

| Componente | Quantità | Uso |
|---|---|---|
| Viti M2.5 (lunghezza ~6 mm) | 4 | Fissaggio modulo LCD al tray |
| Bulloni M4 (lunghezza ~20 mm) + dado | 2 | C-clamp sul manubrio |

## Assemblaggio del giunto a sfera

La sfera del giunto è un **pezzo separato**: va incollata alla piccola staffa che regge il modulo Waveshare **dopo** aver inserito il dado M4 nel giunto.

Ordine di montaggio:
1. Inserisci il dado M4 nella sede del giunto
2. Posiziona la sfera nella sua sede sulla staffa del modulo
3. Incolla la sfera alla staffa (colla epossidica o cianocrilato)
4. Lascia asciugare completamente prima di montare il tutto sul manubrio

> ⚠️ Se incolli prima di inserire il dado, non riesci più a chiudere il giunto.

## Impostazioni di stampa consigliate

- **Materiale**: PETG o ASA (resistenza UV e alle intemperie)
- **Altezza layer**: 0.2 mm
- **Infill**: 40%
- **Supporti**: necessari sotto l'arco del C-clamp
- **Orientamento**: posizionare il tray del display rivolto verso il basso per stampare senza supporti nella parte critica

## Compatibilità manubrio

La v1 è progettata per manubri standard da bici. Se hai un diametro diverso apri una PR con la variante.

## Come contribuire

1. Modifica il file STEP (o esportane uno nuovo dal tuo CAD)
2. Aggiorna questo README con le differenze rispetto alla v1
3. Nomina il file `bikegps_mount_v2.step` (o `_22mm`, `_31.8mm`, etc.)
4. Apri una PR — qualsiasi miglioramento è benvenuto
