# Sculpt Lead 0.1.0

Strumento virtuale VST3 **sempre monofonico**, con la stessa interfaccia 768 × 512 di Sculpt Bass e un'unica manopola di trasformazione:

**Reed → Brass → Analog → Dance → Digital**

| Manopola | Famiglia | Suono |
| --- | --- | --- |
| 0% | Reed | Clarinetto nel registro basso/medio e flauto nel registro medio/alto, con sovrapposizione graduale. |
| 25% | Brass | Trombone nel registro basso/medio e tromba nel registro medio/alto, con sovrapposizione graduale. |
| 50% | Analog | Lead analogico caldo con oscillatori a banda limitata e filtro. |
| 75% | Dance | Lead più presente, attacco pronunciato, armoniche e leggero detune; saturazione contenuta. |
| 100% | Digital | Lead FM brillante con fondamentale riconoscibile e spettro limitato secondo l'altezza della nota. |

Le posizioni intermedie miscelano gradualmente le famiglie adiacenti. La manopola può essere automatizzata anche durante una nota tenuta.

## Utilizzo

- Una sola nota melodica per volta, con priorità all'ultima nota premuta.
- Note sovrapposte: legato programmato, senza ripartire con un attacco completo a ogni nota.
- Rilasciando l'ultima nota si torna a quella precedente ancora tenuta. CC64 gestisce il sustain per canale.
- Note separate e ribattute: nuova articolazione e alternanza degli attacchi registrati dove disponibili.
- Pitch bend: ±2 semitoni. Mod wheel / CC1: vibrato. CC11: espressione.
- La velocity seleziona le dinamiche campionate per layer; CC11 permette una variazione continua del livello. I layer di velocity non sono miscelati con una dissolvenza continua.
- Il controllo **Output** regola il livello. **Glide**, da 0 a 250 ms, è disponibile tra i parametri dell'host; il valore iniziale è 0 ms.
- Uscita a due canali con lo stesso segnale al centro. Monofonia fissa, nessun modo polifonico.
- Zero latenza aggiunta dichiarata dal plugin; la latenza complessiva dipende da buffer e scheda audio.

## Campioni inclusi

Il banco contiene **202 WAV mono PCM16 a 48 kHz**, circa **33.64 MB / 32.08 MiB**, derivati da registrazioni di **VSCO 2 Community Edition** con licenza **CC0**. I campioni vengono incorporati nel VST3 durante la compilazione: non serve installare un player esterno o scaricare file quando si apre lo strumento.

Sono presenti 103 combinazioni di nota/dinamica. Di queste, 99 hanno due varianti registrate d'attacco e quattro regioni del flauto basso mantengono il singolo attacco sustain originale. Le varianti sono ricavate da attacchi staccato reali raccordati al corpo sustain. Il round robin riguarda quindi **gli attacchi**, non due registrazioni sustain indipendenti per ogni nota. Il legato è gestito dal motore e non usa campioni di intervalli registrati.

Le librerie coprono registri diversi; fuori dalle note originali il motore traspone il campione più vicino. I passaggi tra strumenti sono distribuiti sulla tastiera, non aggiunti come ulteriori posizioni della manopola. La documentazione completa su note, fonti, elaborazione e licenza è in [docs/SAMPLES.md](docs/SAMPLES.md).

Fonti originali:

