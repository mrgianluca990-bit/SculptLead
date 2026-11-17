# Verifica di Sculpt Lead 0.1.0

Data: **17 novembre 2026**.

Il progetto è stato compilato come **VST3 Linux x86_64**, con il processore
effettivo, l'interfaccia JUCE e il banco WAV incorporato. Sono stati eseguiti i
test del motore, i test del processore, una verifica visiva dell'interfaccia e
otto render audio. Le verifiche descritte sotto sono riuscite.

**Il binario macOS non è stato compilato in questo ambiente Linux.** La
compilazione Apple Silicon/Intel, la firma ad hoc e la verifica delle due
architetture sono predisposte nello script `tools/build-macos.sh` e nel workflow
`.github/workflows/build-macos-vst3.yml`; saranno eseguite dal runner macOS.
L'apertura e l'uso in una DAW su Mac restano da verificare con quel binario.

## Ambiente e build

- Linux x86_64, GCC 14.2.0, CMake 4.4.5, configurazione Release.
- CPU esposta dall'ambiente: AMD EPYC 7763 64-Core Processor.
- JUCE 8.0.4; VST3 SDK 3.7.12 incluso in quella versione di JUCE.
- Banco: 202 WAV, 33.636.716 byte, mono PCM16 a 48 kHz.
- Target compilati: `SculptLead_VST3`, `SculptLeadCoreTests`,
  `SculptLeadStateTests` e `SculptLeadPreview`.
- Generazione del `moduleinfo.json` VST3 riuscita: strumento `Sculpt Lead`,
  versione 0.1.0, categorie Instrument/Synth e identificativi dedicati.
- Workflow YAML, trigger, percorsi e sintassi degli script verificati.
  Le azioni checkout/upload-artifact sono fissate a commit delle versioni v7
  verificate nei rispettivi repository ufficiali.

## Test del motore

La suite indipendente da JUCE usa segnali con frequenza nota per controllare
il comportamento del lettore campionato e della sintesi. Verifica:

- monofonia, priorità all'ultima nota, ritorno alla nota precedente e rilascio;
- note ripetute, conteggi note-on/note-off e isolamento fra canali MIDI;
- legato e stabilità del round robin durante le note sovrapposte;
- CC64 sustain, CC120 immediato, CC123 rispettoso del pedale e reset;
- pitch bend ±2 semitoni, CC1 vibrato e CC11 espressione;
- controller già impostati prima del note-on e cambio canale dopo il silenzio;
- glide, automazione e continuità del morph;
- intonazione, ricampionamento e note sostenute oltre sei secondi
  a 44,1 / 48 / 96 kHz;
- valori non finiti, banchi invalidi, uscite finite e protezione del picco;
- **zero allocazioni** durante 3.000 blocchi con MIDI, controller, automazione
  e reset sotto stress.

Il controllo specifico su CC11=0 prima della prima nota richiede silenzio esatto
fin dall'attacco. È verificato anche il bend impostato prima del note-on, nei
primi periodi utili del segnale.

## Test del processore completo

`SculptLeadStateTests` carica i **campioni realmente incorporati**, senza stub
del banco, e controlla:

- caricamento dei quattro strumenti e disponibilità del round robin;
- salvataggio e ripristino dello stato di morph, output e glide;
- rifiuto completo di stati corrotti, fuori intervallo, NaN/Inf e versioni
  incompatibili;
- latenza aggiunta dichiarata pari a zero e uscita a due canali senza ingresso;
- note MIDI e panic applicati al corretto offset di campione;
- SysEx ignorato, comportamento del bypass e assenza di note residue;
- rilascio entro la coda dichiarata di 0,5 secondi;
- preparazione/riavvio e cinque endpoint in tre registri.

Esito dell'ultima esecuzione CTest: **2 suite su 2 superate, nessun test fallito**.
Le suite contengono i gruppi di controlli elencati sopra.

L'interfaccia è stata renderizzata direttamente dall'editor reale e ispezionata
a 768 × 512 pixel: titolo Lead, indicazione MONO, controlli, cinque etichette,
indicatore RR e VU. La percentuale è stata posizionata sopra l'arco della
manopola per mantenerla leggibile. I testi accentati usano conversione UTF-8
esplicita.

## Render audio reali

Gli otto WAV sono generati da `SculptLeadPreview` attraverso lo stesso
`SculptLeadProcessor` usato dal plugin: **48 kHz, 24 bit, Output 0 dB**, nessun
effetto esterno e nessuna normalizzazione separata dei timbri.

