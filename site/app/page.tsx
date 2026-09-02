const firmwareFile = "Cardputer-ADV-Agent-Console-2.8.1-M5Apps.bin";

const features = [
  {
    index: "01",
    title: "Record without a timer",
    body: "Stream 16 kHz mono WAV straight to microSD. Long recordings stay responsive instead of filling device memory.",
    tone: "cyan",
  },
  {
    index: "02",
    title: "Talk to Codex",
    body: "Open local Codex tasks, type or dictate a message, read results, handle approvals, and listen to the latest answer.",
    tone: "amber",
  },
  {
    index: "03",
    title: "Land in Obsidian",
    body: "Local Whisper transcribes the recording, Codex cleans the text, and the gateway writes Markdown plus an embedded MP3.",
    tone: "violet",
  },
  {
    index: "04",
    title: "Work offline first",
    body: "Record anywhere. Durable jobs, retry controls, and on-device status keep every note traceable until Wi-Fi returns.",
    tone: "green",
  },
];

const steps = [
  ["01", "Install firmware", "Copy the app-only BIN to a FAT32 microSD card and install it from M5Apps → Installer → SD."],
  ["02", "Configure the card", "Add Wi-Fi profiles, your HTTPS gateway address, device token, and trusted CA to AGENT.CFG."],
  ["03", "Start the gateway", "Choose an Obsidian Vault, local Whisper provider, Codex integration, and optional text-to-speech."],
  ["04", "Record and sync", "Capture a note, watch its job move through the queue, and open the finished Markdown in Obsidian."],
];