- [VSCO 2 Community Edition — Versilian Studios](https://versilian-studios.com/vsco-community/)
- [Repository dei campioni VSCO 2 CE](https://github.com/sgossner/VSCO-2-CE)
- Commit dei WAV: `440300901dfe9275fd84e0b7763af1f8443ae62e`.
- Framework [JUCE 8.0.4](https://github.com/juce-framework/JUCE/tree/8.0.4), commit `51d11a2be6d5c97ccf12b4e5e827006e19f0555a`.

## Avviare la compilazione su GitHub

Il file necessario è già incluso, con questo percorso **esatto**:

```text
.github/workflows/build-macos-vst3.yml
```

1. Estrai lo ZIP dei sorgenti e carica **il contenuto** nella radice del repository scelto per Sculpt Lead. Nella pagina principale del repository devono comparire `CMakeLists.txt`, `Source`, `Samples`, `tools` e `.github`. Non caricare solo lo ZIP e non lasciare il progetto dentro una cartella aggiuntiva.
2. Carica anche **Samples** con tutti i WAV e la cartella nascosta **.github**. Sul Mac, nel Finder, `Cmd + Maiusc + .` mostra le cartelle nascoste. Se l'upload da browser perde `.github`, usa **Add file → Create new file**, incolla `.github/workflows/build-macos-vst3.yml` nel nome e copia il contenuto del file fornito.
3. Dopo il commit il workflow parte con il push, indipendentemente dal nome del repository e del ramo. In alternativa: **Actions → Build macOS VST3 → Run workflow**. Per l'avvio manuale il file deve essere presente nel ramo predefinito; Actions deve essere abilitato nel repository.
4. Al termine apri la run verde e scarica, in **Artifacts**, **Sculpt-Lead-macOS-VST3**.
5. Apri l'archivio dell'artifact e lo ZIP interno `Sculpt-Lead-macOS-VST3.zip`: contiene **Sculpt Lead.vst3**.

Il workflow verifica il banco, compila, esegue i test del motore e del processore, verifica le architetture **Apple Silicon + Intel**, applica una firma ad hoc e crea lo ZIP finale. Il bundle richiede **macOS 11 o successivo**. Il pacchetto sorgente non contiene un binario macOS già compilato.

## Installare il VST3 sul Mac

Copia **Sculpt Lead.vst3** in:

```text
~/Library/Audio/Plug-Ins/VST3/
```

Poi riapri il programma musicale o avvia una nuova scansione dei plugin. Questa versione è VST3: l'host deve supportare strumenti VST3.

Se macOS blocca **questo bundle**, dopo averlo collocato nella cartella sopra, il comando è:

```bash
xattr -dr com.apple.quarantine "$HOME/Library/Audio/Plug-Ins/VST3/Sculpt Lead.vst3"
```

Il comando si riferisce solo al plugin scaricato. La firma creata dal workflow è ad hoc, senza notarizzazione Apple.

## Compilare localmente su macOS

Servono Xcode Command Line Tools, CMake 3.22 o successivo, Git e Python 3.9 o successivo. Python serve alla verifica dei file, non al funzionamento del plugin.

Dalla cartella del progetto:

```bash
bash tools/build-macos.sh
```

Risultato:

```text
dist/Sculpt-Lead-macOS-VST3.zip
```

JUCE 8.0.4 viene recuperato durante la prima configurazione. Per usare una copia locale già disponibile:

```bash
SCULPT_JUCE_SOURCE_DIR=/percorso/assoluto/JUCE-8.0.4 bash tools/build-macos.sh
```

`SCULPT_LEAD_BUILD_JOBS` modifica il parallelismo della compilazione, impostato a 3. I WAV sono già pronti: **non è necessario lanciare l'importer** per compilare lo strumento.

## Test e riproducibilità

Verifica del banco, senza dipendenze Python aggiuntive:

```bash
python3 tools/verify_factory.py
```

Test del motore C++17, senza JUCE:

```bash
bash tools/test-core.sh
```

Il test del processore e il generatore di ascolti si abilitano con `-DSCULPT_LEAD_BUILD_QA=ON` in CMake. Il target `SculptLeadStateTests` verifica anche i campioni realmente incorporati. Il target `SculptLeadPreview` genera WAV a 24 bit usando il processore effettivo, senza riverbero, elaborazioni esterne o normalizzazione distinta dei timbri:

```text
SculptLeadPreview /percorso/assoluto/ascolti
```

Il rapporto delle verifiche effettuate per questa consegna è in [docs/VALIDATION.md](docs/VALIDATION.md). Gli ascolti consentono di confrontare la stessa frase a 0, 25, 50, 75 e 100%; includono anche una rotazione continua della manopola e la risposta alla dinamica di Reed e Brass.

## Dipendenze e licenze

Le registrazioni VSCO sono CC0; licenza, crediti, mappature originali e provenienza sono inclusi in `ThirdParty/VSCO`. Il codice DSP non usa reti neurali, processi esterni o librerie di elaborazione da installare.

JUCE è recuperato dal progetto originale e mantiene la propria licenza duale AGPLv3/commerciale. Le condizioni originali sono riportate in `ThirdParty/JUCE-LICENSE.md`; la licenza CC0 dei campioni non sostituisce quella del framework. I termini delle dipendenze incorporate da JUCE restano quelli dei rispettivi autori.