I primi cinque esempi contengono la stessa frase, le stesse velocity, note
ribattute, legato, ritorno a una nota tenuta e una prova nei registri MIDI
50 / 60 / 72 / 84.

| Famiglia | Picco dell'intera frase | RMS dell'intera frase |
| --- | ---: | ---: |
| Reed | −5,59 dBFS | −18,37 dBFS |
| Brass | −2,72 dBFS | −18,63 dBFS |
| Analog | −14,24 dBFS | −18,14 dBFS |
| Dance | −11,90 dBFS | −19,29 dBFS |
| Digital | −14,27 dBFS | −19,12 dBFS |

Scarto RMS massimo fra i cinque esempi: **1,15 dB**. Questa misura confronta
il livello medio della medesima frase; non è una garanzia di uguale loudness
percepita su qualsiasi nota o velocità. I fiati conservano attacchi con picchi
più alti rispetto alle forme d'onda sintetiche.

La revisione numerica degli otto render ha confermato:

- nessun campione clippato, NaN o infinito;
- canali L e R identici, campione per campione;
- nessuna nota mancante nella frase e nessuna nota bloccata alla fine;
- alternanza effettiva dei due attacchi RR nelle note ripetute Reed e Brass;
- nessun buco nella nota tenuta mentre il morph passa da 0 a 100%.

Nel morph continuo la massima variazione RMS tra finestre consecutive di 10 ms
è circa 0,61 dB. Il livello della nota durante l'intera trasformazione varia di
circa 4,63 dB: il morph è continuo, ma non contiene una compensazione percettiva
perfettamente costante.

Le prove sono strumentali e numeriche; il carattere dei timbri va valutato
ascoltando i file, con il proprio monitoraggio e nel contesto musicale.

## Costo di elaborazione: misura indicativa

Il benchmark chiama il processore reale con il banco già caricato, una nota
MIDI 67 tenuta a velocity 0,85, blocchi da 256 campioni e sample rate 48 kHz.
Ogni posizione elabora **21,33 secondi di audio**. Caricamento, GUI, host,
driver audio e I/O disco sono esclusi dal tratto cronometrato.

| Morph | Tempo di elaborazione | Rapporto tempo / durata audio |
| ---: | ---: | ---: |
| 0% | 0,173 s | 0,81% |
| 12,5% | 0,183 s | 0,86% |
| 25% | 0,127 s | 0,59% |
| 37,5% | 0,157 s | 0,74% |
| 50% | 0,121 s | 0,57% |
| 62,5% | 0,185 s | 0,87% |
| 75% | 0,224 s | 1,05% |
| 87,5% | 0,163 s | 0,76% |
| 100% | 0,137 s | 0,64% |

È una misura di tempo sul server Linux indicato, con una sola voce melodica.
**Non è la percentuale del misuratore CPU di una DAW, né una previsione
garantita per un Mac.** Va misurato il carico nel proprio host e alla propria
dimensione di buffer.

## Limiti dichiarati del banco e della versione

- Round robin degli **attacchi registrati**, con sustain condiviso; non sono due
  registrazioni complete indipendenti di ogni nota lunga.
- Quattro combinazioni di nota/dinamica del flauto basso hanno un solo attacco
  originale, perché la sorgente non offre staccati di quelle note.
- Legato programmato, senza campioni di intervalli registrati.
- Dinamiche selezionate per layer. Il volume risponde anche alla velocity, ma
  i layer non sono miscelati con una dissolvenza continua: negli esempi della
  dinamica si osservano passaggi di circa 4,67 dB Reed e 6,81 dB Brass fra due
  velocity che attraversano una soglia di layer. CC11 permette un controllo
  continuo del livello della nota già in esecuzione.
- Trasposizione oltre i centri registrati; l'estensione virtuale non equivale a
  una registrazione originale per ogni tasto MIDI.
- Build macOS e verifica in una DAW su Mac da eseguire con il workflow incluso.

I dati del benchmark, i livelli dei render e gli hash dei sorgenti audio/DSP
verificati sono in `docs/verification-results.json`. La provenienza completa
dei campioni è in `docs/SAMPLES.md` e nel manifest.

Riferimenti del workflow:
[checkout](https://github.com/actions/checkout),
[upload-artifact](https://github.com/actions/upload-artifact),
[runner macOS](https://github.com/actions/runner-images).