export default function Home() {
  return (
    <main>
      <header className="site-header shell">
        <a className="brand" href="#top" aria-label="Cardputer Agent Console home">
          <span className="brand-mark">CA</span>
          <span>AGENT CONSOLE</span>
        </a>
        <nav aria-label="Primary navigation">
          <a href="#workflow">Workflow</a>
          <a href="#features">Features</a>
          <a href="#install">Install</a>
          <a className="nav-download" href={`downloads/${firmwareFile}`} download>
            Download 2.8.1
          </a>
        </nav>
      </header>

      <section className="hero shell" id="top">
        <div className="hero-copy">
          <div className="eyebrow"><span /> LOCAL-FIRST FIELD TERMINAL</div>
          <h1>Your voice.<br />Your agents.<br /><em>Your data.</em></h1>
          <p className="hero-lead">
            Turn the M5Stack Cardputer ADV into a pocket voice recorder and a
            remote console for Codex — with private transcription on your Mac
            and automatic delivery to Obsidian.
          </p>
          <div className="hero-actions">
            <a className="button primary" href={`downloads/${firmwareFile}`} download>
              <span>↓</span> Download firmware
            </a>
            <a className="button secondary" href="#install">Installation guide <span>→</span></a>
          </div>
          <div className="hero-meta" aria-label="Project highlights">
            <span><b>100%</b> local option</span>
            <span><b>16 kHz</b> PCM capture</span>
            <span><b>HTTPS</b> gateway</span>
          </div>
        </div>

        <div className="hero-visual">
          <div className="signal signal-a" />
          <div className="signal signal-b" />
          <div className="device-frame">
            <div className="frame-label"><span>LIVE UNIT</span><span>ADV / 2.8.1</span></div>
            <img src="images/official/cardputer-adv-angle.webp" alt="Official M5Stack Cardputer ADV product photograph" />
            <div className="frame-status">
              <span><i className="online" /> REAL K132-ADV HARDWARE</span>
              <a href="https://docs.m5stack.com/en/core/Cardputer-Adv" target="_blank" rel="noreferrer">M5STACK SOURCE ↗</a>
            </div>
          </div>
          <div className="floating-chip chip-one">VOICE<br /><b>READY</b></div>
          <div className="floating-chip chip-two">CODEX<br /><b>LINKED</b></div>
        </div>
      </section>

      <section className="ticker" aria-label="Supported capabilities">
        <div>VOICE CAPTURE <span>◆</span> LOCAL WHISPER <span>◆</span> CODEX TASKS <span>◆</span> OBSIDIAN EXPORT <span>◆</span> OFFLINE QUEUE <span>◆</span> TEXT TO SPEECH</div>
      </section>

      <section className="hardware-showcase shell" aria-labelledby="hardware-title">
        <div className="hardware-copy">
          <span className="kicker">ACTUAL HARDWARE / K132-ADV</span>
          <h2 id="hardware-title">Built for the real<br />Cardputer ADV.</h2>
          <p>No concept renders: these are official photographs of the supported M5Stack device. Agent Console targets its ES8311 audio codec, 1.14-inch display, microSD storage, and 56-key keyboard.</p>
          <div className="hardware-specs"><span>ESP32-S3</span><span>1750 mAh</span><span>3.5 mm audio</span><span>81 g</span></div>
          <a className="source-link" href="https://docs.m5stack.com/en/core/Cardputer-Adv" target="_blank" rel="noreferrer">Official M5Stack product page →</a>
        </div>
        <div className="hardware-gallery">
          <figure className="hardware-primary"><img src="images/official/cardputer-adv-front-clean.webp" alt="Front view of the M5Stack Cardputer ADV" /><figcaption>Front / 56-key keyboard</figcaption></figure>
          <figure><img src="images/official/cardputer-adv-ports-angle.webp" alt="Angled view showing Cardputer ADV expansion ports" /><figcaption>Ports / expansion bus</figcaption></figure>
        </div>
      </section>

      <section className="section shell workflow" id="workflow">
        <div className="section-heading">
          <div><span className="kicker">SYSTEM / 01</span><h2>A private path from<br />thought to knowledge.</h2></div>
          <p>Only the Cardputer crosses your Wi-Fi. Speech, agent sessions, credentials, and notes can remain entirely on your own Mac.</p>
        </div>
        <div className="flow-grid">
          <article><span className="flow-no">01</span><div className="flow-icon">●</div><h3>Cardputer ADV</h3><p>Record, type, review status</p></article>
          <div className="flow-arrow">→</div>
          <article><span className="flow-no">02</span><div className="flow-icon">⌁</div><h3>Local Gateway</h3><p>Secure queue over HTTPS</p></article>
          <div className="flow-arrow">→</div>
          <article><span className="flow-no">03</span><div className="flow-icon">✦</div><h3>Whisper + Codex</h3><p>Transcribe, edit, respond</p></article>
          <div className="flow-arrow">→</div>
          <article><span className="flow-no">04</span><div className="flow-icon">◇</div><h3>Obsidian Vault</h3><p>Markdown and linked MP3</p></article>
        </div>
      </section>

      <section className="section shell" id="features">
        <div className="section-heading compact">
          <div><span className="kicker">CAPABILITIES / 02</span><h2>Small device.<br />Serious workflow.</h2></div>
        </div>
        <div className="feature-grid">
          {features.map((feature) => (
            <article className={`feature-card ${feature.tone}`} key={feature.index}>
              <span className="feature-index">{feature.index}</span>
              <div className="feature-line" />
              <h3>{feature.title}</h3>
              <p>{feature.body}</p>
            </article>
          ))}
        </div>
      </section>

      <section className="section screens">
        <div className="shell">
          <div className="section-heading compact">
            <div><span className="kicker">INTERFACE / 03</span><h2>Designed for the<br />screen in your hand.</h2></div>
            <p>Contextual help, service diagnostics, readable transcripts, Wi-Fi scanning, and clear delivery states fit a 240 × 135 display. Frames are rendered from the firmware's own drawing code by the desktop preview.</p>
          </div>
          <div className="screen-grid">
            <figure className="screen-large"><img src="images/screens/recording.png" alt="Active recording screen" /><figcaption><span>01</span> Capture</figcaption></figure>
            <figure><img src="images/screens/codex.png" alt="Codex conversation on the Cardputer" /><figcaption><span>02</span> Codex</figcaption></figure>
            <figure><img src="images/screens/library.png" alt="Recording library with delivery states" /><figcaption><span>03</span> Library</figcaption></figure>
            <figure><img src="images/screens/settings.png" alt="Cardputer Agent Console settings" /><figcaption><span>04</span> Control</figcaption></figure>
            <figure><img src="images/screens/outbox.png" alt="Outbox with pending deliveries" /><figcaption><span>05</span> Outbox</figcaption></figure>
            <figure><img src="images/screens/saver-grid.png" alt="Cyberpunk standby screen" /><figcaption><span>06</span> Standby</figcaption></figure>
          </div>
        </div>
      </section>

      <section className="section shell install" id="install">
        <div className="section-heading">
          <div><span className="kicker">SETUP / 04</span><h2>From zero to your<br />first synced note.</h2></div>
          <p>The device firmware and Mac gateway are separate, auditable components. Configure paths and credentials locally — never in source code.</p>
        </div>
        <div className="steps">
          {steps.map(([number, title, body]) => (
            <article key={number}><span>{number}</span><div><h3>{title}</h3><p>{body}</p></div></article>
          ))}
        </div>
        <div className="config-callout">
          <div><span className="terminal-dot red" /><span className="terminal-dot amber" /><span className="terminal-dot green" /></div>
          <code><span>$</span> python3 gateway/scripts/configure.py</code>
          <p>The setup assistant detects Obsidian Vaults, creates a private device token, and safely stores paths containing spaces.</p>
        </div>
      </section>

      <section className="download-section shell" id="download">
        <div className="download-panel">
          <div className="download-copy">
            <span className="kicker">CURRENT RELEASE</span>
            <h2>Agent Console <em>2.8.1</em></h2>
            <p>App-only firmware for M5Stack Cardputer ADV. Keeps M5Apps intact and installs from a FAT32 microSD card.</p>
            <div className="release-notes"><span>✓ M5Apps compatible</span><span>✓ Local CA from SD</span><span>✓ Contextual help</span></div>
          </div>
          <div className="download-box">
            <span className="file-type">BIN</span>
            <div><b>{firmwareFile}</b><small>1.37 MiB · SHA-256 verified</small></div>
            <a href={`downloads/${firmwareFile}`} download aria-label="Download firmware 2.8.1">↓</a>
          </div>
          <a className="checksum-link" href="downloads/SHA256SUMS.txt" download>Download SHA256SUMS.txt →</a>
        </div>
      </section>

      <section className="privacy shell">
        <span className="privacy-mark">◉</span>
        <div><span className="kicker">LOCAL BY DESIGN</span><h2>Your recordings do not need a cloud.</h2></div>
        <p>Use local Whisper, local Codex authentication, your own TLS certificate, and your personal Obsidian Vault. Cloud transcription remains optional.</p>
      </section>

      <footer>
        <div className="shell footer-inner">
          <div className="brand"><span className="brand-mark">CA</span><span>AGENT CONSOLE</span></div>
          <p>Open-source field tools for Cardputer ADV. Product photography © M5Stack.</p>
          <div><a href="#top">Back to top ↑</a></div>
        </div>
      </footer>
    </main>
  );
}
